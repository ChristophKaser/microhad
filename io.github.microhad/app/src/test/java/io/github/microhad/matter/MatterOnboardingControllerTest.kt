package io.github.microhad.matter

import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test

class MatterOnboardingControllerTest {
    private val id = "1234567890abcdef"
    private val address = "AA:BB:CC:DD:EE:FF"
    private val advertised = MatterAdvertisement(3858, 65521, 32768)
    private fun session(state: String = "waiting_ble", seconds: Int = 120) =
        MatterGatewayWire.Session(id, state, seconds, node = if (state == "complete") "0000000000000101" else null)

    private inner class Fixture : AutoCloseable, MatterOnboardingController.Gateway {
        var available = MatterGatewayWire.Availability(true, true, true)
        var time = 0L
        var starts = 0
        var creates = 0
        var closes = 0
        var startWait: CompletableDeferred<MatterGatewayWire.Session>? = null
        var cancelWait: CompletableDeferred<Unit>? = null
        var startFailure = false
        var relayFailure = false
        val cancelled = mutableListOf<String>()
        val bles = mutableListOf<FakeBle>()
        val relays = mutableListOf<FakeRelay>()
        val controller = MatterOnboardingController(this, { FakeBle(it).also(bles::add) },
            CoroutineScope(Dispatchers.Unconfined), { time }, {})
        override suspend fun availability() = available
        override suspend fun createThread() { creates++; available = available.copy(configured = true) }
        override suspend fun start(discriminator: Int, pin: Int, timeoutSeconds: Int): MatterGatewayWire.Session {
            starts++
            assertEquals(3858, discriminator)
            assertEquals(20202021, pin)
            assertEquals(120, timeoutSeconds)
            if (startFailure) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TRANSPORT)
            return startWait?.await() ?: session()
        }
        override suspend fun cancel(session: String) { cancelled.add(session); cancelWait?.await() }
        override fun relay(session: String, scope: CoroutineScope, listener: MatterGatewayRelay.Listener): MatterOnboardingController.Relay {
            if (relayFailure) throw IllegalStateException("fixture")
            assertEquals(id, session)
            return FakeRelay(listener).also(relays::add)
        }
        fun scan() { controller.resume(); controller.begin("34970112332") }
        fun select() { bles.last().listener.candidate(address, advertised, -40); controller.select(address) }
        fun active() { scan(); select(); bles.last().listener.ready(247) }
        override fun close() { closes++ }
        fun dispose() { controller.close() }
    }
    private class FakeBle(val listener: MatterBleAdapter.Listener) : MatterOnboardingController.Ble {
        var connects = 0
        var closes = 0
        val writes = mutableListOf<Long>()
        override fun scan(matches: (MatterAdvertisement) -> Boolean) { }
        override fun stopScan() { }
        override fun connect(address: String) { connects++ }
        override fun write(write: Long, data: ByteArray) { writes.add(write) }
        override fun close() { closes++ }
    }
    private class FakeRelay(val listener: MatterGatewayRelay.Listener) : MatterOnboardingController.Relay {
        var closes = 0
        val confirmations = mutableListOf<Boolean>()
        override suspend fun open() { }
        override suspend fun attach(address: String, mtu: Int) {
            assertEquals("aabbccddeeff", address); assertEquals(247, mtu); listener.attached(1)
        }
        override fun indicate(generation: Long, sequence: Long, data: ByteArray) { }
        override fun completeWrite(generation: Long, write: Long, success: Boolean) { confirmations.add(success) }
        override fun close() { closes++ }
    }

    @Test fun featureAndNetworkGatesRequireExplicitThreadCreation() {
        val f = Fixture()
        try {
            f.available = MatterGatewayWire.Availability(false, false, false)
            f.controller.resume(); assertEquals(MatterOnboardingController.State.DISABLED, f.controller.snapshot.state)
            f.controller.begin("34970112332"); assertTrue(f.bles.isEmpty())
            f.available = MatterGatewayWire.Availability(true, true, false)
            f.controller.refresh(); assertEquals(MatterOnboardingController.State.NEEDS_THREAD, f.controller.snapshot.state)
            assertEquals(0, f.creates)
            f.controller.createThread(); assertEquals(1, f.creates)
            assertEquals(MatterOnboardingController.State.READY, f.controller.snapshot.state)
            f.controller.createThread(); assertEquals(1, f.creates)
            f.available = f.available.copy(ready = false)
            f.controller.refresh(); assertEquals(MatterOnboardingController.State.UNAVAILABLE, f.controller.snapshot.state)
        } finally { f.dispose() }
    }

    @Test fun selectionUsesFullAdvertisementAndIgnoresUnknownCandidates() {
        val f = Fixture()
        try {
            f.scan()
            f.bles.last().listener.candidate(address, advertised.copy(discriminator = 3839), -40)
            f.controller.select(address); assertEquals(0, f.starts)
            f.select(); assertEquals(1, f.starts); assertEquals(1, f.bles.last().connects)
            assertEquals(id, f.controller.snapshot.sessionId)
            assertFalse(f.controller.snapshot.toString().contains("20202021"))
            f.controller.select(address); assertEquals(1, f.starts)
        } finally { f.dispose() }
    }

    @Test fun lateStartAfterBackgroundIsCancelledBeforeGatewayCloses() {
        val f = Fixture()
        f.startWait = CompletableDeferred(); f.cancelWait = CompletableDeferred()
        f.scan(); f.select(); f.controller.close()
        assertEquals(0, f.closes)
        f.startWait!!.complete(session())
        assertEquals(listOf(id), f.cancelled); assertEquals(0, f.bles.last().connects)
        assertEquals(0, f.closes)
        f.cancelWait!!.complete(Unit); assertEquals(1, f.closes)
    }

    @Test fun staleCallbacksCannotAffectANewAttempt() {
        val f = Fixture()
        try {
            f.active(); val oldBle = f.bles.last(); val oldRelay = f.relays.last()
            f.controller.stop(); assertEquals(listOf(id), f.cancelled)
            f.scan()
            oldBle.listener.ready(247); oldBle.listener.failed(MatterBleAdapter.Error.GATT)
            oldRelay.listener.progress(session("complete", 0))
            assertEquals(MatterOnboardingController.State.SCANNING, f.controller.snapshot.state)
            assertEquals(1, f.relays.size)
        } finally { f.dispose() }
    }

    @Test fun completeSurvivesPhoneStopWithoutCancellingTheFabric() {
        val f = Fixture()
        f.active(); f.relays.last().listener.progress(session("complete", 0))
        assertEquals(MatterOnboardingController.State.COMPLETE, f.controller.snapshot.state)
        assertEquals("0000000000000101", f.controller.snapshot.node)
        assertEquals(id, f.controller.snapshot.sessionId)
        f.controller.stop(); f.controller.resume(); f.dispose()
        assertEquals(MatterOnboardingController.State.COMPLETE, f.controller.snapshot.state)
        assertTrue(f.cancelled.isEmpty()); assertEquals(1, f.closes)
    }

    @Test fun failedGattConfirmationRemainsOpenUntilGatewayFailure() {
        val f = Fixture()
        try {
            f.active(); val relay = f.relays.last(); val ble = f.bles.last()
            relay.listener.write(1, 1, byteArrayOf(1))
            ble.listener.written(1, false); ble.listener.failed(MatterBleAdapter.Error.GATT)
            assertEquals(listOf(false), relay.confirmations)
            assertEquals(0, relay.closes)
            assertEquals(MatterOnboardingController.State.ACTIVE, f.controller.snapshot.state)
            relay.listener.failed(MatterGatewayWire.Error.TRANSPORT)
            assertEquals(MatterOnboardingController.State.FAILED, f.controller.snapshot.state)
            assertTrue(relay.closes > 0)
        } finally { f.dispose() }
    }

    @Test fun progressCannotRenewTheOverallDeadline() {
        val f = Fixture()
        try {
            f.active(); f.time = 100000
            f.relays.last().listener.progress(session("pase", 120))
            f.time = 120000; f.controller.tick()
            assertEquals(MatterOnboardingController.State.TIMED_OUT, f.controller.snapshot.state)
            assertEquals(listOf(id), f.cancelled)
        } finally { f.dispose() }
    }

    @Test fun retainedScanCandidatesExpireAndNeverStartAutomatically() {
        val f = Fixture()
        try {
            f.scan(); f.bles.last().listener.candidate(address, advertised, -40)
            f.bles.last().listener.failed(MatterBleAdapter.Error.SCAN_TIMEOUT)
            assertEquals(MatterOnboardingController.State.SELECTING, f.controller.snapshot.state)
            f.time = 60000; f.controller.tick()
            assertEquals(MatterOnboardingController.State.FAILED, f.controller.snapshot.state)
            assertEquals(0, f.starts)
        } finally { f.dispose() }
    }

    @Test fun uncertainStartFailureIsNotReplayed() {
        val f = Fixture()
        try {
            f.startFailure = true; f.scan(); f.select()
            assertEquals(MatterOnboardingController.State.FAILED, f.controller.snapshot.state)
            repeat(4) { f.controller.tick() }
            assertEquals(1, f.starts); assertTrue(f.cancelled.isEmpty())
        } finally { f.dispose() }
    }

    @Test fun relayConstructionFailureClosesGattAndCancelsKnownSession() {
        val f = Fixture()
        try {
            f.relayFailure = true; f.active()
            assertEquals(MatterOnboardingController.State.FAILED, f.controller.snapshot.state)
            assertEquals(listOf(id), f.cancelled)
            assertTrue(f.bles.last().closes > 0)
        } finally { f.dispose() }
    }
}
