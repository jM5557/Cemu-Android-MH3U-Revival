package info.cemu.cemu.nativeinterface

/**
 * Per-title cheat lists, stored in <UserData>/cheats/<titleId>.txt in the Citra/Azahar layout.
 * The emulator core applies enabled cheats once per frame while the title runs.
 */
object NativeCheats {
    /**
     * Cheats of [titleId] as a flat array, four entries per cheat:
     * name, notes, "1"/"0" for enabled, and the code lines joined with '\n'.
     */
    @JvmStatic
    external fun loadCheats(titleId: Long): Array<String>

    /**
     * Writes the cheat file (same flat layout as [loadCheats]). If the title is running the list
     * applies on the next frame. Returns an empty string on success, otherwise the error.
     */
    @JvmStatic
    external fun saveCheats(titleId: Long, flatCheats: Array<String>): String

    /** Empty string if [code] parses; otherwise the offending line and the reason. */
    @JvmStatic
    external fun validateCode(code: String): String

    @JvmStatic
    external fun getCheatFilePath(titleId: Long): String

    /** Creates the cheats folder if needed and returns its path. */
    @JvmStatic
    external fun ensureCheatFolder(): String

    /** Title ID of the running game, or 0 when nothing is running. */
    @JvmStatic
    external fun getRunningTitleId(): Long

    /** Re-reads the cheat file from disk and applies it to the running game. */
    @JvmStatic
    external fun reloadFromFile(titleId: Long)
}
