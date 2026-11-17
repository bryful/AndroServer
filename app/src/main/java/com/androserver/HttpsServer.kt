package com.androserver

import java.io.Closeable
import java.io.InputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.security.KeyStore
import java.security.PrivateKey
import java.security.UnrecoverableKeyException
import java.security.cert.CertificateExpiredException
import java.security.cert.CertificateNotYetValidException
import java.security.cert.X509Certificate
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket

internal class HttpsServer private constructor(
    private val listener: SSLServerSocket,
    private val backendPort: Int
) : Closeable {
    class IdentityException(val reason: Reason, cause: Throwable? = null) : Exception(cause)

    enum class Reason { WRONG_PASSWORD, INVALID_FILE, MISSING_KEY, EXPIRED, NOT_YET_VALID }

    private val workers = Executors.newCachedThreadPool()
    private val clients = mutableSetOf<RelayConnection>()
    private var closed = false
    private lateinit var acceptTask: Future<*>
    val isListening: Boolean get() = !listener.isClosed && !acceptTask.isDone

    private class RelayConnection(val client: SSLSocket) {
        val backend = Socket()
        @Volatile var lastActivity = System.nanoTime()
            private set

        fun relay(input: InputStream, output: java.io.OutputStream) {
            val buffer = ByteArray(BUFFER_SIZE)
            while (true) {
                val size = input.read(buffer)
                if (size < 0) return
                lastActivity = System.nanoTime()
                output.write(buffer, 0, size)
                lastActivity = System.nanoTime()
            }
        }

        fun close() {
            closeSocket(backend)
            closeSocket(client)
        }
    }

    private fun acceptConnections() {
        while (!listener.isClosed) {
            val client = try {
                listener.accept() as SSLSocket
            } catch (_: IOException) {
                return
            }
            synchronized(clients) {
                if (closed || clients.size >= 16) {
                    closeSocket(client)
                } else {
                    val connection = RelayConnection(client)
                    clients.add(connection)
                    workers.execute { relay(connection) }
                }
            }
        }
    }

    private fun relay(connection: RelayConnection) {
        val client = connection.client
        val backend = connection.backend
        try {
            client.soTimeout = 15_000
            client.startHandshake()
            client.soTimeout = 0
            backend.connect(InetSocketAddress("127.0.0.1", backendPort), 5_000)
            workers.execute {
                try {
                    connection.relay(client.getInputStream(), backend.getOutputStream())
                    backend.shutdownOutput()
                } catch (_: IOException) {
                    closeSocket(backend)
                }
            }
            connection.relay(backend.getInputStream(), client.getOutputStream())
            client.getOutputStream().flush()
        } catch (_: IOException) {
            // An aborted download or TLS handshake affects only this connection.
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            // The server was stopped while establishing the connection.
        } finally {
            connection.close()
            synchronized(clients) { clients.remove(connection) }
        }
    }

    fun closeIdleConnections() {
        val now = System.nanoTime()
        val idle = synchronized(clients) {
            clients.filter { now - it.lastActivity >= IDLE_TIMEOUT_NANOS }
        }
        idle.forEach { it.close() }
    }

    override fun close() {
        val pending = synchronized(clients) {
            closed = true
            workers.shutdownNow()
            clients.toList()
        }
        runCatching { listener.close() }
        pending.forEach { it.close() }
    }

    companion object {
        private const val BUFFER_SIZE = 128 * 1024
        private val IDLE_TIMEOUT_NANOS = TimeUnit.MINUTES.toNanos(5)

        fun loadIdentity(input: InputStream, password: CharArray): SSLContext {
            val keys = KeyStore.getInstance("PKCS12")
            try {
                keys.load(input, password)
            } catch (e: IOException) {
                val wrongPassword = generateSequence<Throwable>(e) { it.cause }.any {
                    it is UnrecoverableKeyException ||
                        it.message.orEmpty().contains("wrong password", ignoreCase = true) ||
                        it.message.orEmpty().contains("password was incorrect", ignoreCase = true) ||
                        it.message.orEmpty().contains("mac invalid", ignoreCase = true)
                }
                throw IdentityException(if (wrongPassword) Reason.WRONG_PASSWORD else Reason.INVALID_FILE, e)
            }

            val keysWithCertificate = keys.aliases().toList().filter(keys::isKeyEntry)
            val alias = keysWithCertificate.singleOrNull() ?: throw IdentityException(Reason.MISSING_KEY)
            val certificate = keys.getCertificate(alias) as? X509Certificate
                ?: throw IdentityException(Reason.MISSING_KEY)
            try {
                certificate.checkValidity()
            } catch (e: CertificateExpiredException) {
                throw IdentityException(Reason.EXPIRED, e)
            } catch (e: CertificateNotYetValidException) {
                throw IdentityException(Reason.NOT_YET_VALID, e)
            }

            val keyManagers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
            try {
                if (keys.getKey(alias, password) !is PrivateKey) throw IdentityException(Reason.MISSING_KEY)
                keyManagers.init(keys, password)
            } catch (e: UnrecoverableKeyException) {
                throw IdentityException(Reason.WRONG_PASSWORD, e)
            }
            return SSLContext.getInstance("TLS").apply { init(keyManagers.keyManagers, null, null) }
        }

        fun start(context: SSLContext, host: String, port: Int, backendPort: Int): HttpsServer {
            val socket = context.serverSocketFactory.createServerSocket() as SSLServerSocket
            try {
                socket.enabledProtocols = socket.supportedProtocols.filter {
                    it == "TLSv1.2" || it == "TLSv1.3"
                }.toTypedArray()
                socket.reuseAddress = true
                socket.bind(InetSocketAddress(host, port))
                return HttpsServer(socket, backendPort).apply {
                    acceptTask = workers.submit { acceptConnections() }
                }
            } catch (e: Exception) {
                runCatching { socket.close() }
                throw e
            }
        }

        private fun closeSocket(socket: Socket) {
            runCatching { socket.close() }
        }
    }
}
