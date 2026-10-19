package io.github.microhad.matter

/** One serialized physical connection. A new connection requires a new instance. */
class MatterGattSession(private val ports: Ports, private val listener: Listener, private val now: () -> Long) {
    interface Ports {
        fun discover(): Boolean
        fun requestMtu(): Boolean
        fun subscribe(): Boolean
        fun write(data: ByteArray): Boolean
        fun close()
    }
    interface Listener {
        fun ready(mtu: Int)
        fun indication(data: ByteArray)
        fun written(write: Long, success: Boolean)
        fun failed()
    }
    enum class Stage { CONNECTING, DISCOVERING, MTU, SUBSCRIBING, READY, CLOSED }
    var stage = Stage.CONNECTING
        private set
    private var deadline = now() + 10000
    private var mtu = 23
    private var pendingWrite: Long? = null
    private var indicationsEnabled = false

    fun connected() {
        if (stage != Stage.CONNECTING) return
        advance(Stage.DISCOVERING)
        if (!operation { ports.discover() }) fail()
    }
    fun discovered(valid: Boolean) {
        if (stage != Stage.DISCOVERING) return
        if (!valid) { fail(); return }
        advance(Stage.MTU)
        if (!operation { ports.requestMtu() }) transportReady()
    }
    fun negotiated(value: Int, success: Boolean) {
        if (stage != Stage.MTU) return
        if (success && value !in 23..517) { fail(); return }
        mtu = if (success) value else 23
        transportReady()
    }
    private fun transportReady() {
        // CHIP queues its capabilities response until subscription arrives AFTER
        // the capabilities write. Let the gateway provide that first write now.
        advance(Stage.READY)
        listener.ready(mtu)
    }
    fun subscribed(success: Boolean) {
        if (stage != Stage.SUBSCRIBING) return
        val write = pendingWrite ?: return
        pendingWrite = null
        if (!success) { close(); listener.written(write, false); listener.failed(); return }
        indicationsEnabled = true
        advance(Stage.READY)
        listener.written(write, true)
    }
    fun write(write: Long, data: ByteArray) {
        if (stage != Stage.READY || pendingWrite != null || write !in 1..0xffffffffL ||
            data.isEmpty() || data.size > minOf(244, mtu - 3)) { fail(); return }
        pendingWrite = write
        deadline = now() + 10000
        if (!operation { ports.write(data.copyOf()) }) written(false)
    }
    fun written(success: Boolean) {
        if (stage != Stage.READY) return
        val write = pendingWrite ?: return
        if (success && !indicationsEnabled) {
            advance(Stage.SUBSCRIBING)
            if (!operation { ports.subscribe() }) subscribed(false)
            return
        }
        pendingWrite = null
        if (!success) {
            close()
            listener.written(write, false)
            listener.failed()
        } else listener.written(write, true)
    }
    fun indication(data: ByteArray) {
        // The capabilities indication can arrive before Android reports the
        // descriptor write callback; the relay is already attached at this point.
        if (stage != Stage.SUBSCRIBING && (stage != Stage.READY || !indicationsEnabled)) return
        if (data.isEmpty() || data.size > minOf(244, mtu - 3)) { fail(); return }
        listener.indication(data.copyOf())
    }
    fun tick() {
        if (stage != Stage.CLOSED && (stage != Stage.READY || pendingWrite != null) && now() >= deadline) fail()
    }
    fun fail() {
        if (stage == Stage.CLOSED) return
        close()
        listener.failed()
    }
    fun close() {
        if (stage == Stage.CLOSED) return
        stage = Stage.CLOSED
        pendingWrite = null
        operation { ports.close(); true }
    }
    private fun advance(next: Stage) { stage = next; deadline = now() + 10000 }
    private fun operation(action: () -> Boolean): Boolean = try { action() } catch (_: RuntimeException) { false }
}
