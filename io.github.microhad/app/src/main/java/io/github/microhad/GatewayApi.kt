package io.github.microhad

import kotlinx.coroutines.suspendCancellableCoroutine
import okhttp3.Call
import okhttp3.Callback
import okhttp3.Response
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
    internal suspend fun request(path: String, body: String? = null, contentType: String = "application/json", maxBytes: Int = 8192): String = suspendCancellableCoroutine { continuation ->
        val request = Request.Builder().url(base + path).apply {
            if (body != null) post(body.toRequestBody(contentType.toMediaType()))
        }.build()
        val call = client.newCall(request)
        continuation.invokeOnCancellation { call.cancel() }
        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) { continuation.resumeWith(Result.failure(e)) }
            override fun onResponse(call: Call, response: Response) {
                val result = runCatching { response.use {
                    if (!response.isSuccessful) throw GatewayHttpException(response.code)
                    val stream = response.body?.byteStream() ?: throw IOException("Empty gateway response")
                    val output = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(1024)
                    while (true) {
                        val count = stream.read(buffer, 0, minOf(buffer.size, maxBytes + 1 - output.size()))
                        if (count < 0) break
                        output.write(buffer, 0, count)
                        require(output.size() <= maxBytes) { "Gateway response too large" }
                    }
                    output.toString("UTF-8")
                } }
                continuation.resumeWith(result)
            }
        })
    }
}
