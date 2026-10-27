package io.github.microhad

import kotlinx.coroutines.*
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.TimeUnit

class HomeRepositoryTest {
    private fun device(fields: String) = HomeDevice(JSONObject("""{"id":"a/b","name":"Test","source":"zigbee",$fields}"""))
    @Test fun inventoryCategoriesUnknownReadingsAndCapabilities() {
        val categories = mapOf("thermostat" to "Thermostats", "water_leak" to "Water leak detectors", "contact" to "Contact sensors", "temperature" to "Climate sensors", "air_quality" to "Air quality sensors", "occupancy" to "Motion sensors", "light" to "Lights", "remote" to "Remotes", "actor" to "Switches", "power" to "Smart plugs")
        categories.forEach { (capability,category) -> assertEquals(category, device("\"has_$capability\":true").category) }
        assertEquals("Other devices",device("\"kind\":\"unknown\"").category)
        assertEquals("Presence sensors",HomeDevice(JSONObject("""{"id":"f","source":"fritz_box","has_occupancy":true}""")).category)
        val unknown = device("\"has_temperature\":true,\"temperature_celsius\":0")
        assertEquals(listOf("Temperature: Unknown"),DeviceReadings(unknown))
        assertFalse(device("\"online\":false").available)
        assertFalse(device("\"controller_available\":false").available)
        assertTrue(device("\"endpoint_descriptors\":[{\"endpoint\":1,\"input_clusters\":[3]}]").canIdentify)
        assertFalse(device("\"has_light\":true").canIdentify)
        assertEquals("zigbee:",unknown.historyId)
    }
    @Test fun snapshotFindsEndpointTargetsAndMissingTargets() {
        val snapshot = HomeSnapshot(JSONObject("""{"devices":[{"id":"parent","endpoint_descriptors":[{"id":"child"}]}],"scene_groups":[]}"""))
        assertEquals("parent",snapshot.device("child")?.id)
        assertNull(snapshot.device("missing"))
    }
    @Test fun historyScalingAndUnitsMatchWebUi() {
        assertEquals(21.5,HistoryPoint(1,2,1,2150,null).y,0.0001)
        assertEquals(45.25,HistoryPoint(1,2,2,4525,null).y,0.0001)
        assertEquals(12.3,HistoryPoint(1,2,4,123,null).y,0.0001)
        assertEquals(21.5,HistoryPoint(1,2,1,21.5,"celsius").y,0.0001)
        assertEquals("°C",HistoryPoint(1,2,1,21.5,"celsius").displayUnit)
        assertEquals("Open",HistoryPoint(1,2,3,1,null).label)
        assertEquals("Clear",HistoryPoint(1,2,5,0,null).label)
        assertEquals("On",HistoryPoint(1,2,10,1,null).label)
        assertEquals(4.0,HistoryPoint(1,2,9,"very_poor",null).y,0.0)
        (1..19).forEach { assertTrue(metricNames[it].isNotEmpty()) }
    }
    @Test fun historyWindowHoldsValueAndSeparatesUnits() {
        val points = listOf(HistoryPoint(1,10,7,100,"ppm"),HistoryPoint(2,20,7,200,"ppb"),HistoryPoint(3,40,7,300,"ppb"))
        val visible = historyWindow(points,7,30,50)
        assertEquals(listOf(30L,40L),visible.map { it.time })
        assertTrue(visible.all { it.displayUnit == "ppb" })
        assertEquals(200.0,visible.first().y,0.0)
    }
    @Test fun ordinaryActionsEncodeIdsAndFormBodies() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("{}"))
            val repo = HomeRepository(GatewayApi(server.url("/").toString().removeSuffix("/")))
            repo.action(device("\"has_light\":true"),"light",mapOf("state" to "on"))
            val request = server.takeRequest()
            assertEquals("/api/devices/a%2Fb/light",request.path)
            assertEquals("state=on",request.body.readUtf8())
            assertEquals("application/x-www-form-urlencoded; charset=utf-8",request.getHeader("Content-Type"))
            assertTrue(runCatching { repo.action(device("\"online\":false"),"refresh") }.isFailure)
            assertEquals(1,server.requestCount)
        }
    }
    @Test fun historyPaginatesDeduplicatesAndDetectsBadCursors() = runBlocking {
        MockWebServer().use { server ->
            val repo = HomeRepository(GatewayApi(server.url("/").toString().removeSuffix("/")))
            server.enqueue(MockResponse().setBody("""{"samples":[[2,200,1,2100]],"next_before":2}"""))
            server.enqueue(MockResponse().setBody("""{"samples":[[1,100,1,2000],[2,200,1,2150]],"next_before":0}"""))
            val result = repo.history("zigbee:0x123","day",100000)
            assertEquals(listOf(1L,2L),result.map { it.cursor })
            assertEquals(21.5,result.last().y,0.0)
            assertEquals("/api/devices/zigbee%3A0x123/history?since=13600",server.takeRequest().path)
            assertTrue(server.takeRequest().path!!.endsWith("&before=2"))
            repeat(2) { server.enqueue(MockResponse().setBody("""{"samples":[],"next_before":2}""")) }
            assertTrue(runCatching { repo.history("id","all",1000) }.isFailure)
        }
    }
    @Test fun inventoryCanExceedCompanionConfigLimit() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("""{"devices":[],"scene_groups":[],"padding":"${"x".repeat(9000)}"}"""))
            assertTrue(HomeRepository(GatewayApi(server.url("/").toString().removeSuffix("/"))).status().devices.isEmpty())
        }
    }
    @Test fun matterCommandsUseClusterEndpointAndPollJobs() = runBlocking {
        MockWebServer().use { server ->
            val repo = HomeRepository(GatewayApi(server.url("/").toString().removeSuffix("/")))
            val d = HomeDevice(JSONObject("""{"id":"m","source":"matter","matter_node":"0123456789abcdef","endpoint_descriptors":[{"endpoint":2,"input_clusters":[6]},{"endpoint":3,"input_clusters":[8]}]}"""))
            server.enqueue(MockResponse().setBody("""{"schema":1,"ok":true,"state":"queued","job":7}"""))
            server.enqueue(MockResponse().setBody("""{"job":7,"state":"running"}"""))
            server.enqueue(MockResponse().setBody("""{"job":7,"state":"complete","response":{"schema":1,"ok":true,"result":{"state":"complete","job":"0000000000000007","result":{"node":"0123456789abcdef","endpoint":3,"cluster":8,"attribute":0,"value":{"type":"u8","value":127}}}}}"""))
            repo.action(d,"light",mapOf("brightness" to "50"))
            val request = server.takeRequest()
            assertEquals("/api/matter/devices/command",request.path)
            val body = JSONObject(request.body.readUtf8())
            assertEquals(3,body.getInt("endpoint")); assertEquals(127,body.getInt("level")); assertEquals("move_to_level",body.getString("command"))
            repeat(2) { assertEquals("/api/matter/jobs/7",server.takeRequest().path) }
            server.enqueue(MockResponse().setBody("""{"schema":1,"ok":true,"state":"queued","job":8}"""))
            server.enqueue(MockResponse().setBody("""{"job":8,"state":"failed","error":"unavailable"}"""))
            assertTrue(runCatching { repo.action(d,"actor",mapOf("state" to "off")) }.isFailure)
        }
    }
    @Test fun cancellationStopsInFlightResponse() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("{}").setBodyDelay(3,TimeUnit.SECONDS))
            val api = GatewayApi(server.url("/").toString().removeSuffix("/"))
            val job = launch { api.config() }
            withContext(Dispatchers.IO) { assertNotNull(server.takeRequest(2,TimeUnit.SECONDS)) }
            withTimeout(2000) { job.cancelAndJoin() }
        }
    }
}
