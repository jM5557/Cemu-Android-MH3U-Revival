#include "Cafe/SaveState/SaveState.h"
#include "Cafe/SaveState/Quiesce.h"
#include "Cafe/SaveState/StateStream.h"

#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/IOSU/fsa/iosu_fsa.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Common/version.h"
#include "Cemu/Logging/CemuLogging.h"

#include <zstd.h>
#include <ctime>
#include <fstream>
#include <functional>

namespace SaveStates
{
	// Guest memory is captured in 64 KiB blocks with all-zero blocks elided. Without
	// this every state would carry the full 1 GiB MEM2 range regardless of how little
	// of it the title actually touched.
	static constexpr uint32 kMemoryBlockSize = 64 * 1024;

	std::string StateFingerprint::GetIncompatibilityReason(const StateFingerprint& current) const
	{
		if (buildVersion != current.buildVersion)
			return fmt::format("State was created by Cemu build '{}', this is '{}'", buildVersion, current.buildVersion);
		if (titleId != current.titleId)
			return fmt::format("State belongs to title {:016x}, currently running {:016x}", titleId, current.titleId);
		if (titleVersion != current.titleVersion)
			return fmt::format("State was created with title version {}, currently running {}", titleVersion, current.titleVersion);
		if (isMulticoreMode != current.isMulticoreMode)
			return "State was created with a different CPU core count";
		if (graphicPackPatchHash != current.graphicPackPatchHash)
			return "State was created with a different set of graphic pack patches";
		return {};
	}

	StateFingerprint BuildCurrentFingerprint()
	{
		StateFingerprint fp;
		fp.buildVersion = BUILD_VERSION_STRING;
		fp.titleId = (uint64)CafeSystem::GetForegroundTitleId();
		fp.titleVersion = CafeSystem::GetForegroundTitleVersion();
		fp.isMulticoreMode = coreinit::__CemuIsMulticoreMode() ? 1 : 0;
		// phase 1: hash the applied graphic pack patch bytes. Until then a state taken
		// with a different set of patches enabled is accepted and may desync. See the
		// determinism fingerprint section of the design doc.
		fp.graphicPackPatchHash = 0;
		return fp;
	}

	// MEMR: every mapped guest memory range.
	//
	// No pointer relocation is ever needed here. Guest memory is a flat, fixed-layout
	// space at memory_base, so a range restores to the exact host addresses it was
	// captured from. Ranges are enumerated rather than hardcoded, so memory areas added
	// upstream are picked up automatically.
	static void MemoryDoState(StateStream& s)
	{
		s.DoMarker(kMarkerMEMR, "MMU");

		std::vector<MMURange*> ranges = memory_getMMURanges();

		uint32 mappedCount = 0;
		for (auto* range : ranges)
		{
			if (range->isMapped())
				mappedCount++;
		}
		uint32 storedCount = mappedCount;
		s.Do(storedCount);
		if (s.HasError())
			return;
		if (s.IsReading() && storedCount != mappedCount)
		{
			s.SetError(fmt::format("State has {} mapped memory ranges, this world has {}", storedCount, mappedCount));
			return;
		}

		for (auto* range : ranges)
		{
			if (!range->isMapped())
				continue;

			uint32 baseAddress = range->getBase();
			uint32 size = range->getSize();
			s.Do(baseAddress);
			s.Do(size);
			if (s.HasError())
				return;
			if (s.IsReading() && (baseAddress != range->getBase() || size != range->getSize()))
			{
				s.SetError(fmt::format("Memory layout mismatch at {} (state: {:08x}+{:x}, world: {:08x}+{:x})",
									   range->getName(), baseAddress, size, range->getBase(), range->getSize()));
				return;
			}

			uint8* data = range->getPtr();
			const uint32 blockCount = (size + kMemoryBlockSize - 1) / kMemoryBlockSize;

			std::vector<uint8> blockPresent(blockCount, 0);
			if (!s.IsReading())
			{
				for (uint32 i = 0; i < blockCount; i++)
				{
					const uint32 offset = i * kMemoryBlockSize;
					const uint32 blockSize = std::min(kMemoryBlockSize, size - offset);
					const uint8* block = data + offset;
					const bool isNonZero = std::find_if(block, block + blockSize, [](uint8 v) { return v != 0; }) != block + blockSize;
					blockPresent[i] = isNonZero ? 1 : 0;
				}
			}
			s.DoBytes(blockPresent.data(), blockCount);
			if (s.HasError())
				return;

			if (s.IsReading())
			{
				// Elided blocks were all-zero at capture time, so zero the range first
				// and then fill in only the blocks that were stored.
				std::memset(data, 0, size);
			}
			for (uint32 i = 0; i < blockCount; i++)
			{
				if (!blockPresent[i])
					continue;
				const uint32 offset = i * kMemoryBlockSize;
				const uint32 blockSize = std::min(kMemoryBlockSize, size - offset);
				s.DoBytes(data + offset, blockSize);
				if (s.HasError())
					return;
			}
		}

		s.DoMarker(kMarkerMEMR, "MMU-end");
	}

	// Sizes a capture buffer for the largest body this world can produce, so it never grows while
	// the world is stopped. Growing by doubling would copy hundreds of megabytes and briefly hold
	// one and a half copies of the state, which on a phone is reason enough for the system to kill
	// the app. Reserving is address space only: pages are committed as the capture writes them, so
	// what goes unused costs nothing. Falls back to a modest reservation if even that is refused.
	static void ReserveCaptureBuffer(std::vector<uint8>& buffer)
	{
		uint64 bound = 16 * 1024 * 1024; // every chunk besides MEMR is small
		for (auto* range : memory_getMMURanges())
		{
			if (range->isMapped())
				bound += (uint64)range->getSize() + range->getSize() / kMemoryBlockSize + 64;
		}
		try
		{
			buffer.reserve((size_t)bound);
		}
		catch (const std::bad_alloc&)
		{
			buffer.reserve(64 * 1024 * 1024);
		}
	}

	// The single bidirectional pass over every participating subsystem.
	static void DoStateBody(StateStream& s)
	{
		MemoryDoState(s);
		if (s.HasError())
			return;
		coreinit::ThreadDoState(s);
		if (s.HasError())
			return;
		PPCTimer_DoState(s);
		if (s.HasError())
			return;
		// Must run after TIME (it rebuilds fire ticks against the restored clock) and
		// before the host thread rebuild, which frees the fiber stacks that some
		// transient alarms point into.
		coreinit::AlarmDoState(s);
		if (s.HasError())
			return;
		// Host file objects behind the guest's FSA handles. Without this a restored game
		// holds handles that resolve to nothing, and anything it was streaming stops
		// without an error the game reports.
		iosu::fsa::FSADoState(s);
		if (s.HasError())
			return;
		// AX voice allocation. Host state that describes guest voices and cannot be derived
		// from guest memory, so it has to travel in the state file like the alarms do.
		snd_core::AXVoiceDoState(s);
		if (s.HasError())
			return;
		// Host-side GPU command ring cursors. Guest submission bookkeeping is covered by
		// MEMR, but the ring itself lives outside guest memory.
		TCL::TCLDoState(s);
		if (s.HasError())
			return;
		// Host-side Latte state, notably the flip handshake counter the vsync handler
		// needs in order to present anything at all after a restore.
		Latte_DoState(s);
		if (s.HasError())
			return;
		s.DoMarker(kMarkerEND, "body-end");
	}

	struct ZstdCCtxDeleter
	{
		void operator()(ZSTD_CCtx* ctx) const { ZSTD_freeCCtx(ctx); }
	};
	struct ZstdDCtxDeleter
	{
		void operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }
	};

	// Compresses through a small window rather than into a second buffer the size of the
	// input. A body is hundreds of megabytes, and on a phone holding it twice over is the
	// difference between saving and the system killing the app for memory.
	static bool CompressStreaming(const std::vector<uint8>& input, int level, std::string& error,
								  const std::function<bool(const uint8*, size_t)>& sink)
	{
		std::unique_ptr<ZSTD_CCtx, ZstdCCtxDeleter> cctx(ZSTD_createCCtx());
		if (!cctx)
		{
			error = "Compression failed: out of memory";
			return false;
		}
		ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level);
		// records the size in the frame header, which the reader checks against the file header
		ZSTD_CCtx_setPledgedSrcSize(cctx.get(), input.size());
		std::vector<uint8> window(ZSTD_CStreamOutSize());
		ZSTD_inBuffer in{input.data(), input.size(), 0};
		while (true)
		{
			ZSTD_outBuffer out{window.data(), window.size(), 0};
			const size_t remaining = ZSTD_compressStream2(cctx.get(), &out, &in, ZSTD_e_end);
			if (ZSTD_isError(remaining))
			{
				error = fmt::format("Compression failed: {}", ZSTD_getErrorName(remaining));
				return false;
			}
			if (out.pos > 0 && !sink(window.data(), out.pos))
			{
				error = "Failed while writing state data (out of disk space?)";
				return false;
			}
			if (remaining == 0)
				return true;
		}
	}

	static OperationResult WriteStateFile(const fs::path& path, StateHead& head, const std::vector<uint8>& body)
	{
		std::vector<uint8> headBuffer;
		StateStream headStream = StateStream::MakeWriter(headBuffer);
		head.DoState(headStream);
		if (headStream.HasError())
			return OperationResult::Fail(headStream.GetError());

		StateFileHeader fileHeader{};
		fileHeader.magic = kStateFileMagic;
		fileHeader.formatVersion = kStateFormatVersion;
		fileHeader.flags = 0;
		fileHeader.headSize = (uint32)headBuffer.size();
		fileHeader.bodySizeUncompressed = (uint64)body.size();
		fileHeader.bodySizeCompressed = 0; // filled in once the body has been streamed out

		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);

		// Write to a temporary file and rename, so an interrupted save cannot destroy a
		// previously good state in the same slot.
		fs::path tempPath = path;
		tempPath += ".tmp";

		{
			std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
			if (!file.is_open())
				return OperationResult::Fail(fmt::format("Failed to open '{}' for writing", _pathToUtf8(tempPath)));
			file.write((const char*)&fileHeader, sizeof(fileHeader));
			if (!headBuffer.empty())
				file.write((const char*)headBuffer.data(), (std::streamsize)headBuffer.size());

			std::string error;
			uint64 compressedSize = 0;
			const bool compressed = CompressStreaming(body, 3, error, [&](const uint8* data, size_t size) {
				file.write((const char*)data, (std::streamsize)size);
				compressedSize += size;
				return file.good();
			});
			if (compressed)
			{
				fileHeader.bodySizeCompressed = compressedSize;
				file.seekp(0);
				file.write((const char*)&fileHeader, sizeof(fileHeader));
				file.flush();
			}
			if (!compressed || !file.good())
			{
				file.close();
				fs::remove(tempPath, ec);
				return OperationResult::Fail(compressed ? "Failed while writing state data (out of disk space?)" : error);
			}
		}

		fs::rename(tempPath, path, ec);
		if (ec)
		{
			fs::remove(tempPath, ec);
			return OperationResult::Fail(fmt::format("Failed to finalize state file: {}", ec.message()));
		}
		return OperationResult::Ok();
	}

	// See CafeSystem::IsTitleOnline. Checked again once the world is stopped, since the game could
	// open a socket in between.
	static constexpr const char* kOnlineMessage =
		"Save states are unavailable while the game is online: a state cannot hold the connection, "
		"so saving or loading would drop you from the session. Go offline in the game first.";

	// Pause (CafeSystem::PauseTitle) works by suspending every guest thread, and the suspend
	// counts are part of the thread state a capture records. A state saved while paused would
	// load frozen, and a load while paused would have the later resume release threads the
	// game suspended itself. So the pause is lifted for the capture or restore. Declare it
	// before the QuiesceScope, so it is released after it.
	//
	// A save pauses again afterwards: nothing on screen changed. A load does not: loading means
	// playing on from that moment, and while paused the screen would stay black, since the load
	// drops the GPU caches that held the last frame.
	class LiftPauseScope
	{
	  public:
		explicit LiftPauseScope(bool pauseAgain) : m_pauseAgain(pauseAgain && CafeSystem::IsTitlePaused())
		{
			if (CafeSystem::IsTitlePaused())
				CafeSystem::ResumeTitle();
		}
		~LiftPauseScope()
		{
			if (m_pauseAgain)
				CafeSystem::PauseTitle();
		}
		LiftPauseScope(const LiftPauseScope&) = delete;
		LiftPauseScope& operator=(const LiftPauseScope&) = delete;

	  private:
		bool m_pauseAgain;
	};

	OperationResult SaveToFile(const fs::path& path)
	{
		if (CafeSystem::IsTitleOnline())
			return OperationResult::Fail(kOnlineMessage);
		std::vector<uint8> body;
		StateHead head;

		{
			// No pause or resume from elsewhere until the world runs again (see GetPauseMutex)
			std::unique_lock pauseLock(CafeSystem::GetPauseMutex());
			LiftPauseScope liftPause(true);
			QuiesceScope quiesce;
			if (!quiesce.IsHeld())
				return OperationResult::Fail(GetQuiesceStatusMessage(quiesce.GetStatus()));
			if (CafeSystem::IsTitleOnline())
				return OperationResult::Fail(kOnlineMessage);

			head.fingerprint = BuildCurrentFingerprint();
			head.unixTimestamp = (uint64)std::time(nullptr);

			ReserveCaptureBuffer(body);

			StateStream stream = StateStream::MakeWriter(body);
			DoStateBody(stream);
			if (stream.HasError())
				return OperationResult::Fail(stream.GetError());
		}
		cemuLog_log(LogType::Force, "Save state: captured {} MiB of guest state", body.size() / (1024 * 1024));
		// The world is running again here. Compression and disk I/O deliberately happen
		// outside the quiesce so the visible pause stays short.

		return WriteStateFile(path, head, body);
	}

	// Reads the fixed header and the uncompressed HEAD payload, leaving the stream
	// positioned at the compressed body. Shared by loading and by the slot picker, which
	// wants the metadata and nothing else.
	static OperationResult ReadStateHead(std::ifstream& file, const fs::path& path,
										 StateFileHeader& fileHeader, StateHead& head)
	{
		if (!file.is_open())
			return OperationResult::Fail(fmt::format("Failed to open '{}'", _pathToUtf8(path)));

		file.read((char*)&fileHeader, sizeof(fileHeader));
		if (!file.good())
			return OperationResult::Fail("State file is truncated");
		if (fileHeader.magic != kStateFileMagic)
			return OperationResult::Fail("Not a Cemu save state file");
		if (fileHeader.formatVersion != kStateFormatVersion)
			return OperationResult::Fail(fmt::format("State was made by a different Cemu build (format {}, this build uses {})",
													 fileHeader.formatVersion, kStateFormatVersion));

		std::vector<uint8> headBuffer(fileHeader.headSize);
		if (fileHeader.headSize > 0)
		{
			file.read((char*)headBuffer.data(), (std::streamsize)headBuffer.size());
			if (!file.good())
				return OperationResult::Fail("State file is truncated (header)");
		}

		StateStream headStream = StateStream::MakeReader(headBuffer);
		head.DoState(headStream);
		if (headStream.HasError())
			return OperationResult::Fail(headStream.GetError());
		return OperationResult::Ok();
	}

	fs::path GetSlotPath(uint32 slot)
	{
		const uint64 titleId = (uint64)CafeSystem::GetForegroundTitleId();
		if (titleId == 0 || slot >= kSlotCount)
			return {};
		return ActiveSettings::GetUserDataPath("savestates/{:016x}/slot{}.cst", titleId, slot);
	}

	SlotInfo QuerySlot(uint32 slot)
	{
		SlotInfo info;
		info.slot = slot;
		const fs::path path = GetSlotPath(slot);
		if (path.empty())
			return info;
		std::error_code ec;
		if (!fs::exists(path, ec))
			return info;

		std::ifstream file(path, std::ios::binary);
		StateFileHeader fileHeader{};
		const OperationResult result = ReadStateHead(file, path, fileHeader, info.head);
		if (!result.success)
		{
			// The slot exists but cannot be described. Surfacing that is better than
			// hiding it, or the user picks a slot that then refuses to load.
			info.exists = true;
			info.incompatibility = result.message;
			return info;
		}
		info.exists = true;
		info.incompatibility = info.head.fingerprint.GetIncompatibilityReason(BuildCurrentFingerprint());
		return info;
	}

	static OperationResult ApplyStateBody(std::vector<uint8>& body);

	OperationResult LoadFromFile(const fs::path& path)
	{
		// before reading and inflating what can be hundreds of megabytes
		if (CafeSystem::IsTitleOnline())
			return OperationResult::Fail(kOnlineMessage);
		std::ifstream file(path, std::ios::binary);
		StateFileHeader fileHeader{};
		StateHead head;
		if (const OperationResult headResult = ReadStateHead(file, path, fileHeader, head); !headResult.success)
			return headResult;

		// Check compatibility before anything touches the running world.
		const std::string incompatibility = head.fingerprint.GetIncompatibilityReason(BuildCurrentFingerprint());
		if (!incompatibility.empty())
			return OperationResult::Fail(incompatibility);

		// Guest memory is a 4 GiB space, so a larger body can only be a damaged file, and
		// believing it would mean attempting the allocation.
		if (fileHeader.bodySizeUncompressed > (4ull << 30) || fileHeader.bodySizeCompressed == 0)
			return OperationResult::Fail("State file is damaged (impossible body size)");

		std::vector<uint8> body;
		try
		{
			body.resize((size_t)fileHeader.bodySizeUncompressed);
		}
		catch (const std::bad_alloc&)
		{
			return OperationResult::Fail("Not enough free memory to load this state");
		}

		// Inflated straight from the file into the body, through a small window, rather than
		// reading the whole compressed body into memory first.
		std::unique_ptr<ZSTD_DCtx, ZstdDCtxDeleter> dctx(ZSTD_createDCtx());
		if (!dctx)
			return OperationResult::Fail("Decompression failed: out of memory");
		std::vector<uint8> window(ZSTD_DStreamInSize());
		ZSTD_outBuffer out{body.data(), body.size(), 0};
		uint64 compressedRemaining = fileHeader.bodySizeCompressed;
		size_t lastResult = 1;
		while (compressedRemaining > 0)
		{
			const size_t chunk = (size_t)std::min<uint64>(compressedRemaining, window.size());
			file.read((char*)window.data(), (std::streamsize)chunk);
			if (!file.good())
				return OperationResult::Fail("State file is truncated (body)");
			compressedRemaining -= chunk;
			ZSTD_inBuffer in{window.data(), chunk, 0};
			while (in.pos < in.size)
			{
				lastResult = ZSTD_decompressStream(dctx.get(), &out, &in);
				if (ZSTD_isError(lastResult))
					return OperationResult::Fail(fmt::format("Decompression failed: {}", ZSTD_getErrorName(lastResult)));
				// more input with the body already full, or after the frame ended
				if (in.pos < in.size && (out.pos == out.size || lastResult == 0))
					return OperationResult::Fail("State body size mismatch");
			}
		}
		file.close();
		if (lastResult != 0 || out.pos != body.size())
			return OperationResult::Fail("State body size mismatch");

		return ApplyStateBody(body);
	}

	// The session as it was immediately before the most recent load.
	//
	// Kept uncompressed and never written to disk: it exists to undo an accident within the
	// same session, which is the only window in which the live session it describes is still
	// the one you want back.
	//
	// Held compressed (zstd's fastest level). Raw it is as large as the session itself, and
	// keeping that for as long as Cemu runs would leave a phone hundreds of megabytes short for
	// the rest of the session. Compressing costs a moment after a load, with the game already
	// running again.
	static std::vector<uint8> s_undoCompressed;
	static uint64 s_undoSize = 0;
	static StateFingerprint s_undoFingerprint;

	bool HasUndoState()
	{
		return !s_undoCompressed.empty();
	}

	void DiscardUndoState()
	{
		s_undoCompressed.clear();
		s_undoCompressed.shrink_to_fit();
		s_undoSize = 0;
	}

	// Replaces the undo snapshot with `raw`, which is released either way.
	static void StoreUndoState(std::vector<uint8>& raw, const StateFingerprint& fingerprint)
	{
		DiscardUndoState();
		std::vector<uint8> compressed;
		compressed.reserve(raw.size() / 4);
		std::string error;
		const bool ok = CompressStreaming(raw, 1, error, [&](const uint8* data, size_t size) {
			compressed.insert(compressed.end(), data, data + size);
			return true;
		});
		const uint64 rawSize = raw.size();
		raw.clear();
		raw.shrink_to_fit();
		if (!ok)
		{
			cemuLog_log(LogType::Force, "Save state: could not keep undo snapshot: {}", error);
			return;
		}
		compressed.shrink_to_fit();
		s_undoCompressed = std::move(compressed);
		s_undoSize = rawSize;
		s_undoFingerprint = fingerprint;
	}

	// Applies an already-decompressed, already-fingerprint-checked body.
	//
	// The undo snapshot is taken inside this same quiesce rather than as a separate pass.
	// The world is stopped either way, so the extra cost is one capture into memory instead
	// of a second stop-the-world -- and taking it here is also the only way to be sure the
	// snapshot describes exactly the world this load is about to replace.
	static OperationResult ApplyStateBody(std::vector<uint8>& body)
	{
		std::vector<uint8> undoBody;
		bool undoCaptured = false;
		OperationResult result = OperationResult::Ok();
		{
			// No pause or resume from elsewhere until the world runs again (see GetPauseMutex)
			std::unique_lock pauseLock(CafeSystem::GetPauseMutex());
			LiftPauseScope liftPause(false);
			QuiesceScope quiesce;
			if (!quiesce.IsHeld())
				return OperationResult::Fail(GetQuiesceStatusMessage(quiesce.GetStatus()));
			if (CafeSystem::IsTitleOnline())
				return OperationResult::Fail(kOnlineMessage);

			ReserveCaptureBuffer(undoBody);
			StateStream undoStream = StateStream::MakeWriter(undoBody);
			DoStateBody(undoStream);
			// A failed snapshot is not a reason to refuse the load the user asked for, but
			// it does mean there is nothing to undo to, and silently keeping a stale one
			// would be worse than having none.
			undoCaptured = !undoStream.HasError();
			if (!undoCaptured)
				cemuLog_log(LogType::Force, "Save state: could not capture undo snapshot: {}", undoStream.GetError());

			StateStream stream = StateStream::MakeReader(body);
			DoStateBody(stream);
			if (stream.HasError())
			{
				// Guest memory is partially overwritten at this point. The snapshot taken
				// above is the way back, so it is kept even though the load failed.
				result = OperationResult::Fail(fmt::format("State load failed after modifying memory: {}", stream.GetError()));
			}
			else
			{
				// Host residue that is rebuilt rather than serialized.
				coreinit::__OSRebuildHostThreadsAfterStateLoad();
				// Threads blocked inside an HLE call cannot resume the host continuation the
				// fiber rebuild just discarded, so they are restarted at the call instead.
				// Must run before the run queue counts are rebuilt: this is what puts threads
				// back on the run queues that those counts are derived from.
				coreinit::__OSRestartHLEBlockedThreadsAfterStateLoad();
				// Must happen before the scope releases the cores: they consult these counters
				// the moment they leave the barrier, and a stale count means they go straight
				// back to sleep with a full run queue.
				coreinit::__OSRebuildRunQueueCountsAfterStateLoad();

				// Translated code may no longer match the restored guest memory. Dropping the
				// whole cache costs a few seconds of re-JIT and carries no correctness risk,
				// which is the same policy Dolphin applies to its JIT cache.
				PPCRecompiler_invalidateRange(0, 0xFFFFFFFF);

				// Same argument on the GPU side: the texture, buffer and index caches are keyed
				// by guest address, and every one of those addresses just changed underneath
				// them. The drop is performed by the parked GPU thread and completes before
				// this scope releases the cores, so no drawcall ever sees a stale cache.
				RequestGpuCacheDropOnRelease();
			}
		}
		// The world is running again. The state just applied is no longer needed, and freeing
		// it before compressing the undo snapshot keeps the peak lower.
		const size_t restoredSize = body.size();
		body.clear();
		body.shrink_to_fit();

		if (undoCaptured)
			StoreUndoState(undoBody, BuildCurrentFingerprint());
		else if (result.success)
			DiscardUndoState(); // it describes a world this load just replaced

		if (!result.success)
			return result;
		cemuLog_log(LogType::Force, "Save state: restored {} MiB of guest state, rebuilt host threads",
					restoredSize / (1024 * 1024));
		// Started after the cores are released so it observes the world actually running
		// rather than the barrier.
		coreinit::__OSStartStallWatchdogAfterStateLoad();
		return OperationResult::Ok();
	}

	// Restores the session the last load replaced, and keeps what it just replaced in turn,
	// so undo toggles rather than being a one-shot. ApplyStateBody() does the second half by
	// snapshotting before it applies anything, and leaves the current snapshot alone when it
	// refuses before touching the world.
	OperationResult UndoLoadState()
	{
		if (s_undoCompressed.empty())
			return OperationResult::Fail("There is no state to undo to");
		if (CafeSystem::IsTitleOnline())
			return OperationResult::Fail(kOnlineMessage);
		// The snapshot describes the world that was running when it was taken. Closing a
		// title and starting another leaves it behind, and applying it then would drop one
		// game's memory into another.
		const std::string incompatibility = s_undoFingerprint.GetIncompatibilityReason(BuildCurrentFingerprint());
		if (!incompatibility.empty())
		{
			DiscardUndoState();
			return OperationResult::Fail(incompatibility);
		}
		std::vector<uint8> body;
		try
		{
			body.resize((size_t)s_undoSize);
		}
		catch (const std::bad_alloc&)
		{
			return OperationResult::Fail("Not enough free memory to undo the load");
		}
		const size_t inflated = ZSTD_decompress(body.data(), body.size(), s_undoCompressed.data(), s_undoCompressed.size());
		if (ZSTD_isError(inflated) || inflated != body.size())
		{
			DiscardUndoState();
			return OperationResult::Fail("The undo snapshot is damaged");
		}
		return ApplyStateBody(body);
	}
} // namespace SaveStates
