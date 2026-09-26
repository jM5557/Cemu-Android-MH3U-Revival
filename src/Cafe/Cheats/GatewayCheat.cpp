#include "Cafe/Cheats/GatewayCheat.h"

#include <cctype>
#include <cstdio>

namespace GatewayCheat
{
	// Hard cap on lines executed per run, so a huge loop count can't stall the thread running it.
	static constexpr uint64_t kMaxStepsPerRun = 1000000;

	static std::string Hex32(uint32_t v)
	{
		char buf[12];
		snprintf(buf, sizeof(buf), "%08X", v);
		return buf;
	}

	static bool ParseHexWord(const std::string& s, uint32_t& out)
	{
		if (s.size() != 8)
			return false;
		uint32_t v = 0;
		for (char c : s)
		{
			v <<= 4;
			if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
			else return false;
		}
		out = v;
		return true;
	}

	// Splits "XXXXXXXX YYYYYYYY" into two words. Returns false if the line isn't exactly that.
	static bool ParseLine(const std::string& line, uint32_t& first, uint32_t& second)
	{
		std::vector<std::string> tokens;
		std::string cur;
		for (char c : line)
		{
			if (std::isspace((unsigned char)c))
			{
				if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
			}
			else
				cur.push_back(c);
		}
		if (!cur.empty())
			tokens.push_back(cur);
		if (tokens.size() != 2)
			return false;
		return ParseHexWord(tokens[0], first) && ParseHexWord(tokens[1], second);
	}

	static bool IsBlank(const std::string& s)
	{
		for (char c : s)
			if (!std::isspace((unsigned char)c))
				return false;
		return true;
	}

	static bool IsCondition(Op op)
	{
		return op >= Op::Less32 && op <= Op::NotEqual16;
	}

	bool Code::Compile(const std::vector<std::string>& lines, Code& out, std::string& error)
	{
		out.m_lines.clear();
		for (size_t i = 0; i < lines.size(); i++)
		{
			if (IsBlank(lines[i]))
				continue;
			const std::string where = "Line " + std::to_string(i + 1) + ": ";
			uint32_t first, second;
			if (!ParseLine(lines[i], first, second))
			{
				error = where + "expected two 8-digit hex values, like \"02123450 38A00000\"";
				return false;
			}

			Line l{};
			l.first = first;
			l.second = second;
			const uint32_t type = first >> 28;
			switch (type)
			{
			case 0x0: l.op = Op::Write32; break;
			case 0x1: l.op = Op::Write16; break;
			case 0x2: l.op = Op::Write8; break;
			case 0x3: l.op = Op::Less32; break;
			case 0x4: l.op = Op::Greater32; break;
			case 0x5: l.op = Op::Equal32; break;
			case 0x6: l.op = Op::NotEqual32; break;
			case 0x7: l.op = Op::Less16; break;
			case 0x8: l.op = Op::Greater16; break;
			case 0x9: l.op = Op::Equal16; break;
			case 0xA: l.op = Op::NotEqual16; break;
			case 0xB: l.op = Op::LoadOffset; break;
			case 0xC: l.op = Op::Loop; break;
			case 0xD:
			{
				const uint32_t sub = (first >> 24) & 0xF;
				switch (sub)
				{
				case 0x0: l.op = Op::EndIf; break;
				case 0x1: l.op = Op::EndLoop; break;
				case 0x2: l.op = Op::Terminator; break;
				case 0x3: l.op = Op::SetOffset; break;
				case 0x4: l.op = Op::AddData; break;
				case 0x5: l.op = Op::SetData; break;
				case 0x6: l.op = Op::IncWrite32; break;
				case 0x7: l.op = Op::IncWrite16; break;
				case 0x8: l.op = Op::IncWrite8; break;
				case 0x9: l.op = Op::Load32; break;
				case 0xA: l.op = Op::Load16; break;
				case 0xB: l.op = Op::Load8; break;
				case 0xC: l.op = Op::AddOffset; break;
				case 0xD:
					error = where + "button conditions (DD) are 3DS-only and aren't supported";
					return false;
				default:
					error = where + "unknown code type D" + Hex32(first).substr(1, 1);
					return false;
				}
				break;
			}
			case 0xE:
			{
				l.op = Op::Patch;
				const uint32_t byteCount = second;
				const size_t dataLines = (byteCount + 7) / 8;
				if (byteCount == 0 || byteCount > 0x10000)
				{
					error = where + "patch (E) byte count must be between 1 and 0x10000";
					return false;
				}
				// collect the data lines that follow, skipping blank ones
				size_t j = i + 1;
				size_t got = 0;
				while (got < dataLines)
				{
					while (j < lines.size() && IsBlank(lines[j]))
						j++;
					if (j >= lines.size())
					{
						error = where + "patch (E) needs " + std::to_string(dataLines) + " data line(s) after it";
						return false;
					}
					uint32_t a, b;
					if (!ParseLine(lines[j], a, b))
					{
						error = "Line " + std::to_string(j + 1) + ": bad data line for the patch (E) code above it";
						return false;
					}
					const uint32_t words[2] = { a, b };
					for (uint32_t w : words)
						for (int shift = 24; shift >= 0; shift -= 8)
							if (l.patchBytes.size() < byteCount)
								l.patchBytes.push_back((uint8_t)(w >> shift));
					got++;
					j++;
				}
				i = j - 1;
				break;
			}
			default:
				error = where + "unknown code type " + Hex32(first).substr(0, 1);
				return false;
			}
			out.m_lines.push_back(std::move(l));
		}
		return true;
	}

	bool Code::Execute(Memory& mem, std::string* error) const
	{
		uint32_t offset = 0;
		uint32_t data = 0;
		uint32_t skipDepth = 0; // > 0 while inside a condition that was false
		bool loopActive = false;
		size_t loopStart = 0;
		uint32_t loopRemaining = 0;
		uint64_t steps = 0;

		auto fail = [&](const std::string& msg) {
			if (error)
				*error = msg;
			return false;
		};
		auto badAddress = [&](uint32_t addr, uint32_t size) {
			return fail("address " + Hex32(addr) + " (" + std::to_string(size) + " bytes) is not mapped");
		};

		for (size_t i = 0; i < m_lines.size(); i++)
		{
			if (++steps > kMaxStepsPerRun)
				return fail("stopped after too many steps (check the loop count)");

			const Line& l = m_lines[i];

			if (skipDepth > 0)
			{
				if (IsCondition(l.op))
					skipDepth++;
				else if (l.op == Op::EndIf)
					skipDepth--;
				else if (l.op == Op::Terminator)
				{
					skipDepth = 0;
					loopActive = false;
					offset = 0;
					data = 0;
				}
				continue;
			}

			const uint32_t addr = (l.first & 0x0FFFFFFF) + offset;
			switch (l.op)
			{
			case Op::Write32:
				if (!mem.IsValid(addr, 4)) return badAddress(addr, 4);
				mem.Write32(addr, l.second);
				break;
			case Op::Write16:
				if (!mem.IsValid(addr, 2)) return badAddress(addr, 2);
				mem.Write16(addr, (uint16_t)l.second);
				break;
			case Op::Write8:
				if (!mem.IsValid(addr, 1)) return badAddress(addr, 1);
				mem.Write8(addr, (uint8_t)l.second);
				break;

			case Op::Less32:
			case Op::Greater32:
			case Op::Equal32:
			case Op::NotEqual32:
			{
				if (!mem.IsValid(addr, 4)) return badAddress(addr, 4);
				const uint32_t v = mem.Read32(addr);
				bool pass;
				if (l.op == Op::Less32) pass = v < l.second;
				else if (l.op == Op::Greater32) pass = v > l.second;
				else if (l.op == Op::Equal32) pass = v == l.second;
				else pass = v != l.second;
				if (!pass)
					skipDepth = 1;
				break;
			}
			case Op::Less16:
			case Op::Greater16:
			case Op::Equal16:
			case Op::NotEqual16:
			{
				if (!mem.IsValid(addr, 2)) return badAddress(addr, 2);
				const uint16_t mask = (uint16_t)(l.second >> 16);
				const uint16_t cmp = (uint16_t)l.second;
				const uint16_t v = (uint16_t)(mem.Read16(addr) & (uint16_t)~mask);
				bool pass;
				if (l.op == Op::Less16) pass = v < cmp;
				else if (l.op == Op::Greater16) pass = v > cmp;
				else if (l.op == Op::Equal16) pass = v == cmp;
				else pass = v != cmp;
				if (!pass)
					skipDepth = 1;
				break;
			}

			case Op::LoadOffset:
				if (!mem.IsValid(addr, 4)) return badAddress(addr, 4);
				offset = mem.Read32(addr);
				break;

			case Op::Loop:
				// no nesting: a new loop replaces the current one, as on Gateway
				loopActive = true;
				loopStart = i;
				loopRemaining = (l.second > 0) ? l.second - 1 : 0;
				break;

			case Op::EndIf:
				break; // closing a condition that passed

			case Op::EndLoop:
			case Op::Terminator:
				if (loopActive && loopRemaining > 0)
				{
					loopRemaining--;
					i = loopStart; // loop increment lands on the first line of the body
					break;
				}
				loopActive = false;
				if (l.op == Op::Terminator)
				{
					offset = 0;
					data = 0;
				}
				break;

			case Op::SetOffset: offset = l.second; break;
			case Op::AddOffset: offset += l.second; break;
			case Op::AddData: data += l.second; break;
			case Op::SetData: data = l.second; break;

			case Op::IncWrite32:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 4)) return badAddress(a, 4);
				mem.Write32(a, data);
				offset += 4;
				break;
			}
			case Op::IncWrite16:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 2)) return badAddress(a, 2);
				mem.Write16(a, (uint16_t)data);
				offset += 2;
				break;
			}
			case Op::IncWrite8:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 1)) return badAddress(a, 1);
				mem.Write8(a, (uint8_t)data);
				offset += 1;
				break;
			}

			case Op::Load32:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 4)) return badAddress(a, 4);
				data = mem.Read32(a);
				break;
			}
			case Op::Load16:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 2)) return badAddress(a, 2);
				data = mem.Read16(a);
				break;
			}
			case Op::Load8:
			{
				const uint32_t a = l.second + offset;
				if (!mem.IsValid(a, 1)) return badAddress(a, 1);
				data = mem.Read8(a);
				break;
			}

			case Op::Patch:
			{
				const uint32_t size = (uint32_t)l.patchBytes.size();
				if (!mem.IsValid(addr, size)) return badAddress(addr, size);
				for (uint32_t k = 0; k < size; k++)
					mem.Write8(addr + k, l.patchBytes[k]);
				break;
			}
			}
		}
		return true;
	}
}
