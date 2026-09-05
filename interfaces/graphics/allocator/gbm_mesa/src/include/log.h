/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#ifndef GRALLOC_GBM_MESA_LOG_H_
#define GRALLOC_GBM_MESA_LOG_H_

#ifndef LOG_TAG
#define LOG_TAG "libgralloc_gm"
#endif

#include <stdio.h>
#include <stdarg.h>

#include <android/log.h>

#define STDOUT_FILE "/data/vendor/stdout.log"
#define STDERR_FILE "/data/vendor/stderr.log"
#define FILEMODE_READONLY "r"
#define FILEMODE_READWRITE "r+"
#define FILEMODE_WRITEONLY "w"
#define FILEMODE_READWRITE_CREATE "w+"
#define FILEMODE_RW_APPEND "a"
#define FILEMODE_RW_APPEND_CREATE "a+"

inline void __flush_redirected_outputs(void) {
    (void)fflush(stderr);
    (void)fflush(stdout);
}

/*
 * Android system has redirect the standard outputs (like stdout and stderr) to /dev/null
 * We should redirect them so that we can fetch the log which was outputed to it.
 */
inline void __redirect_standard_outputs(void) {
    (void)freopen(STDERR_FILE, FILEMODE_RW_APPEND_CREATE, stderr);
    (void)freopen(STDOUT_FILE, FILEMODE_RW_APPEND_CREATE, stdout);
    fprintf(stderr, "%s\n", __func__);
    fprintf(stdout, "%s\n", __func__);
    __flush_redirected_outputs();
}

inline void __log(const int level, const char *format, va_list va) {
    __android_log_vprint(level, LOG_TAG, format, va);
    // HACK: bind the flush operation of the redirects of standard outputs
    // with normal log output
    __flush_redirected_outputs();
}

inline void log_v(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_VERBOSE, format, va);
    va_end(va);
}

inline void log_d(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_DEBUG, format, va);
    va_end(va);
}

inline void log_i(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_INFO, format, va);
    va_end(va);
}

inline void log_w(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_WARN, format, va);
    va_end(va);
}

inline void log_e(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_ERROR, format, va);
    va_end(va);
}

inline void log_f(const char *format, ...) {
    va_list va;
    va_start(va, format);
    __log(ANDROID_LOG_FATAL, format, va);
    va_end(va);
}

#endif // GRALLOC_GBM_MESA_LOG_H_