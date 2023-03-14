#include <Windows.h>

HANDLE heap;
HANDLE stdout;

void main();

void mainCRTStartup() {
    stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    heap = GetProcessHeap();
    main();
    ExitProcess(0);
}

void exit(int v) {
    ExitProcess(v);
}

size_t m_strlen(const char* s) {  // todo: optimize
    size_t i = 0;
    while (*s) {
        s++;
        i++;
    }
    return i;
}

void puts(const char* s) {
    WriteConsoleA(stdout, s, (DWORD) m_strlen(s), NULL, NULL);
}

void* ag_memmove(void* dst, void* src, size_t count) {  // todo: optimize
    char* d = (char*)dst;
    const char* s = (const char*)src;

    if (d < s) {
        while (count--) {
            *d++ = *s++;
        }
    } else {
        const char* lasts = s + (count - 1);
        char* lastd = d + (count - 1);
        while (count--) {
            *lastd-- = *lasts--;
        }
    }
    return dst;
}

void* ag_memcpy(void* dst, void* src, size_t count) {  // todo: optimize
    char* d = (char*)dst;
    const char* s = (const char*)src;
    while (count--) {
        *d++ = *s++;
    }
    return dst;
}

void ag_zero_mem(void* dst, size_t count) {  // todo: optimize
    for (count++; --count != 0;) { // this weirdo just to msvc not insert a `memset` here
        ((char*)dst)[count] = 0;
    }
}

void free(void* ptr) {
    HeapFree(heap, 0, ptr);
}

void* malloc(size_t size) {
    return HeapAlloc(heap, 0, size);
}
