/* gui_platform.c — cross-platform OS shims for the GTK UI layer.
 *
 * For now this is just executable-path discovery. The resource resolvers
 * (gui_theme.c, gui.c) and the restart action (gui_statusbar.c) need the path
 * of the running binary, and there is no portable libc call for it:
 *
 *   Linux   → readlink("/proc/self/exe")
 *   Windows → GetModuleFileNameW (UTF-16, converted to UTF-8 via GLib)
 *   macOS   → _NSGetExecutablePath
 *
 * The result always uses forward slashes so the existing
 * strrchr(path, '/') / "%s/../.." logic keeps working unchanged on Windows.
 */

#include "gui_internal.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <glib.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <unistd.h>
#endif

size_t qirtas_exe_path(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';

#ifdef _WIN32
    wchar_t wbuf[4096];
    DWORD n = GetModuleFileNameW(NULL, wbuf,
                                 (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
    /* n == buffer size means the path was truncated — treat as failure. */
    if (n == 0 || n >= (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]))) return 0;
    gchar *utf8 = g_utf16_to_utf8((const gunichar2 *)wbuf, -1, NULL, NULL, NULL);
    if (!utf8) return 0;
    for (char *p = utf8; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    g_strlcpy(out, utf8, out_sz);
    g_free(utf8);
    return strlen(out);

#elif defined(__APPLE__)
    char tmp[4096];
    uint32_t tsz = (uint32_t)sizeof(tmp);
    if (_NSGetExecutablePath(tmp, &tsz) != 0) return 0;
    g_strlcpy(out, tmp, out_sz);
    return strlen(out);

#else
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    return (size_t)n;
#endif
}
