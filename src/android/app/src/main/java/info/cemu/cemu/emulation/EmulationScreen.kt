package info.cemu.cemu.emulation

import android.annotation.SuppressLint
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.RadioButton
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.viewmodel.MutableCreationExtras
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.R
import info.cemu.cemu.common.settings.GamePadPosition
import info.cemu.cemu.common.cheats.CheatsDialog
import info.cemu.cemu.common.settings.HotkeyAction
import info.cemu.cemu.common.settings.ScreenLayouts
import info.cemu.cemu.common.settings.screenLayoutToString
import info.cemu.cemu.common.ui.extensions.showMessage
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.emulation.emulatedusbdevices.EmulatedUSBDevicesDialog
import info.cemu.cemu.games.customtextures.CustomTexturesRepository
import info.cemu.cemu.emulation.input.HotkeyManager
import info.cemu.cemu.emulation.inputoverlay.InputOverlaySurface
import info.cemu.cemu.emulation.inputoverlay.InputOverlaySurfaceView
import info.cemu.cemu.emulation.inputoverlay.InputOverlaySurfaceView.InputMode.DEFAULT
import info.cemu.cemu.emulation.inputoverlay.InputOverlaySurfaceView.InputMode.EDIT_POSITION
import info.cemu.cemu.emulation.inputoverlay.InputOverlaySurfaceView.InputMode.EDIT_SIZE
import info.cemu.cemu.nativeinterface.NativeCheats
import info.cemu.cemu.nativeinterface.NativeCustomTextures
import info.cemu.cemu.nativeinterface.NativeEmulation
import info.cemu.cemu.nativeinterface.NativeSettings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun EmulationScreen(
    gamePath: String,
    setMotionSensorEnabled: (Boolean) -> Unit,
    setInputListeningEnabled: (Boolean) -> Unit,
    onQuit: () -> Unit,
    viewModel: EmulationViewModel = viewModel(
        factory = EmulationViewModel.Factory, extras = MutableCreationExtras().apply {
            set(EmulationViewModel.LAUNCH_PATH_KEY, gamePath)
        }),
) {
    val snackbarHostState = remember { SnackbarHostState() }
    val drawerState = rememberDrawerState(initialValue = DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var showQuitConfirmationDialog by remember { mutableStateOf(false) }
    var inputOverlayInputMode by rememberSaveable { mutableStateOf(DEFAULT) }
    var showEmulatedUSBDevices by remember { mutableStateOf(false) }
    var cheatsTitleId by remember { mutableStateOf<Long?>(null) }
    var showScreenLayoutDialog by remember { mutableStateOf(false) }
    // 0 until the game is up
    var runningTitleId by remember { mutableLongStateOf(0L) }

    LaunchedEffect(Unit) {
        while (runningTitleId == 0L) {
            runningTitleId = NativeCheats.getRunningTitleId()
            if (runningTitleId == 0L) delay(500)
        }
        // Per-game screen layout, so a layout picked for one game's widescreen mod does not
        // follow you into every other game.
        ScreenLayouts.applyOverride(runningTitleId)
    }

    val emulationError by viewModel.emulationError.collectAsState()
    val isEmulationInitialized by viewModel.isEmulationInitialized.collectAsState()
    val sideMenuState by viewModel.sideMenuState.collectAsState()
    val gamePadPosition by viewModel.gamePadPosition.collectAsState()
    val isInputOverlayVisible by viewModel.isInputOverlayVisible.collectAsState()
    val inputOverlaySettings by viewModel.inputOverlaySettings.collectAsState()


    fun closeDrawer() {
        scope.launch {
            drawerState.close()
        }
    }

    suspend fun toggleMenu() {
        drawerState.apply {
            if (isClosed) {
                open()
            } else {
                close()
            }
        }
    }

    BackHandler {
        if (drawerState.isAnimationRunning) {
            return@BackHandler
        }

        scope.launch {
            toggleMenu()
        }
    }

    LaunchedEffect(drawerState.isClosed) {
        setInputListeningEnabled(drawerState.isClosed)
    }

    LaunchedEffect(Unit) {
        HotkeyManager.actions.collect { action ->
            when (action) {
                HotkeyAction.QUIT -> showQuitConfirmationDialog = true
                HotkeyAction.TOGGLE_MENU -> toggleMenu()
                HotkeyAction.SHOW_EMULATED_USB_DEVICES_DIALOG -> showEmulatedUSBDevices = true
            }
        }
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        gesturesEnabled = drawerState.isOpen,
        drawerContent = {
            ModalDrawerSheet {
                Column(
                    modifier = Modifier
                        .padding(horizontal = 8.dp)
                        .width(IntrinsicSize.Max)
                        .verticalScroll(rememberScrollState())
                ) {
                    EmulationSideMenuContent(
                        sideMenuState = sideMenuState,
                        updateState = {
                            viewModel.updateSideMenuState(it)
                            setMotionSensorEnabled(it.isMotionEnabled)
                            NativeEmulation.setReplaceTVWithPadView(it.isTVReplacedWithPad)
                            closeDrawer()
                        },
                        onEditInputOverlay = {
                            snackbarHostState.showMessage(scope, tr("Edit input positions"))
                            inputOverlayInputMode = EDIT_POSITION
                            closeDrawer()
                        },
                        onResetInputOverlay = {
                            viewModel.resetInputOverlayLayout()
                            closeDrawer()
                        },
                        onQuit = {
                            showQuitConfirmationDialog = true
                            closeDrawer()
                        },
                        onShowEmulatedUSBDevices = {
                            showEmulatedUSBDevices = true
                            closeDrawer()
                        },
                        onShowScreenLayout = {
                            showScreenLayoutDialog = runningTitleId != 0L
                            closeDrawer()
                        },
                        onShowCheats = {
                            // 0 means nothing is running yet (still booting); there is no list to show
                            cheatsTitleId = NativeCheats.getRunningTitleId().takeIf { it != 0L }
                            closeDrawer()
                        },
                    )
                }
            }
        },
    ) {
        EmulationSurfaces(
            sideMenuState = sideMenuState,
            gamePadPosition = gamePadPosition,
            mainHolderCallback = viewModel.mainHolderCallback,
            padHolderCallback = viewModel.padHolderCallback,
            onInitializeEmulation = viewModel::initializeEmulation,
            isInputEnabled = drawerState.isClosed,
        )

        InputOverlaySurface(
            isVisible = isInputOverlayVisible,
            inputOverlaySettings = inputOverlaySettings,
            inputMode = inputOverlayInputMode,
            isInputEnabled = drawerState.isClosed,
            onEditFinished = { viewModel.saveInputOverlayRectangles(it) },
        )

        if (inputOverlayInputMode != DEFAULT) {
            EditInputsLayout(
                inputMode = inputOverlayInputMode,
                onFinishClick = {
                    snackbarHostState.showMessage(scope, tr("Exited input edit mode"))
                    inputOverlayInputMode = DEFAULT
                },
                onMoveClick = {
                    snackbarHostState.showMessage(scope, tr("Edit input positions"))
                    inputOverlayInputMode = EDIT_POSITION
                },
                onResizeClick = {
                    snackbarHostState.showMessage(scope, tr("Edit input size"))
                    inputOverlayInputMode = EDIT_SIZE
                },
            )
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(16.dp)
        )
    }

    emulationError?.let {
        EmulationErrorDialog(it, onQuit)
    }

    if (!isEmulationInitialized) {
        EmulationLoadingDialog()
    }

    if (showQuitConfirmationDialog) {
        EmulationQuitConfirmationDialog(
            onQuit = onQuit,
            onDismiss = { showQuitConfirmationDialog = false },
        )
    }

    if (showEmulatedUSBDevices) {
        EmulatedUSBDevicesDialog(
            onDismiss = { showEmulatedUSBDevices = false },
        )
    }

    if (showScreenLayoutDialog) {
        ScreenLayoutDialog(
            titleId = runningTitleId,
            onDismiss = { showScreenLayoutDialog = false },
        )
    }

    cheatsTitleId?.let { titleId ->
        CheatsDialog(
            titleId = titleId,
            onDismiss = { cheatsTitleId = null },
        )
    }

    EmulationTextInputDialog()
}

@Composable
private fun EditInputsLayout(
    inputMode: InputOverlaySurfaceView.InputMode,
    onFinishClick: () -> Unit,
    onMoveClick: () -> Unit,
    onResizeClick: () -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Button(onClick = onFinishClick) { Text(tr("Done")) }

            Row(
                modifier = Modifier
                    .wrapContentSize()
                    .padding(8.dp),
                horizontalArrangement = Arrangement.spacedBy(36.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                FilledIconButton(enabled = inputMode != EDIT_POSITION, onClick = onMoveClick) {
                    Icon(
                        painter = painterResource(id = R.drawable.ic_move),
                        contentDescription = tr("Move")
                    )
                }

                FilledIconButton(enabled = inputMode != EDIT_SIZE, onClick = onResizeClick) {
                    Icon(
                        painter = painterResource(id = R.drawable.ic_resize),
                        contentDescription = tr("Resize"),
                    )
                }
            }
        }
    }
}

@Composable
private fun EmulationSideMenuContent(
    sideMenuState: SideMenuState,
    updateState: (SideMenuState) -> Unit,
    onShowEmulatedUSBDevices: () -> Unit,
    onShowScreenLayout: () -> Unit,
    onShowCheats: () -> Unit,
    onEditInputOverlay: () -> Unit,
    onResetInputOverlay: () -> Unit,
    onQuit: () -> Unit,
) {
    CheckboxItem(
        label = tr("Enable motion"),
        checked = sideMenuState.isMotionEnabled,
        onCheckedChange = { updateState(sideMenuState.copy(isMotionEnabled = it)) },
    )

    CheckboxItem(
        label = tr("Replace TV with PAD"),
        checked = sideMenuState.isTVReplacedWithPad,
        onCheckedChange = { updateState(sideMenuState.copy(isTVReplacedWithPad = it)) },
    )

    CheckboxItem(
        label = tr("Show PAD"),
        checked = sideMenuState.isPadVisible,
        onCheckedChange = { updateState(sideMenuState.copy(isPadVisible = it)) },
    )

    TextButtonItem(
        label = tr("Emulated USB Devices"),
        onClick = onShowEmulatedUSBDevices,
    )

    TextButtonItem(
        label = tr("Screen layout"),
        onClick = onShowScreenLayout,
    )

    TextButtonItem(
        label = tr("Cheats"),
        onClick = onShowCheats,
    )

    CustomTexturesItem()

    CheckboxItem(
        label = tr("Show input overlay"),
        checked = sideMenuState.isInputOverlayVisible,
        onCheckedChange = { updateState(sideMenuState.copy(isInputOverlayVisible = it)) },
    )

    TextButtonItem(
        label = tr("Edit inputs"),
        enabled = sideMenuState.isInputOverlayVisible,
        onClick = onEditInputOverlay,
    )

    TextButtonItem(
        label = tr("Reset input overlay"),
        enabled = sideMenuState.isInputOverlayVisible,
        onClick = onResetInputOverlay,
    )

    TextButtonItem(
        label = tr("Exit"),
        onClick = onQuit,
    )
}

/**
 * Custom textures for the running game, switchable mid-game. It is the same setting as the switch
 * on the game's Custom textures screen. The texture cache is flushed afterwards so the change is
 * visible straight away, no restart needed; expect a short stutter while textures reload.
 */
@Composable
private fun CustomTexturesItem() {
    val scope = rememberCoroutineScope()
    // 0 while the game is still booting. The drawer content is composed from the start and is
    // not guaranteed to recompose later, so keep asking until the game is up.
    var titleId by remember { mutableLongStateOf(NativeCheats.getRunningTitleId()) }
    LaunchedEffect(Unit) {
        while (titleId == 0L) {
            delay(1000)
            titleId = NativeCheats.getRunningTitleId()
        }
    }
    val active by remember(titleId) {
        CustomTexturesRepository.isActiveForTitleFlow(titleId)
    }.collectAsState(initial = true)

    CheckboxItem(
        label = tr("Enable custom textures"),
        checked = titleId != 0L && active,
        enabled = titleId != 0L,
        onCheckedChange = { enabled ->
            scope.launch {
                withContext(Dispatchers.IO) {
                    CustomTexturesRepository.setTitleEnabled(titleId, enabled)
                }
                NativeCustomTextures.reloadTextures()
            }
        },
    )
}

/**
 * Screen layout for the running game only. Applies on the next frame and is remembered for this
 * game; "Use default" follows the Screen layout graphics setting.
 */
@Composable
private fun ScreenLayoutDialog(titleId: Long, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    val override by remember(titleId) { ScreenLayouts.overrideFlow(titleId) }
        .collectAsState(initial = null)
    val defaultLayout = remember { NativeSettings.getFullscreenScaling() }

    fun select(layout: Int?) {
        scope.launch { ScreenLayouts.setOverride(titleId, layout) }
        onDismiss()
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Screen layout")) },
        text = {
            Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                ScreenLayoutRow(
                    label = tr("Use default ({0})", screenLayoutToString(defaultLayout)),
                    selected = override == null,
                    onClick = { select(null) },
                )
                ScreenLayouts.all.forEach { layout ->
                    ScreenLayoutRow(
                        label = screenLayoutToString(layout),
                        selected = override == layout,
                        onClick = { select(layout) },
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(tr("Close")) }
        },
    )
}

@Composable
private fun ScreenLayoutRow(label: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Text(text = label, modifier = Modifier.padding(start = 8.dp))
    }
}

@Composable
private fun CheckboxItem(
    label: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.6f)
            .clickable(enabled) { onCheckedChange(!checked) }
            .padding(8.dp)
            .minimumInteractiveComponentSize(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            modifier = Modifier
                .padding(end = 8.dp)
                .weight(1f),
            fontSize = 16.sp,
        )

        Checkbox(
            checked = checked,
            onCheckedChange = null,
        )
    }
}

@Composable
private fun TextButtonItem(
    label: String,
    onClick: () -> Unit,
    enabled: Boolean = true,
) {
    Text(
        text = label,
        modifier = Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.6f)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(8.dp)
            .heightIn(min = 48.dp)
            .wrapContentHeight(align = Alignment.CenterVertically),
        fontSize = 16.sp,
    )
}

@Composable
private fun EmulationSurfaces(
    sideMenuState: SideMenuState,
    gamePadPosition: GamePadPosition?,
    mainHolderCallback: SurfaceHolder.Callback,
    padHolderCallback: SurfaceHolder.Callback,
    onInitializeEmulation: () -> Unit,
    isInputEnabled: Boolean,
) {
    if (gamePadPosition == null) {
        return
    }

    val isVertical = gamePadPosition.isVertical()
    val appearsAfterTV = gamePadPosition.appearsAfterTV()
    val isPadVisible = sideMenuState.isPadVisible

    @Composable
    fun MainSurface(modifier: Modifier) {
        EmulationSurface(
            modifier = modifier,
            isTV = true,
            holderCallback = mainHolderCallback,
            isInputEnabled = isInputEnabled,
            afterInit = { onInitializeEmulation() },
        )
    }

    @Composable
    fun PadSurface(modifier: Modifier) {
        if (isPadVisible) {
            EmulationSurface(
                modifier = modifier,
                isTV = false,
                holderCallback = padHolderCallback,
                isInputEnabled = isInputEnabled,
            )
        }
    }

    @Composable
    fun SurfacesInOrder(itemModifier: Modifier) {
        if (appearsAfterTV) {
            MainSurface(itemModifier)
            PadSurface(itemModifier)
        } else {
            PadSurface(itemModifier)
            MainSurface(itemModifier)
        }
    }

    LinearLayout(isVertical) { itemModifier ->
        SurfacesInOrder(itemModifier)
    }
}

@Composable
private fun LinearLayout(
    isVertical: Boolean,
    content: @Composable (Modifier) -> Unit,
) {
    if (isVertical) {
        Column(modifier = Modifier.fillMaxSize()) {
            content(Modifier.weight(1f))
        }
    } else {
        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
            Row(modifier = Modifier.fillMaxSize()) {
                content(Modifier.weight(1f))
            }
        }
    }
}

@Composable
@SuppressLint("ClickableViewAccessibility")
private fun EmulationSurface(
    modifier: Modifier,
    isTV: Boolean,
    holderCallback: SurfaceHolder.Callback,
    isInputEnabled: Boolean,
    afterInit: () -> Unit = {}
) {
    val touchListener = remember(isTV) { CanvasOnTouchListener(isTV) }
    AndroidView(
        modifier = modifier,
        update = {
            // Opening the menu takes the touch away mid-gesture; make sure the game
            // does not keep seeing the touch screen as pressed.
            if (!isInputEnabled) {
                touchListener.release()
            }
        },
        factory = { context ->
            SurfaceView(context).apply {
                var firstChange = true

                setOnTouchListener(touchListener)

                holder.addCallback(holderCallback)

                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceChanged(
                        holder: SurfaceHolder, format: Int, width: Int, height: Int
                    ) {
                        if (firstChange) {
                            afterInit()
                            firstChange = false
                        }
                    }

                    override fun surfaceCreated(holder: SurfaceHolder) {}

                    override fun surfaceDestroyed(holder: SurfaceHolder) {}
                })
            }
        })
}

@Composable
private fun EmulationQuitConfirmationDialog(onQuit: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        title = { Text(tr("Exit confirmation")) },
        text = { Text(tr("Are you sure you want to exit?")) },
        confirmButton = { TextButton(onClick = onQuit) { Text(tr("Yes")) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(tr("No")) } },
        onDismissRequest = onDismiss,
    )
}

@Composable
private fun EmulationLoadingDialog() {
    AlertDialog(
        title = { Text(tr("Initializing emulation")) },
        text = { LinearProgressIndicator(modifier = Modifier.fillMaxWidth()) },
        confirmButton = {},
        onDismissRequest = {},
    )
}

@Composable
private fun EmulationErrorDialog(error: NativeError, onQuit: () -> Unit) {
    val errorMessage = remember(error) {
        when (error) {
            is NativeError.SystemInitializationError -> tr("Failed to initialize")

            is NativeError.RendererInitializationError ->
                tr("Failed creating renderer: {0}", error.message)

            is NativeError.SurfaceCreationError -> tr("Failed creating surface: {0}", error.message)

            NativeError.GameFilesNotFoundError -> tr("Unable to launch game because the base files were not found.")
            NativeError.NoDiscKeysError -> tr("Could not decrypt title. Make sure that keys.txt contains the correct disc key for this title.")
            NativeError.NoTitleTikError -> tr("Could not decrypt title because title.tik is missing.")
            is NativeError.UnknownTilePrepareError ->
                tr("Unable to launch game\nPath: {0}", error.launchPath)

            NativeError.LaunchingTitleError -> tr("Failed to launch title")
        }
    }
    AlertDialog(
        title = { Text(tr("Error")) },
        text = { Text(errorMessage) },
        confirmButton = { TextButton(onClick = onQuit) { Text(tr("Quit")) } },
        onDismissRequest = {},
    )
}
