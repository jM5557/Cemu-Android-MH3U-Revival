// Dumpless "online gate" file generation, the Android equivalent of what the mh3u-revival
// launcher does on Windows.
//
// Cemu gates online mode on iosuCrypt_checkRequirementsForOnlineMode(), which wants:
//   <UserData>/otp.bin      exactly 1024 bytes
//   <UserData>/seeprom.bin  exactly  512 bytes
//   <MLC>/sys/title/0005001b/10054000/content/<each cert name and key>   (existence only)
//
// The certificate paths are read from iosuCrypt_getCertificateNames()/getCertificateKeys()
// rather than hardcoded, so this keeps working if upstream changes the list.
//
// Nothing here contains Nintendo data. otp.bin and seeprom.bin are zero-filled and the
// certificates are 4-byte stubs; they exist only to satisfy the file checks. That is sufficient
// because the MH3U revival patch in napi_act.cpp short-circuits ACT_GetNexToken_WithCache before
// any of this material would be used — no OAuth, no TLS to Nintendo, no crypto. It follows that
// these files enable *the revival server only*. They cannot reach Nintendo or Pretendo, and
// selecting either of those network services with stub files will simply fail.

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "util/helpers/helpers.h"
#include "JNIUtils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	bool WriteIfAbsent(const fs::path& path, size_t byteCount)
	{
		std::error_code ec;
		if (fs::exists(path, ec))
		{
			// Never clobber a real dump. Only replace a file that is the wrong size, which means
			// it is either our own earlier stub or something truncated.
			if (byteCount == 0 || fs::file_size(path, ec) == byteCount)
				return true;
		}
		fs::create_directories(path.parent_path(), ec);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return false;
		const std::vector<char> zeroes(byteCount, 0);
		out.write(zeroes.data(), (std::streamsize)zeroes.size());
		return out.good();
	}

	std::string Narrow(const wchar_t* wide)
	{
		std::string out;
		for (const wchar_t* p = wide; *p; p++)
			out.push_back((char)*p); // cert paths are pure ASCII
		return out;
	}
} // namespace

// Creates every file the online-mode check requires, then re-evaluates the check.
// Returns an empty string on success, or a human-readable reason on failure.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeOnlineFiles_generateGateFiles(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	if (!WriteIfAbsent(ActiveSettings::GetUserDataPath("otp.bin"), 1024))
		return JNIUtils::ToJString(env, std::string("could not write otp.bin"));
	if (!WriteIfAbsent(ActiveSettings::GetUserDataPath("seeprom.bin"), 512))
		return JNIUtils::ToJString(env, std::string("could not write seeprom.bin"));

	const fs::path certRoot = ActiveSettings::GetMlcPath("sys/title/0005001b/10054000/content");
	auto writeCert = [&](const wchar_t* name) -> bool {
		// 4 bytes, not 0: the check loads the file into memory, and a zero-length read is not
		// worth relying on being treated as success.
		return WriteIfAbsent(certRoot / Narrow(name), 4);
	};

	for (const wchar_t* name : iosuCrypt_getCertificateNames())
	{
		if (!writeCert(name))
			return JNIUtils::ToJString(env, "could not write " + Narrow(name));
	}
	for (const wchar_t* key : iosuCrypt_getCertificateKeys())
	{
		if (!writeCert(key))
			return JNIUtils::ToJString(env, "could not write " + Narrow(key));
	}

	ActiveSettings::RefreshOnlineFileStatus();
	if (!ActiveSettings::HasRequiredOnlineFiles())
	{
		std::string reason;
		iosuCrypt_checkRequirementsForOnlineMode(reason);
		return JNIUtils::ToJString(env, "files written but the check still fails: " + reason);
	}
	return JNIUtils::ToJString(env, std::string());
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeOnlineFiles_hasRequiredOnlineFiles([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	ActiveSettings::RefreshOnlineFileStatus();
	return ActiveSettings::HasRequiredOnlineFiles() ? JNI_TRUE : JNI_FALSE;
}

// True only when both halves are satisfied: the files exist AND the active account's network
// service is not Offline. Having the files is not enough, which is the usual point of confusion.
extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeOnlineFiles_isOnlineEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return ActiveSettings::IsOnlineEnabled() ? JNI_TRUE : JNI_FALSE;
}

// 0 = Offline, 1 = Nintendo, 2 = Pretendo, 3 = Custom. MH3U revival needs Custom.
extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeOnlineFiles_getNetworkService([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return (jint)GetConfig().GetAccountNetworkService(ActiveSettings::GetPersistentId());
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeOnlineFiles_setNetworkService([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint service)
{
	GetConfig().SetAccountSelectedService(ActiveSettings::GetPersistentId(), (NetworkService)service);
	GetConfigHandle().Save();
}
