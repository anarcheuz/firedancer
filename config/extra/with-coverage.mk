# LLVM source-level coverage instrumentation for llvm-cov reports.
# Build with: make lib EXTRAS="coverage" MACHINE=linux_clang_x86_64

CPPFLAGS+=-fprofile-instr-generate -fcoverage-mapping
LDFLAGS+=-fprofile-instr-generate -fcoverage-mapping
