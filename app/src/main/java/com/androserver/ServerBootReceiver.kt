package com.androserver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat

class ServerBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED || !SavedServerSettings.exists(context)) {
            return
        }
        val service = Intent(context, ServerService::class.java).apply {
            action = ServerService.ACTION_START_AT_BOOT
        }
        try {
            ContextCompat.startForegroundService(context, service)
        } catch (e: IllegalStateException) {
            Log.w("AndroServer", "Automatic startup was rejected: ${e.javaClass.simpleName}")
        } catch (e: SecurityException) {
            Log.w("AndroServer", "Automatic startup was rejected: ${e.javaClass.simpleName}")
        }
    }
}
