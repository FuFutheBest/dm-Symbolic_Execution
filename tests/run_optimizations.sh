#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PYTHON=${PYTHON:-.venv/bin/python}
make PYTHON="$PYTHON" -j4
z3_dir=$("$PYTHON" -c 'import z3,os; print(os.path.dirname(z3.__file__))')
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Isee -Iminic -I"$z3_dir/include" \
  tests/optimization_driver.cpp build/obj/see/{core,engine,optimizations}.o \
  build/obj/minic/*.o -L"$z3_dir/lib" -Wl,-rpath,"$z3_dir/lib" -lz3 \
  -o build/optimization-test
variants=('' '--incremental' '--batch' '--no-fold' '--simplify' '--array' '--prune' '--unlimited')
names=(default incremental batch no-fold simplify array prune unlimited)
for k in "${!variants[@]}"; do
  echo "=== ${names[$k]} regression ==="
  ./build/optimization-test ${variants[$k]} --self-test
done
if [[ ${1:-} == --bench ]]; then
  mkdir -p build/optimization-results
  for k in "${!variants[@]}"; do
    ./build/grade -j 1 --solver "./build/optimization-test ${variants[$k]}" \
      --json "build/optimization-results/${names[$k]}.json"
  done
  "$PYTHON" - <<'PY'
import json
from pathlib import Path
for name in ('default', 'incremental', 'batch', 'no-fold', 'simplify', 'array', 'prune', 'unlimited'):
    p = Path('build/optimization-results') / (name + '.json')
    rows = list(json.loads(p.read_text())['tasks'].values())
    print(f'{p.stem:16s} {sum(r["points"] == 1 for r in rows):2d}/23 '
          f'{sum(r["elapsed"] for r in rows):8.3f}s')
PY
fi
