package info.cemu.cemu.emulation

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import info.cemu.cemu.common.ui.localization.tr
import java.text.DateFormat
import java.util.Date

/**
 * Save states of the running game: save, load or delete any slot, and undo the last load. Slot 0 is
 * the quick slot the quick save / quick load hotkeys use.
 *
 * A state is tied to the game, its version and this exact build of Cemu; a slot that cannot be
 * loaded says why instead of failing after it is picked.
 */
@Composable
fun SaveStatesDialog(
    viewModel: EmulationViewModel,
    onDismiss: () -> Unit,
) {
    val revision by viewModel.saveStateRevision.collectAsState()
    val busy by viewModel.saveStateBusy.collectAsState()
    var slots by remember { mutableStateOf<List<SaveStateSlot>?>(null) }
    var hasUndo by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf<SlotConfirmation?>(null) }

    LaunchedEffect(revision) {
        slots = viewModel.querySaveStateSlots()
        hasUndo = viewModel.hasUndoState()
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Save states")) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val currentSlots = slots
                if (currentSlots == null) {
                    Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator()
                    }
                } else {
                    currentSlots.forEach { slot ->
                        SaveStateSlotRow(
                            slot = slot,
                            enabled = busy == null,
                            onSave = {
                                if (slot.exists) confirm = SlotConfirmation.Overwrite(slot.slot)
                                else viewModel.saveState(slot.slot)
                            },
                            onLoad = {
                                viewModel.loadState(slot.slot)
                                onDismiss()
                            },
                            onDelete = { confirm = SlotConfirmation.Delete(slot.slot) },
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(tr("Close")) } },
        dismissButton = {
            TextButton(
                enabled = hasUndo && busy == null,
                onClick = {
                    viewModel.undoLoadState()
                    onDismiss()
                },
            ) { Text(tr("Undo last load")) }
        },
    )

    confirm?.let { pending ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = {
                Text(
                    when (pending) {
                        is SlotConfirmation.Overwrite -> tr("Overwrite slot {0}?", pending.slot)
                        is SlotConfirmation.Delete -> tr("Delete slot {0}?", pending.slot)
                    }
                )
            },
            text = {
                Text(
                    when (pending) {
                        is SlotConfirmation.Overwrite -> tr("The state already in this slot will be replaced.")
                        is SlotConfirmation.Delete -> tr("The state in this slot will be deleted.")
                    }
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    when (pending) {
                        is SlotConfirmation.Overwrite -> viewModel.saveState(pending.slot)
                        is SlotConfirmation.Delete -> viewModel.deleteSaveState(pending.slot)
                    }
                    confirm = null
                }) {
                    Text(
                        when (pending) {
                            is SlotConfirmation.Overwrite -> tr("Overwrite")
                            is SlotConfirmation.Delete -> tr("Delete")
                        }
                    )
                }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text(tr("Cancel")) } },
        )
    }
}

private sealed interface SlotConfirmation {
    val slot: Int

    data class Overwrite(override val slot: Int) : SlotConfirmation
    data class Delete(override val slot: Int) : SlotConfirmation
}

@Composable
private fun SaveStateSlotRow(
    slot: SaveStateSlot,
    enabled: Boolean,
    onSave: () -> Unit,
    onLoad: () -> Unit,
    onDelete: () -> Unit,
) {
    val canLoad = slot.exists && slot.incompatibility.isEmpty()
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            Text(
                text = if (slot.slot == 0) tr("Slot 0 (quick)") else tr("Slot {0}", slot.slot),
                fontSize = 16.sp,
            )
            Text(
                text = when {
                    !slot.exists -> tr("Empty")
                    slot.incompatibility.isNotEmpty() -> slot.incompatibility
                    else -> formatSavedAt(slot.savedAtSeconds)
                },
                fontSize = 13.sp,
                color = if (slot.exists && slot.incompatibility.isNotEmpty()) MaterialTheme.colorScheme.error
                else Color.Unspecified,
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                // A slot that cannot be loaded can still be deleted: that is when you want rid of it
                TextButton(enabled = enabled && slot.exists, onClick = onDelete) { Text(tr("Delete")) }
                TextButton(enabled = enabled && canLoad, onClick = onLoad) { Text(tr("Load")) }
                TextButton(enabled = enabled, onClick = onSave) { Text(tr("Save")) }
            }
        }
    }
}

private fun formatSavedAt(seconds: Long): String =
    if (seconds <= 0L) tr("Saved state")
    else DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(seconds * 1000L))

/**
 * Shown over the game while a save, load or undo runs. The game is stopped for part of it, and a
 * second request would only be refused, so it also keeps taps away from the game until it is done.
 */
@Composable
fun SaveStateBusyOverlay(busy: SaveStateBusy) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.35f))
            // swallow every touch so none reaches the game or the input overlay underneath
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        awaitPointerEvent().changes.forEach { it.consume() }
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Card {
            Row(
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                CircularProgressIndicator()
                Text(
                    text = when (busy.action) {
                        SaveStateAction.SAVE -> tr("Saving state to slot {0}…", busy.slot ?: 0)
                        SaveStateAction.LOAD -> tr("Loading state from slot {0}…", busy.slot ?: 0)
                        SaveStateAction.UNDO -> tr("Undoing the last load…")
                    },
                    fontSize = 16.sp,
                )
            }
        }
    }
}

/** The snackbar line for a finished save, load or undo. */
fun saveStateEventMessage(event: SaveStateEvent): String {
    val slot = event.operation.slot ?: 0
    return when (event) {
        is SaveStateEvent.Succeeded -> when (event.operation.action) {
            SaveStateAction.SAVE -> tr("State saved to slot {0}", slot)
            SaveStateAction.LOAD -> tr("State loaded from slot {0}", slot)
            SaveStateAction.UNDO -> tr("Last load undone")
        }

        is SaveStateEvent.Failed -> when (event.operation.action) {
            SaveStateAction.SAVE -> tr("Save state failed: {0}", event.message)
            SaveStateAction.LOAD -> tr("Load state failed: {0}", event.message)
            SaveStateAction.UNDO -> tr("Undo failed: {0}", event.message)
        }
    }
}
