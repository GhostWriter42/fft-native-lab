/* Native versions of the BIOS services the SDK reaches through tail veneers (`li t2,0xa0; jr t2; li t1,N`), for a whole-program native
 * build. On the console these jump into the BIOS ROM; natively the veneer objects cannot be used (they "goto" address 0xa0), so the
 * services are written here with the SAME semantics as the interpreter's BIOS layer (r3000/r3000.c, bios_call), which keeps the two
 * machines identical: the C functions are called through the PS1-address trampolines exactly like every other game function.
 * Function numbers (A table): strcat 0x15, strcmp 0x17, strcpy 0x19, strlen 0x1b, bcopy 0x27, bzero 0x28, memcpy 0x2a, memset 0x2b,
 * memchr 0x2e, rand 0x2f, srand 0x30. The yaml names of the veneers are the SDK names (memcpy's is psyq_api_memcpy). */

unsigned g_bios_rand_seed = 1, g_bios_rand_calls;

int abs(int x) { return x < 0 ? -x : x; }                                     /* the game's C code calls it (-fno-builtin keeps it a real call) */

int native_rand(void) {
    g_bios_rand_calls++;
    g_bios_rand_seed = g_bios_rand_seed * 1103515245u + 12345u;
    return (int)((g_bios_rand_seed >> 16) & 0x7fffu);
}
void native_srand(unsigned seed) { g_bios_rand_seed = seed; }

void* native_bzero(void* dst, unsigned len) {
    unsigned char* p = (unsigned char*)dst;
    unsigned i;
    for (i = 0; i < len; i++) p[i] = 0;
    return dst;
}
void native_bcopy(const void* src, void* dst, unsigned len) {
    const unsigned char* s = (const unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;
    unsigned i;
    for (i = 0; i < len; i++) d[i] = s[i];
}
void* native_psyq_api_memcpy(void* dst, const void* src, unsigned len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned i;
    for (i = 0; i < len; i++) d[i] = s[i];
    return dst;
}
void* native_memset(void* dst, int value, unsigned len) {
    unsigned char* d = (unsigned char*)dst;
    unsigned i;
    for (i = 0; i < len; i++) d[i] = (unsigned char)value;
    return dst;
}
int native_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
char* native_strcpy(char* dst, const char* src) {
    unsigned i = 0;
    for (;; i++) { dst[i] = src[i]; if (!src[i]) break; }
    return dst;
}
char* native_strcat(char* dst, const char* src) {
    char* e = dst;
    unsigned i = 0;
    while (*e) e++;
    for (;; i++) { e[i] = src[i]; if (!src[i]) break; }
    return dst;
}
int native_strcmp(const char* a, const char* b) {                              /* difference of the first differing bytes (unsigned) */
    unsigned i;
    for (i = 0;; i++) {
        unsigned x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return (int)(x - y);
        if (!x) return 0;
    }
}
void* native_memchr(const void* s, int c, unsigned n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned i;
    for (i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void*)(p + i);
    return 0;
}
