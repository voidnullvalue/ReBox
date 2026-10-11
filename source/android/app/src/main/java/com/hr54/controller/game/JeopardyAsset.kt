package com.hr54.controller.game

/** Explicit bundled-page load state so a missing asset never becomes an endless spinner. */
sealed interface JeopardyAssetState {
    data object Loading : JeopardyAssetState
    data class Ready(val html: String) : JeopardyAssetState
    data class Failed(val reason: String) : JeopardyAssetState
}

/**
 * Reads the bundled reviewed page, turning any failure into an explicit
 * [JeopardyAssetState.Failed] the UI can present with Close/Retry instead of
 * leaving the phone on a spinner it cannot dismiss.
 */
fun loadJeopardyAsset(open: () -> String): JeopardyAssetState =
    try {
        val html = open()
        if (html.isBlank()) JeopardyAssetState.Failed("The bundled Jeopardy controller page is empty.")
        else JeopardyAssetState.Ready(html)
    } catch (e: Exception) {
        JeopardyAssetState.Failed("The bundled Jeopardy controller page could not be read: ${e.message ?: "missing asset"}")
    }
