package org.microhad.companion.matter

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import androidx.core.content.ContextCompat
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/** Foreground-only transport. All public calls and listener callbacks use the main looper. */
@SuppressLint("MissingPermission")
@Suppress("DEPRECATION")
class MatterBleAdapter(context: Context, private val listener: Listener) : AutoCloseable {
    interface Listener {
        fun candidate(address: String, advertisement: MatterAdvertisement, rssi: Int)
        fun ready(mtu: Int)
        fun indication(data: ByteArray)
        fun written(write: Long, success: Boolean)
        fun failed(error: Error)
    }
    enum class Error { PERMISSION, BLUETOOTH_OFF, LOCATION_OFF, SCAN, SCAN_TIMEOUT, CONNECT, GATT, OVERFLOW }
    private val app = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val bluetooth = (app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private val queued = AtomicInteger()
    @Volatile private var token = 0L
    @Volatile private var overflowToken = -1L
    private var scan: ScanCallback? = null
    private var scanner: BluetoothLeScanner? = null
    private val candidates = LinkedHashSet<String>()
    private var gatt: BluetoothGatt? = null
    private var c1: BluetoothGattCharacteristic? = null
    private var c2: BluetoothGattCharacteristic? = null
    private var cccd: BluetoothGattDescriptor? = null
    private var session: MatterGattSession? = null
    private val scanDeadline = Runnable { stopScan(); listener.failed(Error.SCAN_TIMEOUT) }
    private val tick = object : Runnable {
        override fun run() {
            session?.tick()
            if (session != null && session?.stage != MatterGattSession.Stage.CLOSED) handler.postDelayed(this, 250)
        }
    }

    fun missingPermissions(): Array<String> = requiredPermissions().filter {
        ContextCompat.checkSelfPermission(app, it) != PackageManager.PERMISSION_GRANTED
    }.toTypedArray()

    fun startScan(matches: (MatterAdvertisement) -> Boolean) {
        checkMain()
        close()
        if (!available()) return
        val owner = token
        MatterDiagnostics.record("BLE scan started token=$owner")
        candidates.clear()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) = accept(result)
            override fun onBatchScanResults(results: MutableList<ScanResult>) { results.take(32).forEach { accept(it) } }
            private fun accept(result: ScanResult) {
                val data = result.scanRecord?.getServiceData(ParcelUuid(SERVICE)) ?: return
                val advertisement = MatterAdvertisement.parse(data) ?: return
                if (!matches(advertisement)) return
                post(owner) {
                    if (scan !== this || candidates.size >= 32) return@post
                    val address = result.device.address
                    if (candidates.add(address)) {
                        MatterDiagnostics.record("BLE candidate address=$address rssi=${result.rssi}")
                        listener.candidate(address, advertisement, result.rssi)
                    }
                }
            }
            override fun onScanFailed(errorCode: Int) { post(owner) { stopScan(); listener.failed(Error.SCAN) } }
        }
        try {
            val activeScanner = bluetooth?.bluetoothLeScanner ?: run { listener.failed(Error.BLUETOOTH_OFF); return }
            scanner = activeScanner
            scan = callback
            // Match service data rather than relying on an optional advertised service-UUID list.
            val filter = ScanFilter.Builder().setServiceData(ParcelUuid(SERVICE), byteArrayOf(0), byteArrayOf(-1)).build()
            activeScanner.startScan(listOf(filter), ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setReportDelay(0).build(), callback)
            handler.postDelayed(scanDeadline, 20000)
        } catch (_: SecurityException) { stopScan(); listener.failed(Error.PERMISSION) }
        catch (_: RuntimeException) { stopScan(); listener.failed(Error.SCAN) }
    }

    fun stopScan() {
        checkMain()
        handler.removeCallbacks(scanDeadline)
        val callback = scan
        val activeScanner = scanner
        scan = null
        scanner = null
        if (callback != null) try { activeScanner?.stopScan(callback) } catch (_: RuntimeException) { }
    }

    fun connect(address: String) {
        checkMain()
        if (address !in candidates) { listener.failed(Error.CONNECT); return }
        stopScan()
        closeGatt()
        if (!available()) return
        token++
        val owner = token
        MatterDiagnostics.record("GATT connecting address=$address token=$owner")
        val callback = object : BluetoothGattCallback() {
            private fun event(connection: BluetoothGatt, action: () -> Unit) = post(owner) {
                if (connection === gatt) action()
            }
            override fun onConnectionStateChange(connection: BluetoothGatt, status: Int, newState: Int) =
                event(connection) {
                    MatterDiagnostics.record("GATT connection status=$status state=$newState")
                    if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                        session?.fail()
                    } else if (newState == BluetoothProfile.STATE_CONNECTED) session?.connected()
                }
            override fun onServicesDiscovered(connection: BluetoothGatt, status: Int) = event(connection) {
                val service = connection.getService(SERVICE)
                c1 = service?.getCharacteristic(C1)
                c2 = service?.getCharacteristic(C2)
                cccd = c2?.getDescriptor(CCCD)
                val valid = status == BluetoothGatt.GATT_SUCCESS && c1 != null && c2 != null && cccd != null &&
                    (c1!!.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0 &&
                    (c2!!.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0
                MatterDiagnostics.record("GATT services status=$status valid=$valid C1=${c1?.properties} C2=${c2?.properties} CCCD=${cccd != null}")
                session?.discovered(valid)
            }
            override fun onMtuChanged(connection: BluetoothGatt, mtu: Int, status: Int) = event(connection) {
                MatterDiagnostics.record("GATT MTU status=$status mtu=$mtu")
                session?.negotiated(mtu, status == BluetoothGatt.GATT_SUCCESS)
            }
            override fun onDescriptorWrite(connection: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) =
                event(connection) {
                    MatterDiagnostics.record("GATT CCCD callback status=$status matched=${descriptor === cccd}")
                    if (descriptor === cccd) session?.subscribed(status == BluetoothGatt.GATT_SUCCESS)
                }
            override fun onCharacteristicWrite(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic,
                                               status: Int) = event(connection) {
                MatterDiagnostics.record("GATT C1 callback status=$status matched=${characteristic === c1}")
                if (characteristic === c1) session?.written(status == BluetoothGatt.GATT_SUCCESS)
            }
            override fun onCharacteristicChanged(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
                changed(connection, characteristic, characteristic.value ?: byteArrayOf())
            }
            override fun onCharacteristicChanged(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic,
                                                 value: ByteArray) { changed(connection, characteristic, value) }
            private fun changed(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
                // Check before copying/queueing to bound retained callback payloads.
                if (value.size > 244) { event(connection) { session?.fail() }; return }
                MatterDiagnostics.record("GATT indication bytes=${value.size} characteristic=${characteristic.uuid}")
                val copy = value.copyOf()
                event(connection) { if (characteristic === c2) session?.indication(copy) }
            }
        }
        session = MatterGattSession(object : MatterGattSession.Ports {
            override fun discover(): Boolean = gatt?.discoverServices() == true
            override fun requestMtu(): Boolean = gatt?.requestMtu(247) == true
            override fun subscribe(): Boolean {
                val connection = gatt ?: return false
                val characteristic = c2 ?: return false
                val descriptor = cccd ?: return false
                if (!connection.setCharacteristicNotification(characteristic, true)) return false
                val accepted = if (Build.VERSION.SDK_INT >= 33) {
                    connection.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_INDICATION_VALUE) ==
                        BluetoothStatusCodes.SUCCESS
                } else {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                    connection.writeDescriptor(descriptor)
                }
                MatterDiagnostics.record("GATT CCCD submitted accepted=$accepted")
                return accepted
            }
            override fun write(data: ByteArray): Boolean {
                val connection = gatt ?: return false
                val characteristic = c1 ?: return false
                val accepted = if (Build.VERSION.SDK_INT >= 33) {
                    connection.writeCharacteristic(characteristic, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                        BluetoothStatusCodes.SUCCESS
                } else {
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    characteristic.value = data
                    connection.writeCharacteristic(characteristic)
                }
                MatterDiagnostics.record("GATT C1 submitted bytes=${data.size} accepted=$accepted")
                return accepted
            }
            override fun close() { closeGatt() }
        }, object : MatterGattSession.Listener {
            override fun ready(mtu: Int) { MatterDiagnostics.record("GATT ready mtu=$mtu"); listener.ready(mtu) }
            override fun indication(data: ByteArray) { listener.indication(data) }
            override fun written(write: Long, success: Boolean) { MatterDiagnostics.record("GATT write completed write=$write success=$success"); listener.written(write, success) }
            override fun failed() { MatterDiagnostics.record("GATT failed stage=${session?.stage}"); listener.failed(Error.GATT) }
        }, SystemClock::elapsedRealtime)
        try {
            gatt = bluetooth?.getRemoteDevice(address)?.connectGatt(app, false, callback, BluetoothDevice.TRANSPORT_LE)
            if (gatt == null) session?.fail() else handler.postDelayed(tick, 250)
        } catch (_: SecurityException) { close(); listener.failed(Error.PERMISSION) }
        catch (_: RuntimeException) { close(); listener.failed(Error.CONNECT) }
    }

    fun write(write: Long, data: ByteArray) {
        checkMain()
        val active = session ?: throw IllegalStateException("GATT_UNAVAILABLE")
        check(active.stage == MatterGattSession.Stage.READY) { "GATT_UNAVAILABLE" }
        MatterDiagnostics.record("GATT write requested write=$write bytes=${data.size}")
        active.write(write, data)
    }

    override fun close() {
        checkMain()
        token++
        stopScan()
        session?.close()
        session = null
        closeGatt()
        candidates.clear()
    }
    private fun closeGatt() {
        handler.removeCallbacks(tick)
        val connection = gatt
        gatt = null
        c1 = null
        c2 = null
        cccd = null
        if (connection != null) {
            try { connection.disconnect() } catch (_: RuntimeException) { }
            try { connection.close() } catch (_: RuntimeException) { }
        }
    }
    private fun available(): Boolean {
        if (missingPermissions().isNotEmpty()) { listener.failed(Error.PERMISSION); return false }
        try {
            if (bluetooth?.isEnabled != true) { listener.failed(Error.BLUETOOTH_OFF); return false }
            if (Build.VERSION.SDK_INT < 31 &&
                !(app.getSystemService(Context.LOCATION_SERVICE) as LocationManager).isLocationEnabled) {
                listener.failed(Error.LOCATION_OFF); return false
            }
        } catch (_: SecurityException) { listener.failed(Error.PERMISSION); return false }
        return true
    }
    private fun post(owner: Long, action: () -> Unit) {
        if (owner != token) return
        if (queued.incrementAndGet() > 8) {
            queued.decrementAndGet()
            synchronized(queued) {
                if (overflowToken == owner) return
                overflowToken = owner
            }
            handler.post { if (owner == token) { close(); listener.failed(Error.OVERFLOW) } }
            return
        }
        handler.post {
            queued.decrementAndGet()
            if (owner == token && overflowToken != owner) {
                try { action() } catch (_: SecurityException) { close(); listener.failed(Error.PERMISSION) }
                catch (_: RuntimeException) { close(); listener.failed(Error.GATT) }
            }
        }
    }
    private fun checkMain() { check(Looper.myLooper() == handler.looper) }

    companion object {
        private val SERVICE = UUID.fromString("0000fff6-0000-1000-8000-00805f9b34fb")
        private val C1 = UUID.fromString("18ee2ef5-263d-4559-959f-4f9c429f9d11")
        private val C2 = UUID.fromString("18ee2ef5-263d-4559-959f-4f9c429f9d12")
        private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        fun requiredPermissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }
}
