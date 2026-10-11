package com.hr54.controller.game

import com.hr54.controller.data.api.apiJson
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response

/** Result of the bounded receiver LAN game health probe. */
sealed interface JeopardyHealth {
    data object Healthy : JeopardyHealth
    data class Unhealthy(val reason: String) : JeopardyHealth
}

/**
 * Bounded, credential-free health probe for the receiver's LAN game endpoint.
 *
 * It only ever contacts the exact plain-HTTP game origin on
 * [JeopardyWebPolicy.GAME_PORT], requests `/health`, never follows a redirect
 * (so an external/HTTPS redirect can never be reached), never sends the
 * management bearer and never opens an external browser. A captive portal that
 * answers every path with HTML is rejected because the body must carry the
 * module's own `{"ok":true,"service":"jeopardy"}` identity.
 */
object JeopardyHealthProbe {
    /** Health path served by the reviewed receiver game module. */
    const val PATH = "/health"

    /**
     * Default bounded client: no redirects, short timeouts, no bearer. Connection
     * retries stay enabled so a receiver restart that staled a pooled socket is
     * re-probed once instead of falsely reporting the game down.
     */
    val client: OkHttpClient = OkHttpClient.Builder()
        .followRedirects(false)
        .followSslRedirects(false)
        .connectTimeout(3, TimeUnit.SECONDS)
        .readTimeout(3, TimeUnit.SECONDS)
        .callTimeout(4, TimeUnit.SECONDS)
        .build()

    /**
     * The `/health` URL for a configured game origin, or null when [gameUrl] is
     * not the plain-HTTP game origin on [JeopardyWebPolicy.GAME_PORT] (or carries
     * credentials).
     */
    fun healthUrl(gameUrl: String): String? {
        val base = gameUrl.toHttpUrlOrNull() ?: return null
        if (base.scheme != "http" || base.port != JeopardyWebPolicy.GAME_PORT) return null
        if (base.username.isNotEmpty() || base.password.isNotEmpty()) return null
        return base.newBuilder().encodedPath(PATH).query(null).fragment(null).build().toString()
    }

    /** Pure classification so the status/identity rules are testable without a socket. */
    fun classify(statusCode: Int, body: String?): JeopardyHealth {
        if (statusCode != 200)
            return JeopardyHealth.Unhealthy("The receiver game answered HTTP $statusCode on its LAN port")
        val obj = runCatching { apiJson.parseToJsonElement(body.orEmpty()).jsonObject }.getOrNull()
            ?: return JeopardyHealth.Unhealthy("The address answered, but it is not the Jeopardy game (unexpected page)")
        // jsonPrimitive throws when a hostile or captive-portal body puts an
        // object/array where a scalar is expected, so guard the accessors too
        // instead of letting a malformed reply escape as an exception.
        val ok = runCatching { obj["ok"]?.jsonPrimitive?.booleanOrNull }.getOrNull() == true
        val service = runCatching { obj["service"]?.jsonPrimitive?.contentOrNull }.getOrNull()
        return if (ok && service == JeopardyWebPolicy.MODULE_ID) JeopardyHealth.Healthy
        else JeopardyHealth.Unhealthy("The address answered, but it is not the Jeopardy game (unexpected page)")
    }

    /**
     * Probes [gameUrl]'s `/health`. Suspends on IO and cancels the in-flight call
     * when the caller leaves or the receiver host changes.
     */
    suspend fun probe(gameUrl: String, client: OkHttpClient = this.client): JeopardyHealth {
        val url = healthUrl(gameUrl) ?: return JeopardyHealth.Unhealthy("Unsupported game address")
        return withContext(Dispatchers.IO) {
            val call = client.newCall(Request.Builder().url(url).get().build())
            val result = suspendCancellableCoroutine<ProbeResponse> { continuation ->
                continuation.invokeOnCancellation { call.cancel() }
                call.enqueue(object : Callback {
                    override fun onFailure(call: Call, e: java.io.IOException) {
                        if (continuation.isActive) continuation.resume(ProbeResponse.Failure(e.message ?: "connection failed"))
                    }

                    override fun onResponse(call: Call, response: Response) {
                        response.use {
                            val body = runCatching { it.body?.string() }.getOrNull()
                            if (continuation.isActive) continuation.resume(ProbeResponse.Answer(it.code, body))
                        }
                    }
                })
            }
            when (result) {
                is ProbeResponse.Answer ->
                    if (result.code in 300..399) JeopardyHealth.Unhealthy("The receiver game redirected off its LAN address")
                    else classify(result.code, result.body)
                is ProbeResponse.Failure -> JeopardyHealth.Unhealthy("Can't reach the receiver game: ${result.message}")
            }
        }
    }

    private sealed interface ProbeResponse {
        data class Answer(val code: Int, val body: String?) : ProbeResponse
        data class Failure(val message: String) : ProbeResponse
    }
}
