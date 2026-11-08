package io.github.microhad

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ElectricalMeasurementsTest {
    @Test fun electricalReadingsPreserveZeroUnknownAndUnits() {
        val device=HomeDevice(JSONObject("""{"id":"plug","source":"matter","has_power":true,"power_known":true,"power_watts":0,"electrical":{"active_power":{"value":0,"unit":"W","quality":"measured"},"voltage":{"value":230.125,"unit":"V","quality":"measured"},"active_current":{"value":null,"unit":"A","quality":"unknown"},"cumulative_imported_energy":{"value":123.456789,"unit":"kWh","quality":"measured"},"cumulative_energy_reset":{"imported_reset_timestamp":5}}}"""))
        val readings=deviceCardReadings(device)
        assertEquals("Smart plugs",device.category)
        assertEquals(1,readings.count{it.key=="power"})
        assertFalse(readings.any{it.key=="electrical.active_power"})
        assertTrue(readings.first{it.key=="electrical.voltage"}.value.contains("230.125 V"))
        assertFalse(readings.first{it.key=="electrical.active_current"}.known)
        assertEquals("Unknown",readings.first{it.key=="electrical.active_current"}.value)
        assertFalse(readings.any{it.key=="electrical.cumulative_energy_reset"})
    }
    @Test fun electricalHistoryUsesReportedUnitsWithoutLegacyScaling() {
        val point=HistoryPoint(1,100,33,123.456789,"kWh")
        assertEquals(123.456789,point.y,0.0)
        assertEquals("kWh",point.displayUnit)
        assertTrue(metricNames[21].contains("Voltage",true))
        assertTrue(metricNames[33].contains("imported",true))
    }
}
