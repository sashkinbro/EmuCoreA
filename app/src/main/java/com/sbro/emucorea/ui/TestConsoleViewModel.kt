
package com.sbro.emucorea.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.sbro.emucorea.core.CoreRuntime
import com.sbro.emucorea.core.NativeApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class ConsoleState(
    val apiVersionHex: String = "0x0",
    val diagnostics: String = "loading...",
    val smokeResult: String = "not run",
    val cpuTestResult: String = "not run",
    val irqTimerResult: String = "not run",
    val dmaTestResult: String = "not run",
    val gteTestResult: String = "not run",
    val gpuTestResult: String = "not run",
    val spuMdecTestResult: String = "not run",
    val cdromSioTestResult: String = "not run",
    val jitTestResult: String = "not run",
    val biosTestResult: String = "not run",
    val optimizedTestResult: String = "not run",
    val regressionTestResult: String = "not run",
    val finalTestResult: String = "not run",
    val discLoaderResult: String = "not run",
    val asyncDiscResult: String = "not run",
    val savestateFileResult: String = "not run",
    val bootResult: String = "not run",
    val hostThreadResult: String = "not run",
    val isRunning: Boolean = false,
    val hostInfo: String = "",
)

class TestConsoleViewModel : ViewModel() {
    private val _state = MutableStateFlow(ConsoleState())
    val state: StateFlow<ConsoleState> = _state

    init {
        refreshDiagnostics()
    }

    fun refreshDiagnostics() {
        viewModelScope.launch {
            val diagnostics = withContext(Dispatchers.IO) {
                runCatching { CoreRuntime.diagnostics() }.getOrDefault("native core unavailable")
            }
            _state.value = _state.value.copy(
                diagnostics = diagnostics,
                hostInfo = NativeApp.getCoreName().orEmpty(),
                apiVersionHex = "0x00000000"
            )
        }
    }

    fun runSmoke() = runStub("smoke") { state, result -> state.copy(smokeResult = result) }
    fun runCpuTests() = runStub("cpu") { state, result -> state.copy(cpuTestResult = result) }
    fun runIrqTimerTests() = runStub("irq-timer") { state, result -> state.copy(irqTimerResult = result) }
    fun runDmaTests() = runStub("dma") { state, result -> state.copy(dmaTestResult = result) }
    fun runGteTests() = runStub("gte") { state, result -> state.copy(gteTestResult = result) }
    fun runGpuTests() = runStub("gpu") { state, result -> state.copy(gpuTestResult = result) }
    fun runSpuMdecTests() = runStub("spu-mdec") { state, result -> state.copy(spuMdecTestResult = result) }
    fun runCdromSioTests() = runStub("cdrom-sio") { state, result -> state.copy(cdromSioTestResult = result) }
    fun runJitTests() = runStub("jit") { state, result -> state.copy(jitTestResult = result) }
    fun runBiosTests() = runStub("bios") { state, result -> state.copy(biosTestResult = result) }
    fun runOptimizedTests() = runStub("optimized") { state, result -> state.copy(optimizedTestResult = result) }
    fun runRegressionTests() = runStub("regression") { state, result -> state.copy(regressionTestResult = result) }
    fun runFinalTests() = runStub("final") { state, result -> state.copy(finalTestResult = result) }
    fun runDiscLoaderTests() = runStub("disc-loader") { state, result -> state.copy(discLoaderResult = result) }
    fun runAsyncDiscTests() = runStub("async-disc") { state, result -> state.copy(asyncDiscResult = result) }
    fun runSavestateFileTests() = runStub("savestate") { state, result -> state.copy(savestateFileResult = result) }
    fun runBootTests() = runStub("boot") { state, result -> state.copy(bootResult = result) }
    fun runHostThreadTests() = runStub("host-thread") { state, result -> state.copy(hostThreadResult = result) }

    private fun runStub(name: String, apply: (ConsoleState, String) -> ConsoleState) {
        if (_state.value.isRunning) return
        _state.value = _state.value.copy(isRunning = true)
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { unavailable(name) }
            _state.value = apply(_state.value, result).copy(isRunning = false)
        }
    }

    private fun unavailable(name: String): String =
        "PPSSPP core: '$name' self-test is not available for the native core"
}
