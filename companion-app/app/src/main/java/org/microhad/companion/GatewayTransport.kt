package org.microhad.companion

import okhttp3.OkHttpClient
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.*

object GatewayTransport {
    fun normalize(input: String): String {
        val value = input.trim().let { if ("://" in it) it else "http://$it" }
        val uri = URI(value)
        require(uri.scheme in listOf("http", "https") && !uri.host.isNullOrEmpty() &&
            uri.rawUserInfo == null && uri.rawQuery == null && uri.rawFragment == null &&
            uri.rawPath in listOf("", "/") && uri.port in -1..65535 && uri.port != 0) {
            "Enter a gateway hostname or http(s)://host[:port]"
        }
        return value.removeSuffix("/")
    }

    fun fingerprint(certificate: X509Certificate): String = MessageDigest.getInstance("SHA-256")
        .digest(certificate.encoded).joinToString("") { "%02x".format(it.toInt() and 255) }

    fun accepts(pin: String, certificate: X509Certificate): Boolean =
        pin.matches(Regex("[0-9a-f]{64}")) && MessageDigest.isEqual(pin.toByteArray(), fingerprint(certificate).toByteArray())

    fun client(pin: String = ""): OkHttpClient {
        val builder = OkHttpClient.Builder().connectTimeout(5, TimeUnit.SECONDS)
            .readTimeout(10, TimeUnit.SECONDS).callTimeout(15, TimeUnit.SECONDS)
            .followRedirects(false).followSslRedirects(false).retryOnConnectionFailure(false)
        if (pin.isNotEmpty()) {
            require(pin.matches(Regex("[0-9a-f]{64}")))
            val trust = object : X509TrustManager {
                override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
                override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = throw CertificateException()
                override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
                    if (chain.isEmpty() || !accepts(pin, chain[0])) throw CertificateException("Gateway certificate changed")
                }
            }
            val context = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trust), null) }
            builder.sslSocketFactory(context.socketFactory, trust)
                .hostnameVerifier { _, session -> accepts(pin, session.peerCertificates[0] as X509Certificate) }
        }
        return builder.build()
    }

    /** Handshake only; sends no HTTP request or application data to an unverified peer. */
    fun inspectCertificate(base: String): String {
        val uri = URI(normalize(base))
        require(uri.scheme == "https") { "Certificate exceptions apply only to HTTPS" }
        val trust = object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = throw CertificateException()
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) { require(chain.isNotEmpty()) }
        }
        val context = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trust), null) }
        Socket().use { tcp ->
            tcp.connect(InetSocketAddress(uri.host, if (uri.port == -1) 443 else uri.port), 5000)
            tcp.soTimeout = 5000
            (context.socketFactory.createSocket(tcp, uri.host, tcp.port, true) as SSLSocket).use { tls ->
                tls.soTimeout = 5000
                tls.startHandshake()
                return fingerprint(tls.session.peerCertificates[0] as X509Certificate)
            }
        }
    }
}
