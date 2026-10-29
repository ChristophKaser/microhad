package io.github.microhad

import android.app.Application
import android.content.Context
import com.google.firebase.FirebaseApp
import com.google.firebase.FirebaseOptions

class CompanionApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        GatewayAuth.initialize(this)
        GatewayStore(this).processStarted()
        (getSystemService(NOTIFICATION_SERVICE) as android.app.NotificationManager).createNotificationChannel(
            android.app.NotificationChannel(GatewayMessagingService.CHANNEL, getString(R.string.gateway_alerts_channel_name), android.app.NotificationManager.IMPORTANCE_DEFAULT))
        runCatching { initializeFirebase(this) }.onFailure { GatewayStore(this).status("Firebase initialization failed; check gateway settings") }
    }
    companion object {
        @Synchronized fun initializeFirebase(context: Context): Boolean {
            val store = GatewayStore(context)
            if (store.resetPending) return false
            val config = store.config?.firebase ?: return false
            if (FirebaseApp.getApps(context).none { it.name == FirebaseApp.DEFAULT_APP_NAME }) {
                FirebaseApp.initializeApp(context, FirebaseOptions.Builder().setApplicationId(config.applicationId)
                    .setApiKey(config.apiKey).setProjectId(config.projectId).setGcmSenderId(config.senderId).build())
            }
            val active = FirebaseApp.getInstance().options
            check(active.applicationId == config.applicationId && active.apiKey == config.apiKey &&
                active.projectId == config.projectId && active.gcmSenderId == config.senderId) { "Restart required after Firebase configuration changes" }
            com.google.firebase.messaging.FirebaseMessaging.getInstance().isAutoInitEnabled = true
            return true
        }
    }
}
