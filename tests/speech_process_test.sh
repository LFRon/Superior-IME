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

pid=""
current=""

# Starts the process with the given fake capture and fake engine.
#
# `stream_cmd` (optional) switches the session to the streaming recogniser. When
# it is empty streaming is disabled explicitly: otherwise the test would pick up
# the real engine and model installed on the developer's machine.
start_process() {
    local capture="$1"
    local engine="$2"
    local stream_cmd="${3:-}"
    current="$root/session-$(date +%s%N)"
    mkdir -p "$current"
    mkfifo "$current/in"
    if [[ -n "$stream_cmd" ]]; then
        # A short cadence keeps the suite fast; the default is 1500 ms.
        SUPERIOR_CAPTURE_CMD="$capture" SUPERIOR_ASR_CMD="$engine" \
            SUPERIOR_ASR_STREAM_CMD="$stream_cmd" SUPERIOR_SPEECH_COMMIT_MS=700 \
            "$binary" < "$current/in" > "$current/out" 2> "$current/err" &
    else
        SUPERIOR_CAPTURE_CMD="$capture" SUPERIOR_ASR_CMD="$engine" \
            SUPERIOR_DISABLE_STREAMING=1 \
            "$binary" < "$current/in" > "$current/out" 2> "$current/err" &
    fi
    pid=$!
    exec 3> "$current/in"
}

stop_process() {
    if [[ -n "$pid" ]]; then
        # No `2>/dev/null` on `exec`: it would redirect the shell's own stderr for
        # the rest of the script, hiding every later failure.
        echo '{"cmd":"quit"}' >&3 || true
        exec 3>&-
        wait "$pid" || true
        pid=""
    fi
}

# Waits for a needle in the event stream, up to ~10 s.
wait_for() {
    local needle="$1"
    for _ in $(seq 1 100); do
        if grep -q "$needle" "$current/out" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

fail() {
    echo "speech process: $1" >&2
    cat "$current/out" >&2 2>/dev/null || true
    cat "$current/err" >&2 2>/dev/null || true
    stop_process
    exit 1
}

# --- Scenario 1: the user clicks start, then stop ---------------------------
start_process 'bash -c "dd if=/dev/zero bs=32000 count=2 2>/dev/null"' \
    'bash -c "echo 你好世界"'

wait_for '"ev":"ready"' || fail "no ready event"
echo '{"cmd":"start"}' >&3
sleep 0.4
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after a manual stop"
grep -q '你好世界' "$current/out" || fail "transcript missing"
stop_process

# --- Scenario 2: the utterance ends by itself -------------------------------
#
# ~0.8 s of speech followed by silence, written at realtime pace because the
# silence rule is clock-based: a capture that dumps everything at once would be
# seen as one long loud block and never time out.
# Single quotes are deliberate: the `$(seq …)` must reach the child's bash
# unexpanded. Escaping them here instead leaves a literal backslash and the
# child fails to parse its own script.
fake_capture='bash -c "for i in $(seq 1 10); do dd if=/dev/urandom bs=3200 count=1 2>/dev/null; sleep 0.08; done; for i in $(seq 1 30); do dd if=/dev/zero bs=3200 count=1 2>/dev/null; sleep 0.08; done; sleep 5"'

start_process "$fake_capture" 'bash -c "echo 自动结束"'
wait_for '"ev":"ready"' || fail "no ready event (auto-stop)"
echo '{"cmd":"start"}' >&3
# No `stop` command is sent: the capability must decide on its own.
wait_for '"ev":"final"' || fail "no final event without a stop command"
grep -q '自动结束' "$current/out" || fail "transcript missing (auto-stop)"
stop_process

# --- Scenario 3: the capture tool dies --------------------------------------
#
# Awkward in practice (device busy, unplugged) and invisible without this: the
# service used to keep "recording" silence until the no-speech timeout.
start_process 'bash -c "true"' 'bash -c "echo 不该到达这里"'
wait_for '"ev":"ready"' || fail "no ready event (capture failure)"
echo '{"cmd":"start"}' >&3
wait_for '"code":"capture_failed"' ||
    fail "a dead capture tool must be reported, not waited out"
grep -q '不该到达这里' "$current/out" && fail "a failed capture must not run the engine"
stop_process

# --- Scenario 4: streaming commits on a cadence, without stopping -----------
#
# Continuous dictation: the recogniser keeps rewriting its draft, so a segment is
# only committed after it has stayed unchanged for the cadence. The fake keeps
# repeating the same draft, which is exactly the "stable" case, and never emits a
# final of its own: the session must keep listening until the user stops it.
cat > "$root/fake-stream.sh" <<'STREAM'
#!/bin/sh
i=0
while [ $i -lt 12 ]; do
  echo '{"type":"partial","text":"你好世界"}'
  i=$((i + 1))
  sleep 0.3
done
cat > /dev/null
echo '{"type":"final","text":"你好世界"}'
STREAM
chmod +x "$root/fake-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 8"' \
    'bash -c "echo 不该走批处理"' "$root/fake-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (streaming)"
echo '{"cmd":"start"}' >&3
# The cadence is 1.5 s by default; give it room without making the suite slow.
wait_for '"ev":"chunk"' || fail "no chunk event while recording"
grep -q '"ev":"chunk".*你好世界' "$current/out" || fail "chunk text missing"
# Committing mid-session must not end it.
grep -q '"ev":"final"' "$current/out" && fail "final arrived before the session ended"
# The engine closes a segment on a pause. In continuous dictation that is a
# commit, not the end: the phase must still be `recording` afterwards.
grep -q '"ev":"phase","name":"transcribing"' "$current/out" &&
    fail "an engine-side pause must not end the session"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (streaming)"
grep -q '不该走批处理' "$current/out" && fail "streaming must not fall back to batch decoding"
# Everything was already committed as chunks, so the final carries no leftover.
grep -q '"ev":"final","text":"你好世界"' "$current/out" &&
    fail "the final repeated text that was already committed as a chunk"
stop_process

echo "speech process: ok"
