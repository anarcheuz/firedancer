#include "fd_solfuzz_private.h"
#include "fd_multiblock_harness.h"
#include "../fd_cost_tracker.h"
#include "fd_txn_harness.h"
#include "../fd_runtime.h"
#include "../fd_system_ids.h"
#include "../fd_runtime_stack.h"
#include "../program/fd_stake_program.h"
#include "../program/vote/fd_vote_state_versioned.h"
#include "../sysvar/fd_sysvar_epoch_schedule.h"
#include "../sysvar/fd_sysvar_rent.h"
#include "../sysvar/fd_sysvar_recent_hashes.h"
#include "../../accdb/fd_accdb_admin_v1.h"
#include "../../accdb/fd_accdb_impl_v1.h"
#include "../../accdb/fd_accdb_sync.h"
#include "../../log_collector/fd_log_collector.h"
#include "../../rewards/fd_rewards.h"
#include "../../types/fd_types.h"
#include "generated/block.pb.h"
#include "fd_multiblock_pb.h"
#include "../../capture/fd_capture_ctx.h"
#include "../../capture/fd_solcap_writer.h"

#include <stdlib.h>

/* Provided by fd_block_harness.c */
ulong
fd_solfuzz_block_hash_epoch_leaders( fd_solfuzz_runner_t *      runner,
                                     fd_epoch_leaders_t const * leaders,
                                     ulong                      seed,
                                     uchar                      out[16] );

/* Fixed leader schedule hash seed (consistent with fd_block_harness / Agave) */
#define LEADER_SCHEDULE_HASH_SEED 0xDEADFACEUL
#define FD_SOLFUZZ_NULL_TXNCACHE_FORK ((fd_txncache_fork_id_t){ .val = USHORT_MAX })

static int fd_solfuzz_multiblock_reused_root_fork = 0;

static void
fd_solfuzz_multiblock_evict_stake_delegations_fork( fd_solfuzz_runner_t * runner,
                                                    fd_bank_data_t *       bank ) {
  if( FD_UNLIKELY( !bank || bank->stake_delegations_fork_id==USHORT_MAX ) ) return;
  fd_stake_delegations_delta_t * sd_delta = fd_banks_get_stake_delegations_delta( runner->banks->data );
  fd_stake_delegations_delta_evict_fork( sd_delta, bank->stake_delegations_fork_id );
  bank->stake_delegations_fork_id = USHORT_MAX;
}

static void
fd_solfuzz_multiblock_register_vote_account( fd_top_votes_t *          top_votes,
                                             fd_accdb_user_t *         accdb,
                                             fd_funk_txn_xid_t const * xid,
                                             fd_pubkey_t *             pubkey ) {
  fd_accdb_ro_t ro[1];
  if( FD_UNLIKELY( !fd_accdb_open_ro( accdb, ro, xid, pubkey ) ) ) return;

  if( !fd_pubkey_eq( fd_accdb_ref_owner( ro ), &fd_solana_vote_program_id ) ||
      fd_accdb_ref_lamports( ro )==0UL ||
      !fd_vsv_is_correct_size_and_initialized( ro->meta ) ) {
    fd_accdb_close_ro( accdb, ro );
    return;
  }

  fd_vote_block_timestamp_t vote_block_timestamp = fd_vsv_get_vote_block_timestamp( fd_account_data( ro->meta ), ro->meta->dlen );
  fd_top_votes_update( top_votes, pubkey, vote_block_timestamp.slot, vote_block_timestamp.timestamp );

  fd_accdb_close_ro( accdb, ro );
}

static void
fd_solfuzz_multiblock_update_prev_epoch_stakes( fd_top_votes_t *                   top_votes,
                                                fd_vote_stakes_t *                 vote_stakes,
                                                fd_exec_test_prev_vote_account_t * vote_accounts,
                                                pb_size_t                          vote_accounts_cnt,
                                                uchar                              is_t_1 ) {
  if( FD_UNLIKELY( !vote_accounts ) ) return;

  for( uint i=0U; i<vote_accounts_cnt; i++ ) {
    fd_pubkey_t vote_pubkey = FD_LOAD( fd_pubkey_t, &vote_accounts[i].address );
    fd_pubkey_t node_pubkey = FD_LOAD( fd_pubkey_t, &vote_accounts[i].node_pubkey );
    ulong       stake       = vote_accounts[i].stake;

    if( is_t_1 ) {
      fd_vote_stakes_root_insert_key( vote_stakes, &vote_pubkey, &node_pubkey, stake, 0 );
    } else {
      fd_vote_stakes_root_update_meta( vote_stakes, &vote_pubkey, &node_pubkey, stake, 0 );
      fd_top_votes_insert( top_votes, &vote_pubkey, &node_pubkey, stake, 0, 0 );
    }
  }
}

static void
fd_solfuzz_multiblock_register_stake_delegation( fd_accdb_user_t *         accdb,
                                                 fd_funk_txn_xid_t const * xid,
                                                 fd_stake_delegations_t *  stake_delegations,
                                                 fd_pubkey_t *             pubkey ) {
  fd_accdb_ro_t ro[1];
  if( FD_UNLIKELY( !fd_accdb_open_ro( accdb, ro, xid, pubkey ) ) ) return;

  fd_stake_state_v2_t stake_state;
  if( !fd_pubkey_eq( fd_accdb_ref_owner( ro ), &fd_solana_stake_program_id ) ||
      fd_accdb_ref_lamports( ro )==0UL ||
      0!=fd_stake_get_state( ro->meta, &stake_state ) ||
      !fd_stake_state_v2_is_stake( &stake_state ) ||
      stake_state.inner.stake.stake.delegation.stake==0UL ) {
    fd_accdb_close_ro( accdb, ro );
    return;
  }

  fd_stake_delegations_update(
      stake_delegations,
      pubkey,
      &stake_state.inner.stake.stake.delegation.voter_pubkey,
      stake_state.inner.stake.stake.delegation.stake,
      stake_state.inner.stake.stake.delegation.activation_epoch,
      stake_state.inner.stake.stake.delegation.deactivation_epoch,
      stake_state.inner.stake.stake.credits_observed,
      stake_state.inner.stake.stake.delegation.warmup_cooldown_rate );
  fd_accdb_close_ro( accdb, ro );
}

static void
fd_solfuzz_multiblock_cleanup( fd_solfuzz_runner_t * runner ) {
  fd_solfuzz_multiblock_evict_stake_delegations_fork( runner, runner->bank->data );

  fd_accdb_v1_clear( runner->accdb_admin );
  fd_progcache_clear( runner->progcache_admin );
  fd_txncache_reset( runner->status_cache );
  runner->bank->data->txncache_fork_id = FD_SOLFUZZ_NULL_TXNCACHE_FORK;
  fd_solfuzz_multiblock_reused_root_fork = 0;

  fd_bank_data_t * bank_pool = fd_banks_get_bank_pool( runner->banks->data );
  ulong root_idx = runner->banks->data->root_idx;
  ulong bank_idx = runner->bank->data->idx;
  if( root_idx!=fd_banks_pool_idx_null( bank_pool ) && bank_idx!=root_idx ) {
    fd_banks_mark_bank_dead( runner->banks, bank_idx );
  }

  for( ;; ) {
    fd_banks_prune_cancel_info_t cancel_info = {0};
    if( !fd_banks_prune_one_dead_bank( runner->banks, &cancel_info ) ) break;
  }

  root_idx = runner->banks->data->root_idx;
  if( root_idx!=fd_banks_pool_idx_null( bank_pool ) ) {
    runner->bank->data = fd_banks_pool_ele( bank_pool, root_idx );
    runner->bank->locks = runner->banks->locks;
    fd_solfuzz_multiblock_evict_stake_delegations_fork( runner, runner->bank->data );
  }

  fd_alloc_compact( fd_accdb_user_v1_funk( runner->accdb )->alloc );
  fd_alloc_compact( runner->progcache_admin->funk->alloc );
}

static void
fd_solfuzz_multiblock_status_cache_seed_history( fd_solfuzz_runner_t *                  runner,
                                                 fd_exec_test_block_bank_t const *      block_bank,
                                                 int                                     needs_current_fork ) {
  fd_txncache_reset( runner->status_cache );

  fd_txncache_fork_id_t parent_fork = FD_SOLFUZZ_NULL_TXNCACHE_FORK;
  for( ulong i=0UL; i<block_bank->blockhash_queue_count; i++ ) {
    fd_txncache_fork_id_t fork_id = fd_txncache_attach_child( runner->status_cache, parent_fork );
    fd_txncache_finalize_fork( runner->status_cache, fork_id, 0UL, block_bank->blockhash_queue[i].blockhash );
    parent_fork = fork_id;
  }

  if( FD_LIKELY( needs_current_fork ) ) {
    runner->bank->data->txncache_fork_id = fd_txncache_attach_child( runner->status_cache, parent_fork );
    fd_solfuzz_multiblock_reused_root_fork = 0;
  } else {
    /* Empty start blocks should reuse the newest rooted recent-blockhash
       fork directly instead of creating a synthetic child fork with no
       produced blockhash. */
    runner->bank->data->txncache_fork_id = parent_fork;
    fd_solfuzz_multiblock_reused_root_fork = 1;
  }
}

static void
fd_solfuzz_multiblock_status_cache_finalize_current( fd_solfuzz_runner_t * runner ) {
  fd_hash_t const * blockhash = fd_blockhashes_peek_last_hash( fd_bank_block_hash_queue_query( runner->bank ) );
  if( FD_UNLIKELY( !blockhash ) ) return;
  fd_txncache_finalize_fork( runner->status_cache, runner->bank->data->txncache_fork_id, 0UL, blockhash->hash );
}

typedef struct {
  int                          active;
  fd_exec_test_block_context_t start;
  fd_pubkey_t *                tracked_keys;
  ulong                        tracked_cnt;
  ulong                        tracked_cap;
} fd_solfuzz_multiblock_prefix_session_t;

static fd_solfuzz_multiblock_prefix_session_t fd_solfuzz_multiblock_prefix_session[1] = {{0}};

static void
fd_solfuzz_multiblock_prefix_session_reset( void ) {
  if( fd_solfuzz_multiblock_prefix_session->active ) {
    pb_release( &fd_exec_test_block_context_t_msg, &fd_solfuzz_multiblock_prefix_session->start );
  }
  free( fd_solfuzz_multiblock_prefix_session->tracked_keys );
  fd_memset( fd_solfuzz_multiblock_prefix_session, 0, sizeof(fd_solfuzz_multiblock_prefix_session_t) );
}

static int
fd_solfuzz_multiblock_prefix_track_key( fd_pubkey_t const * key ) {
  for( ulong i=0UL; i<fd_solfuzz_multiblock_prefix_session->tracked_cnt; i++ ) {
    if( fd_pubkey_eq( &fd_solfuzz_multiblock_prefix_session->tracked_keys[i], key ) ) return 1;
  }

  if( FD_UNLIKELY( fd_solfuzz_multiblock_prefix_session->tracked_cnt>=fd_solfuzz_multiblock_prefix_session->tracked_cap ) ) {
    ulong new_cap = fd_solfuzz_multiblock_prefix_session->tracked_cap ? fd_solfuzz_multiblock_prefix_session->tracked_cap * 2UL : 64UL;
    fd_pubkey_t * new_keys = (fd_pubkey_t *)realloc( fd_solfuzz_multiblock_prefix_session->tracked_keys,
                                                     new_cap * sizeof(fd_pubkey_t) );
    if( FD_UNLIKELY( !new_keys ) ) return 0;
    fd_solfuzz_multiblock_prefix_session->tracked_keys = new_keys;
    fd_solfuzz_multiblock_prefix_session->tracked_cap  = new_cap;
  }

  fd_solfuzz_multiblock_prefix_session->tracked_keys[ fd_solfuzz_multiblock_prefix_session->tracked_cnt++ ] = *key;
  return 1;
}

static int
fd_solfuzz_multiblock_prefix_track_accounts( fd_exec_test_acct_state_t const * acct_states,
                                             ulong                             acct_states_cnt ) {
  for( ulong i=0UL; i<acct_states_cnt; i++ ) {
    fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, acct_states[i].address );
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_key( &pubkey ) ) ) return 0;
  }
  return 1;
}

static int
fd_solfuzz_multiblock_prefix_track_txn_accounts( fd_exec_test_sanitized_transaction_t const * txns,
                                                 ulong                                        txn_cnt ) {
  for( ulong i=0UL; i<txn_cnt; i++ ) {
    if( FD_UNLIKELY( !txns[i].has_message ) ) continue;
    fd_exec_test_transaction_message_t const * message = &txns[i].message;

    for( ulong j=0UL; j<message->account_keys_count; j++ ) {
      pb_bytes_array_t const * key_bytes = message->account_keys[j];
      if( FD_UNLIKELY( !key_bytes || key_bytes->size!=sizeof(fd_pubkey_t) ) ) continue;
      fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, key_bytes->bytes );
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_key( &pubkey ) ) ) return 0;
    }

    for( ulong j=0UL; j<message->address_table_lookups_count; j++ ) {
      fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, message->address_table_lookups[j].account_key );
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_key( &pubkey ) ) ) return 0;
    }
  }
  return 1;
}

static fd_txn_p_t *
fd_solfuzz_multiblock_serialize_txns( fd_solfuzz_runner_t *                       runner,
                                      fd_exec_test_sanitized_transaction_t const * txns,
                                      ulong                                        txn_cnt ) {
  fd_txn_p_t * txn_ptrs = fd_spad_alloc( runner->spad, alignof(fd_txn_p_t), txn_cnt * sizeof(fd_txn_p_t) );
  for( ulong i=0UL; i<txn_cnt; i++ ) {
    fd_txn_p_t * txn    = &txn_ptrs[i];
    ulong        msg_sz = fd_solfuzz_pb_txn_serialize( txn->payload, &txns[i] );

    if( FD_UNLIKELY( msg_sz==ULONG_MAX ) ) return NULL;
    txn->payload_sz = msg_sz;

    if( FD_UNLIKELY( !fd_txn_parse( txn->payload, msg_sz, TXN( txn ), NULL ) ) ) return NULL;
  }
  return txn_ptrs;
}

static void
fd_solfuzz_multiblock_default_poh( fd_hash_t const * parent_last_blockhash,
                                   ulong             slot,
                                   fd_hash_t * poh ) {
  fd_memset( poh, 0, sizeof(fd_hash_t) );
  fd_memcpy( poh->hash, &slot, sizeof(ulong) );

  if( FD_LIKELY( parent_last_blockhash && 0==memcmp( poh, parent_last_blockhash, sizeof(fd_hash_t) ) ) ) {
    ulong derived = fd_hash( slot, parent_last_blockhash, sizeof(fd_hash_t) );
    fd_memset( poh, 0, sizeof(fd_hash_t) );
    fd_memcpy( poh->hash, &derived, sizeof(ulong) );
  }
}

static int
fd_solfuzz_multiblock_init_start( fd_solfuzz_runner_t *                runner,
                                  fd_exec_test_block_context_t const * test_ctx,
                                  fd_hash_t *                          poh ) {
  fd_accdb_user_t * accdb = runner->accdb;
  fd_bank_t *       bank  = runner->bank;
  fd_banks_t *      banks = runner->banks;
  fd_runtime_stack_t * runtime_stack = runner->runtime_stack;

  fd_banks_clear_bank( banks, bank, 2048UL );

  fd_funk_txn_xid_t xid[1] = {{ .ul={ 0UL, 0UL } }};
  fd_funk_txn_xid_t parent_xid; fd_funk_txn_xid_set_root( &parent_xid );
  fd_accdb_attach_child( runner->accdb_admin, &parent_xid, xid );
  fd_progcache_txn_attach_child( runner->progcache_admin, &parent_xid, xid );

  FD_TEST( test_ctx->has_bank );
  fd_exec_test_block_bank_t const * block_bank = &test_ctx->bank;

  ulong slot = block_bank->slot;
  fd_bank_slot_set( bank, slot );
  fd_solfuzz_pb_restore_blockhash_queue( bank, block_bank->blockhash_queue, block_bank->blockhash_queue_count );
  fd_bank_rbh_lamports_per_sig_set( bank, block_bank->rbh_lamports_per_signature );

  FD_TEST( block_bank->has_fee_rate_governor );
  fd_solfuzz_pb_restore_fee_rate_governor( bank, &block_bank->fee_rate_governor );

  ulong parent_slot = block_bank->parent_slot;
  fd_bank_parent_slot_set( bank, parent_slot );
  fd_bank_capitalization_set( bank, block_bank->capitalization );

  FD_TEST( block_bank->has_inflation );
  fd_inflation_t inflation = {
    .initial         = block_bank->inflation.initial,
    .terminal        = block_bank->inflation.terminal,
    .taper           = block_bank->inflation.taper,
    .foundation      = block_bank->inflation.foundation,
    .foundation_term = block_bank->inflation.foundation_term,
  };
  fd_bank_inflation_set( bank, inflation );
  fd_bank_block_height_set( bank, block_bank->block_height );
  fd_memcpy( poh, block_bank->poh, sizeof(fd_hash_t) );

  fd_hash_t * bank_hash = fd_bank_bank_hash_modify( bank );
  fd_memcpy( bank_hash, block_bank->parent_bank_hash, sizeof(fd_hash_t) );
  fd_bank_parent_signature_cnt_set( bank, block_bank->parent_signature_count );
  fd_solfuzz_multiblock_status_cache_seed_history( runner, block_bank, test_ctx->txns_count>0 );

  FD_TEST( block_bank->has_epoch_schedule );
  fd_solfuzz_pb_restore_epoch_schedule( bank, &block_bank->epoch_schedule );
  FD_TEST( block_bank->has_rent );
  fd_solfuzz_pb_restore_rent( bank, &block_bank->rent );

  FD_TEST( block_bank->has_features );
  fd_features_t * features_bm = fd_bank_features_modify( bank );
  FD_TEST( fd_solfuzz_pb_restore_features( features_bm, &block_bank->features ) );

  ulong total_epoch_stake = 0UL;
  for( uint i=0U; i<block_bank->vote_accounts_t_1_count; i++ ) {
    total_epoch_stake += block_bank->vote_accounts_t_1[i].stake;
  }
  fd_bank_total_epoch_stake_set( bank, total_epoch_stake );

  uint128 ns_per_slot = FD_LOAD(uint128, block_bank->ns_per_slot );
  fd_bank_ns_per_slot_set( bank, (fd_w_u128_t){ .ud = ns_per_slot } );
  fd_bank_ticks_per_slot_set( bank, 64UL );
  fd_bank_slots_per_year_set( bank, (double)SECONDS_PER_YEAR * 1e9 / (double)ns_per_slot );
  fd_bank_hashes_per_tick_set( bank, (slot+1UL)*64UL );

  fd_stake_delegations_t * stake_delegations = fd_banks_stake_delegations_root_query( banks );
  fd_stake_delegations_init( stake_delegations );

  fd_stake_delegations_delta_t * stake_delegations_delta = fd_banks_get_stake_delegations_delta( banks->data );
  bank->data->stake_delegations_fork_id = fd_stake_delegations_delta_new_fork( stake_delegations_delta );

  fd_vote_stakes_t * vote_stakes = fd_bank_vote_stakes_locking_modify( bank );
  bank->data->vote_stakes_fork_id = fd_vote_stakes_get_root_idx( vote_stakes );

  fd_top_votes_t * top_votes = fd_bank_top_votes_modify( bank );
  fd_top_votes_init( top_votes );

  FD_TEST( block_bank->vote_accounts_t_1_count<=FD_RUNTIME_EXPECTED_VOTE_ACCOUNTS );
  FD_TEST( block_bank->vote_accounts_t_2_count<=FD_RUNTIME_EXPECTED_VOTE_ACCOUNTS );

  fd_solfuzz_multiblock_update_prev_epoch_stakes(
      top_votes, vote_stakes,
      block_bank->vote_accounts_t_1, block_bank->vote_accounts_t_1_count, 1 );
  fd_solfuzz_multiblock_update_prev_epoch_stakes(
      top_votes, vote_stakes,
      block_bank->vote_accounts_t_2, block_bank->vote_accounts_t_2_count, 0 );

  for( ushort i=0; i<test_ctx->acct_states_count; i++ ) {
    fd_solfuzz_pb_load_account( runner->runtime, accdb, xid, &test_ctx->acct_states[i], i );

    fd_pubkey_t pubkey;
    fd_memcpy( &pubkey, test_ctx->acct_states[i].address, sizeof(fd_pubkey_t) );
    fd_solfuzz_multiblock_register_vote_account( top_votes, accdb, xid, &pubkey );
    fd_solfuzz_multiblock_register_stake_delegation( accdb, xid, stake_delegations, &pubkey );
  }

  fd_bank_epoch_set( bank, fd_slot_to_epoch( fd_bank_epoch_schedule_query( bank ), parent_slot, NULL ) );

  FD_TEST( fd_vote_rewards_map_join( fd_vote_rewards_map_new( runtime_stack->stakes.vote_map_mem, FD_RUNTIME_EXPECTED_VOTE_ACCOUNTS, 999 ) ) );
  fd_vote_rewards_map_t * vote_ele_map = fd_type_pun( runtime_stack->stakes.vote_map_mem );
  for( uint i=0U; i<block_bank->vote_accounts_t_1_count; i++ ) {
    fd_exec_test_prev_vote_account_t const * pva         = &block_bank->vote_accounts_t_1[i];
    fd_pubkey_t                              vote_pubkey = FD_LOAD( fd_pubkey_t, pva->address );

    fd_vote_rewards_t * vote_ele = &runtime_stack->stakes.vote_ele[i];
    fd_memcpy( vote_ele->pubkey.uc, &vote_pubkey, sizeof(fd_pubkey_t) );
    vote_ele->commission = (uchar)pva->commission;

    FD_TEST( pva->epoch_credits_count<=FD_EPOCH_CREDITS_MAX );
    vote_ele->epoch_credits.cnt = pva->epoch_credits_count;
    for( ulong j=0UL; j<pva->epoch_credits_count; j++ ) {
      vote_ele->epoch_credits.epoch[j]        = (ushort)pva->epoch_credits[j].epoch;
      vote_ele->epoch_credits.credits[j]      = pva->epoch_credits[j].credits;
      vote_ele->epoch_credits.prev_credits[j] = pva->epoch_credits[j].prev_credits;
    }

    fd_vote_rewards_map_idx_insert( vote_ele_map, i, runtime_stack->stakes.vote_ele );
  }

  fd_bank_vote_stakes_end_locking_modify( bank );
  fd_runtime_update_leaders( bank, runtime_stack );

  fd_funk_txn_xid_t fork_xid = { .ul = { slot, bank->data->idx } };
  fd_accdb_attach_child        ( runner->accdb_admin,     xid, &fork_xid );
  fd_progcache_txn_attach_child( runner->progcache_admin, xid, &fork_xid );
  xid[0] = fork_xid;

  fd_lthash_value_t * lthash = fd_bank_lthash_locking_modify( bank );
  fd_memcpy( lthash, block_bank->parent_lt_hash, sizeof(fd_lthash_value_t) );
  fd_bank_lthash_end_locking_modify( bank );

  fd_sysvar_cache_restore_fuzz( bank, accdb, xid );
  return 1;
}

static int
fd_solfuzz_multiblock_init_step( fd_solfuzz_runner_t *            runner,
                                 org_solana_sealevel_v1_block_step_t const * step,
                                 fd_hash_t *                      poh ) {
  fd_bank_t * parent_bank = runner->bank;
  ulong parent_slot       = fd_bank_slot_get( parent_bank );
  ulong parent_bank_idx   = parent_bank->data->idx;
  fd_txncache_fork_id_t parent_txncache_fork_id = parent_bank->data->txncache_fork_id;
  ulong parent_block_height = fd_bank_block_height_get( parent_bank );
  fd_hash_t parent_last_blockhash = {0};
  fd_hash_t const * parent_last_blockhash_p = fd_blockhashes_peek_last_hash( fd_bank_block_hash_queue_query( parent_bank ) );
  if( parent_last_blockhash_p ) parent_last_blockhash = *parent_last_blockhash_p;

  FD_TEST( parent_bank->data->flags & FD_BANK_FLAGS_FROZEN );

  ulong slot = step->slot ? step->slot : parent_slot + 1UL;

  ulong new_bank_idx = fd_banks_new_bank( runner->bank, runner->banks, parent_bank_idx, 0L )->data->idx;
  fd_bank_t * new_bank = fd_banks_clone_from_parent( runner->bank, runner->banks, new_bank_idx );
  if( FD_UNLIKELY( !new_bank ) ) return 0;
  /* fd_banks_clone_from_parent() reuses the local bank wrapper, so the
     parent fork id must be captured before the clone. */
  new_bank->data->txncache_fork_id = fd_txncache_attach_child( runner->status_cache, parent_txncache_fork_id );
  fd_solfuzz_multiblock_reused_root_fork = 0;

  fd_bank_slot_set( new_bank, slot );
  fd_bank_parent_slot_set( new_bank, parent_slot );
  fd_bank_block_height_set( new_bank, step->block_height
                                           ? step->block_height
                                           : parent_block_height + 1UL );

  fd_epoch_schedule_t const * epoch_schedule = fd_bank_epoch_schedule_query( new_bank );
  fd_bank_epoch_set( new_bank, fd_slot_to_epoch( epoch_schedule, slot, NULL ) );

  fd_funk_txn_xid_t xid        = { .ul = { slot, new_bank_idx } };
  fd_funk_txn_xid_t parent_xid = { .ul = { parent_slot, parent_bank_idx } };
  fd_accdb_attach_child( runner->accdb_admin, &parent_xid, &xid );
  fd_progcache_txn_attach_child( runner->progcache_admin, &parent_xid, &xid );

  if( step->has_features ) {
    fd_features_t * features_bm = fd_bank_features_modify( new_bank );
    FD_TEST( fd_solfuzz_pb_restore_features( features_bm, &step->features ) );
  }

  if( step->has_inflation ) {
    fd_inflation_t inflation = {
      .initial         = step->inflation.initial,
      .terminal        = step->inflation.terminal,
      .taper           = step->inflation.taper,
      .foundation      = step->inflation.foundation,
      .foundation_term = step->inflation.foundation_term,
    };
    fd_bank_inflation_set( new_bank, inflation );
  }

  if( step->has_fee_rate_governor ) {
    fd_solfuzz_pb_restore_fee_rate_governor( new_bank, &step->fee_rate_governor );
  }
  if( step->parent_signature_count ) fd_bank_parent_signature_cnt_set( new_bank, step->parent_signature_count );

  for( ushort i=0; i<step->acct_states_count; i++ ) {
    fd_solfuzz_pb_load_account( runner->runtime, runner->accdb, &xid, &step->acct_states[i], i );
  }

  fd_sysvar_cache_restore_fuzz( new_bank, runner->accdb, &xid );

  fd_solfuzz_multiblock_default_poh( parent_last_blockhash_p ? &parent_last_blockhash : NULL, slot, poh );

  return 1;
}

static ulong
fd_solfuzz_multiblock_target_slot( ulong                                      parent_slot,
                                   org_solana_sealevel_v1_block_step_t const * step ) {
  return step->slot>parent_slot ? step->slot : parent_slot + 1UL;
}

static int
fd_solfuzz_multiblock_exec_current( fd_solfuzz_runner_t * runner,
                                    fd_txn_p_t *          txn_ptrs,
                                    ulong                 txn_cnt,
                                    fd_hash_t *           poh,
                                    int *                 exec_res,
                                    ulong *               block_cost,
                                    ulong *               vote_cost ) {
  int res = 0;
  *exec_res = FD_RUNTIME_EXECUTE_SUCCESS;

  *block_cost = 0UL;
  *vote_cost  = 0UL;

  FD_SPAD_FRAME_BEGIN( runner->spad ) {
    fd_capture_ctx_t * capture_ctx = NULL;

    fd_funk_txn_xid_t xid = { .ul = { fd_bank_slot_get( runner->bank ), runner->bank->data->idx } };
    fd_rewards_recalculate_partitioned_rewards( runner->banks, runner->bank, runner->accdb, &xid, runner->runtime_stack, capture_ctx );

    int is_epoch_boundary = 0;
    fd_runtime_block_execute_prepare( runner->banks, runner->bank, runner->accdb, runner->runtime_stack, capture_ctx, &is_epoch_boundary );

    for( ulong i=0UL; i<txn_cnt; i++ ) {
      fd_txn_p_t * txn = &txn_ptrs[i];
      res = FD_RUNTIME_EXECUTE_SUCCESS;
      fd_txn_in_t  txn_in = { .txn = txn, .bundle.is_bundle = 0 };
      fd_txn_out_t txn_out;
      fd_runtime_t * runtime = runner->runtime;
      fd_log_collector_t log[1];
      runtime->log.log_collector = log;
      runtime->acc_pool = runner->acc_pool;
      fd_solfuzz_txn_ctx_exec( runner, runtime, &txn_in, runner->status_cache, &res, &txn_out );
      *exec_res = txn_out.err.txn_err;
      txn_out.err.exec_err = res;

      if( FD_UNLIKELY( !txn_out.err.is_committable ) ) {
        fd_runtime_cancel_txn( runtime, &txn_out );
        return 0;
      }

      fd_runtime_commit_txn( runtime, runner->bank, &txn_out );

      if( FD_UNLIKELY( !txn_out.err.is_committable ) ) return 0;
    }

    fd_cost_tracker_t const * cost_tracker = fd_bank_cost_tracker_locking_query( runner->bank );
    if( cost_tracker ) {
      *block_cost = cost_tracker->block_cost;
      *vote_cost  = cost_tracker->vote_cost;
    }
    fd_bank_cost_tracker_end_locking_query( runner->bank );

    fd_bank_poh_set( runner->bank, *poh );
    fd_runtime_block_execute_finalize( runner->bank, runner->accdb, capture_ctx );
    if( !( runner->bank->data->flags & FD_BANK_FLAGS_FROZEN ) ) {
      fd_banks_mark_bank_frozen( runner->banks, runner->bank );
    }
    if( FD_LIKELY( !fd_solfuzz_multiblock_reused_root_fork ) ) {
      fd_solfuzz_multiblock_status_cache_finalize_current( runner );
    }
  } FD_SPAD_FRAME_END;

  return 1;
}

static void
fd_solfuzz_multiblock_release_frozen_cost_tracker( fd_solfuzz_runner_t * runner ) {
  fd_bank_cost_tracker_t * cost_tracker_pool = fd_bank_get_cost_tracker_pool( runner->bank->data );
  ulong null_idx = fd_bank_cost_tracker_pool_idx_null( cost_tracker_pool );
  if( !( runner->bank->data->flags & FD_BANK_FLAGS_FROZEN ) ||
      runner->bank->data->cost_tracker_pool_idx==null_idx ) return;

  fd_rwlock_write( &runner->banks->locks->banks_lock );
  fd_bank_cost_tracker_pool_idx_release( cost_tracker_pool, runner->bank->data->cost_tracker_pool_idx );
  runner->bank->data->cost_tracker_pool_idx = null_idx;
  fd_rwlock_unwrite( &runner->banks->locks->banks_lock );
}

static void
fd_solfuzz_multiblock_advance_root_to_current( fd_solfuzz_runner_t * runner ) {
  fd_bank_data_t * bank_pool = fd_banks_get_bank_pool( runner->banks->data );
  ulong root_idx = runner->banks->data->root_idx;
  ulong bank_idx = runner->bank->data->idx;

  if( FD_UNLIKELY( root_idx==fd_banks_pool_idx_null( bank_pool ) ) ) return;

  if( FD_UNLIKELY( bank_idx==root_idx ) ) return;

  ulong advanceable_idx = fd_banks_pool_idx_null( bank_pool );
  FD_TEST( fd_banks_advance_root_prepare( runner->banks, bank_idx, &advanceable_idx ) );
  FD_TEST( advanceable_idx==bank_idx );
  fd_txncache_advance_root( runner->status_cache, runner->bank->data->txncache_fork_id );
  fd_banks_advance_root( runner->banks, bank_idx );
}

static void
fd_solfuzz_multiblock_build_leader_schedule_effects( fd_solfuzz_runner_t *          runner,
                                                     fd_funk_txn_xid_t const *      xid,
                                                     fd_exec_test_block_effects_t * effects ) {
  fd_epoch_schedule_t es_;
  fd_epoch_schedule_t * sched = fd_sysvar_epoch_schedule_read( runner->accdb, xid, &es_ );
  FD_TEST( sched!=NULL );

  ulong epoch          = fd_bank_epoch_get( runner->bank );
  ulong ls_slot0       = fd_epoch_slot0( sched, epoch );
  ulong slots_in_epoch = fd_epoch_slot_cnt( sched, epoch );

  fd_epoch_leaders_t const * effects_leaders = fd_bank_epoch_leaders_query( runner->bank );

  effects->has_leader_schedule               = 1;
  effects->leader_schedule.leaders_epoch     = epoch;
  effects->leader_schedule.leaders_slot0     = ls_slot0;
  effects->leader_schedule.leaders_slot_cnt  = slots_in_epoch;
  effects->leader_schedule.leaders_sched_cnt = slots_in_epoch;
  effects->leader_schedule.leader_pub_cnt    = fd_solfuzz_block_hash_epoch_leaders(
      runner, effects_leaders, LEADER_SCHEDULE_HASH_SEED, effects->leader_schedule.leader_schedule_hash );
}

static void
fd_solfuzz_multiblock_fill_effects( fd_solfuzz_runner_t *          runner,
                                    int                            is_committable,
                                    ulong                          block_cost,
                                    ulong                          vote_cost,
                                    fd_exec_test_block_effects_t * effects ) {
  fd_memset( effects, 0, sizeof(fd_exec_test_block_effects_t) );
  effects->has_error = !is_committable;
  effects->slot_capitalization = !effects->has_error ? fd_bank_capitalization_get( runner->bank ) : 0UL;

  fd_hash_t bank_hash = !effects->has_error ? fd_bank_bank_hash_get( runner->bank ) : (fd_hash_t){0};
  fd_memcpy( effects->bank_hash, bank_hash.hash, sizeof(fd_hash_t) );

  effects->has_cost_tracker = 1;
  effects->cost_tracker = (fd_exec_test_cost_tracker_t) {
    .block_cost = block_cost,
    .vote_cost  = vote_cost,
  };

  fd_funk_txn_xid_t xid = { .ul = { fd_bank_slot_get( runner->bank ), runner->bank->data->idx } };
  fd_solfuzz_multiblock_build_leader_schedule_effects( runner, &xid, effects );
}

static void *
fd_solfuzz_multiblock_out_alloc( ulong * cursor,
                                 ulong   end,
                                 ulong   align,
                                 ulong   sz ) {
  ulong p = fd_ulong_align_up( *cursor, align );
  if( FD_UNLIKELY( p>end || sz>end-p ) ) return NULL;
  *cursor = p + sz;
  return (void *)p;
}

static pb_bytes_array_t *
fd_solfuzz_multiblock_out_alloc_bytes( ulong * cursor,
                                       ulong   end,
                                       ulong   sz ) {
  pb_bytes_array_t * out = fd_solfuzz_multiblock_out_alloc( cursor, end, alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE( sz ) );
  if( FD_UNLIKELY( !out ) ) return NULL;
  out->size = (pb_size_t)sz;
  return out;
}

static void
fd_solfuzz_multiblock_dump_fee_rate_governor( fd_bank_t *                        bank,
                                              fd_exec_test_fee_rate_governor_t * out ) {
  fd_fee_rate_governor_t const * frg = fd_bank_fee_rate_governor_query( bank );
  *out = (fd_exec_test_fee_rate_governor_t){
    .target_lamports_per_signature = frg->target_lamports_per_signature,
    .target_signatures_per_slot    = frg->target_signatures_per_slot,
    .min_lamports_per_signature    = frg->min_lamports_per_signature,
    .max_lamports_per_signature    = frg->max_lamports_per_signature,
    .burn_percent                  = frg->burn_percent,
  };
}

static void
fd_solfuzz_multiblock_dump_epoch_schedule( fd_bank_t *                     bank,
                                           fd_exec_test_epoch_schedule_t * out ) {
  fd_epoch_schedule_t const * es = fd_bank_epoch_schedule_query( bank );
  *out = (fd_exec_test_epoch_schedule_t){
    .slots_per_epoch             = es->slots_per_epoch,
    .leader_schedule_slot_offset = es->leader_schedule_slot_offset,
    .warmup                      = es->warmup,
    .first_normal_epoch          = es->first_normal_epoch,
    .first_normal_slot           = es->first_normal_slot,
  };
}

static void
fd_solfuzz_multiblock_dump_rent( fd_bank_t *           bank,
                                 fd_exec_test_rent_t * out ) {
  fd_rent_t const * r = fd_bank_rent_query( bank );
  *out = (fd_exec_test_rent_t){
    .lamports_per_byte_year = r->lamports_per_uint8_year,
    .exemption_threshold    = r->exemption_threshold,
    .burn_percent           = r->burn_percent,
  };
}

static int
fd_solfuzz_multiblock_dump_features( fd_bank_t *                   bank,
                                     ulong *                       cursor,
                                     ulong                         end,
                                     fd_exec_test_feature_set_t *  out ) {
  uint64_t * features = fd_solfuzz_multiblock_out_alloc( cursor, end, alignof(uint64_t), FD_FEATURE_ID_CNT * sizeof(uint64_t) );
  if( FD_UNLIKELY( !features ) ) return 0;

  ulong cnt = 0UL;
  fd_features_t const * enabled = fd_bank_features_query( bank );
  for( fd_feature_id_t const * current_feature = fd_feature_iter_init();
       !fd_feature_iter_done( current_feature );
       current_feature = fd_feature_iter_next( current_feature ) ) {
    if( enabled->f[current_feature->index] != FD_FEATURE_DISABLED ) {
      features[cnt++] = (uint64_t)current_feature->id.ul[0];
    }
  }

  out->features       = features;
  out->features_count = (pb_size_t)cnt;
  return 1;
}

static int
fd_solfuzz_multiblock_dump_blockhash_queue( fd_bank_t *                             bank,
                                            ulong *                                 cursor,
                                            ulong                                   end,
                                            fd_exec_test_blockhash_queue_entry_t ** entries_out,
                                            pb_size_t *                             count_out ) {
  fd_blockhashes_t const * bhq      = fd_bank_block_hash_queue_query( bank );
  ulong                    bhq_size = fd_ulong_min( FD_BLOCKHASHES_MAX, fd_blockhash_deq_cnt( bhq->d.deque ) );

  fd_exec_test_blockhash_queue_entry_t * entries = fd_solfuzz_multiblock_out_alloc(
      cursor, end, alignof(fd_exec_test_blockhash_queue_entry_t), bhq_size * sizeof(fd_exec_test_blockhash_queue_entry_t) );
  if( FD_UNLIKELY( !entries && bhq_size ) ) return 0;

  ulong cnt = 0UL;
  for( fd_blockhash_deq_iter_t iter=fd_blockhash_deq_iter_init_rev( bhq->d.deque );
       !fd_blockhash_deq_iter_done_rev( bhq->d.deque, iter ) && cnt<bhq_size;
       iter=fd_blockhash_deq_iter_prev( bhq->d.deque, iter ), cnt++ ) {
    fd_blockhash_info_t const * ele   = fd_blockhash_deq_iter_ele_const( bhq->d.deque, iter );
    fd_exec_test_blockhash_queue_entry_t * entry = &entries[bhq_size-cnt-1UL];
    fd_memcpy( entry->blockhash, ele->hash.uc, sizeof(fd_hash_t) );
    entry->lamports_per_signature = ele->fee_calculator.lamports_per_signature;
  }

  *entries_out = entries;
  *count_out   = (pb_size_t)bhq_size;
  return 1;
}

static int
fd_solfuzz_multiblock_track_account_key( fd_pubkey_t * keys,
                                         ulong *       key_cnt,
                                         ulong         key_cap,
                                         fd_pubkey_t const * key ) {
  for( ulong i=0UL; i<*key_cnt; i++ ) {
    if( fd_pubkey_eq( &keys[i], key ) ) return 1;
  }
  if( FD_UNLIKELY( *key_cnt>=key_cap ) ) return 0;
  keys[ (*key_cnt)++ ] = *key;
  return 1;
}

static int
fd_solfuzz_multiblock_track_txn_accounts( fd_pubkey_t *                                keys,
                                          ulong *                                      key_cnt,
                                          ulong                                        key_cap,
                                          fd_exec_test_sanitized_transaction_t const * txns,
                                          ulong                                        txn_cnt ) {
  for( ulong i=0UL; i<txn_cnt; i++ ) {
    if( FD_UNLIKELY( !txns[i].has_message ) ) continue;
    fd_exec_test_transaction_message_t const * message = &txns[i].message;

    for( ulong j=0UL; j<message->account_keys_count; j++ ) {
      pb_bytes_array_t const * key_bytes = message->account_keys[j];
      if( FD_UNLIKELY( !key_bytes || key_bytes->size!=sizeof(fd_pubkey_t) ) ) continue;
      fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, key_bytes->bytes );
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_account_key( keys, key_cnt, key_cap, &pubkey ) ) ) return 0;
    }

    for( ulong j=0UL; j<message->address_table_lookups_count; j++ ) {
      fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, message->address_table_lookups[j].account_key );
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_account_key( keys, key_cnt, key_cap, &pubkey ) ) ) return 0;
    }
  }
  return 1;
}

static int
fd_solfuzz_multiblock_dump_account_state( fd_solfuzz_runner_t *       runner,
                                          fd_funk_txn_xid_t const *   xid,
                                          fd_pubkey_t const *         pubkey,
                                          ulong *                     cursor,
                                          ulong                       end,
                                          fd_exec_test_acct_state_t * out ) {
  fd_memcpy( out->address, pubkey, sizeof(fd_pubkey_t) );

  fd_accdb_ro_t ro[1];
  if( FD_LIKELY( fd_accdb_open_ro( runner->accdb, ro, xid, pubkey ) ) ) {
    fd_account_meta_t const * meta = ro->meta;
    out->lamports   = meta->lamports;
    out->executable = !!meta->executable;
    fd_memcpy( out->owner, meta->owner, sizeof(fd_pubkey_t) );
    out->data = fd_solfuzz_multiblock_out_alloc_bytes( cursor, end, meta->dlen );
    if( FD_UNLIKELY( !out->data ) ) {
      fd_accdb_close_ro( runner->accdb, ro );
      return 0;
    }
    fd_memcpy( out->data->bytes, fd_account_data( meta ), meta->dlen );
    fd_accdb_close_ro( runner->accdb, ro );
    return 1;
  }

  out->lamports   = 0UL;
  out->executable = 0;
  fd_memset( out->owner, 0, sizeof(fd_pubkey_t) );
  out->data = fd_solfuzz_multiblock_out_alloc_bytes( cursor, end, 0UL );
  return !!out->data;
}

static void
fd_solfuzz_multiblock_apply_bank_template_overrides( fd_exec_test_block_bank_t *                     bank,
                                                     org_solana_sealevel_v1_block_step_t const * step ) {
  if( step->has_features ) {
    bank->has_features = 1;
    bank->features     = step->features;
  }
  if( step->has_inflation ) {
    bank->has_inflation = 1;
    bank->inflation     = step->inflation;
  }
  if( step->has_fee_rate_governor ) {
    bank->has_fee_rate_governor = 1;
    bank->fee_rate_governor     = step->fee_rate_governor;
  }
}

static int
fd_solfuzz_multiblock_fill_final_context( fd_solfuzz_runner_t *                         runner,
                                          org_solana_sealevel_v1_multi_block_context_t const * input,
                                          org_solana_sealevel_v1_multi_block_effects_t * effects,
                                          ulong *                                         cursor,
                                          ulong                                           end ) {
  fd_exec_test_block_context_t * final_ctx = &effects->final_context;
  fd_memset( final_ctx, 0, sizeof(fd_exec_test_block_context_t) );
  final_ctx->has_bank = 1;
  final_ctx->bank     = input->start.bank;

  for( ulong i=0UL; i<input->steps_count; i++ ) {
    fd_solfuzz_multiblock_apply_bank_template_overrides( &final_ctx->bank, &input->steps[i] );
  }

  fd_hash_t const * last_blockhash = fd_blockhashes_peek_last_hash( fd_bank_block_hash_queue_query( runner->bank ) );
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_blockhash_queue(
      runner->bank, cursor, end, &final_ctx->bank.blockhash_queue, &final_ctx->bank.blockhash_queue_count ) ) ) return 0;

  final_ctx->bank.rbh_lamports_per_signature = (uint)fd_bank_rbh_lamports_per_sig_get( runner->bank );
  final_ctx->bank.has_fee_rate_governor      = 1;
  fd_solfuzz_multiblock_dump_fee_rate_governor( runner->bank, &final_ctx->bank.fee_rate_governor );
  final_ctx->bank.slot               = fd_bank_slot_get( runner->bank );
  final_ctx->bank.parent_slot        = fd_bank_parent_slot_get( runner->bank );
  final_ctx->bank.capitalization     = fd_bank_capitalization_get( runner->bank );
  fd_w_u128_t ns_per_slot = fd_bank_ns_per_slot_get( runner->bank );
  fd_memcpy( final_ctx->bank.ns_per_slot, &ns_per_slot.ud, sizeof(uint128) );
  final_ctx->bank.has_inflation      = 1;
  fd_inflation_t const * inflation = fd_bank_inflation_query( runner->bank );
  final_ctx->bank.inflation = (fd_exec_test_inflation_t){
    .initial         = inflation->initial,
    .terminal        = inflation->terminal,
    .taper           = inflation->taper,
    .foundation      = inflation->foundation,
    .foundation_term = inflation->foundation_term,
  };
  final_ctx->bank.block_height       = fd_bank_block_height_get( runner->bank );
  if( last_blockhash ) fd_memcpy( final_ctx->bank.poh, last_blockhash, sizeof(fd_hash_t) );
  else                 fd_memset( final_ctx->bank.poh, 0, sizeof(fd_hash_t) );
  fd_hash_t bank_hash = fd_bank_bank_hash_get( runner->bank );
  fd_memcpy( final_ctx->bank.parent_bank_hash, bank_hash.hash, sizeof(fd_hash_t) );
  fd_lthash_value_t const * lthash = fd_bank_lthash_locking_query( runner->bank );
  fd_memcpy( final_ctx->bank.parent_lt_hash, lthash, sizeof(fd_lthash_value_t) );
  fd_bank_lthash_end_locking_query( runner->bank );
  final_ctx->bank.parent_signature_count = fd_bank_signature_count_get( runner->bank );
  final_ctx->bank.has_epoch_schedule     = 1;
  fd_solfuzz_multiblock_dump_epoch_schedule( runner->bank, &final_ctx->bank.epoch_schedule );
  final_ctx->bank.has_rent               = 1;
  fd_solfuzz_multiblock_dump_rent( runner->bank, &final_ctx->bank.rent );
  final_ctx->bank.has_features           = 1;
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_features( runner->bank, cursor, end, &final_ctx->bank.features ) ) ) return 0;

  final_ctx->txns_count = 0U;
  final_ctx->txns       = NULL;

  ulong tracked_cap = input->start.acct_states_count;
  for( ulong i=0UL; i<input->steps_count; i++ ) {
    tracked_cap += input->steps[i].acct_states_count;
  }
  tracked_cap += input->start.txns_count * 64UL;
  for( ulong i=0UL; i<input->steps_count; i++ ) {
    tracked_cap += input->steps[i].txns_count * 64UL;
  }

  fd_pubkey_t * tracked_keys = fd_solfuzz_multiblock_out_alloc( cursor, end, alignof(fd_pubkey_t), tracked_cap * sizeof(fd_pubkey_t) );
  if( FD_UNLIKELY( !tracked_keys && tracked_cap ) ) return 0;
  ulong tracked_cnt = 0UL;

  for( ulong i=0UL; i<input->start.acct_states_count; i++ ) {
    fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, input->start.acct_states[i].address );
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_account_key( tracked_keys, &tracked_cnt, tracked_cap, &pubkey ) ) ) return 0;
  }
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_txn_accounts(
      tracked_keys, &tracked_cnt, tracked_cap, input->start.txns, input->start.txns_count ) ) ) return 0;

  for( ulong i=0UL; i<input->steps_count; i++ ) {
    org_solana_sealevel_v1_block_step_t const * step = &input->steps[i];
    for( ulong j=0UL; j<step->acct_states_count; j++ ) {
      fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, step->acct_states[j].address );
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_account_key( tracked_keys, &tracked_cnt, tracked_cap, &pubkey ) ) ) return 0;
    }
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_track_txn_accounts(
        tracked_keys, &tracked_cnt, tracked_cap, step->txns, step->txns_count ) ) ) return 0;
  }

  final_ctx->acct_states = fd_solfuzz_multiblock_out_alloc(
      cursor, end, alignof(fd_exec_test_acct_state_t), tracked_cnt * sizeof(fd_exec_test_acct_state_t) );
  if( FD_UNLIKELY( !final_ctx->acct_states && tracked_cnt ) ) return 0;
  final_ctx->acct_states_count = (pb_size_t)tracked_cnt;

  fd_funk_txn_xid_t xid = { .ul = { fd_bank_slot_get( runner->bank ), runner->bank->data->idx } };
  for( ulong i=0UL; i<tracked_cnt; i++ ) {
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_account_state(
        runner, &xid, &tracked_keys[i], cursor, end, &final_ctx->acct_states[i] ) ) ) return 0;
  }

  effects->has_final_context = 1;
  return 1;
}

static int
fd_solfuzz_multiblock_fill_prefix_snapshot( fd_solfuzz_runner_t *       runner,
                                            fd_exec_test_block_context_t const * start,
                                            fd_exec_test_block_context_t * out,
                                            ulong *                      cursor,
                                            ulong                        end ) {
  fd_memset( out, 0, sizeof(fd_exec_test_block_context_t) );
  out->has_bank = 1;
  out->bank     = start->bank;

  fd_hash_t const * last_blockhash = fd_blockhashes_peek_last_hash( fd_bank_block_hash_queue_query( runner->bank ) );
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_blockhash_queue(
      runner->bank, cursor, end, &out->bank.blockhash_queue, &out->bank.blockhash_queue_count ) ) ) return 0;

  out->bank.rbh_lamports_per_signature = (uint)fd_bank_rbh_lamports_per_sig_get( runner->bank );
  out->bank.has_fee_rate_governor      = 1;
  fd_solfuzz_multiblock_dump_fee_rate_governor( runner->bank, &out->bank.fee_rate_governor );
  out->bank.slot               = fd_bank_slot_get( runner->bank );
  out->bank.parent_slot        = fd_bank_parent_slot_get( runner->bank );
  out->bank.capitalization     = fd_bank_capitalization_get( runner->bank );
  fd_w_u128_t ns_per_slot = fd_bank_ns_per_slot_get( runner->bank );
  fd_memcpy( out->bank.ns_per_slot, &ns_per_slot.ud, sizeof(uint128) );
  out->bank.has_inflation      = 1;
  fd_inflation_t const * inflation = fd_bank_inflation_query( runner->bank );
  out->bank.inflation = (fd_exec_test_inflation_t){
    .initial         = inflation->initial,
    .terminal        = inflation->terminal,
    .taper           = inflation->taper,
    .foundation      = inflation->foundation,
    .foundation_term = inflation->foundation_term,
  };
  out->bank.block_height       = fd_bank_block_height_get( runner->bank );
  if( last_blockhash ) fd_memcpy( out->bank.poh, last_blockhash, sizeof(fd_hash_t) );
  else                 fd_memset( out->bank.poh, 0, sizeof(fd_hash_t) );
  fd_hash_t bank_hash = fd_bank_bank_hash_get( runner->bank );
  fd_memcpy( out->bank.parent_bank_hash, bank_hash.hash, sizeof(fd_hash_t) );
  fd_lthash_value_t const * lthash = fd_bank_lthash_locking_query( runner->bank );
  fd_memcpy( out->bank.parent_lt_hash, lthash, sizeof(fd_lthash_value_t) );
  fd_bank_lthash_end_locking_query( runner->bank );
  out->bank.parent_signature_count = fd_bank_signature_count_get( runner->bank );
  out->bank.has_epoch_schedule     = 1;
  fd_solfuzz_multiblock_dump_epoch_schedule( runner->bank, &out->bank.epoch_schedule );
  out->bank.has_rent               = 1;
  fd_solfuzz_multiblock_dump_rent( runner->bank, &out->bank.rent );
  out->bank.has_features           = 1;
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_features( runner->bank, cursor, end, &out->bank.features ) ) ) return 0;

  out->txns_count = 0U;
  out->txns       = NULL;
  out->acct_states = fd_solfuzz_multiblock_out_alloc(
      cursor, end, alignof(fd_exec_test_acct_state_t),
      fd_solfuzz_multiblock_prefix_session->tracked_cnt * sizeof(fd_exec_test_acct_state_t) );
  if( FD_UNLIKELY( !out->acct_states && fd_solfuzz_multiblock_prefix_session->tracked_cnt ) ) return 0;
  out->acct_states_count = (pb_size_t)fd_solfuzz_multiblock_prefix_session->tracked_cnt;

  fd_funk_txn_xid_t xid = { .ul = { fd_bank_slot_get( runner->bank ), runner->bank->data->idx } };
  for( ulong i=0UL; i<fd_solfuzz_multiblock_prefix_session->tracked_cnt; i++ ) {
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_dump_account_state(
        runner, &xid, &fd_solfuzz_multiblock_prefix_session->tracked_keys[i], cursor, end, &out->acct_states[i] ) ) ) return 0;
  }

  return 1;
}

int
fd_solfuzz_pb_multiblock_prefix_begin( fd_solfuzz_runner_t *                runner,
                                       fd_exec_test_block_context_t const * start ) {
  int ok = 0;
  fd_solfuzz_multiblock_prefix_session_reset();

  fd_solfuzz_multiblock_prefix_session->start  = *start;
  fd_solfuzz_multiblock_prefix_session->active = 1;

  if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_accounts( start->acct_states, start->acct_states_count ) ) ) goto fail;
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_txn_accounts( start->txns, start->txns_count ) ) ) goto fail;

  fd_hash_t poh = {0};
  ulong block_cost = 0UL;
  ulong vote_cost  = 0UL;

  if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_start( runner, start, &poh ) ) ) goto fail;

  FD_SPAD_FRAME_BEGIN( runner->spad ) {
    fd_txn_p_t * txn_ptrs = fd_solfuzz_multiblock_serialize_txns( runner, start->txns, start->txns_count );
    if( FD_UNLIKELY( start->txns_count && !txn_ptrs ) ) goto done;

    int exec_res = FD_RUNTIME_EXECUTE_SUCCESS;
    int is_committable = fd_solfuzz_multiblock_exec_current(
        runner, txn_ptrs, start->txns_count, &poh, &exec_res, &block_cost, &vote_cost );
    fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
    if( FD_UNLIKELY( !is_committable ) ) goto done;
    ok = 1;
done:
    ;
  } FD_SPAD_FRAME_END;

  if( FD_LIKELY( ok ) ) {
    fd_solfuzz_multiblock_advance_root_to_current( runner );
    return 1;
  }

fail:
  fd_solfuzz_multiblock_cleanup( runner );
  fd_solfuzz_multiblock_prefix_session_reset();
  return 0;
}

ulong
fd_solfuzz_pb_multiblock_prefix_append( fd_solfuzz_runner_t *                     runner,
                                        org_solana_sealevel_v1_block_step_t const * step,
                                        void **                                    output_,
                                        void *                                     output_buf,
                                        ulong                                      output_bufsz ) {
  fd_exec_test_block_context_t ** output = fd_type_pun( output_ );
  if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_session->active ) ) return 0UL;

  FD_SPAD_FRAME_BEGIN( runner->spad ) {
    fd_hash_t poh = {0};
    ulong block_cost = 0UL;
    ulong vote_cost  = 0UL;
    int exec_res = FD_RUNTIME_EXECUTE_SUCCESS;
    ulong target_slot = fd_solfuzz_multiblock_target_slot( fd_bank_slot_get( runner->bank ), step );

    while( fd_bank_slot_get( runner->bank ) + 1UL < target_slot ) {
      org_solana_sealevel_v1_block_step_t empty_step = {0};
      empty_step.slot = fd_bank_slot_get( runner->bank ) + 1UL;
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_step( runner, &empty_step, &poh ) ) ) goto fail;

      int is_committable = fd_solfuzz_multiblock_exec_current( runner, NULL, 0UL, &poh, &exec_res, &block_cost, &vote_cost );
      fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
      if( FD_UNLIKELY( !is_committable ) ) goto fail;
      fd_solfuzz_multiblock_advance_root_to_current( runner );
    }

    if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_step( runner, step, &poh ) ) ) goto fail;

    fd_txn_p_t * txn_ptrs = fd_solfuzz_multiblock_serialize_txns( runner, step->txns, step->txns_count );
    if( FD_UNLIKELY( step->txns_count && !txn_ptrs ) ) goto fail;

    int is_committable = fd_solfuzz_multiblock_exec_current(
        runner, txn_ptrs, step->txns_count, &poh, &exec_res, &block_cost, &vote_cost );
    fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
    if( FD_UNLIKELY( !is_committable ) ) goto fail;

    if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_accounts( step->acct_states, step->acct_states_count ) ) ) goto fail;
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_prefix_track_txn_accounts( step->txns, step->txns_count ) ) ) goto fail;

    FD_SCRATCH_ALLOC_INIT( l, output_buf );
    ulong output_end = (ulong)output_buf + output_bufsz;
    fd_exec_test_block_context_t * snapshot =
      FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_block_context_t), sizeof(fd_exec_test_block_context_t) );
    if( FD_UNLIKELY( _l > output_end ) ) goto fail;

    if( FD_UNLIKELY( !fd_solfuzz_multiblock_fill_prefix_snapshot(
        runner, &fd_solfuzz_multiblock_prefix_session->start, snapshot, &_l, output_end ) ) ) goto fail;

    ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
    fd_solfuzz_multiblock_advance_root_to_current( runner );
    *output = snapshot;
    return actual_end - (ulong)output_buf;

fail:
    fd_solfuzz_multiblock_cleanup( runner );
    fd_solfuzz_multiblock_prefix_session_reset();
    *output = NULL;
    return 0UL;
  } FD_SPAD_FRAME_END;
}

void
fd_solfuzz_pb_multiblock_prefix_fini( fd_solfuzz_runner_t * runner ) {
  if( fd_solfuzz_multiblock_prefix_session->active ) {
    fd_solfuzz_multiblock_cleanup( runner );
  }
  fd_solfuzz_multiblock_prefix_session_reset();
}

ulong
fd_solfuzz_pb_multiblock_run( fd_solfuzz_runner_t * runner,
                              void const *          input_,
                              void **               output_,
                              void *                output_buf,
                              ulong                 output_bufsz ) {
  org_solana_sealevel_v1_multi_block_context_t const * input  = fd_type_pun_const( input_ );
  org_solana_sealevel_v1_multi_block_effects_t **      output = fd_type_pun( output_ );

  if( FD_UNLIKELY( !input->has_start ) ) return 0;

  FD_SPAD_FRAME_BEGIN( runner->spad ) {
    FD_SCRATCH_ALLOC_INIT( l, output_buf );
    ulong output_end = (ulong)output_buf + output_bufsz;

    org_solana_sealevel_v1_multi_block_effects_t * effects =
      FD_SCRATCH_ALLOC_APPEND( l, alignof(org_solana_sealevel_v1_multi_block_effects_t),
                                  sizeof(org_solana_sealevel_v1_multi_block_effects_t) );
    if( FD_UNLIKELY( _l > output_end ) ) abort();
    fd_memset( effects, 0, sizeof(org_solana_sealevel_v1_multi_block_effects_t) );

    ulong total_blocks = 1UL;
    ulong prev_slot = input->start.bank.slot;
    for( ulong i=0UL; i<input->steps_count; i++ ) {
      ulong target_slot = fd_solfuzz_multiblock_target_slot( prev_slot, &input->steps[i] );
      total_blocks += target_slot>prev_slot ? target_slot-prev_slot : 1UL;
      prev_slot = target_slot;
    }
    fd_exec_test_block_effects_t * per_block =
      FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_block_effects_t),
                                  total_blocks * sizeof(fd_exec_test_block_effects_t) );
    if( FD_UNLIKELY( _l > output_end ) ) abort();
    fd_memset( per_block, 0, total_blocks * sizeof(fd_exec_test_block_effects_t) );
    effects->per_block = per_block;
    effects->per_block_count = 0U;

    fd_hash_t poh = {0};
    ulong block_cost = 0UL;
    ulong vote_cost  = 0UL;
    if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_start( runner, &input->start, &poh ) ) ) {
      fd_solfuzz_multiblock_cleanup( runner );
      return 0;
    }

    fd_txn_p_t * txn_ptrs = fd_solfuzz_multiblock_serialize_txns( runner, input->start.txns, input->start.txns_count );
    if( FD_UNLIKELY( input->start.txns_count && !txn_ptrs ) ) {
      fd_solfuzz_multiblock_cleanup( runner );
      return 0;
    }

    int exec_res = FD_RUNTIME_EXECUTE_SUCCESS;
    int is_committable = fd_solfuzz_multiblock_exec_current( runner, txn_ptrs, input->start.txns_count, &poh, &exec_res, &block_cost, &vote_cost );
    fd_solfuzz_multiblock_fill_effects( runner, is_committable, block_cost, vote_cost, &per_block[0] );
    fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
    effects->per_block_count = 1U;
    effects->blocks_executed = 1U;

    if( !is_committable ) {
      ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
      fd_solfuzz_multiblock_cleanup( runner );
      *output = effects;
      return actual_end - (ulong)output_buf;
    }

    if( FD_LIKELY( input->steps_count ) ) {
      fd_solfuzz_multiblock_advance_root_to_current( runner );
    }

    for( ulong i=0UL; i<input->steps_count; i++ ) {
      org_solana_sealevel_v1_block_step_t const * step = &input->steps[i];
      ulong target_slot = fd_solfuzz_multiblock_target_slot( fd_bank_slot_get( runner->bank ), step );
      while( fd_bank_slot_get( runner->bank ) + 1UL < target_slot ) {
        org_solana_sealevel_v1_block_step_t empty_step = {0};
        empty_step.slot = fd_bank_slot_get( runner->bank ) + 1UL;
        if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_step( runner, &empty_step, &poh ) ) ) goto done;

        exec_res = FD_RUNTIME_EXECUTE_SUCCESS;
        is_committable = fd_solfuzz_multiblock_exec_current( runner, NULL, 0UL, &poh, &exec_res, &block_cost, &vote_cost );
        fd_solfuzz_multiblock_fill_effects( runner, is_committable, block_cost, vote_cost, &per_block[ effects->per_block_count ] );
        fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
        effects->per_block_count++;
        effects->blocks_executed++;

        if( !is_committable ) goto done;
        fd_solfuzz_multiblock_advance_root_to_current( runner );
      }

      if( FD_UNLIKELY( !fd_solfuzz_multiblock_init_step( runner, step, &poh ) ) ) break;

      txn_ptrs = fd_solfuzz_multiblock_serialize_txns( runner, step->txns, step->txns_count );
      if( FD_UNLIKELY( step->txns_count && !txn_ptrs ) ) break;

      exec_res = FD_RUNTIME_EXECUTE_SUCCESS;
      is_committable = fd_solfuzz_multiblock_exec_current( runner, txn_ptrs, step->txns_count, &poh, &exec_res, &block_cost, &vote_cost );
      fd_solfuzz_multiblock_fill_effects( runner, is_committable, block_cost, vote_cost, &per_block[ effects->per_block_count ] );
      fd_solfuzz_multiblock_release_frozen_cost_tracker( runner );
      effects->per_block_count++;
      effects->blocks_executed++;

      if( !is_committable ) break;
      if( FD_LIKELY( i+1UL<input->steps_count ) ) {
        fd_solfuzz_multiblock_advance_root_to_current( runner );
      }
    }

done:
    ;
    if( FD_LIKELY( effects->blocks_executed==effects->per_block_count &&
                   effects->per_block_count==total_blocks ) ) {
      if( FD_UNLIKELY( !fd_solfuzz_multiblock_fill_final_context( runner, input, effects, &_l, output_end ) ) ) {
        fd_solfuzz_multiblock_cleanup( runner );
        return 0;
      }
    }
    ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
    fd_solfuzz_multiblock_cleanup( runner );
    *output = effects;
    return actual_end - (ulong)output_buf;
  } FD_SPAD_FRAME_END;
}
