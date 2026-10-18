package org.microhad.companion

import android.app.Application
import androidx.work.ListenableWorker
import androidx.work.testing.TestListenableWorkerBuilder
import androidx.work.workDataOf
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], application = Application::class)
class GatewayStoreTest {
    private val context get() = RuntimeEnvironment.getApplication()
    private val raw = """{"schema":1,"name":"Test","matter":true,"firebase":null}"""
    @Before fun clean() { context.getSharedPreferences("gateway", 0).edit().clear().commit() }
    @Test fun firstLaunchAndProcessRestartPreserveOneInstallation() {
        val first = GatewayStore(context)
        assertEquals("", first.base); assertNull(first.config)
        val id = first.installationId()
        first.save("https://gateway.local", "a".repeat(64), raw)
        val generation = first.generation
        val restarted = GatewayStore(context)
        restarted.processStarted()
        assertEquals(id, restarted.installationId())
        assertEquals("https://gateway.local", restarted.base)
        assertEquals("a".repeat(64), restarted.fingerprint)
        assertEquals(generation, restarted.generation)
        assertTrue(restarted.config!!.matter)
    }
    @Test fun resetCannotBecomeSetupUntilProcessRestart() {
        val store = GatewayStore(context)
        store.save("http://old.local", "", raw)
        val oldId = store.installationId()
        store.clear()
        val recreatedActivityStore = GatewayStore(context)
        assertTrue(recreatedActivityStore.resetPending)
        assertEquals("", recreatedActivityStore.base)
        assertEquals("", recreatedActivityStore.generation)
        assertNull(recreatedActivityStore.config)
        recreatedActivityStore.processStarted()
        assertFalse(recreatedActivityStore.resetPending)
        assertNotEquals(oldId, recreatedActivityStore.installationId())
    }
    @Test fun queuedRegistrationForOldGatewayExitsBeforeFirebaseAccess() = runBlocking {
        val store = GatewayStore(context)
        store.save("http://old.local", "", raw)
        val oldGeneration = store.generation
        store.save("http://new.local", "", raw)
        assertNotEquals(oldGeneration, store.generation)
        val worker = TestListenableWorkerBuilder<PushRegistrationWorker>(context)
            .setInputData(workDataOf("generation" to oldGeneration)).build()
        assertEquals(ListenableWorker.Result.success(), worker.doWork())
        assertEquals("Not registered", store.status)
    }
    @Test fun registrationQueuedBeforeResetExitsWithoutRecreatingIdentity() = runBlocking {
        val store = GatewayStore(context)
        store.save("http://old.local", "", raw)
        val generation = store.generation
        store.clear()
        val worker = TestListenableWorkerBuilder<PushRegistrationWorker>(context)
            .setInputData(workDataOf("generation" to generation)).build()
        assertEquals(ListenableWorker.Result.success(), worker.doWork())
        assertTrue(store.resetPending); assertEquals("", store.base)
    }
}
