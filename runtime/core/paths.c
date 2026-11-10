/* Where Wonderpark keeps its files.
 *
 *   data  (%APPDATA%\Wonderpark, or WONDERPARK_DATA_DIR): settings.ini,
 *         bindings.ini and the emulated NAND with the saves (nand\).
 *   cache (cache\ beside Wonderpark.exe, or WONDERPARK_CACHE_DIR): things
 *         that can be thrown away, such as the NAND's /tmp, where the game
 *         copies tens of MB of data at every start. A Wii empties /tmp at
 *         boot; so does Wonderpark. Kept beside the program rather than in
 *         %APPDATA%, so it follows the install onto whatever drive that is. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wp.h"

static void make_dirs(const char* path) {
    char tmp[1024];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

static void exe_dir(char* out, size_t n) {
#ifdef _WIN32
    char* slash;
    GetModuleFileNameA(NULL, out, (DWORD)n);
    slash = strrchr(out, '\\');
    if (slash) *slash = '\0';
#else
    snprintf(out, n, ".");
#endif
}

const char* wp_data_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        const char* e = getenv("WONDERPARK_DATA_DIR");
        const char* appdata = getenv("APPDATA");
        if (e && *e) snprintf(dir, sizeof(dir), "%s", e);
        else if (appdata && *appdata) snprintf(dir, sizeof(dir), "%s/Wonderpark", appdata);
        else exe_dir(dir, sizeof(dir));
        make_dirs(dir);
    }
    return dir;
}

const char* wp_cache_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        const char* e = getenv("WONDERPARK_CACHE_DIR");
        if (e && *e) {
            snprintf(dir, sizeof(dir), "%s", e);
        } else {
            exe_dir(dir, sizeof(dir));
            strncat(dir, "/cache", sizeof(dir) - strlen(dir) - 1);
        }
        make_dirs(dir);
    }
    return dir;
}

void wp_make_dirs(const char* path) { make_dirs(path); }
