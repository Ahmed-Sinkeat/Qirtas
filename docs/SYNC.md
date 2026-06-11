# Qirtas Sync — Setup, Architecture & Troubleshooting

Four sync providers, all **on-demand** (fire on Save, app close, or the ● Sync Now button — never continuous):

| Provider | Mechanism | Where files go |
|---|---|---|
| Google Drive | OAuth loopback (port 12345) + Drive `appDataFolder` API, native Zig HTTP | Hidden app-data folder in your Drive |
| Dropbox | OAuth loopback (port 5173), upload via `~/.config/qirtas/dropbox_sync.sh` | Your Dropbox app folder |
| GitHub | Device flow (code shown in dialog), push via `~/.config/qirtas/github_sync.sh` | Repo `lawh-notes` (hardcoded) |
| Local folder | Plain newest-wins file copy | `~/QirtasSync` (override: `QIRTAS_LOCAL_SYNC_DIR`) |

Synced file types: `.md .txt .zig .zon .c .h` in the **current working directory** of the app.
Tokens are stored in the vault DB encrypted with ChaCha20Poly1305 under a key derived from `/etc/machine-id` (= tokens don't survive copying the DB to another machine — by design).

---

## 1. One-time setup (the part that was missing)

Qirtas does **not** ship OAuth app credentials. Until you provide them, Google Drive and Dropbox cannot connect at all — the Connect button will say `Set QIRTAS_GOOGLE_CLIENT_ID` / `Set QIRTAS_DROPBOX_APP_KEY`. GitHub ships with a usable client ID (device flow needs no secret).

### Google Drive

1. Go to <https://console.cloud.google.com> → create a project.
2. **APIs & Services → Library** → enable **Google Drive API**.
3. **OAuth consent screen** → External → add yourself as test user.
4. **Credentials → Create credentials → OAuth client ID** → Application type **Web application**.
5. Add authorized redirect URI exactly: `http://localhost:12345`
6. Copy the client ID, then run Qirtas with:

```sh
QIRTAS_GOOGLE_CLIENT_ID="1234-abc.apps.googleusercontent.com" qirtas
```

(or edit `GOOGLE_CLIENT_ID` in `src/gui/gui_sync.c` and rebuild).

### Dropbox

1. <https://www.dropbox.com/developers/apps> → Create app → Scoped access → App folder.
2. Permissions tab: enable `files.content.write`, `files.content.read`.
3. Settings tab → add redirect URI exactly: `http://localhost:5173`
4. Copy the **App key**:

```sh
QIRTAS_DROPBOX_APP_KEY="abcd1234" qirtas
```

### GitHub

Nothing to register. Connect → a code dialog appears → enter it at github.com/login/device. The sync target repo is currently hardcoded to `lawh-notes`.

### Local folder

Nothing to set up. Files mirror into `~/QirtasSync`; point Syncthing/Nextcloud at that folder.

---

## 2. Troubleshooting — status message → cause → fix

The status text on each sync card is the primary diagnostic. Run the app from a terminal for extra detail: workers print the failing step and HTTP status to stdout (`Search attempt N failed…`, `Token refresh failed with status …: <body>`).

### Connect-phase messages

| Status text | What actually happened | Fix |
|---|---|---|
| `Set QIRTAS_GOOGLE_CLIENT_ID` / `Set QIRTAS_DROPBOX_APP_KEY` | No OAuth app ID configured (the placeholder is still in place) | Do §1 setup |
| `Waiting for browser...` (stuck) | Browser never hit the local callback port — auth page abandoned, wrong redirect URI in the provider console, or the browser is sandboxed (Flatpak/Snap) and can't reach localhost | Finish the consent page; verify redirect URI is byte-exact (`http://localhost:12345` / `:5173`); try a non-sandboxed browser |
| `Connection timed out` | Loopback listener waited 5 min, no callback | Same as above — redo Connect |
| `Port 12345 busy` / `Port 5173 busy` | Another process owns the callback port (5173 is Vite's default dev port!) | `ss -tlnp \| grep 5173` — stop that process, reconnect |
| `Error: token exchange failed.` | Google/Dropbox rejected the code→token exchange. Most common causes: redirect URI mismatch between auth request and exchange (fixed 2026-06-12 — both now `http://localhost:12345`), expired/reused code, wrong app type (must be **Web application** for Google) | Reconnect once; if persistent, check terminal for the HTTP body, verify app type + URI in the console |
| `Connection failed` (GitHub) | Device-code request to github.com failed — offline, or `curl` not installed (GitHub flow shells out to curl) | `which curl`; check network |
| `Generating code...` (stuck) | Same as above | Same |

### Sync-phase messages

| Status text | What actually happened | Fix |
|---|---|---|
| `Error: missing credentials.` | No token row in the vault DB — never connected, or DB was wiped/moved | Connect again |
| `Error: auth expired. reconnect.` | Provider returned 401/403 or `invalid_grant`; tokens were auto-cleared | Reconnect. Google: test-mode OAuth apps get refresh tokens that expire after 7 days — publish the app or reconnect weekly |
| `Error: missing refresh token.` | Access token expired and no refresh token stored. Google only issues a refresh token on the *first* consent — if you connected before without `prompt=consent`, it was never stored | Disconnect, reconnect, approve again |
| `Error: token refresh failed.` | Refresh HTTP call failed (network or provider). Terminal shows status + body | If body says `invalid_grant` → reconnect; otherwise check network |
| `Error: database unavailable.` | Could not open the vault DB (currently hardcoded `/home/.config/lawh/vault.db`) | Ensure path exists and is writable; see "Known limitations" |
| `Error: Google Drive list/download/update/upload/metadata failed.` | The specific Drive API call failed after 3 retries (transient 5xx/429 are retried with backoff) | Terminal shows the HTTP status per attempt. 403 with valid auth = Drive API not enabled for your project |
| `Error: connection refused / unknown host / network unreachable / connection reset / no network addresses.` | Plain network failure at that step | Check connectivity/DNS/VPN |
| `Error: dropbox_sync.sh missing.` / `Error: github_sync.sh missing.` | Helper script absent or not executable at `~/.config/qirtas/` (checked with `access(X_OK)`) | Restore the script, `chmod +x` it |
| `Error: Dropbox sync failed.` / `Error: GitHub sync failed.` | Helper script ran but exited non-zero | Run it by hand to see why: `bash -x /home/.config/lawh/github_sync.sh <token> lawh-notes .` |
| `Error: cannot open folder.` | `opendir(".")` failed — app's working directory vanished or lacks permission | Launch from a valid directory |
| `Error: cannot create sync folder.` / `sync target is not a directory.` | `~/QirtasSync` (or `QIRTAS_LOCAL_SYNC_DIR`) couldn't be created, or exists as a file | Remove/rename the blocking file |
| `Error: sync failed.` | Anything not mapped above | Terminal output has the real error name |

### ⚠️ Conflict behavior — READ THIS, it differs per backend

What happens when the same note changed on two machines since the last sync:

| Backend | Behavior on conflict | Can it lose your edits? |
|---|---|---|
| **Google Drive** | True 3-way detection (local mtime + Drive modifiedTime vs `file_metadata.last_modified` in the vault DB). Cloud version takes the original filename, your local version is preserved as `<name>_conflict.<ext>`. Merge by hand. | No — both versions kept |
| **Dropbox** | **No conflict detection at all.** The script downloads every remote `.md`/`.txt` over your local files FIRST, then uploads. Any local edit made since the other machine's last upload is **silently overwritten by the remote copy**, and the overwritten file is then re-uploaded. | **YES — local edits since last sync are destroyed** |
| **GitHub** | `git pull --rebase --strategy-option=theirs` — on conflicting lines the **remote side silently wins**, then your (now-merged) state is pushed. Non-conflicting changes survive; conflicting hunks of your local edit are lost. Old versions remain recoverable in git history. | Partially — conflicting hunks lost from working copy, recoverable via `git log` in `~/.config/qirtas/github_sync` |
| **Local folder** | Newest mtime wins, full-file copy, no conflict copies, no both-changed detection. If both sides changed, the older edit is **silently overwritten**. | **YES — older side's edits are destroyed** |

**Practical rule until this is unified: treat Google Drive as the only
conflict-safe backend. Press Sync Now *before* editing on a second machine
when using Dropbox/Local.** The right fix is to port the Drive-style 3-way
metadata comparison (+`_conflict` copies) to the other three backends.

---

## 3. Known limitations / sharp edges

1. ~~Hardcoded DB path~~ **Fixed (2026-06-12):** everything now lives in `$XDG_CONFIG_HOME/qirtas/` (default `~/.config/qirtas/`) — vault.db and the sync scripts. On first launch the app automatically copies (never deletes) any data found at the old `/home/.config/lawh/` location. Path is resolved once in `main.zig` (`configDir()`/`dbPathZ()`) and exported to C as `zig_db_path()`.
2. **Helper-script dependency**: Dropbox/GitHub data transfer happens in shell scripts at `~/.config/qirtas/`, outside the repo — not installed by the build; a fresh machine will hit the "script missing" error.
3. **GitHub repo hardcoded** to `lawh-notes`; repo entry field in the UI is not wired.
4. `zig_github_connect()` in sync.zig writes a mock token to a nonexistent column — dead code (the real flow is the C device flow); harmless but should be deleted.
5. Sync scans only the app's **current working directory**, non-recursive.
6. Tokens are machine-bound (`/etc/machine-id` key derivation): restoring vault.db onto new hardware silently invalidates all sync credentials → `Error: missing credentials.` after decryption fails.
7. Diagnostics print to stdout only — launch from a terminal (`zig build run` or `./zig-out/bin/qirtas`) when investigating.
