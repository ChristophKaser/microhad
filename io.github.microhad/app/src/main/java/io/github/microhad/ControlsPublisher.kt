package io.github.microhad

import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/** Demand-aware bridge to the platform's Java Flow API; no reactive runtime dependency. */
@androidx.annotation.RequiresApi(30)
internal fun <T> Flow<T>.controlsPublisher(scope: CoroutineScope): java.util.concurrent.Flow.Publisher<T> =
    java.util.concurrent.Flow.Publisher { subscriber ->
        val demand = AtomicLong()
        val stopped = AtomicBoolean()
        val wake = Channel<Unit>(Channel.CONFLATED)
        val job = scope.launch(start = CoroutineStart.LAZY) {
            try {
                collect { value ->
                    while (demand.get() == 0L) wake.receive()
                    currentCoroutineContext().ensureActive()
                    demand.updateAndGet { if (it == Long.MAX_VALUE) it else it - 1 }
                    synchronized(stopped) { if (!stopped.get()) subscriber.onNext(value) }
                }
                synchronized(stopped) { if (stopped.compareAndSet(false, true)) subscriber.onComplete() }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { synchronized(stopped) { if (stopped.compareAndSet(false, true)) subscriber.onError(e) } }
            finally { wake.close() }
        }
        subscriber.onSubscribe(object : java.util.concurrent.Flow.Subscription {
            override fun request(n: Long) {
                if (stopped.get()) return
                if (n <= 0) {
                    synchronized(stopped) {
                        if (stopped.compareAndSet(false, true)) {
                            job.cancel()
                            subscriber.onError(IllegalArgumentException("Demand must be positive"))
                        }
                    }
                    return
                }
                demand.updateAndGet { if (Long.MAX_VALUE - it < n) Long.MAX_VALUE else it + n }
                wake.trySend(Unit)
            }
            override fun cancel() { stopped.set(true); job.cancel(); wake.close() }
        })
        job.start()
    }
