/* fw-sandbox.h — Linux Landlock LSM hardening
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Drop filesystem-execute permission for the rest of this process's
 * lifetime, plus the create-special-file-type rights that a viewer
 * has no business needing (sym/sock/fifo/block/char/dir mknod).
 * Read and write_file remain allowed — print, save-attachments,
 * state.json persistence all continue to work.
 *
 * The point: if a malicious document exploits MuPDF / DjVuLibre /
 * libarchive into RCE, the foothold can't escalate by execve'ing a
 * shell or by mknod'ing a backdoor — landlock drops the rights at
 * the kernel LSM layer below the libc.
 *
 * No-op on non-Linux platforms and on kernels that don't support
 * Landlock (logged via FW_TRACE_WINDOW). Once applied, the
 * restriction can't be relaxed for the rest of the process — call
 * once at startup, before launching subprocesses you actually need.
 *
 * Pattern from zathura/zathura/landlock.c (Zlib-licensed). */
void fw_sandbox_drop_execute (void);

G_END_DECLS
