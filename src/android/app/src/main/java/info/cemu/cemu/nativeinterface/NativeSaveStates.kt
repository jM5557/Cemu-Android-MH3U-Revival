package info.cemu.cemu.nativeinterface

/**
 * Save states of the running game. Slot files are shared with the desktop build
 * (`savestates/<titleId>/slot<N>.cst` in the user folder).
 *
 * [saveToSlot], [loadFromSlot] and [undoLoadState] stop the emulated world for up to a few
 * seconds: call them from a background dispatcher, never from the UI thread. They return null on
 * success, otherwise a message to show the user.
 */
object NativeSaveStates {
    @JvmStatic
    external fun getSlotCount(): Int

    @JvmStatic
    external fun saveToSlot(slot: Int): String?

    @JvmStatic
    external fun loadFromSlot(slot: Int): String?

    @JvmStatic
    external fun undoLoadState(): String?

    @JvmStatic
    external fun hasUndoState(): Boolean

    @JvmStatic
    external fun deleteSlot(slot: Int): Boolean

    /**
     * Three entries: "1"/"0" whether the slot holds a state, the unix time it was saved in seconds,
     * and why it cannot be loaded into the running game (empty when it can). Null when no game is
     * running.
     */
    @JvmStatic
    external fun querySlot(slot: Int): Array<String>?
}
