/*
 * runasuser - Run a command as the currently logged-in user from SYSTEM context.
 *
 * Must be run as SYSTEM (NT AUTHORITY\SYSTEM). Finds the active user session,
 * obtains their token, and launches the requested command in the user's context.
 *
 * Exit codes:
 *   0              - Success (process created, no --wait)
 *   Child's code   - Success (with --wait)
 *   1              - General failure
 *   2              - No interactive user session found
 *   3              - Failed to get user token (not running as SYSTEM?)
 *   4              - Failed to create process
 *   5              - Invalid arguments / usage error
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

#define EXIT_SUCCESS_CODE       0
#define EXIT_GENERAL_FAILURE    1
#define EXIT_NO_SESSION         2
#define EXIT_TOKEN_FAILURE      3
#define EXIT_PROCESS_FAILURE    4
#define EXIT_USAGE_ERROR        5

/* -------------------------------------------------------------------------- */
/*  Error reporting                                                           */
/* -------------------------------------------------------------------------- */

static void print_error(const WCHAR *message, DWORD errorCode)
{
    WCHAR *sysMsg = NULL;

    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&sysMsg, 0, NULL);

    if (sysMsg) {
        /* Strip trailing \r\n from system message */
        size_t len = wcslen(sysMsg);
        while (len > 0 && (sysMsg[len - 1] == L'\n' || sysMsg[len - 1] == L'\r'))
            sysMsg[--len] = L'\0';

        fwprintf(stderr, L"runasuser: %ls - %ls (error %lu)\n",
                 message, sysMsg, (unsigned long)errorCode);
        LocalFree(sysMsg);
    } else {
        fwprintf(stderr, L"runasuser: %ls (error %lu)\n",
                 message, (unsigned long)errorCode);
    }
}

static void print_message(const WCHAR *message)
{
    fwprintf(stderr, L"runasuser: %ls\n", message);
}

/* -------------------------------------------------------------------------- */
/*  Find the active user session                                              */
/* -------------------------------------------------------------------------- */

static BOOL find_active_session(DWORD *pSessionId)
{
    /* Try the fast path first: the physical console session */
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF) {
        *pSessionId = sessionId;
        return TRUE;
    }

    /* Fallback: enumerate all sessions, pick the first active one (covers RDP) */
    WTS_SESSION_INFOW *pSessions = NULL;
    DWORD count = 0;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1,
                               &pSessions, &count)) {
        return FALSE;
    }

    BOOL found = FALSE;
    for (DWORD i = 0; i < count; i++) {
        if (pSessions[i].State == WTSActive) {
            *pSessionId = pSessions[i].SessionId;
            found = TRUE;
            break;
        }
    }

    WTSFreeMemory(pSessions);
    return found;
}

/* -------------------------------------------------------------------------- */
/*  Query the username for a session (for informational logging)              */
/* -------------------------------------------------------------------------- */

static WCHAR *get_session_username(DWORD sessionId)
{
    WCHAR *userName = NULL;
    DWORD bytes = 0;

    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                    WTSUserName, &userName, &bytes)) {
        return userName; /* Caller must WTSFreeMemory */
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Build a command line string from argv-style arguments                     */
/* -------------------------------------------------------------------------- */

/*
 * Build a correctly-quoted command line string from an argv array.
 *
 * Follows the escaping rules consumed by CommandLineToArgvW:
 *   - Arguments containing spaces, tabs, or double-quotes are wrapped in "...".
 *   - Inside quotes, backslashes are literal UNLESS followed by a double-quote:
 *       * 2n   backslashes + "  →  n literal backslashes, quote ends/begins
 *       * 2n+1 backslashes + "  →  n literal backslashes + literal "
 *   - Trailing backslashes before the closing quote must be doubled so they
 *     don't escape it (e.g. arg  C:\path\  →  "C:\path\\"  not  "C:\path\").
 *   - Empty arguments are quoted as "".
 */
static WCHAR *build_command_line(int argc, wchar_t *argv[])
{
    /* First pass: calculate required buffer size */
    size_t totalLen = 0;
    for (int i = 0; i < argc; i++) {
        const WCHAR *arg = argv[i];
        BOOL needsQuoting = FALSE;

        if (arg[0] == L'\0') {
            needsQuoting = TRUE;      /* empty args must be quoted */
        } else {
            for (const WCHAR *p = arg; *p; p++) {
                if (*p == L' ' || *p == L'\t' || *p == L'"') {
                    needsQuoting = TRUE;
                    break;
                }
            }
        }

        if (needsQuoting) {
            totalLen += 2; /* surrounding quotes */
            size_t numBackslashes = 0;
            for (const WCHAR *p = arg; *p; p++) {
                if (*p == L'\\') {
                    numBackslashes++;
                } else if (*p == L'"') {
                    /* Double preceding backslashes + backslash-quote */
                    totalLen += numBackslashes + 1;
                    numBackslashes = 0;
                } else {
                    numBackslashes = 0;
                }
                totalLen += 1; /* the character itself */
            }
            /* Trailing backslashes are doubled before the closing quote */
            totalLen += numBackslashes;
        } else {
            totalLen += wcslen(arg);
        }

        if (i < argc - 1)
            totalLen += 1; /* space separator */
    }

    totalLen += 1; /* null terminator */

    WCHAR *cmdLine = (WCHAR *)malloc(totalLen * sizeof(WCHAR));
    if (!cmdLine)
        return NULL;

    /* Second pass: build the string */
    WCHAR *dst = cmdLine;
    for (int i = 0; i < argc; i++) {
        const WCHAR *arg = argv[i];
        BOOL needsQuoting = FALSE;

        if (arg[0] == L'\0') {
            needsQuoting = TRUE;
        } else {
            for (const WCHAR *p = arg; *p; p++) {
                if (*p == L' ' || *p == L'\t' || *p == L'"') {
                    needsQuoting = TRUE;
                    break;
                }
            }
        }

        if (needsQuoting) {
            *dst++ = L'"';
            size_t numBackslashes = 0;
            for (const WCHAR *p = arg; *p; p++) {
                if (*p == L'\\') {
                    numBackslashes++;
                    *dst++ = L'\\';
                } else if (*p == L'"') {
                    /* Double the preceding backslashes, then emit \" */
                    for (size_t j = 0; j < numBackslashes; j++)
                        *dst++ = L'\\';
                    *dst++ = L'\\';
                    *dst++ = L'"';
                    numBackslashes = 0;
                } else {
                    numBackslashes = 0;
                    *dst++ = *p;
                }
            }
            /* Double trailing backslashes before the closing quote */
            for (size_t j = 0; j < numBackslashes; j++)
                *dst++ = L'\\';
            *dst++ = L'"';
        } else {
            size_t len = wcslen(arg);
            memcpy(dst, arg, len * sizeof(WCHAR));
            dst += len;
        }

        if (i < argc - 1)
            *dst++ = L' ';
    }

    *dst = L'\0';
    return cmdLine;
}

/* -------------------------------------------------------------------------- */
/*  Usage                                                                     */
/* -------------------------------------------------------------------------- */

static void print_usage(void)
{
    fwprintf(stderr,
        L"Usage: runasuser [--wait] [--session <id>] <command> [args...]\n"
        L"\n"
        L"Run a command as the currently logged-in user (must be run as SYSTEM).\n"
        L"\n"
        L"Options:\n"
        L"  --wait          Wait for the process to exit and propagate its exit code\n"
        L"  --session <id>  Target a specific session ID (default: active console)\n"
        L"\n"
        L"Examples:\n"
        L"  runasuser whoami\n"
        L"  runasuser --wait cmd /c echo hello\n"
        L"  runasuser --session 2 notepad.exe\n"
    );
}

/* -------------------------------------------------------------------------- */
/*  wmain - entry point                                                       */
/* -------------------------------------------------------------------------- */

int wmain(int argc, wchar_t *argv[])
{
    int exitCode            = EXIT_GENERAL_FAILURE;
    BOOL waitForChild       = FALSE;
    BOOL sessionSpecified   = FALSE;
    DWORD targetSessionId   = 0;
    int cmdArgStart         = 0;

    HANDLE hToken           = NULL;
    HANDLE hDupToken        = NULL;
    LPVOID lpEnvironment    = NULL;
    WCHAR *cmdLine          = NULL;
    WCHAR *sessionUser      = NULL;
    WCHAR profileDir[MAX_PATH] = {0};

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    /* ---- Parse flags ---------------------------------------------------- */

    int i = 1;
    while (i < argc) {
        if (wcscmp(argv[i], L"--wait") == 0) {
            waitForChild = TRUE;
            i++;
        } else if (wcscmp(argv[i], L"--session") == 0) {
            if (i + 1 >= argc) {
                print_message(L"--session requires a session ID argument");
                print_usage();
                return EXIT_USAGE_ERROR;
            }
            WCHAR *endPtr = NULL;
            unsigned long val = wcstoul(argv[i + 1], &endPtr, 10);
            if (endPtr == argv[i + 1] || *endPtr != L'\0') {
                fwprintf(stderr, L"runasuser: invalid session ID: %ls\n", argv[i + 1]);
                return EXIT_USAGE_ERROR;
            }
            targetSessionId = (DWORD)val;
            sessionSpecified = TRUE;
            i += 2;
        } else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) {
            print_usage();
            return EXIT_SUCCESS_CODE;
        } else {
            break; /* first non-flag = start of command */
        }
    }

    cmdArgStart = i;

    if (cmdArgStart >= argc) {
        print_message(L"no command specified");
        print_usage();
        return EXIT_USAGE_ERROR;
    }

    /* ---- Step 1: Find the target session -------------------------------- */

    if (!sessionSpecified) {
        if (!find_active_session(&targetSessionId)) {
            print_message(L"no active user session found");
            exitCode = EXIT_NO_SESSION;
            goto cleanup;
        }
    }

    /* Query the username for this session (informational) */
    sessionUser = get_session_username(targetSessionId);
    if (sessionUser && sessionUser[0] != L'\0') {
        fwprintf(stderr, L"runasuser: targeting session %lu (user: %ls)\n",
                 (unsigned long)targetSessionId, sessionUser);
    } else {
        fwprintf(stderr, L"runasuser: targeting session %lu\n",
                 (unsigned long)targetSessionId);
    }

    /* ---- Step 2: Get the user token for the session --------------------- */

    if (!WTSQueryUserToken(targetSessionId, &hToken)) {
        DWORD err = GetLastError();
        if (err == ERROR_PRIVILEGE_NOT_HELD) {
            print_message(L"privilege not held - this tool must be run as SYSTEM "
                          L"(e.g., via PsExec -s, a Windows service, or Task Scheduler "
                          L"running as SYSTEM)");
        } else if (err == ERROR_NO_TOKEN) {
            print_message(L"no user is logged into the target session");
        } else {
            print_error(L"WTSQueryUserToken failed", err);
        }
        exitCode = EXIT_TOKEN_FAILURE;
        goto cleanup;
    }

    /* ---- Step 3: Duplicate the token as a primary token ----------------- */

    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                          SecurityIdentification, TokenPrimary, &hDupToken)) {
        print_error(L"DuplicateTokenEx failed", GetLastError());
        exitCode = EXIT_TOKEN_FAILURE;
        goto cleanup;
    }

    /* ---- Step 4: Create the user's environment block -------------------- */

    if (!CreateEnvironmentBlock(&lpEnvironment, hDupToken, FALSE)) {
        print_error(L"CreateEnvironmentBlock failed", GetLastError());
        exitCode = EXIT_GENERAL_FAILURE;
        goto cleanup;
    }

    /* ---- Step 5: Get the user's profile directory for the working dir --- */

    DWORD profileDirSize = MAX_PATH;
    if (!GetUserProfileDirectoryW(hDupToken, profileDir, &profileDirSize)) {
        /* Non-fatal: fall back to no specific working directory */
        profileDir[0] = L'\0';
    }

    /* ---- Step 6: Build the command line string -------------------------- */

    cmdLine = build_command_line(argc - cmdArgStart, &argv[cmdArgStart]);
    if (!cmdLine) {
        print_message(L"failed to allocate memory for command line");
        exitCode = EXIT_GENERAL_FAILURE;
        goto cleanup;
    }

    /* ---- Step 7: Launch the process as the user ------------------------- */

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"winsta0\\default"; /* Interactive desktop */

    DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;

    if (!CreateProcessAsUserW(
            hDupToken,
            NULL,                                   /* lpApplicationName */
            cmdLine,                                /* lpCommandLine (mutable) */
            NULL,                                   /* lpProcessAttributes */
            NULL,                                   /* lpThreadAttributes */
            FALSE,                                  /* bInheritHandles */
            creationFlags,
            lpEnvironment,
            profileDir[0] ? profileDir : NULL,      /* lpCurrentDirectory */
            &si,
            &pi))
    {
        DWORD err = GetLastError();
        print_error(L"CreateProcessAsUserW failed", err);
        if (err == ERROR_PRIVILEGE_NOT_HELD) {
            print_message(L"hint: ensure this tool is running as SYSTEM with "
                          L"SE_ASSIGNPRIMARYTOKEN_NAME and SE_INCREASE_QUOTA_NAME privileges");
        }
        exitCode = EXIT_PROCESS_FAILURE;
        goto cleanup;
    }

    fwprintf(stderr, L"runasuser: process created (PID %lu)\n",
             (unsigned long)pi.dwProcessId);

    /* ---- Step 8: Optionally wait for the child process ------------------ */

    if (waitForChild) {
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD childExitCode = 1;
        if (GetExitCodeProcess(pi.hProcess, &childExitCode)) {
            exitCode = (int)childExitCode;
        } else {
            print_error(L"GetExitCodeProcess failed", GetLastError());
            exitCode = EXIT_GENERAL_FAILURE;
        }
    } else {
        exitCode = EXIT_SUCCESS_CODE;
    }

    /* ---- Cleanup -------------------------------------------------------- */

cleanup:
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (cmdLine)
        free(cmdLine);
    if (lpEnvironment)
        DestroyEnvironmentBlock(lpEnvironment);
    if (hDupToken)
        CloseHandle(hDupToken);
    if (hToken)
        CloseHandle(hToken);
    if (sessionUser)
        WTSFreeMemory(sessionUser);

    return exitCode;
}
