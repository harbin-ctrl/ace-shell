#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/ace-say-test.XXXXXX")
socket="$runtime_dir/broker.sock"
pid_file="$runtime_dir/voice.pid"
broker_pid=''
fail() { echo "say broker test: $*" >&2; exit 1; }
cleanup() {
    [ -z "$broker_pid" ] || kill "$broker_pid" 2>/dev/null || true
    rm -rf "$runtime_dir"
}
trap cleanup EXIT INT TERM

ACE_BROKER_SOCKET="$socket" ACE_SAY_VOICE_SERVER="$repo_dir/tests/say_voice_fixture.sh" SAY_TEST_PID_FILE="$pid_file" "$repo_dir/build/ace-broker" "$socket" >"$runtime_dir/broker.out" 2>"$runtime_dir/broker.err" &
broker_pid=$!
for _ in $(seq 1 50); do [ -S "$socket" ] && break; sleep .1; done
[ -S "$socket" ] || fail 'broker did not start'

template=$(SAY_CONF="$runtime_dir/no-voices.conf" "$repo_dir/build/say" '?')
[ "$template" = 'TEXT/M,MALE/S,FEMALE/S,VOICE/K,WARM/S,LIST/S' ] || fail 'say template is not AmigaDOS-style'

# The command must be discoverable by the shell itself, not only executable
# by an absolute host path.  This is the path a user takes inside ACE.
shell_output=$(printf 'say ?\nEndCLI\n' | "$repo_dir/build/ace-user-shell" 2>&1 | tr -d '\r')
printf '%s\n' "$shell_output" | grep -Fq 'TEXT/M,MALE/S,FEMALE/S,VOICE/K,WARM/S,LIST/S' \
    || fail 'ace-user-shell did not resolve say through its command drawer'

ACE_BROKER_SOCKET="$socket" "$repo_dir/build/ace-brokerctl" say warm amy
for _ in $(seq 1 50); do [ -s "$pid_file" ] && break; sleep .1; done
[ -s "$pid_file" ] || fail 'broker did not start the voice helper'
voice_pid=$(sed -n '1p' "$pid_file")
kill -0 "$voice_pid" 2>/dev/null || fail 'voice helper is not alive'

# A different command process reaches the same broker and reuses its server.
ACE_BROKER_SOCKET="$socket" "$repo_dir/build/ace-brokerctl" say warm amy
status=$(ACE_BROKER_SOCKET="$socket" "$repo_dir/build/ace-brokerctl" say status)
printf '%s\n' "$status" | grep -q "^amy[[:space:]]$voice_pid$" || fail 'voice is not broker-owned state'

kill "$broker_pid"
wait "$broker_pid" 2>/dev/null || true
broker_pid=''
for _ in $(seq 1 50); do ! kill -0 "$voice_pid" 2>/dev/null && break; sleep .1; done
! kill -0 "$voice_pid" 2>/dev/null || fail 'broker exit did not stop the voice'
printf 'say broker test passed\n'
