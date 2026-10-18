package org.microhad.companion

import org.junit.Assert.*
import org.junit.Test
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate

class CompanionTest {
    private val firebase = """{"application_id":"1:123456:android:abcdef","api_key":"example-key","project_id":"test-project","sender_id":"123456"}"""
    private fun document(value: String) = """{"schema":1,"name":"Test gateway","matter":true,"firebase":$value}"""
    @Test fun optionalPushConfiguration() {
        val result = GatewayConfig.parse(document("null"))
        assertTrue(result.matter); assertNull(result.firebase)
        assertEquals("Test gateway", result.name)
    }
    @Test fun runtimeFirebaseFields() {
        val config = GatewayConfig.parse(document(firebase)).firebase!!
        assertEquals("test-project", config.projectId)
        assertEquals("123456", config.senderId)
    }
    @Test fun invalidConfigurationRejected() {
        for (value in listOf(document(firebase).replace("\"schema\":1", "\"schema\":2"),
            document(firebase.replace("1:123456:", "1:999:")), document("{}"), document(firebase.replace("example-key", "")), "x".repeat(8193))) {
            assertTrue(runCatching { GatewayConfig.parse(value) }.isFailure)
        }
    }
    @Test fun addressValidation() {
        assertEquals("http://gateway.local", GatewayTransport.normalize("gateway.local"))
        assertEquals("https://gateway.local:443", GatewayTransport.normalize(" https://gateway.local:443/ "))
        assertEquals("http://[::1]:8080", GatewayTransport.normalize("http://[::1]:8080"))
        for (address in listOf("ftp://gateway", "http://u:p@gateway", "http://gateway/path", "http://gateway?x=1", "http://gateway/#x", "http://gateway:0"))
            assertTrue(address, runCatching { GatewayTransport.normalize(address) }.isFailure)
    }
    @Test fun explicitCertificatePinMatchesOnlyThatCertificate() {
        val cert = javaClass.getResourceAsStream("/gateway.crt")!!.use { CertificateFactory.getInstance("X.509").generateCertificate(it) as X509Certificate }
        val pin = GatewayTransport.fingerprint(cert)
        assertEquals(64, pin.length)
        assertTrue(GatewayTransport.accepts(pin, cert))
        assertFalse(GatewayTransport.accepts("0".repeat(64), cert))
        assertFalse(GatewayTransport.accepts("", cert))
        GatewayTransport.client(pin)
        assertTrue(runCatching { GatewayTransport.client("invalid") }.isFailure)
    }
    @Test fun registrationRetriesOnlyTransientHttpFailures() {
        assertTrue(PushRegistration.retryable(503)); assertTrue(PushRegistration.retryable(429)); assertTrue(PushRegistration.retryable(408))
        assertFalse(PushRegistration.retryable(400)); assertFalse(PushRegistration.retryable(401)); assertFalse(PushRegistration.retryable(404))
    }
}
