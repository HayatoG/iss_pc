#!/usr/bin/env bash
# One measurement round of the parameter study (PLANO_TESTES_PARAMETROS_ISS.md).
# Usage: scripts/param_round.sh <name> [VAR=value ...]
#   Needs: GAME_LOCK (the session's game.lock), SAVE_SNAPSHOT (a savedata folder to copy per round),
#   RESULTS (default ~/decomp/iss_ref/param_tests). Optional: SCENE (file of "<second> <buttons>" lines that
#   gets the game from the title to the scene; "replay" starts a TLG_PAD_REPLAY recording), WARM_MAX, MEASURE_S.
# One round: isolated user dir (own save copy, shared shader cache), fixed tlg.ini, warm up until the
# shaders stop compiling, 60 s of measurement with the monitor running, a burst of frame dumps in the
# middle, then the game is closed. Never touches port/user/savedata.
set -uo pipefail
name=${1:?name}; shift
root=$(cd -- "$(dirname -- "$0")/.." && pwd); port=$root/port
lock=${GAME_LOCK:?set GAME_LOCK}; snap=${SAVE_SNAPSHOT:?set SAVE_SNAPSHOT}
results=${RESULTS:-$HOME/decomp/iss_ref/param_tests}; warm_max=${WARM_MAX:-180}; measure=${MEASURE_S:-60}
r=$results/$name; rm -rf "$r"; mkdir -p "$r/dumps" "$r/user"
if [[ -z ${PARAM_ROUND_LOCKED:-} ]]; then
    touch "$lock"; exec env PARAM_ROUND_LOCKED=1 flock -x "$lock" "$0" "$name" "$@"
fi
pgrep -x tlg-probe >/dev/null && { echo "a game is already running" >&2; exit 3; }
status=ok; game=; mon=

# isolated user dir: everything of port/user shared (shader cache...) except the saves and the ini
for e in "$port"/user/*; do b=$(basename "$e"); [[ $b == savedata* || $b == tlg.ini ]] || ln -s "$e" "$r/user/$b"; done
cp -a "$snap" "$r/user/savedata"
cat > "$r/user/tlg.ini" <<INI
upscaler=off
jitter=0
object_motion=0
show_fps=1
output_res=1920x1080
display_mode=borderless
vsync=0
fps_limit=0
rcas=0
fsr1=0
render_resolution=native
deferred_readback=1
INI
env_file=$r/env.txt; : > "$env_file"
declare -a envs=(ISS_LANGUAGE=17 ISS_READBACKS=2 ISS_EAGER_READBACK=1 ISS_WRITE_MERGE=1 ISS_DEFERRED_READBACK=1
    ISS_GC_BUDGET_MB=3500 ISS_PIPELINE_BINARIES=0 ISS_FRAME_STATS=1 ISS_OBJECT_MOTION=0 ISS_UPSCALER=off ISS_JITTER=0
    ISS_FRAME_DUMP_TRIGGER=$r/trigger ISS_DUMP_DIR=$r/dumps ISS_FRAME_DUMP_COUNT=150 ISS_PAD_FILE=$r/pad.txt)
[[ -n ${REPLAY_FILE:-} ]] && envs+=(ISS_PAD_REPLAY=$REPLAY_FILE)
for kv in "$@"; do envs+=("$kv"); done
printf '%s\n' "${envs[@]}" > "$env_file"
: > "$r/pad.txt"

cleanup() {
    [[ -n $mon ]] && kill "$mon" 2>/dev/null
    if [[ -n $game ]] && kill -0 "$game" 2>/dev/null; then
        pkill -TERM -x tlg-probe; for _ in $(seq 1 15); do kill -0 "$game" 2>/dev/null || break; sleep 1; done
        pkill -KILL -x tlg-probe 2>/dev/null
    fi
    echo "$status" > "$r/status.txt"
}
trap cleanup EXIT INT TERM

dump=$root/game/CUSA00223
(cd "$r/user" && exec env "${envs[@]}" ISS_APP_NAME="inFAMOUS Second Son" ISS_APP_ID=infamous-pc \
    "$port/out/tlg-probe" "$port/out/boot-linked.bin" --app0 "$dump" --user . \
    --content-profile "$port/out/content.bin" --timeout $((warm_max + measure + 240))) > "$r/game.log" 2>&1 &
game=$!
t0=$(date +%s); now() { echo $(( $(date +%s) - t0 )); }
press() { echo "$1" > "$r/pad.txt"; sleep 0.3; : > "$r/pad.txt"; }

# scene: from the title to the spot
if [[ -n ${SCENE:-} && -f $SCENE ]]; then
    while read -r at buttons; do
        [[ -z $at || $at == \#* ]] && continue
        while (( $(now) < at )); do sleep 0.2; done
        kill -0 "$game" 2>/dev/null || { status=crash; exit 1; }
        press "$buttons"
    done < "$SCENE"
fi

# warm-up: two Frame stats in a row without compiles and with a loaded scene (>2000 draws) or the limit
last_n=0
while (( $(now) < warm_max )); do
    kill -0 "$game" 2>/dev/null || { status=crash; exit 1; }
    ok=$(grep "Frame stats" "$r/game.log" | tail -2 | grep -c " 0 shader/pipeline compiles")
    n=$(grep -c "Frame stats" "$r/game.log")
    (( n > 4 && ok == 2 )) && (( $(now) > ${WARM_MIN:-60} )) && break
    sleep 5
done
echo "warm-up ended at $(now) s" >> "$r/notes.txt"

# measurement
start_lines=$(grep -c "Frame stats" "$r/game.log")
"$root/scripts/round_monitor.sh" "$r/mon" "$measure" 1 > /dev/null 2>&1 &
mon=$!
sleep $((measure / 2)); touch "$r/trigger"
sleep $((measure - measure / 2 + 2))
kill -0 "$game" 2>/dev/null || status=crash
echo "measure window: Frame stats lines $start_lines..$(grep -c 'Frame stats' "$r/game.log")" >> "$r/notes.txt"
grep -c "Stall:" "$r/game.log" > "$r/stalls.txt" 2>/dev/null
cleanup; trap - EXIT
python3 -I "$root/scripts/round_analyze.py" summarize --log "$r/game.log" --mon "$r/mon" --window "$measure" \
    --name "$name" --out "$r" > "$r/summary.txt" 2>&1
echo "$name: $status; $(grep -m1 -i 'fps' "$r/summary.txt" | head -1)"
