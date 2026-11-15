// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.content.Context
import android.provider.Settings
import com.redvaultsystems.redvaultmessage.data.model.StoryAnimation

/** How an animated word or emoji looks at one moment: the size and lift of its text (a fraction of its height up is negative) and its opacity. */
data class MotionFrame(val scale: Float, val liftOfHeight: Float, val alpha: Float) {
    companion object {
        val STILL = MotionFrame(1f, 0f, 1f)
    }
}

/**
 * The three motions an overlay can have (`anim`: pulse, bounce, fade), as the web draws them with CSS (Stories.module.css: story-pulse 1.4 s,
 * story-bounce 1 s, story-fade 2.2 s, each ease-in-out and repeating): the same key moments and the same easing curve, so they move alike on every
 * app. The curve is a plain function of time so it can be tested without a screen; the screen only asks it for the frame at the moment.
 */
object StoryMotion {
    /** CSS `ease-in-out`: cubic-bezier(0.42, 0, 0.58, 1). */
    private const val X1 = 0.42
    private const val Y1 = 0.0
    private const val X2 = 0.58
    private const val Y2 = 1.0

    private class Stop(val at: Double, val value: Double)

    fun durationMs(anim: StoryAnimation): Long = when (anim) {
        StoryAnimation.Pulse -> 1400
        StoryAnimation.Bounce -> 1000
        StoryAnimation.Fade -> 2200
    }

    /** What the animation does along its length (0 to 1): the value at the start and end is the resting one. */
    private fun stops(anim: StoryAnimation): List<Stop> = when (anim) {
        StoryAnimation.Pulse -> listOf(Stop(0.0, 1.0), Stop(0.5, 1.12), Stop(1.0, 1.0))
        StoryAnimation.Bounce -> listOf(Stop(0.0, 0.0), Stop(0.4, -0.14), Stop(0.6, 0.0), Stop(1.0, 0.0))
        StoryAnimation.Fade -> listOf(Stop(0.0, 1.0), Stop(0.5, 0.35), Stop(1.0, 1.0))
    }

    /** The cubic-bezier easing curve at progress [x] (0 to 1), solved by bisection (the curve is monotonic in x). */
    internal fun ease(x: Double): Double {
        if (x <= 0.0) return 0.0
        if (x >= 1.0) return 1.0
        fun bez(t: Double, p1: Double, p2: Double): Double {
            val u = 1 - t
            return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t
        }
        var lo = 0.0
        var hi = 1.0
        repeat(40) {
            val mid = (lo + hi) / 2
            if (bez(mid, X1, X2) < x) lo = mid else hi = mid
        }
        return bez((lo + hi) / 2, Y1, Y2)
    }

    /** The value of an animation [stops] list at [progress] (0 to 1): eased between the neighbouring stops, as CSS does per keyframe. */
    private fun valueAt(stops: List<Stop>, progress: Double): Double {
        for (i in 0 until stops.size - 1) {
            val a = stops[i]
            val b = stops[i + 1]
            if (progress <= b.at) {
                val span = b.at - a.at
                if (span <= 0.0) return b.value
                return a.value + (b.value - a.value) * ease((progress - a.at) / span)
            }
        }
        return stops.last().value
    }

    /** The frame [elapsedMs] after the animation started; null (no motion) is the resting frame. */
    fun frame(anim: StoryAnimation?, elapsedMs: Long): MotionFrame {
        if (anim == null) return MotionFrame.STILL
        val length = durationMs(anim)
        val progress = (((elapsedMs % length) + length) % length).toDouble() / length
        val v = valueAt(stops(anim), progress).toFloat()
        return when (anim) {
            StoryAnimation.Pulse -> MotionFrame(v, 0f, 1f)
            StoryAnimation.Bounce -> MotionFrame(1f, v, 1f)
            StoryAnimation.Fade -> MotionFrame(1f, 0f, v)
        }
    }

    /** The motion after [anim] when the button is pressed: none, pulse, bounce, fade, and round to none (the web's order). */
    fun next(anim: StoryAnimation?): StoryAnimation? = when (anim) {
        null -> StoryAnimation.Pulse
        StoryAnimation.Pulse -> StoryAnimation.Bounce
        StoryAnimation.Bounce -> StoryAnimation.Fade
        StoryAnimation.Fade -> null
    }

    fun label(anim: StoryAnimation?): String = when (anim) {
        null -> "Still"
        StoryAnimation.Pulse -> "Pulse"
        StoryAnimation.Bounce -> "Bounce"
        StoryAnimation.Fade -> "Fade"
    }

    /** Whether the person asked the phone to remove animations (Settings, Accessibility): then words and emoji stay still, as on the web with reduced motion. */
    fun reduced(context: Context): Boolean =
        Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
}
