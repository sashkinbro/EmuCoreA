// SPDX-License-Identifier: GPL-2.0-or-later
plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.sbro.emucorea.xmb.preview"
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.sbro.emucorea.xmb.preview"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.1"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(project(":native-ui:android"))
}
