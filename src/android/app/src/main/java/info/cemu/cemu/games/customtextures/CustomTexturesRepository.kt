package info.cemu.cemu.games.customtextures

import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.CustomTextureTitleSettings
import info.cemu.cemu.nativeinterface.NativeCustomTextures
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

/**
 * Owns the persisted custom texture selection and mirrors it into the emulator core.
 *
 * The core keeps its own in-memory copy (LatteTextureReplace::s_titleSettings) that is not persisted,
 * so [applyAll] has to run once per process before a title is launched. On desktop the equivalent
 * state lives in wxCemuConfig; here it lives in the app DataStore instead.
 */
object CustomTexturesRepository {
    private fun titleKey(titleId: Long): String = "%016x".format(titleId)

    val settingsFlow = AppSettingsStore.dataStore.data.map { it.customTextureSettings }

    fun titleSettingsFlow(titleId: Long) =
        settingsFlow.map { it.titles[titleKey(titleId)] ?: CustomTextureTitleSettings() }

    /** Call once after ActiveSettings has been initialised, before any title starts. */
    suspend fun applyAll() {
        val settings = settingsFlow.first()
        NativeCustomTextures.setGloballyEnabled(settings.globallyEnabled)
        NativeCustomTextures.setDumpingTextures(settings.dumpTextures)
        settings.titles.forEach { (key, titleSettings) ->
            val titleId = key.toULongOrNull(16)?.toLong() ?: return@forEach
            NativeCustomTextures.setTitleSettings(
                titleId,
                titleSettings.enabled,
                titleSettings.packs.toTypedArray(),
            )
        }
    }

    suspend fun setTitleSettings(titleId: Long, enabled: Boolean, packs: List<String>) {
        AppSettingsStore.dataStore.updateData { appSettings ->
            val titles = appSettings.customTextureSettings.titles.toMutableMap()
            titles[titleKey(titleId)] = CustomTextureTitleSettings(enabled, packs)
            appSettings.copy(
                customTextureSettings = appSettings.customTextureSettings.copy(titles = titles)
            )
        }
        NativeCustomTextures.setTitleSettings(titleId, enabled, packs.toTypedArray())
    }

    suspend fun clearTitleSettings(titleId: Long) {
        AppSettingsStore.dataStore.updateData { appSettings ->
            val titles = appSettings.customTextureSettings.titles.toMutableMap()
            titles.remove(titleKey(titleId))
            appSettings.copy(
                customTextureSettings = appSettings.customTextureSettings.copy(titles = titles)
            )
        }
        NativeCustomTextures.clearTitleSettings(titleId)
    }

    suspend fun setGloballyEnabled(enabled: Boolean) {
        AppSettingsStore.dataStore.updateData { appSettings ->
            appSettings.copy(
                customTextureSettings = appSettings.customTextureSettings.copy(globallyEnabled = enabled)
            )
        }
        NativeCustomTextures.setGloballyEnabled(enabled)
    }

    suspend fun setDumpingTextures(enabled: Boolean) {
        AppSettingsStore.dataStore.updateData { appSettings ->
            appSettings.copy(
                customTextureSettings = appSettings.customTextureSettings.copy(dumpTextures = enabled)
            )
        }
        NativeCustomTextures.setDumpingTextures(enabled)
    }
}
