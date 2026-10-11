package com.hr54.controller

import com.hr54.controller.game.JeopardyAssetState
import com.hr54.controller.game.loadJeopardyAsset
import org.junit.Assert.*
import org.junit.Test

class JeopardyAssetTest {
    @Test fun failedLoaderSurfacesAnExplicitErrorInsteadOfNull() {
        val failed = loadJeopardyAsset { throw java.io.FileNotFoundException("jeopardy/index.html (No such file or directory)") }
        assertTrue(failed is JeopardyAssetState.Failed)
        assertTrue((failed as JeopardyAssetState.Failed).reason.contains("jeopardy/index.html"))
    }

    @Test fun blankOrEmptyAssetIsAnExplicitError() {
        assertTrue(loadJeopardyAsset { "" } is JeopardyAssetState.Failed)
        assertTrue(loadJeopardyAsset { "   \n  " } is JeopardyAssetState.Failed)
    }

    @Test fun presentLoaderReturnsReadyHtml() {
        assertEquals(JeopardyAssetState.Ready("<html>ok</html>"), loadJeopardyAsset { "<html>ok</html>" })
    }
}
