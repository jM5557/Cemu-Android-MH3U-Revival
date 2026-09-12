package info.cemu.cemu.nativeinterface

object NativeOnlineFiles {
    /** Empty string on success, otherwise a human-readable failure reason. */
    @JvmStatic
    external fun generateGateFiles(): String

    @JvmStatic
    external fun hasRequiredOnlineFiles(): Boolean

    /** Files present AND the account's network service is not Offline. */
    @JvmStatic
    external fun isOnlineEnabled(): Boolean

    @JvmStatic
    external fun getNetworkService(): Int

    @JvmStatic
    external fun setNetworkService(service: Int)

    const val SERVICE_OFFLINE = 0
    const val SERVICE_NINTENDO = 1
    const val SERVICE_PRETENDO = 2
    const val SERVICE_CUSTOM = 3
}
