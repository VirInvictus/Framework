#!/usr/bin/env bash
# tests/scripts/debug.sh — gdb wrapper for crash investigation
#
# Runs ./builddir/src/framework under gdb in batch mode with the
# breakpoints from tests/scripts/framework.gdb pre-loaded. On crash
# (SIGSEGV / SIGABRT) prints a full multi-thread backtrace.
#
# Usage:
#   tests/scripts/debug.sh <doc> [args...]
#   tests/scripts/debug.sh                     # opens with no document
#
# Env:
#   FW_DEBUG=1                — enable trace logging (forwarded)
#   FW_BIN=builddir/src/...   — override the binary path
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

# Resolve repo root — script lives at tests/scripts/debug.sh, repo at $script/../..
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GDB_INIT="$SCRIPT_DIR/framework.gdb"

FW_BIN="${FW_BIN:-$REPO_ROOT/builddir/src/framework}"
SCHEMA_DIR="${GSETTINGS_SCHEMA_DIR:-$REPO_ROOT/builddir/data}"

if [[ ! -x "$FW_BIN" ]]; then
  echo "debug.sh: binary not found at $FW_BIN — run 'meson compile -C builddir' first." >&2
  exit 1
fi
if [[ ! -f "$GDB_INIT" ]]; then
  echo "debug.sh: missing $GDB_INIT" >&2
  exit 1
fi

# `-batch` runs commands and exits — never drops into interactive
# gdb. `-ex run` starts the binary; `-ex 'thread apply all bt'`
# fires after a crash to dump every thread. The `--args` form passes
# the remaining argv straight to the program.
export GSETTINGS_SCHEMA_DIR="$SCHEMA_DIR"

exec gdb -q -batch \
  -ex "source $GDB_INIT" \
  -ex run \
  -ex 'thread apply all bt' \
  -ex 'info registers' \
  -ex quit \
  --args "$FW_BIN" "$@"
