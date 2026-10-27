package io.github.microhad

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

@Composable
internal fun HistoryChart(repository: HomeRepository, d: HomeDevice, initialRange: String, fixedMetric: Int?, controls: Boolean) {
    val copy = homeCopy()
    var range by remember(d.id,initialRange) { mutableStateOf(initialRange) }
    var metric by remember(d.id,fixedMetric) { mutableIntStateOf(fixedMetric ?: 1) }
    var points by remember(d.historyId,range) { mutableStateOf<List<HistoryPoint>>(emptyList()) }
    var loading by remember { mutableStateOf(true) }
    var error by remember { mutableStateOf("") }
    var selected by remember { mutableStateOf<HistoryPoint?>(null) }
    var now by remember { mutableLongStateOf(System.currentTimeMillis()/1000) }
    LaunchedEffect(repository,d.historyId,range) {
        while(true) {
            loading = true; error = ""
            try { now = System.currentTimeMillis()/1000; points = repository.history(d.historyId,range,now) }
            catch(e: CancellationException) { throw e }
            catch(e: Exception) { error = e.message ?: copy.label("History unavailable") }
            finally { loading = false }
            delay(30000)
        }
    }
    if(controls) {
        Row(Modifier.horizontalScroll(rememberScrollState())) { listOf("day" to "24 hours","week" to "7 days","all" to "All retained").forEach { (value,label) -> FilterChip(selected = range == value,onClick = { range = value },label = { Text(copy.label(label)) }) } }
        val metrics = points.map { it.metric }.distinct().sorted()
        LaunchedEffect(metrics) { if(metric !in metrics && metrics.isNotEmpty()) metric = metrics.first() }
        Row(Modifier.horizontalScroll(rememberScrollState())) { metrics.forEach { m -> FilterChip(selected = metric == m,onClick = { metric = m },label = { Text(copy.label(metricNames.getOrElse(m) { copy.format("Metric %1\$s",m) })) }) } }
    }
    if(loading) LinearProgressIndicator(Modifier.fillMaxWidth())
    if(error.isNotEmpty()) Text(error,color = MaterialTheme.colorScheme.error)
    val all = points.filter { it.metric == metric && it.y.isFinite() }.sortedBy { it.time }
    val unit = all.lastOrNull()?.displayUnit
    val sameUnit = all.filter { it.displayUnit == unit }
    val start = if(range == "all") minOf(sameUnit.firstOrNull()?.time ?: now, now - 60) else now-if(range == "week") 604800 else 86400
    val visible = historyWindow(points,metric,start,now)
    LaunchedEffect(metric,range) { selected = null }
    HistoryPlot(
        deviceName = d.name,
        metric = metric,
        points = visible,
        latest = all.lastOrNull(),
        start = start,
        end = now,
        selected = selected,
        onSelect = { selected = it },
    )
}
