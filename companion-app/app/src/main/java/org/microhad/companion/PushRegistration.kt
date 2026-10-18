package org.microhad.companion

import android.content.Context
import android.os.Build
import androidx.work.*
import com.google.android.gms.common.ConnectionResult
import com.google.android.gms.common.GoogleApiAvailability
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.tasks.await
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException

object PushRegistration {
    fun schedule(context: Context) {
        val store = GatewayStore(context)
        if (store.base.isEmpty() || store.config?.firebase == null) return
        val request = OneTimeWorkRequestBuilder<PushRegistrationWorker>()
            .setInputData(workDataOf("generation" to store.generation))
            .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS).build()
        WorkManager.getInstance(context).enqueueUniqueWork("gateway-push-registration", ExistingWorkPolicy.REPLACE, request)
    }
    fun retryable(code: Int): Boolean = code == 408 || code == 429 || code >= 500
}
class PushRegistrationWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val store = GatewayStore(applicationContext)
        val generation = inputData.getString("generation")
        if (generation != store.generation || store.base.isEmpty()) return Result.success()
        if (GoogleApiAvailability.getInstance().isGooglePlayServicesAvailable(applicationContext) != ConnectionResult.SUCCESS) {
            store.status("Google Play services unavailable; commissioning still works with manual codes")
            return Result.failure()
        }
        return try {
            if (!CompanionApplication.initializeFirebase(applicationContext)) return Result.success()
            val token = FirebaseMessaging.getInstance().token.await()
            if (generation != store.generation || isStopped) return Result.success()
            val body = JSONObject().put("installation_id", store.installationId()).put("model", Build.MODEL).put("token", token)
            GatewayApi(store.base, store.fingerprint).register(body.toString())
            if (generation == store.generation) store.status("Registered")
            Result.success()
        } catch (e: CancellationException) { throw e }
        catch (e: GatewayHttpException) {
            store.status("Registration failed: HTTP ${e.code}")
            if (PushRegistration.retryable(e.code)) Result.retry() else Result.failure()
        } catch (_: SSLException) {
            store.status("Gateway certificate rejected; reconnect in settings")
            Result.failure()
        } catch (_: IOException) {
            store.status("Registration waiting for gateway or Firebase; will retry")
            Result.retry()
        } catch (_: Exception) {
            store.status("Firebase registration failed; check gateway Firebase configuration")
            Result.failure()
        }
    }
}
