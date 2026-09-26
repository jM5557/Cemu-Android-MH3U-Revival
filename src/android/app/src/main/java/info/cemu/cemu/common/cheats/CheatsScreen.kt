package info.cemu.cemu.common.cheats

// Shared by the game list (games) and the in-game side menu (emulation). Feature packages must not
// depend on each other (see ArchitectureTests), so the cheats UI lives in common.

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.viewmodel.MutableCreationExtras
import androidx.lifecycle.viewmodel.compose.viewModel
import info.cemu.cemu.R
import info.cemu.cemu.common.ui.components.Header
import info.cemu.cemu.common.ui.components.ScreenContent
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeGameTitles

/** Cheats of one game, opened from the game list. Works whether or not the game is running. */
@Composable
fun CheatsScreen(
    game: NativeGameTitles.Game,
    navigateBack: () -> Unit,
    viewModel: CheatsViewModel = cheatsViewModel(game.titleId),
) {
    val uiState by viewModel.uiState.collectAsState()
    var editing by remember { mutableStateOf<CheatEditTarget?>(null) }

    ScreenContent(
        appBarText = tr("Cheats"),
        navigateBack = navigateBack,
        actions = { CheatsActions(onAdd = { editing = CheatEditTarget(null) }, onReload = viewModel::reloadFromDisk) },
        contentModifier = Modifier.padding(16.dp),
        contentVerticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Header(text = game.name)
        CheatsContent(
            uiState = uiState,
            onToggle = viewModel::setEnabled,
            onEdit = { editing = CheatEditTarget(it) },
            onDismissError = viewModel::clearError,
        )
    }

    editing?.let { target ->
        CheatEditDialogFor(target, uiState, viewModel, onDone = { editing = null })
    }
}

/**
 * The same list as [CheatsScreen], shown over the running game from the emulation side menu.
 * Toggling a cheat takes effect on the next frame.
 */
@Composable
fun CheatsDialog(
    titleId: Long,
    onDismiss: () -> Unit,
) {
    val viewModel = cheatsViewModel(titleId)
    val uiState by viewModel.uiState.collectAsState()
    var editing by remember { mutableStateOf<CheatEditTarget?>(null) }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = MaterialTheme.colorScheme.surface,
        ) {
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onDismiss) {
                        Icon(
                            painter = painterResource(R.drawable.ic_close),
                            contentDescription = tr("Close"),
                        )
                    }
                    Text(
                        text = tr("Cheats"),
                        fontSize = 18.sp,
                        modifier = Modifier.weight(1f),
                    )
                    CheatsActions(onAdd = { editing = CheatEditTarget(null) }, onReload = viewModel::reloadFromDisk)
                }
                Column(
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier
                        .padding(16.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                    CheatsContent(
                        uiState = uiState,
                        onToggle = viewModel::setEnabled,
                        onEdit = { editing = CheatEditTarget(it) },
                        onDismissError = viewModel::clearError,
                    )
                }
            }
        }
    }

    editing?.let { target ->
        CheatEditDialogFor(target, uiState, viewModel, onDone = { editing = null })
    }
}

@Composable
private fun cheatsViewModel(titleId: Long): CheatsViewModel = viewModel(
    // keyed by title, so the in-game dialog never reuses another game's list
    key = "cheats_%016x".format(titleId),
    factory = CheatsViewModel.Factory,
    extras = MutableCreationExtras().apply {
        set(CheatsViewModel.TITLE_ID_KEY, titleId)
    },
)

/** Which cheat the edit dialog is for; a null index means a new cheat. */
private data class CheatEditTarget(val index: Int?)

@Composable
private fun CheatsActions(onAdd: () -> Unit, onReload: () -> Unit) {
    IconButton(onClick = onReload) {
        Icon(
            painter = painterResource(R.drawable.ic_refresh),
            contentDescription = tr("Reload from file"),
        )
    }
    IconButton(onClick = onAdd) {
        Icon(
            painter = painterResource(R.drawable.ic_add),
            contentDescription = tr("Add cheat"),
        )
    }
}

@Composable
private fun ColumnScope.CheatsContent(
    uiState: CheatsUiState,
    onToggle: (Int, Boolean) -> Unit,
    onEdit: (Int) -> Unit,
    onDismissError: () -> Unit,
) {
    if (uiState.loading) {
        CircularProgressIndicator()
        return
    }

    Text(
        text = if (uiState.isRunning)
            tr("The game is running: changes apply immediately.")
        else
            tr("Changes take effect the next time the game starts."),
        style = MaterialTheme.typography.bodyMedium,
        modifier = Modifier.padding(horizontal = 8.dp),
    )
    Text(
        text = uiState.filePath,
        style = MaterialTheme.typography.bodySmall,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier.padding(horizontal = 8.dp),
    )

    uiState.error?.let { error ->
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = error,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier
                    .weight(1f)
                    .padding(horizontal = 8.dp),
            )
            TextButton(onClick = onDismissError) { Text(tr("OK")) }
        }
    }

    HorizontalDivider()

    if (uiState.cheats.isEmpty()) {
        Text(
            text = tr("No cheats yet. Tap + to add one."),
            modifier = Modifier.padding(8.dp),
        )
        return
    }

    uiState.cheats.forEachIndexed { index, cheat ->
        CheatRow(
            cheat = cheat,
            onToggle = { onToggle(index, it) },
            onClick = { onEdit(index) },
        )
    }
}

@Composable
private fun CheatRow(
    cheat: Cheat,
    onToggle: (Boolean) -> Unit,
    onClick: () -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .heightIn(min = 56.dp)
            .padding(horizontal = 8.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text = cheat.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (cheat.notes.isNotBlank()) {
                Text(
                    text = cheat.notes,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Switch(
            checked = cheat.enabled,
            onCheckedChange = onToggle,
            modifier = Modifier.padding(start = 8.dp),
        )
    }
}

@Composable
private fun CheatEditDialogFor(
    target: CheatEditTarget,
    uiState: CheatsUiState,
    viewModel: CheatsViewModel,
    onDone: () -> Unit,
) {
    val existing = target.index?.let { uiState.cheats.getOrNull(it) }
    CheatEditDialog(
        initial = existing,
        validate = viewModel::validate,
        onSave = { cheat ->
            viewModel.upsert(target.index, cheat)
            onDone()
        },
        onDelete = target.index?.let { index ->
            {
                viewModel.delete(index)
                onDone()
            }
        },
        onDismiss = onDone,
    )
}

@Composable
private fun CheatEditDialog(
    initial: Cheat?,
    validate: (String) -> String,
    onSave: (Cheat) -> Unit,
    onDelete: (() -> Unit)?,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf(initial?.name ?: "") }
    var notes by remember { mutableStateOf(initial?.notes ?: "") }
    var code by remember { mutableStateOf(initial?.code ?: "") }
    var error by remember { mutableStateOf<String?>(null) }
    var confirmDelete by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial == null) tr("Add cheat") else tr("Edit cheat")) },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.verticalScroll(rememberScrollState()),
            ) {
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it; error = null },
                    label = { Text(tr("Name")) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = notes,
                    onValueChange = { notes = it },
                    label = { Text(tr("Notes")) },
                    minLines = 2,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = code,
                    onValueChange = { code = it; error = null },
                    label = { Text(tr("Code")) },
                    placeholder = { Text("02123450 38A00000") },
                    minLines = 4,
                    textStyle = TextStyle(fontFamily = FontFamily.Monospace),
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.Characters,
                        autoCorrectEnabled = false,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
                error?.let {
                    Text(text = it, color = MaterialTheme.colorScheme.error)
                }
                if (onDelete != null) {
                    TextButton(onClick = { confirmDelete = true }) {
                        Text(tr("Delete cheat"), color = MaterialTheme.colorScheme.error)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                // names become the [header] line of the file, so keep them to one line
                val cleanName = name.replace(Regex("[\\r\\n\\[\\]]"), " ").trim()
                val codeError = validate(code)
                when {
                    cleanName.isEmpty() -> error = tr("Please give the cheat a name.")
                    codeError.isNotEmpty() -> error = codeError
                    else -> onSave(
                        Cheat(
                            name = cleanName,
                            notes = notes.trim(),
                            enabled = initial?.enabled ?: false,
                            code = code.trim(),
                        )
                    )
                }
            }) { Text(tr("Save")) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(tr("Cancel")) }
        },
    )

    if (confirmDelete && onDelete != null) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text(tr("Delete cheat")) },
            text = { Text(tr("Delete the cheat \"{0}\"?", initial?.name ?: "")) },
            confirmButton = { TextButton(onClick = onDelete) { Text(tr("Yes")) } },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text(tr("No")) } },
        )
    }
}
