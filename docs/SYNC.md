# Qirtas Sync — Setup, Architecture & Troubleshooting

Four sync providers, all **on-demand** (fire on Save, app close, or the ● Sync Now button — never continuous):

| Provider | Mechanism | Where files go |
|---|---|---|
| Google Drive | OAuth loopback (port 12345) + Drive `appDataFolder` API, native Zig HTTP | Hidden app-data folder in your Drive |
| Dropbox | OAuth loopback (port 5173), native Zig HTTP (Dropbox API v2) | `/qirtas` folder in your Dropbox app scope |
| GitHub | Device flow (code shown in dialog), native Zig HTTP (Contents API) | Repo set via the repo-name field (default `qirtas-notes`, created private+auto-init if missing), one commit per changed file |
| Local folder | 3-way compared file copy | `~/QirtasSync` (override: `QIRTAS_LOCAL_SYNC_DIR`) |

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

Device flow needs only a client ID (no client secret). Qirtas ships with a usable default client ID; override with:

```sh
QIRTAS_GITHUB_CLIENT_ID="Iv1.xxxxxxxxxxxxxxxx" qirtas
```

To register your own OAuth App: GitHub → Settings → Developer settings → OAuth Apps → New OAuth App. Enable **Device Flow** in the app settings. No callback/redirect URL is needed for device flow.

Connect → a code dialog appears → enter it at github.com/login/device. Optionally type a repo name in the field on the GitHub card before connecting (defaults to `qirtas-notes`); if the repo doesn't exist it's created as private with `auto_init`.

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
| `Error: database unavailable.` | Could not open the vault DB at `~/.config/qirtas/vault.db` | Ensure the directory exists and is writable |
| `Error: Google Drive list/download/update/upload/metadata failed.` | The specific Drive API call failed after 3 retries (transient 5xx/429 are retried with backoff) | Terminal shows the HTTP status per attempt. 403 with valid auth = Drive API not enabled for your project |
| `Error: connection refused / unknown host / network unreachable / connection reset / no network addresses.` | Plain network failure at that step | Check connectivity/DNS/VPN |
| `Error: Dropbox list/download/upload failed.` | The specific Dropbox API call failed; terminal shows HTTP status + body | 401 → reconnect; 409 with `not_found` on download = file deleted remotely mid-sync, resync |
| `Error: GitHub auth check / list / download / upload failed.` | The specific Contents-API call failed; terminal shows status + body | 401/403 → token revoked or missing `repo` scope — reconnect; 404 on list = empty repo (normal first run) |
| `Error: GitHub upload conflict, resync.` | Remote blob sha changed between list and upload (simultaneous sync from another machine) | Press Sync Now again — second pass resolves via the 3-way logic |
| `Error: cannot open folder.` | `opendir(".")` failed — app's working directory vanished or lacks permission | Launch from a valid directory |
| `Error: cannot create sync folder.` / `sync target is not a directory.` | `~/QirtasSync` (or `QIRTAS_LOCAL_SYNC_DIR`) couldn't be created, or exists as a file | Remove/rename the blocking file |
| `Error: sync failed.` | Anything not mapped above | Terminal output has the real error name |

### Conflict behavior — unified (2026-06-12)

All four backends now use the same 3-way model: per-file last-synced state is
stored in the vault DB (`file_metadata` for Drive, `dropbox_sync_meta`,
`github_sync_meta`, `local_sync_meta`). On each sync:

- changed locally only → upload/copy out
- changed remotely only → download/copy in (open buffer reloads if clean)
- changed on BOTH sides → contents are compared first; if they actually
  differ, your local version is preserved as `<name>_conflict.<ext>` and the
  remote version takes the original filename. The status card shows
  `Synced ✓ (N conflicts saved)`. Merge by hand.

No backend silently discards edits anymore. History: before 2026-06-12 the
Dropbox script downloaded remote over local unconditionally and Local was
newest-mtime-wins — both could destroy edits. That code is gone (the bash
helper scripts were replaced by native Zig HTTP entirely).

GitHub bonus: every upload is a commit, so even conflict losers remain
recoverable from repo history.

---|---|---|
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

1. ~~Hardcoded DB path~~ **Fixed (2026-06-12):** everything now lives in `$XDG_CONFIG_HOME/qirtas/` (default `~/.config/qirtas/`) — vault.db and the sync scripts. Path is resolved once in `main.zig` (`configDir()`/`dbPathZ()`) and exported to C as `zig_db_path()`.
2. ~~Helper-script dependency~~ **Fixed (2026-06-12):** Dropbox and GitHub sync are native Zig HTTP — no bash, no curl/jq, nothing outside the binary. (`curl` is still used by the GitHub *connect* device flow in `gui_sync.c`; sync itself needs nothing.) Leftover scripts in `~/.config/qirtas/` are unused and can be deleted.
3. ~~GitHub repo hardcoded~~ **Fixed (2026-06-12):** repo-name field on the GitHub sync card is wired; defaults to `qirtas-notes` if left blank.
4. ~~`zig_github_connect()` dead mock-token path~~ **Fixed (2026-06-12):** removed (the real flow is the C device flow).
5. Sync scans only the app's **current working directory**, non-recursive.
6. Deletion does not propagate: removing a file on one side resurrects it from the other on next sync.
6. Tokens are machine-bound (`/etc/machine-id` key derivation): restoring vault.db onto new hardware silently invalidates all sync credentials → `Error: missing credentials.` after decryption fails.
7. Diagnostics print to stdout only — launch from a terminal (`zig build run` or `./zig-out/bin/qirtas`) when investigating.
