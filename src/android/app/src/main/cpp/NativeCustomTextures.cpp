#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "config/ActiveSettings.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/CafeSystem.h"
#include "JNIUtils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
	jobjectArray ToJStringArray(JNIEnv* env, const std::vector<std::string>& values)
	{
		jclass stringClass = env->FindClass("java/lang/String");
		jobjectArray array = env->NewObjectArray((jsize)values.size(), stringClass, nullptr);
		for (jsize i = 0; i < (jsize)values.size(); i++)
		{
			jstring str = JNIUtils::ToJString(env, values[i]);
			env->SetObjectArrayElement(array, i, str);
			env->DeleteLocalRef(str);
		}
		env->DeleteLocalRef(stringClass);
		return array;
	}

	std::vector<std::string> FromJStringArray(JNIEnv* env, jobjectArray array)
	{
		std::vector<std::string> values;
		if (!array)
			return values;
		const jsize count = env->GetArrayLength(array);
		values.reserve(count);
		for (jsize i = 0; i < count; i++)
		{
			auto str = (jstring)env->GetObjectArrayElement(array, i);
			values.emplace_back(JNIUtils::FromJString(env, str));
			env->DeleteLocalRef(str);
		}
		return values;
	}
} // namespace

extern "C" [[maybe_unused]] JNIEXPORT jobjectArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_listPacks(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	return ToJStringArray(env, LatteTextureReplace::ListPacks((uint64_t)titleId));
}

extern "C" [[maybe_unused]] JNIEXPORT jobjectArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_findPackConflicts(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId, jobjectArray packs)
{
	return ToJStringArray(env, LatteTextureReplace::FindPackConflicts((uint64_t)titleId, FromJStringArray(env, packs)));
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_setTitleSettings(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId, jboolean enabled, jobjectArray packs)
{
	LatteTextureReplace::SetTitleSettings((uint64_t)titleId, enabled == JNI_TRUE, FromJStringArray(env, packs));
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_clearTitleSettings([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	LatteTextureReplace::ClearTitleSettings((uint64_t)titleId);
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getTitleFolder(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	return JNIUtils::ToJString(env, LatteTextureReplace::GetTitleFolder((uint64_t)titleId).string());
}

// Creates <UserData>/load/textures/<titleId>/ so the folder shows up in a file manager before the
// user has put anything in it. Harmless if it already exists.
extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_ensureTitleFolder(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	std::error_code ec;
	std::filesystem::create_directories(LatteTextureReplace::GetTitleFolder((uint64_t)titleId), ec);
	return ec ? JNI_FALSE : JNI_TRUE;
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_isGloballyEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return ActiveSettings::LoadCustomTexturesEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_setGloballyEnabled([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean enabled)
{
	ActiveSettings::EnableLoadCustomTextures(enabled == JNI_TRUE);
}

// Safe to call from the UI thread while a game is running: the work is queued and executed on the
// GPU thread. A no-op when no title is running.
extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_reloadTextures([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	if (!CafeSystem::IsTitleRunning())
		return;
	LatteAsyncCommands_queueReloadTextures();
}

// Texture dumping is a Debug-menu item on desktop. The Android app persists the switch itself and
// pushes it back in at startup, because the process exits every time a game is closed.
extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_isDumpingTextures([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return ActiveSettings::DumpTexturesEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_setDumpingTextures([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean enabled)
{
	const bool wasEnabled = ActiveSettings::DumpTexturesEnabled();
	ActiveSettings::EnableDumpTextures(enabled == JNI_TRUE);
	if (enabled != JNI_TRUE || wasEnabled)
		return;
	std::error_code ec;
	const auto dumpDir = ActiveSettings::GetUserDataPath("dump/textures");
	std::filesystem::create_directories(dumpDir, ec);
	cemuLog_log(LogType::Force, "[TextureDump] enabled, writing to {} (folder ok: {})",
		dumpDir.string(), std::filesystem::is_directory(dumpDir, ec));
	// Dumps are written when a texture is loaded, and textures already in the cache are never
	// loaded again. Flush the cache so what is on screen right now gets written too.
	if (CafeSystem::IsTitleRunning())
		LatteAsyncCommands_queueReloadTextures();
}

// <UserData>/dump/textures
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getDumpFolder(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return JNIUtils::ToJString(env, ActiveSettings::GetUserDataPath("dump/textures").string());
}

// [written, failed] since the app started (or since the folder was last cleared).
extern "C" [[maybe_unused]] JNIEXPORT jintArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getDumpCounts(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	const auto stats = LatteTextureReplace::GetDumpStats();
	const jint values[2] = {(jint)stats.written, (jint)stats.failed};
	jintArray result = env->NewIntArray(2);
	env->SetIntArrayRegion(result, 0, 2, values);
	return result;
}

// Why the most recent failed write failed, or an empty string.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getDumpLastError(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return JNIUtils::ToJString(env, LatteTextureReplace::GetDumpStats().lastError);
}

// Number of files sitting in dump/textures.
extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getDumpFileCount([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	std::error_code ec;
	const auto dir = ActiveSettings::GetUserDataPath("dump/textures");
	if (!std::filesystem::is_directory(dir, ec))
		return 0;
	jint count = 0;
	for (auto& entry : std::filesystem::directory_iterator(dir, ec))
		if (entry.is_regular_file(ec))
			count++;
	return count;
}

// Deletes every file in dump/textures and returns how many went. Subdirectories are left alone.
extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_clearDumpFolder([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	std::error_code ec;
	const auto dir = ActiveSettings::GetUserDataPath("dump/textures");
	jint removed = 0;
	if (std::filesystem::is_directory(dir, ec))
	{
		for (auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file(ec))
				continue;
			std::error_code removeEc;
			if (std::filesystem::remove(entry.path(), removeEc))
				removed++;
		}
	}
	// Each file is only written once per session, so forget which ones were written, then flush
	// the texture cache so everything on screen is loaded, and dumped, again.
	LatteTextureReplace::ResetDumpSession();
	if (CafeSystem::IsTitleRunning() && ActiveSettings::DumpTexturesEnabled())
		LatteAsyncCommands_queueReloadTextures();
	return removed;
}
