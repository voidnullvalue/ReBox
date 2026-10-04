package com.hr54.controller.data.api

import kotlinx.serialization.json.*

/** In-memory receiver for UI development. No network or physical playback. */
class FakeReceiverApi : ReceiverApi {
    var online = true
    var authenticated = true
    var playing = false
    var source: String? = null
    var paused = false
    val calls = mutableListOf<Triple<String, Map<String, String>, JsonObject?>>()

    override suspend fun get(path: String, query: Map<String, String>): JsonObject {
        if (!online) throw ReceiverFailure("Receiver unreachable")
        calls += Triple(path, query, null)
        if (path == "/api/youtube/search" && playing)
            throw ReceiverFailure("Stop current playback before searching YouTube", 502)
        return apiJson
            .parseToJsonElement(
                when (path) {
                    "/api/capabilities" ->
                        """{"ok":true,"jellyfin":true,"iptv":true,"youtube":true,"frigate":true,"playback":true,"doom":false}"""
                    "/api/state" ->
                        """{"ok":true,"playing":$playing,"source":${source?.let { "\"$it\"" } ?: "null"},"title":"${if(playing) "Alien" else ""}","paused":$paused,"elapsed":1394,"transport":{"stop":$playing,"pause":${playing && source=="jellyfin"},"resume":${playing && source=="jellyfin"},"seek":${playing && source=="jellyfin"}}}"""
                    "/api/auth/status",
                    "/api/auth/poll" ->
                        """{"ok":true,"authenticated":$authenticated,"pending":${!authenticated},"code":"482 719"}"""
                    "/api/libraries" ->
                        """{"ok":true,"libraries":[{"id":"movies","name":"Movies"},{"id":"shows","name":"TV Shows"}]}"""
                    "/api/items" ->
                        """{"ok":true,"total":2,"items":[{"id":"alien","name":"Alien","type":"Movie","year":1979,"runtime":70200000000,"overview":"The crew of a commercial spacecraft encounters a deadly lifeform.","playable":true},{"id":"collection","name":"Science fiction","isFolder":true,"childCount":12}]}"""
                    "/api/iptv/groups" ->
                        """{"ok":true,"groups":[{"name":"News","count":75},{"name":"Sports","count":28}]}"""
                    "/api/iptv/channels" -> {
                        val offset = query["offset"]?.toInt() ?: 0
                        """{"ok":true,"total":120,"offset":$offset,"limit":60,"hasMore":${offset==0},"channels":[{"id":"ch-$offset","name":"World News ${offset+1}","group":"News"}]}"""
                    }
                    "/api/youtube/search" -> {
                        val page = query["page"]?.toInt() ?: 0
                        """{"ok":true,"page":$page,"hasMore":${page==0},"results":[{"id":"${if(page==0) "jNQXAC9IVRw" else "dQw4w9WgXcQ"}","title":"${if(page==0) "Me at the zoo" else "Never Gonna Give You Up"}","channel":"Music","duration":213}]}"""
                    }
                    "/api/frigate/cameras" ->
                        """{"ok":true,"cameras":[{"id":"driveway","name":"Driveway","playable":true,"live":true},{"id":"porch","name":"Front porch","reason":"H.264 restream unavailable"}]}"""
                    else -> """{"ok":true}"""
                }
            )
            .jsonObject
    }

    override suspend fun post(path: String, body: JsonObject): JsonObject {
        if (!online) throw ReceiverFailure("Receiver unreachable")
        calls += Triple(path, emptyMap(), body)
        when (path) {
            "/api/play" -> {
                playing = true
                source = "jellyfin"
            }
            "/api/iptv/play" -> {
                playing = true
                source = "iptv"
            }
            "/api/youtube/play" -> {
                playing = true
                source = "youtube"
            }
            "/api/frigate/play" -> {
                playing = true
                source = "frigate"
            }
            "/api/playback/stop" -> {
                playing = false
                source = null
            }
            "/api/playback/pause" -> paused = true
            "/api/playback/resume" -> paused = false
            "/api/auth/logout" -> authenticated = false
            "/api/auth/start" ->
                return buildJsonObject {
                    put("ok", true)
                    put("pending", true)
                    put("code", "482 719")
                }
        }
        return buildJsonObject { put("ok", true) }
    }
}
