package info.cemu.cemu.graphicpacks

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import info.cemu.cemu.nativeinterface.NativeActiveSettings
import info.cemu.cemu.nativeinterface.NativeGraphicPacks
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.util.zip.ZipInputStream

sealed interface GraphicPackImportResult {
    /** [name] is the folder created in graphicPacks, [packCount] the rules.txt files found in it. */
    data class Success(val name: String, val packCount: Int) : GraphicPackImportResult
    data object NoRulesFound : GraphicPackImportResult
    data class Error(val message: String) : GraphicPackImportResult
}

/**
 * Your own graphic packs: a folder or a .zip is copied into graphicPacks/<name>, next to
 * downloadedGraphicPacks (which the downloader replaces on every update, so imports never go in
 * there). Cemu finds every rules.txt below graphicPacks, so one import may hold several packs.
 * Importing something with the same name again replaces the earlier copy.
 */
object GraphicPackImporter {
    private const val DOWNLOADED_DIR = "downloadedGraphicPacks"
    private const val TEMP_DIR = ".importing"

    private fun graphicPacksDir(): File =
        File(NativeActiveSettings.getUserDataPath()).resolve("graphicPacks")

    suspend fun importFolder(context: Context, treeUri: Uri): GraphicPackImportResult =
        withContext(Dispatchers.IO) {
            val root = DocumentFile.fromTreeUri(context, treeUri)
                ?: return@withContext GraphicPackImportResult.Error("Can't open the folder")
            import(root.name ?: "Imported graphic pack") { temp ->
                copyTree(context, root, temp)
            }
        }

    suspend fun importZip(context: Context, uri: Uri): GraphicPackImportResult =
        withContext(Dispatchers.IO) {
            val fileName = DocumentFile.fromSingleUri(context, uri)?.name ?: "Imported graphic pack"
            import(fileName.removeSuffix(".zip").removeSuffix(".ZIP")) { temp ->
                val stream = context.contentResolver.openInputStream(uri)
                    ?: throw IOException("Can't open the file")
                stream.use { unzipSafely(it, temp) }
            }
        }

    private fun import(name: String, fill: (File) -> Unit): GraphicPackImportResult {
        val baseDir = graphicPacksDir()
        val temp = baseDir.resolve(TEMP_DIR)
        try {
            temp.deleteRecursively()
            if (!temp.mkdirs())
                return GraphicPackImportResult.Error("Can't write to ${baseDir.path}")
            fill(temp)

            // A zip (or folder) that only wraps one folder: use that folder and its name
            var content = temp
            var folderName = name
            val entries = temp.listFiles().orEmpty()
            if (entries.size == 1 && entries[0].isDirectory && !temp.resolve("rules.txt").exists()) {
                content = entries[0]
                folderName = entries[0].name
            }

            val packCount = content.walkTopDown().count { it.isFile && it.name.equals("rules.txt", ignoreCase = true) }
            if (packCount == 0)
                return GraphicPackImportResult.NoRulesFound

            val target = baseDir.resolve(safeFolderName(folderName))
            target.deleteRecursively()
            if (!content.renameTo(target)) {
                content.copyRecursively(target, overwrite = true)
            }
            temp.deleteRecursively() // before the rescan, so nothing is found twice
            NativeGraphicPacks.refreshGraphicPacks()
            return GraphicPackImportResult.Success(target.name, packCount)
        } catch (e: Exception) {
            return GraphicPackImportResult.Error(e.message ?: e.javaClass.simpleName)
        } finally {
            temp.deleteRecursively()
        }
    }

    private fun safeFolderName(name: String): String {
        val cleaned = name.replace(Regex("[\\\\/:*?\"<>|]"), "_").trim().trimStart('.')
        return when {
            cleaned.isEmpty() -> "Imported graphic pack"
            cleaned.equals(DOWNLOADED_DIR, ignoreCase = true) -> "$cleaned (imported)"
            else -> cleaned
        }
    }

    private fun copyTree(context: Context, source: DocumentFile, target: File) {
        target.mkdirs()
        for (child in source.listFiles()) {
            val childName = child.name ?: continue
            if (child.isDirectory) {
                copyTree(context, child, target.resolve(childName))
            } else if (child.isFile) {
                context.contentResolver.openInputStream(child.uri)?.use { input ->
                    target.resolve(childName).outputStream().use { input.copyTo(it) }
                }
            }
        }
    }

    // Entries that would land outside the target folder ("../") are skipped
    private fun unzipSafely(stream: java.io.InputStream, target: File) {
        val root = target.canonicalFile
        ZipInputStream(stream).use { zip ->
            var entry = zip.nextEntry
            while (entry != null) {
                val file = File(root, entry.name).canonicalFile
                if (file.path.startsWith(root.path + File.separator)) {
                    if (entry.isDirectory) {
                        file.mkdirs()
                    } else {
                        file.parentFile?.mkdirs()
                        file.outputStream().use { zip.copyTo(it) }
                    }
                }
                entry = zip.nextEntry
            }
        }
    }
}
