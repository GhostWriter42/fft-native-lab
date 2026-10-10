/* win_sys.c -- the Linux system calls of the native driver (lockstep.c), on Win32.
 *
 * The driver is freestanding: it talks to the kernel through a handful of i386 Linux system calls (read, write, open, close, lseek, exit, the old mmap,
 * mprotect, rt_sigaction, alarm). On Windows, sys3() / sys4() land here and are carried out with Win32 calls, so the driver itself stays one source.
 *
 * Paths: the container mounts the host's folders at /disc, /disc.bin, /states, /shots, /run.cfg, /session ...; here the environment variable
 * FFT_MOUNTS maps the same prefixes to Windows paths ("/disc=C:\x\files;/disc.bin=C:\x\game.bin;/states=C:\x\states"), longest prefix first.
 *
 * State that must survive a machine-state load (snapshots restore the program's .data / .bss): the descriptor table and the signal handlers live in a
 * page of their own at a fixed address, outside the image, so a state written by another process cannot bring back its stale Windows handles.
 *
 * Stack: a Linux system call runs on the kernel's stack, so the memory below the caller's stack pointer is left alone. Win32 calls run on the caller's
 * stack and write their own (deep) frames there. The game relies on dead stack memory surviving (the name-entry screen keeps its GPU packet buffers
 * in its stack frame and leaves the globals pointing at them after it returns; the next frame draws them), so every call switches to a stack of its
 * own first (win_sys below), exactly as a system call would.
 *
 * Signals: one vectored exception handler turns access violations (SIGSEGV), illegal instructions (SIGILL), divisions by zero (SIGFPE) and single
 * steps (SIGTRAP) into calls of the handler the driver installed, with the parts of a Linux siginfo / ucontext it reads (the fault address at +12, the
 * registers at their i386 sigcontext offsets: ebp +44, eip +76, eflags +84), and writes eflags / eip back. */
#include <windows.h>

#define WS_BASE 0x2f000000u                                                     /* the fixed page (below the snapshot arena at 0x30000000) */
#define WS_FILES 32
typedef struct win_sys_state {
    HANDLE fd[WS_FILES];                                                        /* 0..2: stdin, stdout, stderr */
    void (*handler[32])(int, void*, void*);
    int veh_installed;
    char mounts[4096];
} win_sys_state_t;
#define WS ((win_sys_state_t*)WS_BASE)

static void ws_init(void) {
    static int done;                                                            /* (in .bss: a snapshot restores it as 1 only into a process that has initialised already) */
    DWORD n;
    if (done) return;
    if (!VirtualAlloc((void*)WS_BASE, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) ExitProcess(70);
    WS->fd[0] = GetStdHandle(STD_INPUT_HANDLE);
    WS->fd[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    WS->fd[2] = GetStdHandle(STD_ERROR_HANDLE);
    n = GetEnvironmentVariableA("FFT_MOUNTS", WS->mounts, sizeof WS->mounts);
    if (n >= sizeof WS->mounts) WS->mounts[0] = 0;
    done = 1;
}

/* "/states/slot1.state" -> the Windows path through FFT_MOUNTS (longest matching prefix); 0 = no mount covers it */
static int map_path(const char* p, char* out, int max) {
    const char* m = WS->mounts;
    int best = -1, blen = 0, i;
    const char* bval = 0;
    int bvlen = 0;
    while (*m) {
        const char* eq = m;
        const char* end;
        int klen, vlen;
        while (*eq && *eq != '=' && *eq != ';') eq++;
        if (*eq != '=') break;
        end = eq + 1;
        while (*end && *end != ';') end++;
        klen = (int)(eq - m); vlen = (int)(end - eq - 1);
        for (i = 0; i < klen && p[i] == m[i]; i++) {}
        if (i == klen && (p[klen] == 0 || p[klen] == '/') && klen > blen) { best = 1; blen = klen; bval = eq + 1; bvlen = vlen; }
        m = *end ? end + 1 : end;
    }
    if (best < 0 || bvlen + 1 >= max) return 0;
    for (i = 0; i < bvlen; i++) out[i] = bval[i];
    for (p += blen; *p && i + 1 < max; p++) out[i++] = *p == '/' ? '\\' : *p;
    out[i] = 0;
    return 1;
}

static long ws_open(const char* path, long flags) {
    char wp[MAX_PATH];
    DWORD access, disp;
    HANDLE h;
    int fd;
    if (!map_path(path, wp, sizeof wp)) return -2;                              /* ENOENT */
    switch (flags & 3) { case 0: access = GENERIC_READ; break; case 1: access = GENERIC_WRITE; break; default: access = GENERIC_READ | GENERIC_WRITE; }
    if (flags & 0x40) disp = (flags & 0x200) ? CREATE_ALWAYS : OPEN_ALWAYS;       /* O_CREAT (| O_TRUNC) */
    else disp = (flags & 0x200) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    h = CreateFileA(wp, access, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, disp, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return -2;
    for (fd = 3; fd < WS_FILES; fd++) if (!WS->fd[fd]) { WS->fd[fd] = h; return fd; }
    CloseHandle(h);
    return -24;                                                                 /* EMFILE */
}

/* Page zero. Retail code reads (rarely writes) console RAM at addresses 0..0xffff through NULL pointers; the Linux build maps a zero-filled page there,
 * Windows cannot map it. Each such access is emulated here as Linux would see it: a load gives 0, a store is dropped. Only the simple moves the compiler
 * emits for such accesses are decoded (mov / movzx / movsx loads, mov stores of a register or an immediate); anything else is left to the crash report.
 * Every site is reported once on stderr (it deserves a reviewed source patch, see native_patches.py). */
static unsigned g_null_sites[64], g_n_null_sites;
static int modrm_len(const unsigned char* m) {                                  /* bytes of ModRM + SIB + displacement; -1 = a register operand */
    unsigned mod = m[0] >> 6, rm = m[0] & 7, n = 1;
    if (mod == 3) return -1;
    if (rm == 4) { if (mod == 0 && (m[1] & 7) == 5) n += 4; n++; }
    else if (mod == 0 && rm == 5) n += 4;
    if (mod == 1) n += 1; else if (mod == 2) n += 4;
    return (int)n;
}
/* EFLAGS after `a - b` (cmp) or `a & b` (test) at operand size `bits` */
static void set_flags(CONTEXT* c, unsigned a, unsigned b, int bits, int is_sub) {
    unsigned mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1u, sign = 1u << (bits - 1), r, f = c->EFlags & ~0x8d5u, p, k;
    a &= mask; b &= mask;
    r = (is_sub ? a - b : a & b) & mask;
    if (is_sub && a < b) f |= 0x001;                                            /* CF */
    for (p = 1, k = 0; k < 8; k++) p ^= (r >> k) & 1u;
    if (p) f |= 0x004;                                                          /* PF */
    if (is_sub && ((a ^ b ^ r) & 0x10u)) f |= 0x010;                            /* AF */
    if (!r) f |= 0x040;                                                         /* ZF */
    if (r & sign) f |= 0x080;                                                   /* SF */
    if (is_sub && ((a ^ b) & (a ^ r) & sign)) f |= 0x800;                       /* OF */
    c->EFlags = f;
}
static int emulate_null_access(CONTEXT* c, int write) {
    const unsigned char* ip = (const unsigned char*)c->Eip;
    int prefix = 0, len, op16 = 0;
    DWORD* regs[8];
    regs[0] = &c->Eax; regs[1] = &c->Ecx; regs[2] = &c->Edx; regs[3] = &c->Ebx; regs[4] = &c->Esp; regs[5] = &c->Ebp; regs[6] = &c->Esi; regs[7] = &c->Edi;
    if (ip[0] == 0x66) { op16 = 1; prefix = 1; ip++; }
    if (!write) {
        if (ip[0] == 0x8b || ip[0] == 0x8a) {                                   /* mov r, [m] */
            unsigned r = (ip[1] >> 3) & 7;
            if ((len = modrm_len(ip + 1)) < 0) return 0;
            if (ip[0] == 0x8b) { if (op16) *regs[r] &= 0xffff0000u; else *regs[r] = 0; }
            else if (r < 4) *regs[r] &= 0xffffff00u; else *regs[r - 4] &= 0xffff00ffu;
            c->Eip += (DWORD)(prefix + 1 + len);
            return 1;
        }
        if (ip[0] == 0x80 || ip[0] == 0x81 || ip[0] == 0x83) {                 /* cmp [m], imm (group 1, /7): the memory operand reads as 0 */
            int bits = ip[0] == 0x80 ? 8 : op16 ? 16 : 32, ilen = ip[0] == 0x81 ? (op16 ? 2 : 4) : 1;
            unsigned imm;
            if (((ip[1] >> 3) & 7) != 7 || (len = modrm_len(ip + 1)) < 0) return 0;
            imm = ilen == 1 ? (unsigned)(int)(signed char)ip[1 + len] : ilen == 2 ? (unsigned)*(const unsigned short*)(ip + 1 + len) : *(const unsigned*)(ip + 1 + len);
            set_flags(c, 0, imm, bits, 1);
            c->Eip += (DWORD)(prefix + 1 + len + ilen);
            return 1;
        }
        if (ip[0] >= 0x38 && ip[0] <= 0x3b) {                                   /* cmp with a register: 38/39 [m] - r, 3a/3b r - [m] */
            unsigned r = (ip[1] >> 3) & 7, rv;
            int bits = (ip[0] & 1) ? (op16 ? 16 : 32) : 8;
            if ((len = modrm_len(ip + 1)) < 0) return 0;
            rv = bits == 8 ? (r < 4 ? *regs[r] : *regs[r - 4] >> 8) : *regs[r];
            if (ip[0] <= 0x39) set_flags(c, 0, rv, bits, 1); else set_flags(c, rv, 0, bits, 1);
            c->Eip += (DWORD)(prefix + 1 + len);
            return 1;
        }
        if (ip[0] == 0x84 || ip[0] == 0x85 || ((ip[0] == 0xf6 || ip[0] == 0xf7) && ((ip[1] >> 3) & 7) == 0)) {   /* test: the result is 0 */
            int bits = (ip[0] & 1) ? (op16 ? 16 : 32) : 8, ilen = ip[0] == 0xf6 ? 1 : ip[0] == 0xf7 ? (op16 ? 2 : 4) : 0;
            if ((len = modrm_len(ip + 1)) < 0) return 0;
            set_flags(c, 0, 0, bits, 0);
            c->Eip += (DWORD)(prefix + 1 + len + ilen);
            return 1;
        }
        if (ip[0] == 0x0f && (ip[1] == 0xb6 || ip[1] == 0xb7 || ip[1] == 0xbe || ip[1] == 0xbf)) {   /* movzx / movsx r, [m] */
            unsigned r = (ip[2] >> 3) & 7;
            if ((len = modrm_len(ip + 2)) < 0) return 0;
            if (op16) *regs[r] &= 0xffff0000u; else *regs[r] = 0;
            c->Eip += (DWORD)(prefix + 2 + len);
            return 1;
        }
        return 0;
    }
    if (ip[0] == 0x89 || ip[0] == 0x88) {                                       /* mov [m], r */
        if ((len = modrm_len(ip + 1)) < 0) return 0;
        c->Eip += (DWORD)(prefix + 1 + len);
        return 1;
    }
    if (ip[0] == 0xc7 || ip[0] == 0xc6) {                                       /* mov [m], imm */
        if ((len = modrm_len(ip + 1)) < 0) return 0;
        c->Eip += (DWORD)(prefix + 1 + len + (ip[0] == 0xc6 ? 1 : op16 ? 2 : 4));
        return 1;
    }
    return 0;
}

static LONG CALLBACK ws_veh(EXCEPTION_POINTERS* ep) {
    unsigned info[32] = {0}, uc[64] = {0};
    int sig;
    CONTEXT* c = ep->ContextRecord;
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        if (ep->ExceptionRecord->ExceptionInformation[1] < 0x10000u && emulate_null_access(c, ep->ExceptionRecord->ExceptionInformation[0] == 1)) {
            unsigned k, eip = (unsigned)ep->ExceptionRecord->ExceptionAddress;
            for (k = 0; k < g_n_null_sites && g_null_sites[k] != eip; k++) {}
            if (k == g_n_null_sites && k < 64) {
                static const char msg[] = "NULL-PAGE ACCESS by the native game (emulated as on Linux: a load gives 0) at @0x";
                char hex[11];
                DWORD w;
                int i;
                g_null_sites[g_n_null_sites++] = eip;
                for (i = 0; i < 8; i++) hex[i] = "0123456789abcdef"[(eip >> (4 * (7 - i))) & 15];
                hex[8] = '\n'; hex[9] = 0;
                WriteFile(WS->fd[2], msg, sizeof msg - 1, &w, 0); WriteFile(WS->fd[2], hex, 9, &w, 0);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        sig = 11; info[3] = (unsigned)ep->ExceptionRecord->ExceptionInformation[1]; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: sig = 4; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: sig = 8; break;
    case EXCEPTION_SINGLE_STEP: sig = 5; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!WS->handler[sig]) return EXCEPTION_CONTINUE_SEARCH;
    uc[11] = c->Ebp; uc[19] = c->Eip; uc[21] = c->EFlags;                        /* +44, +76, +84: i386 ucontext.uc_mcontext */
    WS->handler[sig](sig, info, uc);
    c->EFlags = uc[21]; c->Eip = uc[19];
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD prot_of(long prot) {
    switch (prot & 7) {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 3: return PAGE_READWRITE;
    case 5: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

long win_sys_on_own_stack(long n, long a, long b, long c, long d) {
    DWORD done;
    ws_init();
    switch (n) {
    case 1: ExitProcess((UINT)a); return 0;                                     /* exit */
    case 3:                                                                     /* read */
        if (a < 0 || a >= WS_FILES || !WS->fd[a]) return -9;
        if (!ReadFile(WS->fd[a], (void*)b, (DWORD)c, &done, 0)) return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -5;
        return (long)done;
    case 4:                                                                     /* write */
        if (a < 0 || a >= WS_FILES || !WS->fd[a]) return -9;
        if (!WriteFile(WS->fd[a], (const void*)b, (DWORD)c, &done, 0)) return -32;
        return (long)done;
    case 5: return ws_open((const char*)a, b);                                  /* open */
    case 6:                                                                     /* close */
        if (a < 3 || a >= WS_FILES || !WS->fd[a]) return -9;
        CloseHandle(WS->fd[a]); WS->fd[a] = 0;
        return 0;
    case 19: {                                                                  /* lseek */
        DWORD r;
        if (a < 0 || a >= WS_FILES || !WS->fd[a]) return -9;
        r = SetFilePointer(WS->fd[a], (LONG)b, 0, c == 0 ? FILE_BEGIN : c == 1 ? FILE_CURRENT : FILE_END);
        return r == INVALID_SET_FILE_POINTER ? -22 : (long)r;
    }
    case 27: return 0;                                                          /* alarm: the native watchdog is not available here */
    case 90: {                                                                  /* old mmap(struct { addr, len, prot, flags, fd, off }): only anonymous fixed mappings */
        const long* m = (const long*)a;
        void* p;
        if (!m[0]) return -12;                                                  /* page zero cannot be mapped on Windows */
        p = VirtualAlloc((void*)m[0], (SIZE_T)m[1], MEM_RESERVE | MEM_COMMIT, prot_of(m[2]));
        return p ? (long)p : -12;
    }
    case 125: {                                                                 /* mprotect */
        DWORD old;
        return VirtualProtect((void*)a, (SIZE_T)b, prot_of(c), &old) ? 0 : -22;
    }
    case 174: {                                                                 /* rt_sigaction(sig, const struct sigaction* act, old, size) */
        const void* const* act = (const void* const*)b;
        if (a <= 0 || a >= 32) return -22;
        if (act) WS->handler[a] = (void (*)(int, void*, void*))act[0];
        if (!WS->veh_installed) { AddVectoredExceptionHandler(1, ws_veh); WS->veh_installed = 1; }
        return 0;
    }
    }
    (void)d;
    return -38;                                                                 /* ENOSYS */
}

/* long win_sys(n, a, b, c, d): the same call, made on a stack of its own (see the top of this file). Not re-entrant (nothing calls it re-entrantly:
 * the vectored exception handler does not make system calls before it returns). */
static unsigned char ws_stack[256 * 1024] __attribute__((aligned(16)));
void* ws_stack_top = ws_stack + sizeof ws_stack - 16;
__asm__(".text\n"
        ".globl _win_sys\n"
        "_win_sys:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    movl _ws_stack_top, %esp\n"
        "    pushl %ebp\n"                                                     /* the caller's frame (its stack pointer is ebp) */
        "    pushl 24(%ebp)\n"                                                 /* d c b a n */
        "    pushl 20(%ebp)\n"
        "    pushl 16(%ebp)\n"
        "    pushl 12(%ebp)\n"
        "    pushl 8(%ebp)\n"
        "    call _win_sys_on_own_stack\n"
        "    addl $20, %esp\n"
        "    popl %esp\n"                                                      /* back to the caller's stack: esp = its ebp */
        "    popl %ebp\n"
        "    ret\n");
