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

uint64_t helper_lookup_shadow_ret_addr(CPUState *env, target_ulong pc)
{
    int index = pc % MAX_CALL_SLOT;
    shadow_pair *entry = (shadow_pair*)shadow_hash_list[index].next;
    while (entry != NULL) {
        if (entry->guest_eip == pc) return (target_ulong)entry->shadow_slot;
        entry = (shadow_pair*)entry->l.next;
    }
    unsigned long slot;
    insert_unresolved_eip(env, pc, &slot);
    return (uint64_t)slot;
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

#ifdef DEBUG_SHACK
void helper_print_push_shack(CPUState *env)
{
    fprintf(stderr, "push_shack, top = %p\n", env->shack_top);
}

void helper_print_pop_shack(CPUState *env)
{
    fprintf(stderr, "pop_shack, top = %p, value = %016llx\n", env->shack_top, *env->shack_top);
}
#endif

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    int label_flush = gen_new_label();
    int label_end = gen_new_label();
    TCGv_ptr cpu_shack_top = tcg_temp_local_new();
    TCGv_ptr cpu_shack_end = tcg_temp_new();
    tcg_gen_ld_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_ptr(cpu_shack_end, cpu_env, offsetof(CPUState, shack_end));
    // goto flush if stack full
    tcg_gen_brcond_ptr(TCG_COND_GE, cpu_shack_top, cpu_shack_end, label_flush);
    tcg_temp_free(cpu_shack_end);
    // build shack entry
    TCGv_i64 shack_entry = tcg_temp_new_i64();
    tcg_gen_movi_i64(shack_entry, next_eip);
    tcg_gen_shli_i64(shack_entry, shack_entry, 32);
    // TODO load host eip into shack entry
    TCGv_i64 host_eip = tcg_temp_new_i64();
    gen_helper_lookup_shadow_ret_addr(host_eip, cpu_env, tcg_const_tl(next_eip));
    tcg_gen_or_i64(shack_entry, shack_entry, host_eip);
    tcg_temp_free_i64(host_eip);
    // push shack
    tcg_gen_st_i64(shack_entry, cpu_shack_top, 0);
    tcg_temp_free_i64(shack_entry);
    // increment shack top
    tcg_gen_addi_ptr(cpu_shack_top, cpu_shack_top, sizeof(*((CPUState*)0)->shack_top));
    tcg_gen_st_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_temp_free(cpu_shack_top);
    tcg_gen_br(label_end);
    // flush
    gen_set_label(label_flush);
    gen_helper_shack_flush(cpu_env);
    // end
    gen_set_label(label_end);
#ifdef DEBUG_SHACK
    gen_helper_print_push_shack(cpu_env);
#endif
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip)
{
    TCGv_ptr cpu_shack_top = tcg_temp_new();
    tcg_gen_ld_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    // TODO
    tcg_gen_subi_ptr(cpu_shack_top, cpu_shack_top, sizeof(*((CPUState*)0)->shack_top));
    tcg_gen_st_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_temp_free(cpu_shack_top);
#ifdef DEBUG_SHACK
    gen_helper_print_pop_shack(cpu_env);
#endif
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip)
{
    return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb)
{
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env)
{
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
