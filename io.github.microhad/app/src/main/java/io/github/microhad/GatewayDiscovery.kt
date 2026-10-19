package io.github.microhad

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Handler
import android.os.Looper

/** Foreground discovery only. Results are suggestions, verified by the configuration endpoint. */
@Suppress("DEPRECATION")
class GatewayDiscovery(context: Context, private val found: (String, String) -> Unit, private val status: (String) -> Unit) : AutoCloseable {
    private val manager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val handler = Handler(Looper.getMainLooper())
    private var active = false
    private val pending = java.util.ArrayDeque<NsdServiceInfo>()
    private val seen = mutableSetOf<String>()
    private var resolving = false
    private val timeout = Runnable { close(); status("Discovery finished. Enter an address if your gateway was not found.") }
    private val listener = object : NsdManager.DiscoveryListener {
        override fun onDiscoveryStarted(type: String) = Unit
        override fun onDiscoveryStopped(type: String) = Unit
        override fun onStartDiscoveryFailed(type: String, code: Int) { handler.post { close(); status("Discovery unavailable ($code); enter an address.") } }
        override fun onStopDiscoveryFailed(type: String, code: Int) = Unit
        override fun onServiceLost(info: NsdServiceInfo) = Unit
        override fun onServiceFound(info: NsdServiceInfo) { handler.post {
            if (active && info.serviceType.contains("_microhad._tcp") && seen.add(info.serviceName) && pending.size < 32) {
                pending.add(info); resolveNext()
            }
        } }
    }
    fun start() {
        if (active) return
        active = true
        status("Searching this LAN for gateways…")
        try { manager.discoverServices("_microhad._tcp.", NsdManager.PROTOCOL_DNS_SD, listener); handler.postDelayed(timeout, 12000) }
        catch (_: RuntimeException) { close(); status("Discovery unavailable; enter an address.") }
    }
    private fun resolveNext() {
        if (!active || resolving || pending.isEmpty()) return
        resolving = true
        try { manager.resolveService(pending.remove(), object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) { handler.post { resolving = false; resolveNext() } }
            override fun onServiceResolved(info: NsdServiceInfo) { handler.post {
                resolving = false
                if (active) info.host?.hostAddress?.let { address ->
                    val host = if (':' in address) "[$address]" else address
                    found(info.serviceName, "http://$host:${info.port}")
                }
                resolveNext()
            } }
        }) } catch (_: RuntimeException) { resolving = false; resolveNext() }
    }
    override fun close() {
        handler.removeCallbacks(timeout)
        if (active) runCatching { manager.stopServiceDiscovery(listener) }
        active = false; pending.clear()
    }
}
