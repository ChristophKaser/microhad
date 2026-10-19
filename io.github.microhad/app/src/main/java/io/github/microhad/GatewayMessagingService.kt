package io.github.microhad

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage

class GatewayMessagingService : FirebaseMessagingService() {
    override fun onNewToken(token: String) { PushRegistration.schedule(applicationContext) }
    override fun onMessageReceived(message: RemoteMessage) {
        val store = GatewayStore(this)
        if (store.base.isEmpty() || store.resetPending) return
        val sender = store.config?.firebase?.senderId ?: return
        if (message.from != sender) return
        val data = message.data
        val legacy = message.notification
        val key = data["key"]?.takeIf { it.isNotBlank() && it.length <= 256 }
            ?: if (legacy != null) (message.messageId ?: "gateway-test") else return
        val manager = NotificationManagerCompat.from(this)
        when (data["action"] ?: if (legacy != null) "show" else "") {
            "clear" -> manager.cancel(key, 0)
            "show" -> {
                if (Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
                (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(
                    NotificationChannel(CHANNEL, getString(R.string.gateway_alerts_channel_name), NotificationManager.IMPORTANCE_DEFAULT))
                val intent = Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
                val pending = PendingIntent.getActivity(this, key.hashCode(), intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
                manager.notify(key, 0, NotificationCompat.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_gateway)
                    .setContentTitle((data["title"] ?: legacy?.title).orEmpty().take(256)).setContentText((data["body"] ?: legacy?.body).orEmpty().take(2048))
                    .setContentIntent(pending).setAutoCancel(true).build())
            }
        }
    }
    companion object { const val CHANNEL = "microhad.gateway_alerts" }
}
