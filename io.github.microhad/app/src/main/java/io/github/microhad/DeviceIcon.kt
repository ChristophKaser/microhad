package io.github.microhad

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Dp

/** Native vector counterparts of the Web UI's device visuals. Text carries accessible state. */
@Composable
internal fun DeviceIcon(device: HomeDevice, size: Dp = 40.dp) {
    val r = device.raw
    val category = device.category
    val presenceFresh = r.optLong("last_seen") > 0 && System.currentTimeMillis() / 1000 - r.optLong("last_seen") <= 90
    val icon = when (category) {
        "Lights" -> if (r.flag("light_known", true) && r.flag("light_on"))
            R.drawable.ic_device_bulb_on else R.drawable.ic_device_bulb_off
        "Contact sensors" -> when {
            r.flag("contact_trigger_mode") -> R.drawable.ic_device_contact_trigger
            !r.flag("contact_known") -> R.drawable.ic_device_contact_unknown
            r.flag("contact_open") -> R.drawable.ic_device_contact_open
            else -> R.drawable.ic_device_contact_closed
        }
        "Water leak detectors" -> R.drawable.ic_device_water
        "Thermostats" -> R.drawable.ic_device_climate
        "Climate sensors" -> if (device.has("temperature")) R.drawable.ic_device_climate else R.drawable.ic_device_water
        "Smart plugs" -> R.drawable.ic_device_plug
        "Switches" -> R.drawable.ic_device_switch
        "Motion sensors" -> if (r.flag("occupied")) R.drawable.ic_device_motion_active else R.drawable.ic_device_motion_idle
        "Presence sensors" -> if (r.flag("occupied") && presenceFresh)
            R.drawable.ic_device_presence_active else R.drawable.ic_device_presence_idle
        "Air quality sensors" -> R.drawable.ic_device_air
        "Remotes" -> R.drawable.ic_device_remote
        else -> R.drawable.ic_device_generic
    }
    val colors = MaterialTheme.colorScheme
    val active = when (category) {
        "Lights" -> r.flag("light_known", true) && r.flag("light_on")
        "Contact sensors" -> if (r.flag("contact_trigger_mode")) r.flag("contact_trigger_active")
            else r.flag("contact_known") && r.flag("contact_open")
        "Water leak detectors" -> r.flag("water_leak_known") && r.flag("water_detected")
        "Switches", "Smart plugs" -> r.flag("actor_known") && r.flag("actor_on")
        "Motion sensors" -> r.flag("occupied")
        "Presence sensors" -> presenceFresh && r.flag("occupied")
        else -> false
    }
    val tint = when {
        !device.available -> colors.onSurfaceVariant.copy(alpha = .45f)
        category == "Water leak detectors" && active -> colors.error
        category == "Lights" && active -> {
            val level = if (device.has("brightness") && r.flag("brightness_known", true))
                r.optInt("brightness_percent").coerceIn(0, 100) / 100f else 1f
            lerp(colors.onSurfaceVariant, Color(0xFFAD7400), level)
        }
        active -> colors.primary
        else -> colors.onSurfaceVariant
    }
    val background = when {
        !device.available -> colors.surfaceVariant
        category == "Water leak detectors" && active -> colors.errorContainer
        category == "Lights" && active -> Color(0xFFFFF1CB)
        active -> colors.primaryContainer
        else -> colors.surfaceVariant
    }
    Box(Modifier.background(background, RoundedCornerShape(14.dp)).padding(6.dp)) {
        // Decorative: category and confirmed state are already spoken by the card's text.
        Icon(painterResource(icon), contentDescription = null, tint = tint, modifier = Modifier.size(size))
    }
}
