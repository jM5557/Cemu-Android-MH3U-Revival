package info.cemu.cemu.games.customtextures

import android.content.Context
import android.net.Uri
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.documentfile.provider.DocumentFile
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeCustomTextures
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File

/** What the texture dumper has done, as shown in the UI. */
data class TextureDumpStatus(
    val written: Int = 0,
    val failed: Int = 0,
    /** -1 when not counted. */
    val filesInFolder: Int = -1,
    val lastError: String = "",
) {
    fun summary(): String {
        val session = if (failed > 0)
            tr("{0} written this session, {1} failed.", written, failed)
        else
            tr("{0} written this session.", written)
        return if (filesInFolder >= 0) session + " " + tr("{0} files in the folder.", filesInFolder) else session
    }
}

/**
 * Polls the dumper once a second while composed. The counts come straight from the native code
 * that writes the files, so they show whether dumping is working independently of whether a file
 * manager shows the folder (many do not show Android/data, and over USB new files often appear
 * only after a reboot).
 */
@Composable
fun rememberTextureDumpStatus(countFolder: Boolean = true): State<TextureDumpStatus> {
    val status = remember { mutableStateOf(TextureDumpStatus()) }
    LaunchedEffect(countFolder) {
        while (true) {
            status.value = withContext(Dispatchers.IO) { readTextureDumpStatus(countFolder) }
            delay(1000)
        }
    }
    return status
}

/** Counting the folder lists it, so leave it off for anything polled during gameplay. */
fun readTextureDumpStatus(countFolder: Boolean = true): TextureDumpStatus {
    val counts = NativeCustomTextures.getDumpCounts()
    return TextureDumpStatus(
        written = counts.getOrElse(0) { 0 },
        failed = counts.getOrElse(1) { 0 },
        filesInFolder = if (countFolder) NativeCustomTextures.getDumpFileCount() else -1,
        lastError = NativeCustomTextures.getDumpLastError(),
    )
}

object TextureDumpExport {
    private const val EXPORT_FOLDER_NAME = "Cemu texture dump"

    /**
     * Copies every file in dump/textures into a "Cemu texture dump" folder inside the folder the
     * user picked. Files already there are skipped, so exporting again only adds new dumps.
     * Returns the number of files copied. Blocking; call it off the main thread.
     */
    fun export(context: Context, treeUri: Uri): Int {
        val files = File(NativeCustomTextures.getDumpFolder()).listFiles { file -> file.isFile }
            ?: return 0
        val root = DocumentFile.fromTreeUri(context, treeUri) ?: return 0
        val target = root.findFile(EXPORT_FOLDER_NAME)?.takeIf { it.isDirectory }
            ?: root.createDirectory(EXPORT_FOLDER_NAME)
            ?: return 0
        val existing = target.listFiles().mapNotNullTo(HashSet()) { it.name }
        var copied = 0
        for (file in files) {
            if (file.name in existing) continue
            val document = target.createFile("application/octet-stream", file.name) ?: continue
            val output = context.contentResolver.openOutputStream(document.uri) ?: continue
            output.use { out -> file.inputStream().use { it.copyTo(out) } }
            copied++
        }
        return copied
    }
}
