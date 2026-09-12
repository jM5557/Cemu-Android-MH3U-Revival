#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "config/ActiveSettings.h"
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

// Texture dumping is a Debug-menu item on desktop and was never wired up on Android. It is needed
// here to produce dump/textures/rename_map.csv, which migrates a pack to the current hash scheme.
// The flag lives in ActiveSettings and is not persisted, so it resets on restart - deliberate,
// since dumping writes a TGA per texture and is not something to leave on by accident.
extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_isDumpingTextures([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return ActiveSettings::DumpTexturesEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_setDumpingTextures([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jboolean enabled)
{
	ActiveSettings::EnableDumpTextures(enabled == JNI_TRUE);
}

// <UserData>/dump/textures - where the TGAs and rename_map.csv land.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCustomTextures_getDumpFolder(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return JNIUtils::ToJString(env, ActiveSettings::GetUserDataPath("dump/textures").string());
}
