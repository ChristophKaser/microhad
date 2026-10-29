package io.github.microhad

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONObject
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.SecretKeySpec

/** A single gateway session, encrypted with a non-exportable Android Keystore key. */
object GatewayAuth {
    private lateinit var context: Context
    fun initialize(value: Context) { context = value.applicationContext }
    private val prefs get() = context.getSharedPreferences("gateway-auth", Context.MODE_PRIVATE)
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey("microhad-session", null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder("microhad-session", KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    private fun origin(base: String): String {
        val uri = java.net.URI(base)
        val port = if (uri.port == -1 || uri.port == (if (uri.scheme == "https") 443 else 80)) "" else ":${uri.port}"
        val host = uri.host.lowercase().removeSurrounding("[", "]").let { if (':' in it) "[$it]" else it }
        return "${uri.scheme.lowercase()}://$host$port"
    }
    @Synchronized fun clear() { if (::context.isInitialized) check(prefs.edit().clear().commit()) }
    @Synchronized fun token(base: String): String {
        if (!::context.isInitialized || base.isEmpty() || prefs.getString("base", "") != origin(base)) return ""
        return runCatching {
            val bytes = Base64.decode(prefs.getString("session", ""), Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key(), javax.crypto.spec.GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
            cipher.updateAAD(origin(base).toByteArray(Charsets.UTF_8))
            cipher.doFinal(bytes.copyOfRange(12, bytes.size)).toString(Charsets.UTF_8)
        }.getOrDefault("")
    }
    @Synchronized private fun save(base: String, token: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()); updateAAD(origin(base).toByteArray(Charsets.UTF_8)) }
        val bytes = cipher.iv + cipher.doFinal(token.toByteArray(Charsets.UTF_8))
        check(prefs.edit().putString("base", origin(base)).putString("session", Base64.encodeToString(bytes, Base64.NO_WRAP)).commit())
    }
    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun unhex(value: String) = value.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun hmac(key: ByteArray, bytes: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
        init(SecretKeySpec(key, "HmacSHA256")); doFinal(bytes)
    }
    // Explicit UTF-8 PBKDF2 matches browser and Python for non-ASCII passwords.
    internal fun derive(password: String, salt: ByteArray): ByteArray {
        val key = password.toByteArray(Charsets.UTF_8)
        var block = hmac(key, salt + byteArrayOf(0, 0, 0, 1)); val result = block.copyOf()
        repeat(99999) { block = hmac(key, block); for (i in result.indices) result[i] = (result[i].toInt() xor block[i].toInt()).toByte() }
        key.fill(0); block.fill(0); return result
    }
    suspend fun login(base: String, pin: String, role: String, password: String) {
        val api = GatewayApi(base, pin)
        val challenge = JSONObject(api.request("/api/auth/challenge", JSONObject().put("role", role).toString()))
        if (challenge.optBoolean("open")) { clear(); return }
        val salt = challenge.getString("salt"); val nonce = challenge.getString("challenge")
        require(challenge.getInt("iterations") == 100000 && salt.matches(Regex("[0-9a-f]{32}")) && nonce.matches(Regex("[0-9a-f]{64}"))) { "Invalid login challenge" }
        val verifier = derive(password, unhex(salt))
        val proof = hex(hmac(verifier, "microhad-login-v1\n$nonce\n$role\npersistent".toByteArray(Charsets.UTF_8)))
        verifier.fill(0)
        val result = JSONObject(api.request("/api/auth/login", JSONObject().put("challenge", nonce).put("proof", proof).put("mode", "persistent").toString()))
        save(base, result.getString("token"))
    }
    fun logout() { clear() }
}
