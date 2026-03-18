#ifndef HEADER_fd_src_flamenco_runtime_tests_fd_multiblock_harness_h
#define HEADER_fd_src_flamenco_runtime_tests_fd_multiblock_harness_h

#include "fd_solfuzz.h"
#include "generated/block.pb.h"
#include "fd_multiblock_pb.h"

FD_PROTOTYPES_BEGIN

ulong
fd_solfuzz_pb_multiblock_run( fd_solfuzz_runner_t * runner,
                              void const *          input_,
                              void **               output_,
                              void *                output_buf,
                              ulong                 output_bufsz );

int
fd_solfuzz_pb_multiblock_prefix_begin( fd_solfuzz_runner_t *                runner,
                                       fd_exec_test_block_context_t const * start );

ulong
fd_solfuzz_pb_multiblock_prefix_append( fd_solfuzz_runner_t *                     runner,
                                        org_solana_sealevel_v1_block_step_t const * step,
                                        void **                                    output_,
                                        void *                                     output_buf,
                                        ulong                                      output_bufsz );

void
fd_solfuzz_pb_multiblock_prefix_fini( fd_solfuzz_runner_t * runner );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_runtime_tests_fd_multiblock_harness_h */
