package com.hr54.controller.data.api

import com.hr54.controller.data.model.*
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.*

/** Runtime-only fixture: arbitrary module identities, no receiver service dispatch. */
class FakeReceiverApi : ReceiverApi {
    var online = true
    var playing = false
    var source: String? = null
    var paused = false
    var instance = "fixture-core"
    var generation = 0L
    var nativeModule = ""
    var authorizedToken: String? = null
    var expectedToken = "a".repeat(64)
    var actionApproved = false
    val modules = mutableListOf(
        ModuleDescriptor("provider-0", "Example provider", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(true, true, true, true)),
        ModuleDescriptor("search-first", "Search provider", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(search = true, playback = true)),
        ModuleDescriptor("native-example", "Native app", kind = "native-app", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(nativeApp = true)),
    )
    val calls = mutableListOf<Triple<String, Map<String, String>, JsonObject?>>()
    private val fields = mutableMapOf<String, JsonPrimitive>("enabledFeature" to JsonPrimitive(true))
    override fun setManagementToken(token: String?) { authorizedToken = token }
    private val fixtureJson = Json(apiJson) { encodeDefaults = true }
    private inline fun <reified T> json(value: T) = fixtureJson.encodeToJsonElement(value).jsonObject
    private fun checkConnection() { if (!online) throw ReceiverFailure("Receiver unreachable") }
    override suspend fun get(path: String, query: Map<String, String>): JsonObject {
        checkConnection(); calls += Triple(path, query, null)
        if (path == "/api/modules") return json(ModuleRegistry(1, modules.toList()))
        if (path == "/api/state") return json(Playback(playing = playing, source = source, title = "A media item", itemId = "item", paused = paused, transport = Transport(playing, playing && !paused, playing && paused, playing), instance = instance, generation = generation))
        if (path == "/api/system/status") return json(SystemStatus(true, playing || nativeModule.isNotEmpty(), nativeModule))
        val id = path.removePrefix("/api/modules/").substringBefore('/')
        val module = modules.find { it.id == id } ?: throw ReceiverFailure("Module not found", 404)
        when (path.substringAfter("/api/modules/$id/")) {
            "browse", "search" -> {
                val offset = query["offset"]?.toInt() ?: 0
                val row = if (query["parent"].isNullOrEmpty() && query["q"].isNullOrEmpty()) MediaItem("folder-a", "Collection", "folder")
                    else MediaItem("item-$offset", "Result $offset", playable = true, subtitle = query["q"].orEmpty(), artwork = "art/$offset")
                return json(MediaPage(listOf(row), 3, offset, offset < 2))
            }
            "settings" -> return json(ModuleSettings(listOf(ModuleField("enabledFeature", "Feature", "bool", fields.getValue("enabledFeature"))), listOf(ModuleAction("connect", "Connect"))))
            "native/status" -> return buildJsonObject { put("ok", true); put("running", nativeModule == module.id) }
        }
        throw ReceiverFailure("Unknown fixture operation", 404)
    }
    override suspend fun post(path: String, body: JsonObject): JsonObject {
        checkConnection(); calls += Triple(path, emptyMap(), body)
        if (path == "/api/management/pair") return buildJsonObject { put("ok", true); put("token", expectedToken) }
        if (managementRequest("POST", path) && authorizedToken != expectedToken) throw ReceiverFailure("Pairing required", 401)
        if (path == "/api/management/revoke") { expectedToken = "b".repeat(64); return buildJsonObject { put("ok", true) } }
        if (path == "/api/modules/install") {
            modules += ModuleDescriptor("installed-later", "Installed after compilation", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(browse = true, playback = true))
            return buildJsonObject { put("ok", true) }
        }
        if (path.startsWith("/api/playback/")) {
            when (path.substringAfterLast('/')) { "stop" -> { playing = false; source = null }; "pause" -> paused = true; "resume" -> paused = false }
            return buildJsonObject { put("ok", true) }
        }
        val id = path.removePrefix("/api/modules/").substringBefore('/')
        val index = modules.indexOfFirst { it.id == id }; if (index < 0) throw ReceiverFailure("Module missing", 404)
        when (path.substringAfter("/api/modules/$id/")) {
            "play" -> { playing = true; source = id; generation++; paused = false }
            "enable" -> modules[index] = modules[index].copy(enabled = true)
            "disable" -> modules[index] = modules[index].copy(enabled = false)
            "reinstall" -> modules[index] = modules[index].copy(installed = true, enabled = true)
            "native/start" -> nativeModule = id
            "native/stop" -> nativeModule = ""
            "settings" -> { fields.putAll(body.mapValues { it.value.jsonPrimitive }); return get(path, emptyMap()) }
            "actions/connect" -> return json(ActionResult("fixture-code", "Approve this module action", true, "poll"))
            "actions/poll" -> return json(ActionResult(pending = !actionApproved, authenticated = actionApproved))
        }
        return buildJsonObject { put("ok", true) }
    }
    override suspend fun delete(path: String): JsonObject {
        checkConnection(); if (authorizedToken != expectedToken) throw ReceiverFailure("Pairing required", 401)
        calls += Triple(path, emptyMap(), JsonObject(emptyMap()))
        val id = path.substringAfterLast('/'); val i = modules.indexOfFirst { it.id == id }; if (i >= 0) { if (modules[i].core) modules[i] = modules[i].copy(installed = false, enabled = false) else modules.removeAt(i) }
        return buildJsonObject { put("ok", true) }
    }
}
