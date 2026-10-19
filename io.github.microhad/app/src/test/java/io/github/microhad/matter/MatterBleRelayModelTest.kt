package io.github.microhad.matter

import org.junit.Assert.*
import org.junit.Test

class MatterBleRelayModelTest {
    private class Fake : MatterBleRelayModel.Ports {
        val calls = mutableListOf<String>()
        val payloads = mutableListOf<ByteArray>()
        var throwOn: String? = null
        private fun record(call: String) {
            calls.add(call)
            if (call.substringBefore(':') == throwOn) throw IllegalStateException("fake transport failure")
        }
        override fun writeGatt(epoch: Long, generation: Long, write: Long, data: ByteArray) {
            payloads.add(data); record("gatt:$epoch:$generation:$write")
        }
        override fun indicate(session: String, generation: Long, sequence: Long, data: ByteArray) {
            payloads.add(data); record("indicate:$generation:$sequence")
        }
        override fun completeWrite(session: String, generation: Long, write: Long, success: Boolean) {
            record("complete:$generation:$write:$success")
        }
        override fun cancel(session: String) { record("cancel:$session") }
        override fun disconnectGatt() { record("disconnect") }
        override fun closeRelay() { record("close") }
    }
    private val id = "1234567890abcdef"
    private var now = 0L
    private val fake = Fake()
    private val relay = MatterBleRelayModel(fake) { now }
    private fun attach(mtu: Int = 247): Long = relay.begin(id, 30).also { relay.attached(it, 1, mtu) }

    @Test fun writesWaitForGattCallbackAndMatchingGatewayAcknowledgement() {
        val epoch = attach()
        relay.write(epoch, id, 1, 1, byteArrayOf(5))
        assertEquals(listOf("gatt:1:1:1"), fake.calls)
        relay.writeAcknowledged(epoch, 1, 1) // premature ack cannot clear the pending write
        relay.gattWritten(epoch, 1, 2, true)
        relay.gattWritten(epoch, 2, 1, true)
        assertEquals(1, fake.calls.size)
        relay.gattWritten(epoch, 1, 1, true)
        relay.gattWritten(epoch, 1, 1, true)
        assertEquals(listOf("gatt:1:1:1", "complete:1:1:true"), fake.calls)
        relay.writeAcknowledged(epoch, 1, 1)
        relay.write(epoch, id, 1, 2, byteArrayOf(6))
        assertEquals("gatt:1:1:2", fake.calls.last())
    }

    @Test fun reattachRejectsOldGenerationAndNewAttemptRejectsOldEpoch() {
        val old = attach()
        relay.write(old, id, 1, 1, byteArrayOf(1))
        relay.attached(old, 2, 247)
        relay.gattWritten(old, 1, 1, true)
        relay.indication(old, 1, byteArrayOf(2))
        relay.write(old, id, 1, 2, byteArrayOf(3))
        assertEquals(1, fake.calls.size)
        relay.indication(old, 2, byteArrayOf(4))
        assertEquals("indicate:2:1", fake.calls.last())
        relay.cancel()
        val current = attach()
        fake.calls.clear()
        relay.connectionLost(old)
        relay.terminal(old, id, MatterBleRelayModel.State.FAILED)
        relay.indication(old, 1, byteArrayOf(5))
        relay.write(current, "0000000000000001", 1, 1, byteArrayOf(6))
        assertTrue(fake.calls.isEmpty())
        assertEquals(MatterBleRelayModel.State.ATTACHED, relay.state)
    }

    @Test fun payloadCopiesAndIndicationSequenceRemainIndependent() {
        val epoch = attach()
        val data = byteArrayOf(7)
        relay.indication(epoch, 1, data)
        relay.indication(epoch, 1, data)
        relay.write(epoch, id, 1, 1, data)
        data[0] = 9
        assertTrue(fake.payloads.all { it.contentEquals(byteArrayOf(7)) })
        assertEquals(listOf("indicate:1:1", "indicate:1:2", "gatt:1:1:1"), fake.calls)
    }

    @Test fun minimumMtuAndProtocolCeilingAreEnforced() {
        var epoch = attach(23)
        relay.indication(epoch, 1, ByteArray(20))
        assertEquals(MatterBleRelayModel.State.ATTACHED, relay.state)
        relay.indication(epoch, 1, ByteArray(21))
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
        epoch = attach(517)
        relay.write(epoch, id, 1, 1, ByteArray(245))
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
        epoch = attach()
        relay.indication(epoch, 1, byteArrayOf())
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
    }

    @Test fun extraWriteBeforeGatewayAckFailsClosed() {
        val epoch = attach()
        relay.write(epoch, id, 1, 1, byteArrayOf(1))
        relay.gattWritten(epoch, 1, 1, true)
        relay.write(epoch, id, 1, 2, byteArrayOf(2))
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
        assertEquals(1, fake.calls.count { it.startsWith("gatt:") })
        assertTrue(fake.calls.contains("cancel:$id"))
    }

    @Test fun failedGattWriteIsReportedOnceAndWaitsForTheRemoteFailureBeforeClosing() {
        val epoch = attach()
        relay.write(epoch, id, 1, 1, byteArrayOf(1))
        relay.gattWritten(epoch, 1, 1, false)
        relay.gattWritten(epoch, 1, 1, false)
        assertTrue(relay.awaitingFailedWrite)
        assertEquals(listOf("gatt:1:1:1", "complete:1:1:false"), fake.calls)
        relay.terminal(epoch, id, MatterBleRelayModel.State.FAILED)
        assertEquals(listOf("gatt:1:1:1", "complete:1:1:false", "disconnect", "close"), fake.calls)
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
        assertFalse(relay.awaitingFailedWrite)
    }

    @Test fun aLostFailedWriteReplyStillExpiresAndReleasesBothTransports() {
        val epoch = attach()
        relay.write(epoch, id, 1, 1, byteArrayOf(1))
        relay.gattWritten(epoch, 1, 1, false)
        now = 10000
        relay.tick()
        assertEquals(MatterBleRelayModel.State.TIMED_OUT, relay.state)
        assertEquals(listOf("cancel:$id", "disconnect", "close"), fake.calls.takeLast(3))
    }

    @Test fun deadlinesDoNotRenewOnCallbacksOrIndications() {
        val epoch = attach()
        now = 20000
        relay.indication(epoch, 1, byteArrayOf(1))
        now = 30000
        relay.tick()
        assertEquals(MatterBleRelayModel.State.TIMED_OUT, relay.state)
        now = 40000
        val next = attach()
        relay.write(next, id, 1, 1, byteArrayOf(1))
        relay.gattWritten(next, 1, 1, true)
        now = 50000 // write ack never arrives
        relay.tick()
        assertEquals(MatterBleRelayModel.State.TIMED_OUT, relay.state)
    }

    @Test fun completedSessionSurvivesPhoneLossWithoutRemoteCancel() {
        val epoch = attach()
        relay.terminal(epoch, id, MatterBleRelayModel.State.COMPLETE)
        relay.connectionLost(epoch)
        relay.cancel()
        assertEquals(MatterBleRelayModel.State.COMPLETE, relay.state)
        assertEquals(listOf("disconnect", "close"), fake.calls)
    }

    @Test fun transportExceptionsStillReleaseBothOwners() {
        val epoch = attach()
        fake.throwOn = "gatt"
        relay.write(epoch, id, 1, 1, byteArrayOf(1))
        assertEquals(MatterBleRelayModel.State.FAILED, relay.state)
        assertEquals(listOf("cancel:$id", "disconnect", "close"), fake.calls.takeLast(3))
        fake.throwOn = "cancel"
        attach()
        relay.cancel()
        assertEquals(listOf("disconnect", "close"), fake.calls.takeLast(2))
        fake.throwOn = "disconnect"
        attach()
        relay.cancel()
        assertEquals("close", fake.calls.last())
    }
}
