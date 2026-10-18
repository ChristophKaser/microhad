package org.microhad.companion

import android.content.Context
import java.util.UUID

class GatewayStore(context: Context) {
    private val prefs = context.getSharedPreferences("gateway", Context.MODE_PRIVATE)
    val resetPending: Boolean get() = prefs.getBoolean("reset_pending", false)
    fun processStarted() { if (resetPending) check(prefs.edit().clear().commit()) }
    val base: String get() = prefs.getString("base", "").orEmpty()
    val fingerprint: String get() = prefs.getString("fingerprint", "").orEmpty()
    val generation: String get() = prefs.getString("generation", "").orEmpty()
    val config: GatewayConfig? get() = runCatching { GatewayConfig.parse(prefs.getString("config", "")!!) }.getOrNull()
    val status: String get() = prefs.getString("status", "Not registered").orEmpty()
    @Synchronized fun installationId(): String {
        prefs.getString("installation_id", null)?.let { return it }
        val value = UUID.randomUUID().toString()
        check(prefs.edit().putString("installation_id", value).commit())
        return value
    }
    fun save(base: String, pin: String, config: String) {
        check(prefs.edit().putString("base", base).putString("fingerprint", pin).putString("config", config)
            .putString("generation", UUID.randomUUID().toString()).putString("status", "Not registered").commit())
    }
    fun status(value: String) { prefs.edit().putString("status", value).apply() }
    fun clear() { check(prefs.edit().clear().putBoolean("reset_pending", true).commit()) }
}
