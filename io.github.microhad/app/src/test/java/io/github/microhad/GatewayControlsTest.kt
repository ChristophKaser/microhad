package io.github.microhad

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class GatewayControlsTest {
    private val snapshot = HomeSnapshot(JSONObject("""{"devices":[
        {"id":"a:b/c","name":"Lamp","has_light":true,"has_brightness":true,"light_on":true,"brightness_percent":42},
        {"id":"plug","has_actor":true,"actor_known":true,"online":false},
        {"id":"sensor","has_temperature":true,"temperature_celsius":0},
        {"id":"contact","has_contact":true,"contact_known":true,"contact_open":true}
    ],"scene_groups":[{"id":2,"name":"Room","scenes":[{"id":3,"name":"Evening"}]}]}"""))
    @Test fun publishesDevicesReadingsScenesAndTriggers() {
        val script = JSONObject("""{"id":"evening.lua","triggers":[{"id":"run/1","label":"Run evening"}]}""")
        val controls = gatewayControls("key", snapshot, listOf(script))
        val lamp = controls.single { it.capability == "light" }
        assertTrue(lamp.dim); assertTrue(lamp.checked); assertEquals(42f, lamp.brightness)
        assertTrue(lamp.id.contains("a%3Ab%2Fc"))
        assertFalse(controls.single { it.capability == "actor" }.available)
        assertEquals("Unknown", controls.single { it.title.contains("Temperature") }.text)
        assertEquals("Open", controls.single { it.title.contains("Contact") }.text)
        assertEquals("/api/scenes/2/3/activate", controls.single { it.title.contains("Evening") }.route)
        assertEquals("/api/scenes/2/off", controls.single { it.title.endsWith("Off") }.route)
        val trigger = controls.single { it.section == "Automation triggers" }
        assertEquals("/api/automations/trigger", trigger.route)
        assertEquals(mapOf("script_id" to "evening.lua", "trigger_id" to "run/1"), trigger.fields)
        assertEquals(controls.size, controls.map { it.id }.distinct().size)
        assertTrue(controls.filter { it.section == "Sensors" }.all { it.capability.isEmpty() && it.route.isEmpty() })
    }
    @Test fun identitiesAreStableAndIsolatedByGatewayAndCertificate() {
        val key = controlsGatewayKey("https://gateway", "pin")
        assertEquals(key, controlsGatewayKey("https://gateway", "pin"))
        assertNotEquals(key, controlsGatewayKey("https://other", "pin"))
        assertNotEquals(key, controlsGatewayKey("https://gateway", "new-pin"))
        assertEquals(gatewayControls(key, snapshot, emptyList()).map { it.id }, gatewayControls(key, snapshot, emptyList()).map { it.id })
        assertTrue(gatewayControls("other", snapshot, emptyList()).none { it.id.startsWith("$key:") })
    }
}
