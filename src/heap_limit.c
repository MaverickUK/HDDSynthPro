// The heap's top: __HeapLimit, not __StackLimit (heap_limit.h).
//
// The SDK's _sbrk (pico_clib_interface, weak) grows the heap up to
// __StackLimit, the end of RAM. Since 0.35.0-dev.5 the DMA rings sit between
// __HeapLimit and there, so a large enough malloc (the folder disk's list,
// or Print::printf's buffer for a line over 63 characters) could be given
// ring memory, which the DMA overwrites. This one stops at __HeapLimit, so
// such a malloc fails instead (the folder disk tries a smaller list).
//
// C, and its static named heap_end as the SDK's is, so the variable keeps the
// SDK's name (heap_end.0) and so its place in .bss: .bss is sorted by name,
// and another name moved objects to other RAM banks (docs/speed-log.md).
#include "heap_limit.h"

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>

extern char end;          // the heap's start (the linker script's __end__)
extern char __HeapLimit;  // its end (scripts/core1_layout.py)

void* _sbrk(int incr) {
    static char* heap_end;
    if (heap_end == 0) heap_end = &end;
    char* prev = heap_end;
    if (incr > 0 && (uintptr_t)(&__HeapLimit - heap_end) < (uintptr_t)incr) {
        errno = ENOMEM;
        return (void*)-1;
    }
    heap_end += incr;
    return prev;
}

size_t heapFree(void) {
    char* brk = (char*)sbrk(0);
    struct mallinfo m = mallinfo();
    return (size_t)(&__HeapLimit - brk) + m.fordblks;
}
