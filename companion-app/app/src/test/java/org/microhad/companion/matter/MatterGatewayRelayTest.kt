package org.microhad.companion.matter

import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test

class MatterGatewayRelayTest {
    private val id = "1234567890abcdef"
    private fun snapshot(state: String = "waiting_ble") =
        """{"session":"$id","state":"$state","deadline_remaining_seconds":${if (state == "complete") 0 else 30}${if (state == "complete") ",\"node\":\"0000000000000101\"" else ""}}"""
    private fun reply(call: Long, result: String) = """{"schema":1,"id":$call,"response":{"schema":1,"ok":true,"result":$result}}"""
    private fun write(generation: Int, number: Int, data: String) =
        """{"schema":1,"event":"ble_write","data":{"session":"$id","generation":$generation,"write":$number,"data":"$data"}}"""
    private fun request(text: String, opcode: Int, ident: Long): MatterGatewayWire.Object {
        val value = MatterGatewayWire.decode(text)
        assertEquals(opcode.toLong(), value.number("opcode"))
        assertEquals(ident, value.number("id"))
        assertEquals(id, value.obj("body").text("session"))
        return value.obj("body")
    }

    @Test fun realSocketFragmentsPingAndGattConfirmationsPreserveOwnerOrdering() = runBlocking {
        MatterGatewayTestPeer {
            val socket = websocket()
            request(socket.text(), 4, 1)
            socket.send(reply(1, snapshot()))
            val attached = request(socket.text(), 6, 2)
            assertEquals(247L, attached.number("mtu"))
            socket.send(reply(2, """{"generation":1}"""))
            val event = write(1, 1, "0102")
            socket.send(event.take(20), final = false)
            socket.send("ping", opcode = 9)
            socket.send(event.drop(20), opcode = 0)
            assertTrue(request(socket.text(), 8, 3).flag("success"))
            socket.send(reply(3, """{"write":1}"""))
            socket.send(write(1, 2, "0304"))
            assertEquals(2L, request(socket.text(), 8, 4).number("write"))
            socket.send(reply(4, """{"write":2}"""))
            val indication = request(socket.text(), 7, 5)
            assertEquals(1L, indication.number("sequence"))
            assertEquals("09", indication.text("data"))
            socket.send(reply(5, """{"sequence":1}"""))
            socket.send("""{"schema":1,"event":"commissioning","data":${snapshot("complete")}}""")
            assertEquals(1, socket.pongs)
        }.use { peer ->
            MatterGatewayClient(peer.base).use { gateway ->
                val done = CompletableDeferred<Unit>()
                val payloads = mutableListOf<ByteArray>()
                lateinit var relay: MatterGatewayRelay
                lateinit var model: MatterBleRelayModel
                lateinit var gatt: MatterGattSession
                val epoch = 1L
                model = MatterBleRelayModel(object : MatterBleRelayModel.Ports {
                    override fun writeGatt(epoch: Long, generation: Long, write: Long, data: ByteArray) { gatt.write(write, data) }
                    override fun indicate(session: String, generation: Long, sequence: Long, data: ByteArray) { relay.indicate(generation, sequence, data) }
                    override fun completeWrite(session: String, generation: Long, write: Long, success: Boolean) { relay.completeWrite(generation, write, success) }
                    override fun cancel(session: String) { fail("Synthetic successful flow should not cancel") }
                    override fun disconnectGatt() { gatt.close() }
                    override fun closeRelay() { relay.close() }
                }, { 0 })
                model.begin(id, 30)
                gatt = MatterGattSession(object : MatterGattSession.Ports {
                    override fun discover() = true
                    override fun requestMtu() = true
                    override fun subscribe(): Boolean {
                        assertEquals(1, payloads.size)
                        assertEquals(MatterGattSession.Stage.SUBSCRIBING, gatt.stage)
                        gatt.subscribed(true)
                        return true
                    }
                    override fun write(data: ByteArray): Boolean { payloads.add(data); gatt.written(true); return true }
                    override fun close() { }
                }, object : MatterGattSession.Listener {
                    override fun ready(mtu: Int) { }
                    override fun indication(data: ByteArray) { model.indication(epoch, 1, data) }
                    override fun written(write: Long, success: Boolean) { model.gattWritten(epoch, 1, write, success) }
                    override fun failed() { done.completeExceptionally(AssertionError("Fake GATT failed")) }
                }, { 0 })
                gatt.connected(); gatt.discovered(true); gatt.negotiated(247, true)
                relay = MatterGatewayRelay(gateway, id, this, object : MatterGatewayRelay.Listener {
                    override fun progress(snapshot: MatterGatewayWire.Session) {
                        if (snapshot.terminal) {
                            model.terminal(epoch, id, MatterBleRelayModel.State.COMPLETE)
                            done.complete(Unit)
                        }
                    }
                    override fun attached(generation: Long) { model.attached(epoch, generation, 247) }
                    override fun write(generation: Long, write: Long, data: ByteArray) { model.write(epoch, id, generation, write, data) }
                    override fun writeAccepted(generation: Long, write: Long) {
                        model.writeAcknowledged(epoch, generation, write)
                        if (write == 2L) gatt.indication(byteArrayOf(9))
                    }
                    override fun failed(error: MatterGatewayWire.Error) { done.completeExceptionally(AssertionError(error.wire)) }
                })
                try {
                    relay.open()
                    relay.attach("001122334455", 247)
                    withTimeout(5000) { done.await() }
                    assertEquals(MatterBleRelayModel.State.COMPLETE, model.state)
                    assertEquals(2, payloads.size)
                    assertArrayEquals(byteArrayOf(1, 2), payloads[0])
                    assertArrayEquals(byteArrayOf(3, 4), payloads[1])
                } finally { relay.close() }
            }
            peer.verify()
        }
    }
    @Test fun unknownReplyIdAndWrongSessionFailWithoutReplayingBind() = runBlocking {
        for (response in listOf(reply(2, snapshot()), reply(1, snapshot().replace(id, "0000000000000001")))) {
            MatterGatewayTestPeer {
                val socket = websocket(); request(socket.text(), 4, 1); socket.send(response)
            }.use { peer ->
                MatterGatewayClient(peer.base).use { gateway ->
                    val failed = CompletableDeferred<MatterGatewayWire.Error>()
                    val relay = MatterGatewayRelay(gateway, id, this, listener(failed))
                    try {
                        try { relay.open(); fail("Expected relay protocol failure") }
                        catch (e: MatterGatewayWire.Failure) { assertEquals(MatterGatewayWire.Error.PROTOCOL, e.error) }
                        assertEquals(MatterGatewayWire.Error.PROTOCOL, withTimeout(5000) { failed.await() })
                    } finally { relay.close() }
                }
                peer.verify()
            }
        }
    }
    @Test fun failedGattConfirmationReachesTheGatewayBeforeSocketTeardown() = runBlocking {
        MatterGatewayTestPeer {
            val socket = websocket(); request(socket.text(), 4, 1); socket.send(reply(1, snapshot()))
            request(socket.text(), 6, 2); socket.send(reply(2, """{"generation":1}"""))
            socket.send(write(1, 1, "01"))
            assertFalse(request(socket.text(), 8, 3).flag("success"))
            socket.send("""{"schema":1,"id":3,"response":{"schema":1,"ok":false,"error":"transport"}}""")
        }.use { peer ->
            MatterGatewayClient(peer.base).use { gateway ->
                val done = CompletableDeferred<Unit>()
                lateinit var relay: MatterGatewayRelay
                lateinit var model: MatterBleRelayModel
                model = MatterBleRelayModel(object : MatterBleRelayModel.Ports {
                    override fun writeGatt(epoch: Long, generation: Long, write: Long, data: ByteArray) {
                        model.gattWritten(epoch, generation, write, false)
                    }
                    override fun indicate(session: String, generation: Long, sequence: Long, data: ByteArray) { }
                    override fun completeWrite(session: String, generation: Long, write: Long, success: Boolean) {
                        relay.completeWrite(generation, write, success)
                    }
                    override fun cancel(session: String) { }
                    override fun disconnectGatt() { }
                    override fun closeRelay() { relay.close() }
                }, { 0 })
                val epoch = model.begin(id, 30)
                relay = MatterGatewayRelay(gateway, id, this, object : MatterGatewayRelay.Listener {
                    override fun progress(snapshot: MatterGatewayWire.Session) { }
                    override fun attached(generation: Long) { model.attached(epoch, generation, 247) }
                    override fun write(generation: Long, write: Long, data: ByteArray) { model.write(epoch, id, generation, write, data) }
                    override fun writeAccepted(generation: Long, write: Long) { fail("Failed write must return transport") }
                    override fun failed(error: MatterGatewayWire.Error) {
                        assertEquals(MatterGatewayWire.Error.TRANSPORT, error)
                        model.terminal(epoch, id, MatterBleRelayModel.State.FAILED)
                        done.complete(Unit)
                    }
                })
                try {
                    relay.open(); relay.attach("001122334455", 247)
                    withTimeout(5000) { done.await() }
                    assertEquals(MatterBleRelayModel.State.FAILED, model.state)
                } finally { relay.close() }
            }
            peer.verify()
        }
    }
    @Test fun staleGenerationCannotDeliverAnAccessoryWriteAfterReattach() = runBlocking {
        MatterGatewayTestPeer {
            val socket = websocket(); request(socket.text(), 4, 1); socket.send(reply(1, snapshot()))
            request(socket.text(), 6, 2); socket.send(reply(2, """{"generation":1}"""))
            request(socket.text(), 6, 3); socket.send(reply(3, """{"generation":2}"""))
            socket.send(write(1, 1, "01"))
        }.use { peer ->
            MatterGatewayClient(peer.base).use { gateway ->
                val failed = CompletableDeferred<MatterGatewayWire.Error>()
                val relay = MatterGatewayRelay(gateway, id, this, listener(failed))
                try {
                    relay.open(); relay.attach("001122334455", 247); relay.attach("001122334455", 247)
                    assertEquals(MatterGatewayWire.Error.PROTOCOL, withTimeout(5000) { failed.await() })
                } finally { relay.close() }
            }
            peer.verify()
        }
    }
    @Test fun binaryAndOversizedTextFailClosed() = runBlocking {
        for (kind in listOf(1, 2, 3)) {
            MatterGatewayTestPeer {
                val socket = websocket(); request(socket.text(), 4, 1); socket.send(reply(1, snapshot()))
                if (kind == 3) {
                    socket.send(ByteArray(1200), final = false)
                    socket.send(ByteArray(1200), opcode = 0)
                } else socket.send(ByteArray(if (kind == 1) 2049 else 1), opcode = kind)
            }.use { peer ->
                MatterGatewayClient(peer.base).use { gateway ->
                    val failed = CompletableDeferred<MatterGatewayWire.Error>()
                    val relay = MatterGatewayRelay(gateway, id, this, listener(failed))
                    try {
                        relay.open()
                        assertTrue(withTimeout(5000) { failed.await() } in setOf(MatterGatewayWire.Error.PROTOCOL, MatterGatewayWire.Error.TRANSPORT))
                    } finally { relay.close() }
                }
                peer.verify()
            }
        }
    }
    @Test fun pendingCallsAreBoundedAndCapacityFailureClosesTheOwner() = runBlocking {
        MatterGatewayTestPeer {
            val socket = websocket(); request(socket.text(), 4, 1); socket.send(reply(1, snapshot()))
            request(socket.text(), 6, 2); socket.send(reply(2, """{"generation":1}"""))
        }.use { peer ->
            MatterGatewayClient(peer.base).use { gateway ->
                val failed = CompletableDeferred<MatterGatewayWire.Error>()
                val relay = MatterGatewayRelay(gateway, id, this, listener(failed))
                try {
                    relay.open(); relay.attach("001122334455", 247)
                    for (sequence in 1L..8L) relay.indicate(1, sequence, byteArrayOf(1))
                    try { relay.indicate(1, 9, byteArrayOf(1)); fail("Expected bounded capacity error") }
                    catch (e: MatterGatewayWire.Failure) { assertEquals(MatterGatewayWire.Error.CAPACITY, e.error) }
                    assertEquals(MatterGatewayWire.Error.CAPACITY, failed.await())
                } finally { relay.close() }
            }
            peer.verify()
        }
    }
    @Test fun progressDoesNotRenewAnUnansweredCallDeadline() = runBlocking {
        val indicationReceived = CompletableDeferred<Unit>()
        MatterGatewayTestPeer {
            val socket = websocket(); request(socket.text(), 4, 1); socket.send(reply(1, snapshot()))
            request(socket.text(), 6, 2); socket.send(reply(2, """{"generation":1}"""))
            request(socket.text(), 7, 3)
            socket.send("""{"schema":1,"event":"commissioning","data":${snapshot()}}""")
            indicationReceived.complete(Unit)
        }.use { peer ->
            MatterGatewayClient(peer.base).use { gateway ->
                var now = 0L
                val failed = CompletableDeferred<MatterGatewayWire.Error>()
                val relay = MatterGatewayRelay(gateway, id, this, listener(failed), { now })
                try {
                    relay.open(); relay.attach("001122334455", 247)
                    relay.indicate(1, 1, byteArrayOf(1))
                    withTimeout(5000) { indicationReceived.await() }
                    now = 11000000000L
                    assertEquals(MatterGatewayWire.Error.TIMEOUT, withTimeout(2000) { failed.await() })
                } finally { relay.close() }
            }
            peer.verify()
        }
    }
    private fun listener(failed: CompletableDeferred<MatterGatewayWire.Error>) = object : MatterGatewayRelay.Listener {
        override fun progress(snapshot: MatterGatewayWire.Session) { }
        override fun attached(generation: Long) { }
        override fun write(generation: Long, write: Long, data: ByteArray) { fail("Unexpected accessory write") }
        override fun writeAccepted(generation: Long, write: Long) { }
        override fun failed(error: MatterGatewayWire.Error) { failed.complete(error) }
    }
}
