package io.github.microhad

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Color
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], application = Application::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class NotificationArtworkTest {
    private val context get() = RuntimeEnvironment.getApplication()
    private fun pixels(bitmap: Bitmap): IntArray = IntArray(bitmap.width * bitmap.height).also {
        bitmap.getPixels(it, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
    }
    @Test fun everySourceHasDistinctVisibleArtworkAndTransparentCorners() {
        val rendered = listOf("window", "doorbell", "temperature", "humidity", "leak", "battery", "power", "air_quality").map { source ->
            val presentation = NotificationSources.presentation(source, null)
            val bitmap = requireNotNull(NotificationSources.largeIcon(context, presentation))
            assertSame(bitmap, NotificationSources.largeIcon(context, presentation))
            assertEquals(0, Color.alpha(bitmap.getPixel(0, 0)))
            val content = pixels(bitmap)
            assertTrue("$source needs a visible symbol", content.any { it == Color.WHITE })
            assertTrue("$source needs a contrasting background", content.any { it == Color.rgb(38, 59, 80) })
            content
        }
        rendered.forEachIndexed { index, content ->
            rendered.take(index).forEach { assertFalse("Source artwork must be distinguishable", content.contentEquals(it)) }
        }
    }
    @Test @Config(qualifiers = "night") fun sourceArtworkRemainsVisibleInDarkMode() {
        val presentation = NotificationSources.presentation("temperature", null)
        val bitmap = requireNotNull(NotificationSources.largeIcon(context, presentation))
        assertTrue(pixels(bitmap).any { it == Color.WHITE })
        assertNull(NotificationSources.largeIcon(context, NotificationSources.presentation("default", null)))
        assertNull(NotificationSources.largeIcon(context, NotificationSources.presentation("unknown", null)))
    }
}
