package io.github.microhad.matter

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.net.URI
import java.net.URLDecoder

/** Setup secrets are consumed once, never included in debug output or persisted. */
class MatterSetupCode private constructor(
    val discriminator: Int,
    val shortDiscriminator: Boolean,
    val commissioningFlow: Int,
    val vendorId: Int?,
    val productId: Int?,
    val bleAdvertised: Boolean?,
    private var pin: Int,
) : AutoCloseable {
    enum class Error { FORMAT, CHECKSUM, PIN, VERSION, CONSUMED }
    class Invalid(val code: Error) : IllegalArgumentException(code.name)

    fun matchesDiscriminator(advertised: Int): Boolean = advertised in 0..4095 &&
        (if (shortDiscriminator) advertised ushr 8 == discriminator else advertised == discriminator)

    fun consumePin(): Int {
        valid(pin != 0, Error.CONSUMED)
        val result = pin
        pin = 0
        return result
    }

    override fun close() { pin = 0 }
    override fun toString(): String = "MatterSetupCode(redacted)"

    companion object {
        private const val BASE38 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."
        private val forbiddenPins = setOf(11111111, 22222222, 33333333, 44444444, 55555555,
            66666666, 77777777, 88888888, 12345678, 87654321)
        private val permutation = intArrayOf(1, 5, 7, 6, 2, 8, 3, 0, 9, 4)

        private fun valid(condition: Boolean, code: Error = Error.FORMAT) {
            if (!condition) throw Invalid(code)
        }

        fun parse(input: String): MatterSetupCode {
            valid(input.length <= 512)
            val code = unwrap(input.trim())
            return if (code.startsWith("MT:")) qr(code.substring(3)) else manual(code)
        }

        private fun unwrap(input: String): String {
            if (!input.contains("://")) return input
            try {
                val uri = URI(input)
                valid(uri.scheme != null && uri.rawAuthority != null)
                val match = Regex("MT:[0-9A-Z.-]+").find(URLDecoder.decode(input, "UTF-8"))
                valid(match != null)
                return match!!.value
            } catch (e: Invalid) { throw e }
            catch (_: IllegalArgumentException) { throw Invalid(Error.FORMAT) }
            catch (_: java.net.URISyntaxException) { throw Invalid(Error.FORMAT) }
        }

        internal fun pinValid(pin: Int) = pin in 1..99999998 && pin !in forbiddenPins

        private fun multiply(left: Int, right: Int): Int = when {
            left < 5 && right < 5 -> (left + right) % 5
            left < 5 -> (left + right - 5) % 5 + 5
            right < 5 -> (left - 5 - right + 5) % 5 + 5
            else -> (left - right + 5) % 5
        }

        private fun checksum(code: String): Boolean {
            var value = 0
            code.reversed().forEachIndexed { index, digit ->
                var permuted = digit - '0'
                repeat(index % 8) { permuted = permutation[permuted] }
                value = multiply(value, permuted)
            }
            return value == 0
        }

        // Matter manual fields: 1/5/4 decimal chunks followed by optional VID/PID and Verhoeff.
        private fun manual(input: String): MatterSetupCode {
            val code = input.filterNot { it == '-' || it == ' ' }
            valid(code.length == 11 || code.length == 21)
            valid(code.all { it in '0'..'9' })
            valid(checksum(code), Error.CHECKSUM)
            val first = code[0] - '0'
            val second = code.substring(1, 6).toInt()
            val third = code.substring(6, 10).toInt()
            valid(first <= 7 && second <= 65535 && third <= 8191)
            val long = first and 4 != 0
            valid(long == (code.length == 21))
            val discriminator = ((first and 3) shl 2) or (second ushr 14)
            val pin = (third shl 14) or (second and 16383)
            valid(pinValid(pin), Error.PIN)
            val vendor = if (long) code.substring(10, 15).toInt() else null
            val product = if (long) code.substring(15, 20).toInt() else null
            valid(vendor == null || vendor <= 65535)
            valid(product == null || product <= 65535)
            return MatterSetupCode(discriminator, true, if (long) 2 else 0, vendor, product, null, pin)
        }

        private fun base38(code: String): ByteArray {
            valid(code.isNotEmpty() && code.length % 5 in setOf(0, 2, 4))
            val bytes = ArrayList<Byte>()
            var at = 0
            while (at < code.length) {
                val count = minOf(5, code.length - at)
                val width = when (count) { 5 -> 3; 4 -> 2; 2 -> 1; else -> throw Invalid(Error.FORMAT) }
                var value = 0L
                var scale = 1L
                repeat(count) { i ->
                    val digit = BASE38.indexOf(code[at + i])
                    valid(digit >= 0)
                    value += digit * scale
                    scale *= 38
                }
                valid(value < (1L shl (width * 8)))
                repeat(width) { bytes.add((value ushr (it * 8)).toByte()) }
                at += count
            }
            return bytes.toByteArray()
        }

        private fun qr(code: String): MatterSetupCode {
            val bytes = base38(code)
            try {
                valid(bytes.size >= 11)
                fun bits(at: Int, count: Int): Int {
                    var result = 0
                    repeat(count) { i ->
                        result = result or (((bytes[(at + i) / 8].toInt() ushr ((at + i) % 8)) and 1) shl i)
                    }
                    return result
                }
                valid(bits(0, 3) == 0, Error.VERSION)
                val flow = bits(35, 2)
                valid(flow <= 2 && bits(84, 4) == 0)
                val pin = bits(57, 27)
                valid(pinValid(pin), Error.PIN)
                optionalFields(bytes)
                return MatterSetupCode(bits(45, 12), false, flow, bits(3, 16), bits(19, 16),
                    bits(37, 8) and 2 != 0, pin)
            } finally {
                bytes.fill(0)
            }
        }

        // Optional QR data is one anonymous TLV structure with context-tagged integers/UTF-8 strings.
        private fun optionalFields(bytes: ByteArray) {
            if (bytes.size == 11) return
            var at = 11
            valid(bytes[at++].toInt() and 255 == 0x15)
            val tags = HashSet<Int>()
            while (at < bytes.size && bytes[at].toInt() and 255 != 0x18) {
                valid(tags.size < 32 && at + 2 <= bytes.size)
                val control = bytes[at++].toInt() and 255
                valid(control ushr 5 == 1)
                valid(tags.add(bytes[at++].toInt() and 255))
                val type = control and 31
                if (type in 0..7) {
                    val width = 1 shl (type and 3)
                    valid(at + width <= bytes.size)
                    at += width
                } else {
                    valid(type in 12..15)
                    val width = 1 shl (type - 12)
                    valid(at + width <= bytes.size)
                    var length = 0L
                    repeat(width) { i ->
                        val next = bytes[at++].toInt() and 255
                        valid(i < 4 || next == 0)
                        if (i < 4) length = length or (next.toLong() shl (i * 8))
                    }
                    valid(length <= bytes.size - at)
                    try {
                        Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .decode(ByteBuffer.wrap(bytes, at, length.toInt()))
                    } catch (_: java.nio.charset.CharacterCodingException) { throw Invalid(Error.FORMAT) }
                    at += length.toInt()
                }
            }
            valid(at == bytes.size - 1 && bytes[at].toInt() and 255 == 0x18)
        }
    }
}
