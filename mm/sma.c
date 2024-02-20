/*
 * Secure Memory Allocator
 *
 * An allocator based on CMA.
 */

#include <linux/cma.h>
#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/migrate.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/list_sort.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/dma-contiguous.h>
#include <linux/printk.h>
#include <linux/gfp.h>
#include <asm/atomic.h>
#include <asm/bitops.h>
#include <linux/kvm_host.h>

#include <linux/sma.h>
#include <linux/list.h>
#include "cma.h"
#include "internal.h"

/* Utils */

unsigned long migrate_times = 0;
unsigned long migrate_cycles = 0;
unsigned long topup_times = 0;
unsigned long topup_cycles = 0;
unsigned long topup_nr_pages = 0;
EXPORT_SYMBOL(topup_nr_pages);

struct sma global_sma;

static int sec_mem_cache_cmp(void *priv,
		struct list_head *a, struct list_head *b) {
	struct sec_mem_cache *smc_a =
		container_of(a, struct sec_mem_cache, node_for_pool);
	struct sec_mem_cache *smc_b =
		container_of(b, struct sec_mem_cache, node_for_pool);

	BUG_ON(smc_a->base_pfn == smc_b->base_pfn);

	return (smc_a->base_pfn < smc_b->base_pfn) ? -1 : 1;
}

static inline unsigned long avail_pages_after_compact(struct cma_pool *pool) {
	struct cma *cma = pool->cma;
	return (cma->base_pfn + cma->count) -
		(pool->top_pfn - pool->nr_free_cache * SMA_CACHE_PAGES);
}

/* Initialization */

static struct sec_vm_info __dummy_svi;
static struct sec_vm_info *dummy_svi = &__dummy_svi;

void __init sec_mem_init_pools(phys_addr_t selected_size,
		phys_addr_t selected_base, phys_addr_t limit, bool fixed) {
	struct cma_pool *pool = NULL;
	int ret;

	pr_info("%s:%d selected_size: %llx, selected_base: %llx\n",
			__func__, __LINE__, selected_size, selected_base);

	pool = &global_sma.pools[DEFAULT_POOL];
	ret = dma_contiguous_reserve_area(selected_size, selected_base,
			0, &pool->cma, fixed);
	if (ret != 0) {
		pr_err("%s:%d Failed to reserve memory for SMA, ret = %d\n",
				__func__, __LINE__, ret);
	}
	pool->top_pfn = pool->cma->base_pfn;
	INIT_LIST_HEAD(&pool->used_cache_list);
	pool->nr_free_cache = 0;
	INIT_LIST_HEAD(&pool->free_cache_list);
	mutex_init(&pool->pool_lock);

	INIT_LIST_HEAD(&dummy_svi->inactive_cache_list);
	mutex_init(&dummy_svi->vm_lock);
}

static inline void reset_page_states(struct page *page)
{
	init_page_count(page);
	get_page(page);
	page_mapcount_reset(page);
	page->mapping = NULL;
	page->index = 0;
	page->flags &= ~PAGE_FLAGS_CHECK_AT_FREE;
}


/**
 * sec_mem_topup_cache - topup secure memory cache for a secure VM
 * @owner_vm: the secure VM for which cache is allocated
 * @sec_pool_type: the memory type of cma_pool from which cache is allocated
 *
 * Policy: 1) use free_cache_list, 2) use cma_alloc. The list & owner of
 * cache need to be updated.
 *
 * The memory type of *owner_vm* can be different from *sec_pool_type*.
 */
struct sec_mem_cache *sec_mem_topup_cache(
		struct sec_vm_info *owner_vm, enum sec_pool_type sec_pool_type) {
	struct cma_pool *target_pool = &global_sma.pools[sec_pool_type];
	struct list_head *used_head;
	struct sec_mem_cache *ret = NULL;
	struct page *cache_pages = NULL;

	BUG_ON(!target_pool->cma);

	used_head = &target_pool->used_cache_list;

	/* If no free cache, allocate SMA_CACHE_PAGES pages, always print failure */
	cache_pages = cma_alloc(target_pool->cma, SMA_CACHE_PAGES,
			SMA_CACHE_PG_ORDER, false);
	if (!cache_pages) {
		mutex_unlock(&target_pool->pool_lock);
		return NULL;
	}

	ret = kmalloc(sizeof(struct sec_mem_cache), GFP_KERNEL);
	ret->base_pfn = page_to_pfn(cache_pages);
	/* Update allocated memory range of target CMA */
	if (target_pool->top_pfn != ret->base_pfn) {
		kfree(ret);
		cma_release(target_pool->cma, cache_pages, SMA_CACHE_PAGES);
		mutex_unlock(&target_pool->pool_lock);
		return NULL;
	}
	target_pool->top_pfn += SMA_CACHE_PAGES;
	ret->bitmap = kzalloc(SMA_CACHE_BITMAP_SIZE, GFP_KERNEL);
	if (!ret->bitmap) {
		BUG();
	}

	ret->sec_pool_type = sec_pool_type;
	/* Add node to used_cache_list */
	list_add(&ret->node_for_pool, used_head);

	ret->owner_vm = owner_vm;
	/* Set as *active_cache* of *owner_vm* */
	owner_vm->active_cache = ret;
	ret->is_active = true;

	/* Init *cache_lock* */
	mutex_init(&ret->cache_lock);

	return ret;
}

/* Try to allocate from local cache or secure memory pool. */
struct page *sec_mem_alloc_page_local(struct sec_vm_info *owner_vm) {
	struct cma_pool *target_pool;
	struct sec_mem_cache *current_cache;
	unsigned long bitmap_no, pfn;
	struct page *ret = NULL;

	mutex_lock(&owner_vm->vm_lock);
	target_pool = &global_sma.pools[owner_vm->sec_pool_type];
	mutex_lock(&target_pool->pool_lock);
	current_cache = owner_vm->active_cache;

	if (!current_cache) {
		current_cache = sec_mem_topup_cache(owner_vm, owner_vm->sec_pool_type);
		if (!current_cache) {
			mutex_unlock(&target_pool->pool_lock);
			mutex_unlock(&owner_vm->vm_lock);
			return ret;
		}
	}

	mutex_lock(&current_cache->cache_lock);
	bitmap_no = find_next_zero_bit(current_cache->bitmap, SMA_CACHE_PAGES, 0);
	BUG_ON(bitmap_no >= SMA_CACHE_PAGES);

	bitmap_set(current_cache->bitmap, bitmap_no, 1);
	pfn = current_cache->base_pfn + bitmap_no;

	if (bitmap_full(current_cache->bitmap, SMA_CACHE_PAGES)) {

		/* Add the fullfilled cache to *inactive_cache_list* */
		list_add(&current_cache->node_for_vm, &owner_vm->inactive_cache_list);
		owner_vm->active_cache = NULL;
		current_cache->is_active = false;
	}
	mutex_unlock(&current_cache->cache_lock);

	mutex_unlock(&target_pool->pool_lock);
	mutex_unlock(&owner_vm->vm_lock);
	ret = pfn_to_page(pfn);
	get_page(ret);
	ret->is_sec_mem = true;
	return ret;
}

/* Secure Memory Free */

void sec_mem_free_page(struct sec_vm_info *owner_vm, struct page *page) {
	/* Find the cache according to pfn, clear the bit in bitmap */
	struct sec_mem_cache *current_cache;
	unsigned long pfn = page_to_pfn(page);
	unsigned long cache_base_pfn = pfn & ~SMA_CACHE_PG_MASK;
	unsigned long bitmap_no = pfn - cache_base_pfn;
	bool from_inactive = false;

	mutex_lock(&owner_vm->vm_lock);
	current_cache = owner_vm->active_cache;

	/* Page is not in active cache */
	if (!current_cache || cache_base_pfn != current_cache->base_pfn) {
		struct sec_mem_cache *smc_it;
		current_cache = NULL;
		/* Traverse each inactive cache */
		list_for_each_entry(smc_it, &owner_vm->inactive_cache_list,
				node_for_vm) {
			if (cache_base_pfn == smc_it->base_pfn) {
				current_cache = smc_it;
				from_inactive = true;
				break;
			}
		}
	}
	/* Panic if page not found in current VM */
	if (!current_cache) {
		pr_err("%s:%d Failed to free: pfn = %lx, cache_base_pfn = %lx\n",
				__func__, __LINE__, pfn, cache_base_pfn);
		mutex_unlock(&owner_vm->vm_lock);
		return;
	}
	BUG_ON(!current_cache);

	mutex_lock(&current_cache->cache_lock);

	if (!test_bit(bitmap_no, current_cache->bitmap)) {
		pr_err("%s:%d ERROR pfn %lx already freed in this cache, refcount = %d\n",
				__func__, __LINE__, page_to_pfn(page), page_count(page));
		mutex_unlock(&current_cache->cache_lock);
		mutex_unlock(&owner_vm->vm_lock);
		return;
	}

	if (page_mapcount(page))
		pr_err("%s:%d ERROR pfn %lx mapcount = %d, index = 0x%lx\n",
				__func__, __LINE__, page_to_pfn(page), page_mapcount(page), page->index);

	reset_page_states(page);
	put_page(page);
	bitmap_clear(current_cache->bitmap, bitmap_no, 1);
	if (bitmap_empty(current_cache->bitmap, SMA_CACHE_PAGES)) {
		/*
		 * This cache is free now, 1) remove it from used_cache_list,
		 * 2) cma_release if it is the last cache, o.w. 3) add to
		 * free_cache_list of target_pool & add nr_free_cache.
		 */
		struct cma_pool *target_pool =
			&global_sma.pools[current_cache->sec_pool_type];
		struct list_head *node = &current_cache->node_for_pool;

		/*
		 * Remove cache from inactive_cache_list if necessary,
		 * o.w. set active_cache to NULL
		 */
		if (from_inactive)
			list_del(&current_cache->node_for_vm);
		else {
			owner_vm->active_cache = NULL;
			current_cache->is_active = false;
		}

		mutex_lock(&target_pool->pool_lock);
		/* Remove cache from used_cache_list */
		list_del(node);

		list_add(node, &target_pool->free_cache_list);
		target_pool->nr_free_cache++;

		mutex_unlock(&target_pool->pool_lock);
	}
	mutex_unlock(&current_cache->cache_lock);
	mutex_unlock(&owner_vm->vm_lock);
}

/* Secure Memory Compaction */

static struct page *sec_mem_get_migrate_dst(struct page *page,
		unsigned long private) {
	struct sec_mem_cache *dst_cache = (struct sec_mem_cache *)private;
	unsigned long dst_base_pfn = dst_cache->base_pfn;
	unsigned long page_offset = page_to_pfn(page) & SMA_CACHE_PG_MASK;
	struct page *dst_page = pfn_to_page(dst_base_pfn + page_offset);

	dst_page->is_sec_mem = true;
	get_page(dst_page);
	if (page_count(dst_page) != 2) {
		pr_err("%s: ERROR dst pfn %lx refcount = %d, mapcount = %d\n",
				__func__, page_to_pfn(dst_page), page_count(dst_page),
				page_mapcount(dst_page));
	}

	/*
	 * We cannot set src_page->is_sec_mem to false now, subsquent
	 * migration operations (e.g., __migrate_entry_wait) may check this field.
	 */
	if (page_count(page) != 2) {
		pr_err("%s: ERROR src pfn %lx refcount = %d, mapcount = %d\n",
				__func__, page_to_pfn(page), page_count(page),
				page_mapcount(page));
	}

	return dst_page;
}

static void sec_mem_migrate_failure_callback(
		struct page *page, unsigned long private) {
	pr_err("%s: failed to migrate to pfn %lx, refcount = %d, mapcount = %d\n",
			__func__, page_to_pfn(page), page_count(page),
			page_mapcount(page));
}

inline static void update_s_visor_top(uint64_t top_pfn)
{
	kvm_smc_req_t *smc_req;
	smc_req = get_smc_req_region(smp_processor_id());
	smc_req->top_pfn = top_pfn;
	smc_req->req_type = REQ_KVM_TO_S_VISOR_UPDATE_TOP;
	local_irq_disable();
	asm volatile("smc 0x18\n\t");
	local_irq_enable();
}


int sec_mem_compact_pool(enum sec_pool_type target_type) {
	struct cma_pool *target_pool;
	struct list_head *used_head, *free_head;
	struct sec_mem_cache *src_cache, *dst_cache;

	target_pool = &global_sma.pools[target_type];

	mutex_lock(&target_pool->pool_lock);
	/* Prepare migrate src (largest PFN) to dst (smallest PFN) */
	used_head = &target_pool->used_cache_list;
	free_head = &target_pool->free_cache_list;
	list_sort(NULL, used_head, sec_mem_cache_cmp);
	list_sort(NULL, free_head, sec_mem_cache_cmp);

	while (!list_empty(free_head) && !list_empty(used_head)) {
		struct list_head src_page_list;
		unsigned long pfn_it;
		int ret;
		bool release_res;

		src_cache = list_last_entry(used_head, struct sec_mem_cache, node_for_pool);
		dst_cache = list_first_entry(free_head, struct sec_mem_cache, node_for_pool);
		/* If used_pfn < free_pfn, no need to migrate, release the free caches */
		if (src_cache->base_pfn < dst_cache->base_pfn)
			break;
		/*
		 * Remove src_cache from used_cache_list,
		 * remove dst_cache from free_cache_list
		 */
		list_del(&src_cache->node_for_pool);
		list_del(&dst_cache->node_for_pool);
		target_pool->nr_free_cache--;

		INIT_LIST_HEAD(&src_page_list);
		for (pfn_it = src_cache->base_pfn;
				pfn_it < (src_cache->base_pfn + SMA_CACHE_PAGES); pfn_it++) {
			struct page *page = pfn_to_page(pfn_it);
			if (PageLRU(page)) {
				pr_err("%s:%d ERROR CMA %lx should NOT be LRU\n",
						__func__, __LINE__, pfn_it);
			}
			if (page->is_sec_mem && page_count(page) != 1)
				list_add_tail(&page->lru, &src_page_list);
		}
		ret = migrate_sma_pages(&src_page_list,
				sec_mem_get_migrate_dst, sec_mem_migrate_failure_callback,
				(unsigned long)dst_cache, MIGRATE_SYNC, MR_COMPACTION);
		if (ret != 0)
			pr_err("%s:\t migrate_pages ret = %d (nr_pages not migrated/error code)\n",
					__func__, ret);
		for (pfn_it = src_cache->base_pfn;
				pfn_it < (src_cache->base_pfn + SMA_CACHE_PAGES); pfn_it++) {
			struct page *page = pfn_to_page(pfn_it);
			reset_page_states(page);
			put_page(page);
		}

		/* Copy the bitmap, add dst_cache to used_cache_list*/
		memcpy(dst_cache->bitmap, src_cache->bitmap, SMA_CACHE_BITMAP_SIZE);
		list_add(&dst_cache->node_for_pool, &target_pool->used_cache_list);

		/*
		 * Copy the owner of src_cache to dst_cache.
		 */
		dst_cache->owner_vm = src_cache->owner_vm;
		dst_cache->is_active = src_cache->is_active;
		if (src_cache->is_active) {
			src_cache->is_active = false;
			dst_cache->owner_vm->active_cache = dst_cache;
			dst_cache->is_active = true;
		} else {
			list_del(&src_cache->node_for_vm);
			list_add(&dst_cache->node_for_vm,
					&dst_cache->owner_vm->inactive_cache_list);
		}

		update_s_visor_top(target_pool->top_pfn - SMA_CACHE_PAGES);
		release_res = cma_release(target_pool->cma,
				pfn_to_page(src_cache->base_pfn), SMA_CACHE_PAGES);
		target_pool->top_pfn -= SMA_CACHE_PAGES;

		if (!release_res) {
			BUG();
		}
		/* Free source cache */
		kfree(src_cache);
	}
	/* Migration complete, release rest of free caches if any */
	printk("free list is empty ? %d", list_empty(free_head));
	if (!list_empty(free_head)) {
		struct sec_mem_cache *smc_it, *next_it;
		/* Traverse each free cache */
		list_for_each_entry_safe(smc_it, next_it, free_head, node_for_pool) {
			bool release_res;
			list_del(&smc_it->node_for_pool);
			update_s_visor_top(target_pool->top_pfn - SMA_CACHE_PAGES);
			release_res = cma_release(target_pool->cma,
					pfn_to_page(smc_it->base_pfn), SMA_CACHE_PAGES);
			target_pool->top_pfn -= SMA_CACHE_PAGES;
			if (!release_res) {
				BUG();
			}
			/* Free this cache */
			kfree(smc_it);
		}
	}
	mutex_unlock(&target_pool->pool_lock);
	return 0;
}
EXPORT_SYMBOL(sec_mem_compact_pool);

