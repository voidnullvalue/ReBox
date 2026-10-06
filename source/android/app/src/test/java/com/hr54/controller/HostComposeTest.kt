package com.hr54.controller

import androidx.compose.material3.Surface
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.text.input.TextFieldValue
import com.hr54.controller.data.model.*
import com.hr54.controller.ui.*
import com.hr54.controller.ui.components.*
import org.junit.Rule
import org.junit.Test

@org.junit.runner.RunWith(org.robolectric.RobolectricTestRunner::class)
@org.robolectric.annotation.Config(sdk = [36])
class HostComposeTest {
    @get:Rule val compose = createComposeRule()
    @Test fun homeShowsRuntimeModuleNameAndDisabledEntriesStayOut() {
        compose.setContent { Hr54Theme { Surface { HomeScreen(previewState.copy(playback = Playback(), modules = listOf(
            ModuleDescriptor("compiled-later", "Late module", installed = true, enabled = true, healthy = true),
            ModuleDescriptor("hidden-disabled", "Disabled module", installed = true, enabled = false))), {}, { _, _ -> }, {}) } } }
        compose.onNodeWithText("Late module").assertExists(); compose.onNodeWithText("Disabled module").assertDoesNotExist()
    }
    @Test fun zeroModulesShowsManagerEntryExplanation() {
        compose.setContent { Hr54Theme { HomeScreen(ControllerState(), {}, { _, _ -> }, {}) } }
        compose.onNodeWithText("No enabled modules").assertExists()
    }
    @Test fun repeatedOpaqueResultsRenderOnceAndUnavailableItemExplainsReason() {
        val item = MediaItem("repeat", "Repeated title", description = "Stream unavailable")
        compose.setContent { Hr54Theme { ModuleScreen(previewState.copy(page = MediaPage(listOf(item, item))), TextFieldValue(), {}, {}, {}, {}, {}, {}, {}) } }
        compose.onAllNodesWithText("Repeated title").assertCountEquals(1); compose.onNodeWithText("Stream unavailable").assertExists()
    }
    @Test fun miniBarTransportTouchDoesNotOpenSheet() {
        val playback = androidx.compose.runtime.mutableStateOf(Playback(playing = true, source = "unknown", transport = Transport(true, true, true, true)))
        val commands = mutableListOf<String>(); var expanded = false
        compose.setContent { Hr54Theme { NowPlayingMiniBar(playback.value, null, false, { expanded = true }) { command, _ -> commands += command; playback.value = playback.value.copy(paused = command == "pause") } } }
        compose.onNodeWithContentDescription("Pause on HR54").performTouchInput { click() }; compose.onNodeWithContentDescription("Resume on HR54").performTouchInput { click() }; compose.onNodeWithContentDescription("Stop playback").performTouchInput { click() }
        compose.runOnIdle { org.junit.Assert.assertEquals(listOf("pause", "resume", "stop"), commands); org.junit.Assert.assertFalse(expanded) }
    }
    @Test fun expandedTransportDeliversSeekDeltas() {
        val deltas = mutableListOf<Int?>()
        compose.setContent { Hr54Theme { TransportControls(Playback(playing = true, transport = Transport(true, true, true, true)), false, true) { command, delta -> org.junit.Assert.assertEquals("seek", command); deltas += delta } } }
        compose.onNodeWithContentDescription("Back 30 seconds").performTouchInput { click() }; compose.onNodeWithContentDescription("Forward 30 seconds").performTouchInput { click() }
        compose.runOnIdle { org.junit.Assert.assertEquals(listOf(-30, 30), deltas) }
    }
    @Test fun stopOnlyModuleHidesPauseAndSeek() {
        compose.setContent { Hr54Theme { TransportControls(Playback(playing = true, source = "unknown", transport = Transport(stop = true)), false, true, { _, _ -> }) } }
        compose.onNodeWithContentDescription("Stop playback").assertExists(); compose.onNodeWithContentDescription("Pause on HR54").assertDoesNotExist(); compose.onNodeWithContentDescription("Back 30 seconds").assertDoesNotExist()
    }
    @Test fun unknownDurationHasNoSlider() {
        compose.setContent { Hr54Theme { NowPlayingContent(previewState.playback, null, null, false, { _, _ -> }, {}) } }
        compose.onAllNodes(SemanticsMatcher.keyIsDefined(androidx.compose.ui.semantics.SemanticsActions.SetProgress)).assertCountEquals(0)
    }
    @Test fun searchStillLimitsBytesAndOffersLocalDictation() {
        var value = TextFieldValue()
        compose.setContent { Hr54Theme { WhisperTextField(value, { value = it }, "Search module") } }
        compose.onNodeWithText("Search module").performTextInput("😀".repeat(20)); org.junit.Assert.assertEquals(64, value.text.toByteArray().size); compose.onNodeWithContentDescription("Dictate text").assertExists()
    }
    @Test fun unpairedManagerRequiresCodeAndDisablesInstall() {
        compose.setContent { Hr54Theme { ModuleManager(previewState, { _, _ -> }, {}, {}, {}, {}) } }
        compose.onNodeWithText("Pair management").assertIsNotEnabled(); compose.onNode(hasScrollAction()).performScrollToNode(hasText("Install module")); compose.onNodeWithText("Install module").assertIsNotEnabled()
    }
}
