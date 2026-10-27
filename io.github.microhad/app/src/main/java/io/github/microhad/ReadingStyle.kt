package io.github.microhad

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Same bands, unit conversions, and palette as Web UI sensorSeverity and sensor-reading-* CSS. */
internal enum class ReadingSeverity(val label: String, val background: Color, val foreground: Color) {
    GOOD("good", Color(0xFFD9F3DF), Color(0xFF155D32)),
    FAIR("fair", Color(0xFFB9E8C5), Color(0xFF155D32)),
    MODERATE("moderate", Color(0xFFFFF0BB), Color(0xFF765000)),
    POOR("poor", Color(0xFFFFD9D4), Color(0xFF8D3029)),
    VERY_POOR("very poor", Color(0xFFEFA39E), Color(0xFF70231F)),
    EXTREMELY_POOR("extremely poor", Color(0xFF7F1D1D), Color.White),
}

internal fun readingSeverity(key: String, value: Any?, unit: String, known: Boolean): ReadingSeverity? {
    if (!known || value == null) return null
    if (key == "air_quality") return ReadingSeverity.values().find { it.label == value.toString().replace('_', ' ') }
    val number = (value as? Number)?.toDouble()?.takeIf(Double::isFinite) ?: return null
    val normalized: Double
    val limits: List<Double>
    when (key) {
        "co2" -> {
            normalized = when (unit) { "ppm" -> number; "ppb" -> number / 1000; "ppt" -> number / 1000000; else -> return null }
            limits = listOf(800.0, 1000.0, 1400.0, 2000.0, 5000.0)
        }
        "pm25" -> {
            normalized = when (unit) { "ug/m3" -> number; "mg/m3" -> number * 1000; "ng/m3" -> number / 1000; else -> return null }
            limits = listOf(10.0, 20.0, 35.0, 55.0, 125.0)
        }
        else -> return null
    }
    val band = limits.indexOfFirst { normalized < it }
    return ReadingSeverity.values()[if (band < 0) 5 else band]
}

internal fun readingIcon(key: String): Int? = when (key) {
    "temperature" -> R.drawable.ic_reading_temperature
    "humidity" -> R.drawable.ic_reading_humidity
    else -> null
}

@Composable
internal fun SensorReading(reading: CardReading, prominent: Boolean, modifier: Modifier = Modifier) {
    val copy = homeCopy()
    val colors = MaterialTheme.colorScheme
    val severity = reading.severity
    val foreground = when {
        !reading.known -> colors.onSurfaceVariant
        reading.alarm -> colors.error
        severity != null -> severity.foreground
        reading.key == "temperature" -> Color(0xFFC66A36)
        reading.key == "humidity" -> Color(0xFF327DAA)
        prominent -> colors.onSurface
        else -> colors.onSurfaceVariant
    }
    val background = severity?.background ?: Color.Transparent
    val title = if (severity != null && reading.key != "air_quality") "${reading.title} · ${copy.label(severity.label)}" else reading.title
    if (prominent) {
        Column(
            modifier.background(background, RoundedCornerShape(10.dp)).padding(if (severity != null) 8.dp else 0.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(reading.value, fontSize = 24.sp, lineHeight = 30.sp, fontWeight = FontWeight.Medium, color = foreground)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                readingIcon(reading.key)?.let { Icon(painterResource(it), contentDescription = null, tint = foreground, modifier = Modifier.size(16.dp)) }
                Text(title, style = MaterialTheme.typography.labelSmall, color = if (severity != null) foreground else colors.onSurfaceVariant)
            }
        }
    } else {
        Row(
            modifier.background(background, RoundedCornerShape(6.dp)).padding(horizontal = if (severity != null) 6.dp else 0.dp, vertical = if (severity != null) 4.dp else 0.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            readingIcon(reading.key)?.let { Icon(painterResource(it), contentDescription = null, tint = foreground, modifier = Modifier.size(16.dp)) }
            Text("$title ${reading.value}", style = MaterialTheme.typography.bodySmall, color = foreground)
        }
    }
}
