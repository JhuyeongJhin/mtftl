/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <jg@lightnvm.io>
 *                  Matias Bjorling <m@bjorling.me>
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

/*
 * Copy data from current bio to write buffer. This if necessary to guarantee
 * that (i) writes to the media at issued at the right granurality and (ii) that
 * memory-specific constrains are respected (e.g., TLC memories need to write
 * upper, medium and lower pages to guarantee that data has been persisted).
 *
 * This path is exclusively taken by user I/O.
 *
 * return: 1 if bio has been written to buffer, 0 otherwise.
 */
static int __pblk_write_to_cache(struct pblk *pblk, struct bio *bio,
				 unsigned long flags, unsigned int nr_entries,
				 unsigned int cpu, int *kick, int nr_secs)
{
	sector_t laddr = pblk_get_laddr(bio);
	struct pblk_w_ctx w_ctx;
	unsigned long bpos, pos;
	unsigned int i;

	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */

	if (!pblk_rb_may_write(&pblk->rwb[cpu], nr_entries, nr_entries, &bpos))
		return 0;

	if ( pblk->user_pin )
	{
		spin_lock(&pblk->kick_lock[cpu]);
		pblk->write_cnt[cpu] += nr_secs;
	        if (pblk->write_cnt[cpu] > PBLK_KICK_SECTS) {
        	        pblk->write_cnt[cpu] -= PBLK_KICK_SECTS;
	                *kick = 1;
	        }
		spin_unlock(&pblk->kick_lock[cpu]);

	        preempt_enable();
	}

	w_ctx.flags = flags;
	w_ctx.priv = NULL;
	w_ctx.paddr = 0;
	ppa_set_empty(&w_ctx.ppa.ppa);

	for (i = 0; i < nr_entries; i++) {
		void *data = bio_data(bio);
		struct ppa_addr ppa;

		w_ctx.lba = laddr + i;

		pos = pblk_rb_wrap_pos(&pblk->rwb[cpu], bpos + i);
		pblk_rb_write_entry(&pblk->rwb[cpu], data, w_ctx, pos);
		ppa = pblk_cacheline_to_ppa(pos, cpu);

		pblk_update_map(pblk, w_ctx.lba, NULL, ppa);
		bio_advance(bio, PBLK_EXPOSED_PAGE_SIZE);
	}

#ifdef CONFIG_NVM_DEBUG
	atomic_add(nr_entries, &pblk->inflight_writes);
	atomic_add(nr_entries, &pblk->req_writes);
#endif

	return 1;
}

int pblk_write_to_cache(struct pblk *pblk, struct bio *bio, unsigned long flags)
{
	int nr_secs = pblk_get_secs(bio);
	int ret = NVM_IO_DONE;
	unsigned int cpu;
	int kick = 0;

	if (bio->bi_opf & REQ_PREFLUSH) {
		int i;
#ifdef CONFIG_NVM_DEBUG
		atomic_inc(&pblk->nr_flush);
#endif

		for (i = 0 ; i < pblk->nr_rwb ; i++)
			if (pblk_rb_sync_point_set(&pblk->rwb[i], bio))
				ret |= NVM_IO_OK;

		if ( ret != NVM_IO_OK )
			ret = NVM_IO_DONE;

		if (!bio_has_data(bio)) {
			pblk_write_kick_all(pblk);
			goto out;
		}
	}

	if ( pblk->user_pin )
	{
	pin_retry:
	        preempt_disable();
	        cpu = smp_processor_id();
       	 	pblk_rl_user_in(pblk, nr_secs, &cpu); 
        	if (!__pblk_write_to_cache(pblk, bio, flags, nr_secs, cpu, &kick, nr_secs)) {
			pblk_rl_out(pblk, nr_secs, 0, cpu);
               		preempt_enable();
                	schedule();
	                goto pin_retry;
	        }

	        if ( kick == 1 || (bio->bi_opf & REQ_PREFLUSH) )
	                pblk_write_kick(pblk, cpu);

	        return ret;

	}

	if ( pblk->rb_choice == 1 )
		cpu = pblk_rb_random(pblk->nr_rwb, pblk->rb_gc_choice);
	else if ( pblk->rb_choice == 2 )
		cpu = pblk_rb_remain_max(pblk, nr_secs);

	pblk_rl_user_in(pblk, nr_secs, &cpu);
retry:
	if (!__pblk_write_to_cache(pblk, bio, flags, nr_secs, cpu, &kick, nr_secs)) {
		schedule();
		goto retry;
	}

	spin_lock(&pblk->kick_lock[cpu]);
	pblk->write_cnt[cpu] += nr_secs;
	if (pblk->write_cnt[cpu] > PBLK_KICK_SECTS) {
		pblk->write_cnt[cpu] -= PBLK_KICK_SECTS;
		spin_unlock(&pblk->kick_lock[cpu]);

		pblk_write_kick(pblk, cpu);
	} else {
		spin_unlock(&pblk->kick_lock[cpu]);
	}

	if ( bio->bi_opf & REQ_PREFLUSH )
		pblk_write_kick(pblk, cpu);

out:
	return ret;
}

/*
 * On GC the incoming lbas are not necessarily sequential. Also, some of the
 * lbas might reside in the write cache.
 */
int pblk_write_gc_to_cache(struct pblk *pblk, unsigned int cpu, void *data, u64 *lba_list,
			   struct pblk_kref_buf *ref_buf,
			   unsigned int nr_entries, unsigned int nr_rec_entries,
			   unsigned long flags, struct pblk_block *gc_rblk)
{
	struct pblk_w_ctx w_ctx;
	unsigned long bpos, pos;
	unsigned int valid_entries;
	unsigned int i;

retry:
	/* Update the write buffer head (mem) with the entries that we can
	 * write. The write in itself cannot fail, so there is no need to
	 * rollback from here on.
	 */
	if (!pblk_rb_may_write(&pblk->rwb[cpu], nr_entries, nr_rec_entries, &bpos)) {
		schedule();
		goto retry;
	}

	w_ctx.flags = flags | PBLK_IOTYPE_GC;
	w_ctx.priv = ref_buf;
	w_ctx.paddr = 0;
	ppa_set_empty(&w_ctx.ppa.ppa);

	for (i = 0, valid_entries = 0; i < nr_entries; i++) {
		struct ppa_addr ppa;

		if (lba_list[i] == ADDR_EMPTY)
			continue;

		w_ctx.lba = lba_list[i];

#ifdef CONFIG_NVM_DEBUG
		BUG_ON(!(flags & PBLK_IOTYPE_REF));
#endif
		kref_get(&ref_buf->ref);

		pos = pblk_rb_wrap_pos(&pblk->rwb[cpu], bpos + valid_entries);
		pblk_rb_write_entry(&pblk->rwb[cpu], data, w_ctx, pos);
		ppa = pblk_cacheline_to_ppa(pos, cpu);

		pblk_update_map_gc(pblk, w_ctx.lba, NULL, ppa, gc_rblk);

		data += PBLK_EXPOSED_PAGE_SIZE;
		valid_entries++;
	}

#ifdef CONFIG_NVM_DEBUG
	BUG_ON(nr_rec_entries != valid_entries);
	atomic_add(valid_entries, &pblk->inflight_writes);
	atomic_add(valid_entries, &pblk->recov_gc_writes);
#endif

	return NVM_IO_OK;
}

