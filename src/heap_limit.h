#pragma once
// The heap ends at __HeapLimit, under the DMA rings at the top of RAM
// (.ram_rings, scripts/core1_layout.py): heap_limit.c replaces the SDK's
// _sbrk, which let malloc grow up to __StackLimit, over the rings.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bytes malloc can still hand out: never grown yet, up to __HeapLimit, plus
// what has been freed. rp2040.getFreeHeap() counts up to __StackLimit, so it
// counts the rings as free.
size_t heapFree(void);

#ifdef __cplusplus
}
#endif
