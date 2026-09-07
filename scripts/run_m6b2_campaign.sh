#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

RUNS=${RUNS_PER_RUNTIME:-5}
REQUESTS=${REQUESTS_PER_SCENARIO:-10000}
WARMUP=${WARMUP_REQUESTS:-500}
WORKERS=${THREADPOOL_WORKERS:-4}
ADMISSION=${ADMISSION_BUDGET:-260}
PAYLOAD=${PAYLOAD_BYTES:-128}
KEEPALIVE_CONCURRENCIES=${KEEPALIVE_CONCURRENCIES:-2,4,8,16,32}
CONNECT_CONCURRENCIES=${CONNECT_CONCURRENCIES:-8,16,32,64}
ALLOW_DIRTY=${ALLOW_DIRTY_EVIDENCE:-0}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: required command not found: $1" >&2
    exit 2
  }
}

for cmd in git cmake c++ python3; do
  require_cmd "$cmd"
done

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "ERROR: epoll M6B.2 evidence must run on Linux or WSL2." >&2
  exit 2
fi

if [[ "$RUNS" -lt 5 ]]; then
  echo "ERROR: RUNS_PER_RUNTIME must be >= 5 for publishable M6B.2 evidence." >&2
  exit 2
fi

if [[ -n "$(git status --porcelain)" && "$ALLOW_DIRTY" != "1" ]]; then
  echo "ERROR: working tree is dirty. Commit/stash changes before benchmark evidence." >&2
  exit 2
fi

SHA=$(git rev-parse HEAD)
SHORT_SHA=$(git rev-parse --short=8 HEAD)
STAMP=$(date -u +%Y-%m-%dT%H-%M-%SZ)
HOST=$(hostname | tr -cd '[:alnum:]._-')
RUN_ID=${RUN_ID:-${STAMP}_${SHORT_SHA}_${HOST}}
CAMPAIGN_DIR=${CAMPAIGN_DIR:-benchmark-results/local/m6b2-$RUN_ID}
mkdir -p "$CAMPAIGN_DIR"

LOG="$CAMPAIGN_DIR/campaign.log"
exec > >(tee -a "$LOG") 2>&1

printf 'M6B.2 campaign: %s\ncommit: %s\n' "$RUN_ID" "$SHA"
printf 'runs/runtime: %s; requests/scenario: %s\n' "$RUNS" "$REQUESTS"

bash scripts/capture_benchmark_environment.sh "$CAMPAIGN_DIR/ENVIRONMENT.md"

rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

SCENARIOS=()

run_scenario() {
  local mode=$1
  local concurrency=$2
  local name="${mode}-c${concurrency}"
  local out="$CAMPAIGN_DIR/$name"
  mkdir -p "$out"
  SCENARIOS+=("$name")

  python3 tools/compare_runtimes.py \
    --server ./build/vhttp_bench_server \
    --output-dir "$out" \
    --runtimes threadpool,epoll \
    --runs "$RUNS" \
    --admission "$ADMISSION" \
    --workers "$WORKERS" \
    --payload "$PAYLOAD" \
    --requests "$REQUESTS" \
    --concurrency "$concurrency" \
    --warmup "$WARMUP" \
    --mode "$mode" \
    --require-identified-build

  python3 tools/analyze_runtime_pairs.py "$out" --output "$out/paired-analysis.json"
}

IFS=',' read -r -a KEEPALIVE_ARRAY <<< "$KEEPALIVE_CONCURRENCIES"
for concurrency in "${KEEPALIVE_ARRAY[@]}"; do
  run_scenario keepalive "$concurrency"
done

IFS=',' read -r -a CONNECT_ARRAY <<< "$CONNECT_CONCURRENCIES"
for concurrency in "${CONNECT_ARRAY[@]}"; do
  run_scenario connect "$concurrency"
done

MODE=native-linux
if grep -qi microsoft /proc/version 2>/dev/null; then MODE=wsl2; fi
DATE_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)

{
  printf '{\n'
  printf '  "schema_version": 1,\n'
  printf '  "run_id": "%s",\n' "$RUN_ID"
  printf '  "generated_at_utc": "%s",\n' "$DATE_UTC"
  printf '  "commit_sha": "%s",\n' "$SHA"
  printf '  "execution_mode": "%s",\n' "$MODE"
  printf '  "runs_per_runtime": %s,\n' "$RUNS"
  printf '  "requests_per_scenario": %s,\n' "$REQUESTS"
  printf '  "threadpool_workers": %s,\n' "$WORKERS"
  printf '  "admission_budget": %s,\n' "$ADMISSION"
  printf '  "payload_bytes": %s,\n' "$PAYLOAD"
  printf '  "scenarios": ['
  for i in "${!SCENARIOS[@]}"; do
    [[ "$i" -gt 0 ]] && printf ', '
    printf '"%s"' "${SCENARIOS[$i]}"
  done
  printf ']\n}\n'
} > "$CAMPAIGN_DIR/campaign.json"

python3 tools/validate_m6b2_campaign.py "$CAMPAIGN_DIR" \
  --output "$CAMPAIGN_DIR/campaign-validation.json"

cat > "$CAMPAIGN_DIR/README.md" <<EOF
# M6B.2 runtime campaign: $RUN_ID

- Commit: \`$SHA\`
- Execution mode: \`$MODE\`
- Runtimes: threadpool vs Linux epoll
- Runs per runtime/scenario: \`$RUNS\`
- Requests per scenario: \`$REQUESTS\`
- Client placement: same-machine loopback

This is a local comparative benchmark campaign, not an internet-capacity claim. Review \`ENVIRONMENT.md\`, every raw client/server JSON/log, each \`summary.json\`, each \`paired-analysis.json\`, and \`campaign-validation.json\` before curating results into \`benchmark-results/curated/\`.

A throughput plateau must not automatically be attributed to the server because the first-party Python load generator may itself become limiting at high rates.
EOF

printf '\nPASS: M6B.2 campaign created at %s\n' "$CAMPAIGN_DIR"
printf 'Review the full raw evidence before copying any scenario into benchmark-results/curated/.\n'
