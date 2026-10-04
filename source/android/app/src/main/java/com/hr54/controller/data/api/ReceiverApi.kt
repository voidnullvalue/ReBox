package com.hr54.controller.data.api

import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.*
import okhttp3.*
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody

val apiJson = Json {
    ignoreUnknownKeys = true
    coerceInputValues = true
}

fun normalizeReceiver(input: String): String {
    val raw = input.trim()
    require(raw.isNotEmpty()) { "Enter a receiver address" }
    val url = (if ("://" in raw) raw else "http://$raw").toHttpUrl()
    require(
        url.username.isEmpty() &&
            url.password.isEmpty() &&
            url.encodedPath == "/" &&
            url.query == null &&
            url.fragment == null
    ) {
        "Enter a host address, without a path or credentials"
    }
    val explicitPort = Regex(":\\d+$").containsMatchIn(raw.trimEnd('/'))
    val normalized = url.newBuilder().apply { if (!explicitPort) port(8130) }.build()
    val host = if (":" in normalized.host) "[${normalized.host}]" else normalized.host
    // Keep an explicit port even for HTTP :80 / HTTPS :443, so normalization is idempotent.
    return "${normalized.scheme}://$host:${normalized.port}"
}

class ReceiverFailure(val reason: String, val httpCode: Int? = null) : IOException(reason)

interface ReceiverApi {
    suspend fun get(path: String, query: Map<String, String> = emptyMap()): JsonObject

    suspend fun post(path: String, body: JsonObject = JsonObject(emptyMap())): JsonObject
}

class HttpReceiverApi(val base: String) : ReceiverApi {
    private val normal =
        OkHttpClient.Builder()
            .connectTimeout(8, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .callTimeout(20, TimeUnit.SECONDS)
            .build()
    private val long =
        normal
            .newBuilder()
            .readTimeout(100, TimeUnit.SECONDS)
            .callTimeout(110, TimeUnit.SECONDS)
            .build()

    override suspend fun get(path: String, query: Map<String, String>): JsonObject =
        request(path, query, null)

    override suspend fun post(path: String, body: JsonObject): JsonObject =
        request(path, emptyMap(), body)

    private suspend fun request(
        path: String,
        query: Map<String, String>,
        body: JsonObject?,
    ): JsonObject =
        withContext(Dispatchers.IO) {
            val url =
                (base + path)
                    .toHttpUrl()
                    .newBuilder()
                    .apply { query.forEach { (k, v) -> addQueryParameter(k, v) } }
                    .build()
            val request =
                Request.Builder()
                    .url(url)
                    .apply {
                        if (body != null)
                            post(body.toString().toRequestBody("application/json".toMediaType()))
                    }
                    .build()
            val slowPlayback = path in setOf(
                "/api/play", "/api/playback/resume", "/api/playback/seek",
            )
            val call = (if (path.startsWith("/api/youtube/") || slowPlayback) long else normal).newCall(request)
            val raw =
                suspendCancellableCoroutine<Pair<Int, String>> { continuation ->
                    continuation.invokeOnCancellation { call.cancel() }
                    call.enqueue(
                        object : Callback {
                            override fun onFailure(call: Call, e: IOException) {
                                if (continuation.isActive)
                                    continuation.resumeWithException(
                                        ReceiverFailure("Receiver unreachable: ${e.message}")
                                    )
                            }

                            override fun onResponse(call: Call, response: Response) {
                                try {
                                    response.use {
                                        val result = it.code to (it.body?.string() ?: "")
                                        if (continuation.isActive) continuation.resume(result)
                                    }
                                } catch (e: IOException) {
                                    if (continuation.isActive)
                                        continuation.resumeWithException(
                                            ReceiverFailure(
                                                "Receiver response interrupted: ${e.message}"
                                            )
                                        )
                                }
                            }
                        }
                    )
                }
            val obj =
                try {
                    apiJson.parseToJsonElement(raw.second).jsonObject
                } catch (e: Exception) {
                    throw ReceiverFailure("Receiver returned an invalid JSON response", raw.first)
                }
            if (raw.first == 404 && path in setOf("/api/capabilities", "/api/state"))
                throw ReceiverFailure(
                    "Receiver is reachable, but $path is missing. Update the receiver media server to the native-client API version. Server reason: ${obj["error"]?.jsonPrimitive?.contentOrNull ?: "HTTP 404"}",
                    raw.first,
                )
            if (raw.first !in 200..299 || obj["ok"]?.jsonPrimitive?.booleanOrNull == false)
                throw ReceiverFailure(
                    obj["error"]?.jsonPrimitive?.contentOrNull
                        ?: "Receiver responded with HTTP ${raw.first}",
                    raw.first,
                )
            obj
        }
}

inline fun <reified T> JsonObject.decode(): T = apiJson.decodeFromJsonElement(this)

fun body(vararg values: Pair<String, Any>): JsonObject = buildJsonObject {
    values.forEach { (k, v) ->
        when (v) {
            is String -> put(k, v)
            is Boolean -> put(k, v)
            is Number -> put(k, v)
            else -> error("Unsupported JSON value")
        }
    }
}
