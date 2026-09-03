#!/usr/bin/env bash
set -euo pipefail

TOTAL_GAMES=1
PARALLEL_GAMES=1

RESULTS_DIR="/mnt/c/Users/User/RyanFC Results"
WORK_ROOT="$HOME/robocup_tmp_runs"

TEAM1_NAME="HELIOS_base"
TEAM1_DIR="$HOME/ryansrobocup/helios-base/src"

TEAM2_NAME="Ryan_FC"
TEAM2_DIR="$HOME/ryansrobocup/ryans-football-team/src"

BASE_PORT=6000
PORT_STEP=20
MAX_GAME_SECONDS=1500

mkdir -p "$RESULTS_DIR" "$WORK_ROOT"

# Resume numbering from the latest completed HELIOS vs Ryan_FC match.
LAST_GAME=$(find "$RESULTS_DIR" -maxdepth 1 -type f -name "${TEAM1_NAME}vs${TEAM2_NAME}_Game*.rcg" \
  | sed -E 's/.*_Game([0-9]+)\.rcg/\1/' \
  | sort -n \
  | tail -n 1 || true)
LAST_GAME=${LAST_GAME:-0}

echo "Last existing game: $LAST_GAME"

wait_for_game_end() {
    # Keep the simulator running long enough for the match to complete.
    sleep 280
}

cleanup_port_processes() {
    local port="$1"

    # Remove any remaining team processes attached to the match port.
    pkill -f "sample_player.*-p $port" 2>/dev/null || true
    pkill -f "sample_coach.*-p $port" 2>/dev/null || true
    pkill -f "sample_trainer.*-p $port" 2>/dev/null || true
}

run_one_game() {
    local game_num="$1"
    local slot="$2"

    local port=$((BASE_PORT + slot * PORT_STEP))
    local coach_port=$((port + 1))
    local olcoach_port=$((port + 2))

    local game_dir="$WORK_ROOT/tmp_game_$game_num"
    rm -rf "$game_dir"
    mkdir -p "$game_dir"

    echo "===== STARTING GAME $game_num on port $port ====="
    echo "Left team:  $TEAM1_NAME"
    echo "Right team: $TEAM2_NAME"

    (
        cd "$game_dir"

        # Start an isolated server instance for this match.
        local -a server_cmd=(
            rcssserver
            "-server::auto_mode=true"
            "-server::synch_mode=true"
            "-server::half_time=300"
            "-server::port=$port"
            "-server::coach_port=$coach_port"
            "-server::olcoach_port=$olcoach_port"
            "-server::game_logging=true"
            "-server::text_logging=true"
        )

        "${server_cmd[@]}" > server.log 2>&1 &
        SERVER_PID=$!

        sleep 4

        # Start each team from its own source directory.
        (
            cd "$TEAM1_DIR"
            ./start.sh -t "$TEAM1_NAME" -p "$port"
        ) > team1.log 2>&1 &

        (
            cd "$TEAM2_DIR"
            ./start.sh -t "$TEAM2_NAME" -p "$port"
        ) > team2.log 2>&1 &

        wait_for_game_end

        sleep 12

        cleanup_port_processes "$port"

        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true

        sleep 3
    )

    local rcg_file=""
    local rcl_file=""

    rcg_file=$(find "$game_dir" -maxdepth 1 -type f -name "*.rcg" | head -n 1 || true)
    rcl_file=$(find "$game_dir" -maxdepth 1 -type f -name "*.rcl" | head -n 1 || true)

    if [[ -n "$rcg_file" ]]; then
        cp "$rcg_file" "$RESULTS_DIR/${TEAM1_NAME}vs${TEAM2_NAME}_Game${game_num}.rcg"
        echo "Saved Game $game_num (.rcg)"
    else
        echo "WARNING: No .rcg found for game $game_num"
    fi

    if [[ -n "$rcl_file" ]]; then
        cp "$rcl_file" "$RESULTS_DIR/${TEAM1_NAME}vs${TEAM2_NAME}_Game${game_num}.rcl"
        echo "Saved Game $game_num (.rcl)"
    else
        echo "WARNING: No .rcl found for game $game_num"
    fi

    cd "$WORK_ROOT"
    sleep 2
    rm -rf "$game_dir"

    echo "===== FINISHED GAME $game_num ====="
}

current=$((LAST_GAME + 1))
end=$((LAST_GAME + TOTAL_GAMES))

while [[ "$current" -le "$end" ]]; do
    pids=()

    # Supports parallel execution if PARALLEL_GAMES is increased later.
    for slot in $(seq 0 $((PARALLEL_GAMES - 1))); do
        if [[ "$current" -le "$end" ]]; then
            run_one_game "$current" "$slot" &
            pids+=("$!")
            current=$((current + 1))
        fi
    done

    # Wait for the current batch before reusing port slots.
    for pid in "${pids[@]}"; do
        wait "$pid"
    done
done

echo "ALL DONE"