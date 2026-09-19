/*
 * respath.c — resolves paths to bundled resources (images, CSS, database)
 * relative to the running executable, so the app works no matter where
 * it's installed or launched from. This replaces the original hardcoded
 * "C:/Users/hp5cd/Desktop/Codes/MINI/images/..." paths, which only ever
 * worked on the original author's machine.
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L /* for readlink() under -std=c11 */
#endif

#include "respath.h"
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <unistd.h>
#endif

/* Fills out_dir (of size out_size) with the directory containing the
 * running executable, including a trailing separator. Falls back to
 * "./" if the platform call fails, so the app still runs (e.g. when
 * launched from the project directory during development). */
void respath_get_base_dir(char *out_dir, size_t out_size)
{
    char buf[4096];
    size_t len = 0;

#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf))
        len = (size_t)n;
#elif defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        len = strlen(buf);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        len = (size_t)n;
    }
#endif

    if (len == 0) {
        snprintf(out_dir, out_size, "./");
        return;
    }

    /* Strip the executable filename, keep the directory + separator */
    while (len > 0 && buf[len - 1] != '/' && buf[len - 1] != '\\')
        len--;

    if (len == 0) {
        snprintf(out_dir, out_size, "./");
        return;
    }

    if (len >= out_size)
        len = out_size - 1;

    memcpy(out_dir, buf, len);
    out_dir[len] = '\0';
}

/* Joins the executable's base directory with a relative resource path,
 * e.g. respath_resolve("images/apple.jpg", buf, sizeof(buf)). */
void respath_resolve(const char *relative, char *out_path, size_t out_size)
{
    char base[4096];
    respath_get_base_dir(base, sizeof(base));
    snprintf(out_path, out_size, "%s%s", base, relative);
}
