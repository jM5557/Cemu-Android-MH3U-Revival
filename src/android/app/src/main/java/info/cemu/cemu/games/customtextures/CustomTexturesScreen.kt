package info.cemu.cemu.games.customtextures

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.MutableCreationExtras
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.common.ui.components.Button
import info.cemu.cemu.common.ui.components.Header
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.components.Toggle
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeCustomTextures
import info.cemu.cemu.nativeinterface.NativeGameTitles

@Composable
fun CustomTexturesScreen(
    game: NativeGameTitles.Game,
    navigateBack: () -> Unit,
    viewModel: CustomTexturesViewModel = viewModel(
        factory = CustomTexturesViewModel.Factory,
        extras = MutableCreationExtras().apply {
            set(CustomTexturesViewModel.TITLE_ID_KEY, game.titleId)
        },
    ),
) {
    val uiState by viewModel.uiState.collectAsState()

    ScreenContent(
        appBarText = tr("Custom textures"),
        navigateBack = navigateBack,
        contentModifier = Modifier.padding(16.dp),
        contentVerticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Header(text = game.name)

        if (uiState.loading) {
            CircularProgressIndicator()
            return@ScreenContent
        }

        Toggle(
            label = tr("Enable custom textures"),
            description = tr("Replace the game's textures with DDS files from the folder below"),
            checked = uiState.enabled,
            onCheckedChanged = viewModel::setEnabled,
        )

        Text(
            text = uiState.folder,
            style = MaterialTheme.typography.bodySmall,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
        Text(
            text = tr("Each folder here is a pack. Loose files outside a pack always load and act as a base layer. Where two enabled packs provide the same texture, the first alphabetically wins."),
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(horizontal = 8.dp),
        )

        HorizontalDivider()

        if (uiState.availablePacks.isEmpty()) {
            Text(
                text = tr("No texture packs found."),
                modifier = Modifier.padding(8.dp),
            )
        } else {
            uiState.availablePacks.forEach { pack ->
                PackRow(
                    name = pack,
                    checked = pack in uiState.selectedPacks,
                    enabled = uiState.enabled,
                    onCheckedChange = { viewModel.setPackSelected(pack, it) },
                )
            }
        }

        if (uiState.checkingConflicts) {
            CircularProgressIndicator()
        } else if (uiState.conflicts.isNotEmpty()) {
            HorizontalDivider()
            Text(
                text = tr("{0} textures are provided by more than one enabled pack", uiState.conflicts.size),
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(horizontal = 8.dp),
            )
            // Long lists are unhelpful on a phone; the first handful is enough to spot the problem.
            uiState.conflicts.take(10).forEach { conflict ->
                Text(
                    text = conflict,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(horizontal = 8.dp),
                )
            }
        }

        HorizontalDivider()

        TextureDumpSection()

        Button(
            label = tr("Reload textures now"),
            description = tr("Applies changes to a running game without restarting it"),
            onClick = viewModel::reloadNow,
        )
        Button(
            label = tr("Reset to defaults"),
            description = tr("Enable every pack in this title's folder"),
            onClick = viewModel::resetToDefaults,
        )
    }
}

@Composable
private fun PackRow(
    name: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled) { onCheckedChange(!checked) }
            .padding(horizontal = 8.dp),
    ) {
        Checkbox(
            checked = checked,
            enabled = enabled,
            onCheckedChange = onCheckedChange,
        )
        Column(modifier = Modifier.padding(start = 8.dp)) {
            Text(text = name, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

/**
 * Texture dumping: writes each replaceable texture the game loads to dump/textures, named exactly
 * as a replacement for it has to be named. Only textures that are actually drawn get written, so
 * play through the areas you want.
 */
@Composable
private fun TextureDumpSection() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val settings by CustomTexturesRepository.settingsFlow.collectAsState(initial = null)
    val dumping = settings?.dumpTextures ?: NativeCustomTextures.isDumpingTextures()
    val status by rememberTextureDumpStatus()
    val folder = remember { NativeCustomTextures.getDumpFolder() }
    var resultMessage by remember { mutableStateOf<String?>(null) }

    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            resultMessage = tr("Exporting...")
            val copied = withContext(Dispatchers.IO) {
                runCatching { TextureDumpExport.export(context, uri) }
            }
            resultMessage = copied.fold(
                onSuccess = { tr("Exported {0} new files to \"Cemu texture dump\"", it) },
                onFailure = { tr("Export failed: {0}", it.message ?: it.javaClass.simpleName) },
            )
        }
    }

    Toggle(
        label = tr("Dump textures"),
        description = tr("Saves every texture the game draws that a pack can replace, as a TGA named exactly as its replacement must be named. Stays on until you turn it off."),
        checked = dumping,
        onCheckedChanged = { enabled ->
            scope.launch { CustomTexturesRepository.setDumpingTextures(enabled) }
        },
    )
    Text(
        text = status.summary(),
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.padding(horizontal = 8.dp),
    )
    if (status.lastError.isNotEmpty()) {
        Text(
            text = tr("Last error: {0}", status.lastError),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
    }
    Text(
        text = folder,
        style = MaterialTheme.typography.bodySmall,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier.padding(horizontal = 8.dp),
    )

    Button(
        label = tr("Export dumped textures"),
        description = tr("Copies the dump folder to a folder you choose, such as Download, where any file manager or a PC can see it"),
        onClick = { exportLauncher.launch(null) },
    )
    Button(
        label = tr("Clear dump folder"),
        description = when {
            status.filesInFolder == 0 -> tr("Nothing to delete")
            status.filesInFolder > 0 -> tr("Deletes all {0} files in dump/textures. Textures on screen are written again if dumping is on", status.filesInFolder)
            else -> tr("Deletes every file in dump/textures")
        },
        onClick = {
            scope.launch {
                val removed = withContext(Dispatchers.IO) { NativeCustomTextures.clearDumpFolder() }
                resultMessage = tr("Removed {0} files", removed)
            }
        },
    )
    resultMessage?.let {
        Text(
            text = it,
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
    }
}
