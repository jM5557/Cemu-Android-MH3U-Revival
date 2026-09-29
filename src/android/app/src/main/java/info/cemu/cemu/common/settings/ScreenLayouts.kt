package info.cemu.cemu.common.settings

import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeSettings
import info.cemu.cemu.nativeinterface.NativeSettings.FullscreenScaling
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

object ScreenLayouts {
    val all = listOf(
        FullscreenScaling.KEEP_ASPECT_RATIO,
        FullscreenScaling.STRETCH,
        FullscreenScaling.FILL,
        FullscreenScaling.ASPECT_16_9,
        FullscreenScaling.ASPECT_16_10,
        FullscreenScaling.ASPECT_4_3,
        FullscreenScaling.ASPECT_21_9,
        FullscreenScaling.INTEGER_SCALE,
    )

    private fun titleKey(titleId: Long): String = "%016x".format(titleId)

    /** The layout saved for [titleId], or null when it follows the Screen layout setting. */
    fun overrideFlow(titleId: Long) = AppSettingsStore.dataStore.data.map {
        it.screenLayoutOverrides[titleKey(titleId)]
    }

    /** Saves [layout] for [titleId] (null = follow the setting) and applies it straight away. */
    suspend fun setOverride(titleId: Long, layout: Int?) {
        AppSettingsStore.dataStore.updateData { appSettings ->
            val overrides = appSettings.screenLayoutOverrides.toMutableMap()
            if (layout == null) overrides.remove(titleKey(titleId)) else overrides[titleKey(titleId)] = layout
            appSettings.copy(screenLayoutOverrides = overrides)
        }
        NativeSettings.setScreenLayoutOverride(layout ?: -1)
    }

    /** Pushes the saved layout for [titleId] into the core. Call once the game is running. */
    suspend fun applyOverride(titleId: Long) {
        NativeSettings.setScreenLayoutOverride(overrideFlow(titleId).first() ?: -1)
    }
}

fun screenLayoutToString(layout: Int) = when (layout) {
    FullscreenScaling.KEEP_ASPECT_RATIO -> tr("Fit (keep aspect ratio)")
    FullscreenScaling.STRETCH -> tr("Stretch")
    FullscreenScaling.FILL -> tr("Fill (crop edges)")
    FullscreenScaling.ASPECT_16_9 -> tr("16:9")
    FullscreenScaling.ASPECT_16_10 -> tr("16:10")
    FullscreenScaling.ASPECT_4_3 -> tr("4:3")
    FullscreenScaling.ASPECT_21_9 -> tr("21:9")
    FullscreenScaling.INTEGER_SCALE -> tr("Integer scale (pixel-sharp)")
    else -> tr("Fit (keep aspect ratio)")
}
