package org.microhad.companion.matter

import org.junit.Assert.*
import org.junit.Test

class MatterAdvertisementTest {
    @Test fun standardAdvertisementSelectsAFullDiscriminatorForManualInput() {
        val data = byteArrayOf(0, 0x12, 0x0f, 0xf1.toByte(), 0xff.toByte(), 0, 0x80.toByte(), 1)
        val advertisement = MatterAdvertisement.parse(data)!!
        assertEquals(3858, advertisement.discriminator)
        assertEquals(65521, advertisement.vendorId)
        assertEquals(32768, advertisement.productId)
        val code = MatterSetupCode.parse("34970112332")
        assertTrue(advertisement.matches(code))
        assertFalse(advertisement.copy(discriminator = 3839).matches(code))
        code.close()
    }

    @Test fun malformedUnsupportedAndNonCommissionableServiceDataAreIgnored() {
        assertNull(MatterAdvertisement.parse(byteArrayOf()))
        assertNull(MatterAdvertisement.parse(ByteArray(9)))
        assertNull(MatterAdvertisement.parse(ByteArray(8).also { it[0] = 1 }))
        assertNull(MatterAdvertisement.parse(ByteArray(8).also { it[2] = 0x10 }))
        assertNull(MatterAdvertisement.parse(ByteArray(8).also { it[7] = 4 }))
        assertNotNull(MatterAdvertisement.parse(ByteArray(8).also { it[7] = 3 }))
    }
}
