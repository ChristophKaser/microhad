package io.github.microhad.matter

import kotlinx.coroutines.*

/** Serialized foreground owner. Only bounded HTTP cleanup can outlive the screen. */
internal class MatterOnboardingController(
    private val gateway: Gateway,
    private val bleFactory: (MatterBleAdapter.Listener) -> Ble,
    parent: CoroutineScope,
    private val now: () -> Long,
    private var render: ((Snapshot) -> Unit)?,
) : AutoCloseable {
    interface Gateway : AutoCloseable {
        suspend fun availability(): MatterGatewayWire.Availability
        suspend fun createThread()
        suspend fun start(discriminator: Int, pin: Int, timeoutSeconds: Int): MatterGatewayWire.Session
        suspend fun cancel(session: String)
        fun relay(session: String, scope: CoroutineScope, listener: MatterGatewayRelay.Listener): Relay
    }
    interface Ble : AutoCloseable {
        fun scan(matches: (MatterAdvertisement) -> Boolean)
        fun stopScan()
        fun connect(address: String)
        fun write(write: Long, data: ByteArray)
    }
    interface Relay : AutoCloseable {
        suspend fun open()
        suspend fun attach(address: String, mtu: Int)
        fun indicate(generation: Long, sequence: Long, data: ByteArray)
        fun completeWrite(generation: Long, write: Long, success: Boolean)
    }
    enum class State {
        STOPPED, CHECKING, DISABLED, UNAVAILABLE, NEEDS_THREAD, READY, SCANNING, SELECTING,
        STARTING, CONNECTING, BINDING, ACTIVE, COMPLETE, CANCELLED, FAILED, TIMED_OUT
    }
    data class Candidate(val address: String, val advertisement: MatterAdvertisement, val rssi: Int)
    data class Snapshot(val state: State, val phase: String? = null, val remainingSeconds: Int = 0,
                        val error: String? = null, val node: String? = null,
                        val candidates: List<Candidate> = emptyList(), val cleaning: Boolean = false,
                        val sessionId: String? = null)
    var snapshot = Snapshot(State.STOPPED)
        private set
    private val scope = CoroutineScope(parent.coroutineContext + SupervisorJob())
    private var foreground = false
    private var closed = false
    private var gatewayClosed = false
    private var work = 0
    private var readJob: Job? = null
    private var startPending = false
    private var cancelPending = false
    private var threadPending = false
    private var attempt = 0L
    private var setup: MatterSetupCode? = null
    private var ble: Ble? = null
    private var relay: Relay? = null
    private var session: String? = null
    private var generation = 0L
    private var deadline = 0L
    private var scanDeadline = 0L
    private val model = MatterBleRelayModel(object : MatterBleRelayModel.Ports {
        override fun writeGatt(epoch: Long, generation: Long, write: Long, data: ByteArray) {
            (ble ?: throw IllegalStateException("BLE_UNAVAILABLE")).write(write, data)
        }
        override fun indicate(session: String, generation: Long, sequence: Long, data: ByteArray) {
            (relay ?: throw IllegalStateException("RELAY_UNAVAILABLE")).indicate(generation, sequence, data)
        }
        override fun completeWrite(session: String, generation: Long, write: Long, success: Boolean) {
            (relay ?: throw IllegalStateException("RELAY_UNAVAILABLE")).completeWrite(generation, write, success)
        }
        override fun cancel(session: String) { cancelRemote(session) }
        override fun disconnectGatt() { ble?.close(); ble = null }
        override fun closeRelay() { relay?.close(); relay = null }
    }, now)
    private val cleaning get() = startPending || cancelPending || threadPending

    fun resume() {
        if (closed) return
        foreground = true
        if (snapshot.state == State.COMPLETE || cleaning) emit() else refresh()
    }
    fun refresh() {
        if (!foreground || closed || cleaning || busy()) return
        readJob?.cancel()
        attempt++
        val owner = attempt
        setup?.close(); setup = null
        snapshot = Snapshot(State.CHECKING); emit()
        readJob = launch {
            try {
                val status = withTimeout(10000) { gateway.availability() }
                if (current(owner)) {
                    snapshot = Snapshot(when {
                        !status.enabled -> State.DISABLED
                        !status.configured -> State.NEEDS_THREAD
                        !status.ready -> State.UNAVAILABLE
                        else -> State.READY
                    }); emit()
                }
            } catch (e: TimeoutCancellationException) {
                if (current(owner)) { snapshot = Snapshot(State.UNAVAILABLE, error = error(e)); emit() }
            } catch (_: CancellationException) { }
            catch (e: Exception) { if (current(owner)) { snapshot = Snapshot(State.UNAVAILABLE, error = error(e)); emit() } }
        }
    }
    fun createThread() {
        if (!foreground || closed || cleaning || snapshot.state != State.NEEDS_THREAD) return
        threadPending = true
        val owner = attempt
        snapshot = Snapshot(State.CHECKING); emit()
        launch {
            var created = false
            try {
                withTimeout(10000) { gateway.createThread() }
                created = true
                if (current(owner)) snapshot = Snapshot(State.READY)
            } catch (e: Exception) {
                if (current(owner)) snapshot = Snapshot(State.UNAVAILABLE, error = error(e))
            } finally {
                threadPending = false
                if (current(owner) && created) { snapshot = Snapshot(State.STOPPED); refresh() }
                settle()
            }
        }
    }
    fun begin(input: String) {
        if (!foreground || closed || cleaning || snapshot.state != State.READY) return
        val code = try { MatterSetupCode.parse(input) } catch (e: MatterSetupCode.Invalid) {
            snapshot = Snapshot(State.FAILED, error = "setup_${e.code.name.lowercase()}"); emit(); return
        }
        if (code.commissioningFlow == 2 || code.bleAdvertised == false) {
            code.close()
            snapshot = Snapshot(State.FAILED, error = if (code.commissioningFlow == 2) "setup_custom_flow" else "setup_non_ble")
            emit(); return
        }
        attempt++
        val owner = attempt
        setup = code
        scanDeadline = now() + 60000
        snapshot = Snapshot(State.SCANNING); emit()
        try {
            ble = bleFactory(object : MatterBleAdapter.Listener {
                override fun candidate(address: String, advertisement: MatterAdvertisement, rssi: Int) {
                    if (!current(owner) || snapshot.state != State.SCANNING || !advertisement.matches(code) ||
                        !address.matches(Regex("[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}")) || snapshot.candidates.size >= 32 ||
                        snapshot.candidates.any { it.address == address }) return
                    snapshot = snapshot.copy(candidates = snapshot.candidates + Candidate(address, advertisement, rssi)); emit()
                }
                override fun ready(mtu: Int) {
                    if (!current(owner) || snapshot.state != State.CONNECTING) return
                    try { bind(owner, mtu) } catch (e: Exception) { fail(error(e)) }
                }
                override fun indication(data: ByteArray) {
                    if (!current(owner)) return
                    if (generation == 0L) fail("ble_protocol") else { model.indication(model.epoch, generation, data); modelState() }
                }
                override fun written(write: Long, success: Boolean) {
                    if (!current(owner) || generation == 0L) return
                    model.gattWritten(model.epoch, generation, write, success); modelState()
                }
                override fun failed(error: MatterBleAdapter.Error) {
                    if (!current(owner)) return
                    if (error == MatterBleAdapter.Error.SCAN_TIMEOUT && snapshot.state == State.SCANNING && snapshot.candidates.isNotEmpty()) {
                        snapshot = snapshot.copy(state = State.SELECTING); emit()
                    } else if (!model.awaitingFailedWrite) fail("ble_${error.name.lowercase()}")
                }
            })
            ble!!.scan { it.matches(code) }
        } catch (_: Exception) { fail("ble_scan") }
    }
    private var selected: Candidate? = null
    fun select(address: String) {
        if (!foreground || closed || snapshot.state !in setOf(State.SCANNING, State.SELECTING)) return
        val candidate = snapshot.candidates.firstOrNull { it.address == address } ?: return
        val code = setup ?: return
        val owner = attempt
        selected = candidate
        ble?.stopScan()
        var pin = code.consumePin()
        code.close(); setup = null
        startPending = true
        snapshot = Snapshot(State.STARTING); emit()
        launch {
            try {
                val result = withTimeout(10000) { gateway.start(candidate.advertisement.discriminator, pin, 120) }
                if (!current(owner)) { if (!result.terminal) cancelRemote(result.id); return@launch }
                session = result.id
                deadline = now() + result.remainingSeconds * 1000L
                model.begin(result.id, 120)
                if (result.terminal) progress(result) else {
                    snapshot = Snapshot(State.CONNECTING, result.state, result.remainingSeconds, sessionId = result.id); emit()
                    ble?.connect(candidate.address)
                }
            } catch (e: Exception) { if (current(owner)) fail(error(e)) }
            finally { pin = 0; startPending = false; settle() }
        }
    }
    private fun bind(owner: Long, mtu: Int) {
        val id = session ?: return
        val candidate = selected ?: return
        snapshot = snapshot.copy(state = State.BINDING); emit()
        val connection = gateway.relay(id, scope, object : MatterGatewayRelay.Listener {
            override fun progress(snapshot: MatterGatewayWire.Session) {
                if (current(owner)) this@MatterOnboardingController.progress(snapshot)
            }
            override fun attached(generation: Long) {
                if (!current(owner)) return
                this@MatterOnboardingController.generation = generation
                model.attached(model.epoch, generation, mtu)
                snapshot = snapshot.copy(state = State.ACTIVE); emit()
            }
            override fun write(generation: Long, write: Long, data: ByteArray) {
                if (current(owner)) { model.write(model.epoch, id, generation, write, data); modelState() }
            }
            override fun writeAccepted(generation: Long, write: Long) {
                if (current(owner)) { model.writeAcknowledged(model.epoch, generation, write); modelState() }
            }
            override fun failed(error: MatterGatewayWire.Error) { if (current(owner)) fail(error.wire) }
        })
        relay = connection
        launch {
            try { connection.open(); if (current(owner)) connection.attach(candidate.address.replace(":", "").lowercase(), mtu) }
            catch (e: Exception) { if (current(owner)) fail(error(e)) }
        }
    }
    private fun progress(value: MatterGatewayWire.Session) {
        if (value.id != session) { fail("protocol"); return }
        deadline = minOf(deadline, now() + value.remainingSeconds * 1000L)
        if (value.terminal) {
            val state = when (value.state) {
                "complete" -> State.COMPLETE
                "cancelled" -> State.CANCELLED
                "timed_out" -> State.TIMED_OUT
                else -> State.FAILED
            }
            end(state, value.error?.wire, value.node, false)
        } else { snapshot = snapshot.copy(phase = value.state, remainingSeconds = value.remainingSeconds); emit() }
    }
    fun tick() {
        if (!foreground || closed) return
        if (snapshot.state in setOf(State.SCANNING, State.SELECTING) && now() >= scanDeadline) fail("ble_scan_timeout")
        if (session != null && now() >= deadline) end(State.TIMED_OUT, "timeout", null, true)
        model.tick(); modelState()
        if (session != null) { snapshot = snapshot.copy(remainingSeconds = ((deadline - now()).coerceAtLeast(0) + 999).div(1000).toInt()); emit() }
    }
    private fun modelState() {
        if (session == null) return
        if (model.state == MatterBleRelayModel.State.FAILED) fail("transport")
        if (model.state == MatterBleRelayModel.State.TIMED_OUT) end(State.TIMED_OUT, "timeout", null, true)
    }
    private fun fail(error: String) = end(State.FAILED, error, null, true)
    fun cancel() { if (!closed && snapshot.state != State.COMPLETE) end(State.CANCELLED, null, null, true) }
    private fun end(state: State, error: String?, node: String?, cancelRemote: Boolean) {
        val id = session
        if (cancelRemote) {
            if (model.state in setOf(MatterBleRelayModel.State.BINDING, MatterBleRelayModel.State.ATTACHED)) model.cancel()
            else if (id != null) cancelRemote(id)
        } else if (id != null) {
            model.terminal(model.epoch, id, when (state) {
                State.COMPLETE -> MatterBleRelayModel.State.COMPLETE
                State.CANCELLED -> MatterBleRelayModel.State.CANCELLED
                State.TIMED_OUT -> MatterBleRelayModel.State.TIMED_OUT
                else -> MatterBleRelayModel.State.FAILED
            })
        }
        attempt++
        setup?.close(); setup = null
        ble?.close(); ble = null
        relay?.close(); relay = null
        session = null; generation = 0; selected = null
        snapshot = Snapshot(state, error = error, node = node, sessionId = id); emit()
    }
    private fun cancelRemote(id: String) {
        if (cancelPending) return
        cancelPending = true; emit()
        launch {
            try { withTimeout(10000) { gateway.cancel(id) } } catch (_: Exception) { /* No mutation replay. */ }
            finally { cancelPending = false; settle() }
        }
    }
    fun stop() {
        if (closed) return
        foreground = false
        readJob?.cancel()
        if (snapshot.state != State.COMPLETE) { end(State.STOPPED, null, null, true) }
        else { setup?.close(); setup = null; ble?.close(); ble = null; relay?.close(); relay = null }
    }
    override fun close() {
        if (closed) return
        stop(); closed = true; render = null; maybeClose()
    }
    private fun launch(action: suspend () -> Unit): Job {
        work++
        return scope.launch { try { action() } finally { work--; maybeClose() } }
    }
    private fun settle() {
        if (foreground && !closed && !cleaning && snapshot.state == State.STOPPED) refresh() else emit()
    }
    private fun maybeClose() {
        if (closed && work == 0 && !gatewayClosed) { gatewayClosed = true; gateway.close(); scope.cancel() }
    }
    private fun current(owner: Long) = foreground && !closed && owner == attempt
    private fun busy() = snapshot.state in setOf(State.SCANNING, State.SELECTING, State.STARTING, State.CONNECTING, State.BINDING, State.ACTIVE)
    private var lastDiagnostic = ""
    private fun emit() {
        snapshot = snapshot.copy(cleaning = cleaning)
        val diagnostic = "onboarding state=${snapshot.state} phase=${snapshot.phase} session=${snapshot.sessionId ?: session} generation=$generation error=${snapshot.error} cleaning=$cleaning"
        if (diagnostic != lastDiagnostic) { MatterDiagnostics.record(diagnostic); lastDiagnostic = diagnostic }
        if (!closed) render?.invoke(snapshot)
    }
    private fun error(exception: Exception) = when (exception) {
        is MatterGatewayWire.Failure -> exception.error.wire
        is TimeoutCancellationException -> "timeout"
        else -> "transport"
    }
}
