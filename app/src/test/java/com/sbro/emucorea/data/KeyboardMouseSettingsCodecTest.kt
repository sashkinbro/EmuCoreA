package com.sbro.emucorea.data

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class KeyboardMouseSettingsCodecTest {
    @Test
    fun settingsRoundTripThroughThePerGameCodec() {
        val settings = KeyboardMouseSettings(
            keyboardEnabled = false,
            mouseEnabled = true,
            keyboardTargetPad = 0,
            mouseTargetPad = 0,
            keyboardBindingsByPad = mapOf(0 to mapOf("cross" to 39, "select" to -1)),
            mouseBindingsByPad = mapOf(0 to mapOf("circle" to 1)),
            mouseAimMode = AppPreferences.MOUSE_AIM_RIGHT_STICK,
            mouseSensitivity = 140,
            mouseVerticalSensitivity = 90,
            mouseSmoothing = 35,
            mouseAimDeadzone = 7,
            mouseInvertX = true,
            mouseInvertY = true,
            mouseCaptureCursor = false,
            mouseLeftHanded = true,
            mouseWheelMode = AppPreferences.MOUSE_WHEEL_DPAD,
            mouseWheelPulseMs = 120
        )

        val decoded = KeyboardMouseSettingsCodec.decode(
            KeyboardMouseSettingsCodec.encode(settings)
        )

        assertEquals(settings, decoded)
    }

    @Test
    fun codecClampsOutOfRangeValuesAndHandlesMissingJson() {
        assertNull(KeyboardMouseSettingsCodec.decode(null))

        val decoded = KeyboardMouseSettingsCodec.decode(
            JSONObject()
                .put("keyboardTargetPad", 9)
                .put("mouseTargetPad", -4)
                .put("mouseAimMode", 99)
                .put("mouseSensitivity", 9999)
                .put("mouseVerticalSensitivity", 1)
                .put("mouseSmoothing", 1000)
                .put("mouseAimDeadzone", 999)
                .put("mouseWheelMode", 99)
                .put("mouseWheelPulseMs", 1)
        )

        requireNotNull(decoded)
        assertEquals(0, decoded.keyboardTargetPad)
        assertEquals(0, decoded.mouseTargetPad)
        assertEquals(AppPreferences.MOUSE_AIM_LEFT_STICK, decoded.mouseAimMode)
        assertEquals(AppPreferences.MOUSE_SENSITIVITY_MAX, decoded.mouseSensitivity)
        assertEquals(
            AppPreferences.MOUSE_VERTICAL_SENSITIVITY_MIN,
            decoded.mouseVerticalSensitivity
        )
        assertEquals(AppPreferences.MOUSE_SMOOTHING_MAX, decoded.mouseSmoothing)
        assertEquals(AppPreferences.MOUSE_DEADZONE_MAX, decoded.mouseAimDeadzone)
        assertEquals(AppPreferences.MOUSE_WHEEL_SHOULDERS, decoded.mouseWheelMode)
        assertEquals(AppPreferences.MOUSE_WHEEL_PULSE_MIN_MS, decoded.mouseWheelPulseMs)
    }
}
