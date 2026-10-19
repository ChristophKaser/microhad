package io.github.microhad.matter

data class MatterAdvertisement(val discriminator: Int, val vendorId: Int, val productId: Int) {
    fun matches(code: MatterSetupCode): Boolean = code.matchesDiscriminator(discriminator) &&
        (code.vendorId == null || code.vendorId == 0 || code.vendorId == vendorId) &&
        (code.productId == null || code.productId == 0 || code.productId == productId)

    companion object {
        /** Version-zero commissionable FFF6 service data, without the AD type/UUID prefix. */
        fun parse(data: ByteArray): MatterAdvertisement? {
            if (data.size != 8 || data[0] != 0.toByte() || (data[2].toInt() and 0xf0) != 0 ||
                (data[7].toInt() and 0xfc) != 0) return null
            fun little(at: Int) = (data[at].toInt() and 255) or ((data[at + 1].toInt() and 255) shl 8)
            return MatterAdvertisement(little(1), little(3), little(5))
        }
    }
}
