package org.microhad.companion.matter

import kotlinx.coroutines.CoroutineScope

internal class MatterOnboardingGateway(base: String, fingerprint: String = "") : MatterOnboardingController.Gateway {
    private val client = MatterGatewayClient(base, fingerprint)
    override suspend fun availability() = client.availability()
    override suspend fun createThread() = client.createThread()
    override suspend fun start(discriminator: Int, pin: Int, timeoutSeconds: Int) = client.start(discriminator, pin, timeoutSeconds)
    override suspend fun cancel(session: String) { client.cancel(session) }
    override fun relay(session: String, scope: CoroutineScope, listener: MatterGatewayRelay.Listener): MatterOnboardingController.Relay {
        val connection = MatterGatewayRelay(client, session, scope, listener)
        return object : MatterOnboardingController.Relay {
            override suspend fun open() = connection.open()
            override suspend fun attach(address: String, mtu: Int) { connection.attach(address, mtu) }
            override fun indicate(generation: Long, sequence: Long, data: ByteArray) = connection.indicate(generation, sequence, data)
            override fun completeWrite(generation: Long, write: Long, success: Boolean) = connection.completeWrite(generation, write, success)
            override fun close() = connection.close()
        }
    }
    override fun close() = client.close()
}
