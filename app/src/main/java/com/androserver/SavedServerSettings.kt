package com.androserver

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

internal class ServerSettings(
    val port: Int,
    val roots: List<String>,
    val username: String,
    val password: String,
    val certificate: Uri?,
    val certificatePassword: String
) {
    fun startIntent(context: Context) = Intent(context, ServerService::class.java).apply {
        action = ServerService.ACTION_START
        putExtra(ServerService.EXTRA_PORT, port)
        putStringArrayListExtra(ServerService.EXTRA_ROOTS, ArrayList(roots))
        putExtra(ServerService.EXTRA_USERNAME, username)
        putExtra(ServerService.EXTRA_PASSWORD, password)
        certificate?.let {
            putExtra(ServerService.EXTRA_CERTIFICATE, it.toString())
            putExtra(ServerService.EXTRA_CERT_PASSWORD, certificatePassword)
        }
    }
}

internal object SavedServerSettings {
    private const val KEY_ALIAS = "androserver-startup-settings"
    private const val NONCE_SIZE = 12

    private fun file(context: Context) = AtomicFile(File(context.noBackupFilesDir, "server-startup.bin"))

    fun exists(context: Context): Boolean = file(context).baseFile.exists()

    @Synchronized
    fun save(context: Context, settings: ServerSettings) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key(create = true))
        check(cipher.iv.size == NONCE_SIZE)
        val payload = JSONObject()
            .put("port", settings.port)
            .put("roots", JSONArray(settings.roots))
            .put("username", settings.username)
            .put("password", settings.password)
            .put("certificate", settings.certificate?.toString() ?: "")
            .put("certificatePassword", settings.certificatePassword)
            .toString()
            .toByteArray(Charsets.UTF_8)
        val encrypted = try {
            cipher.iv + cipher.doFinal(payload)
        } finally {
            payload.fill(0)
        }
        val file = file(context)
        val output = file.startWrite()
        try {
            output.write(encrypted)
            file.finishWrite(output)
        } catch (e: Exception) {
            file.failWrite(output)
            throw e
        }
    }

    @Synchronized
    fun load(context: Context): ServerSettings? {
        val file = file(context)
        if (!file.baseFile.exists()) return null
        val encrypted = file.readFully()
        require(encrypted.size > NONCE_SIZE + 16)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        val nonce = GCMParameterSpec(128, encrypted, 0, NONCE_SIZE)
        cipher.init(Cipher.DECRYPT_MODE, key(create = false), nonce)
        val payload = cipher.doFinal(encrypted, NONCE_SIZE, encrypted.size - NONCE_SIZE)
        val json = try {
            JSONObject(payload.toString(Charsets.UTF_8))
        } finally {
            payload.fill(0)
        }
        val roots = json.getJSONArray("roots")
        return ServerSettings(
            json.getInt("port"), List(roots.length()) { roots.getString(it) },
            json.getString("username"), json.getString("password"),
            json.getString("certificate").takeIf { it.isNotEmpty() }?.let(Uri::parse),
            json.getString("certificatePassword")
        )
    }

    @Synchronized
    fun clear(context: Context) {
        file(context).delete()
    }

    private fun key(create: Boolean): SecretKey {
        val keystore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val existing = keystore.getKey(KEY_ALIAS, null) as? SecretKey
        if (existing != null) return existing
        check(create) { "Startup settings key is missing" }

        val options = KeyGenParameterSpec.Builder(
            KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        ).setKeySize(256)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build()
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(options)
            generateKey()
        }
    }
}
