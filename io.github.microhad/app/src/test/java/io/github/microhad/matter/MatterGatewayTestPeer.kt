package io.github.microhad.matter

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.junit.Assert.*
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/** Small real TCP peer, independent of Ktor; all values are synthetic test data. */
internal class MatterGatewayTestPeer(script: MatterGatewayTestPeer.() -> Unit) : AutoCloseable {
    private val server = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1")).also { it.soTimeout = 5000 }
    val base = "http://127.0.0.1:${server.localPort}"
    private val error = AtomicReference<Throwable>()
    private val done = CountDownLatch(1)
    private val sockets = java.util.concurrent.CopyOnWriteArrayList<Socket>()
    private val thread = Thread {
        try { script(this@MatterGatewayTestPeer) } catch (e: Throwable) { error.set(e) } finally { done.countDown() }
    }.also { it.isDaemon = true; it.start() }
    class Request(val path: String, val body: String, val headers: Map<String, String>)
    class WebSocket(val socket: Socket) {
        var pongs = 0
        fun text(): String {
            while (true) {
                val input = socket.getInputStream()
                val first = input.read(); assertTrue(first >= 0)
                val second = input.read(); assertTrue(second >= 0 && second and 128 != 0)
                var length = second and 127
                if (length == 126) length = (input.read() shl 8) or input.read()
                assertTrue(length in 0..2048)
                val mask = exact(input, 4)
                val bytes = exact(input, length)
                for (i in bytes.indices) bytes[i] = (bytes[i].toInt() xor mask[i % 4].toInt()).toByte()
                if (first and 15 == 10) { pongs++; continue }
                assertEquals(1, first and 15)
                assertTrue(first and 128 != 0)
                return String(bytes, Charsets.UTF_8)
            }
        }
        fun send(text: String, opcode: Int = 1, final: Boolean = true) = send(text.toByteArray(Charsets.UTF_8), opcode, final)
        fun send(bytes: ByteArray, opcode: Int = 1, final: Boolean = true) {
            val output = socket.getOutputStream()
            output.write(opcode or if (final) 128 else 0)
            if (bytes.size < 126) output.write(bytes.size) else {
                output.write(126); output.write(bytes.size ushr 8); output.write(bytes.size and 255)
            }
            output.write(bytes); output.flush()
        }
    }
    fun http(status: Int = 200, response: String, inspect: (Request) -> Unit = {}) {
        accept().use { socket ->
            inspect(request(socket))
            val body = response.toByteArray(Charsets.US_ASCII)
            socket.getOutputStream().apply {
                write("HTTP/1.1 $status Test\r\nContent-Type: application/json\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n".toByteArray())
                write(body); flush()
            }
        }
    }
    fun websocket(): WebSocket {
        val socket = accept()
        val request = request(socket)
        assertEquals("GET /api/matter/relay", request.path)
        val key = request.headers["sec-websocket-key"] ?: error("Missing test handshake key")
        val accept = Base64.getEncoder().encodeToString(MessageDigest.getInstance("SHA-1")
            .digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toByteArray(Charsets.US_ASCII)))
        socket.getOutputStream().apply {
            write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: $accept\r\n\r\n".toByteArray())
            flush()
        }
        return WebSocket(socket)
    }
    private fun accept(): Socket = server.accept().also { it.soTimeout = 5000; sockets.add(it) }
    private fun request(socket: Socket): Request {
        val input = socket.getInputStream()
        val header = StringBuilder()
        while (!header.endsWith("\r\n\r\n")) {
            val byte = input.read(); assertTrue(byte >= 0); header.append(byte.toChar()); assertTrue(header.length <= 8192)
        }
        val lines = header.toString().split("\r\n")
        val headers = lines.drop(1).filter { ':' in it }.associate {
            it.substringBefore(':').lowercase() to it.substringAfter(':').trim()
        }
        val length = headers["content-length"]?.toInt() ?: 0
        assertTrue(length in 0..2048)
        return Request(lines[0].substringBeforeLast(' '), String(exact(input, length), Charsets.UTF_8), headers)
    }
    suspend fun verify(): Unit = withContext(Dispatchers.IO) {
        assertTrue("Test peer did not finish", done.await(5, TimeUnit.SECONDS))
        error.get()?.let { throw AssertionError("Synthetic test peer failed", it) }
        Unit
    }
    override fun close() {
        sockets.forEach { it.close() }; server.close(); thread.join(100)
    }
    companion object {
        private fun exact(input: InputStream, length: Int): ByteArray {
            val data = ByteArray(length)
            var at = 0
            while (at < length) {
                val count = input.read(data, at, length - at); assertTrue(count > 0); at += count
            }
            return data
        }
    }
}
