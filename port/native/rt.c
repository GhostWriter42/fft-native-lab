/* Minimal runtime for -ffreestanding -nostdlib builds (the compiler may emit calls to these for struct copies).
 * Compile with -fno-tree-loop-distribute-patterns so the loops are not turned back into calls to themselves.
 *
 * With -DPC_SCHEME (PS1-address scheme, see gen_symbols.py --functions) the names that also exist in the game's own libc
 * (memset, memmove) are bound to their PS1 addresses by the symbol script, so the runtime defines them as native_<name>
 * (reached through the trampoline); memcpy/memcmp are not in the game's yaml and keep their plain names. */
void* memcpy(void* d, const void* s, unsigned int n) { unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s; while (n--) *dd++ = *ss++; return d; }
int memcmp(const void* a, const void* b, unsigned int n) {
    const unsigned char* x = (const unsigned char*)a; const unsigned char* y = (const unsigned char*)b;
    while (n--) { if (*x != *y) return *x < *y ? -1 : 1; x++; y++; }
    return 0;
}
#ifdef PC_SCHEME
#define MEMMOVE native_memmove
#define MEMSET native_memset
#define WEAK __attribute__((weak))    /* a whole-program build links the game's own (or bios_rt.c's) definitions instead */
#else
#define MEMMOVE memmove
#define MEMSET memset
#define WEAK
#endif
WEAK void* MEMMOVE(void* d, const void* s, unsigned int n) {
    unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
    if (dd < ss) while (n--) *dd++ = *ss++; else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
WEAK void* MEMSET(void* d, int c, unsigned int n) { unsigned char* dd = (unsigned char*)d; while (n--) *dd++ = (unsigned char)c; return d; }
