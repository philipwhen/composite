/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#include "include/captbl.h"
#include "include/inv.h"
#include "include/thd.h"
#include "include/call_convention.h"
#include "include/ipi_cap.h"
#include "include/liveness_tbl.h"
#include "include/cpuid.h"

#define COS_DEFAULT_RET_CAP 0

static inline void
fs_reg_setup(unsigned long seg) {
#ifdef LINUX_TEST
	return;
#endif
	asm volatile ("movl %%ebx, %%fs\n\t"
		      : : "b" (seg));
}

#ifdef LINUX_TEST
int
syscall_handler(struct pt_regs *regs)

#else

#define MAX_LEN 512

extern char timer_detector[PAGE_SIZE] PAGE_ALIGNED;
static inline int printfn(struct pt_regs *regs) 
{
#ifdef LINUX_TEST
	__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));
	return 0;
#endif
	char *str; 
	int len;
	char kern_buf[MAX_LEN];

	fs_reg_setup(__KERNEL_PERCPU);

	str     = (char *)__userregs_get1(regs);
	len     = __userregs_get2(regs);

	if (len < 1) goto done;
	if (len >= MAX_LEN) len = MAX_LEN - 1;
	memcpy(kern_buf, str, len);

	if (len >= 6) { //well, hack to flush tlb and cache...
		if (kern_buf[0] == 'F' && kern_buf[1] == 'L' && kern_buf[2] == 'U' &&
		    kern_buf[3] == 'S' && kern_buf[4] == 'H' && kern_buf[5] == '!') {
			chal_flush_cache();
			chal_flush_tlb_global();
			u32_t *ticks = (u32_t *)&timer_detector[get_cpuid() * CACHE_LINE];
//			printk("inv ticks %u\n", *ticks);

			__userregs_set(regs, *ticks, 
				       __userregs_getsp(regs), __userregs_getip(regs));

			return 0;
		}
	}
	
	kern_buf[len] = '\0';
	printk("%s", kern_buf);
done:
	__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));

	return 0;
}

static int
cap_switch_thd(struct pt_regs *regs, struct thread *curr, struct thread *next, struct cos_cpu_local_info *cos_info) 
{
	int preempt = 0;
	struct comp_info *next_ci = &(next->invstk[next->invstk_top].comp_info);

	if (unlikely(!ltbl_isalive(&next_ci->liveness))) {
		printk("cos: comp (liveness %d) doesn't exist!\n", next_ci->liveness.id);
		//FIXME: add fault handling here.
		__userregs_set(regs, -EFAULT, __userregs_getsp(regs), __userregs_getip(regs));
		return preempt;
	}
	
	copy_gp_regs(regs, &curr->regs);
	__userregs_set(&curr->regs, COS_SCHED_RET_SUCCESS, __userregs_getsp(regs), __userregs_getip(regs));

	thd_current_update(next, curr, cos_info);

	pgtbl_update(next_ci->pgtbl);

	/* fpu_save(thd); */
	if (next->flags & THD_STATE_PREEMPTED) {
		cos_meas_event(COS_MEAS_SWITCH_PREEMPT);
		/* remove_preempted_status(thd); */
		next->flags &= ~THD_STATE_PREEMPTED;
		preempt = 1;
	}
//	printk("Core %d: switching from %d to thd %d, preempted %d\n", get_cpuid(), curr->tid, next->tid, preempt);
		
	/* update_sched_evts(thd, thd_sched_flags, curr, curr_sched_flags); */
	/* event_record("switch_thread", thd_get_id(thd), thd_get_id(next)); */
	copy_gp_regs(&next->regs, regs);

	return preempt;
}

#define ENABLE_KERNEL_PRINT

__attribute__((section("__ipc_entry"))) COS_SYSCALL int
composite_sysenter_handler(struct pt_regs *regs)
#endif
{
	struct cap_header *ch;
	struct comp_info *ci;
	struct captbl *ct;
	struct thread *thd;
	capid_t cap;
	unsigned long ip, sp;
	syscall_op_t op;
	/* We lookup this struct (which is on stack) only once, and
	 * pass it into other functions to avoid unnecessary
	 * lookup. */
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();
	int ret = 0;

#ifdef ENABLE_KERNEL_PRINT
	fs_reg_setup(__KERNEL_PERCPU);
#endif
	/* printk("calling cap %d: %x, %x, %x, %x\n", */
	/*        cap, __userregs_get1(regs), __userregs_get2(regs), __userregs_get3(regs), __userregs_get4(regs)); */

	cap = __userregs_getcap(regs);

	thd = thd_current(cos_info);

	/* fast path: invocation return */
	if (cap == COS_DEFAULT_RET_CAP) {
		/* No need to lookup captbl */
		sret_ret(thd, regs, cos_info);
		return 0;
	}

	/* FIXME: use a cap for print */
	if (unlikely(regs->ax == PRINT_CAP_TEMP)) {
		printfn(regs);
		return 0;
	}

	ci  = thd_invstk_current(thd, &ip, &sp, cos_info);
	assert(ci && ci->captbl);

	/* We don't check liveness of current component because it's
	 * guaranteed by component quiescence period, which is at
	 * timer tick granularity.*/

	ch  = captbl_lkup(ci->captbl, cap);
	if (unlikely(!ch)) {
		printk("cos: cap %d not found!\n", cap);
		ret = -ENOENT;
		goto done;
	}

	/* fastpath: invocation */
	if (likely(ch->type == CAP_SINV)) {
		sinv_call(thd, (struct cap_sinv *)ch, regs, cos_info);
		return 0;
	}

	/* Some less common cases: thread dispatch, asnd and arcv
	 * operations. */
	if (ch->type == CAP_THD) {
		struct cap_thd *thd_cap = (struct cap_thd *)ch;
		struct thread *next = thd_cap->t;

		if (thd_cap->cpuid != get_cpuid()) cos_throw(err, EINVAL);
		assert(thd_cap->cpuid == next->cpuid);

		// QW: hack!!! for ppos test only. remove!
		next->interrupted_thread = thd;

		return cap_switch_thd(regs, thd, next, cos_info);
	} else if (ch->type == CAP_ASND) {
		int curr_cpu = get_cpuid();
		struct cap_asnd *asnd = (struct cap_asnd *)ch;

		assert(asnd->arcv_capid);

		if (asnd->arcv_cpuid != curr_cpu) {
			/* Cross core: sending IPI */
			ret = cos_cap_send_ipi(asnd->arcv_cpuid, asnd);
			/* printk("sending ipi to cpu %d. ret %d\n", asnd->arcv_cpuid, ret); */
		} else {
			struct cap_arcv *arcv;

			printk("NOT tested yet.\n");

			if (unlikely(!ltbl_isalive(&(asnd->comp_info.liveness)))) {
				// fault handle? 
				cos_throw(err, -EFAULT);
			}

			arcv = (struct cap_arcv *)captbl_lkup(asnd->comp_info.captbl, asnd->arcv_capid);
			if (unlikely(arcv->h.type != CAP_ARCV)) {
				printk("cos: IPI handling received invalid arcv cap %d\n", asnd->arcv_capid);
				cos_throw(err, EINVAL);
			}
			
			return cap_switch_thd(regs, thd, arcv->thd, cos_info);
		}
				
		goto done;
	} else if (ch->type == CAP_ARCV) {
		struct cap_arcv *arcv = (struct cap_arcv *)ch;

		/*FIXME: add epoch checking!*/

		if (arcv->thd != thd) {
			cos_throw(err, EINVAL);
		}

		/* Sanity checks */
		assert(arcv->cpuid == get_cpuid());
		assert(arcv->comp_info.pgtbl = ci->pgtbl);
		assert(arcv->comp_info.captbl = ci->captbl);

		if (arcv->pending) {
			arcv->pending--;
			ret = 0;

			goto done;
		}
		
		if (thd->interrupted_thread == NULL) {
			/* FIXME: handle this case by upcalling into
			 * scheduler, or switch to a scheduling
			 * thread. */
			ret = -1;
			printk("ERROR: not implemented yet!\n");
		} else {
			thd->arcv_cap = cap;
			thd->flags &= !THD_STATE_ACTIVE_UPCALL;
			thd->flags |= THD_STATE_READY_UPCALL;
			
			return cap_switch_thd(regs, thd, thd->interrupted_thread, cos_info);
		}
		
		goto done;
	}

	fs_reg_setup(__KERNEL_PERCPU);

	/* slowpath: other capability operations, most of which
	 * involve writing. */
	op = __userregs_getop(regs);
	ct = ci->captbl; 

	switch(ch->type) {
	case CAP_CAPTBL:
	{
		capid_t capin =  __userregs_get1(regs);
		/* 
		 * FIXME: make sure that the lvl of the pgtbl makes
		 * sense for the op.
		 */
		switch(op) {
		case CAPTBL_OP_CAPTBLACTIVATE:
		{
			capid_t pgtbl_cap      = __userregs_get1(regs);
			vaddr_t kmem_cap       = __userregs_get2(regs);
			capid_t newcaptbl_cap  = __userregs_get3(regs);
			vaddr_t kmem_addr = 0;
			struct captbl *newct;
			
			ret = cap_mem_retype2kern(ct, pgtbl_cap, kmem_cap, (unsigned long *)&kmem_addr);
			if (unlikely(ret)) cos_throw(err, ret);
			assert(kmem_addr);

			newct = captbl_create(kmem_addr);
			assert(newct);
			ret = captbl_activate(ct, cap, newcaptbl_cap, newct, 0);

			break;
		}
		case CAPTBL_OP_PGDACTIVATE:
		{
			capid_t pgtbl_cap  = __userregs_get1(regs);
			vaddr_t kmem_cap   = __userregs_get2(regs);
			capid_t newpgd_cap = __userregs_get3(regs);
			vaddr_t kmem_addr  = 0;
			pgtbl_t new_pt, curr_pt;
			struct cap_pgtbl *pt;

			ret = cap_mem_retype2kern(ct, pgtbl_cap, kmem_cap, (unsigned long *)&kmem_addr);
			if (unlikely(ret)) cos_throw(err, ret);
			assert(kmem_addr);

			curr_pt = ((struct cap_pgtbl *)captbl_lkup(ct, pgtbl_cap))->pgtbl;
			assert(curr_pt);

			new_pt = pgtbl_create(kmem_addr, curr_pt);
			ret = pgtbl_activate(ct, cap, newpgd_cap, new_pt, 0);

			break;
		}
		case CAPTBL_OP_PTEACTIVATE:
		{
			capid_t pgtbl_cap  = __userregs_get1(regs);
			vaddr_t kmem_cap   = __userregs_get2(regs);
			capid_t newpte_cap = __userregs_get3(regs);
			vaddr_t kmem_addr  = 0;

			ret = cap_mem_retype2kern(ct, pgtbl_cap, kmem_cap, (unsigned long *)&kmem_addr);
			if (unlikely(ret)) cos_throw(err, ret);
			assert(kmem_addr);

			pgtbl_init_pte(kmem_addr);
			ret = pgtbl_activate(ct, cap, newpte_cap, (pgtbl_t)kmem_addr, 1);

			break;
		}
		case CAPTBL_OP_THDACTIVATE:
		{
			capid_t thd_cap    = __userregs_get1(regs) & 0xFFFF;
			int init_data      = __userregs_get1(regs) >> 16;
			capid_t pgtbl_cap  = __userregs_get2(regs);
			capid_t pgtbl_addr = __userregs_get3(regs);
			capid_t compcap    = __userregs_get4(regs);
			struct thread *thd;

			ret = cap_mem_retype2kern(ct, pgtbl_cap, pgtbl_addr, (unsigned long *)&thd);
			if (unlikely(ret)) cos_throw(err, ret);

			ret = thd_activate(ct, cap, thd_cap, thd, compcap, init_data);
			/* ret is returned by the overall function */

			break;
		}
		case CAPTBL_OP_THDDEACTIVATE:
			/* 
			 * FIXME: move the thread capability to a
			 * location in a pagetable as COSFRAME
			 */
			ret = thd_deactivate(ct, cap, capin);

			break;
		case CAPTBL_OP_COMPACTIVATE:
		{
			capid_t captbl_cap = __userregs_get2(regs) >> 16;
			capid_t pgtbl_cap  = __userregs_get2(regs) & 0xFFFF;
			livenessid_t lid   = __userregs_get3(regs);
			vaddr_t entry_addr = __userregs_get4(regs);

			ret = comp_activate(ct, cap, capin, captbl_cap, pgtbl_cap, lid, entry_addr, NULL);
			break;
		}
		case CAPTBL_OP_COMPDEACTIVATE:
			ret = comp_deactivate(ct, cap, capin);
			break;
		case CAPTBL_OP_SINVACTIVATE:
		{
			capid_t dest_comp_cap = __userregs_get2(regs);
			vaddr_t entry_addr    = __userregs_get3(regs);
			
			ret = sinv_activate(ct, cap, capin, dest_comp_cap, entry_addr);

			break;
		}
		case CAPTBL_OP_SINVDEACTIVATE:
			ret = sinv_deactivate(ct, cap, capin);
			break;
		case CAPTBL_OP_SRETACTIVATE:
			break;
		case CAPTBL_OP_SRETDEACTIVATE:
			ret = sret_deactivate(ct, cap, capin);
			break;
		case CAPTBL_OP_ASNDACTIVATE:
		{
			capid_t rcv_captbl = __userregs_get2(regs);
			capid_t rcv_cap    = __userregs_get3(regs);

			ret = asnd_activate(ct, cap, capin, rcv_captbl, rcv_cap, 0, 0);

			break;
		}
		case CAPTBL_OP_ASNDDEACTIVATE:
			ret = asnd_deactivate(ct, cap, capin);
			break;
		case CAPTBL_OP_ARCVACTIVATE:
		{
			capid_t thd_cap  = __userregs_get2(regs);
			capid_t comp_cap = __userregs_get3(regs);

			ret = arcv_activate(ct, cap, capin, comp_cap, thd_cap);
			
			break;
		}
		case CAPTBL_OP_ARCVDEACTIVATE:
			ret = arcv_deactivate(ct, cap, capin);
			break;

		case CAPTBL_OP_CPY:
		{
			capid_t from_captbl = cap;
			capid_t from_cap    = __userregs_get1(regs);
			capid_t dest_captbl = __userregs_get2(regs);
			capid_t dest_cap    = __userregs_get3(regs);

			ret = cap_cpy(ct, dest_captbl, dest_cap, 
				      from_captbl, from_cap);
			break;
		}
		case CAPTBL_OP_CONS:
		{
			capid_t target      = cap;
			capid_t target_id   = capin;
			capid_t pgtbl_cap   = __userregs_get2(regs);
			capid_t page_addr  = __userregs_get3(regs);
			void *captbl_mem;
			struct cap_captbl *target_ct;
			
			/* We are doing expanding here. */
			
			ret = cap_mem_retype2kern(ct, pgtbl_cap, page_addr, (unsigned long *)&captbl_mem);
			if (unlikely(ret)) cos_throw(err, ret);

			target_ct = (struct cap_captbl *)captbl_lkup(ct, target);
			if (target_ct->h.type != CAP_CAPTBL) cos_throw(err, EINVAL);

			captbl_init(captbl_mem, 1);
			ret = captbl_expand(target_ct->captbl, target_id, captbl_maxdepth(), captbl_mem);
			if (ret) cos_throw(err, ret);

			captbl_init(&((char*)captbl_mem)[PAGE_SIZE/2], 1);
			ret = captbl_expand(target_ct->captbl, target_id + (PAGE_SIZE/2/CAPTBL_LEAFSZ), 
					    captbl_maxdepth(), &((char*)captbl_mem)[PAGE_SIZE/2]);

			break;
		}
		case CAPTBL_OP_DECONS:
		default: goto err;
		}
		break;
	}
	case CAP_PGTBL:
	{
		capid_t pt = cap;

		switch (op) {
		case CAPTBL_OP_CPY:
		{
			capid_t source_pt   = pt;
			vaddr_t source_addr = __userregs_get1(regs);
			capid_t dest_pt     = __userregs_get2(regs);
			vaddr_t dest_addr   = __userregs_get3(regs);

			ret = cap_cpy(ct, dest_pt, dest_addr, source_pt, source_addr);

			break;
		}
		case CAPTBL_OP_CONS:
		{
			vaddr_t pte_cap   = __userregs_get1(regs);
			vaddr_t cons_addr = __userregs_get2(regs);

			ret = cap_cons(ct, pt, pte_cap, cons_addr);

			break;
		}
		case CAPTBL_OP_DECONS:
		case CAPTBL_OP_MAPPING_CONS:
		{
			break;
		}
		case CAPTBL_OP_MAPPING_DECONS:
		{
			vaddr_t addr = __userregs_get1(regs);

			if (((struct cap_pgtbl *)ch)->lvl) cos_throw(err, EINVAL);
			ret = pgtbl_mapping_del(((struct cap_pgtbl *)ch)->pgtbl, addr);
			
			break;
		}
		case CAPTBL_OP_MAPPING_MOD:
		case CAPTBL_OP_MAPPING_RETYPE:
		default: goto err;
		}
		break;
	}
	case CAP_SRET:
	{
		/* We usually don't have sret cap as we have default
		 * return cap.*/
		sret_ret(thd, regs, cos_info);
		return 0;
	}
	default:
	err:
		ret = -ENOENT;
	}
done:
	__userregs_set(regs, ret, __userregs_getsp(regs), __userregs_getip(regs));

	return 0;
}

void cos_cap_ipi_handling(void)
{
	int idx, end;
	struct IPI_receiving_rings *receiver_rings;
	struct xcore_ring *ring;

	receiver_rings = &IPI_cap_dest[get_cpuid()];

	/* We need to scan the entire buffer once. */
	idx = receiver_rings->start;
	end = receiver_rings->start - 1; //end is int type. could be -1. 
	receiver_rings->start = (receiver_rings->start + 1) % NUM_CPU;

	/* scan the first half */
	for (; idx < NUM_CPU; idx++) {
		ring = &receiver_rings->IPI_source[idx];
		if (ring->sender != ring->receiver) {
			process_ring(ring);
		}
	}

	/* and scan the second half */
	for (idx = 0; idx <= end; idx++) {
		ring = &receiver_rings->IPI_source[idx];
		if (ring->sender != ring->receiver) {
			process_ring(ring);
		}
	}

	return;
}
