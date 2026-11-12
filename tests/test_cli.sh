#!/bin/sh
#
# test_cli.sh - end-to-end checks for i3blocks
#
# Sanity-check the i3bar protocol output, term output, signal-driven exit,
# and draining of multiple persistent-block lines.

set -eu

: "${I3BLOCKS:=./i3blocks}"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/i3blocks-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

run_and_terminate()
{
	pid=$1
	sleep 1
	kill -TERM "$pid" 2>/dev/null || true
	wait "$pid"
}

cat > "$tmp/once.conf" <<'EOF'
[greetings]
command=printf 'hello\n'
interval=once
EOF

"$I3BLOCKS" -c "$tmp/once.conf" -o term >"$tmp/term.out" 2>"$tmp/term.err" &
run_and_terminate $!
grep -q 'hello' "$tmp/term.out"

"$I3BLOCKS" -c "$tmp/once.conf" >"$tmp/bar.out" 2>"$tmp/bar.err" &
run_and_terminate $!
grep -q '"full_text":"hello"' "$tmp/bar.out"

# A missing configuration file must fail cleanly, not crash.
if "$I3BLOCKS" -c "$tmp/missing.conf" -o term >"$tmp/missing.out" 2>"$tmp/missing.err"; then
	echo "missing configuration unexpectedly succeeded" >&2
	exit 1
fi
grep -q 'Failed to load configuration file' "$tmp/missing.err"

cat > "$tmp/persist.conf" <<'EOF'
[persist]
command=printf 'one\ntwo\nthree\n'; sleep 5
interval=persist
EOF

"$I3BLOCKS" -c "$tmp/persist.conf" -o term >"$tmp/persist.out" 2>"$tmp/persist.err" &
run_and_terminate $!
grep -q 'one' "$tmp/persist.out"
grep -q 'two' "$tmp/persist.out"
grep -q 'three' "$tmp/persist.out"

exit 0
