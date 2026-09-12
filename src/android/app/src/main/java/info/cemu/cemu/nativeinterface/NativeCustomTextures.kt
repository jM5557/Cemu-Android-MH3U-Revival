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

    /** Writes a TGA per texture plus rename_map.csv. Resets on app restart. */
    @JvmStatic
    external fun isDumpingTextures(): Boolean

    @JvmStatic
    external fun setDumpingTextures(enabled: Boolean)

    @JvmStatic
    external fun getDumpFolder(): String

    /** Records rename_map.csv only. Independent of texture dumping. */
    @JvmStatic
    external fun isScanningForMigration(): Boolean

    @JvmStatic
    external fun setScanningForMigration(enabled: Boolean)

    /** Full path of rename_map.csv, which sits beside the dumped TGAs. */
    @JvmStatic
    external fun getRenameMapPath(): String

    /** Lines in rename_map.csv, or -1 when the file does not exist yet. */
    @JvmStatic
    external fun getRenameMapEntryCount(): Int

    @JvmStatic
    external fun getDumpFileCount(): Int

    /** Deletes every file in dump/textures and returns how many were removed. */
    @JvmStatic
    external fun clearDumpFolder(): Int
}
