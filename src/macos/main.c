/*
 * runasuser - Execute a command as the currently logged-in macOS console user.
 *
 * Must be run as root. Detects the console user via SystemConfiguration,
 * drops privileges, and exec's the requested command.
 *
 * Exit codes:
 *   0 - Success (execvp doesn't return) / child success (--wait)
 *   1 - General failure (not root, etc.)
 *   2 - No interactive user session found
 *   3 - Failed to drop privileges
 *   4 - Failed to execute command
 *   5 - Invalid arguments / usage error
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#define EXIT_GENERAL       1
#define EXIT_NO_SESSION    2
#define EXIT_PRIV_DROP     3
#define EXIT_EXEC_FAIL     4
#define EXIT_USAGE         5

#define DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

static void usage(void)
{
    fprintf(stderr,
        "Usage: runasuser [--wait] [--session] <command> [args...]\n"
        "\n"
        "Run a command as the currently logged-in user (must be run as root).\n"
        "\n"
        "Options:\n"
        "  --wait      Wait for the command to exit and propagate its exit code\n"
        "  --session   Run in the user's GUI session (Mach bootstrap namespace).\n"
        "              Required for GUI apps, osascript, Keychain access, etc.\n"
        "\n"
        "Examples:\n"
        "  runasuser whoami\n"
        "  runasuser --wait /usr/bin/python3 script.py\n"
        "  runasuser --session osascript -e 'display dialog \"Hello\"'\n"
        "  runasuser --wait --session open -a Safari\n"
    );
}

/*
 * Get the absolute path of this executable using _NSGetExecutablePath.
 * Caller must free the returned string.  Returns NULL on failure.
 */
static char *get_self_path(void)
{
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);        /* get required buffer size */

    char *buf = malloc(size);
    if (!buf)
        return NULL;

    if (_NSGetExecutablePath(buf, &size) != 0) {
        free(buf);
        return NULL;
    }

    /* Resolve symlinks so launchctl gets the real path */
    char *resolved = realpath(buf, NULL);
    free(buf);
    return resolved;    /* may be NULL if realpath fails */
}

/*
 * Handle --session: re-invoke ourselves through `launchctl asuser <uid>`
 * so the command runs inside the user's Mach bootstrap namespace.
 *
 * We build:  launchctl asuser <uid> <self> [--wait] <command> [args...]
 * (--session is stripped to avoid infinite recursion)
 *
 * Always waits for launchctl to finish and propagates the exit code.
 */
static int handle_session(uid_t uid, int flag_wait, int cmd_argc, char **cmd_argv)
{
    char *self_path = get_self_path();
    if (!self_path) {
        fprintf(stderr, "runasuser: failed to determine own executable path\n");
        return EXIT_GENERAL;
    }

    char uid_str[32];
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned)uid);

    /*
     * argv for execvp:
     *   "launchctl" "asuser" "<uid>" "<self>" ["--wait"] <cmd> [args...] NULL
     *
     * Slot count: 4 (launchctl, asuser, uid, self)
     *           + 1 if --wait
     *           + cmd_argc (command + its args)
     *           + 1 (NULL terminator)
     */
    int nargs = 4 + (flag_wait ? 1 : 0) + cmd_argc + 1;
    char **args = calloc(nargs, sizeof(char *));
    if (!args) {
        fprintf(stderr, "runasuser: memory allocation failed\n");
        free(self_path);
        return EXIT_GENERAL;
    }

    int i = 0;
    args[i++] = "launchctl";
    args[i++] = "asuser";
    args[i++] = uid_str;
    args[i++] = self_path;
    if (flag_wait)
        args[i++] = "--wait";
    for (int j = 0; j < cmd_argc; j++)
        args[i++] = cmd_argv[j];
    args[i] = NULL;

    /* Fork so we can wait for launchctl and propagate its exit code */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "runasuser: fork: %s\n", strerror(errno));
        free(args);
        free(self_path);
        return EXIT_EXEC_FAIL;
    }

    if (pid == 0) {
        /* child: exec launchctl */
        execvp("launchctl", args);
        fprintf(stderr, "runasuser: exec launchctl: %s\n", strerror(errno));
        _exit(EXIT_EXEC_FAIL);
    }

    /* parent: wait for launchctl */
    free(args);
    free(self_path);

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            fprintf(stderr, "runasuser: waitpid: %s\n", strerror(errno));
            return EXIT_GENERAL;
        }
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return EXIT_GENERAL;
}

/*
 * Drop privileges to the target user.
 *
 * Order matters for security:
 *   1. initgroups() - set supplementary groups (requires root)
 *   2. setgid()     - set real/effective/saved GID (requires root)
 *   3. setuid()     - set real/effective/saved UID (irreversible)
 *
 * After dropping, verify we cannot regain root.
 */
static int drop_privileges(const struct passwd *pw)
{
    /* 1. Set supplementary group list (must happen while still root) */
    if (initgroups(pw->pw_name, pw->pw_gid) != 0) {
        fprintf(stderr, "runasuser: initgroups: %s\n", strerror(errno));
        return -1;
    }

    /* 2. Set GID before UID (setgid requires root) */
    if (setgid(pw->pw_gid) != 0) {
        fprintf(stderr, "runasuser: setgid(%u): %s\n",
                (unsigned)pw->pw_gid, strerror(errno));
        return -1;
    }

    /* 3. Set UID last (this is irreversible) */
    if (setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "runasuser: setuid(%u): %s\n",
                (unsigned)pw->pw_uid, strerror(errno));
        return -1;
    }

    /*
     * Verify: attempting to regain root must fail.
     * If setuid(0) succeeds, the privilege drop was incomplete.
     */
    if (setuid(0) == 0) {
        fprintf(stderr, "runasuser: privilege drop verification failed "
                        "(was able to regain root)\n");
        return -1;
    }

    return 0;
}

/*
 * Set a clean environment for the target user.
 */
static void setup_environment(const struct passwd *pw)
{
    setenv("HOME",    pw->pw_dir,   1);
    setenv("USER",    pw->pw_name,  1);
    setenv("LOGNAME", pw->pw_name,  1);
    setenv("SHELL",   pw->pw_shell, 1);
    setenv("PATH",    DEFAULT_PATH, 1);
}

int main(int argc, char *argv[])
{
    int flag_wait    = 0;
    int flag_session = 0;

    /* --- Parse flags (stop at first non-flag argument) --- */
    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "--wait") == 0) {
            flag_wait = 1;
            argi++;
        } else if (strcmp(argv[argi], "--session") == 0) {
            flag_session = 1;
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            usage();
            return 0;
        } else {
            break;    /* first non-flag = start of command */
        }
    }

    if (argi >= argc) {
        usage();
        return EXIT_USAGE;
    }

    char **cmd_argv = &argv[argi];
    int    cmd_argc = argc - argi;

    /* --- Must be root --- */
    if (getuid() != 0) {
        fprintf(stderr, "runasuser: must be run as root\n");
        return EXIT_GENERAL;
    }

    /* --- Detect the console (GUI-session) user --- */
    uid_t uid = 0;
    gid_t gid = 0;
    CFStringRef cf_user = SCDynamicStoreCopyConsoleUser(NULL, &uid, &gid);

    if (cf_user == NULL) {
        fprintf(stderr, "runasuser: no console user found "
                        "(no interactive session)\n");
        return EXIT_NO_SESSION;
    }

    /* Convert CFString to C string for the "loginwindow" check */
    char username_buf[256];
    Boolean ok = CFStringGetCString(cf_user, username_buf, sizeof(username_buf),
                                    kCFStringEncodingUTF8);
    CFRelease(cf_user);

    if (!ok || strcmp(username_buf, "loginwindow") == 0) {
        fprintf(stderr, "runasuser: no interactive user session found "
                        "(login window is active)\n");
        return EXIT_NO_SESSION;
    }

    /* --- If --session, re-invoke via launchctl asuser --- */
    if (flag_session) {
        return handle_session(uid, flag_wait, cmd_argc, cmd_argv);
    }

    /* --- Resolve user details from UID --- */
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        fprintf(stderr, "runasuser: getpwuid(%u): %s\n",
                (unsigned)uid, strerror(errno));
        return EXIT_GENERAL;
    }

    /* --- Drop privileges (root -> console user) --- */
    if (drop_privileges(pw) != 0)
        return EXIT_PRIV_DROP;

    /* --- Set clean environment --- */
    setup_environment(pw);

    /* --- Execute the command --- */
    if (flag_wait) {
        /* Fork, exec in child, wait in parent */
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "runasuser: fork: %s\n", strerror(errno));
            return EXIT_EXEC_FAIL;
        }

        if (pid == 0) {
            /* Child: exec the command */
            execvp(cmd_argv[0], cmd_argv);
            fprintf(stderr, "runasuser: exec %s: %s\n",
                    cmd_argv[0], strerror(errno));
            _exit(EXIT_EXEC_FAIL);
        }

        /* Parent: wait and propagate exit code */
        int status;
        while (waitpid(pid, &status, 0) == -1) {
            if (errno != EINTR) {
                fprintf(stderr, "runasuser: waitpid: %s\n", strerror(errno));
                return EXIT_GENERAL;
            }
        }

        if (WIFEXITED(status))
            return WEXITSTATUS(status);

        /* Killed by signal */
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);

        return EXIT_GENERAL;
    }

    /* No --wait: replace this process entirely */
    execvp(cmd_argv[0], cmd_argv);
    fprintf(stderr, "runasuser: exec %s: %s\n", cmd_argv[0], strerror(errno));
    return EXIT_EXEC_FAIL;
}
