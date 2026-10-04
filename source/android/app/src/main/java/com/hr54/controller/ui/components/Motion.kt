package com.hr54.controller.ui.components

import androidx.compose.animation.core.*
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.MotionDurationScale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import androidx.compose.material3.MaterialTheme

// Draw-only motion: layout and click handlers never wait for an animation.
@Composable
fun destinationMotion(destination: String): Modifier {
    var entered by remember(destination) { mutableStateOf(false) }
    LaunchedEffect(destination) { entered = true }
    val progress = animateFloatAsState(
        if (entered) 1f else 0f,
        tween(180, easing = FastOutSlowInEasing), label = "Destination entrance",
    )
    val distance = with(LocalDensity.current) { 10.dp.toPx() }
    return Modifier.graphicsLayer {
        alpha = .8f + .2f * progress.value
        translationY = distance * (1f - progress.value)
    }
}

@Composable
fun pressMotion(source: MutableInteractionSource): Modifier {
    val pressed by source.collectIsPressedAsState()
    val scale = animateFloatAsState(
        if (pressed) .975f else 1f,
        spring(dampingRatio = .78f, stiffness = Spring.StiffnessHigh),
        label = "Card touch response",
    )
    return Modifier.graphicsLayer { scaleX = scale.value; scaleY = scale.value }
}

@Composable
fun LoadingSheen(modifier: Modifier = Modifier) {
    val durationScale = rememberCoroutineScope().coroutineContext[MotionDurationScale]
    if (durationScale?.scaleFactor == 0f) {
        Box(modifier.background(MaterialTheme.colorScheme.surfaceVariant))
        return
    }
    val transition = rememberInfiniteTransition(label = "Loading sheen")
    val travel = transition.animateFloat(
        -1f, 2f, infiniteRepeatable(tween(1400, easing = LinearEasing)),
        label = "Sheen position",
    )
    val surface = MaterialTheme.colorScheme.surfaceVariant
    Canvas(modifier) {
        val x = size.width * travel.value
        drawRect(Brush.linearGradient(
            listOf(surface, Color(0xFF252B2D), surface),
            start = Offset(x - size.width * .6f, 0f),
            end = Offset(x + size.width * .6f, size.height),
        ))
    }
}

@Composable
fun VoiceLevel(level: Float) {
    val amplitude = animateFloatAsState(
        level.coerceIn(0f, 1f), tween(80), label = "Microphone level",
    )
    val accent = MaterialTheme.colorScheme.primary
    val muted = MaterialTheme.colorScheme.surfaceContainerHighest
    Canvas(Modifier.fillMaxWidth().height(48.dp)) {
        val count = 29
        val step = size.width / count
        for (i in 0 until count) {
            val envelope = 1f - kotlin.math.abs(i - count / 2f) / (count / 2f)
            val height = 4.dp.toPx() + (size.height - 4.dp.toPx()) * amplitude.value * envelope
            drawRoundRect(
                color = if (amplitude.value > .025f) accent else muted,
                topLeft = Offset(i * step + step * .25f, (size.height - height) / 2f),
                size = Size(step * .5f, height), cornerRadius = CornerRadius(step * .25f),
            )
        }
    }
}
