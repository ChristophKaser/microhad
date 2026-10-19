package io.github.microhad.matter

import org.junit.Assert.*
import org.junit.Test

class MatterGatewayWireTest {
    private fun malformed(action: () -> Unit) {
        try { action(); fail("Expected fixed protocol error") } catch (e: MatterGatewayWire.Failure) {
            assertEquals(MatterGatewayWire.Error.PROTOCOL, e.error)
            assertEquals("protocol", e.message)
        }
    }
    @Test fun strictParserRejectsAmbiguousAndUnboundedJson() {
        for (input in listOf("{} trailing", "{\"a\":1,\"a\":2}", "{\"a\":01}", "{\"a\":-1}",
            "{\"a\":1.0}", "{\"a\":1e1}", "{\"a\":null}", "{\"a\":[]}", "{\"a\":true,}",
            "{\"a\":\"\\u0061\"}", "{\"a\":\"é\"}", "{\"a\":\"" + "x".repeat(513) + "\"}",
            " ".repeat(2049), "{\"a\":".repeat(10) + "1" + "}".repeat(10))) {
            malformed { MatterGatewayWire.decode(input) }
        }
        assertEquals(1L, MatterGatewayWire.decode(" \n{\"schema\" : 1}\t").number("schema"))
    }
    @Test fun jobCompletionIsRpcDeliveryRatherThanCommissioningCompletion() {
        val job = MatterGatewayWire.accepted(MatterGatewayWire.decode("""{"schema":1,"ok":true,"job":4294967295,"state":"queued"}"""))
        assertEquals(4294967295L, job)
        assertNull(MatterGatewayWire.job(MatterGatewayWire.decode("""{"job":1,"state":"running"}"""), 1))
        val result = MatterGatewayWire.job(MatterGatewayWire.decode("""{"job":1,"state":"complete","response":{"schema":1,"ok":true,"result":{"session":"1234567890abcdef","state":"waiting_ble","deadline_remaining_seconds":30}}}"""), 1)!!
        val session = MatterGatewayWire.session(result)
        assertFalse(session.terminal)
        assertEquals("waiting_ble", session.state)
        malformed { MatterGatewayWire.job(MatterGatewayWire.decode("""{"job":2,"state":"running"}"""), 1) }
        malformed { MatterGatewayWire.accepted(MatterGatewayWire.decode("""{"schema":1,"ok":true,"job":0,"state":"queued"}""")) }
    }
    @Test fun errorsAreFixedAndDoNotEchoGatewayOrSetupData() {
        assertEquals(MatterGatewayWire.Error.BUSY, MatterGatewayWire.httpError(
            MatterGatewayWire.decode("""{"ok":false,"error":"busy"}""")))
        malformed { MatterGatewayWire.httpError(MatterGatewayWire.decode("""{"ok":false,"error":"20202021"}""")) }
        try {
            MatterGatewayWire.success(MatterGatewayWire.decode("""{"schema":1,"ok":false,"error":"transport"}"""))
            fail("Expected gateway failure")
        } catch (e: MatterGatewayWire.Failure) { assertEquals("transport", e.message) }
        malformed { MatterGatewayWire.success(MatterGatewayWire.decode("""{"schema":1,"ok":true,"result":{},"error":"busy"}""")) }
    }
    @Test fun sessionAndBleEventsValidateIdsBoundsAndFields() {
        val event = MatterGatewayWire.message("""{"schema":1,"event":"ble_write","data":{"session":"1234567890abcdef","generation":1,"write":2,"data":"00ff"}}""") as MatterGatewayWire.Write
        assertArrayEquals(byteArrayOf(0, -1), event.data)
        for (input in listOf(
            """{"schema":1,"event":"ble_write","data":{"session":"0000000000000000","generation":1,"write":2,"data":"00"}}""",
            """{"schema":1,"event":"ble_write","data":{"session":"1234567890abcdef","generation":0,"write":2,"data":"00"}}""",
            """{"schema":1,"event":"ble_write","data":{"session":"1234567890abcdef","generation":1,"write":2,"data":"AA"}}""",
            """{"schema":1,"id":1,"response":{},"event":"commissioning"}""",
            """{"schema":1,"event":"thread","data":{}}""")) malformed { MatterGatewayWire.message(input) }
        malformed { MatterGatewayWire.session(MatterGatewayWire.decode("""{"session":"1234567890abcdef","state":"waiting_ble","deadline_remaining_seconds":901}""")) }
    }
    @Test fun requestEnvelopesUseCanonicalIdsAndOpaqueData() {
        val encoded = MatterGatewayWire.envelope(5, 7, MatterGatewayWire.body(
            "session" to MatterGatewayWire.text("1234567890abcdef"), "generation" to MatterGatewayWire.number(1),
            "sequence" to MatterGatewayWire.number(1), "data" to MatterGatewayWire.text(MatterGatewayWire.hex(byteArrayOf(0, -1)))))
        val decoded = MatterGatewayWire.decode(encoded)
        assertEquals(5L, decoded.number("id"))
        assertEquals("00ff", decoded.obj("body").text("data"))
        malformed { MatterGatewayWire.hex(ByteArray(245)) }
        malformed { MatterGatewayWire.id("1234567890ABCDEF") }
    }
    @Test fun disabledStatusAndFreshReadinessAreDistinct() {
        val disabled = MatterGatewayWire.decode("""{"enabled":false,"available":false}""")
        assertEquals(MatterGatewayWire.Availability(false, false, false), MatterGatewayWire.availability(disabled, disabled))
        val matter = MatterGatewayWire.decode("""{"enabled":true,"available":true,"controller_initialized":true,"fabric_ready":true,"state":"running"}""")
        val thread = MatterGatewayWire.decode("""{"enabled":true,"available":true,"configured":true,"attached":false,"state":"attaching","linux_ipv6_or_tun":false,"zigbee_channel":15,"dataset_channel":15}""")
        assertTrue(MatterGatewayWire.availability(matter, thread).ready)
        val mismatch = MatterGatewayWire.decode(MatterGatewayWire.encode(thread).replace("attaching", "channel_mismatch"))
        assertFalse(MatterGatewayWire.availability(matter, mismatch).ready)
        malformed { MatterGatewayWire.thread(MatterGatewayWire.decode(MatterGatewayWire.encode(thread).replace("linux_ipv6_or_tun\":false", "linux_ipv6_or_tun\":true"))) }
    }
}
