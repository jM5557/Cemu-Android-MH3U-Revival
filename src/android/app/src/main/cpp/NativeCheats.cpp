#include "Cafe/Cheats/CheatManager.h"
#include "Cafe/CafeSystem.h"
#include "JNIUtils.h"

#include <filesystem>
#include <string>
#include <vector>

// Cheats are passed to and from Kotlin as a flat string array, four entries per cheat:
//   [name, notes, "1"/"0" enabled, code lines joined with '\n']
// That keeps the file format and parsing in one place (CheatManager) and the JNI surface small.

namespace
{
	constexpr size_t kFieldsPerCheat = 4;

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

	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		std::string line;
		for (char c : text)
		{
			if (c == '\n')
			{
				lines.push_back(line);
				line.clear();
			}
			else if (c != '\r')
				line.push_back(c);
		}
		if (!line.empty())
			lines.push_back(line);
		return lines;
	}

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string text;
		for (size_t i = 0; i < lines.size(); i++)
		{
			if (i)
				text += '\n';
			text += lines[i];
		}
		return text;
	}

	std::vector<std::string> Flatten(const std::vector<CheatManager::Cheat>& cheats)
	{
		std::vector<std::string> flat;
		flat.reserve(cheats.size() * kFieldsPerCheat);
		for (const auto& c : cheats)
		{
			flat.push_back(c.name);
			flat.push_back(c.notes);
			flat.push_back(c.enabled ? "1" : "0");
			flat.push_back(JoinLines(c.code));
		}
		return flat;
	}

	std::vector<CheatManager::Cheat> Unflatten(const std::vector<std::string>& flat)
	{
		std::vector<CheatManager::Cheat> cheats;
		for (size_t i = 0; i + kFieldsPerCheat <= flat.size(); i += kFieldsPerCheat)
		{
			CheatManager::Cheat c;
			c.name = flat[i];
			c.notes = flat[i + 1];
			c.enabled = flat[i + 2] == "1";
			for (auto& line : SplitLines(flat[i + 3]))
				if (line.find_first_not_of(" \t") != std::string::npos)
					c.code.push_back(line);
			cheats.push_back(std::move(c));
		}
		return cheats;
	}
} // namespace

extern "C" [[maybe_unused]] JNIEXPORT jobjectArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_loadCheats(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	return ToJStringArray(env, Flatten(CheatManager::Load((uint64_t)titleId)));
}

// Returns an empty string on success, otherwise the reason the file could not be written.
// When the title is running, the new list takes effect on the next frame.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_saveCheats(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId, jobjectArray flatCheats)
{
	std::string error;
	if (!CheatManager::Save((uint64_t)titleId, Unflatten(FromJStringArray(env, flatCheats)), error))
		return JNIUtils::ToJString(env, error.empty() ? std::string("Could not save cheats") : error);
	return JNIUtils::ToJString(env, std::string());
}

// Returns an empty string if the code parses, otherwise which line is wrong and why.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_validateCode(JNIEnv* env, [[maybe_unused]] jclass clazz, jstring code)
{
	CheatManager::Cheat cheat;
	cheat.code = SplitLines(JNIUtils::FromJString(env, code));
	std::string error;
	if (!CheatManager::Validate(cheat, error))
		return JNIUtils::ToJString(env, error);
	return JNIUtils::ToJString(env, std::string());
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_getCheatFilePath(JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	return JNIUtils::ToJString(env, _pathToUtf8(CheatManager::GetCheatFile((uint64_t)titleId)));
}

// Creates <UserData>/cheats/ so it can be found in a file manager before any cheat exists.
extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_ensureCheatFolder(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	const std::filesystem::path folder = CheatManager::GetCheatFolder();
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	return JNIUtils::ToJString(env, _pathToUtf8(folder));
}

// Title ID of the game currently running, or 0 when nothing is running.
extern "C" [[maybe_unused]] JNIEXPORT jlong JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_getRunningTitleId([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	if (!CafeSystem::IsTitleRunning())
		return 0;
	return (jlong)CafeSystem::GetForegroundTitleId();
}

// Re-reads the title's cheat file and applies it to the running game. For files edited by hand.
extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeCheats_reloadFromFile([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jlong titleId)
{
	CheatManager::Apply((uint64_t)titleId, CheatManager::Load((uint64_t)titleId));
}
