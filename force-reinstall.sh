#!/usr/bin/env bash
set -euo pipefail

# Reconcile a running checkout with every ACE install this user may have left
# behind. This is intentionally narrow: it removes only ACE entry points and
# the install directories owned by an ACE prefix, never a whole bin directory.

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$script_dir"

ace_names=(
    ace-shell ace-user-shell ace-console ace-broker ace-brokerctl
    Echo CD PathPart Dir Delete Protect Filenote Fault Ask Get Getenv Set Unset
    Alias Unalias FailAt Why Prompt MakeDir EndCLI Assign Type Rename Stack Run
    Linux LNX NewCLI If Else EndIf Execute Setenv Unsetenv Copy List Touch Relabel LhA vim broker-start broker-stop
)

as_root()
{
    if "$@"; then
        return 0
    fi
    command -v sudo >/dev/null 2>&1 || return 1
    sudo -n "$@"
}

remove_file()
{
    local path=$1

    if [[ -e $path || -L $path ]]; then
        printf 'remove %s\n' "$path"
        as_root rm -f -- "$path"
    fi
}

remove_tree()
{
    local path=$1

    if [[ -d $path ]]; then
        printf 'remove tree %s\n' "$path"
        as_root rm -rf -- "$path"
    fi
}

stop_named_processes()
{
    local signal=$1
    local name

    for name in ace-shell ace-user-shell ace-console ace-broker ace-brokerctl; do
        pkill -"$signal" -x "$name" 2>/dev/null || true
    done
}

wait_for_named_processes()
{
    local attempt

    for attempt in $(seq 1 100); do
        if ! pgrep -x ace-shell >/dev/null 2>&1 &&
           ! pgrep -x ace-user-shell >/dev/null 2>&1 &&
           ! pgrep -x ace-console >/dev/null 2>&1 &&
           ! pgrep -x ace-broker >/dev/null 2>&1 &&
           ! pgrep -x ace-brokerctl >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.02
    done
    return 1
}

stop_named_processes TERM
if ! wait_for_named_processes; then
    printf '%s\n' 'ACE processes survived TERM; forcing them down' >&2
    stop_named_processes KILL
    wait_for_named_processes
fi

# Discover prefixes that contain an ACE shell. The ordinary local prefix is
# always included; the other two are where earlier manual installs commonly
# landed. A prefix is removed as a set only when its ace-shell proves it is an
# ACE installation.
prefixes=("$HOME/.local" /usr/local /usr)
while IFS= read -r shell_path; do
    [[ $shell_path == */bin/ace-shell ]] || continue
    prefixes+=("${shell_path%/bin/ace-shell}")
done < <(find "$HOME" /usr/local /usr /opt -type f -o -type l 2>/dev/null |
         awk '/\/bin\/ace-shell$/ {print}')

declare -A seen_prefixes=()
for prefix in "${prefixes[@]}"; do
    [[ ${seen_prefixes[$prefix]+yes} ]] && continue
    seen_prefixes[$prefix]=yes
    bindir=$prefix/bin
    has_ace_install=0
    # lib/ace is where the programs live now; the bin names are what older
    # installs left on PATH, and either one proves the prefix is ACE's.
    [[ -d $prefix/lib/ace ]] && has_ace_install=1
    for core_name in ace-shell ace-user-shell ace-console ace-broker; do
        if [[ -e $bindir/$core_name || -L $bindir/$core_name ]]; then
            has_ace_install=1
            break
        fi
    done
    if (( has_ace_install )); then
        for name in "${ace_names[@]}"; do
            remove_file "$bindir/$name"
        done
        remove_tree "$prefix/lib/ace"
        remove_tree "$prefix/share/ace"
        remove_file "$prefix/share/applications/ace.desktop"
        remove_file "$prefix/share/icons/hicolor/512x512/apps/ace.png"
    fi
done

# A build tree is never an install, but clean it before checking resolution so
# an old build/ace-shell cannot be mistaken for the freshly installed binary.
make clean

hash -r 2>/dev/null || true
if command -v ace-shell >/dev/null 2>&1; then
    printf 'ace-shell still resolves after cleanup: %s\n' "$(command -v ace-shell)" >&2
    exit 1
fi
if pgrep -x ace-broker >/dev/null 2>&1; then
    printf '%s\n' 'a broker still exists after cleanup' >&2
    exit 1
fi

make -j2 all
make install

installed_shell=$HOME/.local/lib/ace/ace-shell
if [[ ! -x $installed_shell ]]; then
    printf 'installed ace-shell is missing: %s\n' "$installed_shell" >&2
    exit 1
fi
if [[ $(sha256sum "$installed_shell" | awk '{print $1}') != \
      $(sha256sum build/ace-shell | awk '{print $1}') ]]; then
    printf '%s\n' 'installed ace-shell does not match build/ace-shell' >&2
    exit 1
fi

"$script_dir/broker-start"
hash -r 2>/dev/null || true
printf 'host: '; hostname
printf 'ace-shell: '; command -v ace-shell
printf 'broker: '; pgrep -a -x ace-broker
printf '%s\n' 'force reinstall complete'
