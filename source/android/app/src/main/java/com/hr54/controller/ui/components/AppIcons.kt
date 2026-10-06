package com.hr54.controller.ui.components

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

/** Only the vectors this controller uses; no whole icon-library payload. */
object AppIcons {
    private fun icon(name: String, mirror: Boolean = false, draw: PathBuilder.() -> Unit) =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f, autoMirror = mirror)
            .apply {
                path(
                    fill = null,
                    stroke = SolidColor(Color.Black),
                    strokeLineWidth = 1.8f,
                    strokeLineCap = StrokeCap.Round,
                    strokeLineJoin = StrokeJoin.Round,
                    pathBuilder = draw,
                )
            }
            .build()

    private fun PathBuilder.box(left: Float, top: Float, right: Float, bottom: Float) {
        moveTo(left, top)
        lineTo(right, top)
        lineTo(right, bottom)
        lineTo(left, bottom)
        close()
    }

    val Home =
        icon("Home") {
            moveTo(3f, 11f)
            lineTo(12f, 3f)
            lineTo(21f, 11f)
            moveTo(5f, 10f)
            lineTo(5f, 21f)
            lineTo(10f, 21f)
            lineTo(10f, 15f)
            lineTo(14f, 15f)
            lineTo(14f, 21f)
            lineTo(19f, 21f)
            lineTo(19f, 10f)
        }
    val Movie =
        icon("Library") {
            box(3f, 5f, 21f, 20f)
            moveTo(3f, 9f)
            lineTo(21f, 9f)
            moveTo(6f, 5f)
            lineTo(8f, 9f)
            moveTo(12f, 5f)
            lineTo(14f, 9f)
            moveTo(18f, 5f)
            lineTo(20f, 9f)
        }
    val LiveTv =
        icon("Live TV") {
            box(3f, 6f, 21f, 20f)
            moveTo(8f, 2f)
            lineTo(12f, 6f)
            lineTo(16f, 2f)
            moveTo(10f, 10f)
            lineTo(15f, 13f)
            lineTo(10f, 16f)
            close()
        }
    val SmartDisplay =
        icon("Smart display") {
            box(2f, 5f, 22f, 19f)
            moveTo(10f, 9f)
            lineTo(16f, 12f)
            lineTo(10f, 15f)
            close()
        }
    val Videocam =
        icon("Camera") {
            box(3f, 6f, 16f, 18f)
            moveTo(16f, 10f)
            lineTo(22f, 6f)
            lineTo(22f, 18f)
            lineTo(16f, 14f)
        }
    val Settings =
        icon("Settings") {
            moveTo(9f, 3f)
            lineTo(15f, 3f)
            lineTo(16f, 6f)
            lineTo(19f, 6f)
            lineTo(22f, 11f)
            lineTo(19f, 14f)
            lineTo(20f, 17f)
            lineTo(15f, 21f)
            lineTo(12f, 19f)
            lineTo(9f, 21f)
            lineTo(4f, 17f)
            lineTo(5f, 14f)
            lineTo(2f, 11f)
            lineTo(5f, 6f)
            lineTo(8f, 6f)
            close()
            moveTo(16f, 12f)
            curveTo(16f, 17.3f, 8f, 17.3f, 8f, 12f)
            curveTo(8f, 6.7f, 16f, 6.7f, 16f, 12f)
        }
    val Mic =
        icon("Dictate text") {
            moveTo(9f, 5f)
            curveTo(9f, 1f, 15f, 1f, 15f, 5f)
            lineTo(15f, 11f)
            curveTo(15f, 15f, 9f, 15f, 9f, 11f)
            close()
            moveTo(6f, 10f)
            curveTo(6f, 19f, 18f, 19f, 18f, 10f)
            moveTo(12f, 17f)
            lineTo(12f, 22f)
            moveTo(8f, 22f)
            lineTo(16f, 22f)
        }
    val PlayArrow =
        icon("Play") {
            moveTo(7f, 4f)
            lineTo(20f, 12f)
            lineTo(7f, 20f)
            close()
        }
    val Pause =
        icon("Pause") {
            box(6f, 4f, 9f, 20f)
            box(15f, 4f, 18f, 20f)
        }
    val Stop = icon("Stop") { box(5f, 5f, 19f, 19f) }
    val Replay30 =
        icon("Back 30 seconds") {
            moveTo(6f, 7f)
            curveTo(20f, -1f, 26f, 19f, 12f, 21f)
            curveTo(5f, 22f, 2f, 17f, 3f, 12f)
            moveTo(6f, 3f)
            lineTo(6f, 8f)
            lineTo(11f, 8f)
            moveTo(10f, 11f)
            lineTo(13f, 11f)
            lineTo(11f, 14f)
            lineTo(13f, 14f)
            lineTo(13f, 17f)
            lineTo(10f, 17f)
        }
    val Forward30 =
        icon("Forward 30 seconds") {
            moveTo(18f, 7f)
            curveTo(4f, -1f, -2f, 19f, 12f, 21f)
            curveTo(19f, 22f, 22f, 17f, 21f, 12f)
            moveTo(18f, 3f)
            lineTo(18f, 8f)
            lineTo(13f, 8f)
            moveTo(10f, 11f)
            lineTo(13f, 11f)
            lineTo(11f, 14f)
            lineTo(13f, 14f)
            lineTo(13f, 17f)
            lineTo(10f, 17f)
        }
    val ArrowBack =
        icon("Back", true) {
            moveTo(20f, 12f)
            lineTo(4f, 12f)
            moveTo(11f, 5f)
            lineTo(4f, 12f)
            lineTo(11f, 19f)
        }
}
