package info.cemu.cemu.common.settings

import kotlinx.serialization.Serializable

@Serializable
data class HotkeyCombo(
    val keys: Set<Int>
)

enum class HotkeyAction {
    QUIT,
    TOGGLE_MENU,
    SHOW_EMULATED_USB_DEVICES_DIALOG,
    // the quick save slot, slot 0. Kept while save states are switched off (FeatureFlags), so a
    // binding saved by an earlier build still loads; they are hidden and ignored meanwhile.
    QUICK_SAVE_STATE,
    QUICK_LOAD_STATE;

    val isAvailable: Boolean
        get() = info.cemu.cemu.common.FeatureFlags.SAVE_STATES ||
                (this != QUICK_SAVE_STATE && this != QUICK_LOAD_STATE)
}