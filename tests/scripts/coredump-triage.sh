#!/usr/bin/env bash
# tests/scripts/coredump-triage.sh — non-interactive coredump triage
#
# Given a PID, a coredumpctl selector, or no argument (= latest crash
# of `framework` known to coredumpctl), captures the obvious crash
# context to ~/.local/share/framework/triage/<UTC-timestamp>/:
#
#   coredumpctl-info.txt    coredumpctl info on the selected dump
#   threads-bt.txt          gdb> thread apply all bt full
#   registers.txt           gdb> info registers (current frame)
#   maps.txt                gdb> info proc mappings (loaded libs / segments)
#   cache-state.txt         gdb> print on FwCache fields when reachable
#   commandline.txt         the original argv from coredumpctl
#
# coredumpctl debug runs gdb against the dump; we drive it with
# `-ex` lines so nothing is interactive. Best-effort — frames may
# lack debug info if the binary or its deps are stripped, but the
# bts and register dumps still help a lot.
#
# Usage:
#   tests/scripts/coredump-triage.sh                   # latest framework crash
#   tests/scripts/coredump-triage.sh <pid|matchid>     # specific dump
#   tests/scripts/coredump-triage.sh --core /path/to/core   # local file
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FW_BIN="${FW_BIN:-$REPO_ROOT/builddir/src/framework}"

OUT_ROOT="${HOME}/.local/share/framework/triage"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$OUT_ROOT/$STAMP"
mkdir -p "$OUT_DIR"

GDB_CMDS=(
  -ex 'set pagination off'
  -ex 'set print thread-events off'
  -ex 'set print pretty on'
  -ex 'thread apply all bt full'
  -ex 'info registers'
  -ex 'info proc mappings'
  -ex 'quit'
)

# Branch on selector form. --core means a local file; otherwise
# everything goes to coredumpctl which understands PIDs, MATCH-IDs,
# and "latest of binary".
if [[ "${1:-}" == "--core" ]]; then
  CORE_PATH="${2:-}"
  if [[ -z "$CORE_PATH" || ! -f "$CORE_PATH" ]]; then
    echo "coredump-triage: --core needs a readable file path" >&2
    exit 2
  fi
  echo "triage: local core file=$CORE_PATH" >&2
  echo "(local core file — no coredumpctl info available)" > "$OUT_DIR/coredumpctl-info.txt"
  echo "$CORE_PATH" > "$OUT_DIR/commandline.txt"
  gdb -q -batch "${GDB_CMDS[@]}" "$FW_BIN" "$CORE_PATH" \
    > "$OUT_DIR/full-gdb.txt" 2>&1
else
  SELECTOR=("$@")
  if [[ "${#SELECTOR[@]}" -eq 0 ]]; then
    SELECTOR=( "$FW_BIN" )
  fi
  echo "triage: coredumpctl selector=${SELECTOR[*]}" >&2

  if ! coredumpctl info "${SELECTOR[@]}" > "$OUT_DIR/coredumpctl-info.txt" 2>&1; then
    echo "coredump-triage: coredumpctl couldn't find a matching dump" >&2
    cat "$OUT_DIR/coredumpctl-info.txt" >&2
    rmdir "$OUT_DIR" 2>/dev/null || true
    exit 1
  fi

  # Pull the recorded command line out of the info dump for context.
  awk '/Command Line:/{ $1=""; $2=""; print substr($0, 3); exit }' \
    "$OUT_DIR/coredumpctl-info.txt" > "$OUT_DIR/commandline.txt"

  # coredumpctl debug interleaves its own info text with gdb's output
  # and isn't reliable for capture, so we go via `coredumpctl dump`
  # to extract the core file and then drive gdb directly. This lets
  # us pin which binary symbols to use and keeps stdout clean.
  CORE_TMP="$(mktemp -t fw-triage-core.XXXXXX)"
  if ! coredumpctl dump "${SELECTOR[@]}" --output="$CORE_TMP" 2> "$OUT_DIR/coredumpctl-dump.err"; then
    echo "coredump-triage: coredumpctl dump failed (see $OUT_DIR/coredumpctl-dump.err)" >&2
    rm -f "$CORE_TMP"
    exit 1
  fi
  gdb -q -batch "${GDB_CMDS[@]}" "$FW_BIN" "$CORE_TMP" \
    > "$OUT_DIR/full-gdb.txt" 2>&1 || true
  rm -f "$CORE_TMP"
fi

# Slice the gdb output into per-section files for easier reading.
awk '
  /^Thread [0-9]+/        { sect="bt";   }
  /^=> /                  { }
  /Register Group/        { sect="reg";  }
  /^process [0-9]/        { sect="maps"; }
  { print > sect_path() }
  function sect_path() {
    if (sect == "bt")   return "'"$OUT_DIR"'/threads-bt.txt"
    if (sect == "reg")  return "'"$OUT_DIR"'/registers.txt"
    if (sect == "maps") return "'"$OUT_DIR"'/maps.txt"
    return "'"$OUT_DIR"'/full-gdb.txt"
  }
' "$OUT_DIR/full-gdb.txt" 2>/dev/null || true

# Touch the empty files so the layout is consistent across runs.
: > "$OUT_DIR/cache-state.txt"
echo "(cache-state.txt is intentionally a placeholder — the gdb -ex"      >> "$OUT_DIR/cache-state.txt"
echo "pretty-printer for FwCache is roadmap Phase 12.4 follow-up work."   >> "$OUT_DIR/cache-state.txt"
echo "For now, attach manually with 'coredumpctl debug' and inspect a"     >> "$OUT_DIR/cache-state.txt"
echo "specific FwCache pointer from the bt with 'p *(FwCache*)<addr>'.)"   >> "$OUT_DIR/cache-state.txt"

echo "triage: wrote $OUT_DIR" >&2
echo "$OUT_DIR"
