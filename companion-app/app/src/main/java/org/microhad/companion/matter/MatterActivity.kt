package org.microhad.companion.matter

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View
import android.view.WindowManager
import android.widget.Button
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import android.content.pm.PackageManager
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import org.microhad.companion.GatewayStore
import org.microhad.companion.R
import org.microhad.companion.databinding.ActivityMatterBinding
import kotlinx.coroutines.*

class MatterActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMatterBinding
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val handler = Handler(Looper.getMainLooper())
    private var controller: MatterOnboardingController? = null
    private var gatewayAddress: String? = null
    private var foreground = false
    private var lifecycleEpoch = 0L
    private var permissionEpoch: Long? = null
    private var qrTicket = 0L
    private val permissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        val owner = permissionEpoch
        permissionEpoch = null
        if (foreground && owner == lifecycleEpoch) {
            if (missingPermissions().isEmpty()) scan() else binding.statusText.setText(R.string.matter_permission)
        }
    }
    private val tick = object : Runnable {
        override fun run() {
            controller?.tick()
            if (foreground) handler.postDelayed(this, 250)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        binding = ActivityMatterBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.gatewayInput.setText(GatewayStore(this).base)
        binding.gatewayInput.visibility = View.GONE
        binding.checkButton.setOnClickListener { checkGateway() }
        binding.retryButton.setOnClickListener { binding.setupInput.text?.clear(); controller?.refresh() }
        binding.scanButton.setOnClickListener { scan() }
        binding.cancelButton.setOnClickListener { binding.setupInput.text?.clear(); controller?.cancel() }
        binding.createThreadButton.setOnClickListener { controller?.createThread() }
        binding.qrButton.setOnClickListener { scanQr() }
    }
    override fun onStart() {
        super.onStart()
        foreground = true
        if (controller == null) checkGateway() else controller?.resume()
        handler.postDelayed(tick, 250)
    }
    override fun onStop() {
        foreground = false
        lifecycleEpoch++
        binding.setupInput.text?.clear()
        binding.candidates.removeAllViews()
        handler.removeCallbacks(tick)
        controller?.stop()
        super.onStop()
    }
    override fun onDestroy() {
        qrTicket++
        controller?.close(); controller = null
        scope.cancel()
        super.onDestroy()
    }
    private fun checkGateway() {
        if (!foreground) return
        val address = binding.gatewayInput.text.toString().trim()
        if (controller != null && address == gatewayAddress) { controller?.refresh(); return }
        val gateway = try { MatterOnboardingGateway(address, GatewayStore(this).fingerprint) } catch (_: Exception) {
            binding.statusText.setText(R.string.matter_gateway_invalid); return
        }
        controller?.close()
        gatewayAddress = address
        controller = MatterOnboardingController(gateway, { listener ->
            val adapter = MatterBleAdapter(applicationContext, listener)
            object : MatterOnboardingController.Ble {
                override fun scan(matches: (MatterAdvertisement) -> Boolean) = adapter.startScan(matches)
                override fun stopScan() = adapter.stopScan()
                override fun connect(address: String) = adapter.connect(address)
                override fun write(write: Long, data: ByteArray) = adapter.write(write, data)
                override fun close() = adapter.close()
            }
        }, scope, SystemClock::elapsedRealtime, ::render)
        controller?.resume()
    }
    private fun missingPermissions() = MatterBleAdapter.requiredPermissions().filter {
        ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
    }.toTypedArray()
    private fun scan() {
        if (!foreground || controller?.snapshot?.state != MatterOnboardingController.State.READY) return
        val missing = missingPermissions()
        if (missing.isNotEmpty()) {
            if (permissionEpoch == null) { permissionEpoch = lifecycleEpoch; permissions.launch(missing) }
            return
        }
        val code = binding.setupInput.text.toString()
        binding.setupInput.text?.clear()
        controller?.begin(code)
    }
    private fun scanQr() {
        if (!foreground || controller?.snapshot?.state != MatterOnboardingController.State.READY) return
        binding.setupInput.text?.clear()
        val ticket = ++qrTicket
        val options = GmsBarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).enableAutoZoom().build()
        GmsBarcodeScanning.getClient(this, options).startScan()
            .addOnSuccessListener { barcode ->
                val code = barcode.rawValue
                val valid = code != null && try {
                    MatterSetupCode.parse(code).use { !it.shortDiscriminator }
                } catch (_: MatterSetupCode.Invalid) { false }
                if (!valid) {
                    if (!isDestroyed && ticket == qrTicket) binding.statusText.setText(R.string.matter_invalid_code)
                } else scope.launch {
                    // Scanner UI can complete just before this activity resumes. Never persist the result.
                    try {
                        withTimeout(30000) {
                            while (!foreground) delay(100)
                            if (!isFinishing && ticket == qrTicket) binding.setupInput.setText(code)
                        }
                    } catch (_: CancellationException) { }
                }
            }
            .addOnFailureListener {
                if (!isDestroyed && ticket == qrTicket) binding.statusText.setText(R.string.matter_qr_failed)
            }
    }
    private fun render(value: MatterOnboardingController.Snapshot) {
        val state = value.state
        val active = state in setOf(MatterOnboardingController.State.SCANNING, MatterOnboardingController.State.SELECTING,
            MatterOnboardingController.State.STARTING, MatterOnboardingController.State.CONNECTING,
            MatterOnboardingController.State.BINDING, MatterOnboardingController.State.ACTIVE)
        val ready = state == MatterOnboardingController.State.READY && !value.cleaning
        val message = StringBuilder(getString(stateText(state)))
        value.phase?.let { phaseText(it)?.let { resource -> message.append('\n').append(getString(resource)) } }
        if (value.remainingSeconds > 0) message.append('\n').append(getString(R.string.matter_remaining, value.remainingSeconds))
        value.sessionId?.let { message.append('\n').append(getString(R.string.matter_session, it)) }
        value.error?.let { message.append('\n').append(getString(errorText(it))) }
        if (value.cleaning) message.append('\n').append(getString(R.string.matter_cleanup))
        binding.statusText.text = message
        binding.gatewayInput.isEnabled = !active && !value.cleaning && state != MatterOnboardingController.State.CHECKING
        binding.checkButton.isEnabled = binding.gatewayInput.isEnabled
        binding.setupGroup.visibility = if (ready) View.VISIBLE else View.GONE
        binding.setupInput.isEnabled = ready
        binding.scanButton.isEnabled = ready
        binding.qrButton.isEnabled = ready
        binding.createThreadButton.visibility = if (state == MatterOnboardingController.State.NEEDS_THREAD) View.VISIBLE else View.GONE
        binding.createThreadButton.isEnabled = !value.cleaning
        binding.cancelButton.visibility = if (active) View.VISIBLE else View.GONE
        binding.progress.visibility = if ((active && state != MatterOnboardingController.State.SELECTING) ||
            state == MatterOnboardingController.State.CHECKING || value.cleaning) View.VISIBLE else View.GONE
        binding.retryButton.visibility = if (state in setOf(MatterOnboardingController.State.UNAVAILABLE,
            MatterOnboardingController.State.DISABLED, MatterOnboardingController.State.FAILED,
            MatterOnboardingController.State.CANCELLED, MatterOnboardingController.State.TIMED_OUT,
            MatterOnboardingController.State.COMPLETE)) View.VISIBLE else View.GONE
        binding.retryButton.isEnabled = !value.cleaning
        binding.candidates.removeAllViews()
        value.candidates.forEach { candidate ->
            binding.candidates.addView(Button(this).apply {
                text = getString(R.string.matter_candidate, candidate.address, candidate.rssi)
                setOnClickListener { controller?.select(candidate.address) }
            })
        }
    }
    private fun stateText(state: MatterOnboardingController.State): Int = when (state) {
        MatterOnboardingController.State.STOPPED -> R.string.matter_state_stopped
        MatterOnboardingController.State.CHECKING -> R.string.matter_state_checking
        MatterOnboardingController.State.DISABLED -> R.string.matter_state_disabled
        MatterOnboardingController.State.UNAVAILABLE -> R.string.matter_state_unavailable
        MatterOnboardingController.State.NEEDS_THREAD -> R.string.matter_state_needs_thread
        MatterOnboardingController.State.READY -> R.string.matter_state_ready
        MatterOnboardingController.State.SCANNING -> R.string.matter_state_scanning
        MatterOnboardingController.State.SELECTING -> R.string.matter_state_selecting
        MatterOnboardingController.State.STARTING -> R.string.matter_state_starting
        MatterOnboardingController.State.CONNECTING -> R.string.matter_state_connecting
        MatterOnboardingController.State.BINDING -> R.string.matter_state_binding
        MatterOnboardingController.State.ACTIVE -> R.string.matter_state_active
        MatterOnboardingController.State.COMPLETE -> R.string.matter_state_complete
        MatterOnboardingController.State.CANCELLED -> R.string.matter_state_cancelled
        MatterOnboardingController.State.FAILED -> R.string.matter_state_failed
        MatterOnboardingController.State.TIMED_OUT -> R.string.matter_state_timed_out
    }
    private fun phaseText(phase: String): Int? = when (phase) {
        "waiting_ble" -> R.string.matter_phase_waiting_ble
        "pase" -> R.string.matter_phase_pase
        "provisioning" -> R.string.matter_phase_provisioning
        "waiting_thread" -> R.string.matter_phase_waiting_thread
        "case" -> R.string.matter_phase_case
        "completing" -> R.string.matter_phase_completing
        else -> null
    }
    private fun errorText(error: String): Int = when {
        error == "ble_permission" -> R.string.matter_permission
        error == "ble_bluetooth_off" -> R.string.matter_bluetooth_off
        error == "ble_location_off" -> R.string.matter_location_off
        error == "setup_custom_flow" -> R.string.matter_custom_flow
        error == "setup_non_ble" -> R.string.matter_non_ble
        error.startsWith("setup_") -> R.string.matter_invalid_code
        error == "busy" -> R.string.matter_busy
        else -> R.string.matter_error_generic
    }
}
