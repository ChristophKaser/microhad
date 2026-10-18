package org.microhad.companion.matter

import io.ktor.client.HttpClient
import io.ktor.client.engine.okhttp.OkHttp
import org.microhad.companion.GatewayTransport
import io.ktor.client.plugins.HttpTimeout
import io.ktor.client.plugins.websocket.WebSockets
import io.ktor.client.request.*
import io.ktor.client.statement.*
import io.ktor.http.ContentType
import io.ktor.http.HttpMethod
import io.ktor.http.contentType
import kotlinx.coroutines.*
import java.net.URI

/** Dedicated client: no request logging, redirects, retry plugin or automatic request retries. */
internal class MatterGatewayClient(baseUrl: String, fingerprint: String = "") : AutoCloseable {
    val base: String = validateBase(baseUrl)
    val client = HttpClient(OkHttp) {
        expectSuccess = false
        followRedirects = false
        engine { preconfigured = GatewayTransport.client(fingerprint) }
        install(HttpTimeout) { requestTimeoutMillis = 3000; connectTimeoutMillis = 3000; socketTimeoutMillis = 3000 }
        install(WebSockets) { maxFrameSize = 2048; pingInterval = 15000 }
    }
    suspend fun availability(): MatterGatewayWire.Availability = protected {
        val matter = get("/api/matter/status", true)
        if (!matter.flag("enabled")) MatterGatewayWire.availability(matter, matter)
        else MatterGatewayWire.availability(matter, get("/api/thread/status", true))
    }
    suspend fun start(discriminator: Int, pin: Int, timeoutSeconds: Int): MatterGatewayWire.Session {
        require(discriminator in 0..4095 && timeoutSeconds in 30..900 && MatterSetupCode.pinValid(pin))
        return MatterGatewayWire.session(call("/api/matter/commissioning/start", MatterGatewayWire.body(
            "discriminator" to MatterGatewayWire.number(discriminator.toLong()), "pin" to MatterGatewayWire.number(pin.toLong()),
            "timeout_seconds" to MatterGatewayWire.number(timeoutSeconds.toLong()))))
    }
    suspend fun status(session: String): MatterGatewayWire.Session = sessionCall("status", session)
    suspend fun cancel(session: String): MatterGatewayWire.Session = sessionCall("cancel", session)
    private suspend fun sessionCall(action: String, session: String): MatterGatewayWire.Session =
        MatterGatewayWire.session(call("/api/matter/commissioning/$action",
            MatterGatewayWire.body("session" to MatterGatewayWire.text(MatterGatewayWire.id(session)))))
    suspend fun createThread() {
        val result = call("/api/thread/create", MatterGatewayWire.body())
        MatterGatewayWire.thread(result)
    }
    private suspend fun call(path: String, body: MatterGatewayWire.Object): MatterGatewayWire.Object = protected {
        withTimeout(10000) {
            val accepted = request(path, HttpMethod.Post, MatterGatewayWire.encode(body))
            val job = MatterGatewayWire.accepted(accepted)
            while (true) {
                val result = MatterGatewayWire.job(get("/api/matter/jobs/$job"), job)
                if (result != null) return@withTimeout result
                delay(100)
            }
            @Suppress("UNREACHABLE_CODE")
            throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TIMEOUT)
        }
    }
    private suspend fun get(path: String, status: Boolean = false) = request(path, HttpMethod.Get, null, status)
    private suspend fun request(path: String, method: HttpMethod, body: String? = null,
                                status: Boolean = false): MatterGatewayWire.Object {
        val response = client.request(base + path) {
            this.method = method
            if (body != null) { contentType(ContentType.Application.Json); setBody(body) }
        }
        val buffer = ByteArray(2049)
        val channel = response.bodyAsChannel()
        var size = 0
        try {
            while (size < buffer.size) {
                val count = channel.readAvailable(buffer, size, buffer.size - size)
                if (count < 0) break
                if (count == 0) { yield(); continue }
                size += count
            }
            if (size > 2048 || buffer.take(size).any { it.toInt() !in 0..127 }) {
                throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
            }
            val value = MatterGatewayWire.decode(String(buffer, 0, size, Charsets.US_ASCII))
            val expected = if (method == HttpMethod.Post) 202 else 200
            if (response.status.value != expected) {
                if (status && response.status.value == 503) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.NOT_READY)
                throw MatterGatewayWire.Failure(MatterGatewayWire.httpError(value))
            }
            return value
        } finally { buffer.fill(0); channel.cancel(null) }
    }
    override fun close() { client.close() }
    private suspend fun <T> protected(action: suspend () -> T): T = try { action() }
    catch (_: TimeoutCancellationException) { throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TIMEOUT) }
    catch (e: CancellationException) { throw e }
    catch (e: MatterGatewayWire.Failure) { throw e }
    catch (_: Exception) { throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TRANSPORT) }
    companion object {
        private fun validateBase(baseUrl: String): String {
            val uri = try { URI(baseUrl) } catch (_: Exception) {
                throw MatterGatewayWire.Failure(MatterGatewayWire.Error.MALFORMED)
            }
            if (uri.scheme !in listOf("http", "https") || uri.host.isNullOrEmpty() || uri.rawUserInfo != null || uri.rawQuery != null ||
                uri.rawFragment != null || uri.rawPath !in listOf("", "/") || uri.port !in -1..65535 || uri.port == 0) {
                throw MatterGatewayWire.Failure(MatterGatewayWire.Error.MALFORMED)
            }
            return baseUrl.removeSuffix("/")
        }
    }
}
