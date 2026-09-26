package info.cemu.cemu.common.cheats

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.nativeinterface.NativeCheats
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

data class Cheat(
    val name: String,
    val notes: String = "",
    val enabled: Boolean = false,
    /** Code lines joined with '\n', as typed. */
    val code: String = "",
)

data class CheatsUiState(
    val loading: Boolean = true,
    val cheats: List<Cheat> = emptyList(),
    val filePath: String = "",
    /** True when this title is the one running, so changes apply straight away. */
    val isRunning: Boolean = false,
    /** Last save or validation error, shown until the next successful change. */
    val error: String? = null,
)

/**
 * The cheat list of one title. The file on disk is the source of truth: every change is written
 * straight back, and the core applies it to the running game on the next frame.
 */
class CheatsViewModel(private val titleId: Long) : ViewModel() {
    private val _uiState = MutableStateFlow(CheatsUiState())
    val uiState: StateFlow<CheatsUiState> = _uiState.asStateFlow()

    init {
        reload()
    }

    fun reload() {
        viewModelScope.launch {
            val (cheats, path) = withContext(Dispatchers.IO) {
                NativeCheats.ensureCheatFolder()
                fromFlat(NativeCheats.loadCheats(titleId)) to NativeCheats.getCheatFilePath(titleId)
            }
            _uiState.value = CheatsUiState(
                loading = false,
                cheats = cheats,
                filePath = path,
                isRunning = NativeCheats.getRunningTitleId() == titleId,
            )
        }
    }

    /** Re-reads the file and pushes it to the running game; for files edited outside the app. */
    fun reloadFromDisk() {
        viewModelScope.launch {
            withContext(Dispatchers.IO) { NativeCheats.reloadFromFile(titleId) }
            reload()
        }
    }

    /** Returns an empty string if [code] parses, otherwise the offending line and why. */
    fun validate(code: String): String = NativeCheats.validateCode(code)

    fun setEnabled(index: Int, enabled: Boolean) {
        val cheats = _uiState.value.cheats
        val cheat = cheats.getOrNull(index) ?: return
        if (enabled) {
            val error = validate(cheat.code)
            if (error.isNotEmpty()) {
                _uiState.value = _uiState.value.copy(error = "${cheat.name}: $error")
                return
            }
        }
        save(cheats.toMutableList().also { it[index] = cheat.copy(enabled = enabled) })
    }

    /** Adds a new cheat when [index] is null, otherwise replaces the one at [index]. */
    fun upsert(index: Int?, cheat: Cheat) {
        val cheats = _uiState.value.cheats.toMutableList()
        if (index == null || index !in cheats.indices) cheats.add(cheat) else cheats[index] = cheat
        save(cheats)
    }

    fun delete(index: Int) {
        val cheats = _uiState.value.cheats.toMutableList()
        if (index !in cheats.indices) return
        cheats.removeAt(index)
        save(cheats)
    }

    fun clearError() {
        _uiState.value = _uiState.value.copy(error = null)
    }

    private val saveMutex = Mutex()

    private fun save(cheats: List<Cheat>) {
        // show the change immediately; the write below always stores the latest list, one at a time
        _uiState.value = _uiState.value.copy(cheats = cheats, error = null)
        viewModelScope.launch {
            val error = saveMutex.withLock {
                val latest = toFlat(_uiState.value.cheats)
                withContext(Dispatchers.IO) { NativeCheats.saveCheats(titleId, latest) }
            }
            if (error.isNotEmpty()) {
                // the file wasn't written, so show what is really on disk again
                val onDisk = withContext(Dispatchers.IO) { fromFlat(NativeCheats.loadCheats(titleId)) }
                _uiState.value = _uiState.value.copy(cheats = onDisk, error = error)
            }
        }
    }

    companion object {
        private const val FIELDS_PER_CHEAT = 4

        private fun fromFlat(flat: Array<String>): List<Cheat> =
            flat.toList().chunked(FIELDS_PER_CHEAT)
                .filter { it.size == FIELDS_PER_CHEAT }
                .map { (name, notes, enabled, code) -> Cheat(name, notes, enabled == "1", code) }

        private fun toFlat(cheats: List<Cheat>): Array<String> =
            cheats.flatMap { listOf(it.name, it.notes, if (it.enabled) "1" else "0", it.code) }
                .toTypedArray()

        val TITLE_ID_KEY = object : CreationExtras.Key<Long> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                CheatsViewModel(this[TITLE_ID_KEY] as Long)
            }
        }
    }
}
