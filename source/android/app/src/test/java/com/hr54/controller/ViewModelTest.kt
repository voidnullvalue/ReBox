package com.hr54.controller

import com.hr54.controller.data.api.*
import com.hr54.controller.data.model.*
import com.hr54.controller.data.repository.ManagementTokenStore
import kotlinx.coroutines.*
import kotlinx.coroutines.test.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.*

@OptIn(ExperimentalCoroutinesApi::class)
class ViewModelTest {
    private val dispatcher = StandardTestDispatcher()
    @Before fun setup() { Dispatchers.setMain(dispatcher) }
    @After fun teardown() { Dispatchers.resetMain() }
    private class Tokens : ManagementTokenStore {
        val saved = mutableMapOf<String, String>()
        override suspend fun read(receiver: String) = saved[receiver]
        override suspend fun write(receiver: String, token: String?) { if (token == null) saved.remove(receiver) else saved[receiver] = token }
    }
    @Test fun unknownModuleBrowseSearchPlaybackAndLifecycleWithoutReconnect() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val id = "future-" + java.util.UUID.randomUUID().toString().take(12)
        api.modules[0] = api.modules[0].copy(id = id)
        val tokens = Tokens(); val vm = ControllerViewModel(apiFactory = { api }, tokens = tokens)
        vm.connect("receiver"); runCurrent(); assertTrue(vm.state.value.ready)
        vm.openModule(id); runCurrent(); assertEquals("Collection", vm.state.value.page.items.first().title)
        vm.select(vm.state.value.page.items.first()); runCurrent(); assertEquals("folder-a", vm.state.value.cursor.parent)
        vm.moreItems(); runCurrent(); assertEquals(1, vm.state.value.page.offset)
        assertEquals("1", api.calls.last { it.first.endsWith("/browse") }.second["offset"])
        vm.search("test"); runCurrent(); assertEquals("test", vm.state.value.page.items.first().subtitle)
        vm.play(vm.state.value.page.items.first()); runCurrent(); assertEquals(id, vm.state.value.playback.source)
        assertEquals("item-0", api.calls.last { it.first.endsWith("/play") }.third!!["itemId"]!!.jsonPrimitive.content)
        assertTrue(vm.up()); runCurrent(); assertEquals("folder-a", vm.state.value.cursor.parent)
        assertTrue(vm.up()); runCurrent(); assertEquals("", vm.state.value.cursor.parent)
        vm.transport("stop"); runCurrent(); vm.pair("abcdef12"); runCurrent(); assertTrue(vm.state.value.paired); assertEquals(api.expectedToken, tokens.saved["http://receiver:8130"])
        vm.manage(id, "disable"); runCurrent(); assertFalse(vm.state.value.homeModules.any { it.id == id })
        vm.manage(id, "enable"); runCurrent(); assertTrue(vm.state.value.homeModules.any { it.id == id })
        vm.manage(id, "uninstall"); runCurrent(); assertFalse(vm.state.value.modules.any { it.id == id })
        vm.install("http://example.test/new.rbox?x=a&token=2"); runCurrent(); assertTrue(vm.state.value.homeModules.any { it.id == "installed-later" })
        assertTrue(vm.state.value.ready)
    }
    @Test fun searchFirstDoesNotBrowseOrDispatchOnIdentity() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent()
        vm.openModule("search-first"); runCurrent(); assertFalse(api.calls.any { it.first.contains("search-first/browse") || it.first.contains("search-first/search") })
        vm.search("😀".repeat(30)); runCurrent(); assertEquals(64, api.calls.last { it.first.endsWith("/search") }.second.getValue("q").toByteArray().size)
    }
    @Test fun pairingIsReceiverScopedAnd401RevokesStoredBearer() = runTest(dispatcher) {
        val tokens = Tokens(); val receivers = mutableMapOf<String, FakeReceiverApi>()
        val vm = ControllerViewModel(apiFactory = { receivers.getOrPut(it) { FakeReceiverApi() } }, tokens = tokens)
        vm.connect("first"); runCurrent(); vm.pair("abcdef12"); runCurrent(); assertTrue(vm.state.value.paired)
        vm.connect("second"); runCurrent(); assertFalse(vm.state.value.paired); assertNull(receivers.getValue("http://second:8130").authorizedToken)
        vm.connect("first"); runCurrent(); assertTrue(vm.state.value.paired)
        receivers.getValue("http://first:8130").expectedToken = "b".repeat(64)
        vm.manage("provider-0", "disable"); runCurrent(); assertFalse(vm.state.value.paired); assertNull(tokens.saved["http://first:8130"])
        assertTrue(vm.state.value.module("provider-0")!!.enabled)
    }
    @Test fun unpairedManagementNeverPostsMutation() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent()
        vm.manage("provider-0", "uninstall"); runCurrent(); vm.install("https://example.test/new.rbox"); runCurrent()
        assertFalse(api.calls.any { it.third != null }); assertNotNull(vm.state.value.message)
    }
    @Test fun nativeAppUsesGenericLifecycleAndCorePresentation() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent()
        vm.openModule("native-example"); runCurrent(); vm.nativeApp("native-example", true); runCurrent()
        assertEquals("native-example", vm.state.value.system.nativeModule)
        vm.nativeApp("native-example", false); runCurrent(); assertEquals("", vm.state.value.system.nativeModule)
    }
    @Test fun unsupportedTransportNeverPostedAndCommandsSerializeFreshState() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent()
        vm.transport("pause"); runCurrent(); assertFalse(api.calls.any { it.first == "/api/playback/pause" })
        api.playing = true; api.source = "provider-0"
        vm.transport("pause"); vm.transport("resume"); vm.transport("seek", delta = 30); runCurrent()
        assertEquals(listOf("/api/playback/pause", "/api/playback/resume", "/api/playback/seek"), api.calls.filter { it.third != null }.map { it.first })
        assertFalse(vm.state.value.busy)
    }
    @Test fun newDaemonInstanceAcceptsLowerGenerationAndStaleSameInstanceIsIgnored() = runTest(dispatcher) {
        val api = FakeReceiverApi().apply { generation = 50 }; val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent()
        api.generation = 1; vm.refreshState(); assertEquals(50L, vm.state.value.playback.generation)
        api.instance = "restarted"; vm.refreshState(); assertEquals(1L, vm.state.value.playback.generation)
    }
    @Test fun foregroundPollingStopsInBackgroundAndConnectionFailurePreservesNavigation() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.openModule("provider-0"); runCurrent()
        api.online = false; vm.refreshState(); assertFalse(vm.state.value.connected); assertEquals("Module", vm.state.value.destination)
        api.online = true; vm.setForeground(true); runCurrent(); val count = api.calls.size; advanceTimeBy(2001); runCurrent(); assertTrue(api.calls.size > count)
        vm.setForeground(false); val stopped = api.calls.size; advanceTimeBy(6000); runCurrent(); assertEquals(stopped, api.calls.size)
    }
    @Test fun cancelledBrowseDoesNotOverwriteNewModule() = runTest(dispatcher) {
        val fake = FakeReceiverApi(); val api = object : ReceiverApi by fake {
            override suspend fun get(path: String, query: Map<String, String>): JsonObject { if (path.endsWith("/browse")) delay(1000); return fake.get(path, query) }
        }
        val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.openModule("provider-0"); runCurrent(); vm.openModule("search-first"); advanceTimeBy(1100); runCurrent()
        assertEquals("search-first", vm.state.value.activeModule); assertTrue(vm.state.value.page.items.isEmpty()); assertFalse(vm.state.value.loading)
    }
    @Test fun queuedPlaybackUsesCapturedModuleAndFailedPlaybackRefreshesState() = runTest(dispatcher) {
        val fake = FakeReceiverApi(); val api = object : ReceiverApi by fake {
            override suspend fun post(path: String, body: JsonObject): JsonObject { if (path.endsWith("/play")) delay(100); return fake.post(path, body) }
        }
        val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.openModule("provider-0"); runCurrent()
        vm.play(MediaItem("a", "A", playable = true)); vm.openModule("search-first"); vm.play(MediaItem("b", "B", playable = true)); advanceUntilIdle()
        assertEquals(listOf("/api/modules/provider-0/play", "/api/modules/search-first/play"), fake.calls.filter { it.third != null }.map { it.first })
        assertEquals("search-first", vm.state.value.playback.source)
    }
    @Test fun moduleSettingsAndActionsUsePairingAndPollModuleDeclaredOperation() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.pair("abcdef12"); runCurrent()
        vm.moduleSettings("provider-0"); runCurrent(); val field = vm.state.value.settings.fields.first(); vm.saveField("provider-0", field, JsonPrimitive(false)); runCurrent(); assertEquals(false, vm.state.value.settings.fields.first().value.boolean)
        vm.setForeground(true); vm.moduleAction("provider-0", "connect"); runCurrent(); assertEquals("fixture-code", vm.state.value.actionResult!!.code)
        api.actionApproved = true; advanceTimeBy(2001); runCurrent(); assertTrue(vm.state.value.actionResult!!.authenticated); vm.setForeground(false)
    }
    @Test fun zeroAndThirtyTwoModulesDoNotNeedFixedDestinations() = runTest(dispatcher) {
        val api = FakeReceiverApi(); api.modules.clear(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); assertTrue(vm.state.value.homeModules.isEmpty())
        repeat(32) { api.modules += ModuleDescriptor("module-$it", "Module $it", installed = true, enabled = true, healthy = true) }; vm.refreshModules(); assertEquals(32, vm.state.value.homeModules.size)
    }

    @Test fun exclusiveNativePresentationRequiresLocalShellHandoff() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val m = api.modules.last(); api.modules[api.modules.lastIndex] = m.copy(presentation = Presentation(releaseInput = true, releaseSurface = true))
        val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.nativeApp(m.id, true); runCurrent()
        assertFalse(api.calls.any { it.first.endsWith("/native/start") }); assertTrue(vm.state.value.message!!.contains("HR54 remote"))
        api.nativeModule = m.id; vm.refreshState(); vm.nativeApp(m.id, false); runCurrent(); assertEquals("", vm.state.value.system.nativeModule)
    }

    @Test fun reversePagingAndFolderReturnKeepActualOffsets() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.openModule("provider-0"); runCurrent(); vm.select(vm.state.value.page.items.first()); runCurrent()
        vm.moreItems(); runCurrent(); vm.moreItems(); runCurrent(); assertEquals(2, vm.state.value.page.offset)
        vm.previousPage(); runCurrent(); assertEquals(1, vm.state.value.page.offset)
        vm.select(MediaItem("nested", "Nested", "folder")); runCurrent(); assertTrue(vm.up()); runCurrent(); assertEquals(1, vm.state.value.page.offset)
        vm.previousPage(); runCurrent(); assertEquals(0, vm.state.value.page.offset)
    }
    @Test fun bundledModuleReinstallUpdatesRegistryWithoutReconnect() = runTest(dispatcher) {
        val api = FakeReceiverApi(); api.modules[0] = api.modules[0].copy(core = true, installed = false, enabled = false)
        val vm = ControllerViewModel(apiFactory = { api }); vm.connect("receiver"); runCurrent(); vm.pair("abcdef12"); runCurrent(); vm.manage("provider-0", "reinstall"); runCurrent()
        assertTrue(vm.state.value.module("provider-0")!!.installed); assertTrue(vm.state.value.homeModules.any { it.id == "provider-0" }); assertTrue(vm.state.value.ready)
    }
    @Test fun revokedBearerDuringActionPollClearsPairing() = runTest(dispatcher) {
        val api = FakeReceiverApi(); val tokens = Tokens(); val vm = ControllerViewModel(apiFactory = { api }, tokens = tokens); vm.connect("receiver"); runCurrent(); vm.pair("abcdef12"); runCurrent(); vm.setForeground(true)
        vm.moduleAction("provider-0", "connect"); runCurrent(); api.expectedToken = "b".repeat(64); advanceTimeBy(2001); runCurrent()
        assertFalse(vm.state.value.paired); assertNull(tokens.saved["http://receiver:8130"]); vm.setForeground(false)
    }
}
