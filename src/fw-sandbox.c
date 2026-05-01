/* fw-sandbox.c — Linux Landlock LSM hardening
 *
 * Pattern borrowed from `.zathura/zathura/landlock.c` (Zlib license,
 * compatible). The Framework variant only drops execute + special-
 * file-type creation, so write-to-file (state.json, attachment
 * extraction, print-to-spool) keeps working. Zathura's full
 * `landlock_restrict_write` (path-beneath-only writes) is a stronger
 * lockdown but breaks save-attachments — left as a Phase 15 item if
 * the threat model justifies the loss.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "fw-sandbox.h"
#include "fw-debug.h"

#ifdef __linux__

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

/* Distros without `<linux/landlock.h>` (or with a too-old header)
 * still let us call the syscalls directly via `__NR_landlock_*`,
 * but we need the constants. We do the safe thing and skip the
 * sandbox unless the header is present — landlock is always a
 * defense-in-depth bonus, never a correctness requirement. */
#if __has_include(<linux/landlock.h>)
# include <linux/landlock.h>
# define FW_HAVE_LANDLOCK 1
#else
# define FW_HAVE_LANDLOCK 0
#endif

#if FW_HAVE_LANDLOCK

#ifndef __NR_landlock_create_ruleset
# define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_restrict_self
# define __NR_landlock_restrict_self  446
#endif

static int
fw_landlock_create_ruleset (const struct landlock_ruleset_attr *attr,
                            size_t size, __u32 flags)
{
  return (int) syscall (__NR_landlock_create_ruleset, attr, size, flags);
}

static int
fw_landlock_restrict_self (int ruleset_fd, __u32 flags)
{
  return (int) syscall (__NR_landlock_restrict_self, ruleset_fd, flags);
}

void
fw_sandbox_drop_execute (void)
{
  /* Probe the kernel for Landlock ABI support. -1 here means kernel
   * is too old, was built without LSM=landlock, or landlock wasn't
   * enabled at boot. All graceful — we just skip. */
  int abi = fw_landlock_create_ruleset (NULL, 0,
                                         LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 0) {
    FW_TRACE_WINDOW ("landlock unavailable (errno=%d) — sandbox skipped",
                     errno);
    return;
  }

  /* Drop EXECUTE everywhere on the filesystem, plus the make-* rules
   * a viewer never needs. WRITE_FILE stays allowed so save-attachment,
   * print spool writes, and state.json persistence continue to work. */
  __u64 fs_access =
      LANDLOCK_ACCESS_FS_EXECUTE
    | LANDLOCK_ACCESS_FS_MAKE_CHAR
    | LANDLOCK_ACCESS_FS_MAKE_BLOCK
    | LANDLOCK_ACCESS_FS_MAKE_SOCK
    | LANDLOCK_ACCESS_FS_MAKE_FIFO
    | LANDLOCK_ACCESS_FS_MAKE_SYM;

  const struct landlock_ruleset_attr ruleset_attr = {
    .handled_access_fs = fs_access,
  };

  int ruleset_fd = fw_landlock_create_ruleset (&ruleset_attr,
                                                sizeof (ruleset_attr), 0);
  if (ruleset_fd < 0) {
    FW_TRACE_WINDOW ("landlock_create_ruleset failed (errno=%d)", errno);
    return;
  }

  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    FW_TRACE_WINDOW ("PR_SET_NO_NEW_PRIVS failed (errno=%d)", errno);
    close (ruleset_fd);
    return;
  }

  if (fw_landlock_restrict_self (ruleset_fd, 0) != 0) {
    FW_TRACE_WINDOW ("landlock_restrict_self failed (errno=%d)", errno);
  } else {
    FW_TRACE_WINDOW ("landlock applied: dropped EXECUTE + MAKE_* (abi=%d)", abi);
  }
  close (ruleset_fd);
}

#else  /* FW_HAVE_LANDLOCK */

void
fw_sandbox_drop_execute (void)
{
  FW_TRACE_WINDOW ("landlock unavailable (header missing at build time)");
}

#endif

#else  /* __linux__ */

void
fw_sandbox_drop_execute (void)
{
  /* Non-Linux: no Landlock equivalent. Other OSes have their own
   * sandboxing primitives (macOS sandbox_init, OpenBSD pledge) that
   * could be wired here, but Framework is a Linux-first GNOME app. */
}

#endif
