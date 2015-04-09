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
}

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
inline void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip)
{
}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env)
{
    // TODO
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    int label_flush = gen_new_label();
    int label_end = gen_new_label();
    TCGv_ptr cpu_shack_top = tcg_temp_new();
    TCGv_ptr cpu_shack_end = tcg_temp_new();
    tcg_gen_ld_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_ptr(cpu_shack_end, cpu_env, offsetof(CPUState, shack_end));
    // goto flush if stack full
    tcg_gen_brcond_ptr(TCG_COND_GE, cpu_shack_top, cpu_shack_end, label_flush);
    tcg_temp_free(cpu_shack_top);
    tcg_temp_free(cpu_shack_end);
    // push shack
    cpu_shack_top = tcg_temp_new(); // dead after branch
    tcg_gen_ld_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_addi_ptr(cpu_shack_top, cpu_shack_top, sizeof(*((CPUState*)0)->shack_top));
    tcg_gen_st_ptr(cpu_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_temp_free(cpu_shack_top);
    tcg_gen_br(label_end);
    // flush
    gen_set_label(label_flush);
    helper_shack_flush(env);
    // end
    gen_set_label(label_end);
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
