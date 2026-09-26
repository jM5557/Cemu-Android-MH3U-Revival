#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Per-title cheat lists, stored the way Citra/Azahar store them:
//   <UserData>/cheats/<titleId>.txt
//
//   [Cheat name]
//   {Optional notes, may span lines}
//   *cemu_enabled
//   02123450 38A00000
//
// "*citra_enabled" is accepted as well, so a Citra cheat file's structure carries over (its codes
// still need Wii U addresses). Enabled cheats run once per vsync (about 60 times a second) while
// the title is running. See GatewayCheat.h for the code types.
namespace CheatManager
{
	struct Cheat
	{
		std::string name;
		std::string notes;
		bool enabled = false;
		std::vector<std::string> code;
	};

	std::filesystem::path GetCheatFolder();
	std::filesystem::path GetCheatFile(uint64_t titleId);

	std::vector<Cheat> Parse(const std::string& text);
	std::string Serialize(const std::vector<Cheat>& cheats);

	// Returns an empty list if the title has no cheat file.
	std::vector<Cheat> Load(uint64_t titleId);
	// Writes the file and, if this title is running, applies the list straight away.
	bool Save(uint64_t titleId, const std::vector<Cheat>& cheats, std::string& error);
	// Makes the running game use this list without touching the file. No-op for other titles.
	void Apply(uint64_t titleId, const std::vector<Cheat>& cheats);

	// Checks that a cheat's code parses. On failure, error says which line and why.
	bool Validate(const Cheat& cheat, std::string& error);

	// Emulator lifecycle
	void OnTitleStart(uint64_t titleId);
	void OnTitleStop();
	bool IsRunningTitle(uint64_t titleId);

	// Called once per vsync from the GPU/vsync thread.
	void RunFrame();
}
