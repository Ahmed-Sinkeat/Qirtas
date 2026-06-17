# Qirtas Security — Honest Threat Model

**Status: the vault encryption in its current form protects against casual file
browsing only. It does NOT protect against device theft or disk imaging.
Do not describe Qirtas as "encrypted" or "privacy-first" in user-facing
material until the key-handling roadmap below is implemented and the crypto
path has been reviewed by a human with security experience.**

## Chosen direction (2026-06-12)

Per review: Qirtas will **not** climb toward being a cipher app. The coherent,
honest position is the one the files already imply — *your notes are plain
markdown files you own*. The plan:

1. Move **sync tokens** into the system keyring (libsecret → GNOME
   Keyring/KWallet). That's the only secret worth real protection.
2. The vault encryption stays as-is, quietly covering what it covers today
   (casual browsing). Never marketed.
3. BIP-39 machinery stays dormant; do not expand it.
4. Marketing sentence: *"Notes are plain .md files on your disk; sync
   credentials are stored in your system keyring."* Nothing more.

The roadmap below is kept for reference but is no longer the plan of record.

## What exists today

```
/etc/machine-id  (world-readable, same disk)
      │ SHA-256
      ▼
machine key (32 bytes)
      │ ChaCha20Poly1305
      ▼
encrypted master key  ──── stored in vault.db  (same disk)
      │ unlock at startup
      ▼
master key (32 bytes, random, in memory)
      │ ChaCha20Poly1305 per file
      ▼
encrypted note content in vault.db
```

- Note content and OAuth tokens are genuinely encrypted with ChaCha20Poly1305
  under a random 32-byte master key. The cryptography itself is fine.
- The problem is the **key chain anchor**: the master key is unlocked by a key
  derived solely from `/etc/machine-id`, which is world-readable and sits on
  the same disk as the vault.

## What this protects against

| Threat | Protected? |
|---|---|
| Someone casually opening vault.db in a text editor / sqlite browser | ✅ yes |
| Cloud provider reading synced ciphertext (if ciphertext is what's synced) | ✅ partially — but plaintext `.md` files in the working directory are what sync actually uploads |
| **Stolen laptop / imaged disk** | ❌ no — attacker has vault.db AND machine-id, can derive the unlock key in seconds |
| Another user account on the same machine | ❌ mostly no — machine-id is world-readable; only file permissions on vault.db (0600) stand in the way |
| Malware running as your user | ❌ no (nothing can protect against this while the app can read its own key) |
| Root on the machine | ❌ no |

Note also: the **working directory holds plaintext `.md` files** — the vault
encryption covers DB-stored content, not the markdown files on disk that sync
uploads. Any "encrypted at rest" claim must account for this.

## Decrypt-fail data-loss failure mode (FIXED 2026-06-17)

**Severity: was critical — now mitigated.** The machine-id anchor is still a
confidentiality weakness (see roadmap below), but it is no longer a *data-loss*
trigger. Background: because the master key is unlocked from `/etc/machine-id`,
anything that changes that anchor (moving the vault to a new machine, restoring a
backup on different hardware, an OS reinstall, a container with a fresh
machine-id) makes `decryptToken` fail at startup, leaving `active_master_key`
null (the key is not regenerated when the `system_keys` row already exists).

Previously, with a null/wrong key, `load_file_mmap` loaded the raw
`nonce+ciphertext+tag` bytes into the editor as plaintext (`active_file_is_encrypted
= false`), and because the format had no header nothing detected the failure — so
the next autosave overwrote the recoverable ciphertext with garbage.

**Fix shipped:** the encrypted on-disk format now starts with a magic + version
header (`ENC_MAGIC` / `ENC_VERSION` in `src/main.zig`). `load_file_mmap` now:

- File carries the header → it **must** decrypt. On no-key/wrong-key it sets the
  `active_load_failed` flag, loads the document empty, and surfaces a read-only
  "Cannot decrypt — wrong key" toast/status. The ciphertext on disk is left
  untouched.
- Both save paths (`zig_save_active_page`, `zig_save_document`) refuse to write
  while `active_load_failed` is set — autosave can no longer clobber the original.
- Headerless (legacy, pre-2026-06-17) files still load via tag-verified trial
  decryption and self-upgrade to the header format on their next successful save.

Regression test: *"integration: encrypted file with wrong key is read-only, save
refuses, ciphertext survives"* in `src/main.zig`. Tracked in `docs/ISSUES.md` #1.

## The BIP-39 recovery phrase

The 24-word mnemonic (+ optional passphrase) is a **recovery** mechanism for
the master key, not a day-to-day unlock. It does not change the threat model
above.

## Roadmap to make the claim true (in order of preference)

1. **User passphrase unlock** — derive the unlock key with Argon2id from a
   passphrase prompted at startup. The DB schema already reserves
   `encrypted_master_key_passphrase` and `passphrase_salt` columns in
   `system_keys` — the storage side was planned, the prompt and KDF were never
   implemented. This is the strongest option (survives disk theft) at the cost
   of a startup prompt.
2. **System keyring (libsecret)** — store the master key in GNOME
   Keyring/KWallet. Ties unlock to the user's login session; survives disk
   theft if the login password is decent. Weaker than option 1 against a
   logged-in attacker, but zero friction.
3. Both: keyring by default, passphrase as a hardening toggle.

Until one of these ships, accurate wording is: *"notes are obfuscated at rest;
encryption keys are stored on the same device."*

## Required before any public "encrypted" claim

- [x] **Fix the decrypt-fail → plaintext-overwrite data-loss bug** (magic header + read-only on decrypt failure) — done 2026-06-17, see "Decrypt-fail data-loss failure mode"
- [ ] Implement passphrase or libsecret unlock (above)
- [ ] Decide and document what happens to the plaintext working-directory files
- [ ] Human security review of `main.zig` key handling, `sync.zig`
      encryptToken/decryptToken, nonce handling, and the BIP-39 recovery path
- [ ] Zeroize master key on shutdown (currently only sync tokens are zeroized)
