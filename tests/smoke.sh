#!/bin/sh
# Smoke tests for cpflow. POSIX sh, no dependencies beyond coreutils.
# usage: tests/smoke.sh [path-to-binary]
set -u

BIN=${1:-./cpflow}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=$((fail + 1)); }
num()  { tr -d " "; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

echo "== cpflow smoke tests: $BIN =="

# ---------------------------------------------------------------- fixtures --
mkdir -p "$TMP/src/sub/deep" "$TMP/src/.hidden" "$TMP/dst"
printf 'hello' > "$TMP/src/small.txt"
: > "$TMP/src/empty.bin"
dd if=/dev/zero of="$TMP/src/big.bin" bs=1024 count=3000 2>/dev/null
dd if=/dev/urandom of="$TMP/src/sub/mid.bin" bs=1024 count=200 2>/dev/null
printf 'deep' > "$TMP/src/sub/deep/leaf.txt"
printf 'dot' > "$TMP/src/.hidden/dotfile"
printf 'tr' > "$TMP/src/Türkçe-çğıöşü.txt"
ln -s small.txt "$TMP/src/link.txt" 2>/dev/null || true

# ------------------------------------------------------------------- basic --
"$BIN" "$TMP/src" "$TMP/dst" >/dev/null 2>&1
check "exit code 0" "$?" "0"
diff -r "$TMP/src" "$TMP/dst" >/dev/null 2>&1
check "tree identical (diff -r)" "$?" "0"
check "empty file copied" "$(wc -c < "$TMP/dst/empty.bin" | num)" "0"
check "large file size" "$(wc -c < "$TMP/dst/big.bin" | num)" "3072000"
check "unicode name" "$(cat "$TMP/dst/Türkçe-çğıöşü.txt")" "tr"
check "hidden dir" "$(cat "$TMP/dst/.hidden/dotfile")" "dot"
if [ -L "$TMP/src/link.txt" ]; then
  check "symlink target kept" "$(readlink "$TMP/dst/link.txt")" "small.txt"
fi

# -------------------------------------------------------------- timestamps --
sm=$(stat -c %Y "$TMP/src/sub/mid.bin" 2>/dev/null || stat -f %m "$TMP/src/sub/mid.bin")
dm=$(stat -c %Y "$TMP/dst/sub/mid.bin" 2>/dev/null || stat -f %m "$TMP/dst/sub/mid.bin")
check "mtime preserved" "$dm" "$sm"

# ------------------------------------------------------------------ resume --
printf 'changed' >> "$TMP/dst/small.txt"
"$BIN" "$TMP/src" "$TMP/dst" >/dev/null 2>&1
check "resume fixes a modified file" "$(cat "$TMP/dst/small.txt")" "hello"
before=$(stat -c %Y "$TMP/dst/big.bin" 2>/dev/null || stat -f %m "$TMP/dst/big.bin")
sleep 1
"$BIN" "$TMP/src" "$TMP/dst" >/dev/null 2>&1
after=$(stat -c %Y "$TMP/dst/big.bin" 2>/dev/null || stat -f %m "$TMP/dst/big.bin")
check "unchanged file not rewritten" "$after" "$before"

# ----------------------------------------------------------------- exclude --
rm -rf "$TMP/dst2" && mkdir "$TMP/dst2"
"$BIN" --exclude='*.bin' "$TMP/src" "$TMP/dst2" >/dev/null 2>&1
check "exclude: pattern skipped" "$(ls "$TMP/dst2" | grep -c '\.bin$')" "0"
check "exclude: other files kept" "$(cat "$TMP/dst2/small.txt")" "hello"

# ----------------------------------------------------------------- dry run --
rm -rf "$TMP/dst3" && mkdir "$TMP/dst3"
"$BIN" -n "$TMP/src" "$TMP/dst3" >/dev/null 2>&1
check "dry run copies nothing" "$(ls -A "$TMP/dst3" | wc -l | num)" "0"

# ---------------------------------------------------------------- piped mode --
out=$("$BIN" "$TMP/src" "$TMP/dst" 2>&1)
check "piped mode: no escape codes" "$(printf '%s' "$out" | grep -c "$(printf '\033')")" "0"

# ------------------------------------------------------------------- errors --
"$BIN" /nonexistent "$TMP/dst" >/dev/null 2>&1
check "missing source -> 2" "$?" "2"
"$BIN" "$TMP/src" /nonexistent >/dev/null 2>&1
check "missing dest -> 2" "$?" "2"
"$BIN" "$TMP/src" >/dev/null 2>&1
check "one argument -> 2" "$?" "2"
"$BIN" --version >/dev/null 2>&1
check "--version -> 0" "$?" "0"
"$BIN" --help >/dev/null 2>&1
check "--help -> 0" "$?" "0"

# ------------------------------------------------------------------ summary --
if [ "$fail" -eq 0 ]; then
  echo "== all checks passed =="
  exit 0
fi
echo "== $fail check(s) failed =="
exit 1
