#include "Cafe/SaveState/SaveState.h"
#include "Cafe/CafeSystem.h"
#include "JNIUtils.h"

// Save states for the Android UI. Slot numbers and files are the same as on desktop
// (<user data>/savestates/<titleId>/slot<N>.cst), so a state can be moved between the two when
// both run the same build.
//
// Every call that captures or restores stops the emulated world and can take a few seconds, and
// none may run on a CPU core thread. Kotlin calls them from a background dispatcher, never from
// the UI thread. They return null on success, otherwise a message to show.

namespace
{
	// JNIUtils::ToJString has std::string and string_view overloads, so a literal is ambiguous
	jstring Message(JNIEnv* env, const char* text)
	{
		return JNIUtils::ToJString(env, std::string(text));
	}

	jstring ResultToJString(JNIEnv* env, const SaveStates::OperationResult& result)
	{
		if (result.success)
			return nullptr;
		return JNIUtils::ToJString(env, result.message.empty() ? std::string("Unknown error") : result.message);
	}

	bool IsSlotValid(jint slot)
	{
		return slot >= 0 && (uint32)slot < SaveStates::kSlotCount;
	}

	// Path of a slot of the running title, or the reason there is none.
	std::optional<fs::path> SlotPathOrError(JNIEnv* env, jint slot, jstring& error)
	{
		if (!CafeSystem::IsTitleRunning())
		{
			error = Message(env, "No game is running");
			return std::nullopt;
		}
		if (!IsSlotValid(slot))
		{
			error = Message(env, "Invalid save state slot");
			return std::nullopt;
		}
		fs::path path = SaveStates::GetSlotPath((uint32)slot);
		if (path.empty())
		{
			error = Message(env, "No game is running");
			return std::nullopt;
		}
		return path;
	}

	// Kept out of the JNI functions so an exception never crosses into Java.
	SaveStates::OperationResult Guarded(const std::function<SaveStates::OperationResult()>& fn)
	{
		try
		{
			return fn();
		}
		catch (const std::bad_alloc&)
		{
			return SaveStates::OperationResult::Fail("Not enough free memory");
		}
		catch (const std::exception& e)
		{
			return SaveStates::OperationResult::Fail(e.what());
		}
	}
} // namespace

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_getSlotCount([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return (jint)SaveStates::kSlotCount;
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_saveToSlot(JNIEnv* env, [[maybe_unused]] jclass clazz, jint slot)
{
	jstring error = nullptr;
	const auto path = SlotPathOrError(env, slot, error);
	if (!path)
		return error;
	return ResultToJString(env, Guarded([&] { return SaveStates::SaveToFile(*path); }));
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_loadFromSlot(JNIEnv* env, [[maybe_unused]] jclass clazz, jint slot)
{
	jstring error = nullptr;
	const auto path = SlotPathOrError(env, slot, error);
	if (!path)
		return error;
	std::error_code ec;
	if (!fs::exists(*path, ec))
		return Message(env, "This slot is empty");
	return ResultToJString(env, Guarded([&] { return SaveStates::LoadFromFile(*path); }));
}

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_undoLoadState(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	if (!CafeSystem::IsTitleRunning())
		return Message(env, "No game is running");
	return ResultToJString(env, Guarded([] { return SaveStates::UndoLoadState(); }));
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_hasUndoState([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	return CafeSystem::IsTitleRunning() && SaveStates::HasUndoState() ? JNI_TRUE : JNI_FALSE;
}

extern "C" [[maybe_unused]] JNIEXPORT jboolean JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_deleteSlot(JNIEnv* env, [[maybe_unused]] jclass clazz, jint slot)
{
	jstring error = nullptr;
	const auto path = SlotPathOrError(env, slot, error);
	if (!path)
	{
		if (error)
			env->DeleteLocalRef(error);
		return JNI_FALSE;
	}
	std::error_code ec;
	return fs::remove(*path, ec) && !ec ? JNI_TRUE : JNI_FALSE;
}

// Describes a slot without inflating it, as three strings:
//   [0] "1" if a state file is there, "0" if the slot is empty
//   [1] unix time the state was saved, in seconds ("0" when unknown)
//   [2] why it cannot be loaded into the running game, empty when it can
// null when no game is running.
extern "C" [[maybe_unused]] JNIEXPORT jobjectArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeSaveStates_querySlot(JNIEnv* env, [[maybe_unused]] jclass clazz, jint slot)
{
	if (!CafeSystem::IsTitleRunning() || !IsSlotValid(slot))
		return nullptr;
	SaveStates::SlotInfo info;
	try
	{
		info = SaveStates::QuerySlot((uint32)slot);
	}
	catch (const std::exception& e)
	{
		info.exists = true;
		info.incompatibility = e.what();
	}
	const std::string fields[3] = {
		info.exists ? "1" : "0",
		std::to_string(info.exists ? info.head.unixTimestamp : 0),
		info.incompatibility,
	};
	jclass stringClass = env->FindClass("java/lang/String");
	jobjectArray array = env->NewObjectArray(3, stringClass, nullptr);
	for (jsize i = 0; i < 3; i++)
	{
		jstring str = JNIUtils::ToJString(env, fields[i]);
		env->SetObjectArrayElement(array, i, str);
		env->DeleteLocalRef(str);
	}
	env->DeleteLocalRef(stringClass);
	return array;
}
