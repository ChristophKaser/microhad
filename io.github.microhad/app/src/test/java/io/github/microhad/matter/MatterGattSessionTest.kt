package io.github.microhad.matter

import org.junit.Assert.*
import org.junit.Test

class MatterGattSessionTest {
    private val calls = mutableListOf<String>()
    private var now = 0L
    private var reject: String? = null
    private var payload: ByteArray? = null
    private val session = MatterGattSession(object : MatterGattSession.Ports {
        private fun operation(name: String): Boolean { calls.add(name); return reject != name }
        override fun discover() = operation("discover")
        override fun requestMtu() = operation("mtu")
        override fun subscribe() = operation("subscribe")
        override fun write(data: ByteArray): Boolean { payload = data; return operation("write") }
        override fun close() { calls.add("close") }
    }, object : MatterGattSession.Listener {
        override fun ready(mtu: Int) { calls.add("ready:$mtu") }
        override fun indication(data: ByteArray) { payload = data; calls.add("indication") }
        override fun written(write: Long, success: Boolean) { calls.add("written:$write:$success") }
        override fun failed() { calls.add("failed") }
    }, { now })

    private fun ready(mtu: Int = 247) {
        session.connected()
        session.discovered(true)
        session.negotiated(mtu, true)
        session.write(1, byteArrayOf(0x65, 0x6c, 4, 0, 0, 0, 0xf7.toByte(), 0, 6))
        session.written(true)
        session.subscribed(true)
        calls.clear()
    }

    @Test fun capabilitiesWritePrecedesSubscriptionAndEarlyResponseIsForwarded() {
        session.connected()
        session.connected()
        session.subscribed(true) // wrong stage
        session.indication(byteArrayOf(1))
        assertEquals(listOf("discover"), calls)
        session.discovered(true)
        session.negotiated(247, true)
        assertEquals(listOf("discover", "mtu", "ready:247"), calls)
        session.write(1, byteArrayOf(0x65, 0x6c, 4, 0, 0, 0, 0xf7.toByte(), 0, 6))
        assertEquals("write", calls.last())
        session.written(true)
        assertEquals("subscribe", calls.last())
        assertEquals(MatterGattSession.Stage.SUBSCRIBING, session.stage)
        assertFalse(calls.contains("written:1:true"))
        // CHIP may indicate its response before Android delivers the CCCD callback.
        session.indication(byteArrayOf(0x65, 0x6c, 4, 0xf4.toByte(), 0, 6))
        assertEquals("indication", calls.last())
        session.subscribed(true)
        session.subscribed(true)
        assertEquals("written:1:true", calls.last())
        assertEquals(1, calls.count { it == "ready:247" })
        session.write(2, byteArrayOf(1))
        session.written(true)
        session.written(true)
        assertEquals(1, calls.count { it == "subscribe" })
        assertEquals(1, calls.count { it == "written:2:true" })
    }

    @Test fun subscriptionFailureReportsFailedFirstWrite() {
        session.connected()
        session.discovered(true)
        session.negotiated(247, true)
        session.write(1, byteArrayOf(0x65, 0x6c))
        session.written(true)
        session.subscribed(false)
        assertEquals(listOf("close", "written:1:false", "failed"), calls.takeLast(3))
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
    }

    @Test fun rejectedSubscriptionReportsFailedFirstWrite() {
        reject = "subscribe"
        session.connected()
        session.discovered(true)
        session.negotiated(247, true)
        session.write(1, byteArrayOf(0x65, 0x6c))
        session.written(true)
        assertEquals(listOf("subscribe", "close", "written:1:false", "failed"), calls.takeLast(4))
    }

    @Test fun mtuRejectionAndFailedNegotiationFallBackTo23() {
        reject = "mtu"
        session.connected()
        session.discovered(true)
        assertEquals(MatterGattSession.Stage.READY, session.stage)
        session.negotiated(517, true) // delayed callback after fallback cannot alter the advertised MTU
        session.subscribed(true)
        assertEquals("ready:23", calls.last())
        session.write(1, ByteArray(21))
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
        assertFalse(calls.contains("write"))
    }

    @Test fun failedMtuCallbackAlsoFallsBackTo23() {
        session.connected()
        session.discovered(true)
        session.negotiated(0, false)
        session.subscribed(true)
        assertEquals("ready:23", calls.last())
    }

    @Test fun invalidDiscoveryAndSubscriptionFailOnce() {
        session.connected()
        session.discovered(false)
        session.fail()
        session.connected()
        session.subscribed(true)
        assertEquals(listOf("discover", "close", "failed"), calls)
    }

    @Test fun rejectedWriteReportsFailedCompletionAndReleasesConnection() {
        ready()
        reject = "write"
        session.write(3, byteArrayOf(1))
        assertEquals(listOf("write", "close", "written:3:false", "failed"), calls.takeLast(4))
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
    }

    @Test fun onlyOneGattWriteCanBePending() {
        ready()
        session.write(1, byteArrayOf(1))
        session.write(2, byteArrayOf(2))
        assertEquals(1, calls.count { it == "write" })
        assertEquals(listOf("close", "failed"), calls.takeLast(2))
    }

    @Test fun callbackPayloadsAreCopiedAndMtuCeilingIsEnforced() {
        ready(517)
        val data = byteArrayOf(5)
        session.indication(data)
        data[0] = 6
        assertArrayEquals(byteArrayOf(5), payload)
        session.write(1, data)
        data[0] = 7
        assertArrayEquals(byteArrayOf(6), payload)
        session.written(true)
        session.indication(ByteArray(245))
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
    }

    @Test fun connectionAndWriteCallbacksHaveBoundedDeadlines() {
        now = 9999
        session.tick()
        assertEquals(MatterGattSession.Stage.CONNECTING, session.stage)
        now = 10000
        session.tick()
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
        assertEquals(listOf("close", "failed"), calls)
    }

    @Test fun pendingWriteDeadlineDoesNotExtendForIncomingIndications() {
        ready()
        session.write(1, byteArrayOf(1))
        now = 9000
        session.indication(byteArrayOf(2))
        now = 10000
        session.tick()
        assertEquals(MatterGattSession.Stage.CLOSED, session.stage)
        session.written(true)
        assertFalse(calls.contains("written:1:true"))
    }

    @Test fun intentionalCloseSuppressesLateCallbacksAndFailure() {
        ready()
        session.close()
        session.close()
        session.fail()
        session.indication(byteArrayOf(1))
        assertEquals(1, calls.count { it == "close" })
        assertFalse(calls.contains("failed"))
    }
}
