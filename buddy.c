#include "buddy.h"
#include <stdint.h>
#include <string.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define PAGE_SIZE (1024 * 4)
#define MIN_RANK 1

// Global state
static void *memory_base = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Simple bitmap for allocation tracking
static unsigned char *alloc_bitmap = NULL;

// Free lists for buddy algorithm
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static FreeBlock *free_lists[MAX_RANK + 1];

// Helper functions
static int is_valid_rank(int rank) {
    return (rank >= MIN_RANK && rank <= MAX_RANK);
}

static void *get_buddy(void *block, int rank) {
    uintptr_t addr = (uintptr_t)block;
    uintptr_t block_size = PAGE_SIZE * (1 << (rank - 1));
    return (void *)(addr ^ block_size);
}

static int get_page_index(void *addr) {
    if (memory_base == NULL) return -1;
    return ((uintptr_t)addr - (uintptr_t)memory_base) / PAGE_SIZE;
}

static void *get_addr_from_index(int index) {
    if (memory_base == NULL) return NULL;
    return (void *)((uintptr_t)memory_base + index * PAGE_SIZE);
}

static int is_valid_address(void *p) {
    if (p == NULL || memory_base == NULL) return 0;
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)memory_base;
    
    if ((addr - base) % PAGE_SIZE != 0) return 0;
    if (addr < base || addr >= base + total_pages * PAGE_SIZE) return 0;
    
    return 1;
}

static void mark_allocated(int start_page, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        int idx = start_page + i;
        alloc_bitmap[idx / 8] |= (1 << (idx % 8));
    }
}

static void mark_free(int start_page, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        int idx = start_page + i;
        alloc_bitmap[idx / 8] &= ~(1 << (idx % 8));
    }
}

static int is_allocated(int page_idx) {
    if (page_idx < 0 || page_idx >= total_pages) return 0;
    return (alloc_bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) {
        return OK;
    }
    
    memory_base = p;
    total_pages = pgcount;
    
    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    
    // Calculate maximum rank
    max_rank = 0;
    for (int r = 1; r <= MAX_RANK; r++) {
        int block_size = 1 << (r - 1);
        if (block_size <= total_pages) {
            max_rank = r;
        } else {
            break;
        }
    }
    
    // Allocate bitmap (use first part of memory)
    int bitmap_bytes = (total_pages + 7) / 8;
    int bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Use first few pages for bitmap
    alloc_bitmap = (unsigned char *)memory_base;
    
    // Clear bitmap (all pages initially free)
    memset(alloc_bitmap, 0, bitmap_pages * PAGE_SIZE);
    
    // Move memory_base past bitmap for actual page storage
    memory_base = (void *)((uintptr_t)memory_base + bitmap_pages * PAGE_SIZE);
    total_pages -= bitmap_pages;
    
    // Recalculate max_rank with reduced pages
    max_rank = 0;
    for (int r = 1; r <= MAX_RANK; r++) {
        int block_size = 1 << (r - 1);
        if (block_size <= total_pages) {
            max_rank = r;
        } else {
            break;
        }
    }
    
    // Initialize with one large free block
    if (max_rank > 0) {
        FreeBlock *block = (FreeBlock *)memory_base;
        block->next = free_lists[max_rank];
        free_lists[max_rank] = block;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }
    
    if (rank > max_rank) {
        return ERR_PTR(-ENOSPC);
    }
    
    // Find free block
    int current_rank = rank;
    while (current_rank <= max_rank && free_lists[current_rank] == NULL) {
        current_rank++;
    }
    
    if (current_rank > max_rank) {
        return ERR_PTR(-ENOSPC);
    }
    
    // Split if needed
    while (current_rank > rank) {
        FreeBlock *block = free_lists[current_rank];
        free_lists[current_rank] = block->next;
        
        int smaller_rank = current_rank - 1;
        FreeBlock *buddy1 = block;
        FreeBlock *buddy2 = get_buddy(block, current_rank);
        
        // Add both to free list
        buddy2->next = free_lists[smaller_rank];
        free_lists[smaller_rank] = buddy2;
        
        buddy1->next = free_lists[smaller_rank];
        free_lists[smaller_rank] = buddy1;
        
        current_rank--;
    }
    
    // Allocate
    FreeBlock *block = free_lists[rank];
    free_lists[rank] = block->next;
    
    int start_page = get_page_index(block);
    int block_size = 1 << (rank - 1);
    mark_allocated(start_page, block_size);
    
    return block;
}

int return_pages(void *p) {
    if (!is_valid_address(p)) {
        return -EINVAL;
    }
    
    int page_idx = get_page_index(p);
    
    if (!is_allocated(page_idx)) {
        return -EINVAL;
    }
    
    // Find block size
    int rank = 1;
    int block_size = 1;
    
    for (int r = max_rank; r >= 1; r--) {
        int test_size = 1 << (r - 1);
        if (page_idx % test_size == 0) {
            int all_allocated = 1;
            for (int i = 0; i < test_size; i++) {
                if (!is_allocated(page_idx + i)) {
                    all_allocated = 0;
                    break;
                }
            }
            if (all_allocated) {
                rank = r;
                block_size = test_size;
                break;
            }
        }
    }
    
    // Mark as free
    mark_free(page_idx, block_size);
    
    // Coalesce with buddy
    void *current_block = p;
    int current_rank = rank;
    
    while (current_rank <= max_rank) {
        void *buddy = get_buddy(current_block, current_rank);
        
        if (!is_valid_address(buddy)) {
            break;
        }
        
        int buddy_page = get_page_index(buddy);
        int buddy_size = 1 << (current_rank - 1);
        
        // Check if buddy is completely free
        int buddy_free = 1;
        for (int i = 0; i < buddy_size; i++) {
            if (is_allocated(buddy_page + i)) {
                buddy_free = 0;
                break;
            }
        }
        
        if (!buddy_free) {
            break;
        }
        
        // Check if buddy is in free list
        FreeBlock *prev = NULL;
        FreeBlock *curr = free_lists[current_rank];
        int found = 0;
        
        while (curr != NULL) {
            if (curr == buddy) {
                found = 1;
                if (prev == NULL) {
                    free_lists[current_rank] = curr->next;
                } else {
                    prev->next = curr->next;
                }
                break;
            }
            prev = curr;
            curr = curr->next;
        }
        
        if (!found) {
            break;
        }
        
        // Merge
        if ((uintptr_t)current_block > (uintptr_t)buddy) {
            current_block = buddy;
        }
        
        current_rank++;
    }
    
    // Add to free list
    FreeBlock *block = (FreeBlock *)current_block;
    block->next = free_lists[current_rank];
    free_lists[current_rank] = block;
    
    return OK;
}

int query_ranks(void *p) {
    if (!is_valid_address(p)) {
        return -EINVAL;
    }
    
    int page_idx = get_page_index(p);
    
    if (is_allocated(page_idx)) {
        // Find block rank
        for (int r = max_rank; r >= 1; r--) {
            int block_size = 1 << (r - 1);
            if (page_idx % block_size == 0) {
                int all_allocated = 1;
                for (int i = 0; i < block_size; i++) {
                    if (!is_allocated(page_idx + i)) {
                        all_allocated = 0;
                        break;
                    }
                }
                if (all_allocated) {
                    return r;
                }
            }
        }
        return 1;
    }
    
    // Free page: find largest free block
    for (int r = max_rank; r >= 1; r--) {
        int block_size = 1 << (r - 1);
        int block_start = (page_idx / block_size) * block_size;
        
        // Check free list
        FreeBlock *curr = free_lists[r];
        while (curr != NULL) {
            if (get_page_index(curr) == block_start) {
                return r;
            }
            curr = curr->next;
        }
    }
    
    return 1;
}

int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }
    
    if (rank > max_rank) {
        return 0;
    }
    
    int count = 0;
    FreeBlock *block = free_lists[rank];
    while (block != NULL) {
        count++;
        block = block->next;
    }
    
    return count;
}