// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

/** JNI inspection available only to debug builds and device tests. */
object NativeCoreDiagnostics {
    /** Boot/audio/CPU/FPS/software/textures/display state, in that order. */
    external fun nativeGetRuntimeState(): IntArray
}
