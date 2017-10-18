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
 * pblk-cache.c - pblk's write cache
 */

#include "pblk.h"

int pblk_write_to_cache(struct pblk *pblk, struct bio *bio, unsigned long flags)
{
	struct pblk_w_ctx w_ctx;
	sector_t lba = pblk_get_lba(bio);
	unsigned int bpos, pos;
	int nr_entries = pblk_get_secs(bio);
	int i, ret;
	unsigned int nrb;
	int user_rb_option = pblk->user_rb_option;

	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */
retry:
	if (user_rb_option == 1) {
		preempt_disable();
		nrb = smp_processor_id();
	} else if (user_rb_option == 2) {
		nrb = pblk_rb_random(pblk->nr_rwb);
	} else if (user_rb_option == 3) {
		nrb = pblk_rb_remain_max(pblk, nr_entries);
	}

	ret = pblk_rb_may_write_user(&pblk->rb_ctx[nrb].rwb, nrb, bio, nr_entries, &bpos);
	switch (ret) {
	case NVM_IO_REQUEUE:
		if (user_rb_option == 1)
			preempt_enable();
		io_schedule();
		goto retry;
	case NVM_IO_ERR:
		if (user_rb_option == 1)
			preempt_enable();
		pblk_pipeline_stop(pblk);
		goto out;
	}

	spin_lock(&pblk->rb_ctx[nrb].rwb.w_lock);
	pblk_rl_user_in(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
	spin_unlock(&pblk->rb_ctx[nrb].rwb.w_lock);

	if (user_rb_option == 1)
		preempt_enable();	// JJY: TODO: enable position

	if (unlikely(!bio_has_data(bio)))
		goto out;

	w_ctx.flags = flags;
	pblk_ppa_set_empty(&w_ctx.ppa);

	for (i = 0; i < nr_entries; i++) {
		void *data = bio_data(bio);

		w_ctx.lba = lba + i;

		pos = pblk_rb_wrap_pos(&pblk->rb_ctx[nrb].rwb, bpos + i);
		pblk_rb_write_entry_user(&pblk->rb_ctx[nrb].rwb, data, w_ctx, pos);

		bio_advance(bio, PBLK_EXPOSED_PAGE_SIZE);
	}

#ifdef CONFIG_NVM_DEBUG
	atomic_long_add(nr_entries, &pblk->inflight_writes);
	atomic_long_add(nr_entries, &pblk->req_writes);
#endif
	
	pblk_rl_inserted(&pblk->rb_ctx[nrb].rb_rl, nr_entries);
	printk("JJY: cache cpu %d rb %d pos %d\n", smp_processor_id(), nrb, bpos);
out:
	pblk_write_should_kick(pblk, nrb);
	return ret;
}

// JJY: TODO: GC RB
/*
 * On GC the incoming lbas are not necessarily sequential. Also, some of the
 * lbas might not be valid entries, which are marked as empty by the GC thread
 */
int pblk_write_gc_to_cache(struct pblk *pblk, void *data, u64 *lba_list,
			   unsigned int nr_entries, unsigned int nr_rec_entries,
			   struct pblk_line *gc_line, unsigned long flags)
{
	struct pblk_w_ctx w_ctx;
	unsigned int bpos, pos;
	int i, valid_entries;

	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */
retry:
	if (!pblk_rb_may_write_gc(&pblk->rb_ctx[0].rwb, nr_rec_entries, &bpos)) {
		io_schedule();
		goto retry;
	}

	w_ctx.flags = flags;
	pblk_ppa_set_empty(&w_ctx.ppa);

	for (i = 0, valid_entries = 0; i < nr_entries; i++) {
		if (lba_list[i] == ADDR_EMPTY)
			continue;

		w_ctx.lba = lba_list[i];

		pos = pblk_rb_wrap_pos(&pblk->rb_ctx[0].rwb, bpos + valid_entries);
		pblk_rb_write_entry_gc(&pblk->rb_ctx[0].rwb, data, w_ctx, gc_line, pos);

		data += PBLK_EXPOSED_PAGE_SIZE;
		valid_entries++;
	}

	WARN_ONCE(nr_rec_entries != valid_entries,
					"pblk: inconsistent GC write\n");

#ifdef CONFIG_NVM_DEBUG
	atomic_long_add(valid_entries, &pblk->inflight_writes);
	atomic_long_add(valid_entries, &pblk->recov_gc_writes);
#endif

	pblk_write_should_kick(pblk, 0);
	return NVM_IO_OK;
}
