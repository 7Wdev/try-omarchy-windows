# 7Wdev fork

Source: https://github.com/7Wdev/try-omarchy-windows

This fork tracks `omacom/try-omarchy-windows` and adds an NVIDIA backend
investigation, native host renderer and partial QEMU WDDM control bridge. These do not
accelerate the Omarchy guest; read [Windows NVIDIA backend](WINDOWS-NVIDIA-BACKEND.md).

## Upstream updates

`Sync fork with upstream` runs daily at 06:17 UTC and can be dispatched manually.
It merges upstream `master`, validates on Windows and Linux, then publishes
only a passing fast-forward update to this fork's `master`. Fork commits are
preserved. It never rebases, resets or force-pushes. Conflicts stop the run with
the fork head retained. Failed validation leaves a `sync/upstream-*` candidate
branch for inspection. Concurrent edits of `master` stop publication safely.

GitHub Actions must be enabled. GitHub can suspend schedules after repository
inactivity, so check the Actions page when returning after a long break.
Graphics changes still require physical acceptance even when CI passes.

For a local merge (the script does not push):

```powershell
git remote add upstream https://github.com/omacom/try-omarchy-windows.git
git fetch upstream master
python scripts/fork/prepare_sync.py
```

If `upstream` exists, use `git remote set-url upstream ...`. The script requires
a clean checkout and aborts a conflicted merge. Follow CONTRIBUTING.md checks
before publishing a local merge.

## Build your version

With the Go version in `app/go.mod`:

```powershell
scripts/fork/build.ps1
```

`dist/TryOmarchy-7Wdev.exe` embeds fork identity, defaults to the
`TryOmarchy-7Wdev` data directory and blocks official launcher replacement.
Rebuild after source merges: executable updates and source updates are separate.
Initial guest/runtime downloads still use pinned, verified upstream payloads.
A fork release feed and your own signing keys have not been set up. The builds
are unsigned development artifacts.

The generic upstream `go build` without fork linker flags retains official
update behavior. Use the fork build script for your version. The GPU experiment
builds separately; see its [README](../experimental/windows-native-gpu/README.md).
