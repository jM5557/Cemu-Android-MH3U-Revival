package info.cemu.cemu.emulation

import android.view.SurfaceHolder
import androidx.datastore.core.DataStore
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import info.cemu.cemu.common.either.Either
import info.cemu.cemu.common.either.Error
import info.cemu.cemu.common.either.Success
import info.cemu.cemu.common.either.attemptWithContext
import info.cemu.cemu.common.either.bind
import info.cemu.cemu.common.either.mapError
import info.cemu.cemu.common.settings.AppSettings
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.settings.InputOverlayRect
import info.cemu.cemu.common.settings.InputOverlaySettings
import info.cemu.cemu.common.settings.OverlayInputConfig
import info.cemu.cemu.nativeinterface.NativeCheats
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeEmulation.PrepareTitleResult
import info.cemu.cemu.nativeinterface.NativeException
import info.cemu.cemu.nativeinterface.NativeSaveStates
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class SideMenuState(
    val isMotionEnabled: Boolean = false,
    val isTVReplacedWithPad: Boolean = false,
    val isPadVisible: Boolean = false,
    val isInputOverlayVisible: Boolean = false,
    val isEmulationPaused: Boolean = false,
)

enum class SaveStateAction { SAVE, LOAD, UNDO }

/** What is running: slot is null for an undo. */
data class SaveStateBusy(val action: SaveStateAction, val slot: Int?)

sealed interface SaveStateEvent {
    val operation: SaveStateBusy

    data class Succeeded(override val operation: SaveStateBusy) : SaveStateEvent
    data class Failed(override val operation: SaveStateBusy, val message: String) : SaveStateEvent
}

data class SaveStateSlot(
    val slot: Int,
    val exists: Boolean,
    val savedAtSeconds: Long,
    /** Why the state cannot be loaded into the running game; empty when it can. */
    val incompatibility: String,
)

class ConditionFlags(
    var isMainConditionMet: Boolean = false, var isPadConditionMet: Boolean = false
) {
    fun get(isMain: Boolean): Boolean {
        return if (isMain) isMainConditionMet else isPadConditionMet
    }

    fun set(isMain: Boolean, value: Boolean) {
        if (isMain) {
            isMainConditionMet = value
        } else {
            isPadConditionMet = value
        }
    }
}

sealed interface NativeError {
    data class SurfaceCreationError(val message: String) : NativeError
    data class RendererInitializationError(val message: String) : NativeError

    object GameFilesNotFoundError : NativeError
    object NoDiscKeysError : NativeError
    object NoTitleTikError : NativeError
    data class UnknownTilePrepareError(val launchPath: String) : NativeError
    data class SystemInitializationError(val message: String) : NativeError

    object LaunchingTitleError : NativeError
}

class EmulationViewModel(
    private val launchPath: String,
    private val dataStore: DataStore<AppSettings> = AppSettingsStore.dataStore
) : ViewModel() {
    private val _emulationError = MutableStateFlow<NativeError?>(null)
    val emulationError = _emulationError.asStateFlow()

    private val _sideMenuState = MutableStateFlow(SideMenuState())
    val sideMenuState = _sideMenuState.asStateFlow()

    val isInputOverlayVisible =
        sideMenuState.map { it.isInputOverlayVisible }
            .stateIn(
                viewModelScope,
                SharingStarted.WhileSubscribed(5000),
                false,
            )

    init {
        viewModelScope.launch {
            val settings = dataStore.data.first()
            _sideMenuState.update { it.copy(isInputOverlayVisible = settings.inputOverlaySettings.isOverlayEnabled) }
        }
    }

    val inputOverlaySettings = dataStore.data.map { it.inputOverlaySettings }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        InputOverlaySettings(),
    )

    fun saveInputOverlayRectangles(inputOverlayRectMap: Map<OverlayInputConfig, InputOverlayRect>) {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = inputOverlayRectMap)

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    fun resetInputOverlayLayout() {
        viewModelScope.launch {
            dataStore.updateData {
                val overlaySettings =
                    it.inputOverlaySettings.copy(inputOverlayRectMap = emptyMap())

                it.copy(inputOverlaySettings = overlaySettings)
            }
        }
    }

    // Eagerly, so the value is current when the app goes to the background
    val pauseOnFocusLoss = dataStore.data.map { it.emulationSettings.pauseOnFocusLoss }
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    fun setPauseOnFocusLoss(enabled: Boolean) {
        viewModelScope.launch {
            dataStore.updateData {
                it.copy(emulationSettings = it.emulationSettings.copy(pauseOnFocusLoss = enabled))
            }
        }
    }

    /**
     * The app went to the background (another app, home, or the screen was locked). With
     * "Pause on focus loss / standby" on, the game is paused and stays paused when you come back.
     */
    fun onFocusLost() {
        if (!pauseOnFocusLoss.value || _sideMenuState.value.isEmulationPaused) return
        if (NativeCheats.getRunningTitleId() == 0L) return // still booting, nothing to pause
        updateSideMenuState(_sideMenuState.value.copy(isEmulationPaused = true))
    }

    fun updateSideMenuState(sideMenuState: SideMenuState) {
        val wasPaused = _sideMenuState.value.isEmulationPaused
        _sideMenuState.value = sideMenuState
        if (sideMenuState.isEmulationPaused != wasPaused) {
            setEmulationPaused(sideMenuState.isEmulationPaused)
        }
    }

    /**
     * "Pause emulation" from the side menu. The game stays paused until it is switched off again,
     * including across the screen turning off and on (which pauses and resumes by itself).
     */
    private fun setEmulationPaused(paused: Boolean) {
        if (paused) {
            NativeEmulation.pauseTitle()
        } else if (!destroyedSurfaces.get(isMain = true)) {
            // with the screen off, it resumes once the surface is back (surfaceChanged)
            NativeEmulation.resumeTitle()
        }
    }

    private val _saveStateBusy = MutableStateFlow<SaveStateBusy?>(null)

    /** Set while a save, load or undo runs; the game is stopped for part of that time. */
    val saveStateBusy = _saveStateBusy.asStateFlow()

    private val _saveStateEvents = MutableSharedFlow<SaveStateEvent>(extraBufferCapacity = 4)
    val saveStateEvents = _saveStateEvents.asSharedFlow()

    private val _saveStateRevision = MutableStateFlow(0)

    /** Changes whenever the slots may have changed, so a slot list can read them again. */
    val saveStateRevision = _saveStateRevision.asStateFlow()

    fun saveState(slot: Int) =
        runSaveStateOperation(SaveStateBusy(SaveStateAction.SAVE, slot)) { NativeSaveStates.saveToSlot(slot) }

    fun loadState(slot: Int) =
        runSaveStateOperation(SaveStateBusy(SaveStateAction.LOAD, slot)) { NativeSaveStates.loadFromSlot(slot) }

    fun undoLoadState() =
        runSaveStateOperation(SaveStateBusy(SaveStateAction.UNDO, null)) { NativeSaveStates.undoLoadState() }

    fun deleteSaveState(slot: Int) {
        if (_saveStateBusy.value != null) return
        viewModelScope.launch {
            withContext(Dispatchers.IO) { NativeSaveStates.deleteSlot(slot) }
            _saveStateRevision.update { it + 1 }
        }
    }

    suspend fun querySaveStateSlots(): List<SaveStateSlot> = withContext(Dispatchers.IO) {
        (0 until NativeSaveStates.getSlotCount()).map { slot ->
            val fields = NativeSaveStates.querySlot(slot)
            SaveStateSlot(
                slot = slot,
                exists = fields?.getOrNull(0) == "1",
                savedAtSeconds = fields?.getOrNull(1)?.toLongOrNull() ?: 0L,
                incompatibility = fields?.getOrNull(2).orEmpty(),
            )
        }
    }

    suspend fun hasUndoState(): Boolean = withContext(Dispatchers.IO) { NativeSaveStates.hasUndoState() }

    /**
     * One operation at a time; a second request while one runs is ignored, the same as a second
     * tap on a button that is already working.
     */
    private fun runSaveStateOperation(busy: SaveStateBusy, operation: () -> String?) {
        if (_saveStateBusy.value != null) return
        _saveStateBusy.value = busy
        viewModelScope.launch {
            val error = try {
                withContext(Dispatchers.IO) { operation() }
            } catch (exception: CancellationException) {
                throw exception
            } catch (exception: Exception) {
                exception.message ?: exception.toString()
            }
            _saveStateBusy.value = null
            _saveStateRevision.update { it + 1 }
            if (error == null && busy.action != SaveStateAction.SAVE && _sideMenuState.value.isEmulationPaused) {
                // A load always leaves the game running (while paused the screen would stay black,
                // since the load drops the GPU caches that held the last frame). Native code has
                // already resumed it, so only the menu's checkbox needs to follow.
                _sideMenuState.update { it.copy(isEmulationPaused = false) }
            }
            _saveStateEvents.emit(
                if (error == null) SaveStateEvent.Succeeded(busy) else SaveStateEvent.Failed(busy, error)
            )
        }
    }

    val gamePadPosition = dataStore.data.map { it.emulationSettings.gamePadPosition }
        .stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(5000),
            null,
        )

    val destroyedSurfaces = ConditionFlags()
    var setSurfaces = ConditionFlags()

    private inner class CanvasSurfaceHolderCallback(val isMainCanvas: Boolean) :
        SurfaceHolder.Callback {

        override fun surfaceCreated(surfaceHolder: SurfaceHolder) {}

        override fun surfaceChanged(
            surfaceHolder: SurfaceHolder,
            format: Int,
            width: Int,
            height: Int,
        ) {
            try {
                NativeEmulation.setSurfaceSize(width, height, isMainCanvas)

                if (setSurfaces.get(isMainCanvas)) {
                    return
                }

                NativeEmulation.setSurface(surfaceHolder.surface, isMainCanvas)
                val mainSurfaceWasDestroyed = destroyedSurfaces.get(isMain = true)

                if (mainSurfaceWasDestroyed && isMainCanvas && !_sideMenuState.value.isEmulationPaused) {
                    NativeEmulation.resumeTitle()
                }

                setSurfaces.set(isMainCanvas, true)

                val padSurfaceWasSet = setSurfaces.get(isMain = false)
                if ((!isMainCanvas && !mainSurfaceWasDestroyed) || (isMainCanvas && padSurfaceWasSet)) {
                    NativeEmulation.initializeSurface(isMainCanvas = false)
                }

                destroyedSurfaces.set(isMainCanvas, false)
            } catch (exception: NativeException) {
                _emulationError.value = NativeError.SurfaceCreationError(exception.message!!)
            }
        }

        override fun surfaceDestroyed(surfaceHolder: SurfaceHolder) {
            if (setSurfaces.get(isMain = false)) {
                NativeEmulation.clearPadSurface()
                setSurfaces.set(isMain = false, false)
                destroyedSurfaces.set(isMain = false, true)
            }

            if (isMainCanvas) {
                NativeEmulation.pauseTitle()

                setSurfaces.set(isMain = true, false)
                destroyedSurfaces.set(isMain = true, true)
            }
        }
    }

    val mainHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(true)
    val padHolderCallback: SurfaceHolder.Callback = CanvasSurfaceHolderCallback(false)

    private suspend fun initializeSystems() = attemptWithContext(Dispatchers.IO) {
        NativeEmulation.initializeSystems()
    }.mapError { NativeError.SystemInitializationError(it) }

    private suspend fun initializeRenderer() = attemptWithContext(Dispatchers.IO) {
        NativeEmulation.initializeRenderer()
        NativeEmulation.initializeSurface(isMainCanvas = true)
    }.mapError { NativeError.RendererInitializationError(it) }

    private suspend fun prepareTitle(): Either<Unit, NativeError> =
        attemptWithContext(Dispatchers.IO) { NativeEmulation.prepareTitle(launchPath) }
            .fold(
                onSuccess = { result ->
                    when (result) {
                        PrepareTitleResult.SUCCESSFUL -> Success(Unit)
                        PrepareTitleResult.ERROR_GAME_BASE_FILES_NOT_FOUND -> Error(NativeError.GameFilesNotFoundError)
                        PrepareTitleResult.ERROR_NO_DISC_KEY -> Error(NativeError.NoDiscKeysError)
                        PrepareTitleResult.ERROR_NO_TITLE_TIK -> Error(NativeError.NoTitleTikError)
                        else -> Error(NativeError.UnknownTilePrepareError(launchPath))
                    }
                },
                onError = { Error(NativeError.UnknownTilePrepareError(launchPath)) }
            )

    private suspend fun launchTitle() =
        attemptWithContext(Dispatchers.IO) { NativeEmulation.launchTitle() }
            .mapError { NativeError.LaunchingTitleError }

    private val _isEmulationInitialized = MutableStateFlow(false)
    val isEmulationInitialized = _isEmulationInitialized.asStateFlow()
    private var emulationInitializationJob: Job? = null
    fun initializeEmulation() {
        if (_isEmulationInitialized.value || emulationInitializationJob != null) {
            return
        }

        emulationInitializationJob = viewModelScope.launch {
            prepareTitle()
                .bind { initializeSystems() }
                .bind { initializeRenderer() }
                .bind { launchTitle() }
                .onError { _emulationError.value = it }

            _isEmulationInitialized.value = true
        }
    }

    companion object {
        val LAUNCH_PATH_KEY = object : CreationExtras.Key<String> {}
        val Factory: ViewModelProvider.Factory = viewModelFactory {
            initializer {
                EmulationViewModel(
                    this[LAUNCH_PATH_KEY] as String
                )
            }
        }
    }
}