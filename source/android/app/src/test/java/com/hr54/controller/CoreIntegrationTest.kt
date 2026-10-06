package com.hr54.controller

import androidx.lifecycle.ViewModelStore
import com.hr54.controller.data.model.MediaItem
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.util.concurrent.Executors

@OptIn(ExperimentalCoroutinesApi::class)
class CoreIntegrationTest {
    @Test fun compiledAndroidClientPairsInstallsUnknownPackageBrowsesPlaysAndRemovesThroughRealCore() = runBlocking {
        val directory = File(requireNotNull(System.getProperty("hr54.repoRoot")))
        val fixture = ProcessBuilder("python3", File(directory, "source/android/tools/module_fixture.py").path)
            .redirectError(ProcessBuilder.Redirect.INHERIT).start()
        val dispatcher = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        Dispatchers.setMain(dispatcher)
        val store = ViewModelStore()
        try {
            val line = withContext(Dispatchers.IO) { fixture.inputStream.bufferedReader().readLine() } ?: error("Core fixture did not start")
            val config = Json.parseToJsonElement(line).jsonObject
            val id = config.getValue("id").jsonPrimitive.content
            val vm = ControllerViewModel(); store.put("controller", vm)
            suspend fun wait(predicate: (ControllerState) -> Boolean) = withTimeout(15000) { vm.state.first(predicate) }
            vm.connect(config.getValue("receiver").jsonPrimitive.content); wait { it.ready && !it.loading }
            assertTrue(vm.state.value.modules.isEmpty())
            vm.pair(config.getValue("code").jsonPrimitive.content); wait { it.paired && !it.busy }
            vm.install(config.getValue("package").jsonPrimitive.content); wait { !it.busy && it.homeModules.any { module -> module.id == id && module.healthy } }
            vm.openModule(id); wait { !it.loading && it.page.items.isNotEmpty() }; vm.select(vm.state.value.page.items.first()); wait { !it.loading && it.page.items.firstOrNull()?.id == "item-2" }
            vm.search("foo"); wait { !it.loading && it.page.items.firstOrNull()?.id == "item-1" }
            vm.play(vm.state.value.page.items.first()); wait { !it.busy && it.playback.playing }; assertEquals(id, vm.state.value.playback.source)
            vm.transport("stop"); wait { !it.busy && !it.playback.playing }
            vm.manage(id, "disable"); wait { !it.busy && it.homeModules.none { module -> module.id == id } }
            vm.manage(id, "enable"); wait { !it.busy && it.homeModules.any { module -> module.id == id && module.healthy } }
            vm.manage(id, "uninstall"); wait { !it.busy && it.modules.none { module -> module.id == id } }
            assertTrue(vm.state.value.ready)
        } finally {
            withContext(dispatcher) { store.clear() }
            Dispatchers.resetMain(); dispatcher.close()
            fixture.outputStream.write("stop\n".toByteArray()); fixture.outputStream.flush()
            withContext(Dispatchers.IO) { if (!fixture.waitFor(10, java.util.concurrent.TimeUnit.SECONDS)) fixture.destroyForcibly() }
        }
    }
}
