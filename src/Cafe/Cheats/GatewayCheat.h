#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Gateway-style cheat codes (the format Citra/Azahar use for 3DS cheats), adapted to the Wii U.
//
// Syntax is unchanged: every line is two 32-bit hex words, "XXXXXXXX YYYYYYYY". What differs from
// the 3DS is the machine underneath: the Wii U is big-endian, so a 32-bit write of 38A00000 stores
// the bytes 38 A0 00 00 in that order, which is exactly how a PowerPC instruction is encoded.
//
// Addresses carry 28 bits (the top nibble is the code type), so 0x00000000-0x0FFFFFFF can be
// reached directly. That covers the game's code (0x02000000+) and its static data. Heap memory at
// 0x10000000 and above is reached through the offset register, e.g.
//     D3000000 10000000    offset = 0x10000000
//     00123450 0000270F    [0x10123450] = 9999
//
// Supported code types
//   0XXXXXXX YYYYYYYY   32-bit write  [X+offset] = Y
//   1XXXXXXX 0000YYYY   16-bit write
//   2XXXXXXX 000000YY   8-bit write
//   3XXXXXXX YYYYYYYY   if [X+offset] <  Y   (32-bit)
//   4XXXXXXX YYYYYYYY   if [X+offset] >  Y
//   5XXXXXXX YYYYYYYY   if [X+offset] == Y
//   6XXXXXXX YYYYYYYY   if [X+offset] != Y
//   7XXXXXXX ZZZZYYYY   if ([X+offset] & ~Z) <  Y   (16-bit, Z is a mask of bits to ignore)
//   8XXXXXXX ZZZZYYYY   if ([X+offset] & ~Z) >  Y
//   9XXXXXXX ZZZZYYYY   if ([X+offset] & ~Z) == Y
//   AXXXXXXX ZZZZYYYY   if ([X+offset] & ~Z) != Y
//   BXXXXXXX 00000000   offset = [X+offset]
//   C0000000 YYYYYYYY   loop: run the block up to the next D1/D2 Y times
//   D0000000 00000000   end if
//   D1000000 00000000   end loop
//   D2000000 00000000   end loop, then end everything and clear offset and data
//   D3000000 YYYYYYYY   offset = Y
//   D4000000 YYYYYYYY   data += Y
//   D5000000 YYYYYYYY   data = Y
//   D6000000 YYYYYYYY   [Y+offset] = data (32-bit), offset += 4
//   D7000000 YYYYYYYY   [Y+offset] = data (16-bit), offset += 2
//   D8000000 YYYYYYYY   [Y+offset] = data (8-bit),  offset += 1
//   D9000000 YYYYYYYY   data = [Y+offset] (32-bit)
//   DA000000 YYYYYYYY   data = [Y+offset] (16-bit)
//   DB000000 YYYYYYYY   data = [Y+offset] (8-bit)
//   DC000000 YYYYYYYY   offset += Y
//   EXXXXXXX YYYYYYYY   copy the Y bytes written on the following lines to X+offset, in order
//
// Button conditions (DD) are 3DS-specific and rejected. Conditions nest; a false condition skips
// everything up to its matching D0 (or a D2).

namespace GatewayCheat
{
	// How the interpreter reaches guest memory. All values are in host order; the implementation
	// does the big-endian conversion.
	class Memory
	{
	public:
		virtual ~Memory() = default;
		virtual bool IsValid(uint32_t address, uint32_t size) = 0;
		virtual uint32_t Read32(uint32_t address) = 0;
		virtual uint16_t Read16(uint32_t address) = 0;
		virtual uint8_t Read8(uint32_t address) = 0;
		virtual void Write32(uint32_t address, uint32_t value) = 0;
		virtual void Write16(uint32_t address, uint16_t value) = 0;
		virtual void Write8(uint32_t address, uint8_t value) = 0;
	};

	enum class Op : uint8_t
	{
		Write32, Write16, Write8,
		Less32, Greater32, Equal32, NotEqual32,
		Less16, Greater16, Equal16, NotEqual16,
		LoadOffset,
		Loop,
		EndIf, EndLoop, Terminator,
		SetOffset, AddData, SetData,
		IncWrite32, IncWrite16, IncWrite8,
		Load32, Load16, Load8,
		AddOffset,
		Patch,
	};

	struct Line
	{
		Op op;
		uint32_t first;   // full first word
		uint32_t second;  // full second word
		std::vector<uint8_t> patchBytes; // Op::Patch only
	};

	class Code
	{
	public:
		// Parses code lines. Blank lines are ignored. On failure returns false and fills error
		// with a message naming the offending line (1-based within the given lines).
		static bool Compile(const std::vector<std::string>& lines, Code& out, std::string& error);

		// Runs the code once. Returns false if it touched an unmapped address; error describes it.
		bool Execute(Memory& mem, std::string* error = nullptr) const;

		bool Empty() const { return m_lines.empty(); }

	private:
		std::vector<Line> m_lines;
	};
}
