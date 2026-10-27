package io.github.microhad

import org.junit.Assert.*
import org.junit.Test

class HistoryAxisTest {
    @Test fun constantReadingsHaveHeadroomOnBothSides() {
        for (value in listOf(-20.0, 0.0, 21.5, 1200.0)) {
            val axis = historyAxis(1, listOf(value, value))
            assertTrue(axis.min < value)
            assertTrue(axis.max > value)
            assertTrue(axis.ticks.size in 3..9)
        }
    }
    @Test fun boundsContainNegativeAndFractionalMeasurements() {
        for (values in listOf(listOf(-15.5, -4.2), listOf(.001, .009), listOf(-2.0, 8.0), listOf(650.0, 1800.0))) {
            val axis = historyAxis(1, values)
            assertTrue(axis.min <= values.min())
            assertTrue(axis.max >= values.max())
            assertTrue(axis.step > 0 && axis.step.isFinite())
            assertTrue(axis.ticks.zipWithNext().all { (a,b) -> b > a })
        }
    }
    @Test fun percentReadingsStayWithinMeaningfulBounds() {
        for (metric in listOf(2,6,11)) for (values in listOf(listOf(0.0),listOf(100.0),listOf(0.0,100.0))) {
            val axis = historyAxis(metric,values)
            assertTrue(axis.min >= 0)
            assertTrue(axis.max <= 100)
            assertTrue(axis.max > axis.min)
        }
    }
    @Test fun stateChartsUseOnlyActualStatesAsTicks() {
        for (metric in listOf(3,5,10)) {
            val axis = historyAxis(metric,listOf(1.0))
            assertEquals(listOf(0.0,1.0),axis.ticks)
            assertTrue(axis.min < 0 && axis.max > 1)
        }
        assertEquals((0..5).map(Int::toDouble),historyAxis(9,listOf(2.0)).ticks)
    }
}
