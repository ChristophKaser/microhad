package io.github.microhad

import androidx.compose.material3.MaterialTheme
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.activity.ComponentActivity
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class HomeScreenTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @Test fun lightControlsDetailsAndHistoryWithoutSettings() {
        val d = HomeDevice(JSONObject("""{"id":"lamp","name":"Hall lamp","source":"zigbee","has_light":true,"has_brightness":true,"light_on":false,"brightness_percent":50}"""))
        var command = ""
        var details = false
        var history = false
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) { DeviceCard(d,true,{ _,action,fields -> command = "$action:${fields.values.singleOrNull()}" },{ details = true },{ history = true }) } } }
        compose.onNodeWithText("Hall lamp").assertIsDisplayed()
        compose.onNodeWithContentDescription("Turn on").performClick()
        assertEquals("light:on",command)
        compose.onNodeWithContentDescription("More controls").performScrollTo().performClick()
        compose.onNodeWithText("75%").performScrollTo().performClick()
        assertEquals("light:75",command)
        compose.onNodeWithContentDescription("Details").performScrollTo().performClick(); assertTrue(details)
        compose.onNodeWithContentDescription("History").performScrollTo().performClick(); assertTrue(history)
        compose.onNodeWithText("Settings").assertDoesNotExist()
        compose.onNodeWithText("Delete device").assertDoesNotExist()
    }
    @Test fun offlineDevicesDisableCommands() {
        val d = HomeDevice(JSONObject("""{"id":"lamp","name":"Offline lamp","online":false,"has_light":true}"""))
        compose.setContent { MaterialTheme { DeviceCard(d,true,{ _,_,_ -> fail("Offline command") },{},{}) } }
        compose.onNodeWithContentDescription("Turn on").assertIsNotEnabled()
        compose.onNodeWithContentDescription("Refresh").assertIsNotEnabled()
        compose.onNodeWithContentDescription("Details").assertIsEnabled()
    }
    @Test fun groupsExposeSceneAndOffWithoutEditors() {
        val group = JSONObject("""{"id":2,"name":"Living room","members":[],"scenes":[{"id":3,"name":"Evening"}]}""")
        var route = ""
        compose.setContent { MaterialTheme { GroupCard(group, emptyList(),true) { route = it } } }
        compose.onNodeWithText("Evening").performClick()
        assertEquals("/api/scenes/2/3/activate",route)
        compose.onNodeWithText("Turn group off").performClick()
        assertEquals("/api/scenes/2/off",route)
        compose.onNodeWithText("Configure scenes & groups").assertDoesNotExist()
    }
    @Test fun thermostatTargetsRespectReportedLimitsAndPendingState() {
        val d = HomeDevice(JSONObject("""{"id":"valve","has_thermostat":true}"""))
        val t = JSONObject("""{"values":{"occupied_heating_setpoint":20,"system_mode":4},"supported":{"occupied_heating_setpoint":true,"system_mode":true},"fields":{"occupied_heating_setpoint":{"min":5,"max":30,"step":0.5,"writable":true}},"modes":[0,4],"pending":[]}""")
        var sent = emptyMap<String,String>()
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) { ThermostatControls(d,t,true) { _,_,fields -> sent = fields } } } }
        val target = compose.onNodeWithText("Heating target (°C)")
        target.performTextReplacement("31")
        compose.onNodeWithText("Apply").assertIsNotEnabled()
        target.performTextReplacement("21.3")
        compose.onNodeWithText("Apply").assertIsNotEnabled()
        target.performTextReplacement("21.5")
        compose.onNodeWithText("Apply").performClick()
        assertEquals(mapOf("field" to "occupied_heating_setpoint","value" to "21.5"),sent)
        compose.onNodeWithText("Off").performClick()
        assertEquals("system_mode",sent["field"])
        assertEquals("0",sent["value"])
        compose.onNodeWithText("Thermostat settings").assertDoesNotExist()
        compose.onNodeWithText("Weekly schedule").assertDoesNotExist()
    }
    @Test fun dashboardUsesSavedOrderAndNavigationOpensInventoryAndGroups() {
        MockWebServer().use { server ->
            server.dispatcher = object : Dispatcher() {
                override fun dispatch(request: RecordedRequest): MockResponse {
                    val body = when(request.requestUrl!!.encodedPath) {
                        "/api/dashboard/config" -> """{"gateway_widget_initialized":true,"widgets":[{"id":"g","kind":"group","target":"2"},{"id":"gw","kind":"gateway"},{"id":"d","kind":"device","target":"lamp"},{"id":"o","kind":"open_contacts"},{"id":"p","kind":"present_sensors"},{"id":"a","kind":"automation_trigger","script_id":"evening.lua","trigger_id":"run"},{"id":"c","kind":"chart","target":"lamp","metric":10},{"id":"missing","kind":"device","target":"missing"}]}"""
                        "/api/status" -> """{"online":true,"devices":[{"id":"lamp","name":"Hall lamp","source":"zigbee","has_light":true,"light_on":false,"ieee_address":"0x123"}],"scene_groups":[{"id":2,"name":"Living room","members":[],"scenes":[{"id":3,"name":"Evening"}]}]}"""
                        "/api/automations" -> """{"scripts":[{"id":"evening.lua","enabled":true,"loaded":true,"triggers":[{"id":"run","label":"Evening automation"}]}]}"""
                        "/api/thread/status" -> """{"available":false}"""
                        "/api/controller-stats" -> """{"memory_free_bytes":2048,"storage_free_bytes":1024}"""
                        else -> """{"samples":[[1,1700000000,10,1]],"next_before":0}"""
                    }
                    return MockResponse().setBody(body)
                }
            }
            val base = server.url("/").toString().removeSuffix("/")
            compose.setContent { MaterialTheme { GatewayHome(base,"",compose.activity.lifecycle) { TextForTest() } } }
            compose.waitUntil(10000) { compose.onAllNodesWithText("Living room").fetchSemanticsNodes().isNotEmpty() }
            val groupTop = compose.onNodeWithText("Living room").fetchSemanticsNode().positionInRoot.y
            val gatewayTop = compose.onNodeWithText("Gateway state").fetchSemanticsNode().positionInRoot.y
            assertTrue(groupTop < gatewayTop)
            compose.onNodeWithText("Open contact sensors").performScrollTo().assertIsDisplayed()
            compose.onNodeWithText("Present presence sensors").performScrollTo().assertIsDisplayed()
            compose.onNodeWithText("Evening automation").performScrollTo().assertIsDisplayed()
            compose.onNodeWithText("Unavailable device").performScrollTo().assertIsDisplayed()
            compose.onNodeWithText("Edit dashboard").assertDoesNotExist()
            compose.onNodeWithText("Devices",useUnmergedTree = true).performClick()
            compose.onNodeWithText("Hall lamp").assertIsDisplayed()
            compose.onNodeWithContentDescription("History").performScrollTo().performClick()
            compose.onNodeWithText("Hall lamp history").assertIsDisplayed()
            compose.onNodeWithText("7 days").performClick()
            compose.onNodeWithText("All retained").performClick()
            compose.onNodeWithText("Close").performClick()
            compose.onNodeWithText("Groups",useUnmergedTree = true).performClick()
            compose.onNodeWithText("Evening").assertIsDisplayed()
            compose.onNodeWithText("App").performClick()
            compose.onNodeWithText("App").assertIsSelected()
            compose.onNodeWithText("App management").assertIsDisplayed()
            compose.runOnUiThread { compose.activity.setContentView(android.widget.FrameLayout(compose.activity)) }
        }
    }

}

@androidx.compose.runtime.Composable
private fun TextForTest() { androidx.compose.material3.Text("App management") }
