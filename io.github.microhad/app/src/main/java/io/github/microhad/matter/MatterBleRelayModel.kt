package io.github.microhad.matter

/** Serialized by the onboarding owner. Android/GATT and WebSocket adapters carry the epoch back. */
class MatterBleRelayModel(private val ports: Ports, private val nowMillis: () -> Long) {
    interface Ports {
        fun writeGatt(epoch: Long, generation: Long, write: Long, data: ByteArray)
        fun indicate(session: String, generation: Long, sequence: Long, data: ByteArray)
        fun completeWrite(session: String, generation: Long, write: Long, success: Boolean)
        fun cancel(session: String)
        fun disconnectGatt()
        fun closeRelay()
    }
    enum class State { IDLE, BINDING, ATTACHED, COMPLETE, CANCELLED, FAILED, TIMED_OUT }
    var state = State.IDLE
        private set
    var epoch = 0L
        private set
    private var session: String? = null
    private var generation = 0L
    private var mtu = 23
    private var sequence = 1L
    private var lastWrite = 0L
    private var pendingWrite: Long? = null
    private var completionSent = false
    private var completionSuccess = true
    val awaitingFailedWrite: Boolean
        get() = state == State.ATTACHED && pendingWrite != null && completionSent && !completionSuccess
    private var deadline = 0L
    private var writeDeadline = 0L

    fun begin(id: String, timeoutSeconds: Int): Long {
        require(state !in setOf(State.BINDING, State.ATTACHED))
        require(id.matches(Regex("[0-9a-f]{16}")) && id != "0000000000000000")
        require(timeoutSeconds in 30..900 && epoch < Long.MAX_VALUE)
        epoch++
        session = id
        generation = 0
        deadline = nowMillis() + timeoutSeconds * 1000L
        state = State.BINDING
        return epoch
    }

    fun attached(callbackEpoch: Long, connectionGeneration: Long, negotiatedMtu: Int) {
        if (callbackEpoch != epoch || state !in setOf(State.BINDING, State.ATTACHED)) return
        require(connectionGeneration in 1..0xffffffffL && negotiatedMtu in 23..517)
        require(connectionGeneration > generation)
        generation = connectionGeneration
        mtu = negotiatedMtu
        sequence = 1
        lastWrite = 0
        pendingWrite = null
        completionSent = false
        completionSuccess = true
        state = State.ATTACHED
    }

    fun write(callbackEpoch: Long, id: String, connectionGeneration: Long, write: Long, data: ByteArray) {
        if (!current(callbackEpoch, id, connectionGeneration)) return
        if (pendingWrite != null || write !in 1..0xffffffffL || write <= lastWrite ||
            data.isEmpty() || data.size > minOf(244, mtu - 3)) { finish(State.FAILED, true); return }
        pendingWrite = write
        lastWrite = write
        completionSent = false
        completionSuccess = true
        writeDeadline = nowMillis() + 10000
        transport { ports.writeGatt(epoch, generation, write, data.copyOf()) }
    }

    fun gattWritten(callbackEpoch: Long, connectionGeneration: Long, write: Long, success: Boolean) {
        if (!current(callbackEpoch, session, connectionGeneration) || pendingWrite != write || completionSent) return
        completionSent = true
        completionSuccess = success
        transport { ports.completeWrite(session!!, generation, write, success) }
        // The asynchronous relay must deliver success:false before releasing the socket.
        // Rust replies with transport/failed; terminal, connection-loss and deadline paths release us.
    }

    fun writeAcknowledged(callbackEpoch: Long, connectionGeneration: Long, write: Long) {
        if (!current(callbackEpoch, session, connectionGeneration) || pendingWrite != write || !completionSent) return
        if (!completionSuccess) { finish(State.FAILED, false); return }
        pendingWrite = null
        completionSent = false
    }

    fun indication(callbackEpoch: Long, connectionGeneration: Long, data: ByteArray) {
        if (!current(callbackEpoch, session, connectionGeneration)) return
        if (data.isEmpty() || data.size > minOf(244, mtu - 3) || sequence > 0xffffffffL) {
            finish(State.FAILED, true); return
        }
        transport { ports.indicate(session!!, generation, sequence++, data.copyOf()) }
    }

    fun terminal(callbackEpoch: Long, id: String, terminalState: State) {
        if (callbackEpoch != epoch || session != id || state !in setOf(State.BINDING, State.ATTACHED)) return
        require(terminalState in setOf(State.COMPLETE, State.CANCELLED, State.FAILED, State.TIMED_OUT))
        finish(terminalState, false)
    }

    fun cancel() { if (state in setOf(State.BINDING, State.ATTACHED)) finish(State.CANCELLED, true) }
    fun connectionLost(callbackEpoch: Long) {
        if (callbackEpoch == epoch && state in setOf(State.BINDING, State.ATTACHED)) finish(State.FAILED, true)
    }
    fun tick() {
        if (state !in setOf(State.BINDING, State.ATTACHED)) return
        if (nowMillis() >= deadline || (pendingWrite != null && nowMillis() >= writeDeadline)) finish(State.TIMED_OUT, true)
    }
    private fun current(callbackEpoch: Long, id: String?, connectionGeneration: Long): Boolean =
        callbackEpoch == epoch && id == session && connectionGeneration == generation && state == State.ATTACHED

    private fun transport(action: () -> Unit) {
        try { action() } catch (_: RuntimeException) { finish(State.FAILED, true) }
    }

    private fun cleanup(action: () -> Unit) {
        try { action() } catch (_: RuntimeException) { /* Continue releasing the other transport. */ }
    }

    private fun finish(next: State, cancelRemote: Boolean) {
        val oldSession = session
        state = next
        session = null
        generation = 0
        pendingWrite = null
        completionSent = false
        if (cancelRemote && oldSession != null) cleanup { ports.cancel(oldSession) }
        cleanup { ports.disconnectGatt() }
        cleanup { ports.closeRelay() }
    }
}
