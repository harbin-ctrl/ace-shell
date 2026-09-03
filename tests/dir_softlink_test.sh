#!/bin/sh
set -eu

# SYS:C is a drawer of symbolic links: `make install` links every command
# into it from PROGDIR. A directory scan reports a softlink as ST_SOFTLINK,
# which is positive, so Dir counted every command as a drawer and CD walked
# into one without complaint. Lock() follows the link, so Examine() on a lock
# must describe the target -- which is what Dir re-Lock()s each softlink to
# ask, and what tells CD the object is of the wrong type.

repo_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d "$repo_dir/.ace-dir-softlink.XXXXXX")
sys_dir="$test_dir/sys"
runtime_dir="$test_dir/run"
socket_path="$test_dir/broker.sock"
broker_pid=""

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
    printf 'dir softlink test: %s\n' "$1" >&2
    exit 1
}

work="$sys_dir/C"
mkdir -p "$work" "$sys_dir/S" "$sys_dir/Prefs/Env-Archive" "$runtime_dir"

ACE_SYS_DIR="$sys_dir" XDG_RUNTIME_DIR="$runtime_dir" \
    "$repo_dir/build/ace-broker" "$socket_path" &
broker_pid=$!
for _ in $(seq 1 100); do
    [ -S "$socket_path" ] && break
    sleep 0.05
done
[ -S "$socket_path" ] || fail 'broker did not start'

run_command()
{
    command_name=$1
    shift
    env ACE_BROKER_SOCKET="$socket_path" ACE_SESSION=dir-softlink \
        "$repo_dir/build/$command_name" "$@"
}

# The three kinds of link a command drawer can hold: one to a file (every
# real command), one to a directory, and one whose target is gone.
mkdir -p "$test_dir/bin" "$test_dir/drawer"
printf 'e\n' > "$test_dir/bin/Echo"
ln -s "$test_dir/bin/Echo" "$work/Echo"
ln -s "$test_dir/drawer" "$work/Linked"
ln -s "$test_dir/bin/Missing" "$work/Dangling"

listing=$(run_command Dir SYS:C)

printf '%s\n' "$listing" | grep -q 'Echo (dir)' &&
    fail 'a link to a file was listed as a drawer'
printf '%s\n' "$listing" | grep -q 'Echo' ||
    fail 'a link to a file was not listed at all'
printf '%s\n' "$listing" | grep -q 'Dangling (dir)' &&
    fail 'a dangling link was listed as a drawer'
printf '%s\n' "$listing" | grep -q 'Linked (dir)' ||
    fail 'a link to a drawer was not listed as one'

# CD into a command must fail, and say why, rather than silently doing
# nothing. CD's own exit code is what the shell would see.
if cd_output=$(run_command CD SYS:C/Echo 2>&1); then
    fail 'CD into a link to a file succeeded'
fi
case "$cd_output" in
    *'not of required type'* | *'wrong type'*) ;;
    *) fail "CD into a link to a file did not report a wrong type: $cd_output" ;;
esac

# CD through a link to a drawer still has to work.
run_command CD SYS:C/Linked >/dev/null ||
    fail 'CD through a link to a drawer failed'

printf 'Dir softlink test passed\n'
