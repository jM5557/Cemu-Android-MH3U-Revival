#include "Cafe/SaveState/StateStream.h"

namespace SaveStates
{
	StateStream StateStream::MakeMeasure()
	{
		return StateStream(Mode::Measure);
	}

	StateStream StateStream::MakeWriter(std::vector<uint8>& out)
	{
		StateStream s(Mode::Write);
		s.m_writeBuffer = &out;
		return s;
	}

	StateStream StateStream::MakeReader(std::span<const uint8> in)
	{
		StateStream s(Mode::Read);
		s.m_readBuffer = in;
		return s;
	}

	void StateStream::SetError(std::string error)
	{
		// Keep the first error. Later failures are usually consequences of it and the
		// original message is the one that identifies the faulty subsystem.
		if (m_hasError)
			return;
		m_hasError = true;
		m_error = std::move(error);
	}

	void StateStream::DoBytes(void* ptr, size_t size)
	{
		if (m_hasError || size == 0)
			return;
		switch (m_mode)
		{
		case Mode::Measure:
			m_offset += size;
			break;
		case Mode::Write:
			m_writeBuffer->insert(m_writeBuffer->end(), (const uint8*)ptr, (const uint8*)ptr + size);
			m_offset += size;
			break;
		case Mode::Read:
			if (m_offset + size > m_readBuffer.size())
			{
				SetError(fmt::format("Truncated state: wanted {} bytes at offset {}, only {} available",
									 size, m_offset, m_readBuffer.size() - std::min(m_offset, m_readBuffer.size())));
				return;
			}
			std::memcpy(ptr, m_readBuffer.data() + m_offset, size);
			m_offset += size;
			break;
		}
	}

	void StateStream::DoString(std::string& v)
	{
		uint64 length = (uint64)v.size();
		Do(length);
		if (m_hasError)
			return;
		if (IsReading())
		{
			if (length > kElementLimit)
			{
				SetError(fmt::format("String length {} exceeds sanity limit", length));
				return;
			}
			v.resize((size_t)length);
		}
		if (length > 0)
			DoBytes(v.data(), (size_t)length);
	}

	void StateStream::DoMarker(uint32 magic, const char* subsystemName)
	{
		uint32 value = magic;
		Do(value);
		if (m_hasError)
			return;
		if (IsReading() && value != magic)
		{
			SetError(fmt::format("State desynchronized at subsystem '{}' (offset {}): expected marker 0x{:08x}, found 0x{:08x}",
								 subsystemName, m_offset - sizeof(uint32), magic, value));
		}
	}
} // namespace SaveStates
