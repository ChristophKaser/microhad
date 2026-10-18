package org.microhad.companion

import org.json.JSONObject

data class FirebaseClientConfig(val applicationId: String, val apiKey: String, val projectId: String, val senderId: String)
data class GatewayConfig(val name: String, val matter: Boolean, val firebase: FirebaseClientConfig?) {
    companion object {
        fun parse(text: String): GatewayConfig {
            require(text.length <= 8192) { "Gateway configuration too large" }
            val json = JSONObject(text)
            require(json.getInt("schema") == 1) { "Unsupported gateway configuration version" }
            val firebase = if (json.isNull("firebase")) null else json.getJSONObject("firebase").let {
                fun field(key: String): String = it.getString(key).also { value ->
                    require(value.isNotBlank() && value.length <= 512 && value.none { c -> c <= ' ' }) { "Invalid Firebase $key" }
                }
                FirebaseClientConfig(field("application_id"), field("api_key"), field("project_id"), field("sender_id")).also { config ->
                    require(config.applicationId.startsWith("1:${config.senderId}:android:") && config.senderId.all(Char::isDigit)) {
                        "Firebase application and sender IDs do not match"
                    }
                }
            }
            return GatewayConfig(json.getString("name").take(128), json.getBoolean("matter"), firebase)
        }
    }
}
