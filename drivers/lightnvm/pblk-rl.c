/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <javier@cnexlabs.com>
 *                  Matias Bjorling <matias@cnexlabs.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * pblk-rl.c - pblk's rate limiter for user I/O
 *
 */

#include "pblk.h"

static void pblk_rl_kick_u_timer(struct pblk_rl_per_rb *rb_rl)
{
	mod_timer(&rb_rl->u_timer, jiffies + msecs_to_jiffies(5000));
}

int pblk_rl_is_limit(struct pblk_rl_per_rb *rb_rl)
{
	int rb_space;

	rb_space = atomic_read(&rb_rl->rb_space);

	return (rb_space == 0);
}

int pblk_rl_user_may_insert(struct pblk_rl *rl, struct pblk_rl_per_rb *rb_rl, int nr_entries)
{
	int rb_user_cnt = atomic_read(&rb_rl->rb_user_cnt);
	int rb_space = atomic_read(&rb_rl->rb_space);

	if (unlikely(rb_space >= 0) && (rb_space - nr_entries < 0))
		return NVM_IO_ERR;

	if (rb_user_cnt >= rl->rb_user_max)
		return NVM_IO_REQUEUE;

	return NVM_IO_OK;
}

void pblk_rl_inserted(struct pblk_rl_per_rb *rb_rl, int nr_entries)
{
	int rb_space = atomic_read(&rb_rl->rb_space);

	if (unlikely(rb_space >= 0))
		atomic_sub(nr_entries, &rb_rl->rb_space);
}

int pblk_rl_gc_may_insert(struct pblk_rl *rl, struct pblk_rl_per_rb *rb_rl, int nr_entries)
{
	int rb_gc_cnt = atomic_read(&rb_rl->rb_gc_cnt);
	int rb_user_active;

	/* If there is no user I/O let GC take over space on the write buffer */
	rb_user_active = READ_ONCE(rb_rl->rb_user_active);
	return (!(rb_gc_cnt >= rl->rb_gc_max && rb_user_active));
}

void pblk_rl_user_in(struct pblk_rl_per_rb *rb_rl, int nr_entries)
{
	atomic_add(nr_entries, &rb_rl->rb_user_cnt);

	/* Release user I/O state. Protect from GC */
	smp_store_release(&rb_rl->rb_user_active, 1);
	pblk_rl_kick_u_timer(rb_rl);
}

void pblk_rl_gc_in(struct pblk_rl_per_rb *rb_rl, int nr_entries)
{
	atomic_add(nr_entries, &rb_rl->rb_gc_cnt);
}

void pblk_rl_out(struct pblk_rl_per_rb *rb_rl, int nr_user, int nr_gc)
{
	atomic_sub(nr_user, &rb_rl->rb_user_cnt);
	atomic_sub(nr_gc, &rb_rl->rb_gc_cnt);
}

unsigned long pblk_rl_nr_free_blks(struct pblk_rl *rl)
{
	return atomic_read(&rl->free_blocks);
}

/*
 * We check for (i) the number of free blocks in the current LUN and (ii) the
 * total number of free blocks in the pblk instance. This is to even out the
 * number of free blocks on each LUN when GC kicks in.
 *
 * Only the total number of free blocks is used to configure the rate limiter.
 */
static int pblk_rl_update_rates(struct pblk_rl *rl, unsigned long max)
{
	unsigned long free_blocks = pblk_rl_nr_free_blks(rl);

	if (free_blocks >= rl->high) {
		printk("JJY: update %d %d\n", free_blocks, rl->high);
		rl->rb_user_max = max;
		rl->rb_gc_max = 0;
		rl->rb_state = PBLK_RL_HIGH;
	} else if (free_blocks < rl->high) {
		int shift = rl->high_pw - rl->rb_windows_pw;
		int user_windows = free_blocks >> shift;
		int user_max = user_windows << PBLK_MAX_REQ_ADDRS_PW;

		printk("JJY: update %d %d\n", free_blocks, rl->high);
		rl->rb_user_max = user_max;
		rl->rb_gc_max = max - user_max;

		if (free_blocks <= rl->rsv_blocks) {
			rl->rb_user_max = 0;
			rl->rb_gc_max = max;
		}

		/* In the worst case, we will need to GC lines in the low list
		 * (high valid sector count). If there are lines to GC on high
		 * or mid lists, these will be prioritized
		 */
		rl->rb_state = PBLK_RL_LOW;
	}

	return rl->rb_state;
}

void pblk_rl_free_lines_inc(struct pblk_rl *rl, struct pblk_line *line)
{
	struct pblk *pblk = rl->pblk;
	int blk_in_line = atomic_read(&line->blk_in_line);
	int ret;

	atomic_add(blk_in_line, &rl->free_blocks);
	/* Rates will not change that often - no need to lock update */
	ret = pblk_rl_update_rates(rl, rl->rb_budget);

//	printk("JJY: free_inc %d\n", rl->free_blocks);
	if (ret == (PBLK_RL_MID | PBLK_RL_LOW))
		pblk_gc_should_start(pblk);
	else
		pblk_gc_should_stop(pblk);
}

void pblk_rl_free_lines_dec(struct pblk_rl *rl, struct pblk_line *line)
{
	int blk_in_line = atomic_read(&line->blk_in_line);

	atomic_sub(blk_in_line, &rl->free_blocks);
//	printk("JJY: free_dec %d\n", rl->free_blocks);
}

// JJY: TODO: rl
void pblk_gc_should_kick(struct pblk *pblk)
{
	struct pblk_rl *rl = &pblk->rl;
	int ret;

	/* Rates will not change that often - no need to lock update */
	ret = pblk_rl_update_rates(rl, rl->rb_budget);
	if (ret == (PBLK_RL_MID | PBLK_RL_LOW))
		pblk_gc_should_start(pblk);
	else
		pblk_gc_should_stop(pblk);
}

int pblk_rl_high_thrs(struct pblk_rl *rl)
{
	return rl->high;
}

int pblk_rl_low_thrs(struct pblk_rl *rl)
{
	return rl->low;
}

int pblk_rl_sysfs_rate_show(struct pblk_rl *rl)
{
	return rl->rb_user_max;
}

int pblk_rl_max_io(struct pblk_rl *rl)
{
	return rl->rb_max_io;
}

static void pblk_rl_u_timer(unsigned long data)
{
	struct pblk_rl_per_rb *rb_rl = (struct pblk_rl_per_rb *)data;

	/* Release user I/O state. Protect from GC */
	smp_store_release(&rb_rl->rb_user_active, 0);
}

void pblk_rl_free(struct pblk_rl_per_rb *rb_rl)
{
	del_timer(&rb_rl->u_timer);
}

void pblk_rl_init(struct pblk *pblk, struct pblk_rl *rl, int budget)
{
	struct pblk_line_meta *lm = &pblk->lm;
	int min_blocks = lm->blk_per_line * PBLK_GC_RSV_LINE;
	unsigned int rb_windows;
	int i, nr_rwb = pblk->nr_rwb;

	rl->pblk = pblk;
	rl->high = rl->total_blocks / PBLK_USER_HIGH_THRS;
	rl->high_pw = get_count_order(rl->high);

	rl->low = rl->total_blocks / PBLK_USER_LOW_THRS;
	if (rl->low < min_blocks)
		rl->low = min_blocks;

	rl->rsv_blocks = min_blocks;

	/* This will always be a power-of-2 */
	rb_windows = budget / PBLK_MAX_REQ_ADDRS;
	rl->rb_windows_pw = get_count_order(rb_windows);

	/* To start with, all buffer is available to user I/O writers */
	rl->rb_budget = budget;
	rl->rb_user_max = budget;
	rl->rb_max_io = budget >> 1;
	rl->rb_gc_max = 0;
	rl->rb_state = PBLK_RL_HIGH;

	for (i = 0; i < nr_rwb; i++) {
		struct pblk_rb_ctx *rb_ctx = &pblk->rb_ctx[i];
		
		atomic_set(&rb_ctx->rb_rl.rb_user_cnt, 0);
		atomic_set(&rb_ctx->rb_rl.rb_gc_cnt, 0);			

		rb_ctx->rb_rl.rb_user_active = 0;
		rb_ctx->rb_rl.rb_gc_active = 0;

		atomic_set(&rb_ctx->rb_rl.rb_space, -1);

		setup_timer(&rb_ctx->rb_rl.u_timer, pblk_rl_u_timer, (unsigned long)&rb_ctx->rb_rl);
	}

}
