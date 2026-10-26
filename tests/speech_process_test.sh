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
    local extra_env="${4:-}"
    current="$root/session-$(date +%s%N)"
    mkdir -p "$current"
    mkfifo "$current/in"
    if [[ -n "$stream_cmd" ]]; then
        # A short cadence keeps the suite fast; the default is 1000 ms.
        # shellcheck disable=SC2086 # `extra_env` is a deliberate list of assignments.
        env $extra_env \
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
# Continuous dictation: the recogniser keeps rewriting its draft, so only the part
# that is no longer likely to change is typed (everything but the last two
# characters). The fake keeps repeating the same draft and never emits a final of
# its own: the session must keep listening until the user stops it.
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
grep -q '"ev":"chunk".*"text":"你好"' "$current/out" ||
    fail "the committed prefix is wrong (last two characters must stay a draft)"
# Committing mid-session must not end it.
grep -q '"ev":"final"' "$current/out" && fail "final arrived before the session ended"
# The engine closes a segment on a pause. In continuous dictation that is a
# commit, not the end: the phase must still be `recording` afterwards.
grep -q '"ev":"phase","name":"transcribing"' "$current/out" &&
    fail "an engine-side pause must not end the session"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (streaming)"
grep -q '不该走批处理' "$current/out" && fail "streaming must not fall back to batch decoding"
# Everything but the held-back draft was already committed, so the final carries
# only those two characters and never repeats typed text.
grep -q '"ev":"final","text":"世界"' "$current/out" ||
    fail "the held-back draft was lost when the session ended"
grep -q '"ev":"final".*你好' "$current/out" &&
    fail "the final repeated text that was already committed as a chunk"
stop_process

# --- Scenario 5: a single draft, then engine silence ------------------------
#
# The real decoder emits one partial for the window that contained speech and
# then *nothing* while the audio is quiet (measured against crispasr: a 2 s tone
# followed by 8 s of silence produced exactly one partial and no final until
# EOF). A commit rule that waits for a second, identical partial therefore types
# nothing at all for a sentence the user already finished, which is what happened
# on the desktop. The commit must be driven by time, not by event arrival.
cat > "$root/quiet-stream.sh" <<'STREAM'
#!/bin/sh
sleep 0.2
echo '{"type":"partial","text":"一二三四五"}'
cat > /dev/null
echo '{"type":"final","text":"一二三四五"}'
STREAM
chmod +x "$root/quiet-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 8"' \
    'bash -c "echo 不该走批处理"' "$root/quiet-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (single draft)"
echo '{"cmd":"start"}' >&3
wait_for '"ev":"chunk"' || fail "a lone draft was never committed"
# The last two characters stay in the draft: they are the ones a decoder revises.
grep -q '"ev":"chunk".*"text":"一二三"' "$current/out" ||
    fail "the committed prefix is wrong (last two characters must stay a draft)"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (single draft)"
grep -q '"ev":"final".*"text":"四五"' "$current/out" ||
    fail "the held-back draft was lost when the session ended"
stop_process

# --- Scenario 6: the draft keeps growing, text still has to appear ----------
#
# Plain dictation: every step re-decodes a longer transcript, so the draft never
# stops changing. Requiring a draft to be *fully* stable is the same as requiring
# silence, i.e. nothing is typed until the user pauses.
cat > "$root/growing-stream.sh" <<'STREAM'
#!/bin/sh
for text in 甲 甲乙 甲乙丙 甲乙丙丁 甲乙丙丁戊; do
  echo "{\"type\":\"partial\",\"text\":\"$text\"}"
  sleep 0.3
done
cat > /dev/null
echo '{"type":"final","text":"甲乙丙丁戊"}'
STREAM
chmod +x "$root/growing-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 8"' \
    'bash -c "echo 不该走批处理"' "$root/growing-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (growing draft)"
echo '{"cmd":"start"}' >&3
wait_for '"ev":"chunk"' || fail "a growing draft never commits anything"
grep -q '"ev":"chunk".*"text":"甲' "$current/out" ||
    fail "the settled part of a growing draft was not committed"
grep -q '"ev":"final"' "$current/out" && fail "final arrived before the session ended"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (growing draft)"
# Committed characters must never be typed twice.
grep -q '"ev":"final".*甲乙' "$current/out" &&
    fail "the final repeated text that was already committed"
stop_process

# --- Scenario 7: the decoder rewrites a character that is already typed ----
#
# Streaming decoders do revise, including text that has already been typed. That
# draft must be distrusted (nothing more is committed from it) but it must not
# take the process down: computing the "rest of the draft" once cut inside a
# multi-byte character and `nlohmann::json` aborted on the invalid UTF-8.
cat > "$root/revise-stream.sh" <<'STREAM'
#!/bin/sh
echo '{"type":"partial","text":"一二三四五"}'
sleep 0.3
echo '{"type":"partial","text":"一二三四五"}'
sleep 1.2
echo '{"type":"partial","text":"一二四五六"}'
sleep 0.3
echo '{"type":"partial","text":"一二四五六"}'
cat > /dev/null
echo '{"type":"final","text":"一二四五六"}'
STREAM
chmod +x "$root/revise-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 9"'     'bash -c "echo 不该走批处理"' "$root/revise-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (revised draft)"
echo '{"cmd":"start"}' >&3
wait_for '"ev":"chunk"' || fail "no chunk before the revision"
grep -q '"ev":"chunk".*"text":"一二三"' "$current/out" ||
    fail "the committed prefix is wrong before the revision"
# Give the revision time to arrive and be handled.
sleep 3
grep -q '"ev":"error"' "$current/out" && fail "a revised draft must not be an error"
# Already typed text must never be typed a second time.
grep -q '"ev":"chunk".*一二三' "$current/out" || fail "chunk text missing after the revision"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "the process did not survive a revised draft"
stop_process

# --- Scenario 8: the decode step defaults to 2000 ms and is overridable ------
#
# The step is the latency knob, and the wrong value is invisible: too small and
# the decoder falls behind real time (words are dropped when the capture pipe
# fills), too large and text appears in lumps. 2000 ms is the shipped default; the
# environment variable is the escape hatch for a GPU build or a smaller model.
#
# The step no longer depends on the machine's load: `streamingStepMs` measures
# backpressure from this pipeline instead of reading `/proc/loadavg`, whose 1/5/15
# minute averages said nothing about whether the recogniser was keeping up. The
# default is therefore the model baseline, and it is the same on an idle laptop
# and on a busy shared machine.
expected_step=2000
start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 6"' \
    'bash -c "echo 不该走批处理"' "$root/quiet-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (step default)"
echo '{"cmd":"start"}' >&3
sleep 0.6
grep -q "stepMs=${expected_step}" "$current/err" ||
    fail "the default decode step is not ${expected_step} ms (see the 'streaming recogniser started' log line)"
# The window has to cover the session, or the decoder's draft stops being a
# prefix of the utterance after 10 s (see `streamingWindowMs`).
grep -q "windowMs=60000" "$current/err" ||
    fail "the streaming window does not cover the session (see the 'streaming recogniser started' log line)"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (step default)"
stop_process

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 6"' \
    'bash -c "echo 不该走批处理"' "$root/quiet-stream.sh" "SUPERIOR_SPEECH_STEP_MS=2500"
wait_for '"ev":"ready"' || fail "no ready event (step override)"
echo '{"cmd":"start"}' >&3
sleep 0.6
grep -q "stepMs=2500" "$current/err" || fail "SUPERIOR_SPEECH_STEP_MS was ignored"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"final"' || fail "no final event after stop (step override)"
stop_process

# --- Scenario 9: the recogniser dies while the microphone is open -----------
#
# A crash or an OOM kill used to be completely invisible: the capture thread only
# noticed its pipe closing and kept metering the level, so the panel looked like it
# was listening while every word went nowhere, until the 60 s cap expired. The
# session has to end at once, with an error, and the process has to stay usable.
cat > "$root/dying-stream.sh" <<'STREAM'
#!/bin/sh
echo '{"type":"partial","text":"一二三"}'
sleep 0.4
exit 1
STREAM
chmod +x "$root/dying-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 20"' \
    'bash -c "echo 不该走批处理"' "$root/dying-stream.sh"
wait_for '"ev":"ready"' || fail "no ready event (dead recogniser)"
echo '{"cmd":"start"}' >&3
wait_for '"code":"recognition_failed"' ||
    fail "a recogniser that dies mid-session must be reported"
wait_for '"ev":"phase","name":"idle"' || fail "the session did not end after the recogniser died"
# The capability must still answer: a dead child must not wedge the command loop.
echo '{"cmd":"start"}' >&3
sleep 0.5
grep -q '"ev":"phase","name":"recording"' "$current/out" ||
    fail "the capability stopped accepting commands after the recogniser died"
echo '{"cmd":"stop"}' >&3
wait_for '"ev":"phase","name":"idle"' || fail "no idle after stopping the second session"
stop_process

# --- Scenario 10: a recogniser that never finishes its flush ---------------
#
# The engine is supposed to exit when its stdin closes. If it does not — wedged
# inside a decode, or never seeing EOF because something else still holds the
# write end — the session used to stay "finishing" forever: every later `stop()`
# returned early and every later click was refused because the phase was still
# busy. A bounded flush turns that into one failed utterance.
cat > "$root/wedged-stream.sh" <<'STREAM'
#!/bin/sh
echo '{"type":"partial","text":"你好"}'
cat > /dev/null
# stdin closed, but never exit: exactly the wedged case.
sleep 60
STREAM
chmod +x "$root/wedged-stream.sh"

start_process 'bash -c "dd if=/dev/zero bs=32000 count=1 2>/dev/null; sleep 8"' \
    'bash -c "echo 不该走批处理"' "$root/wedged-stream.sh" "SUPERIOR_SPEECH_FLUSH_MS=1200"
wait_for '"ev":"ready"' || fail "no ready event (wedged flush)"
echo '{"cmd":"start"}' >&3
sleep 0.6
echo '{"cmd":"stop"}' >&3
wait_for '"code":"recognition_timeout"' ||
    fail "a flush that never ends must time out instead of hanging the session"
wait_for '"ev":"phase","name":"idle"' || fail "no idle after the flush timeout"
# And the session must be startable again: that is the whole point.
echo '{"cmd":"start"}' >&3
sleep 0.5
grep -q '"ev":"phase","name":"recording"' "$current/out" ||
    fail "a wedged flush must not leave the capability unable to start again"
echo '{"cmd":"stop"}' >&3
stop_process

echo "speech process: ok"
