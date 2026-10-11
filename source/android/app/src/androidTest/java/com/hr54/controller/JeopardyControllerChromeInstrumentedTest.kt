package com.hr54.controller

import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hr54.controller.ui.ConnectionHelp
import com.hr54.controller.ui.ControllerAssetError
import com.hr54.controller.ui.components.Hr54Theme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Device/emulator coverage for the controller chrome: the retry/close "modals" and
 * the safe-insets surface. It compiles against the exact [safeDrawingPadding]
 * contract used by [com.hr54.controller.ui.JeopardyControllerScreen] so the BUZZ
 * and Close stay reachable above the system bars and the IME.
 */
@RunWith(AndroidJUnit4::class)
class JeopardyControllerChromeInstrumentedTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun controllerHelpExposesReasonUrlRetryAndClose() {
        var retried = 0; var closed = 0
        compose.setContent {
            Hr54Theme { ConnectionHelp("http://127.0.0.1:8135/", "The receiver game answered HTTP 503 on its LAN port", { retried++ }, { closed++ }) }
        }
        compose.onNodeWithText("Can't reach the game").assertIsDisplayed()
        compose.onNodeWithText("The receiver game answered HTTP 503 on its LAN port").assertIsDisplayed()
        compose.onNodeWithText("http://127.0.0.1:8135/").assertIsDisplayed()
        compose.onNodeWithText("Retry").assertIsDisplayed().performClick()
        compose.onNodeWithText("Close").assertIsDisplayed().performClick()
        compose.runOnIdle { assertEquals(1, retried); assertEquals(1, closed) }
    }

    @Test fun assetErrorModalKeepsTheControllerDismissableAboveSafeInsets() {
        var closed = 0; var retried = 0
        compose.setContent {
            Hr54Theme { ControllerAssetError("The bundled Jeopardy controller page could not be read", { retried++ }, { closed++ }, Modifier.safeDrawingPadding()) }
        }
        compose.onNodeWithText("Can't open the phone controller").assertIsDisplayed()
        compose.onNodeWithText("Retry").assertIsDisplayed()
        compose.onNodeWithText("Close").assertIsDisplayed().performClick()
        compose.runOnIdle { assertEquals(1, closed) }
    }
}
