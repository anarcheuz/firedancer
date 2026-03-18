This regression covers a multiblock duplicate-message case where:

- block 1 executes a `SystemProgram::CreateAccount`
- block 2 replays the same transaction message with perturbed signatures

Agave rejects the second block as already processed. The FD multiblock
solfuzz harness previously accepted it because the status cache was not
wired into multiblock execution.

Regression artifact:

- `duplicate_status_cache.bin`

Recommended validation path:

```bash
CORE_BPF_BUILTINS_DIR=/path/to/builtins \
FD_LIB_PATH=/home/anthony/firedancer/build/linux/clang/x86_64/lib/libfd_exec_sol_compat.so \
AG_LIB_PATH=/home/anthony/firedancer/solfuzz-agave/target/release/libsolfuzz_agave.so \
/home/anthony/firedancer/firedancer-fuzz/fire-block/target/release/fire-block \
  --mode reproduce
```

with `duplicate_status_cache.bin` copied into the working directory's `input/`
subdirectory. The fix is correct when the old mismatch does not reproduce.
