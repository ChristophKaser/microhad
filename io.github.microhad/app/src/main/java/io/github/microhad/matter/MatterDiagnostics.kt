package io.github.microhad.matter

import android.util.Log

/** Transport metadata only: never log setup codes, PINs, datasets or packet contents. */
internal object MatterDiagnostics {
    fun record(message: String) {
        // Local JVM tests do not provide Android's logging implementation.
        runCatching { Log.i("MicroHADMatter", message) }
    }
}
