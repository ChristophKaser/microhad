package io.github.microhad

import android.app.Activity
import android.os.Bundle
import android.webkit.WebView

/** Offline bundled legal notices; no gateway or JavaScript is needed. */
class LicensesActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = "Open-source licenses"
        val view = WebView(this)
        view.settings.javaScriptEnabled = false
        view.settings.blockNetworkLoads = true
        view.settings.allowFileAccess = false
        view.loadDataWithBaseURL(null, assets.open("licenses.html").bufferedReader().use { it.readText() }, "text/html", "UTF-8", null)
        setContentView(view)
    }
}
