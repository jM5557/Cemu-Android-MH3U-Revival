// The MH3U revival redirect in napi_act.cpp and nsysnet.cpp reads its target endpoint from
// <ConfigPath>/mh3u_server.txt. That file is fine on desktop but unreachable on Android, where the
// config directory is app-private. This bridge lets the Compose settings screen own the value and
// write the file, so the emulator core needs no Android-specific changes.
//
// The host is limited to 15 characters by the Wii U nexToken.host field (char[0x10]); validation
// lives here so the UI can reject bad input before it silently falls back to 127.0.0.1.

#include "config/ActiveSettings.h"
#include "JNIUtils.h"

#include <fstream>
#include <string>

namespace
{
	constexpr const char* kFileName = "mh3u_server.txt";
	constexpr size_t kMaxHostLength = 15;

	std::string Trim(const std::string& in)
	{
		const size_t a = in.find_first_not_of(" \t\r\n");
		if (a == std::string::npos)
			return {};
		const size_t b = in.find_last_not_of(" \t\r\n");
		return in.substr(a, b - a + 1);
	}

	// Splits "<host>" or "<host>:<port>" and returns the host part only.
	std::string HostPart(const std::string& endpoint)
	{
		const size_t colon = endpoint.rfind(':');
		if (colon == std::string::npos || colon + 1 >= endpoint.size())
			return endpoint;
		if (endpoint.find_first_not_of("0123456789", colon + 1) != std::string::npos)
			return endpoint;
		return endpoint.substr(0, colon);
	}
} // namespace

// Returns "<host>" or "<host>:<port>" as stored, or an empty string when unset. Mirrors the parsing
// in napi_act.cpp: first non-blank, non-comment line wins.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeMH3U_getServerEndpoint(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	std::ifstream f(ActiveSettings::GetConfigPath(kFileName));
	std::string line;
	while (f.is_open() && std::getline(f, line))
	{
		line = Trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		return JNIUtils::ToJString(env, line);
	}
	return JNIUtils::ToJString(env, std::string());
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeMH3U_setServerEndpoint(JNIEnv* env, [[maybe_unused]] jclass clazz, jstring endpoint)
{
	const std::string value = Trim(JNIUtils::FromJString(env, endpoint));
	const auto path = ActiveSettings::GetConfigPath(kFileName);
	std::error_code ec;
	if (value.empty())
	{
		std::filesystem::remove(path, ec);
		return;
	}
	std::ofstream out(path, std::ios::trunc);
	if (!out.is_open())
		return;
	out << "# Written by Cemu Android. Host must be 15 characters or fewer (Wii U nexToken limit).\n";
	out << value << "\n";
}

// True when the endpoint is something the emulator core will actually accept.
extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeMH3U_isValidEndpoint(JNIEnv* env, [[maybe_unused]] jclass clazz, jstring endpoint)
{
	const std::string value = Trim(JNIUtils::FromJString(env, endpoint));
	if (value.empty())
		return JNI_TRUE; // empty means "use the 127.0.0.1 default"
	const std::string host = HostPart(value);
	if (host.empty() || host.size() > kMaxHostLength)
		return JNI_FALSE;
	if (host.find_first_of(" \t/\\") != std::string::npos)
		return JNI_FALSE;
	return JNI_TRUE;
}

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeMH3U_getMaxHostLength([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return (jint)kMaxHostLength;
}
