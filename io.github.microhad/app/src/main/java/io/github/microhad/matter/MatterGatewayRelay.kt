package io.github.microhad.matter

import io.ktor.client.plugins.websocket.*
import io.ktor.websocket.*
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel

/** Caller-owned serialized coroutine context; connection loss never replays a request. */
internal class MatterGatewayRelay(private val gateway: MatterGatewayClient, session: String,
                                  parent: CoroutineScope, private val listener: Listener,
                                  private val now: () -> Long = System::nanoTime) : AutoCloseable {
    interface Listener {
        fun progress(snapshot: MatterGatewayWire.Session)
        fun attached(generation: Long)
        fun write(generation: Long, write: Long, data: ByteArray)
        fun writeAccepted(generation: Long, write: Long)
        fun failed(error: MatterGatewayWire.Error)
    }
    private class Pending(val opcode: Int, val value: Long, val generation: Long,
                          val deadline: Long, val result: CompletableDeferred<MatterGatewayWire.Object>)
    private val session = MatterGatewayWire.id(session)
    private val scope = CoroutineScope(parent.coroutineContext + SupervisorJob(parent.coroutineContext[Job]))
    private val outgoing = Channel<String>(8)
    private val pending = LinkedHashMap<Long, Pending>()
    private var socket: DefaultClientWebSocketSession? = null
    private var nextId = 1L
    private var generation = 0L
    private var bound = false
    private var opening = false
    private var closed = false
    private var lastWrite = 0L

    suspend fun open() {
        check(!opening && !closed)
        opening = true
        MatterDiagnostics.record("relay opening session=$session")
        try {
            socket = withTimeout(3000) { gateway.client.webSocketSession(urlString =
                "ws" + gateway.base.removePrefix("http") + "/api/matter/relay") }
            scope.launch {
                try {
                    for (message in outgoing) withTimeout(2000) {
                        socket!!.send(Frame.Text(message)); socket!!.flush()
                    }
                } catch (e: CancellationException) { if (e is TimeoutCancellationException) fail(MatterGatewayWire.Error.TIMEOUT) }
                catch (_: Exception) { fail(MatterGatewayWire.Error.TRANSPORT) }
            }
            scope.launch {
                try {
                    for (frame in socket!!.incoming) {
                        if (frame !is Frame.Text || frame.data.size > 2048) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                        receive(MatterGatewayWire.message(frame.readText()))
                    }
                    fail(MatterGatewayWire.Error.TRANSPORT)
                } catch (_: CancellationException) { }
                catch (e: MatterGatewayWire.Failure) { fail(e.error) }
                catch (_: Exception) { fail(MatterGatewayWire.Error.TRANSPORT) }
            }
            scope.launch {
                while (isActive) {
                    delay(250)
                    if (pending.values.any { now() >= it.deadline }) fail(MatterGatewayWire.Error.TIMEOUT)
                }
            }
            enqueue(4, fields()).await()
        } catch (e: CancellationException) {
            if (e is TimeoutCancellationException) {
                fail(MatterGatewayWire.Error.TIMEOUT)
                throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TIMEOUT)
            }
            close(); throw e
        } catch (e: MatterGatewayWire.Failure) { fail(e.error); throw e }
        catch (_: Exception) { fail(MatterGatewayWire.Error.TRANSPORT); throw MatterGatewayWire.Failure(MatterGatewayWire.Error.TRANSPORT) }
    }
    suspend fun attach(address: String, mtu: Int): Long {
        check(bound && !closed)
        require(address.matches(Regex("[0-9a-f]{12}")) && mtu in 23..517)
        return enqueue(6, fields("address" to MatterGatewayWire.text(address), "mtu" to MatterGatewayWire.number(mtu.toLong())))
            .await().number("generation", 1)
    }
    fun indicate(connectionGeneration: Long, sequence: Long, data: ByteArray) {
        current(connectionGeneration)
        require(sequence in 1..0xffffffffL)
        enqueue(7, fields("generation" to MatterGatewayWire.number(connectionGeneration),
            "sequence" to MatterGatewayWire.number(sequence), "data" to MatterGatewayWire.text(MatterGatewayWire.hex(data))), sequence)
    }
    fun completeWrite(connectionGeneration: Long, write: Long, success: Boolean) {
        current(connectionGeneration)
        require(write in 1..0xffffffffL)
        enqueue(8, fields("generation" to MatterGatewayWire.number(connectionGeneration),
            "write" to MatterGatewayWire.number(write), "success" to MatterGatewayWire.flag(success)), write)
    }
    private fun fields(vararg pairs: Pair<String, MatterGatewayWire.Value>) =
        MatterGatewayWire.body("session" to MatterGatewayWire.text(session), *pairs)
    private fun current(value: Long) {
        if (!bound || closed || value != generation || value == 0L) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.STALE_GENERATION)
    }
    private fun enqueue(opcode: Int, body: MatterGatewayWire.Object, value: Long = 0): CompletableDeferred<MatterGatewayWire.Object> {
        if (closed || pending.size >= 8 || nextId > 0xffffffffL) {
            fail(MatterGatewayWire.Error.CAPACITY)
            throw MatterGatewayWire.Failure(MatterGatewayWire.Error.CAPACITY)
        }
        val id = nextId++
        MatterDiagnostics.record("relay enqueue session=$session request=$id opcode=$opcode generation=$generation sequence=$value")
        val result = CompletableDeferred<MatterGatewayWire.Object>()
        pending[id] = Pending(opcode, value, generation, now() + 10000000000L, result)
        if (!outgoing.trySend(MatterGatewayWire.envelope(id, opcode, body)).isSuccess) {
            fail(MatterGatewayWire.Error.CAPACITY)
            throw MatterGatewayWire.Failure(MatterGatewayWire.Error.CAPACITY)
        }
        return result
    }
    private fun receive(message: MatterGatewayWire.Message) {
        when (message) {
            is MatterGatewayWire.Reply -> {
                MatterDiagnostics.record("relay reply session=$session request=${message.id}")
                val request = pending.remove(message.id) ?: throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                try {
                    val result = MatterGatewayWire.success(message.response)
                    when (request.opcode) {
                        4 -> {
                            val snapshot = MatterGatewayWire.session(result)
                            if (snapshot.id != session) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                            bound = true
                            listener.progress(snapshot)
                        }
                        6 -> {
                            result.exact(setOf("generation"))
                            val next = result.number("generation", 1)
                            if (next <= generation) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                            generation = next
                            lastWrite = 0
                            // Publish before consuming the next frame, which can already contain a C1 write.
                            MatterDiagnostics.record("relay attached session=$session generation=$next")
                            listener.attached(next)
                        }
                        7, 8 -> {
                            val key = if (request.opcode == 7) "sequence" else "write"
                            result.exact(setOf(key))
                            if (result.number(key, 1) != request.value || request.generation != generation) {
                                throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                            }
                            if (request.opcode == 8) listener.writeAccepted(generation, request.value)
                        }
                        else -> throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                    }
                    request.result.complete(result)
                } catch (e: MatterGatewayWire.Failure) { request.result.completeExceptionally(e); throw e }
                catch (_: Exception) {
                    val error = MatterGatewayWire.Failure(MatterGatewayWire.Error.TRANSPORT)
                    request.result.completeExceptionally(error)
                    throw error
                }
            }
            is MatterGatewayWire.Progress -> {
                if (!bound || message.snapshot.id != session) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                MatterDiagnostics.record("relay progress session=$session phase=${message.snapshot.state} error=${message.snapshot.error?.wire} remaining=${message.snapshot.remainingSeconds}")
                listener.progress(message.snapshot)
            }
            is MatterGatewayWire.Write -> {
                if (!bound || message.session != session || generation == 0L || message.generation != generation ||
                    message.write <= lastWrite) throw MatterGatewayWire.Failure(MatterGatewayWire.Error.PROTOCOL)
                MatterDiagnostics.record("relay C1 received session=$session generation=${message.generation} write=${message.write} bytes=${message.data.size}")
                lastWrite = message.write
                listener.write(generation, message.write, message.data)
            }
        }
    }
    private fun fail(error: MatterGatewayWire.Error) {
        if (closed) return
        MatterDiagnostics.record("relay failed session=$session error=${error.wire}")
        shutdown(error)
        listener.failed(error)
    }
    override fun close() = shutdown(MatterGatewayWire.Error.CANCELLED)
    private fun shutdown(error: MatterGatewayWire.Error) {
        if (closed) return
        closed = true
        outgoing.cancel()
        pending.values.forEach { it.result.completeExceptionally(MatterGatewayWire.Failure(error)) }
        pending.clear()
        socket?.cancel()
        socket = null
        scope.cancel()
    }
}
