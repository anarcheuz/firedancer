$(call add-hdrs,fd_base58.h)
$(call add-objs,fd_base58,fd_ballet)
$(call make-unit-test,test_base58,test_base58,fd_ballet fd_util)
ifdef FD_HAS_HOSTED
$(call make-fuzz-test,fuzz_base58_roundtrip,fuzz_base58_roundtrip,fd_ballet fd_util)
$(call make-fuzz-test,fuzz_base58_garbage,fuzz_base58_garbage,fd_ballet fd_util)
endif
