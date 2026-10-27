package io.github.microhad

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.material3.Icon
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.DateFormat
import java.text.DecimalFormat
import java.text.NumberFormat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.*

internal data class HistoryAxis(val min: Double, val max: Double, val ticks: List<Double>, val step: Double)

/** Padded, human-readable bounds keep constant values visible and discrete states distinct. */
internal fun historyAxis(metric: Int, values: List<Double>): HistoryAxis {
    if (metric in listOf(3, 5, 10)) return HistoryAxis(-.2, 1.2, listOf(0.0, 1.0), 1.0)
    if (metric == 9) return HistoryAxis(-.4, 5.4, (0..5).map(Int::toDouble), 1.0)
    val finite = values.filter(Double::isFinite)
    val low = finite.minOrNull() ?: 0.0
    val high = finite.maxOrNull() ?: 1.0
    val padding = if (high == low) max(abs(low) * .05, .5) else (high - low) * .08
    val bottom = if (metric in listOf(2, 6, 11)) max(0.0, low - padding) else low - padding
    val top = if (metric in listOf(2, 6, 11)) min(100.0, high + padding) else high + padding
    val span = (top - bottom).coerceAtLeast(.001)
    val magnitude = 10.0.pow(floor(log10(span / 4)))
    val ratio = span / 4 / magnitude
    val step = ((listOf(1.0, 2.0, 2.5, 5.0, 10.0).firstOrNull { it >= ratio } ?: 10.0)) * magnitude
    val min = floor(bottom / step) * step
    val max = ceil(top / step) * step
    val count = ((max - min) / step).roundToInt().coerceIn(1, 8)
    return HistoryAxis(min, max, (0..count).map { min + it * step }, step)
}

private fun chartColor(metric: Int) = when (metric) {
    1 -> Color(0xFFC66A36)
    2, 11 -> Color(0xFF327DAA)
    3 -> Color(0xFF8862AD)
    4 -> Color(0xFFAE7C20)
    5, 9 -> Color(0xFF38856F)
    7, 12, 13, 14, 15 -> Color(0xFF975A99)
    10 -> Color(0xFFAD7C10)
    else -> Color(0xFF587D96)
}

private fun axisLabel(metric: Int, value: Double, step: Double, copy: HomeCopy): String = when (metric) {
    3 -> copy.label(if (value == 0.0) "Closed" else "Open")
    5 -> copy.label(if (value == 0.0) "Clear" else "Occupied")
    10 -> copy.label(if (value == 0.0) "Off" else "On")
    9 -> copy.label(listOf("good", "fair", "moderate", "poor", "very poor", "extremely poor")[value.roundToInt().coerceIn(0, 5)])
    else -> if (abs(value) >= 1_000_000) DecimalFormat("0.#E0").format(value) else NumberFormat.getNumberInstance().apply {
        maximumFractionDigits = if (step >= 1) 0 else (ceil(-log10(step)).toInt() + 1).coerceIn(0, 4)
        isGroupingUsed = false
    }.format(if (abs(value) < step / 1000) 0.0 else value)
}

@Composable
internal fun HistoryPlot(
    deviceName: String,
    metric: Int,
    points: List<HistoryPoint>,
    latest: HistoryPoint?,
    start: Long,
    end: Long,
    selected: HistoryPoint?,
    onSelect: (HistoryPoint) -> Unit,
) {
    val copy = homeCopy()
    val colors = MaterialTheme.colorScheme
    val accent = chartColor(metric)
    val discrete = metric in listOf(3, 5, 9, 10)
    val metricName = copy.label(metricNames.getOrElse(metric) { copy.format("Metric %1\$s", metric) })
    val value = latest?.let { point ->
        if (discrete) copy.label(point.label) else {
            val format = NumberFormat.getNumberInstance().apply { maximumFractionDigits = if (metric in listOf(6, 11)) 0 else 2 }
            "${format.format(point.y)} ${point.displayUnit}".trim()
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            readingIcon(when(metric) { 1 -> "temperature"; 2 -> "humidity"; else -> "" })?.let {
                Icon(painterResource(it), contentDescription = null, tint = accent, modifier = Modifier.size(16.dp))
            }
            Text(metricName, style = MaterialTheme.typography.labelMedium, color = colors.onSurfaceVariant)
        }
        if (value != null) Text(value, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Medium, color = accent)
        if (points.isEmpty()) {
            Text(copy.label("No retained samples for this time period."), style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
            return@Column
        }
        val axis = remember(metric, points) { historyAxis(metric, points.map { it.y }) }
        val density = LocalDensity.current
        val measurer = rememberTextMeasurer()
        val labelStyle = MaterialTheme.typography.labelSmall.copy(fontSize = 10.sp, color = colors.onSurfaceVariant)
        val use24Hours = android.text.format.DateFormat.is24HourFormat(LocalContext.current)
        val pattern = android.text.format.DateFormat.getBestDateTimePattern(Locale.getDefault(), when {
            end - start < 180 -> if (use24Hours) "Hms" else "hms"
            end - start <= 172800 -> if (use24Hours) "Hm" else "hm"
            end - start > 300 * 86400L -> "yMMM"
            else -> "MMMd"
        })
        val timeFormat = remember(pattern) { SimpleDateFormat(pattern, Locale.getDefault()) }
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val width = with(density) { maxWidth.toPx() }
            val gap = with(density) { 8.dp.toPx() }
            val yLabels = axis.ticks.map { tick ->
                measurer.measure(AnnotatedString(axisLabel(metric, tick, axis.step, copy)), labelStyle, maxLines = 1, overflow = TextOverflow.Ellipsis, constraints = Constraints(maxWidth = (width * .4f).toInt().coerceAtLeast(1)))
            }
            val textHeight = yLabels.maxOf { it.size.height }
            val chartHeight = maxOf(210.dp, with(density) { (textHeight * (axis.ticks.size - 1)).toDp() } + 48.dp)
            val height = with(density) { chartHeight.toPx() }
            val left = yLabels.maxOf { it.size.width }.toFloat() + gap
            val top = maxOf(gap * 2, textHeight.toFloat())
            val bottom = height - textHeight - gap * 2
            val right = (width - gap).coerceAtLeast(left + 1)
            val plot = Rect(left, top, right, bottom.coerceAtLeast(top + 1))
            val longestTimeLabel = measurer.measure(AnnotatedString(timeFormat.format(Date(start * 1000))), labelStyle).size.width
            val tickCount = (plot.width / (longestTimeLabel + gap * 2)).toInt().coerceIn(2, 4)
            val timeTicks = (0 until tickCount).map { index -> start + ((end - start) * index / (tickCount - 1)) }
            val timeLabels = timeTicks.map { time -> measurer.measure(AnnotatedString(timeFormat.format(Date(time * 1000))), labelStyle) }
            val unit = latest?.displayUnit.orEmpty()
            val unitLabel = measurer.measure(AnnotatedString(unit), labelStyle)
            fun x(time: Long) = plot.left + ((time - start).toDouble() / (end - start).coerceAtLeast(1) * plot.width).toFloat()
            fun y(value: Double) = plot.bottom - ((value - axis.min) / (axis.max - axis.min) * plot.height).toFloat()
            fun position(point: HistoryPoint) = Offset(x(point.time), y(point.y))
            fun select(position: Offset) {
                val time = start + ((position.x - plot.left) / plot.width).coerceIn(0f, 1f).toDouble() * (end - start)
                points.minByOrNull { abs(it.time - time) }?.let(onSelect)
            }
            val description = copy.format("%1\$s %2\$s history, %3\$s samples, %4\$s", deviceName, metricName, points.size, copy.label(points.last().label))
            Canvas(
                Modifier.fillMaxWidth().height(chartHeight)
                    .semantics { contentDescription = description }
                    .pointerInput(points, plot, start, end) { detectTapGestures(onTap = ::select) }
                    .pointerInput(points, plot, start, end) { detectHorizontalDragGestures(onDragStart = ::select) { change, _ -> change.consume(); select(change.position) } },
            ) {
                val dashed = PathEffect.dashPathEffect(floatArrayOf(3.dp.toPx(), 4.dp.toPx()))
                axis.ticks.forEachIndexed { index, tick ->
                    val at = y(tick)
                    drawLine(colors.outlineVariant.copy(alpha = .65f), Offset(plot.left, at), Offset(plot.right, at), pathEffect = dashed)
                    drawText(yLabels[index], topLeft = Offset(plot.left - gap - yLabels[index].size.width, at - yLabels[index].size.height / 2f))
                    drawLine(colors.outlineVariant, Offset(plot.left - 3.dp.toPx(), at), Offset(plot.left, at))
                }
                timeTicks.forEachIndexed { index, time ->
                    val at = x(time)
                    drawLine(colors.outlineVariant.copy(alpha = .3f), Offset(at, plot.top), Offset(at, plot.bottom))
                    val label = timeLabels[index]
                    drawText(label, topLeft = Offset((at - label.size.width / 2f).coerceIn(0f, (size.width - label.size.width).coerceAtLeast(0f)), plot.bottom + gap))
                }
                if (unit.isNotEmpty()) drawText(unitLabel, topLeft = Offset(plot.left + gap / 2, 0f))
                drawLine(colors.outlineVariant, Offset(plot.left, plot.top), Offset(plot.left, plot.bottom), strokeWidth = 1.dp.toPx())
                drawLine(colors.outlineVariant, Offset(plot.left, plot.bottom), Offset(plot.right, plot.bottom), strokeWidth = 1.dp.toPx())
                clipRect(plot.left, plot.top, plot.right, plot.bottom) {
                    val path = Path()
                    val first = position(points.first())
                    path.moveTo(first.x, first.y)
                    points.zipWithNext().forEach { (previous, next) ->
                        val at = position(next)
                        if (discrete) path.lineTo(at.x, y(previous.y))
                        path.lineTo(at.x, at.y)
                    }
                    val last = position(points.last())
                    val fill = Path().apply { addPath(path); lineTo(last.x, plot.bottom); lineTo(first.x, plot.bottom); close() }
                    drawPath(fill, Brush.verticalGradient(listOf(accent.copy(alpha = .2f), accent.copy(alpha = .015f)), plot.top, plot.bottom))
                    drawPath(path, accent, style = Stroke(width = 2.2.dp.toPx(), cap = StrokeCap.Round))
                    drawCircle(colors.surface, 4.dp.toPx(), last)
                    drawCircle(accent, 2.5.dp.toPx(), last)
                    selected?.let { picked -> points.find { it.cursor == picked.cursor } }?.let { picked ->
                        val at = position(picked)
                        drawLine(accent.copy(alpha = .5f), Offset(at.x, plot.top), Offset(at.x, plot.bottom), pathEffect = dashed)
                        drawCircle(colors.surface, 5.dp.toPx(), at)
                        drawCircle(accent, 3.dp.toPx(), at)
                    }
                }
            }
        }
        selected?.let { point ->
            Text(copy.format("%1\$s: %2\$s", DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(point.time * 1000)), copy.label(point.label)), style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
        }
    }
}
