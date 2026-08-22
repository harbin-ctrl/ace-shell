#!/bin/sh
# The real AROS Shell parser owns AmigaDOS 3.1 redirection.  This test talks
# to ace-user-shell rather than invoking commands directly, so it covers the
# parser, SelectInput()/SelectOutput(), and the fork/exec hand-off together.
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "$repo_dir/ace-shell-redirection.XXXXXX")
sys_dir="$test_dir/sys"
runtime_dir="$test_dir/run"
socket_path="$test_dir/broker.sock"
relative_dir=${test_dir##*/}
test_path="$relative_dir"
transcript="$test_dir/transcript"
expected="$test_dir/expected"
first="$test_dir/first"
second="$test_dir/second"
restored="$test_dir/restored"
output="$test_dir/output"
copy="$test_dir/copy"
lnx_report="$test_dir/lnx-report"
probe_path="$sys_dir/C/lnx-pty-probe"
broker_pid=

cleanup()
{
    if [ -n "$broker_pid" ] && kill -0 "$broker_pid" 2>/dev/null; then
        kill -TERM "$broker_pid" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "$broker_pid" 2>/dev/null || break
            sleep 0.01
        done
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT HUP INT TERM

fail()
{
    printf 'ACE shell redirection test: %s\n' "$1" >&2
    [ -f "$transcript" ] && cat "$transcript" >&2
    exit 1
}

mkdir -p "$sys_dir/C" "$sys_dir/S" "$sys_dir/Prefs/Env-Archive" \
         "$runtime_dir/ace/t"
cp "$repo_dir/build/Type" "$repo_dir/build/LNX" "$repo_dir/build/EndCLI" \
   "$repo_dir/build/lnx-pty-probe" \
   "$sys_dir/C/"
ACE_SYS_DIR="$sys_dir" XDG_RUNTIME_DIR="$runtime_dir" \
    "$repo_dir/build/ace-broker" "$socket_path" &
broker_pid=$!
for _ in $(seq 1 100); do
    [ -S "$socket_path" ] && break
    kill -0 "$broker_pid" 2>/dev/null || break
    sleep 0.05
done
[ -S "$socket_path" ] || fail 'the broker did not start'

printf 'first\nsecond\n' > "$expected"
printf 'first\n' > "$first"
printf 'second\n' > "$second"
printf 'restored\n' > "$restored"
printf '%s\n' \
    "Type $test_path/first > $test_path/output" \
    "Type $test_path/second >> $test_path/output" \
    "LNX cat < $test_path/output > $test_path/copy" \
    "LNX $probe_path report > $test_path/lnx-report" \
    'EndCLI' |
    ACE_BROKER_SOCKET="$socket_path" ACE_SESSION=shell-redirection \
    ACE_SYS_DIR="$sys_dir" XDG_RUNTIME_DIR="$runtime_dir" \
    "$repo_dir/build/ace-user-shell" >"$transcript" 2>&1 ||
    fail 'the shell did not complete the redirected commands'

cmp "$expected" "$output" || fail '> and >> did not produce the expected file'
cmp "$expected" "$copy" || fail '< and > did not connect the command streams'
grep -q '^isatty 0 0 0$' "$lnx_report" ||
    fail 'piped shell unexpectedly activated LNX PTY mode'
grep -q '^term xterm-256color$' "$lnx_report" ||
    fail 'official redirected LNX did not select the Debian xterm TERM contract'
grep -q '^pty-marker (unset)$' "$lnx_report" ||
    fail 'redirected LNX leaked the private PTY marker to its target'

# A failed redirection prevents its command from running, then the shell
# returns to its normal streams for the command that follows.
printf '%s\n' \
    "Type $test_path/first > $test_path/missing/output" \
    "Type $test_path/restored" \
    'EndCLI' |
    ACE_BROKER_SOCKET="$socket_path" ACE_SESSION=shell-redirection-failure \
    ACE_SYS_DIR="$sys_dir" XDG_RUNTIME_DIR="$runtime_dir" \
    "$repo_dir/build/ace-user-shell" >"$transcript" 2>&1 ||
    fail 'the shell did not recover after a failed redirection'
grep -q 'restored' "$transcript" ||
    fail 'the shell did not restore its normal output after a failed redirection'

printf 'ACE shell implements AmigaDOS 3.1 <, > and >> redirection\n'
