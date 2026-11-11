/* The game thread never waits to write a log line. Everything printed to
 * stderr goes into a pipe in memory, and a thread of its own copies it to
 * where stderr really went (a console, a file). Writing to a console or a
 * busy disk directly could block the game thread: a 0.4-1.1 s frame in
 * the park, or the whole game for as long as text is selected in the
 * console. WONDERPARK_DIRECT_LOG=1 writes directly instead.
 *
 * wp_logpipe_drain() closes the pipe and waits (up to 2 s) until what was
 * printed is out; it runs at exit and on a crash. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE s_rd, s_out;
static HANDLE s_copier;

static DWORD WINAPI copier(LPVOID arg) {
    static char buf[65536];
    DWORD n, done;
    (void)arg;
    for (;;) {
        if (!ReadFile(s_rd, buf, sizeof(buf), &n, NULL) || n == 0) break;
        while (n > 0 && WriteFile(s_out, buf, n, &done, NULL) && done > 0) {
            memmove(buf, buf + done, n - done);
            n -= done;
        }
    }
    return 0;
}

void wp_logpipe_drain(void) {
    /* Closing the pipe's only write end lets the copier read what's left,
     * then end. (Asking the pipe how much is left would wait behind the
     * copier's pending read.) Nothing printed after this is shown. */
    if (!s_copier) return;
    fflush(stderr);
    _close(_fileno(stderr));
    WaitForSingleObject(s_copier, 2000);
    s_copier = NULL;
}

void wp_logpipe_start(void) {
    HANDLE wr;
    int fd;
    const char* e = getenv("WONDERPARK_DIRECT_LOG");
    if (e && *e && *e != '0') return;
    s_out = GetStdHandle(STD_ERROR_HANDLE);
    if (!s_out || s_out == INVALID_HANDLE_VALUE) return; /* nowhere to write: nothing to wait for either */
    /* A copy of its own: replacing stderr below closes the original. */
    if (!DuplicateHandle(GetCurrentProcess(), s_out, GetCurrentProcess(), &s_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    fflush(stderr);
    if (!CreatePipe(&s_rd, &wr, NULL, 1 << 20)) return;
    fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
    if (fd < 0 || _dup2(fd, _fileno(stderr)) != 0) {
        CloseHandle(s_rd);
        s_rd = NULL;
        return;
    }
    _close(fd);
    s_copier = CreateThread(NULL, 0, copier, NULL, 0, NULL);
    atexit(wp_logpipe_drain);
}
#else
void wp_logpipe_start(void) {}
void wp_logpipe_drain(void) {}
#endif
