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
    // the quick save slot, slot 0
    QUICK_SAVE_STATE,
    QUICK_LOAD_STATE,
}