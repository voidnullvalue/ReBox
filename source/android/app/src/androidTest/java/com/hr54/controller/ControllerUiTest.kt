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

class ControllerUiTest {
    @get:Rule val compose = createComposeRule()

    @Test
    fun miniBarTransportTouchDoesNotOpenSheet() {
        val playback = androidx.compose.runtime.mutableStateOf(
            Playback(playing = true, source = "jellyfin", transport = Transport(true, true, true, true))
        )
        val commands = mutableListOf<String>()
        var expanded = false
        compose.setContent {
            Hr54Theme {
                NowPlayingMiniBar(playback.value, null, false, { expanded = true }) { command, _ ->
                    commands += command
                    playback.value = playback.value.copy(paused = command == "pause")
                }
            }
        }
        compose.onNodeWithContentDescription("Pause on HR54").performTouchInput { click() }
        compose.onNodeWithContentDescription("Resume on HR54").performTouchInput { click() }
        compose.onNodeWithContentDescription("Stop playback").performTouchInput { click() }
        compose.runOnIdle {
            org.junit.Assert.assertEquals(listOf("pause", "resume", "stop"), commands)
            org.junit.Assert.assertFalse(expanded)
        }
    }

    @Test
    fun expandedTransportDeliversSeekDeltas() {
        val deltas = mutableListOf<Int?>()
        compose.setContent {
            Hr54Theme {
                TransportControls(
                    Playback(playing = true, transport = Transport(true, true, true, true)),
                    false, true,
                ) { command, delta ->
                    org.junit.Assert.assertEquals("seek", command)
                    deltas += delta
                }
            }
        }
        compose.onNodeWithContentDescription("Back 30 seconds").performTouchInput { click() }
        compose.onNodeWithContentDescription("Forward 30 seconds").performTouchInput { click() }
        compose.runOnIdle { org.junit.Assert.assertEquals(listOf(-30, 30), deltas) }
    }

    @Test
    fun selectingSearchResultWithRepeatedSeasonsDoesNotCrash() {
        val series = Item(id = "series", name = "Star Trek", isFolder = true)
        val season = Item(id = "season", name = "Season 1", isFolder = true)
        val state = androidx.compose.runtime.mutableStateOf(
            previewState.copy(receiver = null, items = listOf(series), itemTotal = 1)
        )
        compose.setContent {
            Hr54Theme {
                Surface {
                    JellyfinScreen(state.value, TextFieldValue("star trek"), {}, {}, {}, {
                        state.value = state.value.copy(items = listOf(season, season), itemTotal = 2)
                    }, {})
                }
            }
        }
        compose.onNodeWithText("Star Trek").performClick()
        compose.onAllNodesWithText("Season 1").assertCountEquals(1)
    }

    @Test
    fun homeShowsAvailableSources() {
        compose.setContent {
            Hr54Theme {
                Surface {
                    HomeScreen(
                        previewState.copy(
                            receiver = null,
                            playback = Playback(),
                            capabilities = Capabilities(iptv = true),
                        ),
                        {},
                        { _, _ -> },
                        {},
                    )
                }
            }
        }
        compose.onNodeWithText("Live TV").assertExists()
        compose.onNodeWithText("Jellyfin").assertDoesNotExist()
    }

    @Test
    fun stopOnlySourceHidesPauseAndSeek() {
        compose.setContent {
            Hr54Theme {
                TransportControls(
                    Playback(playing = true, source = "iptv", transport = Transport(stop = true)),
                    false,
                    true,
                    { _, _ -> },
                )
            }
        }
        compose.onNodeWithContentDescription("Stop playback").assertExists()
        compose.onNodeWithContentDescription("Pause on HR54").assertDoesNotExist()
        compose.onNodeWithContentDescription("Back 30 seconds").assertDoesNotExist()
    }

    @Test
    fun unknownDurationHasNoSlider() {
        compose.setContent {
            Hr54Theme {
                NowPlayingContent(previewState.playback, null, null, false, { _, _ -> }, {})
            }
        }
        compose
            .onAllNodes(
                SemanticsMatcher.keyIsDefined(
                    androidx.compose.ui.semantics.SemanticsActions.SetProgress
                )
            )
            .assertCountEquals(0)
    }

    @Test
    fun manualFieldLimitsBytesAndOffersDictation() {
        var value = TextFieldValue()
        compose.setContent {
            Hr54Theme { WhisperTextField(value, { value = it }, "Search YouTube") }
        }
        compose.onNodeWithText("Search YouTube").performTextInput("😀".repeat(20))
        org.junit.Assert.assertEquals(64, value.text.toByteArray().size)
        compose.onNodeWithContentDescription("Dictate text").assertExists()
    }

    @Test
    fun unavailableCameraExplainsReason() {
        compose.setContent { Hr54Theme { CamerasScreen(previewState, {}) } }
        compose.onNodeWithText("H.264 restream unavailable").assertExists()
        compose.onNodeWithText("Front porch").assertExists()
    }
}
