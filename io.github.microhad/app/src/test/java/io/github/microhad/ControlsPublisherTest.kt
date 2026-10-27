package io.github.microhad

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.flow
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.Flow
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class ControlsPublisherTest {
    private class Probe : Flow.Subscriber<Int> {
        lateinit var subscription: Flow.Subscription
        val values = LinkedBlockingQueue<Int>()
        val errors = LinkedBlockingQueue<Throwable>()
        override fun onSubscribe(s: Flow.Subscription) { subscription = s }
        override fun onNext(item: Int) { values.add(item) }
        override fun onError(t: Throwable) { errors.add(t) }
        override fun onComplete() {}
    }
    @Test fun honorsDemandAndCancellation() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val cancelled = CompletableDeferred<Unit>()
        try {
            val probe = Probe()
            flow { try { emit(1); emit(2); awaitCancellation() } finally { cancelled.complete(Unit) } }
                .controlsPublisher(scope).subscribe(probe)
            assertNull(probe.values.poll(100, TimeUnit.MILLISECONDS))
            probe.subscription.request(1)
            assertEquals(1, probe.values.poll(2, TimeUnit.SECONDS))
            assertNull(probe.values.poll(100, TimeUnit.MILLISECONDS))
            probe.subscription.request(Long.MAX_VALUE)
            assertEquals(2, probe.values.poll(2, TimeUnit.SECONDS))
            probe.subscription.cancel()
            withTimeout(2000) { cancelled.await() }
            assertTrue(probe.errors.isEmpty())
        } finally { scope.cancel() }
    }
    @Test fun rejectsNonPositiveDemand() {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val probe = Probe()
            flow { emit(1) }.controlsPublisher(scope).subscribe(probe)
            probe.subscription.request(0)
            assertTrue(probe.errors.poll(2, TimeUnit.SECONDS) is IllegalArgumentException)
            assertTrue(probe.values.isEmpty())
        } finally { scope.cancel() }
    }
}
