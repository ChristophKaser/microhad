package io.github.microhad.matter

/** Canonical, bounded v1 gateway JSON. No untrusted strings appear in exceptions. */
internal object MatterGatewayWire {
    enum class Error(val wire: String) {
        MALFORMED("malformed"), UNSUPPORTED("unsupported"), NOT_READY("not_ready"), BUSY("busy"),
        STALE_SESSION("stale_session"), STALE_GENERATION("stale_generation"), NOT_FOUND("not_found"),
        TIMEOUT("timeout"), CANCELLED("cancelled"), TRANSPORT("transport"), STORAGE("storage"),
        CAPACITY("capacity"), ACCESSORY("accessory"), PROTOCOL("protocol"), DISABLED("disabled")
    }
    class Failure(val error: Error) : IllegalStateException(error.wire)
    sealed class Value
    class Object(val fields: Map<String, Value>) : Value() {
        fun exact(required: Set<String>, optional: Set<String> = emptySet()): Object {
            check(fields.keys.containsAll(required) && fields.keys.all { it in required || it in optional })
            return this
        }
        fun obj(name: String) = fields[name] as? Object ?: bad()
        fun text(name: String) = (fields[name] as? Text)?.value ?: bad()
        fun number(name: String, min: Long = 0, max: Long = 0xffffffffL): Long {
            val value = (fields[name] as? Number)?.value ?: bad()
            check(value in min..max)
            return value
        }
        fun flag(name: String) = (fields[name] as? Flag)?.value ?: bad()
    }
    class Text(val value: String) : Value()
    class Number(val value: Long) : Value()
    class Flag(val value: Boolean) : Value()
    data class Session(val id: String, val state: String, val remainingSeconds: Int,
                       val error: Error? = null, val node: String? = null) {
        val terminal get() = state in setOf("complete", "cancelled", "failed", "timed_out")
    }
    data class Availability(val enabled: Boolean, val ready: Boolean, val configured: Boolean)
    data class ThreadState(val enabled: Boolean, val available: Boolean, val configured: Boolean,
                           val attached: Boolean, val state: String)
    sealed class Message
    class Reply(val id: Long, val response: Object) : Message()
    class Progress(val snapshot: Session) : Message()
    class Write(val session: String, val generation: Long, val write: Long, val data: ByteArray) : Message()

    private fun bad(): Nothing = throw Failure(Error.PROTOCOL)
    private fun check(condition: Boolean) { if (!condition) bad() }
    fun error(code: String): Error = Error.values().firstOrNull { it.wire == code && it !in
        setOf(Error.PROTOCOL, Error.DISABLED) } ?: bad()
    fun id(value: String): String {
        check(value.matches(Regex("[0-9a-f]{16}")) && value != "0000000000000000")
        return value
    }
    fun decode(text: String): Object {
        check(text.length <= 2048 && text.all { it.code <= 127 })
        return Parser(text).parse()
    }
    private class Parser(private val text: String) {
        private var at = 0
        fun parse(): Object {
            val result = value(0) as? Object ?: bad()
            space()
            check(at == text.length)
            return result
        }
        private fun space() { while (at < text.length && text[at] in " \r\n\t") at++ }
        private fun value(depth: Int): Value {
            space()
            check(at < text.length && depth <= 8)
            return when (text[at]) {
                '{' -> {
                    at++
                    val fields = LinkedHashMap<String, Value>()
                    space()
                    if (take('}')) Object(fields) else {
                        do {
                            space()
                            val key = string()
                            check(fields.size < 32 && key !in fields)
                            space(); check(take(':'))
                            fields[key] = value(depth + 1)
                            space()
                        } while (take(','))
                        check(take('}'))
                        Object(fields)
                    }
                }
                '"' -> Text(string())
                't' -> { literal("true"); Flag(true) }
                'f' -> { literal("false"); Flag(false) }
                in '0'..'9' -> {
                    val start = at
                    while (at < text.length && text[at] in '0'..'9') at++
                    check(at - start <= 10 && (at - start == 1 || text[start] != '0'))
                    Number(text.substring(start, at).toLongOrNull() ?: bad())
                }
                else -> bad()
            }
        }
        private fun string(): String {
            check(take('"'))
            val start = at
            while (at < text.length && text[at] != '"') {
                check(text[at].code in 32..126 && text[at] != '\\' && at - start < 512)
                at++
            }
            check(take('"'))
            return text.substring(start, at - 1)
        }
        private fun take(char: Char): Boolean = (at < text.length && text[at] == char).also { if (it) at++ }
        private fun literal(word: String) { check(text.startsWith(word, at)); at += word.length }
    }

    fun body(vararg fields: Pair<String, Value>): Object = Object(linkedMapOf("schema" to Number(1)) + fields)
    fun text(value: String) = Text(value)
    fun number(value: Long) = Number(value)
    fun flag(value: Boolean) = Flag(value)
    fun encode(value: Object): String {
        fun render(item: Value): String = when (item) {
            is Object -> item.fields.entries.joinToString(",", "{", "}") { (key, child) ->
                check(key.matches(Regex("[a-z_][a-z0-9_]*")))
                "\"$key\":" + render(child)
            }
            is Text -> { check(item.value.all { it.code in 32..126 && it !in "\\\"" }); "\"${item.value}\"" }
            is Number -> { check(item.value in 0..0xffffffffL); item.value.toString() }
            is Flag -> item.value.toString()
        }
        return render(value).also { check(it.length <= 2048) }
    }
    fun envelope(id: Long, opcode: Int, body: Object): String {
        check(id in 1..0xffffffffL && opcode in 4..8)
        return encode(body("id" to number(id), "opcode" to number(opcode.toLong()), "body" to body))
    }
    fun success(response: Object): Object {
        response.exact(setOf("schema", "ok"), setOf("result", "error"))
        check(response.number("schema", 1, 1) == 1L)
        if (!response.flag("ok")) {
            check("result" !in response.fields)
            throw Failure(error(response.text("error")))
        }
        check("error" !in response.fields)
        return response.obj("result")
    }
    fun session(value: Object): Session {
        value.exact(setOf("session", "state", "deadline_remaining_seconds"), setOf("error", "node"))
        val state = value.text("state")
        check(state in setOf("waiting_ble", "pase", "provisioning", "waiting_thread", "case", "completing",
            "complete", "cancelled", "failed", "timed_out"))
        return Session(id(value.text("session")), state, value.number("deadline_remaining_seconds", 0, 900).toInt(),
            if ("error" in value.fields) error(value.text("error")) else null,
            if ("node" in value.fields) id(value.text("node")) else null)
    }
    fun accepted(value: Object): Long {
        value.exact(setOf("schema", "ok", "job", "state"))
        check(value.number("schema", 1, 1) == 1L && value.flag("ok") && value.text("state") == "queued")
        return value.number("job", 1)
    }
    fun availability(matter: Object, thread: Object): Availability {
        if (!matter.flag("enabled")) {
            matter.exact(setOf("enabled", "available"))
            check(!matter.flag("available"))
            return Availability(false, false, false)
        }
        matter.exact(setOf("enabled", "available", "controller_initialized", "fabric_ready", "state"))
        check(matter.text("state") in setOf("running", "awaiting_coordinator"))
        val network = thread(thread)
        return Availability(true, matter.flag("available") && matter.flag("controller_initialized") &&
            matter.flag("fabric_ready") && matter.text("state") == "running" && network.enabled &&
            network.available && network.configured && network.state in setOf("attaching", "attached", "detached"),
            network.configured)
    }
    fun thread(thread: Object): ThreadState {
        thread.exact(setOf("enabled", "available", "configured", "attached", "state", "linux_ipv6_or_tun"),
            setOf("zigbee_channel", "dataset_channel"))
        check(!thread.flag("linux_ipv6_or_tun") && thread.text("state") in setOf("awaiting_coordinator",
            "detached", "attaching", "attached", "channel_mismatch", "error"))
        for (key in listOf("zigbee_channel", "dataset_channel")) if (key in thread.fields) thread.number(key, 11, 26)
        val configured = thread.flag("configured")
        val attached = thread.flag("attached")
        check(!attached || configured)
        return ThreadState(thread.flag("enabled"), thread.flag("available"), configured, attached, thread.text("state"))
    }
    fun job(value: Object, expected: Long): Object? {
        value.exact(setOf("job", "state"), setOf("response", "error"))
        check(value.number("job", 1) == expected)
        return when (value.text("state")) {
            "queued", "running" -> { value.exact(setOf("job", "state")); null }
            "complete" -> { value.exact(setOf("job", "state", "response")); success(value.obj("response")) }
            "failed" -> {
                value.exact(setOf("job", "state", "error"))
                check(value.text("error") == "transport")
                throw Failure(Error.TRANSPORT)
            }
            else -> bad()
        }
    }
    fun httpError(value: Object): Error {
        value.exact(setOf("ok", "error"))
        check(!value.flag("ok"))
        return error(value.text("error"))
    }
    fun message(text: String): Message {
        val value = decode(text)
        check(value.number("schema", 1, 1) == 1L)
        if ("id" in value.fields) {
            value.exact(setOf("schema", "id", "response"))
            return Reply(value.number("id", 1), value.obj("response"))
        }
        value.exact(setOf("schema", "event", "data"))
        return when (value.text("event")) {
            "commissioning" -> Progress(session(value.obj("data")))
            "ble_write" -> {
                val data = value.obj("data").exact(setOf("session", "generation", "write", "data"))
                val hex = data.text("data")
                check(hex.length in 2..488 && hex.length % 2 == 0 && hex.all { it in "0123456789abcdef" })
                Write(id(data.text("session")), data.number("generation", 1), data.number("write", 1),
                    ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() })
            }
            else -> bad()
        }
    }
    fun hex(data: ByteArray): String {
        check(data.size in 1..244)
        val digits = "0123456789abcdef"
        return buildString(data.size * 2) { data.forEach { append(digits[(it.toInt() and 255) ushr 4]); append(digits[it.toInt() and 15]) } }
    }
}
