// SPDX-License-Identifier: GPL-2.0-or-later
plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.sbro.emucorea.xmb"
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }
    ndkVersion = "29.0.14206865"

    defaultConfig {
        minSdk = 26
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.5"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true
    }
}

// AGP 9 applies Kotlin itself; only the JVM target needs to match Java.
kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

// No firmware-derived file is packaged. The XMB resource tree is built on the
// device from the user's own firmware (see XmbResources and docs/integration.md).
