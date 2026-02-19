#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define MEMLENGTH 4096
#define HEADER_SIZE 8            
#define MIN_CHUNK_SIZE 16
#define ALLOC_BIT ((size_t)1)


//fixed heap storage
static union {
    char bytes[MEMLENGTH];
    double not_used; 
} heap;

static int heap_initialized = 0;

//rounds n up to next multiple of 8 for 8 byte alignment 
static size_t round_up_8(size_t n) {
    return (n + 7) & ~(size_t)7;
}

//Extract chunk size from header
static size_t get_chunk_size(void *hdr) {
    return (*(size_t *)hdr) & ~(size_t)0x7;
}

//Check allocation bit in header
static int is_allocated(void *hdr) {
    return ((*(size_t *)hdr) & ALLOC_BIT) != 0;
}

//Write header: store chunk size plus allocation bit if allocated.
static void set_header(void *hdr, size_t chunk_size, int allocated) {
    *(size_t *)hdr = chunk_size | (allocated ? ALLOC_BIT : 0);
}

//Leak checker registered with atexit().
static void leak_check(void) {
    size_t off = 0;
    size_t leaked_bytes = 0;
    size_t leaked_objs = 0;

    while (off < MEMLENGTH) {
        void *hdr = (void *)(heap.bytes + off);
        size_t csz = get_chunk_size(hdr);

         //Stop if header is corrupted or chunk would go out of bounds

        if (csz == 0 || off + csz > MEMLENGTH) {
            break;
        }

        //If chunk is still allocated at program exit, count it as a leak
        if (is_allocated(hdr)) {
            leaked_objs++;
            // payload capacity
            leaked_bytes += (csz >= HEADER_SIZE) ? (csz - HEADER_SIZE) : 0;
        }

        off += csz;
    }
    
    if (leaked_objs > 0) {
        fprintf(stderr, "mymalloc: %zu bytes leaked in %zu objects.\n",
                leaked_bytes, leaked_objs);
    }
}
//initializes heap on first call to mymalloc or myfree, also registers leak checker with atexit()
static void heap_init(void) {
    set_header((void *)heap.bytes, MEMLENGTH, 0);
    atexit(leak_check);
    heap_initialized = 1;
}
//prints error message if memory allocation fails
static void malloc_fail(size_t req, char *file, int line) {
    fprintf(stderr, "malloc: Unable to allocate %zu bytes (%s:%d)\n", req, file, line);
}
//prints error message if memory freeing fails and exits program
static void free_fail(char *file, int line) {
    fprintf(stderr, "free: Inappropriate pointer (%s:%d)\n", file, line);
    exit(2);
}

void *mymalloc(size_t size, char *file, int line) {
    //Initialize heap on first call
    if (!heap_initialized) heap_init();

    //Zero size allocation returns NULL
    if (size == 0) {
        return NULL;
    }
    // Calculate total chunk size needed (payload + header), round payload up to 8 bytes, and enforce minimum chunk size
    size_t payload = round_up_8(size);
    size_t need = payload + HEADER_SIZE;
    if (need < MIN_CHUNK_SIZE) need = MIN_CHUNK_SIZE;

    // Traverse heap to find first free chunk
    size_t off = 0;
    while (off < MEMLENGTH) {
        void *hdr = (void *)(heap.bytes + off);
        size_t csz = get_chunk_size(hdr);

        // Stop if header is corrupted or chunk would go out of bounds
        if (csz == 0 || off + csz > MEMLENGTH) {
            break;
        }

        //Found free chunk large enough 
        if (!is_allocated(hdr) && csz >= need) {
            size_t rem = csz - need;
            if (rem >= MIN_CHUNK_SIZE) {
                set_header(hdr, need, 1);
                void *next_hdr = (void *)(heap.bytes + off + need);
                set_header(next_hdr, rem, 0);
            } else {
                set_header(hdr, csz, 1);
            }

            return (void *)(heap.bytes + off + HEADER_SIZE);
        }

        off += csz;
    }
    //no suitable chunk found, allocation fails
    malloc_fail(size, file, line);
    return NULL;
}

void myfree(void *ptr, char *file, int line) {
    //Initialize heap on first call if required
    if (!heap_initialized) heap_init();
    //freeing NULL pointer, which returns nothing
    if (ptr == NULL) {
       
        return;
    }

    char *p = (char *)ptr;
    char *base = heap.bytes;
    char *end = heap.bytes + MEMLENGTH;
    // pointer must be within heap bounds and point to a valid payload start 
    if (p < base + HEADER_SIZE || p >= end) {
        free_fail(file, line);
    }

    //walks heap to find exact chunk whose payload starts at pointer

    size_t off = 0;
    void *prev_hdr = NULL;
    size_t prev_off = 0;

    while (off < MEMLENGTH) {
        void *hdr = (void *)(heap.bytes + off);
        size_t csz = get_chunk_size(hdr);
        // if metadata is corrupted, memory freeing fails
        if (csz == 0 || off + csz > MEMLENGTH) {
            free_fail(file, line);
        }

        // Check if this chunk's payload starts at the pointer we're trying to free
        void *payload_ptr = (void *)(heap.bytes + off + HEADER_SIZE);
        // If the pointer matches the chunk's payload start
        if (payload_ptr == ptr) {
            // freeing an already freed chunk, leads to an error
            if (!is_allocated(hdr)) { 
                free_fail(file, line);
            }
            //marks chunk as free
            set_header(hdr, csz, 0);
            // performs coalescing with next chunk if it's free and valid

            size_t next_off = off + csz; // Merge current chunk size + next chunk size into current header
            // Check if next chunk is within bounds and valid before attempting to coalesce
            if (next_off < MEMLENGTH) {
                void *next_hdr = (void *)(heap.bytes + next_off);
                size_t next_sz = get_chunk_size(next_hdr);

                if (next_sz == 0 || next_off + next_sz > MEMLENGTH) {
                    free_fail(file, line);
                }

                if (!is_allocated(next_hdr)) {
                    set_header(hdr, csz + next_sz, 0);
                    csz += next_sz; //updates the coalesced size of the chunk so the previously coalesced chunk uses the merged size
                }
            }

            // Coalesce with previous chunk if previous exists and is free
            // Merge into previous header (so the combined chunk begins at prev_hdr)
            if (prev_hdr != NULL && !is_allocated(prev_hdr)) {
                size_t prev_sz = get_chunk_size(prev_hdr);
                set_header(prev_hdr, prev_sz + csz, 0);
            }

            return;
        }

        // Tracks previous chunk header in case for further coalescing
        prev_hdr = hdr;
        prev_off = off;
        (void)prev_off; 

        off += csz;
    }

    // if pointer isn't found in heap, the pointer is invalid
    free_fail(file, line);
}
