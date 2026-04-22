#include "buddy.h"
#include <stdint.h>
#include <string.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define MIN_RANK 1
#define PAGE_SIZE 4096  // 4KB
#define MAX_PAGES 32768  // 128MB / 4KB

// Global variables
static void *memory_base = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Bitmap for tracking allocated pages (1 bit per 4KB page)
static unsigned char page_allocated[MAX_PAGES / 8 + 1];

// Free lists: for each rank, we store the number of free blocks
// and a simple stack of free block indices
#define MAX_BLOCKS_AT_RANK(r) (total_pages >> ((r) - 1))
static int free_count[MAX_RANK + 1];  // Number of free blocks at each rank
static int free_lists[MAX_RANK + 1][MAX_PAGES];  // Stack of free block indices at each rank

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_block_index(void *addr, int rank) {
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)memory_base;
    uintptr_t block_size = get_block_size(rank);
    return offset / block_size;
}

static void *get_block_address(int block_index, int rank) {
    uintptr_t offset = block_index * get_block_size(rank);
    return (void *)((uintptr_t)memory_base + offset);
}

static int get_rank_from_pages(int pages) {
    int rank = 0;
    while (pages > 0) {
        pages >>= 1;
        rank++;
    }
    return rank;
}

static void mark_pages_allocated(int start_page, int num_pages, int allocated) {
    for (int i = 0; i < num_pages; i++) {
        int page_idx = start_page + i;
        int byte_idx = page_idx / 8;
        int bit_idx = page_idx % 8;
        if (allocated) {
            page_allocated[byte_idx] |= (1 << bit_idx);
        } else {
            page_allocated[byte_idx] &= ~(1 << bit_idx);
        }
    }
}

static int are_pages_allocated(int start_page, int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        int page_idx = start_page + i;
        int byte_idx = page_idx / 8;
        int bit_idx = page_idx % 8;
        if (!(page_allocated[byte_idx] & (1 << bit_idx))) {
            return 0;
        }
    }
    return 1;
}

// Initialize the buddy system
int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) {
        return -EINVAL;
    }

    memory_base = p;
    total_pages = pgcount;

    // Calculate maximum rank that can fit in the memory
    max_rank = get_rank_from_pages(pgcount);
    if (max_rank > MAX_RANK) {
        max_rank = MAX_RANK;
    }

    // Clear allocation bitmap
    int bitmap_size = (pgcount + 7) / 8;
    memset(page_allocated, 0, bitmap_size);

    // Initialize free lists
    for (int r = MIN_RANK; r <= MAX_RANK; r++) {
        free_count[r] = 0;
    }

    // Add the entire memory as a free block at the maximum possible rank
    // Block index 0 at max_rank represents the entire memory
    free_lists[max_rank][free_count[max_rank]++] = 0;

    return OK;
}

// Allocate pages of specified rank
void *alloc_pages(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find the smallest rank >= requested rank that has free blocks
    int current_rank = rank;
    while (current_rank <= MAX_RANK && free_count[current_rank] == 0) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Split blocks if necessary
    while (current_rank > rank) {
        // Take a free block from current rank
        int block_index = free_lists[current_rank][--free_count[current_rank]];

        // Split into two buddies at rank-1
        // Left buddy has same index * 2
        // Right buddy has index * 2 + 1
        int left_index = block_index * 2;
        int right_index = block_index * 2 + 1;

        // Add both buddies to free list at rank-1 (right first, then left so left is on top)
        free_lists[current_rank - 1][free_count[current_rank - 1]++] = right_index;
        free_lists[current_rank - 1][free_count[current_rank - 1]++] = left_index;

        current_rank--;
    }

    // Allocate block at requested rank
    int block_index = free_lists[rank][--free_count[rank]];
    void *addr = get_block_address(block_index, rank);

    // Mark pages as allocated
    int block_size_in_pages = 1 << (rank - 1);
    int start_page = block_index * block_size_in_pages;
    mark_pages_allocated(start_page, block_size_in_pages, 1);

    return addr;
}

// Return pages to buddy system
int return_pages(void *p) {
    if (!p || (uintptr_t)p < (uintptr_t)memory_base ||
        (uintptr_t)p >= (uintptr_t)memory_base + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    // Find the rank and index of this block
    int rank = -1;
    int block_index = -1;

    for (int r = MIN_RANK; r <= max_rank; r++) {
        uintptr_t offset = (uintptr_t)p - (uintptr_t)memory_base;
        uintptr_t block_size = get_block_size(r);

        // Check if address is aligned to block size
        if (offset % block_size != 0) {
            continue;
        }

        block_index = offset / block_size;
        int block_size_in_pages = 1 << (r - 1);
        int start_page = block_index * block_size_in_pages;

        // Check if all pages in this block are allocated
        if (are_pages_allocated(start_page, block_size_in_pages)) {
            rank = r;
            break;
        }
    }

    if (rank == -1) {
        return -EINVAL;  // Block not allocated or already freed
    }

    // Mark pages as free
    int block_size_in_pages = 1 << (rank - 1);
    int start_page = block_index * block_size_in_pages;
    mark_pages_allocated(start_page, block_size_in_pages, 0);

    // Add block to free list and merge with buddy if possible
    while (1) {
        // Add current block to free list
        free_lists[rank][free_count[rank]++] = block_index;

        // Check if we can merge with buddy
        if (rank >= max_rank) {
            break;  // Cannot merge beyond max rank
        }

        // Buddy index: if block_index is even, buddy is block_index+1
        // if block_index is odd, buddy is block_index-1
        int buddy_index;
        if (block_index % 2 == 0) {
            buddy_index = block_index + 1;
        } else {
            buddy_index = block_index - 1;
        }

        // Check if buddy is free
        int buddy_free = 0;
        for (int i = 0; i < free_count[rank]; i++) {
            if (free_lists[rank][i] == buddy_index) {
                buddy_free = 1;
                // Remove buddy from free list
                for (int j = i; j < free_count[rank] - 1; j++) {
                    free_lists[rank][j] = free_lists[rank][j + 1];
                }
                free_count[rank]--;
                break;
            }
        }

        if (!buddy_free) {
            break;  // Cannot merge
        }

        // Remove current block from free list (it was just added)
        for (int i = 0; i < free_count[rank]; i++) {
            if (free_lists[rank][i] == block_index) {
                for (int j = i; j < free_count[rank] - 1; j++) {
                    free_lists[rank][j] = free_lists[rank][j + 1];
                }
                free_count[rank]--;
                break;
            }
        }

        // Merge: parent block index is block_index / 2 at rank+1
        block_index = block_index / 2;
        rank++;
    }

    return OK;
}

// Query rank of page at address
int query_ranks(void *p) {
    if (!p || (uintptr_t)p < (uintptr_t)memory_base ||
        (uintptr_t)p >= (uintptr_t)memory_base + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    uintptr_t offset = (uintptr_t)p - (uintptr_t)memory_base;
    int page_index = offset / PAGE_SIZE;

    // Check if page is allocated
    int byte_idx = page_index / 8;
    int bit_idx = page_index % 8;
    int is_allocated = (page_allocated[byte_idx] >> bit_idx) & 1;

    if (is_allocated) {
        // Find which allocated block contains this page
        for (int r = MIN_RANK; r <= max_rank; r++) {
            int block_size_in_pages = 1 << (r - 1);
            int block_index = page_index / block_size_in_pages;
            int start_page = block_index * block_size_in_pages;

            if (are_pages_allocated(start_page, block_size_in_pages)) {
                // Check if address is within this block
                uintptr_t block_start = block_index * get_block_size(r);
                uintptr_t block_end = block_start + get_block_size(r);
                if (offset >= block_start && offset < block_end) {
                    return r;
                }
            }
        }
        return -EINVAL;  // Should not happen
    } else {
        // Page is free, find maximum rank of free block containing it
        for (int r = max_rank; r >= MIN_RANK; r--) {
            int block_size_in_pages = 1 << (r - 1);
            int block_index = page_index / block_size_in_pages;
            int start_page = block_index * block_size_in_pages;

            // Check if this entire block is free
            int all_free = 1;
            for (int i = 0; i < block_size_in_pages; i++) {
                int idx = start_page + i;
                int byte_idx2 = idx / 8;
                int bit_idx2 = idx % 8;
                if ((page_allocated[byte_idx2] >> bit_idx2) & 1) {
                    all_free = 0;
                    break;
                }
            }

            if (all_free) {
                // Check if address is within this block
                uintptr_t block_start = block_index * get_block_size(r);
                uintptr_t block_end = block_start + get_block_size(r);
                if (offset >= block_start && offset < block_end) {
                    return r;
                }
            }
        }
        return -EINVAL;  // Should not happen
    }
}

// Query number of free blocks of specified rank
int query_page_counts(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK) {
        return -EINVAL;
    }

    return free_count[rank];
}