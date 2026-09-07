#!/usr/bin/env bash
set -euo pipefail

OUT=${1:-benchmark-results/local/ENVIRONMENT.md}
mkdir -p "$(dirname "$OUT")"

sha=$(git rev-parse HEAD 2>/dev/null || echo unknown)
dirty=no
[[ -n "$(git status --porcelain 2>/dev/null || true)" ]] && dirty=yes
os=$(grep '^PRETTY_NAME=' /etc/os-release 2>/dev/null | cut -d= -f2- | tr -d '"' || uname -s)
kernel=$(uname -a)
cpu=$(awk -F: '/model name/{gsub(/^ +/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null || echo unknown)
logical=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo unknown)
physical=$(awk '/physical id/{p[$4]=1} /core id/{c[$4]=1} END{if(length(c)>0) print length(c); else print "unknown"}' /proc/cpuinfo 2>/dev/null || echo unknown)
ram=$(awk '/MemTotal/{printf "%.1f GiB", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo unknown)
compiler=$(c++ --version 2>/dev/null | head -n 1 || echo unavailable)
cmake_version=$(cmake --version 2>/dev/null | head -n 1 || echo unavailable)
python_version=$(python3 --version 2>&1 || echo unavailable)
mode=native-linux
if grep -qi microsoft /proc/version 2>/dev/null; then mode=wsl2; fi
governor=unknown
if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
  governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
fi

cat > "$OUT" <<EOF
# M6B.2 Benchmark Environment

- Git SHA: \`$sha\`
- Working tree dirty: \`$dirty\`
- Execution mode: \`$mode\`
- OS: $os
- Kernel: \`$kernel\`
- CPU: $cpu
- Physical cores (best effort): \`$physical\`
- Logical CPUs: \`$logical\`
- RAM: $ram
- Compiler: \`$compiler\`
- CMake: \`$cmake_version\`
- Python: \`$python_version\`
- CPU scaling governor: \`$governor\`
- Client placement: same-machine loopback (127.0.0.1)

## Interpretation boundary

This file records the host used for local runtime comparison. WSL2 results are valid for the documented WSL2 environment, but they must not be described as native-Linux or internet-facing capacity.

The first-party Python load generator prioritizes reproducibility over maximum request-generation capacity. A plateau therefore requires follow-up before attributing it solely to the server runtime.
EOF

printf 'wrote %s\n' "$OUT"
