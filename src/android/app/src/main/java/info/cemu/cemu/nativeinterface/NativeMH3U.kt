package info.cemu.cemu.nativeinterface

object NativeMH3U {
    /** `<host>` or `<host>:<port>`, or an empty string when unset (core then uses 127.0.0.1:1223). */
    @JvmStatic
    external fun getServerEndpoint(): String

    /** Writes mh3u_server.txt. An empty or blank value deletes the file. */
    @JvmStatic
    external fun setServerEndpoint(endpoint: String)

    @JvmStatic
    external fun isValidEndpoint(endpoint: String): Boolean

    /** 15 — the Wii U nexToken.host field is char[0x10]. */
    @JvmStatic
    external fun getMaxHostLength(): Int

    const val DEFAULT_PORT = 1223
}
