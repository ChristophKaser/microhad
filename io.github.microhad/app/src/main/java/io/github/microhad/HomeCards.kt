package io.github.microhad

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.json.JSONObject

/** One surface treatment for inventory, dashboard widgets, and scene groups. */
@Composable
internal fun HomeCardSurface(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = .65f)),
        content = content,
    )
}

@Composable
internal fun HomeCard(
    title: String,
    subtitle: String? = null,
    leading: (@Composable () -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    HomeCardSurface {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                leading?.invoke()
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                    if (subtitle != null) Text(subtitle, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            ProvideTextStyle(MaterialTheme.typography.bodyMedium) { content() }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun GroupCard(group: JSONObject?, devices: List<HomeDevice>, enabled: Boolean, action: (String) -> Unit) {
    val copy = homeCopy()
    HomeCard(
        group?.optString("name") ?: copy.label("Unavailable group"),
        subtitle = group?.let { stringResource(R.string.home_group_summary, it.array("members").length(), it.optInt("id")) },
    ) {
        if (group == null) {
            Text(stringResource(R.string.home_this_saved_group_is_no_longer_configured), color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                group.array("members").strings().forEach { address ->
                    val device = devices.find { it.raw.optString("ieee_address") == address }
                    val state = if (device == null || !device.raw.flag("light_known", true)) "Unknown" else if (device.raw.flag("light_on")) "On" else "Off"
                    Text(copy.format("%1\$s · %2\$s", device?.name ?: address, copy.label(state)), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                group.array("scenes").objects().forEach { scene ->
                    FilledTonalButton(enabled = enabled, onClick = { action("/api/scenes/${group.getInt("id")}/${scene.getInt("id")}/activate") }) {
                        Text(scene.optString("name"))
                    }
                }
                OutlinedButton(enabled = enabled, onClick = { action("/api/scenes/${group.getInt("id")}/off") }) {
                    Text(stringResource(R.string.home_turn_group_off))
                }
            }
        }
    }
}
