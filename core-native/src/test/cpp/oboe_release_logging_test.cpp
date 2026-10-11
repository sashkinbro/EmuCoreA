// Compile with -DOBOE_ENABLE_LOGGING=1, with and without EMUCOREA_RELEASE.
// Includes the pinned Oboe header; only its Android sink is replaced on the host.
#include "OboeDebug.h"

#include <cstdio>

static int logCalls = 0;

int __android_log_print(int, const char *, const char *, ...) {
    ++logCalls;
    return 0;
}

int main() {
    int arguments = 0;
    LOGV("%d", ++arguments);
    LOGD("%d", ++arguments);
    LOGI("%d", ++arguments);
    LOGW("%d", ++arguments);
    LOGE("%d", ++arguments);
    LOGF("%d", ++arguments);
#ifdef EMUCOREA_RELEASE
    const int expected = 0;
#else
    const int expected = 6;
#endif
    if (arguments != expected || logCalls != expected) {
        std::fprintf(stderr, "FAIL: expected=%d arguments=%d logCalls=%d\n", expected, arguments, logCalls);
        return 1;
    }
    std::printf("PASS: Oboe logging arguments=%d logCalls=%d\n", arguments, logCalls);
    return 0;
}
