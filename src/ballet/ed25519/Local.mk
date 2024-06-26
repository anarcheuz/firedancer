$(call add-hdrs,fd_ed25519.h fd_x25519.h fd_f25519.h fd_curve25519.h fd_curve25519_scalar.h)
$(call add-objs,fd_f25519 fd_curve25519 fd_curve25519_scalar fd_ed25519_user fd_x25519,fd_ballet)
$(call add-objs,fd_ristretto255,fd_ballet)
$(call make-unit-test,test_ed25519,test_ed25519,fd_ballet fd_util)
$(call make-unit-test,test_ed25519_signature_malleability,test_ed25519_signature_malleability,fd_ballet fd_util)
$(call make-unit-test,test_x25519,test_x25519,fd_ballet fd_util)
$(call make-unit-test,test_ristretto255,test_ristretto255,fd_ballet fd_util)
#$(call make-unit-test,fd_curve25519_tables,fd_curve25519_tables,fd_ballet fd_util)
$(call run-unit-test,test_ed25519)
$(call run-unit-test,test_ed25519_signature_malleability)
$(call run-unit-test,test_x25519)
$(call run-unit-test,test_ristretto255)

ifdef FD_HAS_HOSTED
$(call make-fuzz-test,fuzz_ed25519_verify,fuzz_ed25519_verify,fd_ballet fd_util)
$(call make-fuzz-test,fuzz_ed25519_sigverify,fuzz_ed25519_sigverify,fd_ballet fd_util)
$(call make-fuzz-test,fuzz_ed25519_sigverify_diff,fuzz_ed25519_sigverify_diff,fd_ballet fd_util)
endif
