#!/usr/bin/env bash
# tests/scripts/trace-replay.sh — render an FW_DEBUG=1 log to SVG
#
# Parses Framework's FW_DEBUG=1 trace lines (format:
# `[timestamp] [domain] message`) and emits an SVG timeline showing
# cache state transitions, render-worker dispatch, zoom changes, and
# byte-cap eviction events. Useful for "why did it freeze for 200ms"
# investigations without scrolling through hundreds of trace lines.
#
# Usage:
#   FW_DEBUG=1 GSETTINGS_SCHEMA_DIR=builddir/data \
#     ./builddir/src/framework <doc> 2> trace.log
#   tests/scripts/trace-replay.sh trace.log > trace.svg
#   xdg-open trace.svg
#
# Or stream:
#   FW_DEBUG=1 ./builddir/src/framework <doc> 2>&1 | \
#     tests/scripts/trace-replay.sh > trace.svg
#
# Tracks (from top):
#   1. Cache state (green/yellow/red bands for STATIC/CRUISING/SCRUBBING)
#   2. Zoom level (vertical orange line at every fw_cache_start)
#   3. Worker dispatch (small green tick at every worker start)
#   4. Worker completion (small blue tick at every worker done)
#   5. Byte-cap eviction (red triangle at every eviction event)
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

INPUT="${1:-/dev/stdin}"

awk '
  BEGIN {
    width  = 1400
    height = 500
    margin_left = 90
    margin_right = 20
    margin_top = 60
    track_h = 60
    plot_w = width - margin_left - margin_right
  }
  /^\[/ {
    # `[12345.6789] [domain] message`
    ts = $1
    sub(/^\[/, "", ts); sub(/\]$/, "", ts)
    if (start_time == 0) start_time = ts + 0
    rel = ts - start_time
    if (rel > last_rel) last_rel = rel

    if (match($0, /velocity [^ ]+ . state ([A-Z]+)/, m)) {
      n_state++
      state_t[n_state] = rel
      state_v[n_state] = m[1]
    } else if (match($0, /^\[[0-9.]+\] \[cache \] start: zoom=([0-9.]+)/, m)) {
      n_zoom++
      zoom_t[n_zoom] = rel
      zoom_v[n_zoom] = m[1] + 0
    } else if (match($0, /worker start: page=([0-9]+)/, m)) {
      n_ws++
      ws_t[n_ws] = rel
      ws_p[n_ws] = m[1] + 0
    } else if (match($0, /worker done: page=([0-9]+)/, m)) {
      n_wd++
      wd_t[n_wd] = rel
      wd_p[n_wd] = m[1] + 0
    } else if (/byte-cap evict/) {
      n_ev++
      ev_t[n_ev] = rel
    }
  }
  END {
    if (last_rel <= 0) last_rel = 0.01
    scale = plot_w / last_rel

    # Header / canvas
    printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" font-family=\"monospace\" font-size=\"11\">\n", width, height
    printf "<rect width=\"%d\" height=\"%d\" fill=\"#1e1e2e\"/>\n", width, height
    printf "<text x=\"%d\" y=\"24\" fill=\"#cdd6f4\" font-size=\"14\">Framework trace replay — %.3fs total, %d events</text>\n", margin_left, last_rel, n_state + n_ws + n_wd + n_ev + n_zoom

    # Time-axis ticks every 0.5s, labels every 1s
    for (t = 0; t <= last_rel; t += 0.5) {
      x = margin_left + t * scale
      printf "<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#45475a\" stroke-width=\"1\"/>\n", x, margin_top - 4, x, height - 20
      if (int(t * 10) % 10 == 0) {
        printf "<text x=\"%.1f\" y=\"%d\" fill=\"#bac2de\" text-anchor=\"middle\">%.0fs</text>\n", x, height - 6, t
      }
    }

    # Track 1: cache state bands
    track_y = margin_top
    printf "<text x=\"6\" y=\"%d\" fill=\"#cdd6f4\">state</text>\n", track_y + 14
    for (i = 1; i <= n_state; i++) {
      x0 = margin_left + state_t[i] * scale
      x1 = (i < n_state) ? margin_left + state_t[i+1] * scale : margin_left + plot_w
      color = "#a6e3a1"  # STATIC = green
      if (state_v[i] == "CRUISING")  color = "#f9e2af"  # yellow
      if (state_v[i] == "SCRUBBING") color = "#f38ba8"  # red
      printf "<rect x=\"%.1f\" y=\"%d\" width=\"%.1f\" height=\"24\" fill=\"%s\" opacity=\"0.55\"/>\n", x0, track_y, x1 - x0, color
      printf "<text x=\"%.1f\" y=\"%d\" fill=\"#11111b\">%s</text>\n", x0 + 4, track_y + 16, state_v[i]
    }
    if (n_state == 0) {
      printf "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"24\" fill=\"#45475a\" opacity=\"0.55\"/>\n", margin_left, track_y, plot_w
      printf "<text x=\"%d\" y=\"%d\" fill=\"#cdd6f4\">(no state transitions)</text>\n", margin_left + 4, track_y + 16
    }

    # Track 2: zoom changes (vertical lines)
    track_y = margin_top + track_h
    printf "<text x=\"6\" y=\"%d\" fill=\"#cdd6f4\">zoom</text>\n", track_y + 14
    for (i = 1; i <= n_zoom; i++) {
      x = margin_left + zoom_t[i] * scale
      printf "<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#fab387\" stroke-width=\"2\"/>\n", x, track_y, x, track_y + 30
      printf "<text x=\"%.1f\" y=\"%d\" fill=\"#fab387\" text-anchor=\"middle\">%.2f</text>\n", x, track_y + 26, zoom_v[i]
    }

    # Track 3: worker start (green ticks)
    track_y = margin_top + 2*track_h
    printf "<text x=\"6\" y=\"%d\" fill=\"#cdd6f4\">w-start</text>\n", track_y + 14
    for (i = 1; i <= n_ws; i++) {
      x = margin_left + ws_t[i] * scale
      printf "<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#a6e3a1\" stroke-width=\"1\" opacity=\"0.85\"/>\n", x, track_y + 4, x, track_y + 28
    }

    # Track 4: worker done (blue ticks)
    track_y = margin_top + 3*track_h
    printf "<text x=\"6\" y=\"%d\" fill=\"#cdd6f4\">w-done</text>\n", track_y + 14
    for (i = 1; i <= n_wd; i++) {
      x = margin_left + wd_t[i] * scale
      printf "<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#89b4fa\" stroke-width=\"1\" opacity=\"0.85\"/>\n", x, track_y + 4, x, track_y + 28
    }

    # Track 5: evictions (red triangles)
    track_y = margin_top + 4*track_h
    printf "<text x=\"6\" y=\"%d\" fill=\"#cdd6f4\">evict</text>\n", track_y + 14
    for (i = 1; i <= n_ev; i++) {
      x = margin_left + ev_t[i] * scale
      printf "<polygon points=\"%.1f,%d %.1f,%d %.1f,%d\" fill=\"#f38ba8\"/>\n", x - 6, track_y + 28, x + 6, track_y + 28, x, track_y + 12
    }

    # Summary in bottom-right
    printf "<text x=\"%d\" y=\"%d\" fill=\"#bac2de\" text-anchor=\"end\">workers: %d started / %d done   evictions: %d   zoom changes: %d</text>\n", width - 8, 22, n_ws, n_wd, n_ev, n_zoom

    print "</svg>"
  }
' "$INPUT"
