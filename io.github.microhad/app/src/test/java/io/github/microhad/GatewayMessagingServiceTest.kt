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
}
