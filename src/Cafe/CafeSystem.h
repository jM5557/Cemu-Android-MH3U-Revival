#pragma once
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/TitleList/TitleId.h"

enum class CosCapabilityBits : uint64;
enum class CosCapabilityGroup : uint32;
enum class CafeConsoleRegion;

namespace CafeSystem
{
	class SystemImplementation
	{
	public:
		virtual void CafeRecreateCanvas() = 0;
	};

	enum class PREPARE_STATUS_CODE
	{
		SUCCESS,
		INVALID_RPX,
		UNABLE_TO_MOUNT, // failed to mount through TitleInfo (most likely caused by an invalid or outdated path)
	};

	class CafeSystemCallbacks
	{
	public:
		virtual void updateWindowTitles(bool isIdle, bool isLoading, double fps) = 0;
		virtual void notifyGameLoaded() = 0;
	};

	void registerCafeSystemCallbacks(CafeSystemCallbacks* cafeSystemCallbacks);
	void unregisterCafeSystemCallbacks();
	CafeSystemCallbacks* getCafeSystemCallbacks();

	void Initialize();
	void SetImplementation(SystemImplementation* impl);
    void Shutdown();

	PREPARE_STATUS_CODE PrepareForegroundTitle(TitleId titleId);
	PREPARE_STATUS_CODE PrepareForegroundTitleFromStandaloneRPX(const fs::path& path);
	void LaunchForegroundTitle();
	bool IsTitleRunning();

	// Pause: every game thread is suspended and audio output stops. The CPU idle loop and the GPU
	// thread slow their polling right down, and the last frame keeps being shown (with the
	// overlays, so menus still work). Safe to call from any thread. Refused while the game is
	// online.
	void PauseTitle();
	void ResumeTitle();
	bool IsTitlePaused();

	// The game has a network socket open. A paused game stops answering, and a save state can
	// neither hold a connection nor bring one back, so either would drop it from an online
	// session (a multiplayer room drops the player). Pause and save states are refused while
	// this is true.
	bool IsTitleOnline();

	bool GetOverrideArgStr(std::vector<std::string>& args);
	void SetOverrideArgs(std::span<std::string> args);
	void UnsetOverrideArgs();

	TitleId GetForegroundTitleId();
	uint16 GetForegroundTitleVersion();
	uint32 GetForegroundTitleSDKVersion();
	CafeConsoleRegion GetForegroundTitleRegion();
	CafeConsoleRegion GetPlatformRegion();
	std::string GetForegroundTitleName();
	std::string GetForegroundTitleArgStr();
	uint32 GetForegroundTitleOlvAccesskey();
	CosCapabilityBits GetForegroundTitleCosCapabilities(CosCapabilityGroup group);

	void ShutdownTitle();

	std::string GetMlcStoragePath(TitleId titleId);
	void MlcStorageMountAllTitles();

	std::string GetInternalVirtualCodeFolder();

	uint32 GetRPXHashBase();
	uint32 GetRPXHashUpdated();

	void RequestRecreateCanvas();
};

extern RPLModule* applicationRPX;

extern std::atomic_bool g_isGPUInitFinished;
