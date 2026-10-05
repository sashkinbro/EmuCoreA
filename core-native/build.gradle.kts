// Native PPSSPP core for Android, driven by EmuCoreA's own frontend.
//
// The vendored CMake project in ../core is configured with EMUCOREA_NATIVE=ON:
// Core/GPU/Common are built, PPSSPP's ImGui UI (ppsspp_ui) and its Java app
// (ppsspp_jni) are skipped, and libemucorea_core.so is produced from
// src/main/cpp/native_core.cpp. No libretro, no PPSSPP UI.
plugins {
    alias(libs.plugins.android.library)
}

val emucoreaNativeDir = file("src/main/cpp").absolutePath.replace('\\', '/')

android {
    namespace = "com.sbro.emucorea.core.nativecore"
    compileSdk {
        version = release(37)
    }
    ndkVersion = "29.0.14206865"

    defaultConfig {
        minSdk = 26
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DEMUCOREA_NATIVE=ON",
                    "-DEMUCOREA_NATIVE_DIR=$emucoreaNativeDir",
                    "-DANDROID_STL=c++_shared"
                )
                targets += "emucorea_core"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../core/CMakeLists.txt")
            version = "3.30.5"
        }
    }

    buildTypes {
        debug {
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_C_FLAGS_DEBUG=-O3 -g",
                        "-DCMAKE_CXX_FLAGS_DEBUG=-O3 -g"
                    )
                }
            }
        }
        release {
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG",
                        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG"
                    )
                }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}


