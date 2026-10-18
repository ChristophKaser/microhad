package org.microhad.companion.matter

import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test

class MatterGatewayClientTest {
    @Test fun startIsPostedOnceAndPollsDeliveryWithoutClaimingCommissioningSuccess() = runBlocking {
        MatterGatewayTestPeer {
            http(202, """{"schema":1,"ok":true,"job":1,"state":"queued"}""") {
                assertEquals("POST /api/matter/commissioning/start", it.path)
                val body = MatterGatewayWire.decode(it.body)
                assertEquals(3858L, body.number("discriminator"))
                assertEquals(20202021L, body.number("pin"))
                assertEquals(30L, body.number("timeout_seconds"))
            }
            http(response = """{"job":1,"state":"running"}""") { assertEquals("GET /api/matter/jobs/1", it.path) }
            http(response = """{"job":1,"state":"complete","response":{"schema":1,"ok":true,"result":{"session":"1234567890abcdef","state":"waiting_ble","deadline_remaining_seconds":30}}}""")
        }.use { peer ->
            MatterGatewayClient(peer.base).use { client ->
                val result = client.start(3858, MatterSetupCode.parse("34970112332").consumePin(), 30)
                assertEquals("waiting_ble", result.state)
                assertFalse(result.terminal)
            }
            peer.verify()
        }
    }
    @Test fun nativeHttpErrorsUseTheActualSchemaLessEnvelope() = runBlocking {
        MatterGatewayTestPeer { http(503, """{"ok":false,"error":"busy"}""") }.use { peer ->
            MatterGatewayClient(peer.base).use { client ->
                failure(MatterGatewayWire.Error.BUSY) { client.start(3840, 20202021, 30) }
            }
            peer.verify()
        }
    }
    @Test fun transportFailureInAJobIsNotReplayed() = runBlocking {
        MatterGatewayTestPeer {
            http(202, """{"schema":1,"ok":true,"job":1,"state":"queued"}""")
            http(response = """{"job":1,"state":"failed","error":"transport"}""")
        }.use { peer ->
            MatterGatewayClient(peer.base).use { client ->
                failure(MatterGatewayWire.Error.TRANSPORT) { client.start(3840, 20202021, 30) }
            }
            peer.verify()
        }
    }
    @Test fun disabledBuildDoesNotRequestThreadOrStartAJob() = runBlocking {
        MatterGatewayTestPeer { http(response = """{"enabled":false,"available":false}""") }.use { peer ->
            MatterGatewayClient(peer.base).use { client -> assertFalse(client.availability().enabled) }
            peer.verify()
        }
    }
    @Test fun oversizedHttpResponseAndWrongJobIdFailClosed() = runBlocking {
        MatterGatewayTestPeer { http(202, " ".repeat(2049)) }.use { peer ->
            MatterGatewayClient(peer.base).use { client ->
                failure(MatterGatewayWire.Error.PROTOCOL) { client.start(3840, 20202021, 30) }
            }
            peer.verify()
        }
        MatterGatewayTestPeer {
            http(202, """{"schema":1,"ok":true,"job":1,"state":"queued"}""")
            http(response = """{"job":2,"state":"running"}""")
        }.use { peer ->
            MatterGatewayClient(peer.base).use { client ->
                failure(MatterGatewayWire.Error.PROTOCOL) { client.start(3840, 20202021, 30) }
            }
            peer.verify()
        }
    }
    @Test fun cancelReturnsTheRetainedSessionAndCannotEchoExtraSecrets() = runBlocking {
        MatterGatewayTestPeer {
            http(202, """{"schema":1,"ok":true,"job":1,"state":"queued"}""") {
                assertEquals("POST /api/matter/commissioning/cancel", it.path)
                assertEquals(setOf("schema", "session"), MatterGatewayWire.decode(it.body).fields.keys)
            }
            http(response = """{"job":1,"state":"complete","response":{"schema":1,"ok":true,"result":{"session":"1234567890abcdef","state":"cancelled","deadline_remaining_seconds":0,"error":"cancelled"}}}""")
        }.use { peer ->
            MatterGatewayClient(peer.base).use { client -> assertTrue(client.cancel("1234567890abcdef").terminal) }
            peer.verify()
        }
    }
    @Test fun rejectsUnsafeOrUnsupportedGatewayUrlBeforeCreatingAClient() {
        for (url in listOf("ftp://example.com", "http://user:secret@example.com", "http://example.com/path",
            "http://example.com?pin=20202021", "http://example.com/#x", "http://example.com:0", "%%%")) {
            try { MatterGatewayClient(url).close(); fail("Expected fixed URL error") }
            catch (e: MatterGatewayWire.Failure) { assertEquals("malformed", e.message) }
        }
    }
    private suspend fun failure(error: MatterGatewayWire.Error, action: suspend () -> Unit) {
        try { action(); fail("Expected fixed gateway error") } catch (e: MatterGatewayWire.Failure) {
            assertEquals(error, e.error)
            assertEquals(error.wire, e.message)
        }
    }
}
