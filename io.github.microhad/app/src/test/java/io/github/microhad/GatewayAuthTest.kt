package io.github.microhad

import org.junit.Assert.assertEquals
import org.junit.Test

class GatewayAuthTest {
    @Test fun utf8PasswordDerivationMatchesBrowserAndHostProtocol() {
        val derived = GatewayAuth.derive("pássword", ByteArray(16) { it.toByte() })
        assertEquals("8a5e58984d55d73b0db57cee4429943c1d274ec1fd9fc4281abcde31a2426380",
            derived.joinToString("") { "%02x".format(it.toInt() and 255) })
    }
}
