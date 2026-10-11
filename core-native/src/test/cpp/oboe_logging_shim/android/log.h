#pragma once

// Only for the host test of Oboe's real logging header.
enum {
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
    ANDROID_LOG_FATAL = 7,
};

int __android_log_print(int priority, const char *tag, const char *format, ...);
