/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdlib.h>
#include "exec-all.h"
#include "tcg-op.h"
#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"
#include "optimization.h"

extern uint8_t *optimization_ret_addr;

/*
 * Shadow Stack
 */
list_t *shadow_hash_list;

static inline void shack_init(CPUState *env)
{
    env->shack = (uint64_t*)malloc(SHACK_SIZE);
    env->shack_top = env->shack;
    env->shack_end = (void*)env->shack + SHACK_SIZE;
    shadow_hash_list = (list_t*)malloc(sizeof(list_t) * MAX_CALL_SLOT);
    int i;
    for (i = 0; i < MAX_CALL_SLOT; i++)
        shadow_hash_list[i].prev = shadow_hash_list[i].next = NULL;
}

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
inline void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip)
{
    int index = guest_eip % MAX_CALL_SLOT;
    list_t **entry_ptr = &shadow_hash_list[index].next;
    int found = 0;
    while (*entry_ptr != NULL) {
        if (((shadow_pair*)*entry_ptr)->guest_eip == guest_eip) {
            found = 1;
            ((shadow_pair*)*entry_ptr)->shadow_slot = host_eip;
            break;
        }
        entry_ptr = &(*entry_ptr)->next;
    }
    if (!found) {
        *entry_ptr = (list_t*)malloc(sizeof(shadow_pair));
        ((shadow_pair*)*entry_ptr)->l.prev = (void*)entry_ptr - offsetof(list_t, next);
        ((shadow_pair*)*entry_ptr)->l.next = NULL;
        ((shadow_pair*)*entry_ptr)->guest_eip = guest_eip;
        ((shadow_pair*)*entry_ptr)->shadow_slot = host_eip;
    }
}

inline void insert_unresolved_eip(CPUState *env, target_ulong next_eip, unsigned long *slot)
{
    int index = next_eip % MAX_CALL_SLOT;
    shadow_pair *entry = (shadow_pair*)malloc(sizeof(shadow_pair));
    entry->l.prev = &shadow_hash_list[index];
    entry->l.next = shadow_hash_list[index].next;
    entry->guest_eip = next_eip;
    entry->shadow_slot = NULL;
    if (shadow_hash_list[index].next != NULL)
        shadow_hash_list[index].next->prev = (list_t*)entry;
    shadow_hash_list[index].next = (list_t*)entry;
    *slot = (unsigned long)entry->shadow_slot;
}

unsigned long lookup_shadow_ret_addr(CPUState *env, target_ulong pc)
{
    int index = pc % MAX_CALL_SLOT;
    shadow_pair *entry = (shadow_pair*)shadow_hash_list[index].next;
    while (entry != NULL) {
        if (entry->guest_eip == pc) return (target_ulong)entry->shadow_slot;
        entry = (shadow_pair*)entry->l.next;
    }
    unsigned long slot;
    insert_unresolved_eip(env, pc, &slot);
    return slot;
}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env)
{
    env->shack_top = env->shack;
#ifdef DEBUG_SHACK
    fprintf(stderr, "flush\n");
#endif
}

void helper_push_shack(CPUState *env, target_ulong next_eip)
{
    if (env->shack_top >= env->shack_end) helper_shack_flush(env);
    *env->shack_top++ = ((uint64_t)next_eip << 32) | lookup_shadow_ret_addr(env, next_eip);
#ifdef DEBUG_SHACK
    fprintf(stderr, "push_shack, top = %p\n", env->shack_top);
#endif
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    gen_helper_push_shack(cpu_env, tcg_const_tl(next_eip));
}

target_ulong helper_pop_shack(CPUState *env, target_ulong next_eip)
{
#ifdef DEBUG_SHACK
    fprintf(stderr, "pop_shack, top = %p, value = %016llx\n", env->shack_top - 1, *(env->shack_top - 1));
#endif
    if (env->shack_top <= env->shack) return 0;
    uint64_t shack_entry = *--env->shack_top;
    if (shack_entry >> 32 == next_eip) return shack_entry & 0xffffffff;
    return 0;
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip)
{
    int label_end = gen_new_label();
    TCGv host_eip = tcg_temp_local_new();
    gen_helper_pop_shack(host_eip, cpu_env, next_eip);
    // jump to next_eip if not zero
    tcg_gen_brcondi_tl(TCG_COND_EQ, host_eip, 0, label_end);
    *gen_opc_ptr++ = INDEX_op_jmp;
    *gen_opparam_ptr++ = GET_TCGV_PTR(host_eip);
    tcg_temp_free(host_eip);
    gen_set_label(label_end);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;
struct ibtc_table ibtc_table;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip)
{
    struct jmp_pair *entry = &ibtc_table.htable[guest_eip & IBTC_CACHE_MASK];
    if (entry->guest_eip == guest_eip) {
#ifdef DEBUG_IBTC
        fprintf(stderr, "hit\n");
#endif
        return entry->tb->tc_ptr;
    }
    return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb)
{
    struct jmp_pair *entry = &ibtc_table.htable[tb->pc & IBTC_CACHE_MASK];
    entry->guest_eip = tb->pc;
    entry->tb = tb;
#ifdef DEBUG_IBTC
    fprintf(stderr, "update %p -> %p\n", tb->pc, tb->tc_ptr);
#endif
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env)
{
    update_ibtc = 1;
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env)
{
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
