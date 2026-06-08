# Recovery Layers & File Encryption Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a Master Key encryption architecture for Qirtas where files are encrypted on disk using a Master Key, and the Master Key itself is locked by three independent layers: machine-id (zero-friction daily use), a 24-word BIP-39 recovery code (emergency), and an optional passphrase.

**Architecture:** 
- A 32-byte cryptographically secure random `Master Key` is generated on first startup.
- The `Master Key` serves as the entropy for a 24-word BIP-39 mnemonic recovery code displayed to the user once.
- The `Master Key` is stored in the SQLite database (`vault.db`) in a new table `system_keys`, encrypted with a key derived from `machine-id` (Lock 1) and optionally encrypted with a key derived from a passphrase (Lock 2).
- Files on disk are encrypted using ChaCha20Poly1305 with the active `Master Key`. On tab load, files are decrypted; on save, they are encrypted.
- On a new machine, if the `machine-id` decryption fails, the user is prompted to input their 24-word mnemonic (or optional passphrase) to recover the `Master Key` and re-encrypt it for the new machine.

**Tech Stack:**
- Zig (`src/main.zig`, `src/sync.zig`) for cryptographic logic, KDF, database queries, and file I/O.
- C (`src/gui.c`) for GTK4/Adwaita user interface setup, modal prompts, and configurations.

---

### Task 1: BIP-39 Mnemonic Module in Zig

**Files:**
- Create: `src/bip39.zig`
- Modify: `build.zig` (include `src/bip39.zig` if needed, or import it in `src/main.zig`)

**Step 1: Write the failing test**
Create a test file `src/bip39.zig` that generates a mnemonic from random entropy, parses it back, and verifies the round-trip matches the original entropy.

```zig
const std = @import("std");
const bip39 = @This();

test "BIP-39 entropy to mnemonic and back" {
    const entropy = [_]u8{0x00} ** 32;
    var mnemonic_buf: [24][]const u8 = undefined;
    
    // We expect standard BIP-39 mapping for 32 bytes of zeros to be 24 "abandon" words
    try bip39.entropyToMnemonic(entropy, &mnemonic_buf);
    for (mnemonic_buf) |word| {
        try std.testing.expectEqualStrings("abandon", word);
    }
    
    const parsed_entropy = try bip39.mnemonicToEntropy(&mnemonic_buf);
    try std.testing.expectEqualSlices(u8, &entropy, &parsed_entropy);
}
```

**Step 2: Run test to verify it fails**
Run: `zig test src/bip39.zig`
Expected: FAIL (compilation error, functions not defined)

**Step 3: Write minimal implementation**
Write the BIP-39 module in `src/bip39.zig`. It should contain the standard 2048 English wordlist embedded as a static array and implement the bit-splitting and SHA-256 checksum logic.

```zig
const std = @import("std");

pub const wordlist = [_][]const u8{
    "abandon", "ability", "able", "about", "above", "abroad", "absorb", "abstract", "absurd", "abuse",
    // ... [Include standard BIP-39 English wordlist of 2048 words] ...
};

pub fn entropyToMnemonic(entropy: [32]u8, out_words: *[24][]const u8) !void {
    // 1. Calculate SHA-256 checksum (first 8 bits of hash)
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&entropy, &hash, .{});
    const checksum = hash[0];

    // 2. Concatenate 256 bits of entropy + 8 bits checksum = 264 bits
    var bits: [264]bool = undefined;
    for (entropy, 0..) |byte, i| {
        for (0..8) |b| {
            bits[i * 8 + b] = ((byte >> @intCast(7 - b)) & 1) == 1;
        }
    }
    for (0..8) |b| {
        bits[256 + b] = ((checksum >> @intCast(7 - b)) & 1) == 1;
    }

    // 3. Split into 24 chunks of 11 bits and map to wordlist indices
    for (0..24) |i| {
        var index: u11 = 0;
        for (0..11) |b| {
            index = (index << 1) | @intFromBool(bits[i * 11 + b]);
        }
        out_words[i] = wordlist[index];
    }
}

pub fn mnemonicToEntropy(words: []const []const u8) ![32]u8 {
    if (words.len != 24) return error.InvalidMnemonicLength;

    // 1. Map words back to 11-bit indices
    var bits: [264]bool = undefined;
    for (words, 0..) |word, i| {
        const index = try findWordIndex(word);
        for (0..11) |b| {
            bits[i * 11 + b] = ((index >> @intCast(10 - b)) & 1) == 1;
        }
    }

    // 2. Extract entropy (first 256 bits)
    var entropy: [32]u8 = undefined;
    for (0..32) |i| {
        var byte: u8 = 0;
        for (0..8) |b| {
            byte = (byte << 1) | @intFromBool(bits[i * 8 + b]);
        }
        entropy[i] = byte;
    }

    // 3. Verify checksum (last 8 bits)
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&entropy, &hash, .{});
    const expected_checksum = hash[0];

    var actual_checksum: u8 = 0;
    for (0..8) |b| {
        actual_checksum = (actual_checksum << 1) | @intFromBool(bits[256 + b]);
    }

    if (expected_checksum != actual_checksum) return error.InvalidChecksum;
    return entropy;
}

fn findWordIndex(word: []const u8) !u11 {
    for (wordlist, 0..) |w, i| {
        if (std.mem.eql(u8, w, word)) return @intCast(i);
    }
    return error.WordNotFound;
}
```

**Step 4: Run test to verify it passes**
Run: `zig test src/bip39.zig`
Expected: PASS

**Step 5: Commit**
```bash
git add src/bip39.zig
git commit -m "feat: add BIP-39 mnemonic key derivation module"
```

---

### Task 2: Database Schema & Key Storage Initialization

**Files:**
- Modify: `src/main.zig`
- Modify: `src/sync.zig`

**Step 1: Write a test verifying system key table creation**
In `src/main.zig`, add database migration test ensuring table `system_keys` exists and has required columns.

**Step 2: Run test to verify it fails**
Run: `zig build test`
Expected: FAIL (table not initialized)

**Step 3: Write minimal implementation**
Modify DB connection routine in `src/main.zig` to execute:
```sql
CREATE TABLE IF NOT EXISTS system_keys (
    id INTEGER PRIMARY KEY CHECK (id = 1), -- Ensure single row
    encrypted_master_key_machine TEXT NOT NULL,
    encrypted_master_key_passphrase TEXT,
    passphrase_salt TEXT
);
```

**Step 4: Run test to verify it passes**
Run: `zig build test`
Expected: PASS

**Step 5: Commit**
```bash
git add src/main.zig src/sync.zig
git commit -m "db: create system_keys schema for secure master key persistence"
```

---

### Task 3: Master Key Derivation & File Encryption Hooks

**Files:**
- Modify: `src/main.zig`
- Modify: `src/sync.zig`

**Step 1: Add unit tests for file encryption/decryption**
Write a test in `src/main.zig` that encrypts a mock document slice using the Master Key, writes it, reads it back, decrypts it, and verifies the content matches.

**Step 2: Run test to verify it fails**
Expected: FAIL

**Step 3: Implement file encryption/decryption**
- Define `var active_master_key: ?[32]u8 = null;` globally in Zig.
- Implement FFI helper to derive the master key from the database:
  - If a record exists in `system_keys`, decrypt `encrypted_master_key_machine` with `machine-id` key to populate `active_master_key`.
  - If decryption fails, throw a recovery error back to C.
  - If no record exists, generate a cryptographically secure 32-byte random key, encrypt it with `machine-id` key, save it to the DB, and export the BIP-39 mnemonic string to C to show the user.
- Update `load_file_mmap` to decrypt the file payload with `active_master_key` using ChaCha20Poly1305.
- Update `zig_save_active_page` to encrypt the payload with `active_master_key` using ChaCha20Poly1305 before writing it to disk.

**Step 4: Run test to verify it passes**
Run: `zig build test`
Expected: PASS

**Step 5: Commit**
```bash
git commit -am "crypto: secure file read/write operations using Master Key encryption"
```

---

### Task 4: Setup & Recovery UI in GTK

**Files:**
- Modify: `src/gui.c`
- Modify: `src/gui_shared.h`

**Step 1: Create UI mockups and check compiler**
Ensure that the C FFI bindings compile and link.

**Step 2: Run compiler to verify success**
Run: `zig build`
Expected: PASS

**Step 3: Implement Setup & Recovery Views in C**
- If Zig returns a mnemonic setup code on startup, show a full-window Adwaita dialog displaying the 24 words. Implement a validation step requesting the user to select/input 3 randomly chosen words from the phrase before allowing them to access the editor.
- If Zig returns a decryption failure (hardware mismatch) on startup, intercept window loading and show a recovery overlay dialog prompting the user to enter their 24-word recovery phrase.
- Implement recovery verification: pass the entered words to Zig FFI. If verified, re-encrypt the Master Key with the current `machine-id`, update the DB, and load the editor workspace cleanly.

**Step 4: Verify manual testing of recovery views**
Run manual verification of the recovery screen (by modifying `machine-id` key or forcing recovery flow in code).

**Step 5: Commit**
```bash
git commit -am "ui: implement BIP-39 first-time setup and hardware recovery screens"
```

---

### Task 5: Passphrase Key Escrow Option

**Files:**
- Modify: `src/gui.c`
- Modify: `src/main.zig`
- Modify: `src/sync.zig`

**Step 1: Write tests for Passphrase KDF & Encryption**
Add a test in `src/sync.zig` verifying that the Master Key can be encrypted with a passphrase, stored, and successfully recovered using only the passphrase.

**Step 2: Run test to verify it fails**
Expected: FAIL

**Step 3: Implement Optional Passphrase**
- In the C settings panel, add a section "Set Master Passphrase".
- When a user inputs a passphrase, derive a 32-byte key from it using PBKDF2/SHA-256 with a random salt.
- Encrypt the `Master Key` with the derived key and store `encrypted_master_key_passphrase` and `passphrase_salt` in `vault.db`.
- Update the recovery flow: if recovery is needed, allow the user to input either their 24-word code OR their passphrase to decrypt and restore the master key.

**Step 4: Verify all tests pass**
Run: `zig build test`
Expected: PASS

**Step 5: Commit**
```bash
git commit -am "feat: implement optional passphrase encryption escrow layer"
```
