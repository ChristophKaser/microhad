package io.github.microhad

import android.Manifest
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.lifecycle.lifecycleScope
import androidx.work.WorkManager
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.*
import kotlinx.coroutines.tasks.await
import io.github.microhad.matter.MatterActivity

class MainActivity : AppCompatActivity() {
    private lateinit var store: GatewayStore
    private lateinit var content: LinearLayout
    private lateinit var status: TextView
    private var discovery: GatewayDiscovery? = null
    private var busy = false
    private val permission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { PushRegistration.schedule(this); home() }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState); store = GatewayStore(this); home()
        WorkManager.getInstance(this).getWorkInfosForUniqueWorkLiveData("gateway-push-registration").observe(this) { work ->
            if (!busy && store.base.isNotEmpty() && work.any { it.state.isFinished }) home()
        }
    }
    override fun onResume() { super.onResume(); if (::store.isInitialized && store.base.isNotEmpty() && !busy) home() }
    override fun onStop() { discovery?.close(); discovery = null; super.onStop() }
    private fun screen(title: String) {
        discovery?.close(); discovery = null
        content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 24, 24, 24) }
        val scroll = ScrollView(this).apply { addView(content) }
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom); insets
        }
        setContentView(scroll)
        content.addView(TextView(this).apply { text = title; textSize = 26f })
    }
    private fun text(value: String) = TextView(this).apply { text = value; setPadding(0, 16, 0, 16); content.addView(this) }
    private fun button(label: String, action: () -> Unit) = Button(this).apply {
        text = label; content.addView(this); setOnClickListener { if (!busy) action() }
    }
    private fun home() {
        if (store.resetPending) {
            screen("Gateway disconnected")
            text("Close and reopen the app to configure a gateway. If the previous gateway was unreachable, remove this phone from its Push clients page.")
            busy = false
            button("Close app") { finishAffinity(); android.os.Process.killProcess(android.os.Process.myPid()) }
            return
        }
        if (store.base.isEmpty()) { setup(); return }
        screen("MicroHAD Companion")
        text("${store.config?.name ?: "Gateway"}\n${store.base}")
        status = text("Connection: not checked\nMatter: ${if (store.config?.matter == true) "available" else "disabled"}\nPush: ${if (store.config?.firebase == null) "not configured on gateway" else store.status}\nNotifications: ${if (NotificationManagerCompat.from(this).areNotificationsEnabled()) "allowed" else "blocked in Android settings"}")
        button("Check gateway / retry push registration") {
            busy = true
            lifecycleScope.launch {
                try {
                    val config = GatewayConfig.parse(GatewayApi(store.base, store.fingerprint).config())
                    if (config.firebase != store.config?.firebase) status.text = "Firebase settings changed. Change gateway and reconnect to load the new settings."
                    else {
                        PushRegistration.schedule(this@MainActivity)
                        status.text = "Connection: connected\nMatter: ${if (config.matter) "available" else "disabled"}\nPush: ${if (config.firebase == null) "not configured" else "registration scheduled"}"
                    }
                } catch (_: Exception) { status.text = "Connection failed. Check the address, network, and gateway certificate." }
                finally { busy = false }
            }
        }
        button("Commission Matter device") { startActivity(Intent(this, MatterActivity::class.java)) }
        button("Open gateway dashboard") {
            runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(store.base))) }.onFailure { status.text = "No browser installed. Open ${store.base} in a browser." }
        }
        button("Enable notifications / Android settings") {
            startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, packageName))
        }
        button("Change gateway / reset") {
            AlertDialog.Builder(this).setTitle("Disconnect this gateway?")
                .setMessage("This removes local settings and notifications and attempts to remove this phone from the gateway. The app will close; reopen it to configure a gateway.")
                .setNegativeButton("Cancel", null).setPositiveButton("Disconnect") { _, _ -> reset() }.show()
        }
    }
    private fun setup() {
        screen("Connect to your gateway")
        text("Enter a gateway address or discover gateways on this LAN.")
        val address = EditText(this).apply {
            hint = "http(s)://gateway[:port]"; inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI
            isSingleLine = true; content.addView(this)
        }
        status = text("")
        button("Connect") { connect(address.text.toString(), "") }
        button("Discover on LAN") {
            discovery?.close()
            val results = mutableSetOf<String>()
            discovery = GatewayDiscovery(this, { name, base ->
                if (results.add(base)) button("$name — $base") { address.setText(base) }
            }, { status.text = it }).also { it.start() }
        }
        button("Allow an untrusted HTTPS certificate…") {
            val base = runCatching { GatewayTransport.normalize(address.text.toString()) }.getOrElse { status.text = it.message; return@button }
            busy = true
            lifecycleScope.launch {
                try {
                    val pin = withContext(Dispatchers.IO) { GatewayTransport.inspectCertificate(base) }
                    AlertDialog.Builder(this@MainActivity).setTitle("Trust this gateway certificate?")
                        .setMessage("$base\n\nSHA-256:\n${pin.chunked(2).joinToString(":")}\n\nVerify this fingerprint through a trusted source. The exception permits an expired certificate or hostname mismatch only for this exact certificate.")
                        .setNegativeButton("Cancel", null).setPositiveButton("Trust and connect") { _, _ -> connect(base, pin) }.show()
                } catch (_: Exception) { status.text = "Could not inspect the HTTPS certificate. Check the address and network." }
                finally { busy = false }
            }
        }
    }
    private fun connect(input: String, pin: String) {
        val base = runCatching { GatewayTransport.normalize(input) }.getOrElse { status.text = it.message; return }
        busy = true; status.text = "Connecting…"; discovery?.close()
        lifecycleScope.launch {
            try {
                val raw = GatewayApi(base, pin).config(); GatewayConfig.parse(raw)
                store.save(base, pin, raw)
                runCatching { CompanionApplication.initializeFirebase(this@MainActivity) }.onFailure { store.status("Firebase initialization failed; check gateway settings") }
                PushRegistration.schedule(this@MainActivity)
                busy = false; home()
                if (store.config?.firebase != null && Build.VERSION.SDK_INT >= 33 &&
                    ContextCompat.checkSelfPermission(this@MainActivity, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                    permission.launch(Manifest.permission.POST_NOTIFICATIONS)
            } catch (_: Exception) { status.text = "Connection failed. Check the gateway address and configuration. For untrusted HTTPS, use the certificate option." }
            finally { busy = false }
        }
    }
    private fun reset() {
        busy = true; status.text = "Disconnecting…"
        val base = store.base; val pin = store.fingerprint; val id = store.installationId()
        store.clear() // Invalidate queued workers before asynchronous cleanup.
        WorkManager.getInstance(this).cancelUniqueWork("gateway-push-registration")
        lifecycleScope.launch {
            val removed = runCatching { GatewayApi(base, pin).remove(id) }.isSuccess
            runCatching { withTimeout(10000) {
                if (FirebaseApp.getApps(this@MainActivity).isNotEmpty()) FirebaseMessaging.getInstance().deleteToken().await()
            } }
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).cancelAll()
            if (isFinishing || isDestroyed) return@launch
            AlertDialog.Builder(this@MainActivity).setTitle("Gateway disconnected")
                .setMessage(if (removed) "Reopen the app to configure a gateway." else "Reopen the app to configure a gateway. The old gateway could not be updated; remove this phone from its Push clients page if needed.")
                .setCancelable(false).setPositiveButton("Close app") { _, _ -> finishAffinity(); android.os.Process.killProcess(android.os.Process.myPid()) }.show()
        }
    }
}
