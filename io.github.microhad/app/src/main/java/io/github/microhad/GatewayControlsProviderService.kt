package io.github.microhad

import android.app.PendingIntent
import android.content.SharedPreferences
import android.content.Intent
import android.net.Uri
import android.service.controls.Control
import android.service.controls.ControlsProviderService
import android.service.controls.DeviceTypes
import android.service.controls.actions.*
import android.service.controls.templates.*
import androidx.annotation.RequiresApi
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import java.security.MessageDigest
import java.util.concurrent.Flow
import java.util.function.Consumer
import kotlin.math.roundToInt

internal data class GatewayControl(
    val id: String, val title: String, val section: String, val text: String = "",
    val device: HomeDevice? = null, val capability: String = "", val route: String = "",
    val fields: Map<String, String> = emptyMap(), val available: Boolean = true,
    val dim: Boolean = false, val checked: Boolean = false, val brightness: Float = 0f,
)
internal fun controlsGatewayKey(base: String, fingerprint: String): String = MessageDigest.getInstance("SHA-256")
    .digest("$base\n$fingerprint".toByteArray()).joinToString("") { "%02x".format(it.toInt() and 255) }
internal fun gatewayControls(key: String, snapshot: HomeSnapshot, scripts: List<org.json.JSONObject>): List<GatewayControl> = buildList {
    fun id(vararg parts: String) = (listOf(key) + parts.map(::encode)).joinToString(":")
    snapshot.devices.forEach { d ->
        listOf("light", "actor").filter(d::has).forEach { capability ->
            val known = d.raw.flag("${capability}_known", capability == "light")
            val on = d.raw.flag("${capability}_on")
            add(GatewayControl(id("device", d.id, capability), d.name + if (capability == "actor" && d.has("light")) " · Switch" else "",
                "Devices", if (known) { if (on) "On" else "Off" } else "Unknown", d, capability,
                available = d.available, dim = capability == "light" && d.canDim, checked = on,
                brightness = d.raw.optInt("brightness_percent", 0).coerceIn(0, 100).toFloat()))
        }
        deviceCardReadings(d).filter { it.key !in listOf("light", "actor") }.forEach { reading ->
            add(GatewayControl(id("sensor", d.id, reading.key), "${d.name} · ${reading.title}", "Sensors", reading.value,
                device = d, available = d.available))
        }
    }
    snapshot.groups.forEach { group ->
        val groupId = group.getInt("id")
        group.array("scenes").objects().forEach { scene ->
            add(GatewayControl(id("scene", "$groupId", scene.getInt("id").toString()),
                "${group.optString("name")} · ${scene.optString("name")}", "Scenes", "Run",
                route = "/api/scenes/$groupId/${scene.getInt("id")}/activate"))
        }
        add(GatewayControl(id("group", "$groupId", "off"), "${group.optString("name")} · Off", "Scenes", "Run", route = "/api/scenes/$groupId/off"))
    }
    scripts.forEach { script -> script.array("triggers").objects().forEach { trigger ->
        add(GatewayControl(id("trigger", script.getString("id"), trigger.getString("id")),
            "${script.optString("name", script.getString("id"))} · ${trigger.optString("label", trigger.getString("id"))}",
            "Automation triggers", "Run", route = "/api/automations/trigger",
            fields = mapOf("script_id" to script.getString("id"), "trigger_id" to trigger.getString("id"))))
    } }
}

@RequiresApi(30)
@OptIn(ExperimentalCoroutinesApi::class)
class GatewayControlsProviderService : ControlsProviderService() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val store by lazy { GatewayStore(this) }
    private data class Snapshot(val generation: String, val controls: List<GatewayControl>, val online: Boolean)
    private val remembered = java.util.concurrent.ConcurrentHashMap<String, GatewayControl>()
    private val actions = java.util.concurrent.ConcurrentHashMap.newKeySet<Job>()
    private val preferences by lazy { getSharedPreferences("gateway", MODE_PRIVATE) }
    private val configurationListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == "generation" || key == "reset_pending" || key == null) {
            actions.forEach { it.cancel() }
            refresh.update { it + 1 }
        }
    }
    override fun onCreate() {
        super.onCreate()
        preferences.registerOnSharedPreferenceChangeListener(configurationListener)
    }
    private val refresh = MutableStateFlow(0L)
    private fun valid(generation: String) = !store.resetPending && store.base.isNotEmpty() && store.generation == generation
    private suspend fun fetch(): Snapshot {
        val generation = store.generation
        if (!valid(generation)) return Snapshot(generation, emptyList(), false)
        val base = store.base
        val fingerprint = store.fingerprint
        val repository = HomeRepository(GatewayApi(base, fingerprint))
        return try {
            val snapshot = repository.status()
            val scripts = try { repository.json("/api/automations").array("scripts").objects() }
                catch (e: CancellationException) { throw e }
                catch (_: Exception) { null }
            if (!valid(generation)) return Snapshot(generation, emptyList(), false)
            val key = controlsGatewayKey(base, fingerprint)
            val controls = gatewayControls(key, snapshot, scripts.orEmpty()).toMutableList()
            if (scripts == null) controls += remembered.values.filter { it.id.startsWith("$key:trigger:") }.map { it.copy(available = false) }
            controls.forEach { remembered[it.id] = it }
            Snapshot(generation, controls, true)
        } catch (e: CancellationException) { throw e }
        catch (_: Exception) { Snapshot(generation, emptyList(), false) }
    }
    private val snapshots by lazy {
        refresh.flatMapLatest { flow { while (true) { emit(fetch()); delay(3000) } } }
            .shareIn(scope, SharingStarted.WhileSubscribed(stopTimeoutMillis = 0, replayExpirationMillis = 0), replay = 1)
    }
    private fun intent(id: String): PendingIntent = PendingIntent.getActivity(this, 0,
        Intent(this, MainActivity::class.java).setData(Uri.parse("microhad://controls/${Uri.encode(id)}")),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
    private fun type(c: GatewayControl) = when {
        c.capability == "light" -> DeviceTypes.TYPE_LIGHT
        c.capability == "actor" -> DeviceTypes.TYPE_SWITCH
        c.route.isNotEmpty() -> DeviceTypes.TYPE_ROUTINE
        else -> DeviceTypes.TYPE_UNKNOWN
    }
    private fun stateless(c: GatewayControl): Control = Control.StatelessBuilder(c.id, intent(c.id))
        .setTitle(c.title).setSubtitle(c.section).setStructure(store.config?.name ?: store.base).setZone(c.section).setDeviceType(type(c)).build()
    private fun stateful(c: GatewayControl, status: Int = Control.STATUS_OK): Control {
        val template = when {
            status != Control.STATUS_OK || !c.available -> ControlTemplate.getNoTemplateObject()
            c.dim -> ToggleRangeTemplate("toggle", ControlButton(c.checked, "Power"), RangeTemplate("brightness", 0f, 100f, c.brightness, 1f, "%1.0f%%"))
            c.capability.isNotEmpty() -> ToggleTemplate("toggle", ControlButton(c.checked, "Power"))
            c.route.isNotEmpty() -> StatelessTemplate("run")
            else -> ControlTemplate.getNoTemplateObject()
        }
        return Control.StatefulBuilder(c.id, intent(c.id)).setTitle(c.title).setSubtitle(c.section)
            .setStructure(store.config?.name ?: store.base).setZone(c.section).setDeviceType(type(c))
            .setStatus(if (!c.available && status == Control.STATUS_OK) Control.STATUS_DISABLED else status)
            .setStatusText(if (status == Control.STATUS_NOT_FOUND) "Removed" else if (!c.available || status != Control.STATUS_OK) "Unavailable" else c.text)
            .setControlTemplate(template).build()
    }
    override fun createPublisherForAllAvailable(): Flow.Publisher<Control> = flow {
        fetch().controls.forEach { emit(stateless(it)) }
    }.controlsPublisher(scope)
    override fun createPublisherFor(controlIds: MutableList<String>): Flow.Publisher<Control> = snapshots.transform { snapshot ->
        controlIds.distinct().forEach { id ->
            val current = snapshot.controls.find { it.id == id }
            val fallback = remembered[id] ?: GatewayControl(id, "MicroHAD", "Devices")
            val sameGateway = store.base.isNotEmpty() && id.startsWith(controlsGatewayKey(store.base, store.fingerprint) + ":")
            emit(stateful(current ?: fallback, when {
                !sameGateway || store.resetPending -> Control.STATUS_NOT_FOUND
                !snapshot.online || !valid(snapshot.generation) -> Control.STATUS_ERROR
                current == null -> Control.STATUS_NOT_FOUND
                else -> Control.STATUS_OK
            }))
        }
    }.controlsPublisher(scope)
    override fun performControlAction(controlId: String, action: ControlAction, consumer: Consumer<Int>) {
        val job = scope.launch(start = CoroutineStart.LAZY) {
            val result = try {
                val snapshot = fetch()
                val c = snapshot.controls.find { it.id == controlId }
                if (c == null || !c.available || !snapshot.online || !valid(snapshot.generation)) ControlAction.RESPONSE_FAIL
                else {
                    val base = store.base
                    val pin = store.fingerprint
                    check(valid(snapshot.generation) && controlId.startsWith(controlsGatewayKey(base, pin) + ":"))
                    currentCoroutineContext().ensureActive()
                    val repository = HomeRepository(GatewayApi(base, pin))
                    when {
                        action is BooleanAction && c.capability.isNotEmpty() && action.templateId == "toggle" ->
                            repository.action(requireNotNull(c.device), c.capability, mapOf("state" to if (action.newState) "on" else "off"))
                        action is FloatAction && c.dim && action.templateId == "brightness" && action.newValue.isFinite() && action.newValue in 0f..100f ->
                            repository.action(requireNotNull(c.device), "light", mapOf("brightness" to action.newValue.roundToInt().toString()))
                        action is CommandAction && c.route.isNotEmpty() && action.templateId == "run" -> repository.form(c.route, c.fields)
                        else -> error("Unsupported control action")
                    }
                    if (valid(snapshot.generation)) ControlAction.RESPONSE_OK else ControlAction.RESPONSE_FAIL
                }
            } catch (e: CancellationException) { consumer.accept(ControlAction.RESPONSE_FAIL); throw e }
            catch (_: Exception) { ControlAction.RESPONSE_FAIL }
            consumer.accept(result)
            refresh.update { it + 1 }
        }
        actions.add(job)
        job.invokeOnCompletion { actions.remove(job) }
        job.start()
    }
    override fun onDestroy() {
        preferences.unregisterOnSharedPreferenceChangeListener(configurationListener)
        scope.cancel()
        super.onDestroy()
    }
}
