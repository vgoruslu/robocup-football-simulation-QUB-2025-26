#!/usr/bin/env bash
set -euo pipefail

echo "SCRIPT STARTED"

TOTAL_GAMES=2
RESULTS_DIR="/mnt/c/Users/User/Results"
TEAM_DIR="$HOME/ryansrobocup/helios-base/src"

TEAM1="HELIOS1"
TEAM2="HELIOS2"
PORT=6000

mkdir -p "$RESULTS_DIR"

# Resume numbering from the latest completed match in the results directory.
LAST_GAME=$(find "$RESULTS_DIR" -maxdepth 1 -type f -name "${TEAM1}vs${TEAM2}_Game*.rcg" \
  | sed -E 's/.*_Game([0-9]+)\.rcg/\1/' \
  | sort -n \
  | tail -n 1 || true)

LAST_GAME=${LAST_GAME:-0}

echo "Last existing game: $LAST_GAME"

for ((i=LAST_GAME+1; i<=LAST_GAME+TOTAL_GAMES; i++)); do
    echo "===== STARTING GAME $i ====="

    # Isolate each match so logs and simulator outputs do not overwrite each other.
    GAME_DIR="$RESULTS_DIR/tmp_game_$i"
    rm -rf "$GAME_DIR"
    mkdir -p "$GAME_DIR"

    (
        cd "$GAME_DIR"

        # Start the RoboCup server in auto mode for a full match.
        rcssserver \
          -server::auto_mode=true \
          -server::half_time=300 \
          -server::port=$PORT \
          > server.log 2>&1 &
        SERVER_PID=$!

        sleep 2

        # Start both teams against the same local server instance.
        (
            cd "$TEAM_DIR"
            ./start.sh -t "$TEAM1" -p $PORT
        ) > team1.log 2>&1 &
        TEAM1_PID=$!

        (
            cd "$TEAM_DIR"
            ./start.sh -t "$TEAM2" -p $PORT
        ) > team2.log 2>&1 &
        TEAM2_PID=$!

        # Allow the match to complete before collecting simulator output.
        sleep 650

        kill "$TEAM1_PID" 2>/dev/null || true
        kill "$TEAM2_PID" 2>/dev/null || true
        kill "$SERVER_PID" 2>/dev/null || true

        wait "$TEAM1_PID" 2>/dev/null || true
        wait "$TEAM2_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    )

    RCG_FILE=$(find "$GAME_DIR" -maxdepth 1 -name "*.rcg" | head -n 1 || true)
    RCL_FILE=$(find "$GAME_DIR" -maxdepth 1 -name "*.rcl" | head -n 1 || true)

    if [ -n "$RCG_FILE" ]; then
        mv "$RCG_FILE" "$RESULTS_DIR/${TEAM1}vs${TEAM2}_Game${i}.rcg"
        echo "Saved Game $i (.rcg)"
    else
        echo "WARNING: No .rcg found for game $i"
    fi

    if [ -n "$RCL_FILE" ]; then
        mv "$RCL_FILE" "$RESULTS_DIR/${TEAM1}vs${TEAM2}_Game${i}.rcl"
        echo "Saved Game $i (.rcl)"
    else
        echo "WARNING: No .rcl found for game $i"
    fi

    rm -rf "$GAME_DIR"

    echo "===== FINISHED GAME $i ====="
done

echo "ALL DONE"