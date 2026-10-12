/* restart: recomp_restart.h. */
#include "recomp_restart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static void later_handler(void)
{
    /* Registered after recomp_restart_init, so it runs before the restart:
     * a program's own shutdown work is done by then. */
    FILE *f = fopen("restart_order.txt", "a");
    if (f) { fputs("shutdown\n", f); fclose(f); }
}

int main(int argc, char **argv)
{
    recomp_restart_init(argc, argv);
    atexit(later_handler);

    if (argc >= 2 && !strcmp(argv[1], "snapshot")) {
        int ac = 0;
        char **av = NULL, **env = NULL;
        const char *cwd = NULL;
        int i, found = 0;

        CHECK("available", recomp_restart_available());
        CHECK("not asked for", !recomp_restart_requested());
        /* What the program does to itself after the start is not in it. */
        set_env("RESTART_TEST_LATER", "1");
        argv[1] = "changed";
        recomp_restart_snapshot(&ac, &av, &env, &cwd);
        CHECK("argument count", ac == argc);
        CHECK("the arguments as given", av && !strcmp(av[1], "snapshot") && !strcmp(av[2], "a b")
                                        && av[ac] == NULL);
        CHECK("a directory", cwd && *cwd);
#if !defined(_WIN32)
        for (i = 0; env && env[i]; i++)
            if (!strncmp(env[i], "RESTART_TEST_LATER=", 19))
                found = 1;
        CHECK("the environment as given", env && !found);
#else
        (void)i; (void)found;
#endif
        recomp_restart_request("a test");
        CHECK("asked for", recomp_restart_requested());
        recomp_restart_cancel();
        CHECK("taken back", !recomp_restart_requested());
        printf(failures ? "%d FAILED\n" : "ok\n", failures);
        return failures ? 1 : 0;       /* exits without restarting */
    }

    if (argc >= 4 && !strcmp(argv[1], "real")) {
        const char *mark = argv[2];
        FILE *f = fopen(mark, "r");
        if (!f) {
            /* First life: leave the mark, dirty the environment, restart. */
            f = fopen(mark, "w");
            if (!f)
                return 2;
            fprintf(f, "%ld\n", (long)getpid());
            fclose(f);
            remove("restart_order.txt");
            set_env("RESTART_TEST_GIVEN", "changed by the first life");
            set_env("RESTART_TEST_LATER", "1");
            recomp_restart_request("the test's first life");
            return 0;                   /* the ordinary exit: the restart follows */
        } else {
            long first = 0;
            const char *given = getenv("RESTART_TEST_GIVEN");
            FILE *o;
            char line[32] = "";
            if (fscanf(f, "%ld", &first) != 1)
                first = 0;
            fclose(f);
            remove(mark);
            CHECK("the argument came along", !strcmp(argv[3], "an argument"));
            CHECK("the environment as it was given", given && !strcmp(given, "given"));
            CHECK("not what the first life added", getenv("RESTART_TEST_LATER") == NULL);
#if !defined(_WIN32)
            CHECK("the same process", first == (long)getpid());
#else
            CHECK("another process on Windows", first != 0 && first != (long)getpid());
#endif
            o = fopen("restart_order.txt", "r");
            CHECK("the first life's shutdown ran before the restart",
                  o && fgets(line, sizeof line, o) && !strncmp(line, "shutdown", 8));
            if (o) fclose(o);
            CHECK("no second restart", !recomp_restart_requested());
            printf(failures ? "%d FAILED\n" : "ok (restarted once)\n", failures);
            return failures ? 1 : 0;
        }
    }
    printf("usage: restart_test snapshot ... | real <mark file> <argument>\n");
    return 2;
}
