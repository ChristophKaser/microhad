package io.github.microhad

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException

class GatewayHttpException(val code: Int) : IOException("Gateway returned HTTP $code")
class GatewayApi(private val base: String, pin: String = "") {
    private val client = GatewayTransport.client(pin)
    suspend fun config(): String = request("/api/companion/config")
    suspend fun register(body: String) { request("/api/push/devices/register", body) }
    suspend fun remove(id: String) { request("/api/push/devices/$id/delete", "") }
    private suspend fun request(path: String, body: String? = null): String = withContext(Dispatchers.IO) {
        val request = Request.Builder().url(base + path).apply {
            if (body != null) post(body.toRequestBody("application/json".toMediaType()))
        }.build()
        client.newCall(request).execute().use { response ->
            if (!response.isSuccessful) throw GatewayHttpException(response.code)
            val stream = response.body?.byteStream() ?: throw IOException("Empty gateway response")
            val output = java.io.ByteArrayOutputStream()
            val buffer = ByteArray(1024)
            while (true) {
                val count = stream.read(buffer, 0, minOf(buffer.size, 8193 - output.size()))
                if (count < 0) break
                output.write(buffer, 0, count)
                require(output.size() <= 8192) { "Gateway response too large" }
            }
            output.toString("UTF-8")
        }
    }
}
