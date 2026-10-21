package info.cemu.cemu.common

/** Switches for features that can be turned off in a build without removing their code. */
object FeatureFlags {
    /**
     * Save states (side menu, quick save/load hotkeys). Set to false to hide them again without
     * touching anything else: the side menu entries and the two hotkeys disappear, a hotkey already
     * bound is ignored, and the save state core stays in place.
     */
    const val SAVE_STATES = true
}
