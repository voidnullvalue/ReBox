package com.hr54.controller

import com.hr54.controller.data.api.*
import com.hr54.controller.data.model.*
import kotlinx.coroutines.*
import kotlinx.coroutines.test.*
import kotlinx.serialization.json.JsonObject
import org.junit.*
import org.junit.Assert.*

@OptIn(ExperimentalCoroutinesApi::class)
class ViewModelTest {
    private val dispatcher = StandardTestDispatcher()

    @Test
    fun failedResumeRefreshesDeadPlaybackWithoutLosingError() = runTest(dispatcher) {
        val fake = FakeReceiverApi().apply { playing = true; source = "jellyfin"; paused = true }
        val api = object : ReceiverApi {
            override suspend fun get(path: String, query: Map<String, String>) = fake.get(path, query)
            override suspend fun post(path: String, body: JsonObject): JsonObject {
                fake.playing = false
                fake.source = null
                throw ReceiverFailure("Jellyfin stream failed before playback became ready", 502)
            }
        }
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        vm.transport("resume")
        runCurrent()
        assertFalse(vm.state.value.playback.playing)
        assertFalse(vm.state.value.playback.transport.stop)
        assertEquals("Jellyfin stream failed before playback became ready", vm.state.value.message)
        assertFalse(vm.state.value.busy)
    }

    @Before
    fun setup() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun teardown() {
        Dispatchers.resetMain()
    }

    @Test
    fun connectionAndDisconnectionPreserveNavigation() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            assertTrue(vm.state.value.ready)
            vm.destination("Cameras")
            runCurrent()
            api.online = false
            vm.refreshState()
            assertFalse(vm.state.value.connected)
            assertEquals("Cameras", vm.state.value.destination)
            api.online = true
            vm.refreshState()
            assertTrue(vm.state.value.connected)
        }

    @Test
    fun failedSetupStaysInSetup() =
        runTest(dispatcher) {
            val api = FakeReceiverApi().apply { online = false }
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            assertFalse(vm.state.value.ready)
            assertNotNull(vm.state.value.error)
            assertFalse(vm.state.value.loading)
        }

    @Test
    fun jellyfinFolderBackRestoresCachedItems() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.destination("Jellyfin")
            runCurrent()
            assertEquals("Movies", vm.state.value.items.first().name)
            vm.folder(vm.state.value.items.first())
            runCurrent()
            vm.folder(vm.state.value.items.last())
            runCurrent()
            assertTrue(vm.up())
            assertEquals("Alien", vm.state.value.items.first().name)
            assertTrue(vm.up())
            assertEquals("Movies", vm.state.value.items.first().name)
            assertFalse(vm.up())
        }

    @Test
    fun pagingUsesActualCount() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.destination("Live TV")
            runCurrent()
            vm.loadChannels(more = true)
            runCurrent()
            assertEquals(2, vm.state.value.channels.size)
            assertEquals("1", api.calls.last { it.first == "/api/iptv/channels" }.second["offset"])
        }

    @Test
    fun youtubePagingAndExplicitSearch() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.destination("YouTube")
            runCurrent()
            assertFalse(api.calls.any { it.first == "/api/youtube/search" })
            vm.searchYoutube("music")
            runCurrent()
            vm.searchYoutube(more = true)
            runCurrent()
            assertEquals(1, vm.state.value.videoPage)
            assertEquals(2, vm.state.value.videos.size)
            assertFalse(vm.state.value.videoMore)
        }

    @Test
    fun unsupportedTransportNeverPosted() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.transport("pause")
            runCurrent()
            assertFalse(api.calls.any { it.first == "/api/playback/pause" })
            vm.play(Channel("ch-a", "News"))
            runCurrent()
            vm.transport("seek", delta = 30)
            runCurrent()
            assertFalse(api.calls.any { it.first == "/api/playback/seek" })
            vm.transport("stop")
            runCurrent()
            assertFalse(vm.state.value.playback.playing)
        }

    @Test
    fun playUsesReturnToTv() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.play(Item("alien", "Alien", runtime = 70200000000, playable = true))
            runCurrent()
            assertEquals(
                "true",
                api.calls.last { it.first == "/api/play" }.third?.get("returnToTv").toString(),
            )
            assertEquals(7020.0, vm.state.value.knownDurations["alien"]!!, 0.0)
        }

    @Test
    fun foregroundPollingStopsInBackground() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.setForeground(true)
            runCurrent()
            val before = api.calls.size
            advanceTimeBy(2001)
            runCurrent()
            assertTrue(api.calls.size > before)
            vm.setForeground(false)
            val stopped = api.calls.size
            advanceTimeBy(6000)
            runCurrent()
            assertEquals(stopped, api.calls.size)
        }

    @Test
    fun youtubeConflictAutomaticallyStopsAndRetries() = runTest(dispatcher) {
        val api = FakeReceiverApi().apply { playing = true; source = "jellyfin" }
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        vm.searchYoutube("music")
        runCurrent()
        assertFalse(api.playing)
        assertEquals(1, vm.state.value.videos.size)
        assertNull(vm.state.value.error)
        val stop = api.calls.indexOfFirst { it.first == "/api/playback/stop" }
        assertEquals("/api/state", api.calls[stop + 1].first)
        assertEquals("/api/youtube/search", api.calls[stop + 2].first)
    }

    @Test
    fun everySourceSwitchStopsBeforeStartingSelectedContent() = runTest(dispatcher) {
        val api = FakeReceiverApi()
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        for (source in listOf("iptv", "jellyfin", "youtube", "frigate")) {
            for (target in listOf("iptv", "jellyfin", "youtube", "frigate")) {
                api.playing = true
                api.source = source
                api.calls.clear()
                when (target) {
                    "iptv" -> vm.play(Channel("news", "News"))
                    "jellyfin" -> vm.play(Item("alien", "Alien", playable = true))
                    "youtube" -> vm.play(Video("jNQXAC9IVRw", "Zoo"))
                    "frigate" -> vm.play(Camera("driveway", "Driveway", playable = true))
                }
                runCurrent()
                assertEquals(target, api.source)
                val posts = api.calls.filter { it.third != null }.map { it.first }
                assertEquals("/api/playback/stop", posts.first())
                assertEquals(2, posts.size)
                assertNull(vm.state.value.message)
            }
        }
    }

    @Test
    fun selectionDuringInFlightPlaybackIsNotDropped() = runTest(dispatcher) {
        val fake = FakeReceiverApi()
        val api = object : ReceiverApi {
            override suspend fun get(path: String, query: Map<String, String>) = fake.get(path, query)
            override suspend fun post(path: String, body: JsonObject): JsonObject {
                if (path == "/api/play") delay(1000)
                return fake.post(path, body)
            }
        }
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        vm.play(Item("alien", "Alien", playable = true))
        runCurrent()
        assertTrue(vm.state.value.busy)
        vm.play(Channel("news", "News"))
        advanceUntilIdle()
        assertEquals("iptv", fake.source)
        assertEquals(listOf("/api/play", "/api/playback/stop", "/api/iptv/play"),
            fake.calls.filter { it.third != null }.map { it.first })
    }

    @Test
    fun failedStopDoesNotStartAnotherStream() = runTest(dispatcher) {
        val fake = FakeReceiverApi().apply { playing = true; source = "iptv" }
        val api = object : ReceiverApi {
            override suspend fun get(path: String, query: Map<String, String>) = fake.get(path, query)
            override suspend fun post(path: String, body: JsonObject): JsonObject {
                if (path == "/api/playback/stop") throw ReceiverFailure("Stop failed", 502)
                return fake.post(path, body)
            }
        }
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        vm.play(Item("alien", "Alien", playable = true))
        runCurrent()
        assertEquals("iptv", fake.source)
        assertFalse(fake.calls.any { it.first == "/api/play" })
        assertEquals("Stop failed", vm.state.value.message)
        assertFalse(vm.state.value.busy)
    }

    @Test
    fun rapidTransportCommandsAreSerializedAndUseFreshState() = runTest(dispatcher) {
        val api = FakeReceiverApi()
        val vm = ControllerViewModel(apiFactory = { api })
        vm.connect("receiver")
        runCurrent()
        api.playing = true
        api.source = "jellyfin"
        vm.transport("pause")
        vm.transport("resume")
        vm.transport("seek", delta = 30)
        runCurrent()
        assertEquals(listOf("/api/playback/pause", "/api/playback/resume", "/api/playback/seek"),
            api.calls.filter { it.third != null }.map { it.first })
        assertFalse(vm.state.value.playback.paused)
        assertFalse(vm.state.value.busy)
    }

    @Test
    fun quickConnectPollsAndOpensLibrary() =
        runTest(dispatcher) {
            val api = FakeReceiverApi().apply { authenticated = false }
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.setForeground(true)
            vm.destination("Jellyfin")
            runCurrent()
            vm.startAuth()
            runCurrent()
            assertEquals("482 719", vm.state.value.auth.code)
            api.authenticated = true
            advanceTimeBy(2501)
            runCurrent()
            assertTrue(vm.state.value.auth.authenticated)
            assertEquals("Movies", vm.state.value.items.first().name)
            vm.setForeground(false)
        }

    @Test
    fun jellyfinSearchBackReturnsToFolder() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.destination("Jellyfin")
            runCurrent()
            vm.folder(vm.state.value.items.first())
            runCurrent()
            val folders = vm.state.value.folders
            vm.searchJellyfin("alien")
            runCurrent()
            assertTrue(vm.up())
            runCurrent()
            assertEquals(folders, vm.state.value.folders)
            assertEquals("", vm.state.value.jellyQuery)
        }

    @Test
    fun searchFolderBackRestoresSearchThenLibraries() =
        runTest(dispatcher) {
            val api = FakeReceiverApi()
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.destination("Jellyfin")
            runCurrent()
            vm.searchJellyfin("science")
            runCurrent()
            vm.folder(vm.state.value.items.last())
            runCurrent()
            assertTrue(vm.up())
            assertEquals("science", vm.state.value.jellyQuery)
            assertTrue(vm.up())
            runCurrent()
            assertEquals("Movies", vm.state.value.items.first().name)
            assertFalse(vm.up())
        }

    @Test
    fun youtubeNeverRequestsPastServerPageLimit() =
        runTest(dispatcher) {
            val fake = FakeReceiverApi()
            val api =
                object : ReceiverApi {
                    override suspend fun get(path: String, query: Map<String, String>): JsonObject {
                        val response = fake.get(path, query)
                        return if (path == "/api/youtube/search")
                            JsonObject(
                                response +
                                    ("hasMore" to kotlinx.serialization.json.JsonPrimitive(true))
                            )
                        else response
                    }

                    override suspend fun post(path: String, body: JsonObject) =
                        fake.post(path, body)
                }
            val vm = ControllerViewModel(apiFactory = { api })
            vm.connect("receiver")
            runCurrent()
            vm.searchYoutube("music")
            runCurrent()
            repeat(100) {
                vm.searchYoutube(more = true)
                runCurrent()
            }
            assertEquals(100, vm.state.value.videoPage)
            assertFalse(vm.state.value.videoMore)
            vm.searchYoutube(more = true)
            runCurrent()
            assertEquals(101, fake.calls.count { it.first == "/api/youtube/search" })
        }
}
