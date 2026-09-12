package info.cemu.cemu.games.customtextures

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
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.MutableCreationExtras
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.common.ui.components.Button
import info.cemu.cemu.common.ui.components.Header
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.components.Toggle
import info.cemu.cemu.common.ui.localization.tr
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

        TextureDumpToggle()

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
 * Turns Cemu's texture dumping on for this session.
 *
 * Two uses: it produces correctly-named TGA files to base a pack on, and it writes
 * dump/textures/rename_map.csv, which tools/migrate_texture_pack.py uses to rename a pack built
 * against the old hash scheme. Only textures actually drawn get recorded, so play through the
 * areas the pack covers.
 *
 * Not persisted - it resets when the app restarts, because dumping writes a file per texture.
 */
@Composable
private fun TextureDumpToggle() {
    var dumping by remember { mutableStateOf(NativeCustomTextures.isDumpingTextures()) }
    val folder = remember { NativeCustomTextures.getDumpFolder() }

    Toggle(
        label = tr("Dump textures"),
        description = tr("Writes every texture the game draws, plus rename_map.csv for migrating a pack. Resets when the app restarts."),
        checked = dumping,
        onCheckedChanged = {
            dumping = it
            NativeCustomTextures.setDumpingTextures(it)
        },
    )
    if (dumping) {
        Text(
            text = folder,
            style = MaterialTheme.typography.bodySmall,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
    }
}
