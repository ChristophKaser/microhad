package io.github.microhad

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun GatewayHome(base: String, pin: String, lifecycle: Lifecycle, management: @Composable () -> Unit) {
    val copy = homeCopy()
    val repository = remember(base, pin) { HomeRepository(GatewayApi(base, pin)) }
    var active by remember { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, _ -> active = lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    var tab by remember { mutableStateOf(0) }
    var snapshot by remember(repository) { mutableStateOf<HomeSnapshot?>(null) }
    var dashboard by remember(repository) { mutableStateOf<JSONObject?>(null) }
    var thread by remember(repository) { mutableStateOf<JSONObject?>(null) }
    var stats by remember(repository) { mutableStateOf<JSONObject?>(null) }
    var scripts by remember(repository) { mutableStateOf<List<JSONObject>>(emptyList()) }
    var scriptsAvailable by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var configError by remember { mutableStateOf("") }
    var overviewError by remember { mutableStateOf("") }
    var message by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var fresh by remember { mutableStateOf(false) }
    var filter by remember { mutableStateOf("All") }
    var details by remember { mutableStateOf<String?>(null) }
    var history by remember { mutableStateOf<String?>(null) }
    var refresh by remember { mutableIntStateOf(0) }
    val refreshMutex = remember { Mutex() }
    suspend fun refreshStatus() = refreshMutex.withLock {
        try { snapshot = repository.status(); error = ""; fresh = true }
        catch(e: CancellationException) { throw e }
        catch(e: Exception) { error = e.message ?: copy.label("Gateway unavailable"); fresh = false }
    }
    LaunchedEffect(repository, active, refresh) {
        if (!active) return@LaunchedEffect
        try { dashboard = repository.json("/api/dashboard/config"); configError = "" }
        catch(e: CancellationException) { throw e }
        catch(e: Exception) { configError = e.message ?: copy.label("Dashboard unavailable") }
    }
    LaunchedEffect(repository, active) {
        if (!active) { fresh = false; return@LaunchedEffect }
        while (true) { refreshStatus(); delay(3000) }
    }
    LaunchedEffect(repository, active) {
        if (!active) return@LaunchedEffect
        while(true) {
            try { scripts = repository.json("/api/automations").array("scripts").objects(); scriptsAvailable = true }
            catch(e: CancellationException) { throw e }
            catch(_: Exception) { scriptsAvailable = false }
            delay(3000)
        }
    }
    LaunchedEffect(repository, active) {
        if (!active) return@LaunchedEffect
        while(true) {
            val errors = mutableListOf<String>()
            try { thread = repository.json("/api/thread/status") } catch(e: CancellationException) { throw e } catch(e: Exception) { errors += e.message ?: copy.label("Matter status unavailable") }
            try { stats = repository.json("/api/controller-stats") } catch(e: CancellationException) { throw e } catch(e: Exception) { errors += e.message ?: copy.label("Resources unavailable") }
            overviewError = errors.joinToString("\n")
            delay(10000)
        }
    }
    // The action scope is cancelled on stop, including network calls and Matter job polling.
    if (!active) return
    val scope = rememberCoroutineScope()
    val tabScroll = remember(tab) { ScrollState(0) }
    val snackbar = remember { SnackbarHostState() }
    LaunchedEffect(message,error) {
        val notice = message.ifEmpty { if(error.isNotEmpty()) copy.format("Stale / unavailable: %1\$s",error) else "" }
        if(notice.isNotEmpty()) snackbar.showSnackbar(notice)
    }
    val enabled = snapshot != null && fresh && error.isEmpty() && !busy
    val action: (suspend () -> Unit) -> Unit = { operation ->
        if(enabled && !busy) {
            busy = true; message = ""
            scope.launch {
                try { operation(); message = copy.label("Request sent. Waiting for confirmed state."); refreshStatus() }
                catch(e: CancellationException) { throw e }
                catch(e: Exception) { message = e.message ?: copy.label("Command failed") }
                finally { busy = false }
            }
        }
    }
    val deviceAction: (HomeDevice, String, Map<String,String>) -> Unit = { d, a, fields -> action { repository.action(d,a,fields) } }
    Scaffold(snackbarHost = { SnackbarHost(snackbar) }, topBar = { TopAppBar(title = { Text(stringResource(R.string.home_microhad)) }, actions = { TextButton(onClick = { refresh++; scope.launch { refreshStatus() } }) { Text(stringResource(R.string.home_refresh)) } }) }, bottomBar = {
        NavigationBar { listOf(R.string.home_dashboard,R.string.home_devices,R.string.home_groups,R.string.home_app).forEachIndexed { index, label -> NavigationBarItem(selected = tab == index, onClick = { tab = index }, icon = { Icon(painterResource(listOf(R.drawable.ic_nav_dashboard, R.drawable.ic_nav_devices, R.drawable.ic_nav_groups, R.drawable.ic_nav_app)[index]), contentDescription = null) }, label = { Text(stringResource(label)) }) } }
    }) { padding ->
        if(tab == 3) { Box(Modifier.padding(padding)) { management() }; return@Scaffold }
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(tabScroll).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if(snapshot == null && error.isEmpty()) CircularProgressIndicator()
            if(error.isNotEmpty()) Text(stringResource(R.string.home_stale, error), color = MaterialTheme.colorScheme.error)
            if(message.isNotEmpty()) Text(message, Modifier.semantics { contentDescription = message })
            if(busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            when(tab) {
                0 -> {
                    if(configError.isNotEmpty()) Text(configError, color = MaterialTheme.colorScheme.error)
                    val config = dashboard
                    val widgets = config?.array("widgets")?.objects()?.toMutableList() ?: mutableListOf()
                    if(config != null && !config.flag("gateway_widget_initialized") && widgets.none { it.optString("kind") == "gateway" }) widgets.add(0, JSONObject().put("kind", "gateway"))
                    if(config == null && configError.isEmpty()) CircularProgressIndicator()
                    widgets.forEach { widget -> key(widget.optString("id", widget.toString())) {
                        val d = snapshot?.device(widget.optString("target"))
                        when(widget.optString("kind")) {
                            "gateway" -> HomeCard(stringResource(R.string.home_gateway_state)) {
                                val s = snapshot?.raw
                                Text(copy.format("Coordinator: %1\$s", copy.label(if(s == null) "Checking" else if(s.flag("online")) "Online" else "Offline")))
                                Text(copy.format("Zigbee network: %1\$s", copy.label(if(s == null) "Checking" else if(s.flag("network_formed")) "Formed" else if(s.flag("network_forming")) "Forming" else "Not formed")))
                                Text(copy.format("Matter network: %1\$s", copy.label(if(thread == null) "Checking" else if(!thread!!.flag("available")) "Unavailable" else if(!thread!!.flag("configured")) "Not configured" else if(thread!!.flag("attached")) "Attached" else "Waiting for attachment")))
                                Text(copy.format("Joining: %1\$s", copy.label(if(s?.flag("joining_preparing") == true) "Preparing" else if((s?.optLong("joining_until") ?: 0) > System.currentTimeMillis()/1000) "Open" else "Closed")))
                                Text(copy.format("Devices: %1\$s", snapshot?.devices?.size ?: "—"))
                                Text(copy.format("Free memory: %1\$s · Free flash: %2\$s", bytes(stats,"memory_free_bytes"),bytes(stats,"storage_free_bytes")))
                                if(overviewError.isNotEmpty()) Text(stringResource(R.string.home_stale_overview, overviewError), color = MaterialTheme.colorScheme.error)
                            }
                            "device" -> if(d == null) HomeCard(stringResource(R.string.home_unavailable_device)) { Text(stringResource(R.string.home_this_saved_device_is_unavailable)) } else DeviceCard(d, enabled, deviceAction, { message = ""; details = d.id }, { history = d.id })
                            "group" -> GroupCard(snapshot?.groups?.find { it.optString("id") == widget.optString("target") }, snapshot?.devices.orEmpty(), enabled) { route -> action { repository.form(route) } }
                            "chart" -> HomeCard(d?.name ?: copy.label("Unavailable device")) { if(d != null) HistoryChart(repository, d, widget.optString("range", "day"), widget.optInt("metric",1), false) }
                            "open_contacts", "present_sensors" -> {
                                val contacts = widget.optString("kind") == "open_contacts"
                                val devices = snapshot?.devices.orEmpty().filter { item -> val r = item.raw; if(contacts) item.has("contact") && !item.has("water_leak") && !r.flag("contact_trigger_mode") && r.flag("contact_known") && r.flag("contact_open") else item.has("occupancy") && r.optString("kind") in listOf("presence","occupancy") && r.flag("occupied") }
                                HomeCard(copy.label(if(contacts) "Open contact sensors" else "Present presence sensors")) { Text(stringResource(R.string.home_device_count, devices.size)); devices.forEach { Text(it.name) }; if(devices.isEmpty()) Text(stringResource(R.string.home_none)) }
                            }
                            "automation_trigger" -> {
                                val script = scripts.find { it.optString("id") == widget.optString("script_id") && it.flag("enabled") && it.flag("loaded") }
                                val trigger = script?.array("triggers")?.objects()?.find { it.optString("id") == widget.optString("trigger_id") }
                                HomeCard(trigger?.optString("label") ?: widget.optString("label", copy.label("Automation trigger"))) {
                                    Text(widget.optString("script_id"))
                                    Button(enabled = enabled && scriptsAvailable && trigger != null, onClick = { action { repository.form("/api/automations/trigger", mapOf("script_id" to widget.optString("script_id"), "trigger_id" to widget.optString("trigger_id"))) } }) { Text(stringResource(R.string.home_run)) }
                                    if(!scriptsAvailable || trigger == null) Text(stringResource(R.string.home_unavailable))
                                }
                            }
                            else -> HomeCard(stringResource(R.string.home_unavailable_widget)) { Text(widget.optString("kind")) }
                        }
                    } }
                    if(config != null && widgets.isEmpty()) Text(stringResource(R.string.home_no_dashboard_widgets_configured))
                }
                1 -> {
                    val devices = snapshot?.devices.orEmpty()
                    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) { (listOf("All") + devices.map { it.category }.distinct().sorted()).forEach { type -> FilterChip(selected = filter == type, onClick = { filter = type }, label = { Text(copy.label(type)) }) } }
                    val filtered = devices.filter { filter == "All" || it.category == filter }.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
                    filtered.forEach { d -> key(d.id) { DeviceCard(d, enabled, deviceAction, { message = ""; details = d.id }, { history = d.id }) } }
                    if(filtered.isEmpty() && snapshot != null) Text(stringResource(R.string.home_no_devices_in_this_view))
                }
                2 -> {
                    snapshot?.groups?.forEach { group -> GroupCard(group, snapshot?.devices.orEmpty(), enabled) { route -> action { repository.form(route) } } }
                    if(snapshot?.groups?.isEmpty() == true) Text(stringResource(R.string.home_no_scene_groups_configured_yet))
                }
            }
        }
    }
    details?.let { id ->
        val d = snapshot?.device(id)
        AlertDialog(onDismissRequest = { details = null }, title = { Text(d?.name ?: copy.label("Unavailable device")) }, text = { Column(Modifier.verticalScroll(rememberScrollState())) {
            if(busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            if(message.isNotEmpty()) Text(message)
            if(d != null) {
                listOf("source","kind","manufacturer_name","model_id","ieee_address","matter_node","matter_endpoint","commissioning_state","setup_state").forEach { field -> if(d.raw.has(field)) Text(copy.format("%1\$s: %2\$s", copy.label(field.replace('_',' ')),d.raw.opt(field) ?: "—")) }
                Text(copy.format("Seen: %1\$s", if(d.raw.optLong("last_seen") == 0L) copy.label("Never") else DateFormat.getDateTimeInstance().format(Date(d.raw.optLong("last_seen")*1000))))
                d.endpoints.forEach { Text(it.toString(2)) }
                d.raw.optJSONObject("node_descriptor")?.let { Text(it.toString(2)) }
            }
        } }, confirmButton = { TextButton(onClick = { details = null }) { Text(stringResource(R.string.home_close)) } }, dismissButton = { if(d?.canIdentify == true) TextButton(enabled = enabled && d.available, onClick = { deviceAction(d, "identify", emptyMap()) }) { Text(stringResource(R.string.home_identify)) } })
    }
    history?.let { id -> val d = snapshot?.device(id)
        AlertDialog(onDismissRequest = { history = null }, title = { Text(stringResource(R.string.home_device_history, d?.name ?: stringResource(R.string.home_unavailable_device))) }, text = { Column(Modifier.verticalScroll(rememberScrollState())) { if(d != null) HistoryChart(repository,d,"day",null,true) } }, confirmButton = { TextButton(onClick = { history = null }) { Text(stringResource(R.string.home_close)) } })
    }
}

private fun bytes(value: JSONObject?, key: String): String = if(value == null || value.isNull(key)) "—" else "%.1f KiB".format(value.optDouble(key)/1024)

@Composable
internal fun ThermostatControls(d: HomeDevice, t: JSONObject, enabled: Boolean, action: (HomeDevice,String,Map<String,String>) -> Unit) {
    val copy = homeCopy()
    val values = t.optJSONObject("values") ?: JSONObject()
    val supported = t.optJSONObject("supported") ?: JSONObject()
    Text(t.optString("status", copy.label("Waiting for thermostat readings")))
    if(t.optString("error").isNotEmpty()) Text(t.optString("error"), color = MaterialTheme.colorScheme.error)
    listOf("occupied_heating_setpoint" to "Heating target", "system_mode" to "Operating mode", "occupied_cooling_setpoint" to "Cooling target").forEach { (field,label) ->
        if(supported.flag(field)) {
            val canSend = enabled && field !in t.array("pending").strings()
            if(field == "system_mode") {
                Text(copy.label(label))
                Row(Modifier.horizontalScroll(rememberScrollState())) { val modes = t.array("modes"); (0 until modes.length()).forEach { i -> val mode = modes.getInt(i); FilterChip(selected = values.optInt(field,-1) == mode, enabled = canSend, onClick = { action(d,"thermostat",mapOf("field" to field,"value" to "$mode")) }, label = { Text(copy.label(when(mode) { 0 -> "Off"; 1 -> "Auto"; 3 -> "Cool"; 4 -> "Heat"; else -> copy.format("Mode %1\$s",mode) })) }) } }
            } else {
                var draft by remember(d.id,field) { mutableStateOf(if(values.has(field)) values.optString(field) else "") }
                LaunchedEffect(values.opt(field)) { draft = if(values.has(field)) values.optString(field) else "" }
                val metadata = t.optJSONObject("fields")?.optJSONObject(field)
                val value = draft.toDoubleOrNull()
                val min = metadata?.optDouble("min",Double.NEGATIVE_INFINITY) ?: if(field == "occupied_heating_setpoint") 4.0 else Double.NEGATIVE_INFINITY
                val max = metadata?.optDouble("max",Double.POSITIVE_INFINITY) ?: if(field == "occupied_heating_setpoint") 35.0 else Double.POSITIVE_INFINITY
                val step = metadata?.optDouble("step",0.0) ?: if(field == "occupied_heating_setpoint") 0.5 else 0.0
                val valid = value != null && value.isFinite() && value in min..max && (step <= 0 || kotlin.math.abs((value-(if(min.isFinite()) min else 0.0))/step-kotlin.math.round((value-(if(min.isFinite()) min else 0.0))/step)) < 0.00001)
                OutlinedTextField(value = draft,onValueChange = { draft = it },label = { Text(stringResource(R.string.home_temperature_label, copy.label(label))) },singleLine = true,enabled = canSend,modifier = Modifier.fillMaxWidth(),isError = draft.isNotEmpty() && !valid)
                TextButton(enabled = canSend && valid && draft != values.optString(field) && metadata?.flag("writable",true) != false,onClick = { action(d,"thermostat",mapOf("field" to field,"value" to draft)) }) { Text(stringResource(R.string.home_apply)) }
            }
        }
    }
}
