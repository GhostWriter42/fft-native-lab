/* Minimal runtime for -ffreestanding -nostdlib builds (the compiler may emit calls to these for struct copies).
 * Compile with -fno-tree-loop-distribute-patterns so the loops are not turned back into calls to themselves. */
void* memcpy(void* d, const void* s, unsigned int n) { unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s; while (n--) *dd++ = *ss++; return d; }
void* memmove(void* d, const void* s, unsigned int n) {
    unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
    if (dd < ss) while (n--) *dd++ = *ss++; else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void* memset(void* d, int c, unsigned int n) { unsigned char* dd = (unsigned char*)d; while (n--) *dd++ = (unsigned char)c; return d; }
int memcmp(const void* a, const void* b, unsigned int n) {
    const unsigned char* x = (const unsigned char*)a; const unsigned char* y = (const unsigned char*)b;
    while (n--) { if (*x != *y) return *x < *y ? -1 : 1; x++; y++; }
    return 0;
}
