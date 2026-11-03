package io.github.microhad

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import androidx.core.content.ContextCompat
import kotlin.math.roundToInt
import androidx.annotation.DrawableRes
import androidx.annotation.StringRes

/** Source metadata chooses presentation; remote channel IDs and drawable names are never trusted. */
internal object NotificationSources {
    data class Presentation(val channel: String, @DrawableRes val icon: Int, @StringRes val name: Int)
    val default = Presentation(GatewayMessagingService.CHANNEL, R.drawable.ic_gateway, R.string.gateway_alerts_channel_name)
    private fun source(category: String, @DrawableRes icon: Int, @StringRes name: Int) =
        Presentation("microhad.alerts.$category", icon, name)
    private val categories = mapOf(
        "window" to source("window", R.drawable.ic_notification_window, R.string.notification_channel_window),
        "doorbell" to source("doorbell", R.drawable.ic_notification_doorbell, R.string.notification_channel_doorbell),
        "temperature" to source("temperature", R.drawable.ic_notification_temperature, R.string.notification_channel_temperature),
        "humidity" to source("humidity", R.drawable.ic_notification_humidity, R.string.notification_channel_humidity),
        "leak" to source("leak", R.drawable.ic_notification_leak, R.string.notification_channel_leak),
        "battery" to source("battery", R.drawable.ic_notification_battery, R.string.notification_channel_battery),
        "power" to source("power", R.drawable.ic_notification_power, R.string.notification_channel_power),
        "air_quality" to source("air_quality", R.drawable.ic_notification_air_quality, R.string.notification_channel_air_quality)
    )

    fun presentation(source: String?, key: String?): Presentation {
        // An explicit unknown/default source must not accidentally inherit a keyed category.
        if (source != null) return categories[source] ?: default
        val inferred = when {
            key?.startsWith("contact:") == true && key.endsWith(":window") -> "window"
            key?.startsWith("value:") == true || key?.startsWith("legacy:") == true -> {
                when (val reading = key.substringBeforeLast(':', "").substringAfterLast(':', "")) {
                    "co2", "pm25" -> "air_quality"
                    else -> reading
                }
            }
            else -> "default"
        }
        return categories[inferred] ?: default
    }

    private data class ArtworkKey(val icon: Int, val density: Int)
    private val artwork = mutableMapOf<ArtworkKey, Bitmap>()

    /** Draw locally so background delivery never needs an image download. */
    @Synchronized
    fun largeIcon(context: Context, presentation: Presentation): Bitmap? {
        if (presentation == default) return null
        val resources = context.resources
        val key = ArtworkKey(presentation.icon, resources.displayMetrics.densityDpi)
        return artwork.getOrPut(key) {
            val scale = resources.displayMetrics.density
            val size = (64 * scale).roundToInt().coerceAtLeast(1)
            val inset = (12 * scale).roundToInt()
            Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888).apply {
                density = resources.displayMetrics.densityDpi
                val canvas = Canvas(this)
                val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(38, 59, 80) }
                canvas.drawRoundRect(0f, 0f, size.toFloat(), size.toFloat(), 16 * scale, 16 * scale, paint)
                val drawable = requireNotNull(ContextCompat.getDrawable(context, presentation.icon)).mutate()
                drawable.setTint(Color.WHITE)
                drawable.setBounds(inset, inset, size - inset, size - inset)
                drawable.draw(canvas)
            }
        }
    }

    fun register(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannels((listOf(default) + categories.values).map {
            NotificationChannel(it.channel, context.getString(it.name), NotificationManager.IMPORTANCE_DEFAULT)
        })
    }
}
