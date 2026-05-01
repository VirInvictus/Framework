# framework.gdb — pre-loaded breakpoints for crash investigation
#
# Sourced by tests/scripts/debug.sh. Targets the failure modes the
# repo has historically tripped over:
#
#   fz_throw          — MuPDF's exception entry point. Catches every
#                       fz_try/fz_catch raise, including the ones that
#                       fz_catch silently logs and swallows. Use to
#                       see the throw path before the catch block
#                       hides it.
#   cache_entry_free  — runs on every page eviction; useful when
#                       tracking down "why was this entry freed".
#   submit_next_jobs  — every render-worker dispatch. Lets you watch
#                       priority decisions live.
#
# Breakpoints are commented to print + continue rather than stop, so
# the program runs through and the operator still gets a crash bt at
# the end. Comment out the `commands` blocks if you want to step.

set print thread-events off
set print pretty on
set pagination off

# --- MuPDF exception breakpoint -------------------------------------
break fz_throw
commands
  silent
  printf "[fz_throw] hit\n"
  continue
end

# --- Cache eviction --------------------------------------------------
break cache_entry_free
commands
  silent
  printf "[cache_entry_free] entry=%p\n", entry
  continue
end

# --- Render dispatch -------------------------------------------------
break submit_next_jobs
commands
  silent
  printf "[submit_next_jobs] cache=%p priority_len=%d\n", self, self->priority_len
  continue
end

# Show all-thread bt on any signal that isn't SIGINT
handle SIGSEGV stop print
handle SIGABRT stop print
