static int __unmap_sma_page(struct page *page, int force,
		enum migrate_mode mode, enum migrate_reason reason)
{
	int rc = -EAGAIN;
	struct anon_vma *anon_vma = NULL;
	bool is_lru = !__PageMovable(page);

	if (!page->is_sec_mem) {
		pr_err("%s:%d non-SMA page %lx not supported!\n",
				__func__, __LINE__, page_to_pfn(page));
		BUG();
	}

	if (!trylock_page(page)) {
		if (!force || mode == MIGRATE_ASYNC)
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readpages). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	if (PageWriteback(page)) {
		pr_err("%s:%d disk page cache not support! pfn: %lx\n",
				__func__, __LINE__, page_to_pfn(page));
		BUG();
	}

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);

	if (unlikely(!is_lru)) {
		pr_err("%s:%d ERROR: page %lx is_lru: %d\n",
				__func__, __LINE__, page_to_pfn(page), is_lru);
		BUG();
	}

	if (!page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(page), page);
		VM_BUG_ON_PAGE(page_has_private(page), page);
		if (!page_mapped(page)) {
			rc = MIGRATEPAGE_SUCCESS;
		}
	} else if (page_mapped(page)) {
		bool is_unmapped = false;
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(page) && !PageKsm(page) && !anon_vma,
				page);
		is_unmapped = try_to_unmap(page,
				TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		if (is_unmapped)
			rc = MIGRATEPAGE_SUCCESS;
		else {
			/* Failed to unmap old page, clear the swap_pte */
			remove_migration_ptes(page, page, false);
			unlock_page(page);
		}
	}

	/* Drop an anon_vma reference if we took one */
	if (anon_vma)
		put_anon_vma(anon_vma);
out:
	return rc;
}

static int unmap_sma_page(struct page *page, int force,
		enum migrate_mode mode, enum migrate_reason reason)
{
	return __unmap_sma_page(page, force, mode, reason);
}

static int unmap_sma_pages(struct list_head *from,
		enum migrate_mode mode, enum migrate_reason reason,
		struct list_head *unmapped_head)
{
	int retry = 1;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;

	int rc = MIGRATEPAGE_SUCCESS;

	for (pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {
			cond_resched();

			rc = unmap_sma_page(page, pass > 2, mode, reason);

			switch(rc) {
				case -EAGAIN:
					retry++;
					if (pass == 10)
						pr_err("%s:-EAGAIN pass = %d, retry = %d, pfn = %lx, mapcount = %d\n",
								__func__, pass, retry, page_to_pfn(page),
								page_mapcount(page));
					break;
				case MIGRATEPAGE_SUCCESS:
					nr_succeeded++;
					/* Move page from *from* to *unmapped_head* */
					list_del(&page->lru);
					list_add(&page->lru, unmapped_head);
					break;
				default:
					pr_err("%s:%d Invalid rc: %d, succeed: %d, retry: %d, page: %lx\n",
							__func__, __LINE__, rc, nr_succeeded,
							retry, page_to_pfn(page));
					break;
			}
		}
	}

	return rc;
}

static int __move_sma_page(struct page *page, struct page *newpage,
		enum migrate_mode mode)
{
	int rc = -EAGAIN;
	struct anon_vma *anon_vma = NULL;

	/*
	 * Block others from accessing the new page when we get around to
	 * establishing additional references. We are usually the only one
	 * holding a reference to newpage at this point. We used to have a BUG
	 * here if trylock_page(newpage) fails, but would like to allow for
	 * cases where there might be a race with the previous use of newpage.
	 * This is much like races on refcount of oldpage: just don't BUG().
	 */
	if (unlikely(!trylock_page(newpage))) {
		pr_err("%s:%d ERROR: failed to lock newpage %lx\n",
				__func__, __LINE__, page_to_pfn(newpage));
		goto failed;
	}

	if (!page_mapped(page)) {
		rc = move_to_new_page(newpage, page, mode);
		if (page_count(newpage) != 2) {
			pr_err("%s:%d ERROR newpage refcount: %d\n",
					__func__, __LINE__, page_count(newpage));
		}
	} else {
		pr_err("%s:%d ERROR: page %lx mapping: %px, mapcount: %d\n",
				__func__, __LINE__, page_to_pfn(page), page->mapping,
				page_mapcount(page));
		goto failed;
	}

	/* page must be mapped in *__unmap_sma_page* after *try_to_unmap* */
	if (rc == MIGRATEPAGE_SUCCESS)
		remove_migration_ptes(page, newpage, false);
	unlock_page(newpage);
	if (rc != MIGRATEPAGE_SUCCESS)
		goto failed;

	/* Drop an anon_vma reference if we took one */
	anon_vma = page_get_anon_vma(page);
	if (anon_vma) {
		/* We get this anon_vma in *__unmap_sma_page* & previous line */
		put_anon_vma(anon_vma);
	}
	unlock_page(page);
failed:

	return rc;
}

static int move_sma_page(new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		struct page *page, enum migrate_mode mode,
		enum migrate_reason reason)
{
	int rc = MIGRATEPAGE_SUCCESS;
	struct page *newpage;

	newpage = get_new_page(page, private);
	if (!newpage) {
		pr_err("%s:%d ERROR no newpage, old page refcount: %d\n",
				__func__, __LINE__, page_count(page));
		return -ENOMEM;
	}

	rc = __move_sma_page(page, newpage, mode);
	if (rc == MIGRATEPAGE_SUCCESS)
		set_page_owner_migrate_reason(newpage, reason);

	/*
	 * If migration is successful, releases reference grabbed during
	 * isolation. Otherwise, restore the page to right list unless
	 * we want to retry.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		/*
		 * SMA will handle this old page, do NOT put_page here.
		 * *reason* must be MR_MEMORY_COMPACTION.
		 */
		if (!newpage->is_sec_mem) {
			pr_err("%s:%d migrate to *non-secure* page! pfn %lx, count: %d:%d\n",
					__func__, __LINE__, page_to_pfn(page),
					page_count(page), page_mapcount(page));
			BUG();
		}
	} else if (rc == -EAGAIN) {
		if (put_new_page)
			put_new_page(newpage, private);
		else
			put_page(newpage);
	} else {
		pr_err("%s:%d ERROR: invalid rc: %d, PFN %lx, count: %d\n",
				__func__, __LINE__, rc, page_to_pfn(page), page_count(page));
	}

	return rc;
}

static int move_sma_pages(new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		struct list_head *unmapped_head, enum migrate_mode mode,
		enum migrate_reason reason, struct list_head *moved_head)
{
	int retry = 1;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;

	int rc = MIGRATEPAGE_SUCCESS;

	for (pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, unmapped_head, lru) {
			cond_resched();

			rc = move_sma_page(get_new_page, put_new_page, private,
					page, mode, reason);
			switch(rc) {
				case -EAGAIN:
					retry++;
					break;
				case MIGRATEPAGE_SUCCESS:
					nr_succeeded++;
					/* Move page from *unmapped_head* to *moved_head* */
					list_del(&page->lru);
					list_add(&page->lru, moved_head);
					break;
				default:
					pr_err("%s:%d Invalid rc: %d, succeed: %d, retry: %d, page: %lx\n",
							__func__, __LINE__, rc, nr_succeeded,
							retry, page_to_pfn(page));
					break;
			}
		}
	}

	return rc;
}

#include <linux/kvm_host.h>
#include <asm/kvm_host.h>
#include <linux/sma.h>
#include <linux/sort.h>

bool is_migrating = false;
uint32_t migrate_sec_vm_id = 0;
uint32_t nr_migrate_pages = 0;
/* 8M == 2048 pages */
uint64_t migrate_ipns[2048] = {0};


/*
 * A batch version of *migrate_pages* for SMA, *from* is sorted by PFN.
 * Only support CMA 4K pages now.
 */
int migrate_sma_pages(struct list_head *from, new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		enum migrate_mode mode, enum migrate_reason reason)
{
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;
	struct list_head unmapped_head, moved_head;
	struct page *head_page = list_first_entry(from, struct page, lru);
	unsigned long src_base_pfn = page_to_pfn(head_page);
	struct sec_mem_cache *dst_cache = (struct sec_mem_cache *)private;
	unsigned long dst_base_pfn = dst_cache->base_pfn;
	kvm_smc_req_t* smc_req;
	INIT_LIST_HEAD(&unmapped_head);
	INIT_LIST_HEAD(&moved_head);

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	/* Init variables to record IPA range, used in *handle_hva_to_gpa* */
	migrate_sec_vm_id = 0;
	nr_migrate_pages = 0;
	is_migrating = true;

	rc = unmap_sma_pages(from, mode, reason, &unmapped_head);
	if (rc != 0) {
		pr_err("%s:%d failed to unmap %d sma pages\n",
				__func__, __LINE__, rc);
		goto out;
	}
	is_migrating = false;

	smc_req = get_smc_req_region(smp_processor_id());
	smc_req->sec_vm_id = migrate_sec_vm_id;
	smc_req->req_type = REQ_KVM_TO_S_VISOR_REMAP_IPA;
	smc_req->remap_ipa.src_start_pfn = src_base_pfn;
	smc_req->remap_ipa.dst_start_pfn = dst_base_pfn;
	smc_req->remap_ipa.nr_pages = (8 << (20 - 12));
	memcpy(smc_req->remap_ipa.ipn_list, migrate_ipns, sizeof(migrate_ipns));
	local_irq_disable();
	asm volatile("smc #0x18\n");
	local_irq_enable();

	/* S-visor has copied secure memory, change mode to SYNC_NO_COPY */
	mode = MIGRATE_SYNC_NO_COPY;
	rc = move_sma_pages(get_new_page, put_new_page,
			private, &unmapped_head, mode, reason, &moved_head);
	if (rc != 0) {
		pr_err("%s:%d failed to move %d sma pages\n",
				__func__, __LINE__, rc);
		goto out;
	}

out:
	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}
