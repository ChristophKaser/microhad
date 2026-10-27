package io.github.microhad

import android.text.format.DateUtils
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.json.JSONObject

/** Shared by the dashboard and inventory; secondary controls expand in place. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun DeviceCard(
    d: HomeDevice,
    enabled: Boolean,
    action: (HomeDevice, String, Map<String, String>) -> Unit,
    details: () -> Unit,
    history: () -> Unit,
) {
    val copy = homeCopy()
    val r = d.raw
    val colors = MaterialTheme.colorScheme
    val controls = enabled && d.available
    val power = when { d.has("light") -> "light"; d.has("actor") -> "actor"; else -> null }
    val readings = deviceCardReadings(d, copy::label)
    val focusKeys = when (d.category) {
        "Climate sensors" -> listOf("temperature", "humidity")
        "Thermostats" -> listOf("temperature")
        "Water leak detectors" -> listOf("water_leak")
        "Contact sensors" -> listOf("contact")
        "Presence sensors", "Motion sensors" -> listOf("occupancy")
        "Lights" -> listOf("light")
        "Switches", "Smart plugs" -> listOf("actor", "power").take(1 + if (d.has("power")) 1 else 0)
        "Air quality sensors" -> listOf("co2", "pm25", "air_quality").filter { key -> readings.any { it.key == key } }.take(2)
        else -> readings.take(2).map { it.key }
    }
    val primary = readings.filter { it.key in focusKeys }.take(2)
    val secondary = readings.filter { it !in primary }
    val expandable = (d.has("light") && d.canDim) || d.has("thermostat") || (d.has("light") && d.has("actor"))
    var expanded by remember(d.id) { mutableStateOf(false) }

    HomeCardSurface {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                DeviceIcon(d, size = 28.dp)
                Column(Modifier.weight(1f)) {
                    Text(d.name, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(copy.label(d.category), style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
                }
                if (power != null) {
                    val on = r.flag("${power}_on")
                    Switch(
                        checked = on,
                        enabled = controls,
                        onCheckedChange = { next -> action(d, power, mapOf("state" to if (next) "on" else "off")) },
                        modifier = Modifier.semantics { contentDescription = copy.label(if (on) "Turn off" else "Turn on") },
                    )
                }
            }
            if (!d.available) Text(copy.label("Unavailable"), style = MaterialTheme.typography.labelMedium, color = colors.error)
            if (primary.isNotEmpty()) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    primary.forEach { reading ->
                        SensorReading(reading, prominent = true, modifier = Modifier.weight(1f))
                    }
                }
            }
            if (secondary.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    secondary.forEach { reading ->
                        SensorReading(reading, prominent = false)
                    }
                }
            }
            if (d.has("light") && d.canDim) {
                var brightness by remember(d.id) { mutableFloatStateOf(r.optInt("brightness_percent", 100).toFloat()) }
                LaunchedEffect(r.optInt("brightness_percent")) { brightness = r.optInt("brightness_percent", 100).toFloat() }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Slider(
                        value = brightness.coerceIn(0f, 100f),
                        onValueChange = { brightness = it },
                        valueRange = 0f..100f,
                        enabled = controls,
                        onValueChangeFinished = { action(d, "light", mapOf("brightness" to brightness.toInt().toString())) },
                        modifier = Modifier.weight(1f).semantics { contentDescription = copy.format("Brightness for %1\$s", d.name) },
                    )
                    Text(if (r.flag("brightness_known", true)) "${brightness.toInt()}%" else "—", style = MaterialTheme.typography.labelLarge, color = colors.onSurfaceVariant)
                }
            }
            AnimatedVisibility(expanded) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (d.has("light") && d.canDim) {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            listOf(25, 50, 75, 100).forEach { value ->
                                TextButton(enabled = controls, onClick = { action(d, "light", mapOf("brightness" to "$value")) }) { Text("$value%") }
                            }
                        }
                    }
                    if (d.has("light") && d.has("actor")) {
                        val on = r.flag("actor_on")
                        TextButton(enabled = controls, onClick = { action(d, "actor", mapOf("state" to if (on) "off" else "on")) }) {
                            Text("${copy.label("Switch")}: ${copy.label(if (on) "Turn off" else "Turn on")}")
                        }
                    }
                    r.optJSONObject("thermostat")?.let { ThermostatControls(d, it, controls, action) }
                }
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(Modifier.weight(1f)) {
                    val metadata = buildList {
                        if (d.has("battery")) add(stringResource(R.string.home_battery, r.optInt("battery_percent")))
                        when (d.source) {
                            "zigbee" -> add("LQI ${r.optInt("link_quality")}")
                            "matter" -> if (!r.isNull("matter_rssi_dbm")) add("${r.optInt("matter_rssi_dbm")} dBm")
                            else -> if (d.has("wifi_signal")) add("Wi-Fi ${r.optInt("wifi_signal_percent")}%")
                        }
                    }
                    if (metadata.isNotEmpty()) Text(metadata.joinToString(" · "), style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
                    val seen = r.optLong("last_seen")
                    val relative = if (seen == 0L) copy.label("Never") else DateUtils.getRelativeTimeSpanString(seen * 1000, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS, DateUtils.FORMAT_ABBREV_RELATIVE).toString()
                    Text(copy.format("Seen: %1\$s", relative), style = MaterialTheme.typography.labelSmall, color = colors.onSurfaceVariant)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CardAction(R.drawable.ic_card_details, stringResource(R.string.home_details), onClick = details)
                    if (historyCapabilities.any(d::has)) CardAction(R.drawable.ic_card_history, stringResource(R.string.home_history), onClick = history)
                    CardAction(R.drawable.ic_card_refresh, stringResource(R.string.home_refresh), enabled = controls) { action(d, "refresh", emptyMap()) }
                    if (expandable) CardAction(
                        if (expanded) R.drawable.ic_card_collapse else R.drawable.ic_card_expand,
                        stringResource(if (expanded) R.string.home_fewer_controls else R.string.home_more_controls),
                    ) { expanded = !expanded }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CardAction(icon: Int, label: String, enabled: Boolean = true, onClick: () -> Unit) {
    TooltipBox(
        positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
        tooltip = { PlainTooltip { Text(label) } },
        state = rememberTooltipState(),
    ) {
        IconButton(onClick = onClick, enabled = enabled, modifier = Modifier.size(40.dp)) {
            Icon(painterResource(icon), contentDescription = label, modifier = Modifier.size(18.dp), tint = if (enabled) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .38f))
        }
    }
}

private val historyCapabilities = listOf("temperature", "humidity", "contact", "power", "occupancy", "battery", "co2", "pm25", "air_quality", "light", "actor", "brightness")
internal data class CardReading(val key: String, val title: String, val value: String, val known: Boolean = true, val alarm: Boolean = false, val severity: ReadingSeverity? = null)

internal fun DeviceReadings(d: HomeDevice, label: (String) -> String = { it }): List<String> = deviceCardReadings(d, label).map { "${it.title}: ${it.value}" }

internal fun deviceCardReadings(d: HomeDevice, label: (String) -> String = { it }): List<CardReading> = buildList {
    val r = d.raw
    fun reading(capability: String, field: String, title: String, unit: String, defaultKnown: Boolean = false) {
        if (!d.has(capability)) return
        val known = r.has(field) && !r.isNull(field) && r.flag("${capability}_known", defaultKnown)
        val value = if (known) "${if (capability == "air_quality") label(r.optString(field).replace('_', ' ')) else r.opt(field)} $unit".trim() else label("Unknown")
        add(CardReading(capability, label(title), value, known, severity = readingSeverity(capability, r.opt(field), unit, known)))
    }
    fun state(key: String, title: String, known: Boolean, value: String, alarm: Boolean = false) {
        add(CardReading(key, label(title), label(if (known) value else "Unknown"), known, alarm && known))
    }
    reading("temperature", "temperature_celsius", "Temperature", "°C")
    reading("humidity", "humidity_percent", "Humidity", "%", true)
    reading("power", "power_watts", "Power", "W")
    if (d.has("water_leak")) state("water_leak", "Water", r.flag("water_leak_known"), if (r.flag("water_detected")) "Wet" else "Dry", r.flag("water_detected"))
    else if (d.has("contact")) {
        val trigger = r.flag("contact_trigger_mode")
        val active = if (trigger) r.flag("contact_trigger_active") else r.flag("contact_open")
        state("contact", "Contact", trigger || r.flag("contact_known"), if (trigger) { if (active) "Triggered" else "Idle" } else { if (active) "Open" else "Closed" })
    }
    if (d.has("occupancy")) {
        val known = d.category != "Presence sensors" || (r.optLong("last_seen") > 0 && System.currentTimeMillis() / 1000 - r.optLong("last_seen") <= 90)
        state("occupancy", "Occupancy", known, if (r.flag("occupied")) "Present" else "Clear")
    }
    if (d.has("light")) state("light", "Light", r.flag("light_known", true), if (r.flag("light_on")) "On" else "Off")
    if (d.has("actor")) state("actor", "Switch", r.flag("actor_known"), if (r.flag("actor_on")) "On" else "Off")
    val sensors = r.optJSONObject("sensor_values") ?: JSONObject()
    sensors.keys().forEach { key ->
        if (key !in listOf("temperature", "humidity")) {
            val sensor = sensors.getJSONObject(key)
            val known = sensor.optString("quality") == "measured" && !sensor.isNull("value")
            val title = when (key) { "co2" -> "CO₂"; "pm25" -> "PM2.5"; "air_quality" -> "Air quality"; else -> key.replace('_', ' ') }
            val value = if (known) "${label(sensor.get("value").toString().replace('_', ' '))} ${sensor.optString("unit")}".trim() else label("Unknown")
            add(CardReading(key, label(title), value, known, severity = readingSeverity(key, sensor.opt("value"), sensor.optString("unit"), known)))
        }
    }
    if (!sensors.has("co2")) reading("co2", "co2_ppm", "CO₂", "ppm")
    if (!sensors.has("pm25")) reading("pm25", "pm25_micrograms_m3", "PM2.5", "ug/m3")
    if (!sensors.has("air_quality")) reading("air_quality", "air_quality", "Air quality", "")
}
