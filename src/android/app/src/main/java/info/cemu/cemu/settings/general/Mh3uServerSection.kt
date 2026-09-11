package info.cemu.cemu.settings.general

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import info.cemu.cemu.common.ui.components.Header
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeMH3U

/**
 * Edits the MH3U revival server endpoint.
 *
 * On desktop this lives in a text file the user edits by hand ([mh3u_server.txt] in the Cemu config
 * folder). That directory is app-private on Android, so the value is edited here and written to the
 * same file — the emulator core reads it unchanged.
 *
 * The 15-character host limit is not cosmetic: the Wii U nexToken host field is char[0x10], so a
 * longer host is silently discarded by the core in favour of 127.0.0.1. Use an IP address.
 *
 * Drop this into [GeneralSettingsScreen]'s ScreenContent body, or give it its own settings route.
 */
@Composable
fun Mh3uServerSection() {
    var endpoint by remember { mutableStateOf(NativeMH3U.getServerEndpoint()) }
    val maxHostLength = remember { NativeMH3U.getMaxHostLength() }
    val valid = remember(endpoint) { NativeMH3U.isValidEndpoint(endpoint) }

    Column(modifier = Modifier.padding(horizontal = 8.dp)) {
        Header(text = tr("Monster Hunter 3 Ultimate online"))
        OutlinedTextField(
            value = endpoint,
            onValueChange = {
                endpoint = it.trim()
                // Only commit values the core will accept; an invalid one would fall back to
                // 127.0.0.1 without telling the user.
                if (NativeMH3U.isValidEndpoint(endpoint)) {
                    NativeMH3U.setServerEndpoint(endpoint)
                }
            },
            label = { Text(tr("Revival server address")) },
            placeholder = { Text("127.0.0.1:${NativeMH3U.DEFAULT_PORT}") },
            singleLine = true,
            isError = !valid,
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.None,
                imeAction = ImeAction.Done,
            ),
            supportingText = {
                Text(
                    text = if (valid) {
                        tr(
                            "IP address, optionally with :port. Host must be {0} characters or fewer. Leave empty to use 127.0.0.1:{1}.",
                            maxHostLength,
                            NativeMH3U.DEFAULT_PORT,
                        )
                    } else {
                        tr("Host must be {0} characters or fewer and contain no spaces or slashes.", maxHostLength)
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
            },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
