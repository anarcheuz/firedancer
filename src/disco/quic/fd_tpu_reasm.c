#include "fd_tpu_reasm_private.h"

FD_FN_CONST ulong
fd_tpu_reasm_align( void ) {
  return alignof(fd_tpu_reasm_t);
}

FD_FN_CONST ulong
fd_tpu_reasm_footprint( ulong depth,
                        ulong burst ) {

  if( FD_UNLIKELY(
      ( fd_ulong_popcnt( depth )!=1 ) |
      ( depth>0x7fffffffUL          ) |
      ( burst<2                     ) |
      ( burst>0x7fffffffUL          ) ) )
    return 0UL;

  return FD_TPU_REASM_FOOTPRINT( depth, burst );
}

void *
fd_tpu_reasm_new( void * shmem,
                  ulong  depth,
                  ulong  burst,
                  ulong  orig ) {

  if( FD_UNLIKELY( !shmem ) ) return NULL;
  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, FD_TPU_REASM_ALIGN ) ) ) return NULL;
  if( FD_UNLIKELY( !fd_tpu_reasm_footprint( depth, burst ) ) ) return NULL;
  if( FD_UNLIKELY( orig > FD_FRAG_META_ORIG_MAX ) ) return NULL;

  /* Memory layout */

  ulong slot_cnt = depth+burst;
  if( FD_UNLIKELY( !slot_cnt ) ) return NULL;

  FD_SCRATCH_ALLOC_INIT( l, shmem );
  fd_tpu_reasm_t *      reasm     = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_tpu_reasm_t), sizeof(fd_tpu_reasm_t) );
  ulong *               pub_slots = FD_SCRATCH_ALLOC_APPEND( l, alignof(uint),                depth*sizeof(uint)                   );
  fd_tpu_reasm_slot_t * slots     = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_tpu_reasm_slot_t), slot_cnt*sizeof(fd_tpu_reasm_slot_t) );
  uchar *               chunks    = FD_SCRATCH_ALLOC_APPEND( l, FD_CHUNK_ALIGN,               slot_cnt*FD_TPU_REASM_MTU            );
  FD_SCRATCH_ALLOC_FINI( l, alignof(fd_tpu_reasm_t) );

  fd_memset( reasm, 0, sizeof(fd_tpu_reasm_t) );
  fd_memset( slots, 0, burst*sizeof(fd_tpu_reasm_slot_t) );

  /* Initialize reasm object */

  reasm->slots_off     = (ulong)( (uchar *)slots     - (uchar *)reasm );
  reasm->pub_slots_off = (ulong)( (uchar *)pub_slots - (uchar *)reasm );
  reasm->chunks_off    = (ulong)( (uchar *)chunks    - (uchar *)reasm );

  reasm->depth    = (uint)depth;
  reasm->burst    = (uint)burst;
  reasm->head     = (uint)slot_cnt-1U;
  reasm->tail     = (uint)depth;
  reasm->slot_cnt = (uint)slot_cnt;
  reasm->orig     = (ushort)orig;

  /* Initial slot distribution */

  fd_tpu_reasm_reset( reasm );

  FD_COMPILER_MFENCE();
  reasm->magic = FD_TPU_REASM_MAGIC;
  FD_COMPILER_MFENCE();

  return reasm;
}

void
fd_tpu_reasm_reset( fd_tpu_reasm_t * reasm ) {

  uint depth    = reasm->depth;
  uint burst    = reasm->burst;
  uint node_cnt = depth+burst;
  fd_tpu_reasm_slot_t * slots = fd_tpu_reasm_slots_laddr( reasm );
  uint * pub_slots = fd_tpu_reasm_pub_slots_laddr( reasm );

  /* The initial state moves the first 'depth' slots to the mcache (PUB)
     and leaves the rest as FREE. */

  for( uint j=0U; j<depth; j++ ) {
    slots    [ j ].state = FD_TPU_REASM_STATE_PUB;
    pub_slots[ j ]       = j;
  }
  for( uint j=depth; j<node_cnt; j++ ) {
    fd_tpu_reasm_slot_t * slot = slots + j;
    slot->state    = FD_TPU_REASM_STATE_FREE;
    slot->prev_idx = fd_uint_if( j<node_cnt-1U, j+1U, UINT_MAX );
    slot->next_idx = fd_uint_if( j>depth,       j-1U, UINT_MAX );
  }
}

fd_tpu_reasm_t *
fd_tpu_reasm_join( void * shreasm ) {
  fd_tpu_reasm_t * reasm = shreasm;
  if( FD_UNLIKELY( reasm->magic != FD_TPU_REASM_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return NULL;
  }
  return reasm;
}

void *
fd_tpu_reasm_leave( fd_tpu_reasm_t * reasm ) {
  return reasm;
}

void *
fd_tpu_reasm_delete( void * shreasm ) {
  fd_tpu_reasm_t * reasm = shreasm;
  if( FD_UNLIKELY( !reasm ) ) return NULL;
  reasm->magic = 0UL;
  return shreasm;
}

FD_FN_CONST ulong
fd_tpu_reasm_chunk0( fd_tpu_reasm_t const * reasm,
                     void const *           base ) {
  return fd_laddr_to_chunk( base, slot_get_data_const( reasm, 0UL ) );
}


FD_FN_CONST ulong
fd_tpu_reasm_wmark( fd_tpu_reasm_t const * reasm,
                    void const *           base ) {
  /* U.B. if slot_cnt==0, but this is checked in fd_tpu_reasm_new */
  return fd_laddr_to_chunk( base, slot_get_data_const( reasm, reasm->slot_cnt - 1UL ) );
}


fd_tpu_reasm_slot_t *
fd_tpu_reasm_prepare( fd_tpu_reasm_t * reasm,
                      ulong            tsorig ) {
  fd_tpu_reasm_slot_t * slot = slotq_pop_tail( reasm );
  slot_begin( slot );
  slotq_push_head( reasm, slot );
  slot->tsorig = (uint)tsorig;
  return slot;
}

int
fd_tpu_reasm_append( fd_tpu_reasm_t *      reasm,
                     fd_tpu_reasm_slot_t * slot,
                     uchar const *         data,
                     ulong                 data_sz,
                     ulong                 data_off ) {

  if( FD_UNLIKELY( slot->state != FD_TPU_REASM_STATE_BUSY ) )
    return FD_TPU_REASM_ERR_STATE;

  ulong slot_idx = slot_get_idx( reasm, slot );
  ulong mtu      = FD_TXN_MTU;
  ulong sz0      = slot->sz;

  if( FD_UNLIKELY( data_off>sz0 ) ) {
    fd_tpu_reasm_cancel( reasm, slot );
    return FD_TPU_REASM_ERR_SKIP;
  }

  if( FD_UNLIKELY( data_off<sz0 ) ) {
    /* Fragment partially known ... should not happen */
    ulong skip = sz0 - data_off;
    if( skip>data_sz ) return FD_TPU_REASM_SUCCESS;
    data_off  += skip;
    data_sz   -= skip;
    data      += skip;
  }

  ulong sz1 = sz0 + data_sz;
  if( FD_UNLIKELY( (sz1<sz0)|(sz1>mtu) ) ) {
    fd_tpu_reasm_cancel( reasm, slot );
    return FD_TPU_REASM_ERR_SZ;
  }

  uchar * msg = slot_get_data( reasm, slot_idx );
  fd_memcpy( msg+sz0, data, data_sz );

  slot->sz = (ushort)sz1;
  return FD_TPU_REASM_SUCCESS;
}

int
fd_tpu_reasm_publish( fd_tpu_reasm_t *      reasm,
                      fd_tpu_reasm_slot_t * slot,
                      fd_frag_meta_t *      mcache,
                      void *                base,  /* Assumed aligned FD_CHUNK_ALIGN */
                      ulong                 seq,
                      ulong                 tspub ) {

  if( FD_UNLIKELY( slot->state != FD_TPU_REASM_STATE_BUSY ) )
    return FD_TPU_REASM_ERR_STATE;

  /* Derive chunk index */
  uint    slot_idx      = slot_get_idx( reasm, slot );
  uchar * data          = slot_get_data( reasm, slot_idx );
  ulong   data_laddr    = (ulong)data;
  ulong   chunk         = fd_laddr_to_chunk( base, (void *)data_laddr );
  if( FD_UNLIKELY( ( data_laddr<(ulong)base ) |
                   ( chunk>UINT_MAX         ) ) ) {
    FD_LOG_CRIT(( "invalid base %p for slot %p in tpu_reasm %p",
                  base, (void *)slot, (void *)reasm ));
  }

  /* Acquire mcache line */
  ulong  depth          = reasm->depth;

  /* Detect which slot this message belongs to */
  uint * pub_slot       = fd_tpu_reasm_pub_slots_laddr( reasm ) + fd_mcache_line_idx( seq, depth );
  uint   freed_slot_idx = *pub_slot;

  if( FD_UNLIKELY( freed_slot_idx >= reasm->slot_cnt ) ) {
    /* mcache corruption */
    FD_LOG_WARNING(( "mcache corruption detected! tpu_reasm slot %u out of bounds (max %u)",
                     freed_slot_idx, reasm->slot_cnt ));
    fd_tpu_reasm_reset( reasm );
    return FD_TPU_REASM_ERR_STATE;
  }

  /* Mark new slot as published */
  slotq_remove( reasm, slot );
  slot->state = FD_TPU_REASM_STATE_PUB;
  *pub_slot = slot_idx;

  /* Free oldest published slot */
  fd_tpu_reasm_slot_t * free_slot = fd_tpu_reasm_slots_laddr( reasm ) + freed_slot_idx;
  uint free_slot_state = free_slot->state;
  if( FD_UNLIKELY( free_slot_state != FD_TPU_REASM_STATE_PUB ) ) {
    /* mcache/slots out of sync (memory leak) */
    FD_LOG_WARNING(( "mcache corruption detected! tpu_reasm seq %lu owns slot %u, but it's state is %u",
                     seq, freed_slot_idx, free_slot_state ));
    fd_tpu_reasm_reset( reasm );
    return FD_TPU_REASM_ERR_STATE;
  }
  free_slot->state = FD_TPU_REASM_STATE_FREE;
  slotq_push_tail( reasm, free_slot );

  /* Publish to mcache */
  ulong sz     = slot->sz;
  ulong ctl    = fd_frag_meta_ctl( reasm->orig, 1, 1, 0 );
  ulong tsorig = slot->tsorig;

# if FD_HAS_AVX
  fd_mcache_publish_avx( mcache, depth, seq, 0UL, chunk, sz, ctl, tsorig, tspub );
# elif FD_HAS_SSE
  fd_mcache_publish_sse( mcache, depth, seq, 0UL, chunk, sz, ctl, tsorig, tspub );
# else
  fd_frag_meta_t * meta = mcache + fd_mcache_line_idx( seq, depth );
  FD_COMPILER_MFENCE();
  meta->seq    = fd_seq_dec( seq, 1UL );
  FD_COMPILER_MFENCE();
  meta->chunk  = (uint  )chunk;
  meta->sz     = (ushort)sz;
  meta->ctl    = (ushort)ctl;
  meta->tsorig = (uint  )tsorig;
  meta->tspub  = (uint  )tspub;
  FD_COMPILER_MFENCE();
  meta->seq    = seq;
  FD_COMPILER_MFENCE();
# endif

  return FD_TPU_REASM_SUCCESS;
}

void
fd_tpu_reasm_cancel( fd_tpu_reasm_t *      reasm,
                     fd_tpu_reasm_slot_t * slot ) {
  if( FD_UNLIKELY( slot->state == FD_TPU_REASM_STATE_FREE ) ) return;
  slotq_remove( reasm, slot );
  slot->state     = FD_TPU_REASM_STATE_FREE;
  slot->conn_id   = 0UL;
  slot->stream_id = 0UL;
  slotq_push_tail( reasm, slot );
}
