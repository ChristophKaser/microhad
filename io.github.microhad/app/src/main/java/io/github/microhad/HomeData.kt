package io.github.microhad

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import org.json.JSONArray
import org.json.JSONObject
import java.net.URLEncoder
import kotlin.math.roundToInt

internal fun JSONArray.objects(): List<JSONObject> = (0 until length()).mapNotNull { optJSONObject(it) }
internal fun JSONArray.strings(): List<String> = (0 until length()).map { getString(it) }
internal fun JSONObject.array(key: String): JSONArray = optJSONArray(key) ?: JSONArray()
internal fun JSONObject.flag(key: String, default: Boolean = false) = optBoolean(key, default)
internal fun encode(value: String): String = URLEncoder.encode(value, "UTF-8").replace("+", "%20")
internal data class HomeDevice(val raw: JSONObject) {
    val id = raw.getString("id")
    val name = raw.optString("name", id)
    val source = raw.optString("source")
    val available get() = raw.flag("online", true) && raw.flag("controller_available", true)
    fun has(capability: String) = raw.flag("has_$capability")
    val category: String get() = when {
        has("thermostat") -> "Thermostats"
        has("water_leak") -> "Water leak detectors"
        has("contact") -> "Contact sensors"
        !has("light") && (has("temperature") || has("humidity")) -> "Climate sensors"
        !has("light") && (has("air_quality") || has("co2") || has("pm25")) -> "Air quality sensors"
        has("occupancy") -> if (source == "fritz_box") "Presence sensors" else "Motion sensors"
        has("light") -> "Lights"
        has("remote") -> "Remotes"
        has("actor") && !has("power") -> "Switches"
        has("power") -> "Smart plugs"
        else -> "Other devices"
    }
    val historyId: String get() = when (source) {
        "shelly" -> "shelly:${raw.optString("external_id")}:${raw.optString("component")}"
        "fritz_box" -> "fritz:${raw.optString("external_id")}:${raw.optString("component")}"
        "matter" -> raw.optString("external_id")
        else -> "zigbee:${raw.optString("ieee_address")}"
    }
    val endpoints: List<JSONObject> get() = raw.array("endpoint_descriptors").objects().ifEmpty {
        listOf(JSONObject().put("endpoint", raw.optInt("matter_endpoint", -1)).put("input_clusters", raw.array("matter_servers")))
    }
    fun endpoint(cluster: Int): Int? = endpoints.firstOrNull { e -> e.array("input_clusters").let { a -> (0 until a.length()).any { a.optInt(it) == cluster } } }?.optInt("endpoint")?.takeIf { it in 0..65534 }
    val canDim get() = has("brightness") || endpoint(8) != null
    val canIdentify get() = source in listOf("zigbee", "matter") && endpoint(3) != null
}
internal data class HomeSnapshot(val raw: JSONObject) {
    val devices = raw.array("devices").objects().map(::HomeDevice)
    val groups = raw.array("scene_groups").objects()
    fun device(target: String) = devices.find { d -> d.id == target || d.raw.array("endpoint_descriptors").objects().any { it.optString("id") == target } }
}
internal data class HistoryPoint(val cursor: Long, val time: Long, val metric: Int, val value: Any, val unit: String?) {
    val y: Double get() = if (metric == 9) listOf("good", "fair", "moderate", "poor", "very_poor", "extremely_poor").indexOf(value.toString()).coerceAtLeast(0).toDouble()
        else (value.toString().toDoubleOrNull() ?: Double.NaN) / if (unit != null) 1 else when (metric) { 1, 2 -> 100; 4 -> 10; else -> 1 }
    val displayUnit get() = when (unit) { "celsius" -> "°C"; "percent" -> "%"; "watts" -> "W"; null -> when(metric) { 1 -> "°C"; 2,6,11 -> "%"; 4 -> "W"; 7,16 -> "ppm"; 8,12 -> "ug/m3"; 13,17 -> "particles/m3"; 14,18 -> "Bq/m3"; 15,19 -> "dB/m"; else -> "" }; else -> unit }
    val label get() = when(metric) { 3 -> if (y != 0.0) "Open" else "Closed"; 5 -> if(y != 0.0) "Occupied" else "Clear"; 10 -> if(y != 0.0) "On" else "Off"; 9 -> value.toString().replace('_', ' '); else -> "${"%.2f".format(y)} $displayUnit" }
}
internal val metricNames = listOf("", "Temperature", "Humidity", "Contact", "Power", "Occupancy", "Battery", "CO₂", "PM2.5", "Air quality", "Light", "Brightness", "CO₂ mass concentration", "CO₂ particle concentration", "CO₂ activity concentration", "CO₂ attenuation", "PM2.5 (ppm)", "PM2.5 particle concentration", "PM2.5 activity concentration", "PM2.5 attenuation")
internal class HomeRepository(private val api: GatewayApi) {
    suspend fun json(path: String) = withContext(Dispatchers.Default) { JSONObject(api.request(path, maxBytes = 2 * 1024 * 1024)) }
    suspend fun status() = withContext(Dispatchers.Default) { HomeSnapshot(json("/api/status")) }
    suspend fun form(path: String, fields: Map<String, String> = emptyMap()): JSONObject {
        val result = api.request(path, fields.entries.joinToString("&") { "${encode(it.key)}=${encode(it.value)}" }, "application/x-www-form-urlencoded", 65536)
        return if (result.isBlank()) JSONObject() else JSONObject(result)
    }
    suspend fun action(device: HomeDevice, action: String, fields: Map<String, String> = emptyMap()) {
        require(device.available) { "Device unavailable" }
        if (device.source != "matter") { form("/api/devices/${encode(device.id)}/$action", fields); return }
        val node = device.raw.optString("matter_node")
        require(node.matches(Regex("[0-9a-f]{16}"))) { "Invalid Matter node" }
        suspend fun call(cluster: Int, command: JSONObject? = null) {
            val endpoint = requireNotNull(device.endpoint(cluster)) { "Unsupported endpoint" }
            val body = (command ?: JSONObject().put("cluster", cluster).put("attribute", 0)).put("node", node).put("endpoint", endpoint).put("schema", 1)
            matter(if (command == null) "read" else "command", body)
        }
        when(action) {
            "light", "actor" -> if (fields.containsKey("brightness")) {
                val percent = fields.getValue("brightness").toInt(); require(percent in 0..100)
                call(8, JSONObject().put("command", "move_to_level").put("level", (percent * 254.0 / 100).roundToInt()).put("transition_deciseconds", 0))
            } else { val state = fields.getValue("state"); require(state in listOf("on", "off", "toggle")); call(6, JSONObject().put("command", state)) }
            "identify" -> call(3, JSONObject().put("command", "identify"))
            "refresh" -> for (e in device.endpoints) for (cluster in listOf(6,8,69,1026,1029,1037,1066,91)) {
                val clusters = e.array("input_clusters")
                if ((0 until clusters.length()).any { clusters.optInt(it) == cluster }) matter("read", JSONObject().put("schema",1).put("node",node).put("endpoint",e.getInt("endpoint")).put("cluster",cluster).put("attribute",0))
            }
            else -> error("Unsupported Matter action")
        }
    }
    private suspend fun matter(action: String, body: JSONObject) = withTimeout(35000) {
        val accepted = JSONObject(api.request("/api/matter/devices/$action", body.toString()))
        require(accepted.optInt("schema") == 1 && accepted.flag("ok") && accepted.optString("state") == "queued") { "Invalid Matter response" }
        val job = accepted.getLong("job"); require(job in 1..0xffffffffL)
        while (true) {
            val status = json("/api/matter/jobs/$job")
            require(status.getLong("job") == job)
            when(status.getString("state")) {
                "complete" -> { val response = status.getJSONObject("response"); require(response.optInt("schema") == 1 && response.flag("ok")) { response.optString("error", "Matter command failed") }
                    val completed = response.getJSONObject("result")
                    require(completed.optString("state") == "complete" && completed.optString("job").matches(Regex("[0-9a-f]{16}"))) { "Invalid Matter result" }
                    val result = completed.getJSONObject("result")
                    val cluster = if(action == "read") body.getInt("cluster") else when(body.getString("command")) { "identify" -> 3; "move_to_level" -> 8; else -> 6 }
                    require(result.optString("node") == body.getString("node") && result.getInt("endpoint") == body.getInt("endpoint") && result.getInt("cluster") == cluster && result.getInt("attribute") == 0) { "Mismatched Matter result" }
                    break }
                "failed" -> error(status.optString("error", "Matter command failed"))
                "queued", "running" -> delay(200)
                else -> error("Invalid Matter job state")
            }
        }
    }
    suspend fun history(id: String, range: String, now: Long): List<HistoryPoint> {
        val points = linkedMapOf<Long, HistoryPoint>()
        var before = 0L
        var pages = 0
        do {
            require(++pages <= 1000) { "Too many history pages" }
            val query = mutableListOf<String>()
            if (range != "all") query += "since=${(now - if(range == "week") 604800 else 86400).coerceAtLeast(0)}"
            if (before != 0L) query += "before=$before"
            val page = json("/api/devices/${encode(id)}/history" + if(query.isEmpty()) "" else "?${query.joinToString("&")}")
            val samples = page.array("samples")
            for (i in 0 until samples.length()) {
                val a = samples.getJSONArray(i)
                val p = HistoryPoint(a.getLong(0), a.getLong(1), a.getInt(2), a.get(3), if(a.length() > 4 && !a.isNull(4)) a.getString(4) else null)
                points[p.cursor] = p
            }
            val next = page.optLong("next_before")
            require(next == 0L || before == 0L || next < before) { "History cursor did not advance" }
            before = next
        } while (before != 0L && points.size < 10000)
        return points.values.sortedBy { it.cursor }.takeLast(10000)
    }
}

/** Match the Web UI's held value at the left edge, without joining different units. */
internal fun historyWindow(points: List<HistoryPoint>, metric: Int, start: Long, now: Long): List<HistoryPoint> {
    val matching = points.filter { it.metric == metric && it.y.isFinite() }.sortedWith(compareBy<HistoryPoint> { it.time }.thenBy { it.cursor })
    val unit = matching.lastOrNull()?.displayUnit
    val sameUnit = matching.filter { it.displayUnit == unit }
    val visible = sameUnit.filter { it.time in start..now }.toMutableList()
    sameUnit.lastOrNull { it.time < start }?.let { visible.add(0,it.copy(time = start)) }
    return visible
}
