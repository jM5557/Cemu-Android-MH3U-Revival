package info.cemu.cemu.games.customtextures

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.nativeinterface.NativeCustomTextures
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class CustomTexturesUiState(
    val loading: Boolean = true,
    val enabled: Boolean = true,
    val availablePacks: List<String> = emptyList(),
    /** Packs the user has ticked. Empty with [neverConfigured] means "all of them". */
    val selectedPacks: Set<String> = emptySet(),
    val neverConfigured: Boolean = true,
    val conflicts: List<String> = emptyList(),
    val checkingConflicts: Boolean = false,
    val folder: String = "",
)

class CustomTexturesViewModel(private val titleId: Long) : ViewModel() {
    private val _uiState = MutableStateFlow(CustomTexturesUiState())
    val uiState: StateFlow<CustomTexturesUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            NativeCustomTextures.ensureTitleFolder(titleId)
            val folder = NativeCustomTextures.getTitleFolder(titleId)
            val packs = withContext(Dispatchers.IO) {
                NativeCustomTextures.listPacks(titleId).toList().sorted()
            }
            val stored = CustomTexturesRepository.titleSettingsFlow(titleId).first()
            val configured = CustomTexturesRepository.settingsFlow.first()
                .titles.containsKey("%016x".format(titleId))
            _uiState.value = CustomTexturesUiState(
                loading = false,
                enabled = stored.enabled,
                availablePacks = packs,
                // An unconfigured title means "all packs", which is also what the core assumes.
                selectedPacks = if (configured) stored.packs.toSet() else packs.toSet(),
                neverConfigured = !configured,
                folder = folder,
            )
            refreshConflicts()
        }
    }

    fun setEnabled(enabled: Boolean) {
        _uiState.value = _uiState.value.copy(enabled = enabled)
        persist()
    }

    fun setPackSelected(pack: String, selected: Boolean) {
        val packs = _uiState.value.selectedPacks.toMutableSet()
        if (selected) packs.add(pack) else packs.remove(pack)
        _uiState.value = _uiState.value.copy(selectedPacks = packs, neverConfigured = false)
        persist()
        refreshConflicts()
    }

    fun resetToDefaults() {
        viewModelScope.launch {
            CustomTexturesRepository.clearTitleSettings(titleId)
            val packs = _uiState.value.availablePacks
            _uiState.value = _uiState.value.copy(
                enabled = true,
                selectedPacks = packs.toSet(),
                neverConfigured = true,
            )
            refreshConflicts()
        }
    }

    /** Applies the current selection to a running game without restarting it. */
    fun reloadNow() = NativeCustomTextures.reloadTextures()

    private fun persist() {
        val state = _uiState.value
        viewModelScope.launch {
            CustomTexturesRepository.setTitleSettings(
                titleId,
                state.enabled,
                state.selectedPacks.toList().sorted(),
            )
        }
    }

    private fun refreshConflicts() {
        val packs = _uiState.value.selectedPacks.toList().sorted()
        if (packs.size < 2) {
            _uiState.value = _uiState.value.copy(conflicts = emptyList(), checkingConflicts = false)
            return
        }
        _uiState.value = _uiState.value.copy(checkingConflicts = true)
        viewModelScope.launch {
            // Walks every pack folder on disk; must not run on the main thread.
            val conflicts = withContext(Dispatchers.IO) {
                NativeCustomTextures.findPackConflicts(titleId, packs.toTypedArray()).toList()
            }
            _uiState.value = _uiState.value.copy(conflicts = conflicts, checkingConflicts = false)
        }
    }

    companion object {
        val TITLE_ID_KEY = object : CreationExtras.Key<Long> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                CustomTexturesViewModel(this[TITLE_ID_KEY] as Long)
            }
        }
    }
}
