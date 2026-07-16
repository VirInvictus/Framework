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
#include "fw-config.h"

#ifdef __linux__

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

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
#ifndef __NR_landlock_add_rule
# define __NR_landlock_add_rule       445
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
fw_landlock_add_rule (int ruleset_fd, enum landlock_rule_type type,
                      const void *attr, __u32 flags)
{
  return (int) syscall (__NR_landlock_add_rule, ruleset_fd, type, attr, flags);
}

static int
fw_landlock_restrict_self (int ruleset_fd, __u32 flags)
{
  return (int) syscall (__NR_landlock_restrict_self, ruleset_fd, flags);
}

/* Allow EXECUTE beneath `dir`.  Returns FALSE when the dir doesn't
 * exist or the rule can't be added — the caller decides whether that
 * failure is fatal to the EXECUTE plan. */
static gboolean
allow_execute_beneath (int ruleset_fd, const char *dir)
{
  int fd = open (dir, O_PATH | O_CLOEXEC | O_DIRECTORY);
  if (fd < 0)
    return FALSE;
  struct landlock_path_beneath_attr pb = {
    .allowed_access = LANDLOCK_ACCESS_FS_EXECUTE,
    .parent_fd      = fd,
  };
  int r = fw_landlock_add_rule (ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
  close (fd);
  if (r != 0) {
    FW_TRACE_WINDOW ("landlock: add_rule EXECUTE %s failed (errno=%d)",
                     dir, errno);
    return FALSE;
  }
  FW_TRACE_WINDOW ("landlock: EXECUTE allowed beneath %s", dir);
  return TRUE;
}

/* Locate the WebKitGTK helper-process directory (WebKitWebProcess,
 * WebKitNetworkProcess, WebKitGPUProcess).  WebKit fork-execs these
 * lazily at first load, so the EXECUTE drop must allow them or the
 * WebView bricks with EACCES.  Baked candidates come from the build's
 * pkg-config (prefix/libexec and libdir layouts); hardcoded fallbacks
 * cover the common distro layouts when the binary runs against a
 * different filesystem than it was built on. */
static char *
find_webkit_exec_dir (void)
{
  static const char *const fallbacks[] = {
    "/usr/libexec/webkitgtk-6.0",                 /* Fedora, Flatpak runtime */
    "/usr/lib64/webkitgtk-6.0",                   /* openSUSE               */
    "/usr/lib/webkitgtk-6.0",                     /* Arch                   */
    "/usr/lib/x86_64-linux-gnu/webkitgtk-6.0",    /* Debian/Ubuntu          */
    "/usr/lib/aarch64-linux-gnu/webkitgtk-6.0",
  };

  g_auto (GStrv) baked = g_strsplit (FW_WEBKIT_EXEC_DIRS, ":", -1);
  for (char **d = baked; d && *d; d++)
    if (**d && g_file_test (*d, G_FILE_TEST_IS_DIR))
      return g_strdup (*d);
  for (gsize i = 0; i < G_N_ELEMENTS (fallbacks); i++)
    if (g_file_test (fallbacks[i], G_FILE_TEST_IS_DIR))
      return g_strdup (fallbacks[i]);
  return NULL;
}

/* Build a ruleset handling `fs_access`, adding EXECUTE allow rules for
 * `exec_dirs` (all required) when EXECUTE is in the handled set.
 * Returns the ruleset fd, or -1 when any required piece fails —
 * with EXECUTE handled but a rule missing, every execve in the process
 * (and its children) would fail, which is worse than no ruleset. */
static int
build_ruleset (__u64 fs_access, const char *const *exec_dirs, gsize n_dirs)
{
  const struct landlock_ruleset_attr ruleset_attr = {
    .handled_access_fs = fs_access,
  };
  int fd = fw_landlock_create_ruleset (&ruleset_attr, sizeof (ruleset_attr), 0);
  if (fd < 0) {
    FW_TRACE_WINDOW ("landlock_create_ruleset failed (errno=%d)", errno);
    return -1;
  }
  if (fs_access & LANDLOCK_ACCESS_FS_EXECUTE) {
    for (gsize i = 0; i < n_dirs; i++) {
      if (!allow_execute_beneath (fd, exec_dirs[i])) {
        close (fd);
        return -1;
      }
    }
  }
  return fd;
}

void
fw_sandbox_drop_execute (void)
{
  /* Debugging escape hatch: if the EXECUTE allowlist ever breaks a
   * feature on an exotic layout, FW_NO_LANDLOCK=1 disables the sandbox
   * without a rebuild. */
  if (g_getenv ("FW_NO_LANDLOCK")) {
    FW_TRACE_WINDOW ("landlock disabled via FW_NO_LANDLOCK");
    return;
  }

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

  /* Drop EXECUTE and the make-* filesystem rights a viewer never needs:
   * a malicious document exploiting a parser (MuPDF, DjVuLibre,
   * libarchive, WebKit) can't reach a shell or plant device nodes.
   * WRITE_FILE stays allowed so save-attachment, print spool writes,
   * and state.json persistence continue to work.
   *
   * EXECUTE needs a narrow allowlist (v0.78, restoring the v0.38 drop
   * that v0.68 removed): WebKitGTK fork-execs its helper processes
   * lazily, and the kernel also checks EXECUTE on the ELF interpreter
   * (ld.so under /usr/lib*) when spawning them.  Shells in /usr/bin and
   * payloads under /tmp or $HOME stay denied.  If the helper dir can't
   * be found, fall back to the MAKE_*-only ruleset rather than brick
   * the WebView. */
  __u64 make_access =
      LANDLOCK_ACCESS_FS_MAKE_CHAR
    | LANDLOCK_ACCESS_FS_MAKE_BLOCK
    | LANDLOCK_ACCESS_FS_MAKE_SOCK
    | LANDLOCK_ACCESS_FS_MAKE_FIFO
    | LANDLOCK_ACCESS_FS_MAKE_SYM;

  int ruleset_fd = -1;
  gboolean exec_dropped = FALSE;
  g_autofree char *webkit_dir = find_webkit_exec_dir ();
  if (webkit_dir) {
    const char *exec_dirs[] = { webkit_dir, "/usr/lib64", "/usr/lib" };
    /* Loader dirs are best-effort per-arch (a distro has one or both);
     * require the WebKit dir plus at least one loader dir. */
    const char *required[3];
    gsize n = 0;
    required[n++] = webkit_dir;
    for (gsize i = 1; i < G_N_ELEMENTS (exec_dirs); i++)
      if (g_file_test (exec_dirs[i], G_FILE_TEST_IS_DIR))
        required[n++] = exec_dirs[i];
    if (n >= 2) {
      ruleset_fd = build_ruleset (make_access | LANDLOCK_ACCESS_FS_EXECUTE,
                                  required, n);
      exec_dropped = ruleset_fd >= 0;
    }
  } else {
    FW_TRACE_WINDOW ("landlock: no WebKit helper dir found — "
                     "EXECUTE drop skipped");
  }

  if (ruleset_fd < 0)
    ruleset_fd = build_ruleset (make_access, NULL, 0);
  if (ruleset_fd < 0)
    return;

  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    FW_TRACE_WINDOW ("PR_SET_NO_NEW_PRIVS failed (errno=%d)", errno);
    close (ruleset_fd);
    return;
  }

  if (fw_landlock_restrict_self (ruleset_fd, 0) != 0) {
    FW_TRACE_WINDOW ("landlock_restrict_self failed (errno=%d)", errno);
  } else {
    FW_TRACE_WINDOW ("landlock applied: dropped %s (abi=%d)",
                     exec_dropped ? "EXECUTE + MAKE_*" : "MAKE_*", abi);
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
