/*
 * recomp_restart.c - see recomp_restart.h.
 */
#include "recomp_restart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

static int    s_argc;
static char **s_argv;
static char **s_envp;
static char   s_cwd[2048];
static int    s_ready;
static volatile int s_requested;
static void (*s_hook)(void);
#if defined(_WIN32)
static wchar_t *s_wenv;                 /* the environment block as given */
static wchar_t  s_wcwd[2048];
#endif

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char **dup_vec(char **v, int n)
{
    char **d = (char **)calloc((size_t)n + 1, sizeof *d);
    int i;
    if (!d)
        return NULL;
    for (i = 0; i < n; i++)
        if (!(d[i] = dup_str(v[i] ? v[i] : "")))
            return NULL;
    return d;
}

#if !defined(_WIN32) && !defined(__ANDROID__)
/* The executable itself: argv[0] may be a bare name found on PATH. */
static const char *exe_path(void)
{
    static char path[PATH_MAX];
#if defined(__APPLE__)
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) == 0)
        return path;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) {
        path[n] = 0;
        return path;
    }
#endif
    return s_argv[0];
}
#endif

/* The last atexit handler to run (the first registered). */
static void restart_at_exit(void)
{
    if (!s_requested || !s_ready)
        return;
    fflush(NULL);
#if defined(_WIN32)
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t *cmd = _wcsdup(GetCommandLineW());

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (cmd && CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT,
                                  s_wenv, s_wcwd[0] ? s_wcwd : NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            fprintf(stderr, "[RESTART] the program could not be started again (error %lu)\n",
                    (unsigned long)GetLastError());
            fflush(stderr);
        }
        free(cmd);
    }
#elif !defined(__ANDROID__)
    {
        int fd, max = (int)sysconf(_SC_OPEN_MAX);
        const char *exe = exe_path();

        /* What this run opened (the log, devices, sockets) does not go
         * along: closed by the exec, not here, where other threads still
         * run. */
        if (max < 0 || max > 65536)
            max = 65536;
        for (fd = 3; fd < max; fd++) {
            int fl = fcntl(fd, F_GETFD);
            if (fl >= 0)
                fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        }
        if (s_cwd[0] && chdir(s_cwd) != 0)
            fprintf(stderr, "[RESTART] cannot go back to %s\n", s_cwd);
        execve(exe, s_argv, s_envp);
        fprintf(stderr, "[RESTART] the program could not be started again (%s)\n", exe);
        fflush(stderr);
    }
#endif
}

void recomp_restart_init(int argc, char **argv)
{
    int n = 0;

    if (s_ready || argc < 1 || !argv)
        return;
    s_argc = argc;
    s_argv = dup_vec(argv, argc);
#if defined(_WIN32)
    {
        const wchar_t *env = GetEnvironmentStringsW(), *p = env;
        size_t len;
        if (env) {
            while (*p)
                p += wcslen(p) + 1;
            len = (size_t)(p - env) + 1;
            s_wenv = (wchar_t *)malloc(len * sizeof *s_wenv);
            if (s_wenv)
                memcpy(s_wenv, env, len * sizeof *s_wenv);
            FreeEnvironmentStringsW((LPWCH)env);
        }
        if (!GetCurrentDirectoryW(2048, s_wcwd))
            s_wcwd[0] = 0;
        if (!GetCurrentDirectoryA(sizeof s_cwd, s_cwd))
            s_cwd[0] = 0;
        s_envp = (char **)calloc(1, sizeof *s_envp);
        (void)n;
    }
#else
    while (environ && environ[n])
        n++;
    s_envp = dup_vec(environ, n);
    if (!getcwd(s_cwd, sizeof s_cwd))
        s_cwd[0] = 0;
#endif
    if (!s_argv || !s_envp)
        return;
    s_ready = 1;
    atexit(restart_at_exit);
}

int recomp_restart_available(void)
{
    return s_ready || s_hook != NULL;
}

void recomp_restart_set_hook(void (*fn)(void))
{
    s_hook = fn;
}

void recomp_restart_request(const char *why)
{
    fprintf(stderr, "[RESTART] asked for: %s\n", why && *why ? why : "(no reason given)");
    if (s_hook) {
        s_hook();
        return;
    }
    s_requested = 1;
}

int recomp_restart_requested(void)
{
    return s_requested;
}

void recomp_restart_cancel(void)
{
    s_requested = 0;
}

void recomp_restart_snapshot(int *argc, char ***argv, char ***envp, const char **cwd)
{
    if (argc) *argc = s_argc;
    if (argv) *argv = s_argv;
    if (envp) *envp = s_envp;
    if (cwd)  *cwd = s_cwd;
}
