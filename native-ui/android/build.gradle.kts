import org.gradle.process.ExecOperations
import javax.inject.Inject

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

    sourceSets["main"].assets.srcDir(
        File(layout.buildDirectory.get().asFile, "generated/nativeUiAssets").absolutePath
    )

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

/**
 * Runs the offline packer. A clean checkout without a firmware dump has no
 * resource tree; the task then writes nothing and the installer reports the
 * missing pack at runtime instead of failing every build.
 */
abstract class PackNativeUiResources : DefaultTask() {
    @get:InputDirectory
    abstract val resources: DirectoryProperty

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @get:Inject
    abstract val execOperations: ExecOperations

    @TaskAction
    fun pack() {
        val root = resources.get().asFile
        if (!File(root, "manifest.json").isFile) {
            logger.warn("native-ui resources are absent; the APK will not carry a resource pack")
            return
        }
        val outputDirectory = output.get().asFile
        outputDirectory.mkdirs()
        val repository = root.parentFile.parentFile
        execOperations.exec {
            workingDir(repository)
            commandLine(
                "python",
                "native-ui/tools/pack_resources.py",
                "--resources", root.absolutePath,
                "--output", File(outputDirectory, "native-ui.zip").absolutePath,
            )
        }
    }
}

val prepareNativeUiPack = tasks.register<PackNativeUiResources>("prepareNativeUiPack") {
    resources.set(rootProject.layout.projectDirectory.dir("native-ui/resources"))
    output.set(layout.buildDirectory.dir("generated/nativeUiAssets"))
}

tasks.named("preBuild").configure {
    dependsOn(prepareNativeUiPack)
}
