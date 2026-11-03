package io.github.microhad

import android.app.Application
import android.app.NotificationManager
import android.content.Context
import android.os.Bundle
import com.google.firebase.messaging.RemoteMessage
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], application = Application::class)
class GatewayMessagingServiceTest {
    private val context get() = RuntimeEnvironment.getApplication()
    private val manager get() = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    private fun message(action: String, sender: String = "123456") = RemoteMessage(Bundle().apply {
        putString("from", sender); putString("action", action); putString("key", "test-window")
        putString("title", "Window open"); putString("body", "Synthetic test alert")
    })
    @Before fun setup() {
        context.getSharedPreferences("gateway", 0).edit().clear().commit()
        manager.cancelAll()
        GatewayStore(context).save("http://gateway.local", "", """{"schema":1,"name":"Test","matter":true,"firebase":{"application_id":"1:123456:android:abcdef","api_key":"example-key","project_id":"example","sender_id":"123456"}}""")
    }
    @Test fun showUpdateAndClearUseOneKeyedNotification() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(message("show"))
        assertEquals(1, manager.activeNotifications.size)
        service.onMessageReceived(message("show"))
        assertEquals(1, manager.activeNotifications.size)
        service.onMessageReceived(message("clear"))
        assertEquals(0, manager.activeNotifications.size)
    }
    @Test fun resetAndDifferentFirebaseSenderCannotDisplayAlerts() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(message("show", "999999"))
        assertEquals(0, manager.activeNotifications.size)
        GatewayStore(context).clear()
        service.onMessageReceived(message("show"))
        assertEquals(0, manager.activeNotifications.size)
    }
    private fun alert(key: String, source: String? = null, legacy: Boolean = false) = RemoteMessage(Bundle().apply {
        putString("from", "123456"); putString("key", key)
        if (legacy) {
            putString("gcm.n.e", "1"); putString("gcm.n.title", "Alert"); putString("gcm.n.body", "Body")
        } else {
            putString("action", "show"); putString("title", "Alert"); putString("body", "Body")
        }
        source?.let { putString("source", it) }
    })
    private fun assertPresentation(key: String, source: String?, category: String?, icon: Int, legacy: Boolean = false) {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(alert(key, source, legacy))
        val notification = manager.activeNotifications.single().notification
        assertEquals(category?.let { "microhad.alerts.$it" } ?: GatewayMessagingService.CHANNEL, notification.channelId)
        assertEquals(icon, notification.smallIcon.resId)
        if (category == null) assertNull(notification.getLargeIcon()) else assertNotNull(notification.getLargeIcon())
        manager.cancelAll()
    }
    @Test fun explicitSourcesChooseTheirChannelsAndIcons() {
        listOf(
            "window" to R.drawable.ic_notification_window,
            "doorbell" to R.drawable.ic_notification_doorbell,
            "temperature" to R.drawable.ic_notification_temperature,
            "humidity" to R.drawable.ic_notification_humidity,
            "leak" to R.drawable.ic_notification_leak,
            "battery" to R.drawable.ic_notification_battery,
            "power" to R.drawable.ic_notification_power,
            "air_quality" to R.drawable.ic_notification_air_quality
        ).forEach { (source, icon) -> assertPresentation("automation:test", source, source, icon) }
        assertNotEquals(NotificationSources.presentation("humidity", null).channel, NotificationSources.presentation("leak", null).channel)
    }
    @Test fun oldGatewayKeysInferSources() {
        assertPresentation("contact:sensor:window", null, "window", R.drawable.ic_notification_window)
        assertPresentation("legacy:sensor:temperature:low", null, "temperature", R.drawable.ic_notification_temperature)
        assertPresentation("value:sensor:temperature:high", null, "temperature", R.drawable.ic_notification_temperature)
        assertPresentation("value:sensor:humidity:low", null, "humidity", R.drawable.ic_notification_humidity)
        assertPresentation("value:sensor:battery:low", null, "battery", R.drawable.ic_notification_battery)
        assertPresentation("value:sensor:power:high", null, "power", R.drawable.ic_notification_power)
        listOf("co2", "pm25").forEach { assertPresentation("value:sensor:$it:high", null, "air_quality", R.drawable.ic_notification_air_quality) }
        listOf("brightness", "wifi_signal", "link_quality").forEach {
            assertPresentation("value:sensor:$it:low", null, null, R.drawable.ic_gateway)
        }
        assertPresentation("automation:test", null, null, R.drawable.ic_gateway)
        assertPresentation("unknown", null, null, R.drawable.ic_gateway)
    }
    @Test fun explicitSourceOverridesKeyIncludingDefaultAndUnknownSources() {
        assertPresentation("contact:sensor:window", "humidity", "humidity", R.drawable.ic_notification_humidity)
        listOf("default", "presence", "motion", "radio", "light", "automation", "unexpected").forEach {
            assertPresentation("contact:sensor:window", it, null, R.drawable.ic_gateway)
        }
    }
    @Test fun notificationPayloadUsesSourceInForeground() {
        assertPresentation("legacy-message", "doorbell", "doorbell", R.drawable.ic_notification_doorbell, legacy = true)
        assertPresentation("legacy-message", null, null, R.drawable.ic_gateway, legacy = true)
    }
    @Test fun sourceMetadataDoesNotChangeKeyedUpdateOrClear() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(alert("test-window", "window"))
        service.onMessageReceived(alert("test-window", "window"))
        assertEquals(1, manager.activeNotifications.size)
        service.onMessageReceived(message("clear"))
        assertEquals(0, manager.activeNotifications.size)
    }
    @Test fun channelRegistrationPreservesUserSettings() {
        // Simulate an existing channel whose importance the user has lowered.
        val channel = android.app.NotificationChannel("microhad.alerts.doorbell", "Doorbells", android.app.NotificationManager.IMPORTANCE_LOW)
        manager.createNotificationChannel(channel)
        NotificationSources.register(context)
        assertEquals(android.app.NotificationManager.IMPORTANCE_LOW, manager.getNotificationChannel(channel.id).importance)
        assertEquals(9, manager.notificationChannels.size)
    }

    private fun oneShot(id: String?, sender: String = "123456", action: String = "show", key: String? = null) = RemoteMessage(Bundle().apply {
        putString("from", sender); putString("action", action); putString("source", "doorbell")
        putString("title", "Doorbell"); putString("body", "At the front door")
        id?.let { putString("google.message_id", it) }
        key?.let { putString("key", it) }
    })
    @Test fun dataOnlyOneShotMessagesKeepSeparateEventsAndDeduplicateRedelivery() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(oneShot("event-1"))
        service.onMessageReceived(oneShot("event-2"))
        service.onMessageReceived(oneShot("event-1"))
        assertEquals(2, manager.activeNotifications.size)
        assertEquals(setOf("fcm:event-1", "fcm:event-2"), manager.activeNotifications.map { it.tag }.toSet())
        manager.activeNotifications.forEach {
            assertEquals("microhad.alerts.doorbell", it.notification.channelId)
            assertEquals(R.drawable.ic_notification_doorbell, it.notification.smallIcon.resId)
            assertNotNull(it.notification.getLargeIcon())
            assertEquals("Doorbell", it.notification.extras.getString(android.app.Notification.EXTRA_TITLE))
            assertEquals("At the front door", it.notification.extras.getString(android.app.Notification.EXTRA_TEXT))
        }
    }
    @Test fun malformedIdentitiesAndUnkeyedClearMessagesCannotPostOrClearAlerts() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        listOf(null, "", " ", "x".repeat(253)).forEach { service.onMessageReceived(oneShot(it)) }
        listOf("", " ", "x".repeat(257)).forEach { service.onMessageReceived(oneShot("valid-id", key = it)) }
        service.onMessageReceived(oneShot("event-1", action = "clear"))
        service.onMessageReceived(oneShot("event-1", action = "unsupported"))
        assertEquals(0, manager.activeNotifications.size)
        service.onMessageReceived(oneShot("event-1"))
        service.onMessageReceived(oneShot("event-1", action = "clear"))
        assertEquals(1, manager.activeNotifications.size)
    }
    @Test fun malformedExplicitKeyDoesNotFallBackToLegacyNotificationIdentity() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(alert("", "doorbell", legacy = true))
        assertEquals(0, manager.activeNotifications.size)
    }
    @Test fun oneShotMessagesStillRequireConfiguredSenderAndGateway() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(oneShot("event-1", sender = "wrong-sender"))
        assertEquals(0, manager.activeNotifications.size)
        GatewayStore(context).clear()
        service.onMessageReceived(oneShot("event-1"))
        assertEquals(0, manager.activeNotifications.size)
    }
    @Test fun artworkSourceCanChangeWithoutLeavingAnOldKeyedCard() {
        val service = Robolectric.buildService(GatewayMessagingService::class.java).create().get()
        service.onMessageReceived(alert("test-window", "window"))
        assertNotNull(manager.activeNotifications.single().notification.getLargeIcon())
        service.onMessageReceived(alert("test-window", "default"))
        assertEquals(1, manager.activeNotifications.size)
        assertNull(manager.activeNotifications.single().notification.getLargeIcon())
        service.onMessageReceived(message("clear"))
        assertEquals(0, manager.activeNotifications.size)
    }

}
