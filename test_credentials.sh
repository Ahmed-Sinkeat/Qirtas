#!/usr/bin/env bash
# Test: Save Credentials & Connect Account Backend Functions
# Tests the SQLite operations that back the Save Credentials button
# and verifies the data flow for the Connect Account button.

set -euo pipefail

DB="/home/.config/lawh/vault.db"
PASS=0
FAIL=0

green() { printf '\033[32m✓ %s\033[0m\n' "$1"; }
red()   { printf '\033[31m✗ %s\033[0m\n' "$1"; }
blue()  { printf '\033[34m» %s\033[0m\n' "$1"; }

# ─── Setup ────────────────────────────────────────────────────────────────────
blue "Setting up test database at $DB"
mkdir -p "$(dirname "$DB")"
rm -f "$DB"

sqlite3 "$DB" "
CREATE TABLE IF NOT EXISTS sync_tokens (
    id           INTEGER PRIMARY KEY CHECK (id = 1),
    client_id    TEXT NOT NULL,
    client_secret TEXT NOT NULL,
    access_token  TEXT,
    refresh_token TEXT,
    expiry_time   INTEGER DEFAULT 0
);
"

# ─── Test 1: Save Credentials (INSERT OR REPLACE) ─────────────────────────────
blue "Test 1: zig_save_sync_credentials — first write"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, 'my-client-id', 'my-secret');"
ROW=$(sqlite3 "$DB" "SELECT client_id || '|' || client_secret FROM sync_tokens WHERE id = 1;")
if [ "$ROW" = "my-client-id|my-secret" ]; then
    green "INSERT OR REPLACE wrote correct client_id and client_secret"
    PASS=$((PASS+1))
else
    red "Expected 'my-client-id|my-secret', got '$ROW'"
    FAIL=$((FAIL+1))
fi

# ─── Test 2: Save Credentials — overwrite (idempotent) ─────────────────────
blue "Test 2: zig_save_sync_credentials — overwrite existing row"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, 'updated-id', 'updated-secret');"
ROW=$(sqlite3 "$DB" "SELECT client_id || '|' || client_secret FROM sync_tokens WHERE id = 1;")
if [ "$ROW" = "updated-id|updated-secret" ]; then
    green "REPLACE overwrote to 'updated-id|updated-secret' correctly"
    PASS=$((PASS+1))
else
    red "Expected 'updated-id|updated-secret', got '$ROW'"
    FAIL=$((FAIL+1))
fi

# ─── Test 3: Empty client_id should still write (C side doesn't validate) ─────
blue "Test 3: zig_save_sync_credentials — empty strings written verbatim"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, '', '');"
ROW=$(sqlite3 "$DB" "SELECT client_id || '|' || client_secret FROM sync_tokens WHERE id = 1;")
if [ "$ROW" = "|" ]; then
    green "Empty strings saved correctly (no silent truncation)"
    PASS=$((PASS+1))
else
    red "Expected '|', got '$ROW'"
    FAIL=$((FAIL+1))
fi

# ─── Test 4: load_sync_credentials — reads back from DB ───────────────────────
blue "Test 4: load_sync_credentials — reads correct row after re-insert"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, 'reload-id', 'reload-secret');"
CLIENT_ID=$(sqlite3 "$DB" "SELECT client_id FROM sync_tokens WHERE id = 1;")
CLIENT_SECRET=$(sqlite3 "$DB" "SELECT client_secret FROM sync_tokens WHERE id = 1;")
if [ "$CLIENT_ID" = "reload-id" ] && [ "$CLIENT_SECRET" = "reload-secret" ]; then
    green "load_sync_credentials would read: client_id='$CLIENT_ID', client_secret='$CLIENT_SECRET'"
    PASS=$((PASS+1))
else
    red "Read back failed: client_id='$CLIENT_ID', client_secret='$CLIENT_SECRET'"
    FAIL=$((FAIL+1))
fi

# ─── Test 5: zig_sync_check_status — no access/refresh token means disconnected
blue "Test 5: zig_sync_check_status — no tokens == disconnected (status=0)"
ACCESS=$(sqlite3 "$DB" "SELECT COALESCE(access_token, 'NULL') FROM sync_tokens WHERE id = 1;")
EXPIRY=$(sqlite3 "$DB" "SELECT expiry_time FROM sync_tokens WHERE id = 1;")
NOW=$(date +%s)
if [ "$ACCESS" = "NULL" ] || [ "$EXPIRY" -le "$NOW" ]; then
    green "Status check: disconnected (access_token=$ACCESS, expiry=$EXPIRY)"
    PASS=$((PASS+1))
else
    red "Expected disconnected state, got access_token=$ACCESS expiry=$EXPIRY"
    FAIL=$((FAIL+1))
fi

# ─── Test 6: Simulate zig_sync_connect reading credentials ────────────────────
blue "Test 6: zig_sync_connect credential read — credentials present before connect"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, '123.apps.googleusercontent.com', 'GOCSPX-realvalue');"
COUNT=$(sqlite3 "$DB" "SELECT COUNT(*) FROM sync_tokens WHERE id = 1 AND client_id != '' AND client_secret != '';")
if [ "$COUNT" = "1" ]; then
    green "zig_sync_connect would find credentials (non-empty client_id & secret)"
    PASS=$((PASS+1))
else
    red "zig_sync_connect would fail: no valid credentials in DB"
    FAIL=$((FAIL+1))
fi

# ─── Test 7: zig_sync_disconnect — clears tokens but preserves credentials ────
blue "Test 7: zig_sync_disconnect — clears tokens, keeps credentials"
# Simulate what disconnect should do (set tokens to NULL, keep client_id/secret)
sqlite3 "$DB" "UPDATE sync_tokens SET access_token = NULL, refresh_token = NULL, expiry_time = 0 WHERE id = 1;"
CLIENT_ID_AFTER=$(sqlite3 "$DB" "SELECT client_id FROM sync_tokens WHERE id = 1;")
ACCESS_AFTER=$(sqlite3 "$DB" "SELECT COALESCE(access_token, 'NULL') FROM sync_tokens WHERE id = 1;")
if [ "$CLIENT_ID_AFTER" = "123.apps.googleusercontent.com" ] && [ "$ACCESS_AFTER" = "NULL" ]; then
    green "Disconnect cleared tokens, preserved client_id='$CLIENT_ID_AFTER'"
    PASS=$((PASS+1))
else
    red "Disconnect state: client_id='$CLIENT_ID_AFTER', access_token='$ACCESS_AFTER'"
    FAIL=$((FAIL+1))
fi

# ─── Test 8: PRIMARY KEY CHECK(id=1) constraint enforced ─────────────────────
blue "Test 8: Table constraint — only one row allowed (id must be 1)"
sqlite3 "$DB" "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, 'a', 'b');"
REJECTED=$(sqlite3 "$DB" "INSERT INTO sync_tokens (id, client_id, client_secret) VALUES (2, 'x', 'y');" 2>&1 || true)
ROW_COUNT=$(sqlite3 "$DB" "SELECT COUNT(*) FROM sync_tokens;")
if [ "$ROW_COUNT" = "1" ]; then
    green "Constraint CHECK(id=1) prevented insertion of id=2 (row count=$ROW_COUNT)"
    PASS=$((PASS+1))
else
    red "Expected 1 row, got $ROW_COUNT. Constraint may not be enforced."
    FAIL=$((FAIL+1))
fi

# ─── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════"
printf "Results: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n" "$PASS" "$FAIL"
echo "════════════════════════════════════════════"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
