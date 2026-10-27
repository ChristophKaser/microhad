package io.github.microhad

import org.junit.Assert.*
import org.junit.Test

class ReadingSeverityTest {
    @Test fun concentrationConversionsProduceTheSameQualityBand() {
        assertEquals(ReadingSeverity.MODERATE,readingSeverity("co2",1200,"ppm",true))
        assertEquals(readingSeverity("co2",1200,"ppm",true),readingSeverity("co2",1200000,"ppb",true))
        assertEquals(readingSeverity("co2",1200,"ppm",true),readingSeverity("co2",1200000000,"ppt",true))
        assertEquals(ReadingSeverity.POOR,readingSeverity("pm25",40,"ug/m3",true))
        assertEquals(readingSeverity("pm25",40,"ug/m3",true),readingSeverity("pm25",.04,"mg/m3",true))
        assertEquals(readingSeverity("pm25",40,"ug/m3",true),readingSeverity("pm25",40000,"ng/m3",true))
    }
    @Test fun webUiThresholdsUseTheHigherBandAtTheBoundary() {
        assertEquals(ReadingSeverity.GOOD,readingSeverity("co2",799,"ppm",true))
        assertEquals(ReadingSeverity.FAIR,readingSeverity("co2",800,"ppm",true))
        assertEquals(ReadingSeverity.EXTREMELY_POOR,readingSeverity("co2",5000,"ppm",true))
        assertEquals(ReadingSeverity.FAIR,readingSeverity("pm25",10,"ug/m3",true))
        assertEquals(ReadingSeverity.EXTREMELY_POOR,readingSeverity("pm25",125,"ug/m3",true))
    }
    @Test fun unknownOrIncompatibleMeasurementsAreNeverColoredAsGood() {
        assertNull(readingSeverity("co2",0,"ppm",false))
        assertNull(readingSeverity("co2",null,"ppm",true))
        assertNull(readingSeverity("co2",2,"mg/m3",true))
        assertNull(readingSeverity("pm25",2,"ppm",true))
        assertNull(readingSeverity("co2",Double.NaN,"ppm",true))
        assertNull(readingSeverity("air_quality","unknown","",true))
        assertEquals(ReadingSeverity.VERY_POOR,readingSeverity("air_quality","very_poor","",true))
    }
    @Test fun bothLegacyAndTypedReadingsCarryQualityIntoCards() {
        val raw = org.json.JSONObject("""{"id":"air","has_co2":true,"co2_known":true,"co2_ppm":1600,"has_air_quality":true,"air_quality_known":false,"sensor_values":{"pm25":{"value":0.04,"unit":"mg/m3","quality":"measured"}}}""")
        val readings = deviceCardReadings(HomeDevice(raw))
        assertEquals(ReadingSeverity.POOR,readings.single { it.key == "co2" }.severity)
        assertEquals(ReadingSeverity.POOR,readings.single { it.key == "pm25" }.severity)
        assertNull(readings.single { it.key == "air_quality" }.severity)
    }
}
