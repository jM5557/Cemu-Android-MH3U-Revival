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
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import info.cemu.cemu.common.ui.components.Header
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeMH3U
import info.cemu.cemu.nativeinterface.NativeOnlineFiles
import info.cemu.cemu.common.ui.components.Button

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
        OnlineGateFilesRow()
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

/**
 * Generates the dumpless "online gate" files and switches the account to a custom network service.
 *
 * Cemu will not let a title go online unless iosuCrypt_checkRequirementsForOnlineMode() passes
 * (otp.bin at 1024 bytes, seeprom.bin at 512, and every certificate path present under
 * sys/title/0005001b/10054000/content) *and* the active account's network service is not Offline.
 * Both halves are required; having only the files is the usual reason nothing appears to happen.
 *
 * The generated files contain no Nintendo data — zeroes and 4-byte stubs, exactly as the
 * mh3u-revival launcher produces on Windows. They work because the MH3U patch replaces the whole
 * account-server exchange, so nothing ever reads their contents. They therefore only enable the
 * revival server: Nintendo and Pretendo will not work with stub files, and shouldn't be selected.
 *
 * An existing real dump is never overwritten — a file of the correct size is left alone.
 */
@Composable
private fun OnlineGateFilesRow() {
    val scope = rememberCoroutineScope()
    var status by remember { mutableStateOf(describeState()) }
    var error by remember { mutableStateOf<String?>(null) }
    var working by remember { mutableStateOf(false) }

    Text(
        text = status,
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.padding(vertical = 4.dp),
    )
    error?.let {
        Text(
            text = it,
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.bodySmall,
        )
    }
    Button(
        label = tr("Set up dumpless online"),
        description = tr("Creates the files Cemu requires for online mode and selects a custom network service. Does not overwrite a real dump."),
        onClick = {
            if (working) return@Button
            working = true
            scope.launch {
                // generateGateFiles also writes network_services.xml and selects the Custom
                // service; doing it separately here would skip the XML and read back as Offline.
                val failure = withContext(Dispatchers.IO) { NativeOnlineFiles.generateGateFiles() }
                error = failure.ifEmpty { null }
                status = describeState()
                working = false
            }
        },
    )
}

private fun describeState(): String = when {
    NativeOnlineFiles.isOnlineEnabled() -> "Online mode: ready"
    NativeOnlineFiles.hasRequiredOnlineFiles() ->
        "Online mode: files present, but the account network service is Offline"
    // (kept distinct so the two halves of IsOnlineEnabled stay diagnosable)
    else -> "Online mode: required files are missing"
}
