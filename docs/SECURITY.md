# Qirtas Security — Honest Threat Model

**Status: the vault encryption in its current form protects against casual file
browsing only. It does NOT protect against device theft or disk imaging.
Do not describe Qirtas as "encrypted" or "privacy-first" in user-facing
material until the key-handling roadmap below is implemented and the crypto
path has been reviewed by a human with security experience.**

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

- [ ] Implement passphrase or libsecret unlock (above)
- [ ] Decide and document what happens to the plaintext working-directory files
- [ ] Human security review of `main.zig` key handling, `sync.zig`
      encryptToken/decryptToken, nonce handling, and the BIP-39 recovery path
- [ ] Zeroize master key on shutdown (currently only sync tokens are zeroized)
