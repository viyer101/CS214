#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "mymalloc.h"

#define ITERATIONS 50
#define SMALL_PTRS 120   
#define MIX_PTRS   90    
#define MID_PTRS   100   

//returns the starting time (in microseconds)
static long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

//Repeatedly allocates and frees one byte of memory, up to 120 times.
static void workload_A(void) {
    for (int i = 0; i < SMALL_PTRS; i++) {
        char *p = malloc(1); //calls mymalloc to allocate 1 byte
        if (p == NULL) return; //terminates if allocation fails
        free(p); //frees byte of memory
    }
}

//Allocates and frees 120 one byte blocks of memory 
static void workload_B(void) {
    char *ptrs[SMALL_PTRS];

    for (int i = 0; i < SMALL_PTRS; i++) { //small_ptrs is defined as global var to be 120
        ptrs[i] = malloc(1); //calls mymalloc to allocate 1 byte
        if (ptrs[i] == NULL) { //terminates if allocation fails
            for (int j = 0; j < i; j++) free(ptrs[j]); //for each pointer, it frees the allocated byte to avoid leakage
            return;
        }
    }
    //frees all object pointers in the array
    for (int i = 0; i < SMALL_PTRS; i++) {
        free(ptrs[i]);
    }
}

//Chooses to perform allocation until we've reached 120 successful total allocations
static void workload_C(void) {
    char *ptrs[SMALL_PTRS] = {0}; //initializes all pointers to NULL
    int total_allocs = 0; //total number of allocations 
    int curr_allocated = 0; //current number of allocations 

    //continues until we have successfully allocated 120 blocks of memory
    while (total_allocs < SMALL_PTRS) {
        int do_alloc = (rand() % 2); //decides whether to allocate or free memory with equal probability

        if (do_alloc) { //if we decide to allocate, we randomly select an index in the ptrs array
            int idx = rand() % SMALL_PTRS; //allocate into a random index in the ptrs array
            if (ptrs[idx] == NULL) { 
                ptrs[idx] = malloc(1);  //allocate 1 byte of memory if array is empty      
                if (ptrs[idx] != NULL) {
                    total_allocs++; //increase total allocations if allocation is successful
                    curr_allocated++; //increase current allocations
                } else if (curr_allocated > 0) { //if allocation failed
                    int j = rand() % SMALL_PTRS;
                    if (ptrs[j] != NULL) { free(ptrs[j]); ptrs[j] = NULL; curr_allocated--; } //frees one currently allocated block from memory
                }
            }
        } else {
            if (curr_allocated > 0) { //num of allocated blocks is greater than 0, we can free memory
                int idx = rand() % SMALL_PTRS;
                if (ptrs[idx] != NULL) {
                    free(ptrs[idx]);
                    ptrs[idx] = NULL;
                    curr_allocated--; //free up one currently allocated block from memory
                }
            }
        }
    }
    //free any remaining allocated blocks of memory to avoid leakage
    for (int i = 0; i < SMALL_PTRS; i++) {
        if (ptrs[i] != NULL) free(ptrs[i]);
    }
}

//Allocates alternating size blocks of memory (8 and 64 bytes)
static void workload_D(void) {
    char *ptrs[MIX_PTRS];

    for (int i = 0; i < MIX_PTRS; i++) {
        ptrs[i] = (i % 2 == 0) ? malloc(8) : malloc(64);
        if (ptrs[i] == NULL) {
            for (int j = 0; j < i; j++) free(ptrs[j]);
            return;
        }
    }
    //frees pointers in reverse order 
    for (int i = MIX_PTRS - 1; i >= 0; i--) {
        free(ptrs[i]);
    }
}

//allocates up to 100 blocks of memory, each of size 32 bytes (frees even bytes and then odd bytes)
static void workload_E(void) {
    char *ptrs[MID_PTRS];

    for (int i = 0; i < MID_PTRS; i++) {
        ptrs[i] = malloc(32); //allocates 32 bytes of memory for each pointer in the array
        if (ptrs[i] == NULL) {
            for (int j = 0; j < i; j++) free(ptrs[j]); //frees any previously allocated memory if allocation failed
            return;
        }
    }
    //frees even bytes
    for (int i = 0; i < MID_PTRS; i += 2) free(ptrs[i]);
    //frees odd bytes
    for (int i = 1; i < MID_PTRS; i += 2) free(ptrs[i]);
}

//Runs the given workload function for several iterations and prints average time taken per run in microseconds.
static void run_and_print(const char *name, void (*fn)(void)) {
    long start = get_time_us();
    for (int i = 0; i < ITERATIONS; i++) fn();
    long end = get_time_us();

    double avg_us = (end - start) / (double)ITERATIONS;
    printf("%s: %.2f microseconds\n", name, avg_us);
}

int main(void) {
    srand(0); //accounts for reproducibility of workload C

    printf("Memgrind results (average over %d runs):\n\n", ITERATIONS);

    run_and_print("Workload A", workload_A);
    run_and_print("Workload B", workload_B);
    run_and_print("Workload C", workload_C);
    run_and_print("Workload D", workload_D);
    run_and_print("Workload E", workload_E);

    return 0;
}