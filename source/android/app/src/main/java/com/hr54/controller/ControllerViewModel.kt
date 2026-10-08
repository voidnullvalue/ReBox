package com.hr54.controller

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hr54.controller.data.api.*
import com.hr54.controller.data.model.*
import com.hr54.controller.data.repository.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.*

data class BrowseCursor(val parent: String = "", val title: String = "", val query: String = "", val offset: Int = 0, val previousOffsets: List<Int> = emptyList())
data class ControllerState(
    val receiver: String? = null, val ready: Boolean = false, val connected: Boolean = false,
    val modules: List<ModuleDescriptor> = emptyList(), val destination: String = "Home", val activeModule: String? = null,
    val managedModule: String? = null, val cursor: BrowseCursor = BrowseCursor(), val history: List<BrowseCursor> = emptyList(),
    val page: MediaPage = MediaPage(), val settings: ModuleSettings = ModuleSettings(), val actionResult: ActionResult? = null,
    val paired: Boolean = false, val playback: Playback = Playback(), val system: SystemStatus = SystemStatus(),
    val loading: Boolean = false, val busy: Boolean = false, val error: String? = null, val message: String? = null,
) {
    fun module(id: String?) = modules.find { it.id == id }
    val homeModules get() = modules.filter { it.installed && it.enabled && it.home }
}
class ControllerViewModel(
    private val preferences: ReceiverPreferences? = null,
    private val apiFactory: (String) -> ReceiverApi = { HttpReceiverApi(it) },
    private val tokens: ManagementTokenStore? = null,
) : ViewModel() {
    private val mutable = MutableStateFlow(ControllerState())
    val state = mutable.asStateFlow()
    private var api: ReceiverApi? = null
    private var receiverScope: CoroutineScope? = null
    private var connecting: Job? = null
    private var browsing: Job? = null
    private var poll: Job? = null
    private var actionPoll: Job? = null
    private var foreground = false
    private var epoch = 0
    private var loadGeneration = 0
    private var registryGeneration = 0
    private var retry: (() -> Unit)? = null
    private var pendingAction: Pair<String, String>? = null
    private val actionMutex = Mutex()
    private val refreshMutex = Mutex()
    private fun update(block: (ControllerState) -> ControllerState) = mutable.update(block)
    init { preferences?.let { viewModelScope.launch { it.receiver.first()?.let { address -> if (state.value.receiver == null) { update { s -> s.copy(receiver = address) }; if (foreground) connect(address, false) } } } } }
    fun dismissAction() { actionPoll?.cancel(); pendingAction = null; update { it.copy(actionResult = null) } }
    fun clearMessage() = update { it.copy(message = null) }
    fun connect(address: String, save: Boolean = true) {
        connecting?.cancel(); receiverScope?.cancel(); api = null; retry = null; pendingAction = null; ++loadGeneration; val generation = ++epoch
        connecting = viewModelScope.launch {
            update { ControllerState(receiver = address, loading = true) }
            try {
                val base = normalizeReceiver(address); val candidate = apiFactory(base)
                val token = tokens?.read(base); candidate.setManagementToken(token)
                val modules = candidate.get("/api/modules").decode<ModuleRegistry>().validated().modules.sortedWith(compareBy<ModuleDescriptor> { it.order }.thenBy { it.id })
                val system = candidate.get("/api/system/status").decode<SystemStatus>(); check(system.ready) { "Receiver core is starting" }
                val playback = playback(candidate)
                if (generation != epoch) return@launch
                if (save) preferences?.save(base)
                api = candidate; receiverScope = CoroutineScope(viewModelScope.coroutineContext + SupervisorJob(viewModelScope.coroutineContext[Job]))
                pendingAction = null
                update { ControllerState(receiver = base, ready = true, connected = true, modules = modules, system = system, playback = playback, paired = token != null) }
                setForeground(foreground)
            } catch (e: Exception) { if (e is CancellationException) throw e; update { it.copy(error = e.message, connected = false) } }
            finally { if (generation == epoch) update { it.copy(loading = false) } }
        }
    }
    private suspend fun playback(a: ReceiverApi): Playback {
        val raw = a.get("/api/state"); require(raw["playing"]?.jsonPrimitive?.booleanOrNull != null) { "Invalid playback state" }
        return raw.decode<Playback>().also {
            require(!it.playing || moduleId(it.source.orEmpty())) { "Invalid playback provider" }
            require(it.elapsed.isFinite() && it.elapsed >= 0 && (it.duration == null || (it.duration.isFinite() && it.duration in 0.0..604800.0)) &&
                it.generation >= 0 && it.instance.toByteArray().size <= 32 && it.title.toByteArray().size <= 255) { "Invalid playback state" }
        }
    }
    fun setForeground(active: Boolean) {
        foreground = active; poll?.cancel(); actionPoll?.cancel()
        if (!active) return
        if (api == null && !state.value.loading) { state.value.receiver?.let { connect(it, false) }; return }
        poll = receiverScope?.launch { var count = 0; while (isActive) { refreshState(); if (count++ % 3 == 0) refreshModules(); delay(2000) } }
        startActionPoll()
    }
    suspend fun refreshState() = refreshMutex.withLock {
        val a = api ?: return@withLock; val generation = epoch
        try {
            val next = playback(a); val system = a.get("/api/system/status").decode<SystemStatus>()
            if (generation == epoch) update { s ->
                val current = s.playback
                val accepted = if (current.instance == next.instance && next.generation < current.generation) current else next
                s.copy(playback = accepted, system = system, connected = true)
            }
        } catch (e: Exception) { if (e is CancellationException) throw e; if (generation == epoch) update { it.copy(connected = false) } }
    }
    suspend fun refreshModules() {
        val a = api ?: return; val generation = epoch; val request = ++registryGeneration
        try {
            val rows = a.get("/api/modules").decode<ModuleRegistry>().validated().modules.sortedWith(compareBy<ModuleDescriptor> { it.order }.thenBy { it.id })
            if (generation == epoch && request == registryGeneration) update { s ->
                val active = rows.find { it.id == s.activeModule }
                val available = active?.let { it.installed && it.enabled } ?: false
                val managed = rows.find { it.id == s.managedModule }
                val settingsGone = s.destination == "ModuleSettings" && (managed == null || !managed.installed || !managed.enabled || !managed.healthy)
                s.copy(modules = rows, destination = if (s.destination == "Module" && !available) "Home" else if (settingsGone) "Modules" else s.destination,
                    settings = if (settingsGone) ModuleSettings() else s.settings,
                    managedModule = s.managedModule.takeIf { id -> rows.any { it.id == id } })
            }
        } catch (e: Exception) { if (e is CancellationException) throw e; if (generation == epoch) update { it.copy(message = e.message) } }
    }
    fun retryConnection() { if (!state.value.ready) state.value.receiver?.let { connect(it, false) } else receiverScope?.launch { refreshState(); refreshModules() } }
    fun testConnection() { receiverScope?.launch { refreshState(); refreshModules(); update { it.copy(message = if (it.connected) "Receiver connection verified" else "Receiver unavailable") } } }
    private fun cancelBrowse() { ++loadGeneration; browsing?.cancel(); actionPoll?.cancel(); pendingAction = null }
    fun destination(name: String) {
        require(name in listOf("Home", "Settings", "Modules")); cancelBrowse()
        update { it.copy(destination = name, loading = false, error = null, managedModule = null, actionResult = null) }
        if (name == "Modules") receiverScope?.launch { refreshModules() }
    }
    private fun load(retryAction: () -> Unit, block: suspend (ReceiverApi) -> Unit) {
        browsing?.cancel(); retry = retryAction; val generation = ++loadGeneration; val session = epoch
        browsing = receiverScope?.launch {
            update { it.copy(loading = true, error = null) }
            try { block(api ?: error("Receiver not configured")) }
            catch (e: Exception) { if (e is CancellationException) throw e; if (generation == loadGeneration && session == epoch) update { it.copy(error = e.message) } }
            finally { if (generation == loadGeneration && session == epoch) update { it.copy(loading = false) } }
        }
    }
    fun retryList() { retry?.invoke() }
    fun openModule(id: String) {
        val module = state.value.module(id) ?: return; if (!module.installed || !module.enabled || !module.healthy) { update { it.copy(message = module.error.ifBlank { "Module unavailable" }) }; return }
        cancelBrowse(); update { it.copy(destination = "Module", activeModule = id, history = emptyList(), cursor = BrowseCursor(title = module.name), page = MediaPage(), loading = false, error = null, actionResult = null) }
        if (module.kind == "native-app") return
        if (module.capabilities.browse) browse()
        else if (!module.capabilities.search && module.capabilities.settings) moduleSettings(id)
    }
    private fun browse(cursor: BrowseCursor = state.value.cursor) {
        val id = state.value.activeModule ?: return
        load({ browse(cursor) }) { a ->
            val page = a.get(moduleRoute(id, if (cursor.query.isEmpty()) "browse" else "search"), mapOf("parent" to cursor.parent, "q" to cursor.query, "offset" to cursor.offset.toString(), "limit" to "60")).decode<MediaPage>().validated()
            currentCoroutineContext().ensureActive()
            if (state.value.activeModule == id && state.value.destination == "Module") update { it.copy(cursor = cursor.copy(offset = page.offset), page = page) }
        }
    }
    fun select(item: MediaItem) {
        when (item.kind) {
            "folder" -> { if (state.value.history.size >= 12) { update { it.copy(message = "Folder is too deep") }; return }; update { it.copy(history = it.history + it.cursor) }; browse(BrowseCursor(parent = item.id, title = item.title)) }
            "action" -> state.value.activeModule?.let { moduleAction(it, item.id) }
        }
    }
    fun search(query: String) {
        val q = truncateUtf8(query).trim(); val s = state.value
        if (q.isEmpty()) { if (s.cursor.query.isNotEmpty()) up(); return }
        if (s.cursor.query.isEmpty()) { if (s.history.size >= 12) return; update { it.copy(history = it.history + it.cursor) } }
        browse(BrowseCursor(title = "Search results", query = q))
    }
    fun moreItems() { val s = state.value; val page = s.page; if (page.hasMore) browse(s.cursor.copy(offset = page.offset + page.items.size, previousOffsets = (s.cursor.previousOffsets + page.offset).takeLast(128))) }
    fun previousPage() { val s = state.value; if (s.cursor.previousOffsets.isNotEmpty()) browse(s.cursor.copy(offset = s.cursor.previousOffsets.last(), previousOffsets = s.cursor.previousOffsets.dropLast(1))) }
    fun up(): Boolean {
        val s = state.value
        if (s.destination == "ModuleSettings") { cancelBrowse(); update { it.copy(destination = "Modules", actionResult = null, loading = false) }; return true }
        if (s.destination != "Module" || s.history.isEmpty()) return false
        val cursor = s.history.last(); update { it.copy(history = it.history.dropLast(1)) }; browse(cursor); return true
    }
    private fun action(management: Boolean = false, block: suspend (ReceiverApi) -> Unit) {
        val session = epoch
        receiverScope?.launch { actionMutex.withLock {
            if (session != epoch) return@withLock
            update { it.copy(busy = true, message = null) }
            try { if (management) check(state.value.paired) { "Pair module management in Settings first" }; block(api ?: error("Receiver not configured")) }
            catch (e: Exception) {
                if (e is CancellationException) throw e
                if (management && e is ReceiverFailure && e.httpCode == 401) forgetPairing()
                update { it.copy(message = e.message) }
            } finally { if (session == epoch) { refreshState(); update { it.copy(busy = false) } } }
        } }
    }
    fun play(item: MediaItem) {
        val id = state.value.activeModule ?: return; if (!item.playable) return
        action { a -> a.post(moduleRoute(id, "play"), body("itemId" to item.id, "startSeconds" to item.resume)) }
    }
    fun transport(command: String, seconds: Double? = null, delta: Int? = null) = action { a ->
        val current = playback(a); update { it.copy(playback = current) }; val t = current.transport
        val supported = when (command) { "stop" -> t.stop; "pause" -> t.pause; "resume" -> t.resume; "seek" -> t.seek; "channelUp" -> t.channelUp; "channelDown" -> t.channelDown; else -> false }
        if (supported) a.post("/api/playback/$command", when { seconds != null -> body("seconds" to seconds); delta != null -> body("delta" to delta); else -> body() })
    }
    fun nativeApp(id: String, start: Boolean) = action { a ->
        val module = state.value.module(id) ?: error("Module unavailable")
        require(module.kind == "native-app" && module.capabilities.nativeApp)
        check(!start || (!module.presentation.releaseInput && !module.presentation.releaseSurface)) {
            "Launch this app with the HR54 remote so its display and input can be released safely"
        }
        a.post(moduleRoute(id, "native/${if (start) "start" else "stop"}"))
    }
    private suspend fun forgetPairing() { api?.setManagementToken(null); update { it.copy(paired = false) }; state.value.receiver?.let { tokens?.write(it, null) } }
    fun pair(code: String) = action { a ->
        val result = a.post("/api/management/pair", body("code" to code.trim().lowercase()))
        val token = result["token"]?.jsonPrimitive?.content ?: error("No pairing token returned")
        require(Regex("[a-f0-9]{64}").matches(token)) { "Invalid pairing token" }
        tokens?.write(state.value.receiver!!, token); a.setManagementToken(token); update { it.copy(paired = true, message = "Module management paired") }
    }
    fun revokePairing() = action(true) { a -> a.post("/api/management/revoke"); tokens?.write(state.value.receiver!!, null); a.setManagementToken(null); update { it.copy(paired = false, message = "Management pairing revoked") } }
    fun manage(id: String, operation: String) = action(true) { a ->
        require(operation in listOf("enable", "disable", "uninstall", "reinstall"))
        if (operation == "uninstall") a.delete(moduleRoute(id)) else a.post(moduleRoute(id, operation))
        refreshModules(); update { it.copy(message = "Module updated") }
    }
    fun install(url: String) = action(true) { a ->
        val value = url.trim(); require(value.toByteArray().size <= 1024 && (value.startsWith("https://") || value.startsWith("http://"))) { "Enter an HTTP/HTTPS package URL (up to 1024 bytes)" }
        a.post("/api/modules/install", body("url" to value)); refreshModules(); update { it.copy(message = "Module installed") }
    }
    fun moduleSettings(id: String) {
        cancelBrowse(); update { it.copy(destination = "ModuleSettings", managedModule = id, settings = ModuleSettings(), actionResult = null) }
        load({ moduleSettings(id) }) { a -> val fields = a.get(moduleRoute(id, "settings")).decode<ModuleSettings>().validated(); update { it.copy(settings = fields) } }
    }
    fun saveField(id: String, field: ModuleField, value: JsonPrimitive) = action(true) { a ->
        val result = a.post(moduleRoute(id, "settings"), JsonObject(mapOf(field.key to value))).decode<ModuleSettings>().validated()
        if (state.value.managedModule == id) update { it.copy(settings = result, message = "Module setting saved") }
    }
    fun moduleAction(id: String, action: String) = action(true) { a ->
        require(moduleId(action)); val result = a.post(moduleRoute(id, "actions/$action")).decode<ActionResult>().validated()
        update { it.copy(actionResult = result, managedModule = id) }
        pendingAction = result.pollAction.takeIf { result.pending || !result.code.isNullOrBlank() }?.takeIf(::moduleId)?.let { id to it }
        startActionPoll()
    }
    private fun startActionPoll() {
        actionPoll?.cancel(); val (id, operation) = pendingAction ?: return; if (!foreground) return
        actionPoll = receiverScope?.launch {
            val deadline = System.nanoTime() + 600_000_000_000L
            while (isActive && foreground && System.nanoTime() < deadline) {
                delay(2000)
                try {
                    val result = actionMutex.withLock { api!!.post(moduleRoute(id, "actions/$operation")).decode<ActionResult>().validated() }
                    update { it.copy(actionResult = result) }
                    if (result.authenticated || !result.pending) {
                        pendingAction = null
                        if (state.value.managedModule == id && state.value.destination == "ModuleSettings") {
                            val fields = api!!.get(moduleRoute(id, "settings")).decode<ModuleSettings>().validated()
                            update { it.copy(settings = fields) }
                        }
                        return@launch
                    }
                } catch (e: Exception) { if (e is CancellationException) throw e; if (e is ReceiverFailure && e.httpCode == 401) forgetPairing(); update { it.copy(message = e.message) }; pendingAction = null; return@launch }
            }
        }
    }
    override fun onCleared() { receiverScope?.cancel(); connecting?.cancel(); super.onCleared() }
}
