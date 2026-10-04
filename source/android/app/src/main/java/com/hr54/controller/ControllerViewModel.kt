package com.hr54.controller

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hr54.controller.data.api.*
import com.hr54.controller.data.model.*
import com.hr54.controller.data.repository.ReceiverPreferences
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

data class Folder(
    val id: String?,
    val name: String,
    val items: List<Item>,
    val total: Int,
    val query: String = "",
)

data class ControllerState(
    val receiver: String? = null,
    val capabilities: Capabilities = Capabilities(),
    val ready: Boolean = false,
    val connected: Boolean = false,
    val playback: Playback = Playback(),
    val destination: String = "Home",
    val loading: Boolean = false,
    val busy: Boolean = false,
    val error: String? = null,
    val message: String? = null,
    val auth: Auth = Auth(),
    val folders: List<Folder> = emptyList(),
    val items: List<Item> = emptyList(),
    val itemTotal: Int = 0,
    val groups: List<Group> = emptyList(),
    val group: String = "",
    val channels: List<Channel> = emptyList(),
    val channelMore: Boolean = false,
    val videos: List<Video> = emptyList(),
    val videoPage: Int = 0,
    val videoMore: Boolean = false,
    val cameras: List<Camera> = emptyList(),
    val knownDurations: Map<String, Double> = emptyMap(),
    val jellyQuery: String = "",
    val tvQuery: String = "",
    val youtubeQuery: String = "",
)

class ControllerViewModel(
    private val preferences: ReceiverPreferences? = null,
    private val apiFactory: (String) -> ReceiverApi = { HttpReceiverApi(it) },
) : ViewModel() {
    private val mutable = MutableStateFlow(ControllerState())
    val state = mutable.asStateFlow()
    private var api: ReceiverApi? = null
    private var poll: Job? = null
    private var browsing: Job? = null
    private var authPoll: Job? = null
    private var foreground = false
    private val stateMutex = Mutex()
    private val actionMutex = Mutex()
    private var retry: (() -> Unit)? = null
    private var searchSnapshot: ControllerState? = null
    private var loadGeneration = 0

    init {
        preferences?.let {
            viewModelScope.launch {
                it.receiver.first()?.let { address ->
                    update { s -> s.copy(receiver = address) }
                    if (foreground) connect(address, false)
                }
            }
        }
    }

    private fun update(block: (ControllerState) -> ControllerState) {
        mutable.update(block)
    }

    fun clearMessage() = update { it.copy(message = null) }

    fun setForeground(active: Boolean) {
        foreground = active
        poll?.cancel()
        authPoll?.cancel()
        if (active && api == null && state.value.receiver != null && !state.value.loading) {
            connect(state.value.receiver!!, false)
            return
        }
        if (active && api != null) {
            poll = viewModelScope.launch {
                while (isActive) {
                    refreshState()
                    delay(2000)
                }
            }
            if (state.value.destination == "Jellyfin" && state.value.auth.pending) pollAuth()
        }
    }

    fun connect(address: String, save: Boolean = true) {
        browsing?.cancel()
        viewModelScope.launch {
            update { it.copy(loading = true, error = null) }
            try {
                val base = normalizeReceiver(address)
                val candidate = apiFactory(base)
                val caps = candidate.get("/api/capabilities").decode<Capabilities>()
                val playback = candidate.get("/api/state").decode<Playback>()
                api = candidate
                if (save) preferences?.save(base)
                update {
                    ControllerState(
                        receiver = base,
                        capabilities = caps,
                        ready = true,
                        connected = true,
                        playback = playback,
                    )
                }
                setForeground(foreground)
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                update { it.copy(error = e.message, connected = false) }
            } finally {
                update { it.copy(loading = false) }
            }
        }
    }

    suspend fun refreshState() {
        stateMutex.withLock {
            try {
                val playback = api?.get("/api/state")?.decode<Playback>() ?: return
                update { it.copy(playback = playback, connected = true) }
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                update { it.copy(connected = false) }
            }
        }
    }

    fun testConnection() {
        viewModelScope.launch {
            try {
                val a = api ?: error("Receiver not configured")
                a.get("/api/capabilities")
                refreshState()
                update {
                    it.copy(
                        message =
                            if (it.connected) "Receiver connection verified"
                            else "Receiver state unavailable"
                    )
                }
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                update { it.copy(message = e.message, connected = false) }
            }
        }
    }

    fun retryConnection() {
        if (!state.value.ready) state.value.receiver?.let { connect(it, false) }
        else viewModelScope.launch { refreshState() }
    }

    fun destination(name: String) {
        if (name != "Settings" && name !in state.value.capabilities.destinations()) return
        browsing?.cancel()
        authPoll?.cancel()
        update { it.copy(destination = name, loading = false, error = null) }
        when (name) {
            "Jellyfin" -> openJellyfin()
            "Live TV" -> if (state.value.channels.isEmpty()) loadChannels()
            "Cameras" -> loadCameras()
        }
    }

    private fun load(action: () -> Unit, block: suspend (ReceiverApi) -> Unit) {
        browsing?.cancel()
        retry = action
        val generation = ++loadGeneration
        browsing = viewModelScope.launch {
            update { it.copy(loading = true, error = null) }
            try {
                block(api ?: error("Receiver not configured"))
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                update { it.copy(error = e.message) }
            } finally {
                if (generation == loadGeneration) update { it.copy(loading = false) }
            }
        }
    }

    fun retryList() {
        retry?.invoke()
    }

    fun openJellyfin(): Unit =
        load(::openJellyfin) { a ->
            val auth = a.get("/api/auth/status").decode<Auth>()
            update { it.copy(auth = auth) }
            if (auth.authenticated) {
                if (state.value.items.isEmpty()) libraries(a)
            } else if (auth.pending) pollAuth()
        }

    private suspend fun libraries(a: ReceiverApi) {
        val result =
            a.get("/api/libraries").decode<Libraries>().libraries.map { it.copy(isFolder = true) }
        update {
            it.copy(items = result, itemTotal = result.size, folders = emptyList(), jellyQuery = "")
        }
    }

    fun startAuth(): Unit =
        load(::startAuth) { a ->
            val auth = a.post("/api/auth/start").decode<Auth>()
            update { it.copy(auth = auth) }
            pollAuth()
        }

    private fun pollAuth() {
        authPoll?.cancel()
        authPoll = viewModelScope.launch {
            while (isActive && foreground && state.value.destination == "Jellyfin") {
                delay(2500)
                try {
                    val auth = api!!.get("/api/auth/poll").decode<Auth>()
                    update { it.copy(auth = auth) }
                    if (auth.authenticated) {
                        libraries(api!!)
                        break
                    }
                    if (auth.expired) break
                } catch (e: Exception) {
                    if (e is CancellationException) throw e
                    update { it.copy(error = e.message) }
                    delay(3000)
                }
            }
        }
    }

    fun logout(): Unit =
        load(::logout) { a ->
            a.post("/api/auth/logout")
            update { it.copy(auth = Auth(), items = emptyList(), folders = emptyList()) }
        }

    fun folder(item: Item): Unit =
        load({ folder(item) }) { a ->
            val s = state.value
            val result =
                a.get("/api/items", mapOf("parent" to item.id, "limit" to "60")).decode<Items>()
            update {
                it.copy(
                    folders =
                        s.folders +
                            Folder(
                                s.folders.lastOrNull()?.id,
                                s.folders.lastOrNull()?.name ?: "Libraries",
                                s.items,
                                s.itemTotal,
                                s.jellyQuery,
                            ) +
                            Folder(item.id, item.name, emptyList(), 0),
                    items = result.items,
                    itemTotal = result.total,
                    jellyQuery = "",
                )
            }
        }

    fun up(): Boolean {
        val s = state.value
        if (s.destination != "Jellyfin") return false
        if (s.jellyQuery.isNotEmpty()) {
            searchJellyfin("")
            return true
        }
        if (s.folders.isEmpty()) return false
        browsing?.cancel()
        val previous = s.folders[s.folders.size - 2]
        update {
            it.copy(
                folders = s.folders.dropLast(2),
                items = previous.items,
                itemTotal = previous.total,
                jellyQuery = previous.query,
                error = null,
                loading = false,
            )
        }
        return true
    }

    fun searchJellyfin(query: String): Unit =
        load({ searchJellyfin(query) }) { a ->
            val q = truncateUtf8(query)
            if (q.isEmpty()) {
                val cached = searchSnapshot
                if (cached != null) {
                    update {
                        it.copy(
                            items = cached.items,
                            itemTotal = cached.itemTotal,
                            folders = cached.folders,
                            jellyQuery = "",
                        )
                    }
                    searchSnapshot = null
                } else libraries(a)
            } else {
                if (state.value.jellyQuery.isEmpty()) searchSnapshot = state.value
                val r = a.get("/api/items", mapOf("search" to q, "limit" to "60")).decode<Items>()
                update { it.copy(items = r.items, itemTotal = r.total, jellyQuery = q) }
            }
        }

    fun moreItems(): Unit =
        load(::moreItems) { a ->
            val s = state.value
            val params = mutableMapOf("offset" to s.items.size.toString(), "limit" to "60")
            if (s.jellyQuery.isNotEmpty()) params["search"] = s.jellyQuery
            else s.folders.lastOrNull()?.id?.let { params["parent"] = it }
            val r = a.get("/api/items", params).decode<Items>()
            update {
                it.copy(items = (s.items + r.items).distinctBy(Item::id), itemTotal = r.total)
            }
        }

    fun loadChannels(
        group: String = state.value.group,
        query: String = state.value.tvQuery,
        more: Boolean = false,
    ): Unit =
        load({ loadChannels(group, query, more) }) { a ->
            if (state.value.groups.isEmpty()) {
                val g = a.get("/api/iptv/groups").decode<Groups>()
                update { it.copy(groups = g.groups) }
            }
            val s = state.value
            val q = truncateUtf8(query)
            val p =
                mutableMapOf(
                    "limit" to "60",
                    "offset" to if (more) s.channels.size.toString() else "0",
                )
            if (group.isNotEmpty()) p["group"] = group
            if (q.isNotEmpty()) p["query"] = q
            val r = a.get("/api/iptv/channels", p).decode<Channels>()
            update {
                it.copy(
                    group = group,
                    tvQuery = q,
                    channels =
                        if (more) (s.channels + r.channels).distinctBy(Channel::id) else r.channels,
                    channelMore = r.hasMore,
                )
            }
        }

    fun searchYoutube(query: String = state.value.youtubeQuery, more: Boolean = false): Unit =
        load({ searchYoutube(query, more) }) { a ->
            val q = truncateUtf8(query).trim()
            if (q.isEmpty()) return@load
            val s = state.value
            if (more && s.videoPage >= 100) return@load
            val page = if (more) s.videoPage + 1 else 0
            update { it.copy(youtubeQuery = q) }
            val parameters = mapOf("q" to q, "page" to page.toString())
            val r = actionMutex.withLock {
                try {
                    a.get("/api/youtube/search", parameters).decode<Videos>()
                } catch (e: ReceiverFailure) {
                    if (!e.reason.contains("stop", true) &&
                        !e.reason.contains("slot", true) && !e.reason.contains("busy", true)) throw e
                    stopCurrentPlayback(a)
                    a.get("/api/youtube/search", parameters).decode<Videos>()
                }
            }
            update {
                it.copy(
                    videos = if (more) (s.videos + r.results).distinctBy(Video::id) else r.results,
                    videoPage = r.page,
                    videoMore = r.hasMore && r.page < 100,
                )
            }
        }

    private suspend fun stopCurrentPlayback(a: ReceiverApi) {
        // Consult the receiver even when the last foreground poll is stale.
        val current = a.get("/api/state").decode<Playback>()
        update { it.copy(playback = current, connected = true) }
        if (current.playing) {
            a.post("/api/playback/stop")
            val stopped = a.get("/api/state").decode<Playback>()
            update { it.copy(playback = stopped, connected = true) }
            check(!stopped.playing) { "Receiver could not stop current playback" }
        }
    }

    fun loadCameras(): Unit =
        load(::loadCameras) { a ->
            val r = a.get("/api/frigate/cameras").decode<Cameras>()
            update { it.copy(cameras = r.cameras) }
        }

    private fun action(block: suspend (ReceiverApi) -> Unit) {
        viewModelScope.launch {
            actionMutex.withLock {
                update { it.copy(busy = true, message = null) }
                try {
                    block(api ?: error("Receiver not configured"))
                    refreshState()
                } catch (e: Exception) {
                    if (e is CancellationException) throw e
                    update { it.copy(message = e.message) }
                    // A failed or restarted stream can change receiver state.
                    refreshState()
                } finally {
                    update { it.copy(busy = false) }
                }
            }
        }
    }

    fun play(item: Item) = action { a ->
        stopCurrentPlayback(a)
        a.post("/api/play", body("itemId" to item.id, "startSeconds" to 0, "returnToTv" to false))
        item.seconds?.let { duration ->
            update { it.copy(knownDurations = it.knownDurations + (item.id to duration)) }
        }
    }

    fun play(channel: Channel) = action {
        stopCurrentPlayback(it)
        it.post("/api/iptv/play", body("channelId" to channel.id))
    }

    fun play(video: Video) = action {
        require(video.thumbnail != null) { "Invalid video ID" }
        stopCurrentPlayback(it)
        it.post("/api/youtube/play", body("videoId" to video.id))
    }

    fun play(camera: Camera) = action {
        require(camera.playable)
        stopCurrentPlayback(it)
        it.post("/api/frigate/play", body("cameraId" to camera.id))
    }

    fun transport(command: String, seconds: Double? = null, delta: Int? = null) = action { a ->
        // Validate when dispatched, after any queued playback changes.
        val current = a.get("/api/state").decode<Playback>()
        update { it.copy(playback = current, connected = true) }
        val t = current.transport
        val supported = when (command) {
            "stop" -> t.stop
            "pause" -> t.pause
            "resume" -> t.resume
            "seek" -> t.seek
            else -> false
        }
        if (!supported) return@action
        a.post(
            "/api/playback/$command",
            when {
                seconds != null -> body("seconds" to seconds)
                delta != null -> body("delta" to delta)
                else -> body()
            },
        )
    }

    override fun onCleared() {
        poll?.cancel()
        authPoll?.cancel()
        super.onCleared()
    }
}
