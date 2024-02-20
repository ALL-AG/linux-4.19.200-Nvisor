#ifndef __SMA_H__
#define __SMA_H__

#include <linux/init.h>
#include <linux/types.h>

#define MAX_SEC_VMS         (8)

/* Current cache size is 8M */
#define SMA_CACHE_ORDER     (23)
#define SMA_CACHE_PG_ORDER  (SMA_CACHE_ORDER - PAGE_SHIFT)
#define SMA_CACHE_SIZE      (1UL << SMA_CACHE_ORDER)
#define SMA_CACHE_MASK      (SMA_CACHE_SIZE - 1)
#define SMA_CACHE_PAGES     (SMA_CACHE_SIZE >> PAGE_SHIFT)
#define SMA_CACHE_PG_MASK   (SMA_CACHE_PAGES - 1)
#define SMA_CACHE_BITMAP_SIZE   (SMA_CACHE_PAGES / sizeof(char))

enum sec_pool_type {
	DEFAULT_POOL,
	/* Reserved for fallback secure memory pool */
	FALLBACK_POOL,
	SEC_POOL_TYPE_MAX,
};

/*
 * Caches are divided into used/free/release types.
 * Used includes all active & inactive caches used by VMs.
 * Free includes caches freed by VMs but not released to CMA yet.
 *
 * Compaction policy: migrate *last used* to *first free*.
 *
 * Migrate times: the number of free caches.
 *
 * Available memory after compaction: (cma->base_pfn + cma->count) -
 * (top_pfn - nr_free_cache * SMA_CACHE_PAGES)
 */
struct cma_pool {
	struct cma              *cma;

	/* [cma->base_pfn, top_pfn] is allocated in *cma* */
	unsigned long           top_pfn;

	struct list_head        used_cache_list;
	/* Free cache, not cma_release memory */
	unsigned long           nr_free_cache;
	struct list_head        free_cache_list;

	struct mutex            pool_lock;
};

struct sma {
	struct cma_pool         pools[SEC_POOL_TYPE_MAX];
};

struct sec_mem_cache {
	unsigned long           base_pfn;
	/* Record available pages */
	unsigned long           *bitmap;

	/* Current cache is allocated from *pools[sec_pool_type]* */
	enum sec_pool_type      sec_pool_type;
	/* Node in *pools[sec_pool_type].used/free_cache_list* */
	struct list_head        node_for_pool;

	/* Current cache belongs to *owner_vm* */
	struct sec_vm_info      *owner_vm;
	bool                    is_active;
	/* Node in *owner_vm->full_cache_list* */
	struct list_head        node_for_vm;

	struct mutex            cache_lock;
};

struct sec_vm_info {
	pid_t                   tgid;
	unsigned long           sec_hva_start;
	unsigned long           sec_hva_size;
	atomic_t                is_exiting;
	/*
	 * May use memory of type other than *sec_pool_type*
	 * if the pool of *sec_pool_type* is used up.
	 */
	enum sec_pool_type      sec_pool_type;
	struct sec_mem_cache    *active_cache;
	/* Currently we do NOT expect page free before VM shutdown */
	struct list_head        inactive_cache_list;

	struct mutex            vm_lock;
};

void sma_recycle_test(void);

void sec_mem_init_pools(phys_addr_t size, phys_addr_t base,
		phys_addr_t limit, bool fixed);

struct sec_mem_cache *sec_mem_topup_cache(
		struct sec_vm_info *owner_vm, enum sec_pool_type sec_pool_type);
struct page *sec_mem_alloc_page_local(struct sec_vm_info *owner_vm);
struct page *sec_mem_alloc_page_other(struct sec_vm_info *owner_vm);

void sec_mem_free_page(struct sec_vm_info *owner_vm, struct page *page);

int sec_mem_compact_pool(enum sec_pool_type target_type);
int sec_mem_compaction(unsigned long req_nr_pages);
#endif
