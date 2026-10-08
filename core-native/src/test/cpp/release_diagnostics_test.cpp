// Release diagnostics must not evaluate log arguments or debug assertions.
#include "Common/Log.h"

#include <cstdio>

static bool logEnabled = true;
bool *g_bLogEnabledSetting = &logEnabled;
LogChannel g_log[static_cast<size_t>(Log::NUMBER_OF_LOGS)];
static int logCalls = 0;

void GenericLog(Log, LogLevel, const char *, int, const char *, ...) {
    ++logCalls;
}

int main() {
    int arguments = 0;
    int checks = 0;
    NOTICE_LOG(Log::System, "%d", ++arguments);
    ERROR_LOG(Log::System, "%d", ++arguments);
    WARN_LOG(Log::System, "%d", ++arguments);
    INFO_LOG(Log::System, "%d", ++arguments);
    DEBUG_LOG(Log::System, "%d", ++arguments);
    VERBOSE_LOG(Log::System, "%d", ++arguments);
    _dbg_assert_or_log_(++checks > 0);
    _dbg_assert_msg_or_log_(++checks > 0, Log::System, "diagnostic");
    _dbg_assert_(++checks > 0);
    _dbg_assert_msg_(++checks > 0, "diagnostic");
#ifdef EMUCOREA_RELEASE
    const bool passed = arguments == 0 && checks == 0 && logCalls == 0 &&
        !GenericLogEnabled(Log::System, LogLevel::LERROR);
#else
    // Control build: enabled PPSSPP logs and conditional debug checks execute.
    const bool passed = arguments == 5 && checks == 2 && logCalls == 5 &&
        GenericLogEnabled(Log::System, LogLevel::LERROR);
#endif
    if (!passed) {
        std::fprintf(stderr, "arguments=%d checks=%d logCalls=%d\n", arguments, checks, logCalls);
        return 1;
    }
    std::puts("Release diagnostic evaluation / enabled control: PASS");
    return 0;
}
