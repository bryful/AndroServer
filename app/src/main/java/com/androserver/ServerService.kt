package com.androserver

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Base64
import android.widget.Toast
import androidx.core.app.NotificationCompat
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext

class ServerService : Service() {

    companion object {
        const val ACTION_START = "com.androserver.ACTION_START"
        const val ACTION_START_AT_BOOT = "com.androserver.ACTION_START_AT_BOOT"
        const val ACTION_STOP = "com.androserver.ACTION_STOP"
        const val EXTRA_PORT = "EXTRA_PORT"
        const val EXTRA_ROOTS = "EXTRA_ROOTS"
        const val EXTRA_USERNAME = "EXTRA_USERNAME"
        const val EXTRA_PASSWORD = "EXTRA_PASSWORD"
        const val EXTRA_CERTIFICATE = "EXTRA_CERTIFICATE"
        const val EXTRA_CERT_PASSWORD = "EXTRA_CERT_PASSWORD"
        const val NOTIFICATION_CHANNEL_ID = "andro_server_channel"
        const val NOTIFICATION_ID = 1001

        private val executor = Executors.newSingleThreadScheduledExecutor()
        private const val RECOVERY_INTERVAL_SECONDS = 5L
        private val serverState = MutableLiveData<ConnectionInfo?>()
        val connection: LiveData<ConnectionInfo?> = serverState
        private val startupError = MutableLiveData<Int?>()
        val lastStartupError: LiveData<Int?> = startupError
    }

    data class ConnectionInfo(val url: String, val username: String)

    private val mainHandler = Handler(Looper.getMainLooper())
    private var active = false
    @Volatile private var generation = 0
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    // Accessed only by the server executor.
    private var httpsServer: HttpsServer? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val port = intent.getIntExtra(EXTRA_PORT, 8080)
                val roots = intent.getStringArrayListExtra(EXTRA_ROOTS).orEmpty()
                val username = intent.getStringExtra(EXTRA_USERNAME).orEmpty()
                val password = intent.getStringExtra(EXTRA_PASSWORD).orEmpty()
                if (roots.isEmpty() || username.isBlank() || password.isBlank()) {
                    stopSelf()
                } else {
                    val certificate = intent.getStringExtra(EXTRA_CERTIFICATE)?.let(Uri::parse)
                    val certificatePassword = intent.getStringExtra(EXTRA_CERT_PASSWORD).orEmpty()
                    startServerService(port, roots, username, password, certificate, certificatePassword)
                }
            }
            ACTION_START_AT_BOOT -> {
                val saved = runCatching { SavedServerSettings.load(this) }.getOrNull()
                if (saved == null) {
                    startupError.value = R.string.auto_start_load_failed
                    stopSelf()
                } else {
                    startServerService(
                        saved.port, saved.roots, saved.username, saved.password,
                        saved.certificate, saved.certificatePassword, waitForLan = true
                    )
                }
            }
            ACTION_STOP -> {
                stopServerService()
            }
        }
        return START_NOT_STICKY
    }

    private fun startServerService(
        port: Int,
        roots: List<String>,
        username: String,
        password: String,
        certificate: Uri?,
        certificatePassword: String,
        waitForLan: Boolean = false
    ) {
        if (active) return
        active = true
        startupError.value = null
        val attempt = ++generation

        // 1. WakeLockの取得（CPU省電力による停止防止）
        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "AndroServer::WakeLock"
        ).apply {
            acquire(24 * 60 * 60 * 1000L) // 最大24時間
        }

        // 2. WifiLockの取得（Wi-Fi帯域の最大パフォーマンス維持）
        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            wifiManager.createWifiLock(
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY,
                "AndroServer::WifiLock"
            )
        } else {
            @Suppress("DEPRECATION")
            wifiManager.createWifiLock(
                WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                "AndroServer::WifiLock"
            )
        }.apply {
            acquire()
        }

        // 3. フォアグラウンドサービス開始
        startForeground(NOTIFICATION_ID, buildNotification(null, false))

        val authorization = "Basic " + Base64.encodeToString(
            "$username:$password".toByteArray(Charsets.UTF_8), Base64.NO_WRAP
        )
        val paths = roots.map { it.toByteArray(Charsets.UTF_8) }.toTypedArray()
        executor.execute {
            val startup = runCatching {
                val host = if (waitForLan) awaitLanAddress(attempt) else NetworkUtils.getLocalIpAddress()
                val address = host ?: if (waitForLan) throw LanUnavailable() else "127.0.0.1"
                val context = certificate?.let {
                    val secret = certificatePassword.toCharArray()
                    try {
                        val input = try {
                            contentResolver.openInputStream(it)
                                ?: throw java.io.FileNotFoundException()
                        } catch (e: Exception) {
                            throw UnreadableServerCertificate(e)
                        }
                        input.use { HttpsServer.loadIdentity(it, secret) }
                    } finally {
                        secret.fill('\u0000')
                    }
                }
                val listen = { ip: String -> startListeners(ip, port, paths, authorization, context) }
                val url = listen(address)
                monitorListeners(attempt, username, address, listen)
                url
            }
            if (startup.isFailure) stopListeners()
            mainHandler.post {
                if (attempt == generation) {
                    if (startup.isSuccess) {
                        val url = startup.getOrThrow()
                        publishConnection(url, username)
                    } else {
                        val error = startup.exceptionOrNull()
                        val message = when (error) {
                            is LanUnavailable -> R.string.auto_start_no_network
                            is UnreadableServerCertificate -> R.string.https_certificate_unreadable
                            is HttpsServer.IdentityException -> when (error.reason) {
                                HttpsServer.Reason.WRONG_PASSWORD -> R.string.https_certificate_wrong_password
                                HttpsServer.Reason.INVALID_FILE -> R.string.https_certificate_invalid
                                HttpsServer.Reason.MISSING_KEY -> R.string.https_certificate_no_key
                                HttpsServer.Reason.EXPIRED -> R.string.https_certificate_expired
                                HttpsServer.Reason.NOT_YET_VALID -> R.string.https_certificate_not_yet_valid
                            }
                            is java.security.GeneralSecurityException -> R.string.https_certificate_invalid
                            else -> R.string.server_start_failed
                        }
                        val reason = (error as? HttpsServer.IdentityException)?.reason?.name
                            ?: error?.javaClass?.simpleName
                        android.util.Log.w("AndroServer", "Server startup failed: $reason")
                        startupError.value = message
                        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
                        stopServerService()
                    }
                }
            }
        }
    }

    private fun startListeners(
        address: String,
        port: Int,
        paths: Array<ByteArray>,
        authorization: String,
        context: SSLContext?
    ): String {
        val scheme = if (context == null) "http" else "https"
        val defaultPort = if (context == null) 80 else 443
        val url = if (port == defaultPort) "$scheme://$address" else "$scheme://$address:$port"
        if (context == null) {
            check(NativeLib.startServer(port, paths, address, authorization))
        } else {
            check(NativeLib.startServer(0, paths, "127.0.0.1", authorization, url))
            httpsServer = HttpsServer.start(context, address, port, NativeLib.getServerPort())
        }
        return url
    }

    private fun monitorListeners(
        attempt: Int,
        username: String,
        boundAddress: String,
        listen: (String) -> String
    ) {
        executor.schedule({
            if (attempt != generation) return@schedule

            val address = NetworkUtils.getLocalIpAddress() ?: boundAddress
            val running = NativeLib.isServerRunning() && httpsServer?.isListening != false
            var nextAddress = boundAddress
            if (!running || address != boundAddress) {
                stopListeners()
                try {
                    val url = listen(address)
                    nextAddress = address
                    mainHandler.post {
                        if (attempt == generation) publishConnection(url, username)
                    }
                } catch (e: Exception) {
                    stopListeners()
                    android.util.Log.w("AndroServer", "Server recovery failed: ${e.javaClass.simpleName}")
                }
            } else {
                httpsServer?.closeIdleConnections()
            }
            monitorListeners(attempt, username, nextAddress, listen)
        }, RECOVERY_INTERVAL_SECONDS, TimeUnit.SECONDS)
    }

    private fun publishConnection(url: String, username: String) {
        serverState.value = ConnectionInfo(url, username)
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(url, true))
    }

    private fun stopServerService() {
        if (active) {
            active = false
            generation++
            executor.execute { stopListeners() }
        }
        wakeLock?.let {
            if (it.isHeld) it.release()
        }
        wakeLock = null

        wifiLock?.let {
            if (it.isHeld) it.release()
        }
        wifiLock = null

        serverState.value = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun awaitLanAddress(attempt: Int): String? {
        repeat(60) {
            if (attempt != generation) return null
            NetworkUtils.getLocalIpAddress()?.let { return it }
            Thread.sleep(1000)
        }
        return null
    }

    private class LanUnavailable : Exception()

    private class UnreadableServerCertificate(cause: Exception) : Exception(cause)

    private fun stopListeners() {
        httpsServer?.close()
        httpsServer = null
        NativeLib.stopServer()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                getString(R.string.notification_title),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = getString(R.string.notification_text)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(serverUrl: String?, running: Boolean): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = Intent(this, ServerService::class.java).apply {
            action = ACTION_STOP
        }
        val stopPendingIntent = PendingIntent.getService(
            this,
            1,
            stopIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle(getString(if (running) R.string.server_running else R.string.server_starting))
            .setContentText(serverUrl ?: getString(R.string.server_starting))
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentIntent(pendingIntent)
            .addAction(android.R.drawable.ic_media_pause, getString(R.string.stop_server), stopPendingIntent)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        stopServerService()
        super.onDestroy()
    }
}

