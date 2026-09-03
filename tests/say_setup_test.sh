#!/bin/bash
set -euo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/ace-say-setup-test.XXXXXX")
data_dir="$test_dir/data"
config_dir="$test_dir/config"
mock_dir="$test_dir/mock"
curl_log="$test_dir/curl.log"
conf="$config_dir/say/voices.conf"
fail()
{
    printf 'say setup test: %s\n' "$*" >&2
    exit 1
}
cleanup()
{
    rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$data_dir/say/venv/bin" "$mock_dir"
ln -s /bin/true "$data_dir/say/venv/bin/python"
cat >"$mock_dir/curl" <<'EOF'
#!/bin/bash
set -eu
while (( $# > 1 )); do
    if [[ $1 == -o ]]; then
        target=$2
        shift 2
        continue
    fi
    shift
done
printf '%s\n' "$1" >>"$SAY_TEST_CURL_LOG"
printf 'model\n' >"$target"
EOF
chmod +x "$mock_dir/curl"

run_setup()
{
    PATH="$mock_dir:$PATH" XDG_DATA_HOME="$data_dir" \
        XDG_CONFIG_HOME="$config_dir" SAY_TEST_CURL_LOG="$curl_log" \
        "$repo_dir/data/say/say-setup" "$@"
}

run_setup >/dev/null
grep -Fq 'en_US-amy-medium.onnx' "$conf" || fail 'default female quality changed'
grep -Fq 'en_US-ryan-medium.onnx' "$conf" || fail 'default male quality changed'

run_setup --quality low >/dev/null
grep -Fq 'en_US-amy-low.onnx' "$conf" || fail 'low female model not selected'
grep -Fq 'en_US-ryan-low.onnx' "$conf" || fail 'low male model not selected'

warning=$(run_setup -q high 2>&1 >/dev/null)
grep -Fq 'en_US-amy-medium.onnx' "$conf" || fail 'missing high fallback'
grep -Fq 'en_US-ryan-high.onnx' "$conf" || fail 'high male model not selected'
[[ $warning == *'en_US-amy has no high model; using medium'* ]] ||
    fail 'missing high fallback warning'

if run_setup --quality x_low >/dev/null 2>&1; then
    fail 'accepted an unavailable quality'
fi

printf 'say setup test passed\n'
