package io.github.microhad

import android.Manifest
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.work.WorkManager
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessaging
import io.github.microhad.matter.MatterActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

class MainActivity : ComponentActivity() {
    private enum class Destination { DASHBOARD, SETUP, DISCONNECTED }
    private data class Gateway(val name: String, val base: String)

    private lateinit var store: GatewayStore
    private var destination by mutableStateOf(Destination.SETUP)
    private var status by mutableStateOf("")
    private var address by mutableStateOf("")
    private var setupFingerprintBase = ""
    private var setupFingerprint = ""
    private var loginOpen by mutableStateOf(false)
    private var loginRole by mutableStateOf("administrator")
    private var loginPassword by mutableStateOf("")
    private var loginError by mutableStateOf("")
    private var busy by mutableStateOf(false)
    private var confirmDisconnect by mutableStateOf(false)
    private var certificateProposal by mutableStateOf<String?>(null)
    private val discoveredGateways = mutableStateListOf<Gateway>()
    private var discovery: GatewayDiscovery? = null

    private val permission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        PushRegistration.schedule(this)
        home()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = GatewayStore(this)
        setContent { CompanionApp() }
        home()
        WorkManager.getInstance(this).getWorkInfosForUniqueWorkLiveData("gateway-push-registration")
            .observe(this) { work -> if (!busy && store.base.isNotEmpty() && work.any { it.state.isFinished }) home() }
    }

    override fun onResume() {
        super.onResume()
        if (::store.isInitialized && store.base.isNotEmpty() && !busy) home()
    }

    override fun onStop() {
        discovery?.close()
        discovery = null
        super.onStop()
    }

    private fun home() {
        discovery?.close()
        discovery = null
        discoveredGateways.clear()
        when {
            store.resetPending -> { destination = Destination.DISCONNECTED; status = "" }
            store.base.isEmpty() -> { destination = Destination.SETUP; status = "" }
            else -> { destination = Destination.DASHBOARD; status = gatewayStatus() }
        }
    }

    private fun gatewayStatus(config: GatewayConfig? = store.config) = buildString {
        append("Connection: ready")
        append("\nMatter: ").append(if (config?.matter == true) "available" else "disabled")
        append("\nPush: ").append(if (config?.firebase == null) "not configured" else store.status)
        append("\nNotifications: ").append(if (NotificationManagerCompat.from(this@MainActivity).areNotificationsEnabled()) "allowed" else "blocked in Android settings")
    }

    private fun discover() {
        discovery?.close()
        discoveredGateways.clear()
        status = "Looking for gateways on this Wi-Fi network…"
        discovery = GatewayDiscovery(this, { name, base ->
            if (discoveredGateways.none { it.base == base }) discoveredGateways += Gateway(name, base)
        }, { status = it }).also { it.start() }
    }

    private fun inspectCertificate() {
        val base = runCatching { GatewayTransport.normalize(address) }.getOrElse {
            status = it.message ?: "Enter a valid gateway address."
            return
        }
        busy = true
        status = "Checking certificate…"
        lifecycleScope.launch {
            try { certificateProposal = withContext(Dispatchers.IO) { GatewayTransport.inspectCertificate(base) } }
            catch (_: Exception) { status = "Could not inspect the HTTPS certificate. Check the address and network." }
            finally { busy = false }
        }
    }

    private fun connect(input: String, pin: String) {
        val base = runCatching { GatewayTransport.normalize(input) }.getOrElse {
            status = it.message ?: "Enter a valid gateway address."
            return
        }
        if (store.base.isEmpty()) { setupFingerprintBase = base; setupFingerprint = pin }
        busy = true
        status = "Connecting to gateway…"
        discovery?.close()
        lifecycleScope.launch {
            try {
                val raw = GatewayApi(base, pin).config()
                GatewayConfig.parse(raw)
                store.save(base, pin, raw)
                runCatching { CompanionApplication.initializeFirebase(this@MainActivity) }
                    .onFailure { store.status("Firebase initialization failed; check gateway settings") }
                PushRegistration.schedule(this@MainActivity)
                busy = false
                home()
                if (store.config?.firebase != null && Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(this@MainActivity, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) permission.launch(Manifest.permission.POST_NOTIFICATIONS)
            } catch (error: Exception) {
                if (error is GatewayHttpException && error.code in listOf(401, 403)) {
                    status = "Login required. Use Log in to gateway."; loginOpen = true
                } else status = "Connection failed. Check the address and configuration. For an untrusted HTTPS certificate, use the certificate option."
            } finally { busy = false }
        }
    }

    private fun checkGateway() {
        busy = true
        status = "Checking gateway…"
        lifecycleScope.launch {
            try {
                val config = GatewayConfig.parse(GatewayApi(store.base, store.fingerprint).config())
                status = if (config.firebase != store.config?.firebase) "Firebase settings changed. Disconnect and reconnect to load the new settings."
                else { PushRegistration.schedule(this@MainActivity); gatewayStatus(config).replace("Connection: ready", "Connection: connected") }
            } catch (_: Exception) { status = "Connection failed. Check the address, network, and gateway certificate." }
            finally { busy = false }
        }
    }

    private fun reset() {
        busy = true
        status = "Disconnecting…"
        val base = store.base
        val pin = store.fingerprint
        val id = store.installationId()
        store.clear()
        WorkManager.getInstance(this).cancelUniqueWork("gateway-push-registration")
        lifecycleScope.launch {
            runCatching { GatewayApi(base, pin).remove(id) }
            runCatching { withTimeout(10_000) { if (FirebaseApp.getApps(this@MainActivity).isNotEmpty()) FirebaseMessaging.getInstance().deleteToken().await() } }
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).cancelAll()
            busy = false
            destination = Destination.DISCONNECTED
        }
    }

    @Composable
    private fun CompanionApp() {
        MaterialTheme(colorScheme = lightColorScheme(primary = Color(0xFF006875), onPrimary = Color.White, primaryContainer = Color(0xFFB9F1F5), surface = Color(0xFFF8FAFA), surfaceVariant = Color(0xFFE2E8E8), onSurface = Color(0xFF171D1E), onSurfaceVariant = Color(0xFF3F494A), outline = Color(0xFF6F797A), error = Color(0xFFB3261E))) {
            Surface(Modifier.fillMaxSize()) {
                when (destination) {
                    Destination.DASHBOARD -> GatewayHome(store.base, store.fingerprint, lifecycle) { Dashboard() }
                    Destination.SETUP -> Setup()
                    Destination.DISCONNECTED -> Disconnected()
                }
            }
            LoginDialog()
            CertificateDialog()
            DisconnectDialog()
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun Page(title: String, content: @Composable ColumnScope.() -> Unit) {
        Column(Modifier.fillMaxSize()) {
            TopAppBar(title = { Text(title, fontWeight = FontWeight.SemiBold) }, actions = { TextButton({ startActivity(Intent(this@MainActivity, LicensesActivity::class.java)) }) { Text("Licenses") } }, colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface))
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(16.dp), content = content)
        }
    }

    @Composable
    private fun Header(title: String, subtitle: String) {
        Row(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.primary, RoundedCornerShape(28.dp)).padding(24.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(painterResource(R.drawable.ic_gateway), null, tint = Color.White, modifier = Modifier.size(42.dp))
            Spacer(Modifier.width(16.dp))
            Column {
                Text(title, color = Color.White, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(4.dp))
                Text(subtitle, color = MaterialTheme.colorScheme.primaryContainer, style = MaterialTheme.typography.bodyMedium)
            }
        }
    }

    @Composable
    private fun StatusCard(value: String) {
        if (value.isBlank()) return
        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant), shape = RoundedCornerShape(20.dp)) {
            Text(value, Modifier.padding(20.dp), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }

    @Composable
    private fun Action(label: String, primary: Boolean = false, action: () -> Unit) {
        if (primary) Button(onClick = action, enabled = !busy, modifier = Modifier.fillMaxWidth().height(52.dp), shape = RoundedCornerShape(26.dp)) { Text(label) }
        else OutlinedButton(onClick = action, enabled = !busy, modifier = Modifier.fillMaxWidth().height(52.dp), shape = RoundedCornerShape(26.dp)) { Text(label) }
    }

    @Composable
    private fun Dashboard() = Page(stringResource(R.string.companion_title)) {
        Action("Log in") { loginOpen = true }
        if (GatewayAuth.token(store.base).isNotEmpty()) Action("Log out") {
            lifecycleScope.launch { runCatching { GatewayAuth.logout() }.onSuccess { status = "Logged out" }.onFailure { status = "Unable to log out" } }
        }
        Header(store.config?.name ?: "Your gateway", store.base)
        StatusCard(status)
        Text(stringResource(R.string.manage_gateway), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        Action(stringResource(R.string.check_gateway), true, ::checkGateway)
        Action(stringResource(R.string.commission_matter)) { startActivity(Intent(this@MainActivity, MatterActivity::class.java)) }
        Action(stringResource(R.string.open_dashboard)) { runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(store.base))) }.onFailure { status = "No browser is installed. Open ${store.base} in a browser." } }
        Action(stringResource(R.string.notification_settings)) { startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, packageName)) }
        TextButton({ confirmDisconnect = true }, Modifier.align(Alignment.CenterHorizontally)) { Text(stringResource(R.string.disconnect_gateway), color = MaterialTheme.colorScheme.error) }
        Spacer(Modifier.height(12.dp))
    }

    @Composable
    private fun Setup() = Page(stringResource(R.string.connect_title)) {
        Action("Log in to gateway") { loginOpen = true }
        Header(stringResource(R.string.welcome_title), stringResource(R.string.welcome_subtitle))
        Text(stringResource(R.string.connect_intro), style = MaterialTheme.typography.bodyLarge)
        OutlinedTextField(address, { address = it }, Modifier.fillMaxWidth(), label = { Text(stringResource(R.string.gateway_address)) }, placeholder = { Text("https://gateway[:port]") }, singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri), enabled = !busy)
        Action(stringResource(R.string.connect), true) { connect(address, "") }
        Action(stringResource(R.string.find_gateways), action = ::discover)
        discoveredGateways.forEach { gateway ->
            Card(Modifier.fillMaxWidth().clickable { address = gateway.base }, colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant), shape = RoundedCornerShape(16.dp)) {
                Text(gateway.name, Modifier.padding(start = 18.dp, top = 14.dp, end = 18.dp), fontWeight = FontWeight.SemiBold)
                Text(gateway.base, Modifier.padding(start = 18.dp, end = 18.dp, bottom = 14.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        StatusCard(status)
        TextButton(::inspectCertificate, enabled = !busy, modifier = Modifier.align(Alignment.CenterHorizontally)) { Text(stringResource(R.string.certificate)) }
        Spacer(Modifier.height(12.dp))
    }

    @Composable
    private fun Disconnected() = Page(stringResource(R.string.disconnected_title)) {
        Header(stringResource(R.string.disconnected_heading), stringResource(R.string.disconnected_subtitle))
        Text(stringResource(R.string.disconnected_intro))
        Action(stringResource(R.string.close_app), true) { finishAndRemoveTask() }
    }

    @Composable
    private fun LoginDialog() {
        if (!loginOpen) return
        AlertDialog(onDismissRequest = { if (!busy) { loginOpen = false; loginPassword = "" } },
            title = { Text("Log in to gateway") },
            text = { Column {
                listOf("guest", "user", "administrator").forEach { role ->
                    Row(verticalAlignment = Alignment.CenterVertically) { RadioButton(selected = loginRole == role, onClick = { loginRole = role }); Text(role) }
                }
                OutlinedTextField(value = loginPassword, onValueChange = { loginPassword = it }, label = { Text("Password") }, visualTransformation = androidx.compose.ui.text.input.PasswordVisualTransformation(), singleLine = true)
                if (loginError.isNotEmpty()) Text(loginError, color = MaterialTheme.colorScheme.error)
            } },
            confirmButton = { TextButton(enabled = !busy, onClick = {
                busy = true; loginError = ""
                lifecycleScope.launch {
                    try {
                        val base = if (store.base.isEmpty()) GatewayTransport.normalize(address) else store.base
                        val pin = if (store.base.isNotEmpty()) store.fingerprint else if (setupFingerprintBase == base) setupFingerprint else ""
                        withContext(Dispatchers.IO) { GatewayAuth.login(base, pin, loginRole, loginPassword) }
                        loginPassword = ""; loginOpen = false; status = "Logged in as $loginRole"
                        if (store.base.isEmpty()) connect(base, pin) else PushRegistration.schedule(this@MainActivity)
                    } catch (_: Exception) { loginError = "Login failed. Check gateway, access level and password." }
                    finally { busy = false }
                }
            }) { Text(if (busy) "Please wait…" else "Log in") } },
            dismissButton = { TextButton(enabled = !busy, onClick = { loginOpen = false; loginPassword = ""; loginError = "" }) { Text("Cancel") } })
    }

    @Composable
    private fun CertificateDialog() {
        val pin = certificateProposal ?: return
        AlertDialog(onDismissRequest = { certificateProposal = null }, title = { Text("Trust this certificate?") }, text = { Text("Verify this SHA-256 fingerprint through a trusted source before continuing.\n\n${pin.chunked(2).joinToString(":")}") }, confirmButton = { TextButton({ certificateProposal = null; connect(address, pin) }) { Text("Trust and connect") } }, dismissButton = { TextButton({ certificateProposal = null }) { Text("Cancel") } })
    }

    @Composable
    private fun DisconnectDialog() {
        if (!confirmDisconnect) return
        AlertDialog(onDismissRequest = { confirmDisconnect = false }, title = { Text("Disconnect this gateway?") }, text = { Text("This removes local settings and notifications, then asks the gateway to remove this phone. You can reconnect at any time.") }, confirmButton = { TextButton({ confirmDisconnect = false; reset() }, colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)) { Text("Disconnect") } }, dismissButton = { TextButton({ confirmDisconnect = false }) { Text("Cancel") } })
    }
}
