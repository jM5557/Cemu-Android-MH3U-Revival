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

	static OperationResult WriteStateFile(const fs::path& path, StateHead& head, const std::vector<uint8>& body)
	{
		std::vector<uint8> headBuffer;
		StateStream headStream = StateStream::MakeWriter(headBuffer);
		head.DoState(headStream);
		if (headStream.HasError())
			return OperationResult::Fail(headStream.GetError());

		const size_t bound = ZSTD_compressBound(body.size());
		std::vector<uint8> compressed(bound);
		const size_t compressedSize = ZSTD_compress(compressed.data(), bound, body.data(), body.size(), 3);
		if (ZSTD_isError(compressedSize))
			return OperationResult::Fail(fmt::format("Compression failed: {}", ZSTD_getErrorName(compressedSize)));

		StateFileHeader fileHeader{};
		fileHeader.magic = kStateFileMagic;
		fileHeader.formatVersion = kStateFormatVersion;
		fileHeader.flags = 0;
		fileHeader.headSize = (uint32)headBuffer.size();
		fileHeader.bodySizeUncompressed = (uint64)body.size();
		fileHeader.bodySizeCompressed = (uint64)compressedSize;

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
			file.write((const char*)compressed.data(), (std::streamsize)compressedSize);
			if (!file.good())
			{
				file.close();
				fs::remove(tempPath, ec);
				return OperationResult::Fail("Failed while writing state data (out of disk space?)");
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

	OperationResult SaveToFile(const fs::path& path)
	{
		std::vector<uint8> body;
		StateHead head;

		{
			QuiesceScope quiesce;
			if (!quiesce.IsHeld())
				return OperationResult::Fail(GetQuiesceStatusMessage(quiesce.GetStatus()));

			head.fingerprint = BuildCurrentFingerprint();
			head.unixTimestamp = (uint64)std::time(nullptr);

			// Reserve up front so the capture does not repeatedly reallocate a buffer
			// that can reach hundreds of megabytes while the world is stopped.
			body.reserve(64 * 1024 * 1024);

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

	OperationResult LoadFromFile(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		StateFileHeader fileHeader{};
		StateHead head;
		if (const OperationResult headResult = ReadStateHead(file, path, fileHeader, head); !headResult.success)
			return headResult;

		// Check compatibility before anything touches the running world.
		const std::string incompatibility = head.fingerprint.GetIncompatibilityReason(BuildCurrentFingerprint());
		if (!incompatibility.empty())
			return OperationResult::Fail(incompatibility);

		std::vector<uint8> compressed((size_t)fileHeader.bodySizeCompressed);
		if (!compressed.empty())
		{
			file.read((char*)compressed.data(), (std::streamsize)compressed.size());
			if (!file.good())
				return OperationResult::Fail("State file is truncated (body)");
		}
		file.close();

		std::vector<uint8> body((size_t)fileHeader.bodySizeUncompressed);
		const size_t decompressedSize = ZSTD_decompress(body.data(), body.size(), compressed.data(), compressed.size());
		if (ZSTD_isError(decompressedSize))
			return OperationResult::Fail(fmt::format("Decompression failed: {}", ZSTD_getErrorName(decompressedSize)));
		if (decompressedSize != body.size())
			return OperationResult::Fail("State body size mismatch");

		{
			QuiesceScope quiesce;
			if (!quiesce.IsHeld())
				return OperationResult::Fail(GetQuiesceStatusMessage(quiesce.GetStatus()));

			StateStream stream = StateStream::MakeReader(body);
			DoStateBody(stream);
			if (stream.HasError())
			{
				// Guest memory is now partially overwritten and there is no way back
				// without a backup state (phase 2 "undo load state").
				return OperationResult::Fail(fmt::format("State load failed after modifying memory: {}", stream.GetError()));
			}

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

		cemuLog_log(LogType::Force, "Save state: restored {} MiB of guest state, rebuilt host threads",
					body.size() / (1024 * 1024));
		// The restored world resumes, renders, and then stops. Started after the cores are
		// released so it observes the world actually running rather than the barrier.
		coreinit::__OSStartStallWatchdogAfterStateLoad();
		return OperationResult::Ok();
	}
} // namespace SaveStates
