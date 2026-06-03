#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

CONFIG_FILE="$SCRIPT_DIR/block_vs_baseline.conf"
PRESET="default"

OPS="put get del"
SIZE=2097152
TOTAL_GIB=20
THREADS="1 4 8"
ROUNDS=1
FULL_TOTAL_GIB=100
FULL_THREADS="1 2 4 8"
FULL_ROUNDS=3
HOST="127.0.0.1"
PORT=55510
CAPACITY=107374182400
BLOCK_DATA_DIR="/tmp/opencode/falcon-block-data"
RESULT_DIR="/tmp/opencode/block-vs-baseline-results"
POSIX_ROOT="/tmp/compare_block_vs_baseline_posix"
CLEANUP=true
REQUIRE_NO_ERRORS=true
MIN_FREE_GIB_EXTRA=20
BLOCK_VERIFY=false

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --config PATH          Config file path (default: tests/block_api/block_vs_baseline.conf)
  --preset default       Only 20GiB default mode is supported
  --ops "put get del"   Operations to run
  --size BYTES          Operation size in bytes
  --total-gib GiB       Total data per case
  --threads "1 4 8"     Thread counts
  --rounds N            Rounds per op/thread/case
  --result-dir DIR      Output directory
  --keep-data           Do not clean generated test data
  --help                Show this help

Default config runs put/get/del with 2MiB ops, 20GiB total, threads 1/4/8, rounds=1.
EOF
}

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

parse_bool() {
    local value
    value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
    case "$value" in
        true|1|yes|on) printf 'true' ;;
        false|0|no|off) printf 'false' ;;
        *) echo "invalid boolean: $1" >&2; exit 1 ;;
    esac
}

apply_config_value() {
    local key="$1"
    local value="$2"
    case "$key" in
        ops) OPS="$value" ;;
        size) SIZE="$value" ;;
        total_gib) TOTAL_GIB="$value" ;;
        threads) THREADS="$value" ;;
        rounds) ROUNDS="$value" ;;
        full_total_gib) FULL_TOTAL_GIB="$value" ;;
        full_threads) FULL_THREADS="$value" ;;
        full_rounds) FULL_ROUNDS="$value" ;;
        host) HOST="$value" ;;
        port) PORT="$value" ;;
        capacity) CAPACITY="$value" ;;
        block_data_dir) BLOCK_DATA_DIR="$value" ;;
        result_dir) RESULT_DIR="$value" ;;
        posix_root) POSIX_ROOT="$value" ;;
        cleanup) CLEANUP=$(parse_bool "$value") ;;
        require_no_errors) REQUIRE_NO_ERRORS=$(parse_bool "$value") ;;
        min_free_gib_extra) MIN_FREE_GIB_EXTRA="$value" ;;
        block_verify) BLOCK_VERIFY=$(parse_bool "$value") ;;
        "") ;;
        *) echo "unknown config key: $key" >&2; exit 1 ;;
    esac
}

load_config() {
    [[ -f "$CONFIG_FILE" ]] || return 0
    local line key value
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%%#*}"
        line=$(trim "$line")
        [[ -z "$line" ]] && continue
        if [[ "$line" != *=* ]]; then
            echo "invalid config line: $line" >&2
            exit 1
        fi
        key=$(trim "${line%%=*}")
        value=$(trim "${line#*=}")
        apply_config_value "$key" "$value"
    done < "$CONFIG_FILE"
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --config) CONFIG_FILE="$2"; shift 2 ;;
            --preset) PRESET="$2"; shift 2 ;;
            --ops) OPS="$2"; shift 2 ;;
            --size) SIZE="$2"; shift 2 ;;
            --total-gib) TOTAL_GIB="$2"; shift 2 ;;
            --threads) THREADS="$2"; shift 2 ;;
            --rounds) ROUNDS="$2"; shift 2 ;;
            --result-dir) RESULT_DIR="$2"; shift 2 ;;
            --keep-data) CLEANUP=false; shift ;;
            --help|-h) usage; exit 0 ;;
            *) echo "unknown argument: $1" >&2; usage; exit 1 ;;
        esac
    done
}

apply_preset() {
    case "$PRESET" in
        default) ;;
        full) echo "full preset is disabled; this runner is scoped to 20GiB mode" >&2; exit 1 ;;
        *) echo "unknown preset: $PRESET" >&2; exit 1 ;;
    esac
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing command: $1" >&2; exit 1; }
}

check_port() {
    if ! timeout 2 bash -c "cat < /dev/null > /dev/tcp/$HOST/$PORT" >/dev/null 2>&1; then
        echo "Falcon endpoint $HOST:$PORT is not reachable" >&2
        exit 1
    fi
}

ceil_div() {
    local a="$1"
    local b="$2"
    echo $(((a + b - 1) / b))
}

check_disk_space() {
    local thread_count=0
    local t
    for t in $THREADS; do
        thread_count=$((thread_count + 1))
    done

    local block_required_gib=$((TOTAL_GIB * thread_count * ROUNDS + MIN_FREE_GIB_EXTRA))
    local posix_required_gib=$((TOTAL_GIB + MIN_FREE_GIB_EXTRA))

    check_path_free_space "$BLOCK_DATA_DIR" "$block_required_gib" "block_data_dir"
    check_path_free_space "$POSIX_ROOT" "$posix_required_gib" "posix_root"
    check_path_free_space "$RESULT_DIR" 1 "result_dir"
}

check_path_free_space() {
    local path="$1"
    local required_gib="$2"
    local label="$3"
    local check_path="$path"

    while [[ ! -e "$check_path" ]]; do
        check_path=$(dirname "$check_path")
    done

    local avail_kib
    avail_kib=$(df -Pk "$check_path" | awk 'NR==2 {print $4}')
    local avail_gib=$((avail_kib / 1024 / 1024))
    if ((avail_gib < required_gib)); then
        echo "not enough free space for ${label} (${path}): available=${avail_gib}GiB required=${required_gib}GiB" >&2
        exit 1
    fi
}

validate_inputs() {
    [[ "$SIZE" =~ ^[0-9]+$ ]] || { echo "size must be integer" >&2; exit 1; }
    [[ "$TOTAL_GIB" =~ ^[0-9]+$ ]] || { echo "total_gib must be integer" >&2; exit 1; }
    [[ "$ROUNDS" =~ ^[0-9]+$ ]] || { echo "rounds must be integer" >&2; exit 1; }
    ((SIZE > 0 && TOTAL_GIB > 0 && ROUNDS > 0)) || { echo "size/total_gib/rounds must be positive" >&2; exit 1; }

    local total_bytes=$((TOTAL_GIB * 1024 * 1024 * 1024))
    if ((total_bytes % SIZE != 0)); then
        echo "total_gib * GiB must be divisible by size" >&2
        exit 1
    fi
    TOTAL_OPS=$((total_bytes / SIZE))

    local t
    for t in $THREADS; do
        [[ "$t" =~ ^[0-9]+$ ]] || { echo "invalid thread count: $t" >&2; exit 1; }
        ((t > 0)) || { echo "thread count must be positive" >&2; exit 1; }
        if ((TOTAL_OPS % t != 0)); then
            echo "total_ops=$TOTAL_OPS must be divisible by threads=$t" >&2
            exit 1
        fi
    done

    local op
    for op in $OPS; do
        case "$op" in
            put|get|del) ;;
            *) echo "unsupported op: $op" >&2; exit 1 ;;
        esac
    done

    local thread_count=0
    for t in $THREADS; do
        thread_count=$((thread_count + 1))
    done
    local capacity_gib=$((CAPACITY / 1024 / 1024 / 1024))
    local append_gib=$((TOTAL_GIB * thread_count * ROUNDS))
    if ((append_gib > capacity_gib)); then
        echo "20GiB mode requires cumulative append (${append_gib}GiB) <= capacity (${capacity_gib}GiB)" >&2
        exit 1
    fi
}

contains_op() {
    local expected="$1"
    local op
    for op in $OPS; do
        [[ "$op" == "$expected" ]] && return 0
    done
    return 1
}

log_progress() {
    local message="$1"
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$message"
}

prepare_outputs() {
    mkdir -p "$RESULT_DIR/logs" "$BLOCK_DATA_DIR" /tmp/opencode
    CSV_FILE="$RESULT_DIR/block_vs_baseline.csv"
    MD_FILE="$RESULT_DIR/block_vs_baseline.md"
    : > "$CSV_FILE"
    : > "$MD_FILE"
    echo "case,op,round,size,threads,total_ops,total_gib,time_sec,ops_sec,mib_sec,avg_latency_ns,avg_latency_us,p50_us,p95_us,p99_us,errors,prefix,log" >> "$CSV_FILE"
    {
        echo "# Block API vs POSIX Baseline"
        echo
        echo "- size: $SIZE bytes"
        echo "- total_gib: $TOTAL_GIB"
        echo "- total_ops: $TOTAL_OPS"
        echo "- ops: $OPS"
        echo "- threads: $THREADS"
        echo "- rounds: $ROUNDS"
        echo
        echo "| case | op | round | threads | MiB/s | ops/s | avg latency ms | p50 us | p95 us | p99 us | errors |"
        echo "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    } >> "$MD_FILE"
}

block_data_file() {
    echo "$BLOCK_DATA_DIR/data_${SIZE}.dat"
}

block_cleanup() {
    local prefix="$1"
    "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op del --size "$SIZE" --capacity "$CAPACITY" \
        --block-data-dir "$BLOCK_DATA_DIR" \
        --keys "$TOTAL_OPS" --threads 1 --prefix "$prefix" --phase put --no-prepare --csv \
        > /dev/null 2>&1 || true
}

parse_block_csv() {
    local line="$1"
    IFS=',' read -r _op _size _threads _keys _attempt_ops success_ops _total_bytes seconds ops_sec mib_sec avg_us p50_us p95_us p99_us errors _error_codes _prefix <<< "$line"
    avg_ns=$(awk -v us="$avg_us" 'BEGIN { printf "%.3f", us * 1000.0 }')
}

run_block_prepare() {
    local prefix="$1"
    local log="$2"
    local round="${3:-?}"
    local threads="${4:-1}"
    log_progress "start block prepare round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$log"
    "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op put --size "$SIZE" --capacity "$CAPACITY" \
        --block-data-dir "$BLOCK_DATA_DIR" \
        --keys "$TOTAL_OPS" --threads 1 --prefix "$prefix" --csv > "$log" 2>&1
    local line
    line=$(tail -n 1 "$log")
    parse_block_csv "$line"
    if [[ "$REQUIRE_NO_ERRORS" == true && "$errors" != "0" ]]; then
        echo "block prepare failed with errors=$errors, log=$log" >&2
        exit 1
    fi
}

run_block_case() {
    local op="$1"
    local round="$2"
    local threads="$3"
    local prefix="bvb_block_${op}_r${round}_t${threads}_$RANDOM"
    local log="$RESULT_DIR/logs/block_${op}_r${round}_t${threads}.log"
    local measured_phase="put"

    block_cleanup "$prefix"

    if [[ "$op" == "get" || "$op" == "del" ]]; then
        run_block_prepare "$prefix" "$RESULT_DIR/logs/block_${op}_prepare_r${round}_t${threads}.log" "$round" "$threads"
    fi

    local verify_args=()
    if [[ "$op" == "get" && "$BLOCK_VERIFY" == true ]]; then
        verify_args+=(--verify)
    fi

    if [[ "$op" == "put" ]]; then
        log_progress "start block put round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$log"
        "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op put --size "$SIZE" --capacity "$CAPACITY" \
            --block-data-dir "$BLOCK_DATA_DIR" \
            --keys "$TOTAL_OPS" --threads "$threads" --prefix "$prefix" --csv > "$log" 2>&1
    else
        log_progress "start block $op round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$log"
        "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op "$op" --size "$SIZE" --capacity "$CAPACITY" \
            --block-data-dir "$BLOCK_DATA_DIR" \
            --keys "$TOTAL_OPS" --threads "$threads" --prefix "$prefix" --phase "$measured_phase" \
            --no-prepare --csv "${verify_args[@]}" > "$log" 2>&1
    fi

    local line
    line=$(tail -n 1 "$log")
    parse_block_csv "$line"
    if [[ "$REQUIRE_NO_ERRORS" == true && "$errors" != "0" ]]; then
        echo "block $op failed with errors=$errors, log=$log" >&2
        exit 1
    fi

    append_result "block" "$op" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
        "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "$prefix" "$log"

    if [[ "$CLEANUP" == true ]]; then
        block_cleanup "$prefix"
    fi
}

run_block_group() {
    local round="$1"
    local threads="$2"
    local prefix="bvb_block_r${round}_t${threads}_$RANDOM"
    local verify_args=()

    block_cleanup "$prefix"

    if contains_op put; then
        local put_log="$RESULT_DIR/logs/block_put_r${round}_t${threads}.log"
        log_progress "start block put round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$put_log"
        "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op put --size "$SIZE" --capacity "$CAPACITY" \
            --block-data-dir "$BLOCK_DATA_DIR" \
            --keys "$TOTAL_OPS" --threads "$threads" --prefix "$prefix" --csv > "$put_log" 2>&1
        local line
        line=$(tail -n 1 "$put_log")
        parse_block_csv "$line"
        if [[ "$REQUIRE_NO_ERRORS" == true && "$errors" != "0" ]]; then
            echo "block put failed with errors=$errors, log=$put_log" >&2
            exit 1
        fi
        append_result "block" "put" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "$prefix" "$put_log"
    elif contains_op get || contains_op del; then
        run_block_prepare "$prefix" "$RESULT_DIR/logs/block_prepare_r${round}_t${threads}.log" "$round" "$threads"
    fi

    if contains_op get; then
        local get_log="$RESULT_DIR/logs/block_get_r${round}_t${threads}.log"
        if [[ "$BLOCK_VERIFY" == true ]]; then
            verify_args+=(--verify)
        fi
        log_progress "start block get round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$get_log"
        "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op get --size "$SIZE" --capacity "$CAPACITY" \
            --block-data-dir "$BLOCK_DATA_DIR" \
            --keys "$TOTAL_OPS" --threads "$threads" --prefix "$prefix" --phase put --no-prepare --csv \
            "${verify_args[@]}" > "$get_log" 2>&1
        local line
        line=$(tail -n 1 "$get_log")
        parse_block_csv "$line"
        if [[ "$REQUIRE_NO_ERRORS" == true && "$errors" != "0" ]]; then
            echo "block get failed with errors=$errors, log=$get_log" >&2
            exit 1
        fi
        append_result "block" "get" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "$prefix" "$get_log"
    fi

    if contains_op del; then
        local del_log="$RESULT_DIR/logs/block_del_r${round}_t${threads}.log"
        log_progress "start block del round=$round threads=$threads total_gib=$TOTAL_GIB prefix=$prefix log=$del_log"
        "$BLOCK_BENCH" --host "$HOST" --port "$PORT" --op del --size "$SIZE" --capacity "$CAPACITY" \
            --block-data-dir "$BLOCK_DATA_DIR" \
            --keys "$TOTAL_OPS" --threads "$threads" --prefix "$prefix" --phase put --no-prepare --csv \
            > "$del_log" 2>&1
        local line
        line=$(tail -n 1 "$del_log")
        parse_block_csv "$line"
        if [[ "$REQUIRE_NO_ERRORS" == true && "$errors" != "0" ]]; then
            echo "block del failed with errors=$errors, log=$del_log" >&2
            exit 1
        fi
        append_result "block" "del" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "$prefix" "$del_log"
    elif [[ "$CLEANUP" == true ]]; then
        block_cleanup "$prefix"
    fi
}

posix_port_for() {
    local round="$1"
    local threads="$2"
    local op_idx="$3"
    echo $((12000 + round * 100 + threads * 10 + op_idx))
}

posix_prepare_dirs() {
    local root="$1"
    local port="$2"
    local threads="$3"
    local i
    rm -rf "$root"
    for ((i = 0; i < threads; ++i)); do
        mkdir -p "$root/client_0_${port}/thread_${i}"
    done
}

run_posix_round() {
    local root="$1"
    local files_per_thread="$2"
    local threads="$3"
    local round_idx="$4"
    local port="$5"
    local log="$6"
    local label="${7:-round_idx=$round_idx}"
    log_progress "start posix $label threads=$threads files_per_thread=$files_per_thread size=$SIZE log=$log"
    "$POSIX_BENCH" "$root/" "$files_per_thread" "$threads" "$round_idx" 0 1 16384 "$port" "$SIZE" 1 \
        > "$log" 2>&1 &
    local pid=$!
    sleep 1
    python3 "$SEND_SIGNAL" 127.0.0.1 "$port" >/dev/null 2>&1 || true
    wait "$pid"
}

parse_posix_log() {
    local log="$1"
    if grep -q "Failed to open file\|Failed to write file" "$log"; then
        echo "POSIX workload failed, log=$log" >&2
        exit 1
    fi
    local line
    line=$(grep "\[FINISH\]" "$log" | tail -n 1)
    if [[ -z "$line" ]]; then
        echo "missing POSIX [FINISH] line, log=$log" >&2
        exit 1
    fi
    seconds=$(awk -F', ' '{for (i=1;i<=NF;i++) if ($i ~ /^Time /) {split($i,a," "); print a[2]}}' <<< "$line")
    success_ops=$(awk -F', ' '{for (i=1;i<=NF;i++) if ($i ~ /^OPs /) {split($i,a," "); print a[2]}}' <<< "$line")
    ops_sec=$(awk -F', ' '{for (i=1;i<=NF;i++) if ($i ~ /^Throughput /) {split($i,a," "); print a[2]}}' <<< "$line")
    avg_ns=$(awk -F', ' '{for (i=1;i<=NF;i++) if ($i ~ /^Average Latency /) {split($i,a," "); print a[3]}}' <<< "$line")
    mib_sec=$(awk -v ops="$ops_sec" -v size="$SIZE" 'BEGIN { printf "%.3f", ops * size / 1024.0 / 1024.0 }')
    avg_us=$(awk -v ns="$avg_ns" 'BEGIN { printf "%.3f", ns / 1000.0 }')
    p50_us=""
    p95_us=""
    p99_us=""
    errors=0
}

run_posix_case() {
    local op="$1"
    local round="$2"
    local threads="$3"
    local op_idx="$4"
    local files_per_thread=$((TOTAL_OPS / threads))
    local port
    port=$(posix_port_for "$round" "$threads" "$op_idx")
    local root="${POSIX_ROOT}_${op}_r${round}_t${threads}"
    local log="$RESULT_DIR/logs/posix_${op}_r${round}_t${threads}.log"
    local prepare_log="$RESULT_DIR/logs/posix_${op}_prepare_r${round}_t${threads}.log"

    posix_prepare_dirs "$root" "$port" "$threads"

    case "$op" in
        put)
            run_posix_round "$root" "$files_per_thread" "$threads" 8 "$port" "$log" "put round=$round"
            ;;
        get)
            run_posix_round "$root" "$files_per_thread" "$threads" 8 "$port" "$prepare_log" "prepare-get round=$round"
            run_posix_round "$root" "$files_per_thread" "$threads" 10 "$port" "$log" "get round=$round"
            ;;
        del)
            run_posix_round "$root" "$files_per_thread" "$threads" 8 "$port" "$prepare_log" "prepare-del round=$round"
            run_posix_round "$root" "$files_per_thread" "$threads" 5 "$port" "$log" "del round=$round"
            ;;
    esac

    parse_posix_log "$log"
    append_result "posix" "$op" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
        "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "" "$log"

    if [[ "$CLEANUP" == true ]]; then
        rm -rf "$root"
    fi
}

run_posix_group() {
    local round="$1"
    local threads="$2"
    local files_per_thread=$((TOTAL_OPS / threads))
    local port
    port=$(posix_port_for "$round" "$threads" 0)
    local root="${POSIX_ROOT}_r${round}_t${threads}"

    posix_prepare_dirs "$root" "$port" "$threads"

    if contains_op put; then
        local put_log="$RESULT_DIR/logs/posix_put_r${round}_t${threads}.log"
        run_posix_round "$root" "$files_per_thread" "$threads" 8 "$port" "$put_log" "put round=$round"
        parse_posix_log "$put_log"
        append_result "posix" "put" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "" "$put_log"
    elif contains_op get || contains_op del; then
        run_posix_round "$root" "$files_per_thread" "$threads" 8 "$port" \
            "$RESULT_DIR/logs/posix_prepare_r${round}_t${threads}.log" "prepare round=$round"
    fi

    if contains_op get; then
        local get_log="$RESULT_DIR/logs/posix_get_r${round}_t${threads}.log"
        run_posix_round "$root" "$files_per_thread" "$threads" 10 "$port" "$get_log" "get round=$round"
        parse_posix_log "$get_log"
        append_result "posix" "get" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "" "$get_log"
    fi

    if contains_op del; then
        local del_log="$RESULT_DIR/logs/posix_del_r${round}_t${threads}.log"
        run_posix_round "$root" "$files_per_thread" "$threads" 5 "$port" "$del_log" "del round=$round"
        parse_posix_log "$del_log"
        append_result "posix" "del" "$round" "$threads" "$success_ops" "$seconds" "$ops_sec" "$mib_sec" \
            "$avg_ns" "$avg_us" "$p50_us" "$p95_us" "$p99_us" "$errors" "" "$del_log"
    fi

    if [[ "$CLEANUP" == true ]]; then
        rm -rf "$root"
    fi
}

append_result() {
    local case_name="$1"
    local op="$2"
    local round="$3"
    local threads="$4"
    local ops="$5"
    local seconds="$6"
    local ops_sec="$7"
    local mib_sec="$8"
    local avg_ns="$9"
    local avg_us="${10}"
    local p50_us="${11}"
    local p95_us="${12}"
    local p99_us="${13}"
    local errors="${14}"
    local prefix="${15}"
    local log="${16}"

    echo "$case_name,$op,$round,$SIZE,$threads,$ops,$TOTAL_GIB,$seconds,$ops_sec,$mib_sec,$avg_ns,$avg_us,$p50_us,$p95_us,$p99_us,$errors,$prefix,$log" >> "$CSV_FILE"
    local avg_ms
    avg_ms=$(awk -v us="$avg_us" 'BEGIN { printf "%.3f", us / 1000.0 }')
    echo "| $case_name | $op | $round | $threads | $mib_sec | $ops_sec | $avg_ms | $p50_us | $p95_us | $p99_us | $errors |" >> "$MD_FILE"
    echo "[$case_name][$op][round=$round][threads=$threads] ${mib_sec} MiB/s, ${ops_sec} ops/s, avg ${avg_ms} ms"
}

main() {
    parse_args "$@"
    load_config
    parse_args "$@"
    apply_preset

    if [[ -f "$REPO_ROOT/deploy/falcon_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$REPO_ROOT/deploy/falcon_env.sh" >/dev/null
    fi

    BLOCK_BENCH="$REPO_ROOT/build/tests/block_api/BlockApiBench"
    POSIX_BENCH="$REPO_ROOT/build/tests/private-directory-test/test_posix"
    SEND_SIGNAL="$REPO_ROOT/tests/private-directory-test/send_signal.py"

    require_cmd awk
    require_cmd grep
    require_cmd python3
    [[ -x "$BLOCK_BENCH" ]] || { echo "missing executable: $BLOCK_BENCH" >&2; exit 1; }
    [[ -x "$POSIX_BENCH" ]] || { echo "missing executable: $POSIX_BENCH" >&2; exit 1; }
    [[ -f "$SEND_SIGNAL" ]] || { echo "missing send_signal.py: $SEND_SIGNAL" >&2; exit 1; }

    validate_inputs
    check_port
    check_disk_space
    prepare_outputs

    export FALCON_BLOCK_DATA_DIR="$BLOCK_DATA_DIR"
    export FALCON_BLOCK_FILE_CAPACITY="$CAPACITY"

    local round threads
    for ((round = 1; round <= ROUNDS; ++round)); do
        for threads in $THREADS; do
            run_block_group "$round" "$threads"
            run_posix_group "$round" "$threads"
        done
    done

    echo "CSV: $CSV_FILE"
    echo "Markdown: $MD_FILE"
}

main "$@"
