#pragma once

#include <span>

namespace SaveStates
{
	// Bidirectional serializer, modeled on Dolphin's PointerWrap.
	//
	// Every participating subsystem implements a single DoState(StateStream&) function
	// that is used for both saving and loading. Asymmetric read/write paths are the
	// largest source of save state corruption in every emulator that has them; running
	// one code path in both directions makes that class of bug structurally impossible.
	class StateStream
	{
	  public:
		enum class Mode
		{
			Measure, // compute the serialized size without storing anything
			Write,
			Read,
		};

		static StateStream MakeMeasure();
		static StateStream MakeWriter(std::vector<uint8>& out);
		static StateStream MakeReader(std::span<const uint8> in);

		Mode GetMode() const { return m_mode; }
		bool IsReading() const { return m_mode == Mode::Read; }
		bool IsWriting() const { return m_mode == Mode::Write; }
		bool IsMeasuring() const { return m_mode == Mode::Measure; }

		// Once an error is set the stream is poisoned: every subsequent operation is a
		// no-op. Callers therefore only need to test HasError() at chunk boundaries
		// rather than after every field.
		bool HasError() const { return m_hasError; }
		const std::string& GetError() const { return m_error; }
		void SetError(std::string error);

		// Number of bytes written/read so far. In Measure mode this is the total size.
		size_t GetOffset() const { return m_offset; }

		void DoBytes(void* ptr, size_t size);

		template<typename T>
		void Do(T& v)
		{
			static_assert(std::is_trivially_copyable_v<T>, "StateStream::Do requires a trivially copyable type");
			static_assert(!std::is_pointer_v<T>, "Raw host pointers must never enter a save state");
			DoBytes(&v, sizeof(T));
		}

		template<typename T>
		void DoPODVector(std::vector<T>& v)
		{
			static_assert(std::is_trivially_copyable_v<T>, "DoPODVector requires a trivially copyable element type");
			uint64 count = (uint64)v.size();
			Do(count);
			if (m_hasError)
				return;
			if (IsReading())
			{
				// A corrupt length must not turn into a huge allocation
				if (count > kElementLimit)
				{
					SetError(fmt::format("Vector element count {} exceeds sanity limit", count));
					return;
				}
				v.resize((size_t)count);
			}
			if (count > 0)
				DoBytes(v.data(), (size_t)count * sizeof(T));
		}

		void DoString(std::string& v);

		// Section sentinel. If a deserializer drifts out of sync this fails here, naming
		// the subsystem, instead of silently reading garbage hundreds of megabytes later.
		void DoMarker(uint32 magic, const char* subsystemName);

	  private:
		explicit StateStream(Mode mode) : m_mode(mode) {}

		static constexpr uint64 kElementLimit = 64 * 1024 * 1024;

		Mode m_mode;
		size_t m_offset = 0;

		std::vector<uint8>* m_writeBuffer = nullptr; // Write mode
		std::span<const uint8> m_readBuffer;         // Read mode

		bool m_hasError = false;
		std::string m_error;
	};

	// Chunk markers. Spelled as FourCCs so they are greppable in a hex dump of the
	// uncompressed body when debugging a desync.
	constexpr uint32 kMarkerMEMR = 0x4D454D52; // 'MEMR' guest memory
	constexpr uint32 kMarkerCPUS = 0x43505553; // 'CPUS' cpu/scheduler
	constexpr uint32 kMarkerTIME = 0x54494D45; // 'TIME' emulated timebase
	constexpr uint32 kMarkerALRM = 0x414C524D; // 'ALRM' host alarms
	constexpr uint32 kMarkerGX2S = 0x47583253; // 'GX2S' command ring / GPU submission
	constexpr uint32 kMarkerLATT = 0x4C415454; // 'LATT' Latte GPU state
	constexpr uint32 kMarkerEND  = 0x454E4421; // 'END!' end of body
} // namespace SaveStates
