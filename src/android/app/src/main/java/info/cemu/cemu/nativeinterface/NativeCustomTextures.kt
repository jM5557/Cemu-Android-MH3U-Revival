package info.cemu.cemu.nativeinterface

object NativeCustomTextures {
    /** Pack folder names directly inside load/textures/<titleId>/. Cheap; safe on the UI thread. */
    @JvmStatic
    external fun listPacks(titleId: Long): Array<String>

    /**
     * One line per texture supplied by more than one of [packs]. Walks the pack folders, so call it
     * off the main thread.
     */
    @JvmStatic
    external fun findPackConflicts(titleId: Long, packs: Array<String>): Array<String>

    /** Pushes the per-title selection into the emulator core. Takes effect on the next reload. */
    @JvmStatic
    external fun setTitleSettings(titleId: Long, enabled: Boolean, packs: Array<String>)

    /** Reverts the title to the default of "enabled, every pack in its folder". */
    @JvmStatic
    external fun clearTitleSettings(titleId: Long)

    @JvmStatic
    external fun getTitleFolder(titleId: Long): String

    @JvmStatic
    external fun ensureTitleFolder(titleId: Long): Boolean

    @JvmStatic
    external fun isGloballyEnabled(): Boolean

    @JvmStatic
    external fun setGloballyEnabled(enabled: Boolean)

    /** Drops every cached host texture so replacements are picked up. No-op when nothing is running. */
    @JvmStatic
    external fun reloadTextures()

    /**
     * Writes each replaceable texture the game loads to dump/textures as a TGA named exactly as a
     * replacement for it must be named. Persisted by [CustomTexturesRepository], not by the core.
     */
    @JvmStatic
    external fun isDumpingTextures(): Boolean

    @JvmStatic
    external fun setDumpingTextures(enabled: Boolean)

    @JvmStatic
    external fun getDumpFolder(): String

    /** [written, failed] since the app started or the folder was last cleared. */
    @JvmStatic
    external fun getDumpCounts(): IntArray

    /** Why the most recent write failed, or an empty string. */
    @JvmStatic
    external fun getDumpLastError(): String

    @JvmStatic
    external fun getDumpFileCount(): Int

    /** Deletes every file in dump/textures and returns how many were removed. */
    @JvmStatic
    external fun clearDumpFolder(): Int
}
