#include "Cafe/Cheats/CheatManager.h"
#include "Cafe/Cheats/GatewayCheat.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "config/ActiveSettings.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

namespace CheatManager
{
	// Guest code lives below this address; writes that change bytes there must drop any recompiled
	// copy of that code, or the game keeps running the old instructions.
	static constexpr uint32_t kCodeRegionEnd = 0x10000000;

	class CemuMemory final : public GatewayCheat::Memory
	{
	public:
		bool IsValid(uint32_t address, uint32_t size) override
		{
			if (size == 0 || (uint64_t)address + size > 0x100000000ull)
				return false;
			return memory_isAddressRangeAccessible(address, size);
		}
		uint32_t Read32(uint32_t address) override { return memory_readU32(address); }
		uint16_t Read16(uint32_t address) override { return memory_readU16(address); }
		uint8_t Read8(uint32_t address) override { return memory_readU8(address); }

		// Only store when the value differs: cheats rewrite the same values every frame, and an
		// unchanged instruction must not throw away recompiled code 60 times a second.
		void Write32(uint32_t address, uint32_t value) override
		{
			if (memory_readU32(address) == value)
				return;
			memory_writeU32(address, value);
			Invalidate(address, 4);
		}
		void Write16(uint32_t address, uint16_t value) override
		{
			if (memory_readU16(address) == value)
				return;
			memory_writeU16(address, value);
			Invalidate(address, 2);
		}
		void Write8(uint32_t address, uint8_t value) override
		{
			if (memory_readU8(address) == value)
				return;
			memory_writeU8(address, value);
			Invalidate(address, 1);
		}

	private:
		static void Invalidate(uint32_t address, uint32_t size)
		{
			if (address < kCodeRegionEnd)
				PPCRecompiler_invalidateRange(address & ~3u, (address + size + 3) & ~3u);
		}
	};

	struct ActiveCheat
	{
		std::string name;
		GatewayCheat::Code code;
		bool errorReported = false;
	};

	static std::mutex s_mutex;
	static uint64_t s_runningTitle = 0;
	static bool s_titleRunning = false;
	static std::vector<ActiveCheat> s_active;
	static std::atomic<bool> s_hasActive{false};

	fs::path GetCheatFolder()
	{
		return ActiveSettings::GetUserDataPath("cheats");
	}

	static std::string TitleIdHex(uint64_t titleId, bool upper)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), upper ? "%016llX" : "%016llx", (unsigned long long)titleId);
		return buf;
	}

	fs::path GetCheatFile(uint64_t titleId)
	{
		const fs::path folder = GetCheatFolder();
		// prefer an existing file in either case, so a copied-in Citra-style uppercase name works
		// on case-sensitive filesystems too
		std::error_code ec;
		const fs::path upper = folder / (TitleIdHex(titleId, true) + ".txt");
		const fs::path lower = folder / (TitleIdHex(titleId, false) + ".txt");
		if (!fs::exists(lower, ec) && fs::exists(upper, ec))
			return upper;
		return lower;
	}

	std::vector<Cheat> Load(uint64_t titleId)
	{
		std::ifstream f(GetCheatFile(titleId), std::ios::binary);
		if (!f)
			return {};
		std::stringstream ss;
		ss << f.rdbuf();
		return Parse(ss.str());
	}

	bool Validate(const Cheat& cheat, std::string& error)
	{
		GatewayCheat::Code code;
		return GatewayCheat::Code::Compile(cheat.code, code, error);
	}

	// Rebuilds the set of cheats that run each frame. Caller holds s_mutex.
	static void ActivateLocked(const std::vector<Cheat>& cheats)
	{
		s_active.clear();
		for (const auto& c : cheats)
		{
			if (!c.enabled)
				continue;
			ActiveCheat a;
			a.name = c.name;
			std::string error;
			if (!GatewayCheat::Code::Compile(c.code, a.code, error))
			{
				cemuLog_log(LogType::Force, "[Cheats] '{}' not enabled: {}", c.name, error);
				continue;
			}
			if (a.code.Empty())
				continue;
			cemuLog_log(LogType::Force, "[Cheats] Enabled '{}'", c.name);
			s_active.push_back(std::move(a));
		}
		s_hasActive = !s_active.empty();
	}

	bool Save(uint64_t titleId, const std::vector<Cheat>& cheats, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(GetCheatFolder(), ec);
		const fs::path target = GetCheatFile(titleId);
		const fs::path temp = fs::path(target).concat(".tmp");
		{
			std::ofstream f(temp, std::ios::binary | std::ios::trunc);
			if (!f)
			{
				error = "Could not write " + _pathToUtf8(temp);
				return false;
			}
			f << Serialize(cheats);
			if (!f)
			{
				error = "Could not write " + _pathToUtf8(temp);
				return false;
			}
		}
		fs::rename(temp, target, ec);
		if (ec)
		{
			fs::remove(temp, ec);
			error = "Could not replace " + _pathToUtf8(target);
			return false;
		}

		Apply(titleId, cheats);
		return true;
	}

	void Apply(uint64_t titleId, const std::vector<Cheat>& cheats)
	{
		std::lock_guard lock(s_mutex);
		if (s_titleRunning && s_runningTitle == titleId)
			ActivateLocked(cheats);
	}

	void OnTitleStart(uint64_t titleId)
	{
		const auto cheats = Load(titleId);
		std::lock_guard lock(s_mutex);
		s_runningTitle = titleId;
		s_titleRunning = true;
		ActivateLocked(cheats);
		if (!cheats.empty())
			cemuLog_log(LogType::Force, "[Cheats] {} cheat(s) in file, {} enabled", cheats.size(), s_active.size());
	}

	void OnTitleStop()
	{
		std::lock_guard lock(s_mutex);
		s_titleRunning = false;
		s_runningTitle = 0;
		s_active.clear();
		s_hasActive = false;
	}

	bool IsRunningTitle(uint64_t titleId)
	{
		std::lock_guard lock(s_mutex);
		return s_titleRunning && s_runningTitle == titleId;
	}

	void RunFrame()
	{
		if (!s_hasActive.load(std::memory_order_relaxed))
			return;
		// never stall the vsync thread behind the UI saving a file; just skip this frame
		std::unique_lock lock(s_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		CemuMemory mem;
		for (auto& a : s_active)
		{
			std::string error;
			if (!a.code.Execute(mem, &error) && !a.errorReported)
			{
				// keep running it: the address may become valid later (e.g. after a heap allocation)
				cemuLog_log(LogType::Force, "[Cheats] '{}': {}", a.name, error);
				a.errorReported = true;
			}
		}
	}
}
