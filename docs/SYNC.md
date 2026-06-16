# Qirtas Sync — Setup, Architecture & Troubleshooting

Four sync providers, all **on-demand** (fire on Save, app close, or the ● Sync Now button — never continuous):

| Provider | Mechanism | Where files go |
|---|---|---|
| Google Drive | OAuth loopback (port 12345) + PKCE (S256) + Drive `appDataFolder` API, native Zig HTTP | Hidden app-data folder in your Drive |
| Dropbox | OAuth loopback (port 5173) + PKCE (S256), native Zig HTTP (Dropbox API v2) | `/qirtas` folder in your Dropbox app scope |
| GitHub | **Personal Access Token (recommended)** or browser device flow, native Zig HTTP (Contents API) | Repo set via the repo-name field (default `qirtas-notes`, created private+auto-init if missing), one commit per changed file |
| Local folder | 3-way compared file copy | `~/QirtasSync` (override: `QIRTAS_LOCAL_SYNC_DIR`) |

> **Easiest paths for non-technical users:** **GitHub via Personal Access Token** (paste one token, nothing to configure) or the **Local folder** (no accounts at all). Google Drive and Dropbox require you to register your own OAuth app first (see §1).

> **All provider HTTP requests send `Accept-Encoding: identity`** (`accept_encoding = .omit`). GitHub/Google/Dropbox gzip responses by default, and the native Zig HTTP client does not auto-decompress on the raw reader — gzipped JSON used to surface as `Error: SyntaxError`. Fixed across every sync call (2026-06-16).

Synced file types: `.md .txt .zig .zon .c .h` in the **current working directory** of the app.
Tokens are stored in the vault DB encrypted with ChaCha20Poly1305 under a key derived from `/etc/machine-id` (= tokens don't survive copying the DB to another machine — by design).

---

## 1. One-time setup (the part that was missing)

Qirtas does **not** ship OAuth app credentials for Google Drive or Dropbox. Until you provide them those two cannot connect — the Connect button will say `Needs Google setup — see Help below` / `Needs Dropbox setup — see Help below`. **GitHub needs no setup: paste a Personal Access Token (see below).**

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

### GitHub — Personal Access Token (recommended)

This is the reliable path. A PAT with the `repo` scope can **both create the repo and push to it**, with no OAuth-app registration and no install/permission dance.

1. On the GitHub card, click **"Get a token from GitHub →"** (opens `github.com/settings/tokens/new` with the `repo` scope pre-selected). Or, for a fine-grained token, grant **Contents: Read and write** on the target repo.
2. Click **Generate token**, copy it (`ghp_…` classic, or `github_pat_…` fine-grained).
3. Paste it into the **token field** on the GitHub card. Optionally set a repo name (defaults to `qirtas-notes`).
4. Click **Connect to GitHub** → it verifies the token against `GET /user` → **Connected**.
5. **Sync Now** creates the repo if missing (private, `auto_init`) and uploads each changed file as a commit.

The token is stored encrypted in the vault DB (same ChaCha20-Poly1305 as all other secrets).

### GitHub — browser device flow (fallback)

Leave the token field **empty** and click Connect to use the browser device flow instead. Qirtas ships a default GitHub **App** client ID; override with:

```sh
QIRTAS_GITHUB_CLIENT_ID="Iv23li…" qirtas
```

Connect → a code dialog appears → enter it at github.com/login/device.

> **Caveat:** the bundled client ID is a **GitHub App**, whose user-to-server token can only write to repos the App is installed on with **Contents: write** permission. If sync fails with `404 Not Found` on upload even though the repo exists, the App lacks write access to it — use the Personal Access Token path above instead. Repo creation via the App token returns `403 "Resource not accessible by integration"` and is skipped (a PAT does not have this limit).

### Local folder

Nothing to set up. Files mirror into `~/QirtasSync`; point Syncthing/Nextcloud at that folder.

---

## 2. Troubleshooting — status message → cause → fix

The status text on each sync card is the primary diagnostic. Run the app from a terminal for extra detail: workers print the failing step and HTTP status to stdout (`Search attempt N failed…`, `Token refresh failed with status …: <body>`).

### Connect-phase messages

| Status text | What actually happened | Fix |
|---|---|---|
| `Needs Google setup — see Help below` / `Needs Dropbox setup — see Help below` | No OAuth app ID configured (the placeholder is still in place) | Do §1 setup (`QIRTAS_GOOGLE_CLIENT_ID` / `QIRTAS_DROPBOX_APP_KEY`) |
| `Paste a token first.` (GitHub) | Clicked Connect with an empty token field, and device-flow fallback couldn't start | Paste a Personal Access Token, or leave empty to use the browser flow |
| `Verifying token...` (stuck) (GitHub) | `GET /user` with the pasted token is hanging | Check network; if it never resolves the token line will switch to an error |
| `Token rejected. Needs 'repo' scope.` (GitHub) | The pasted token reached GitHub but was refused (expired, revoked, or missing the `repo` / Contents:write scope) | Generate a new token with the `repo` scope (classic) or Contents: Read+write (fine-grained) |
| `Cannot reach GitHub. Check connection.` (GitHub) | Token verification couldn't reach api.github.com | Check network/proxy |
| `Waiting for browser...` (stuck) | Browser never hit the local callback port — auth page abandoned, wrong redirect URI in the provider console, or the browser is sandboxed (Flatpak/Snap) and can't reach localhost | Finish the consent page; verify redirect URI is byte-exact (`http://localhost:12345` / `:5173`); try a non-sandboxed browser |
| `Sign-in timed out. Try again.` | Loopback listener waited 5 min, no callback | Same as above — redo Connect |
| `Sign-in port busy. Close other apps, retry.` | Another process owns the callback port (5173 is Vite's default dev port!) | `ss -tlnp \| grep 5173` — stop that process, reconnect |
| `Error: token exchange failed.` | Google/Dropbox rejected the code→token exchange. Now that PKCE (S256) is sent, the common remaining causes are: expired/reused code, wrong app type, or a PKCE method mismatch in the provider console | Reconnect once; if persistent, check terminal for the HTTP body, verify app type + redirect URI in the console |
| `Couldn't reach GitHub. Check connection.` (GitHub device flow) | Device-code request to github.com failed — offline, or `curl` not installed (the device flow shells out to curl) | `which curl`; check network |
| `Couldn't start sign-in. Try again.` | The local OAuth callback socket couldn't be created or bound | Retry; check no firewall blocks localhost |

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

### Rename detection — Google Drive only (2026-06-15)

Renaming a file locally used to look like a brand-new file (uploaded as a
duplicate) **and** a deleted cloud file (the old name gets downloaded back,
resurrecting it). Drive sync now catches this: if a tracked cloud file's
local counterpart has disappeared, and some other local file's MD5 content
hash matches that cloud file's `md5Checksum`, Qirtas treats it as a rename —
the Drive file is renamed in place (metadata-only PATCH, no re-upload) and
`file_metadata` is updated to the new filename. No duplicate, no resurrection.

This only fires when the file's **content is unchanged** (a pure
rename/move). Rename + edit in the same sync cycle falls back to the old
behavior. Dropbox, GitHub, and Local folder are unchanged — renames on those
backends still duplicate + resurrect (see limitation 6 below).

---

## 3. Known limitations / sharp edges

1. ~~Hardcoded DB path~~ **Fixed (2026-06-12):** everything now lives in `$XDG_CONFIG_HOME/qirtas/` (default `~/.config/qirtas/`) — vault.db and the sync scripts. Path is resolved once in `main.zig` (`configDir()`/`dbPathZ()`) and exported to C as `zig_db_path()`.
2. ~~Helper-script dependency~~ **Fixed (2026-06-12):** Dropbox and GitHub sync are native Zig HTTP — no bash, no curl/jq, nothing outside the binary. (`curl` is still used by the GitHub *connect* device flow in `gui_sync.c`; sync itself needs nothing.) Leftover scripts in `~/.config/qirtas/` are unused and can be deleted.
3. ~~GitHub repo hardcoded~~ **Fixed (2026-06-12):** repo-name field on the GitHub sync card is wired; defaults to `qirtas-notes` if left blank.
4. ~~`zig_github_connect()` dead mock-token path~~ **Fixed (2026-06-12):** removed (the real flow is the C device flow).
5. Sync scans only the app's **current working directory**, non-recursive.
6. Deletion does not propagate: removing a file on one side resurrects it from the other on next sync. ~~Renames~~ **Partially fixed for Google Drive (2026-06-15):** a pure rename (same content, new filename) is detected via MD5 hash match and applied as a rename on Drive — no duplicate, no resurrection. A true *deletion* (nothing replaces the file) still resurrects on next sync, on every backend. Dropbox/GitHub/Local: renames still behave like delete+create (duplicate + resurrection).
7. Tokens are machine-bound (`/etc/machine-id` key derivation): restoring vault.db onto new hardware silently invalidates all sync credentials → `Error: missing credentials.` after decryption fails.
8. Diagnostics print to stdout only — launch from a terminal (`zig build run` or `./zig-out/bin/qirtas`) when investigating.
9. ~~gzip responses broke JSON parsing~~ **Fixed (2026-06-16):** every sync HTTP call now sends `Accept-Encoding: identity` (`accept_encoding = .omit` on the request options). Previously gzipped API responses parsed as `Error: SyntaxError`.
10. ~~Google Drive / Dropbox OAuth missing PKCE~~ **Fixed (2026-06-16):** public-client token exchanges now send `code_challenge`/`code_challenge_method=S256` (auth URL) + `code_verifier` (exchange), generated by `zig_pkce_challenge()` in `sync.zig`. Without PKCE the providers rejected the exchange. **You must register the redirect URI's app as a "Desktop"/native (PKCE) client, or keep the Web-application client and ensure PKCE is allowed.**
11. ~~GitHub App token cannot write~~ **Worked around (2026-06-16):** the bundled GitHub *App* client ID yields a user-to-server token that 404s on writes to repos it isn't installed on. The GitHub card now accepts a **Personal Access Token** (`zig_github_connect_with_token()`), which creates the repo and pushes with no app-permission limits. Device flow remains as a fallback.
