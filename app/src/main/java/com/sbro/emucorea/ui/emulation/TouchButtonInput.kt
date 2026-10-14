package com.sbro.emucorea.ui.emulation

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect

internal fun touchButtonTargetsAt(bounds: Map<String, Rect>, x: Float, y: Float): Set<String> {
    val position = Offset(x, y)
    return bounds.filterValues { it.contains(position) }.keys
}

internal class TouchButtonPressOwners {
    private val counts = mutableMapOf<String, Int>()

    fun handler(actionId: String, dispatch: (Boolean) -> Unit): (Boolean) -> Unit {
        var active = false
        return handler@{ pressed ->
            if (active == pressed) return@handler
            active = pressed
            val previousCount = counts[actionId] ?: 0
            val count = previousCount + if (pressed) 1 else -1
            if (count == 0) counts.remove(actionId) else counts[actionId] = count
            if (previousCount == 0 || count == 0) dispatch(pressed)
        }
    }
}

/** Tracks each finger independently so a button stays pressed until its last finger leaves. */
internal class TouchButtonInput(
    private val hitTargets: (Float, Float) -> Set<String>,
    private val isTapToHold: (String) -> Boolean,
    private val onPressChange: (String, Boolean) -> Unit,
    private val onClick: (String) -> Unit,
    private val onHaptic: (Set<String>, Boolean) -> Unit
) {
    private val activeTargets = mutableMapOf<Long, Set<String>>()
    private val downTargets = mutableMapOf<Long, Set<String>>()
    private val latchedTargets = mutableSetOf<String>()

    val pressedTargets: Set<String>
        get() = activeTargets.values.flatten().toSet() + latchedTargets

    fun down(pointerId: Long, x: Float, y: Float): Boolean {
        val targets = hitTargets(x, y)
        if (targets.isEmpty()) return false
        downTargets[pointerId] = targets
        updateTargets(pointerId, targets)
        return true
    }

    fun move(pointerId: Long, x: Float, y: Float): Boolean {
        if (pointerId !in downTargets) return false
        updateTargets(pointerId, hitTargets(x, y))
        return true
    }

    fun up(pointerId: Long, x: Float, y: Float): Boolean {
        val initialTargets = downTargets.remove(pointerId) ?: return false
        val releasedOn = hitTargets(x, y)
        updateTargets(pointerId, emptySet())
        (initialTargets intersect releasedOn).forEach { target ->
            if (isTapToHold(target)) {
                val pressed = if (latchedTargets.remove(target)) false else {
                    latchedTargets.add(target)
                    true
                }
                onPressChange(target, pressed)
            } else {
                onClick(target)
            }
        }
        return true
    }

    fun cancelPointers() {
        val released = activeTargets.values.flatten().toSet()
        activeTargets.clear()
        downTargets.clear()
        released.filterNot(isTapToHold).forEach { onPressChange(it, false) }
    }

    fun clear() {
        latchedTargets.toList().forEach { onPressChange(it, false) }
        latchedTargets.clear()
        cancelPointers()
    }

    private fun updateTargets(pointerId: Long, targets: Set<String>) {
        if (activeTargets[pointerId].orEmpty() == targets) return
        val previouslyActive = activeTargets.values.flatten().toSet()
        if (targets.isEmpty()) activeTargets.remove(pointerId) else activeTargets[pointerId] = targets
        val currentlyActive = activeTargets.values.flatten().toSet()
        val released = previouslyActive - currentlyActive
        val pressed = currentlyActive - previouslyActive
        released.filterNot(isTapToHold).forEach { onPressChange(it, false) }
        if (targets.isEmpty() && released.isNotEmpty()) onHaptic(released, false)
        if (pressed.isNotEmpty()) onHaptic(pressed, true)
        pressed.filterNot(isTapToHold).forEach { onPressChange(it, true) }
    }
}
