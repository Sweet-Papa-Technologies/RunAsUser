# RunAsUser

A lightweight, standalone tool that runs a command as the currently logged-in user from a root/SYSTEM context. Designed for MDM tools, system services, and IT automation scripts that need to execute commands in the user's context.

## Building

### macOS (universal binary: arm64 + x86_64)

```
make macos
```

Produces `build/runasuser` (~82 KB). Requires only Xcode Command Line Tools.

### Windows (cross-compile from macOS)

```
brew install mingw-w64
make windows
```

Produces `build/runasuser.exe` with static CRT — no vcredist or DLL dependencies beyond standard Windows system DLLs.

### Both

```
make all
```

## Usage

```
runasuser [options] <command> [args...]
```

### macOS

Must be run as **root**. Detects the console user via `SCDynamicStoreCopyConsoleUser`, drops privileges (`initgroups` → `setgid` → `setuid`), and exec's the command.

```bash
sudo runasuser whoami
sudo runasuser --wait /usr/bin/python3 script.py
sudo runasuser --session osascript -e 'display dialog "Hello"'
sudo runasuser --wait --session open -a Safari
```

### Windows

Must be run as **SYSTEM**. Obtains the user's session token via `WTSQueryUserToken` and launches the process with `CreateProcessAsUserW`.

```cmd
runasuser whoami
runasuser --wait cmd /c echo hello
runasuser --session 2 notepad.exe
```

## Options

| Flag | macOS | Windows | Description |
|------|-------|---------|-------------|
| `--wait` | Yes | Yes | Wait for the command to finish and propagate its exit code. Without this, macOS replaces the process via `execvp` and Windows exits immediately after launching. |
| `--session` | Yes | Yes | **macOS:** Run in the user's Mach bootstrap namespace (via `launchctl asuser`). Required for GUI apps, `osascript`, Keychain access, `open`, etc. **Windows:** Target a specific session ID (e.g., `--session 2` for an RDP session). Without this, targets the active console session. |
| `--help` | Yes | Yes | Show usage information. |

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success (`execvp` doesn't return on macOS; process created on Windows) |
| _N_ | Child process exit code (when using `--wait`) |
| 1 | General failure (not root/SYSTEM, etc.) |
| 2 | No interactive user session found |
| 3 | Failed to drop privileges (macOS) / Failed to get user token (Windows) |
| 4 | Failed to execute/create process |
| 5 | Invalid arguments / usage error |

## How It Works

### macOS

1. `SCDynamicStoreCopyConsoleUser()` — detects the logged-in console user (UID/GID)
2. `getpwuid()` — resolves username, home directory, shell, groups
3. `initgroups()` → `setgid()` → `setuid()` — drops privileges (order is critical for security)
4. Verifies privilege drop is irreversible (`setuid(0)` must fail)
5. Sets clean environment (`HOME`, `USER`, `LOGNAME`, `SHELL`, `PATH`)
6. `execvp()` — replaces process with the command (or `fork`+`exec` with `--wait`)

With `--session`: re-invokes itself through `launchctl asuser <uid>` to enter the user's Mach bootstrap namespace before dropping privileges.

### Windows

1. `WTSGetActiveConsoleSessionId()` — finds the active console session (with `WTSEnumerateSessions` fallback for RDP)
2. `WTSQueryUserToken()` — obtains the user's session token (requires SYSTEM privileges)
3. `DuplicateTokenEx()` — creates a primary token suitable for process creation
4. `CreateEnvironmentBlock()` — builds the user's environment variables
5. `GetUserProfileDirectoryW()` — gets the user's profile path for the working directory
6. `CreateProcessAsUserW()` — launches the process on `winsta0\default` (the interactive desktop)

## License

See [LICENSE](LICENSE).
