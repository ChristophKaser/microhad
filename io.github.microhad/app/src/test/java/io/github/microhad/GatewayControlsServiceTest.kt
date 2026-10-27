package io.github.microhad

import android.app.Application
import android.service.controls.Control
import android.service.controls.actions.*
import android.service.controls.templates.*
import okhttp3.mockwebserver.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.util.concurrent.Flow
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.function.Consumer

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class)
class GatewayControlsServiceTest {
    private class Probe : Flow.Subscriber<Control> {
        lateinit var subscription: Flow.Subscription
        val controls = LinkedBlockingQueue<Control>()
        val errors = LinkedBlockingQueue<Throwable>()
        override fun onSubscribe(s: Flow.Subscription) { subscription = s; s.request(Long.MAX_VALUE) }
        override fun onNext(c: Control) { controls.add(c) }
        override fun onError(t: Throwable) { errors.add(t) }
        override fun onComplete() {}
        fun next(): Control = requireNotNull(controls.poll(5, TimeUnit.SECONDS)) { "No control; error=${errors.peek()}" }
    }
    private val inventory = """{"devices":[{"id":"lamp","name":"Lamp","has_light":true,"has_brightness":true,"light_on":true,"brightness_percent":42},{"id":"sensor","has_temperature":true}],"scene_groups":[{"id":2,"scenes":[{"id":3,"name":"Evening"}]}]}"""
    @Test fun discoveryTemplatesActionsAndGatewayIsolation() {
        val server = MockWebServer()
        val writes = LinkedBlockingQueue<RecordedRequest>()
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse {
                if (request.method == "POST") writes.add(request)
                return MockResponse().setBody(when(request.path) {
                    "/api/status" -> inventory
                    "/api/automations" -> """{"scripts":[{"id":"evening.lua","triggers":[{"id":"run","label":"Run"}]}]}"""
                    else -> "{}"
                })
            }
        }
        server.start()
        val context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("gateway", 0).edit().clear().commit()
        val store = GatewayStore(context)
        store.save(server.url("/").toString().removeSuffix("/"), "", """{"schema":1,"name":"Test","matter":false,"firebase":null}""")
        val controller = Robolectric.buildService(GatewayControlsProviderService::class.java).create()
        val service = controller.get()
        try {
            val discovery = Probe()
            service.createPublisherForAllAvailable().subscribe(discovery)
            val available = List(5) { discovery.next() }
            val lamp = available.single { it.title == "Lamp" }
            val sensor = available.single { it.zone == "Sensors" }
            val scene = available.single { it.controlId.contains(":scene:") }
            val trigger = available.single { it.zone == "Automation triggers" }
            assertTrue(available.all { it.structure == "Test" })
            val selected = Probe()
            service.createPublisherFor(mutableListOf(lamp.controlId, sensor.controlId, scene.controlId)).subscribe(selected)
            val states = List(3) { selected.next() }
            assertTrue(states.single { it.controlId == lamp.controlId }.controlTemplate is ToggleRangeTemplate)
            assertEquals(ControlTemplate.TYPE_NO_TEMPLATE, states.single { it.controlId == sensor.controlId }.controlTemplate.templateType)
            assertTrue(states.single { it.controlId == scene.controlId }.controlTemplate is StatelessTemplate)
            assertTrue(states.all { it.isAuthRequired })
            fun action(id: String, action: ControlAction): Int {
                val result = LinkedBlockingQueue<Int>()
                service.performControlAction(id, action, Consumer { result.add(it) })
                return requireNotNull(result.poll(5, TimeUnit.SECONDS))
            }
            assertEquals(ControlAction.RESPONSE_OK, action(lamp.controlId, BooleanAction("toggle", false)))
            assertEquals("state=off", writes.poll(2, TimeUnit.SECONDS)!!.body.readUtf8())
            assertEquals(ControlAction.RESPONSE_OK, action(lamp.controlId, FloatAction("brightness", 25f)))
            assertEquals("brightness=25", writes.poll(2, TimeUnit.SECONDS)!!.body.readUtf8())
            assertEquals(ControlAction.RESPONSE_OK, action(scene.controlId, CommandAction("run")))
            assertEquals("/api/scenes/2/3/activate", writes.poll(2, TimeUnit.SECONDS)!!.path)
            assertEquals(ControlAction.RESPONSE_OK, action(trigger.controlId, CommandAction("run")))
            assertEquals("script_id=evening.lua&trigger_id=run", writes.poll(2, TimeUnit.SECONDS)!!.body.readUtf8())
            assertEquals(ControlAction.RESPONSE_FAIL, action(sensor.controlId, CommandAction("run")))
            assertEquals(ControlAction.RESPONSE_FAIL, action(lamp.controlId, FloatAction("brightness", Float.NaN)))
            assertEquals(ControlAction.RESPONSE_FAIL, action(lamp.controlId, BooleanAction("wrong", true)))
            assertTrue(writes.isEmpty())
            selected.subscription.cancel()
            store.save("http://127.0.0.1:1", "", "{}")
            assertEquals(ControlAction.RESPONSE_FAIL, action(lamp.controlId, BooleanAction("toggle", true)))
            val old = Probe()
            service.createPublisherFor(mutableListOf(lamp.controlId)).subscribe(old)
            assertEquals(Control.STATUS_NOT_FOUND, old.next().status)
            old.subscription.cancel()
        } finally { controller.destroy(); server.shutdown() }
    }
    @Test fun subscriptionsReportRemovedOfflineAndStopPolling() {
        val server = MockWebServer()
        val mode = java.util.concurrent.atomic.AtomicInteger(0)
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse = when {
                request.method == "POST" -> MockResponse().setResponseCode(500)
                request.path == "/api/automations" -> MockResponse().setBody("{\"scripts\":[]}")
                mode.get() == 1 -> MockResponse().setBody("{\"devices\":[]}")
                mode.get() == 2 -> MockResponse().setResponseCode(503)
                else -> MockResponse().setBody(inventory)
            }
        }
        server.start()
        val context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("gateway", 0).edit().clear().commit()
        GatewayStore(context).save(server.url("/").toString().removeSuffix("/"), "", "{}")
        val controller = Robolectric.buildService(GatewayControlsProviderService::class.java).create()
        try {
            val service = controller.get()
            val key = controlsGatewayKey(server.url("/").toString().removeSuffix("/"), "")
            val id = "$key:device:lamp:light"
            val probe = Probe()
            service.createPublisherFor(mutableListOf(id)).subscribe(probe)
            assertEquals(Control.STATUS_OK, probe.next().status)
            val response = LinkedBlockingQueue<Int>()
            service.performControlAction(id, BooleanAction("toggle", false), Consumer { response.add(it) })
            assertEquals(ControlAction.RESPONSE_FAIL, response.poll(5, TimeUnit.SECONDS))
            // Action completion requests an immediate refresh; drain that state first.
            assertEquals(Control.STATUS_OK, probe.next().status)
            mode.set(1)
            assertEquals(Control.STATUS_NOT_FOUND, probe.next().status)
            mode.set(2)
            assertEquals(Control.STATUS_ERROR, probe.next().status)
            mode.set(0)
            assertEquals(Control.STATUS_OK, probe.next().status)
            probe.subscription.cancel()
            // Give cancellation time to finish, then exceed one polling interval.
            Thread.sleep(100)
            val requests = server.requestCount
            Thread.sleep(3200)
            assertEquals(requests, server.requestCount)
        } finally { controller.destroy(); server.shutdown() }
    }
    @Test @Config(sdk = [28]) fun serviceDisabledOnOlderAndroid() {
        assertFalse(RuntimeEnvironment.getApplication().resources.getBoolean(R.bool.device_controls_enabled))
    }
}
