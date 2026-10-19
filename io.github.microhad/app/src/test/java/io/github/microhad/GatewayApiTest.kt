package io.github.microhad

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.tls.HandshakeCertificates
import okhttp3.tls.HeldCertificate
import org.junit.Assert.*
import org.junit.Test
import javax.net.ssl.SSLException

class GatewayApiTest {
    @Test fun configAndRegistrationUseSelectedOrigin() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            server.enqueue(MockResponse().setBody("""{"schema":1,"name":"Test","matter":false,"firebase":null}"""))
            server.enqueue(MockResponse().setBody("""{"ok":true}"""))
            val api = GatewayApi(server.url("/").toString().removeSuffix("/"))
            assertNull(GatewayConfig.parse(api.config()).firebase)
            api.register("""{"installation_id":"test","model":"phone","token":"synthetic-token"}""")
            assertEquals("/api/companion/config", server.takeRequest().path)
            val post = server.takeRequest()
            assertEquals("POST", post.method)
            assertEquals("/api/push/devices/register", post.path)
            assertTrue(post.body.readUtf8().contains("synthetic-token"))
        }
    }
    @Test fun responseSizeAndHttpErrorsAreBounded() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            val api = GatewayApi(server.url("/").toString().removeSuffix("/"))
            server.enqueue(MockResponse().setBody("x".repeat(8193)))
            assertTrue(runCatching { api.config() }.exceptionOrNull() is IllegalArgumentException)
            server.enqueue(MockResponse().setResponseCode(401))
            assertEquals(401, (runCatching { api.config() }.exceptionOrNull() as GatewayHttpException).code)
            server.enqueue(MockResponse().setResponseCode(302).addHeader("Location", "http://127.0.0.1:1/"))
            assertEquals(302, (runCatching { api.config() }.exceptionOrNull() as GatewayHttpException).code)
        }
    }
    @Test fun selfSignedHttpsNeedsExactAcceptedPin() = runBlocking {
        val certificate = HeldCertificate.Builder().commonName("different-host.example").build()
        val tls = HandshakeCertificates.Builder().heldCertificate(certificate).build()
        MockWebServer().use { server ->
            server.useHttps(tls.sslSocketFactory(), false); server.start()
            val base = server.url("/").toString().removeSuffix("/")
            assertTrue(runCatching { GatewayApi(base).config() }.exceptionOrNull() is SSLException)
            val pin = GatewayTransport.fingerprint(certificate.certificate)
            assertEquals(pin, GatewayTransport.inspectCertificate(base))
            server.enqueue(MockResponse().setBody("accepted"))
            assertEquals("accepted", GatewayApi(base, pin).config())
            val other = HeldCertificate.Builder().commonName("other.example").build()
            val wrong = GatewayTransport.fingerprint(other.certificate)
            assertTrue(runCatching { GatewayApi(base, wrong).config() }.exceptionOrNull() is SSLException)
        }
    }
}
