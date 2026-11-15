package com.androserver

import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections

object NetworkUtils {
    /**
     * 端末のローカルWi-Fi / イーサネットIPv4アドレスを取得
     */
    fun getLocalIpAddress(): String? {
        try {
            val interfaces = Collections.list(NetworkInterface.getNetworkInterfaces())
            // wlan0 (Wi-Fi) を優先的に検索
            for (intf in interfaces) {
                if (intf.isUp && (intf.name.startsWith("wlan") || intf.name.startsWith("eth") || intf.name.startsWith("ap"))) {
                    val addrs = Collections.list(intf.inetAddresses)
                    for (addr in addrs) {
                        if (!addr.isLoopbackAddress && addr is Inet4Address) {
                            return addr.hostAddress
                        }
                    }
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
        return null
    }
}

