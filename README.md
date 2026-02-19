Varun Iyer (vmi14) Ahmed Elshenawy (ae508)
Test Case Plan: The project implements a dynamic memory allocator, which is backed by a 4096 byte heap. The test plan accounts for leaks, memory safety and error handling. 
1) Error Handling (memtest.c) --> This ensures that the myfree() function correctly detects and handles invalid pointers as required. Test cases to be accounted for are primarily freeing a pointer that is not allocated by the memory allocator, double freeing the same pointer, and freeing a pointer that points into the middle of an allocated block.
2) Leak Detection (leaktest.c) --> This verifies that memory leaks are detected and reported accurately before terminating the program. We had created an entire test program, leaktest, that accounts for leak detection, where the allocator registers a leak checker via atexit() that would scan the heap. When program exits, leak reporting happens automatically
3) Performance and Stress Testing (memgrind.c) --> Evaluates allocator performance through various cases and patterns. Five workloads were implemented and executed 50 times each and the average run time is printed for each of them:
         i) Workload A: Repeated malloc(1) following immediately with freeing the byte of memory.
        ii) Workload B: Repeat allocation of small pointers before freeing up memory.
       iii) Workload C: Randomize mix of allocations and memory freeing.
        iv) Workload D: Alternating small and big bytes (8 and 64 bytes) for memory allocation.
         v) Workload E: Uniform middle-sized allocations freed in non-sequential order
4) Boundary and Edge case testing --> Additional test cases testing (malloc(0) returns NULL, free(NULL) returns nothing, minimum chunk size enforced ins at least 16 bytes, and 8-byte payload alignment)
