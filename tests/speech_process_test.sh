#!/usr/bin/env bash
# End-to-end test of the recogniser process without a microphone or a model.
#
# Both boundaries are replaceable on purpose:
#   SUPERIOR_CAPTURE_CMD -> a command that prints raw PCM on stdout
#   SUPERIOR_ASR_CMD     -> a command that prints the transcript
# so the whole protocol (start -> stop -> final) is exercised for real.
set -euo pipefail

binary="${1:?path to superior_speech_recognition}"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

export SUPERIOR_SPEECH_DIR="$root/data"
export SUPERIOR_CAPTURE_CMD="bash -c \"dd if=/dev/zero bs=32000 count=2 2>/dev/null\""
export SUPERIOR_ASR_CMD="bash -c \"echo 你好世界\""

mkfifo "$root/in"
"$binary" < "$root/in" > "$root/out" 2>"$root/err" &
pid=$!
exec 3> "$root/in"

cleanup() {
    exec 3>&- || true
    wait "$pid" 2>/dev/null || true
}
trap 'cleanup; rm -rf "$root"' EXIT

echo '{"cmd":"start"}' >&3
sleep 0.4
echo '{"cmd":"stop"}' >&3

for _ in $(seq 1 100); do
    if grep -q '"ev":"final"' "$root/out" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

echo '{"cmd":"quit"}' >&3
cleanup
trap 'rm -rf "$root"' EXIT

if ! grep -q '"ev":"ready"' "$root/out"; then
    echo "speech process: no ready event" >&2
    cat "$root/out" >&2
    exit 1
fi
if ! grep -q '"ev":"final"' "$root/out"; then
    echo "speech process: no final event" >&2
    cat "$root/out" >&2
    cat "$root/err" >&2
    exit 1
fi
if ! grep -q '你好世界' "$root/out"; then
    echo "speech process: transcript missing" >&2
    cat "$root/out" >&2
    exit 1
fi
echo "speech process: ok"
