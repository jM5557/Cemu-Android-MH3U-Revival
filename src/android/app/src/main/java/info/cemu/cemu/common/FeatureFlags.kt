package info.cemu.cemu.common

/** Features that are built in but switched off in this build. */
object FeatureFlags {
    /**
     * Save states (side menu, quick save/load hotkeys). Off until loading a state stops crashing on
     * Android: a load restores guest memory and the files but the app dies right after, and the
     * crash itself needs a logcat backtrace to find. The core and the UI are kept, so turning this
     * back on is all that re-enabling them takes.
     */
    const val SAVE_STATES = false
}
