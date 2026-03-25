# SanCov-only instrumentation for coverage-guided fuzzing.
# Unlike with-fuzz.mk, this does NOT include -fsanitize=fuzzer-no-link
# (which adds comparison tracing, stack-depth tracking, etc. and
# requires linking the full sanitizer runtime).
#
# This is intended for building libfd_exec_sol_compat.so that will be
# dlopen'd by the fire-block fuzzer, which provides its own
# __sanitizer_cov_8bit_counters_init callback.

CPPFLAGS+=-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp
LDFLAGS+=-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp
