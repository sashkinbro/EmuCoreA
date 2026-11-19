#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <android/log.h>
#include "compat.h"
#include "mem.h"

void av_log(int level, const char *fmt, ...) {
    char buffer[512];
    va_list vl;
    va_start(vl, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    va_end(vl);

    int android_level = ANDROID_LOG_DEBUG;
    if (level <= AV_LOG_ERROR) {
        android_level = ANDROID_LOG_ERROR;
    } else if (level <= AV_LOG_WARNING) {
        android_level = ANDROID_LOG_WARN;
    } else if (level <= AV_LOG_INFO) {
        android_level = ANDROID_LOG_INFO;
    }

    __android_log_print(android_level, "EmuCoreA_AT3", "%s", buffer);
}

void *av_malloc(size_t size) {
    if (!size) size = 1;
    void *ptr = nullptr;
    if (posix_memalign(&ptr, 32, size) != 0) {
        return nullptr;
    }
    return ptr;
}

void av_free(void *ptr) {
    free(ptr);
}

void av_freep(void *arg) {
    void *val;
    memcpy(&val, arg, sizeof(val));
    memset(arg, 0, sizeof(val));
    av_free(val);
}

void *av_mallocz(size_t size) {
    void *ptr = av_malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}
