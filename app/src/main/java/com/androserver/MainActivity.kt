package com.androserver

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.storage.StorageManager
import android.provider.Settings
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import com.androserver.databinding.ActivityMainBinding
import org.json.JSONArray
import java.io.File

class MainActivity : AppCompatActivity() {
    class CredentialDraft : ViewModel() {
        var password = ""
    }

    private lateinit var binding: ActivityMainBinding
    private val settings by lazy { getSharedPreferences("server_settings", MODE_PRIVATE) }
    private val sharedFolders = mutableListOf<File>()
    private var certificate: Uri? = null

    private val certificatePicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            certificate = uri
            if (runCatching {
                    contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }.isSuccess
            ) {
                settings.edit().putString("certificate", uri.toString()).apply()
            }
            updateCertificateSettings()
        }
    }

    private val storageSettingsLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (hasStorageAccess() && ServerService.connection.value == null) chooseStorage()
    }

    private val storagePermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted && ServerService.connection.value == null) chooseStorage()
    }

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted ->
        if (!isGranted) {
            Toast.makeText(this, "通知権限がない場合、バックグラウンドでの安定稼働に影響する場合があります", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val savedFolders = JSONArray(settings.getString("folders", "[]"))
        for (index in 0 until savedFolders.length()) {
            sharedFolders.add(File(savedFolders.getString(index)))
        }
        binding.etUsername.setText(settings.getString("username", "androserver"))
        val draft = ViewModelProvider(this)[CredentialDraft::class.java]
        val savedStartup = runCatching { SavedServerSettings.load(this) }.getOrNull()
        binding.switchAutoStart.isChecked = SavedServerSettings.exists(this)
        savedStartup?.let {
            binding.etPort.setText(it.port.toString())
            binding.etCertificatePassword.setText(it.certificatePassword)
        }
        binding.etPassword.setText(draft.password.ifEmpty { savedStartup?.password.orEmpty() })
        binding.etPassword.doAfterTextChanged { draft.password = it.toString() }
        certificate = settings.getString("certificate", null)?.let(Uri::parse)
        binding.switchHttps.isChecked = settings.getBoolean("https", true)
        setupListeners()
        updateCertificateSettings()
        ServerService.connection.observe(this) { updateUiState() }
        ServerService.lastStartupError.observe(this) { updateUiState() }
        checkPermissions()
    }

    override fun onResume() {
        super.onResume()
        updateUiState()
    }

    private fun setupListeners() {
        binding.switchAutoStart.setOnCheckedChangeListener { _, enabled ->
            if (!enabled) SavedServerSettings.clear(this)
        }

        binding.btnSelectCertificate.setOnClickListener { certificatePicker.launch(arrayOf("*/*")) }
        binding.switchHttps.setOnCheckedChangeListener { _, enabled ->
            settings.edit().putBoolean("https", enabled).apply()
            updateCertificateSettings()
        }

        binding.btnAddFolder.setOnClickListener {
            if (hasStorageAccess()) chooseStorage() else requestStorageAccess()
        }

        binding.btnToggleServer.setOnClickListener {
            if (ServerService.connection.value != null) {
                stopServer()
            } else {
                startServer()
            }
        }

        binding.btnCopyUrl.setOnClickListener {
            val url = binding.tvServerUrl.text.toString()
            val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            val clip = ClipData.newPlainText("AndroServer URL", url)
            clipboard.setPrimaryClip(clip)
            Toast.makeText(this, "URLをクリップボードにコピーしました", Toast.LENGTH_SHORT).show()
        }
    }

    private fun updateCertificateSettings() {
        binding.httpsSettings.visibility = if (binding.switchHttps.isChecked) View.VISIBLE else View.GONE
        binding.tvCertificate.setText(if (certificate == null) R.string.no_certificate else R.string.certificate_selected)
    }

    private fun checkPermissions() {
        // 1. Android 13+ 通知権限
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }

    private fun hasStorageAccess(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED
        }

    private fun requestStorageAccess() {
        Toast.makeText(this, R.string.storage_permission_required, Toast.LENGTH_LONG).show()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val appSettings = Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.fromParts("package", packageName, null)
            )
            try {
                storageSettingsLauncher.launch(appSettings)
            } catch (_: android.content.ActivityNotFoundException) {
                storageSettingsLauncher.launch(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            storagePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    private fun chooseStorage() {
        val storage = getSystemService(StorageManager::class.java)
        @Suppress("DEPRECATION")
        val primary = Environment.getExternalStorageDirectory()
        val volumes = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            storage.storageVolumes.mapNotNull { volume ->
                volume.directory?.let { volume.getDescription(this) to it }
            }
        } else {
            emptyList()
        }
        val choices = volumes.ifEmpty { listOf(primary.absolutePath to primary) }
        if (choices.size == 1) {
            val root = choices.first().second
            browseFolder(root, root)
        } else {
            AlertDialog.Builder(this)
                .setTitle(R.string.select_storage)
                .setItems(choices.map { it.first }.toTypedArray()) { _, index ->
                    val root = choices[index].second
                    browseFolder(root, root)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun browseFolder(volume: File, folder: File) {
        val children = folder.listFiles { child ->
            child.isDirectory && !java.nio.file.Files.isSymbolicLink(child.toPath())
        }
        if (children == null) {
            Toast.makeText(this, R.string.folder_not_accessible, Toast.LENGTH_SHORT).show()
            return
        }
        val folders = children.sortedBy { it.name.lowercase(java.util.Locale.ROOT) }.toMutableList()
        if (folder != volume) folders.add(0, requireNotNull(folder.parentFile))
        val labels = folders.map {
            if (it == folder.parentFile) getString(R.string.parent_folder) else it.name
        }

        AlertDialog.Builder(this)
            .setTitle(folder.absolutePath)
            .setItems(labels.toTypedArray()) { _, index -> browseFolder(volume, folders[index]) }
            .setPositiveButton(R.string.select_shared_folder) { _, _ ->
                val selected = runCatching { folder.canonicalFile }.getOrNull()
                when {
                    selected == null || !selected.canRead() || !selected.canWrite() -> {
                        Toast.makeText(this, R.string.folder_not_accessible, Toast.LENGTH_SHORT).show()
                    }
                    selected in sharedFolders -> {
                        Toast.makeText(this, R.string.folder_already_selected, Toast.LENGTH_SHORT).show()
                    }
                    else -> {
                        sharedFolders.add(selected)
                        saveFolders()
                        renderFolders()
                    }
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun saveFolders() {
        settings.edit().putString("folders", JSONArray(sharedFolders.map { it.path }).toString()).apply()
    }

    private fun renderFolders() {
        val list = binding.sharedFolderList
        list.removeAllViews()
        for (folder in sharedFolders) {
            val row = LinearLayout(this).apply { gravity = android.view.Gravity.CENTER_VERTICAL }
            val label = TextView(this).apply { text = folder.path }
            row.addView(label, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(Button(this).apply {
                setText(R.string.remove_shared_folder)
                isEnabled = ServerService.connection.value == null
                setOnClickListener {
                    sharedFolders.remove(folder)
                    saveFolders()
                    renderFolders()
                }
            })
            list.addView(row)
        }
    }

    private fun startServer() {
        val portStr = binding.etPort.text.toString().trim()
        val port = portStr.ifEmpty { "8080" }.toIntOrNull()
        if (port == null || port !in 1..65535) {
            binding.etPort.error = getString(R.string.invalid_port)
            return
        }
        val username = binding.etUsername.text.toString()
        val password = binding.etPassword.text.toString()
        if (username.isBlank() || username.any { it.isISOControl() || it == ':' }) {
            binding.etUsername.error = getString(R.string.invalid_username)
            binding.etUsername.requestFocus()
            return
        }
        if (password.isBlank() || password.any { it.isISOControl() }) {
            binding.etPassword.error = getString(R.string.invalid_password)
            binding.etPassword.requestFocus()
            return
        }
        if (sharedFolders.isEmpty()) {
            Toast.makeText(this, R.string.select_folder_first, Toast.LENGTH_SHORT).show()
            return
        }
        if (!hasStorageAccess()) {
            requestStorageAccess()
            return
        }
        val tlsCertificate = if (binding.switchHttps.isChecked) certificate else null
        if (binding.switchHttps.isChecked && tlsCertificate == null) {
            Toast.makeText(this, R.string.no_certificate, Toast.LENGTH_SHORT).show()
            certificatePicker.launch(arrayOf("*/*"))
            return
        }
        settings.edit().putString("username", username).apply()

        val configuration = ServerSettings(
            port, sharedFolders.map { it.path }, username, password, tlsCertificate,
            if (tlsCertificate == null) "" else binding.etCertificatePassword.text.toString()
        )
        if (binding.switchAutoStart.isChecked) {
            if (tlsCertificate != null && contentResolver.persistedUriPermissions.none {
                    it.uri == tlsCertificate && it.isReadPermission
                }) {
                Toast.makeText(this, R.string.auto_start_certificate_permission, Toast.LENGTH_LONG).show()
                return
            }
            if (runCatching { SavedServerSettings.save(this, configuration) }.isFailure) {
                Toast.makeText(this, R.string.auto_start_save_failed, Toast.LENGTH_LONG).show()
                return
            }
        }
        ContextCompat.startForegroundService(this, configuration.startIntent(this))
    }

    private fun stopServer() {
        val serviceIntent = Intent(this, ServerService::class.java).apply {
            action = ServerService.ACTION_STOP
        }
        startService(serviceIntent)
    }

    private fun updateUiState() {
        val connection = ServerService.connection.value
        if (connection != null) {
            binding.tvStatus.text = getString(R.string.server_running)
            binding.tvStatus.setTextColor(ContextCompat.getColor(this, R.color.green_500))
            binding.ivStatus.setColorFilter(ContextCompat.getColor(this, R.color.green_500))
            binding.btnToggleServer.text = getString(R.string.stop_server)
            binding.btnToggleServer.setBackgroundColor(ContextCompat.getColor(this, R.color.red_500))

            binding.tvServerUrl.text = connection.url
            binding.tvServerCredentials.text = getString(
                R.string.server_credentials, connection.username
            )
            binding.tvServerCredentials.visibility = View.VISIBLE
            binding.tvServerUrl.visibility = View.VISIBLE
            binding.btnCopyUrl.visibility = View.VISIBLE

            binding.etPort.isEnabled = false
        } else {
            binding.tvStatus.text = getString(
                ServerService.lastStartupError.value ?: R.string.server_stopped
            )
            binding.tvStatus.setTextColor(ContextCompat.getColor(this, R.color.black))
            binding.ivStatus.setColorFilter(ContextCompat.getColor(this, R.color.red_500))
            binding.btnToggleServer.text = getString(R.string.start_server)
            binding.btnToggleServer.setBackgroundColor(ContextCompat.getColor(this, R.color.purple_500))

            binding.tvServerCredentials.text = ""
            binding.tvServerCredentials.visibility = View.GONE
            binding.tvServerUrl.visibility = View.GONE
            binding.btnCopyUrl.visibility = View.GONE

            binding.etPort.isEnabled = true
        }
        val editable = connection == null
        binding.etUsername.isEnabled = editable
        binding.etPassword.isEnabled = editable
        binding.btnAddFolder.isEnabled = editable
        binding.switchHttps.isEnabled = editable
        binding.btnSelectCertificate.isEnabled = editable
        binding.etCertificatePassword.isEnabled = editable
        renderFolders()
    }
}
