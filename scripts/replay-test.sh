#!/bin/bash
# Repeatable test runs on macOS from one recording of controller 1 (MACOS.md, "Repeatable test runs").
#
#   scripts/replay-test.sh snapshot              your saves, settings and shader cache (build/macapp) as every run's start
#   scripts/replay-test.sh record [name]         you play from that start; controller 1 recorded to .local/replay/<name>.pad
#   scripts/replay-test.sh play [name] [ENV=..]  the same run again from the same start, the recording in place of the
#                                                controller, until it ends; screenshots, log and sound in .local/replay/out
#
# The recording is keyed to the game's own reads of the controller (once a frame), not the clock, so it presses the
# same buttons on the same frames however long loading takes. Every run starts from a fresh copy of the snapshot, as
# the menus must be the same for the presses to land on the same items. play takes the game's settings from the
# snapshot too; give ENV=value pairs to change one thing between runs, for example PGR2_CAR_REFLECTIONS=0.
# For screenshots at chosen moments: SHOTS="read read ..." (the read numbers are in the .pad file and the log);
# SHOT_EVERY=N for one every N frames; a file .local/replay/<name>.shots holds a recording's usual reads. AFTER=seconds
# keeps it running past the end of the recording (default 3).
# The usual test: "demos", the five built-in demo replays (Start Player's Game > Options > PGR 2 Demos), two shots each.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
dir=$here/.local/replay
cmd=${1:-}
name=${2:-race}
case "$cmd" in
snapshot)
    rm -rf "$dir/start"
    mkdir -p "$dir/start"
    cp -c -R "$here/build/macapp/hdd" "$dir/start/hdd"  # a clone: the cache is 400 MB
    cp "$here/build/macapp/pgr2.toml" "$dir/start/"
    [ -f "$here/build/macapp/achievements.toml" ] && cp "$here/build/macapp/achievements.toml" "$dir/start/"
    [ -d "$here/build/macapp/metalcache" ] && cp -c -R "$here/build/macapp/metalcache" "$dir/start/metalcache"
    echo "snapshot taken in $dir/start"
    ;;
record | play)
    [ -d "$dir/start" ] || { echo "no snapshot: run $0 snapshot first"; exit 1; }
    cmake --build "$here/build/macapp" --target PGR2 > /dev/null
    rm -rf "$dir/run"
    cp -c -R "$dir/start" "$dir/run"
    cp "$here/build/macapp/PGR2" "$dir/run/PGR2"
    if [ "$cmd" = record ]; then
        echo "recording to $dir/$name.pad: play, then quit the game"
        (cd "$dir/run" && XBE_PAD_RECORD="$dir/$name.pad" ./PGR2)
        cp "$dir/run/logs/pgr2.log" "$dir/$name.record.log"
        echo "recorded $(grep -vc '^#' "$dir/$name.pad") changes to read $(tail -1 "$dir/$name.pad" | cut -d' ' -f1)"
        exit 0
    fi
    [ -f "$dir/$name.pad" ] || { echo "no recording $dir/$name.pad"; exit 1; }
    shift 2 2>/dev/null || shift $#
    # the screenshot reads kept with the recording, unless SHOTS says otherwise
    [ -z "${SHOTS:-}" ] && [ -f "$dir/$name.shots" ] && SHOTS=$(cat "$dir/$name.shots")
    rm -rf "$dir/out"
    mkdir -p "$dir/out"
    (cd "$dir/run" && exec env XBE_PAD_PLAY="$dir/$name.pad" ${SHOTS:+XBE_SHOT_READS="$SHOTS"} ${SHOT_EVERY:+XBE_SHOT_EVERY=$SHOT_EVERY} \
        XBE_AUDIO_WAV=sound.wav "$@" ./PGR2 > run.out 2>&1) &
    pid=$!
    # until the recording ends (the log says so), then AFTER seconds more
    log="$dir/run/logs/pgr2.log"
    until grep -q "the pad recording ends" "$log" 2>/dev/null; do
        kill -0 $pid 2>/dev/null || break
        sleep 1
    done
    sleep "${AFTER:-3}"
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    cp "$log" "$dir/out/pgr2.log"
    cp "$dir/run/sound.wav" "$dir/out/" 2>/dev/null || true
    n=0
    for f in $(ls "$dir/run/screenshots" 2>/dev/null | sort); do
        n=$((n + 1))
        cp "$dir/run/screenshots/$f" "$dir/out/shot_$(printf %02d $n).png"
    done
    echo "played $name: $n screenshots, log and sound in $dir/out"
    grep -E "screenshot at read|crash|fatal" "$dir/out/pgr2.log" | head -20 || true
    ;;
*)
    sed -n 2,16p "$0"
    exit 1
    ;;
esac
