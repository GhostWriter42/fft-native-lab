/* addr_test.c -- can a 32-bit Windows process hold the native port's fixed address regions?
 * Build (i686 MinGW-w64): gcc -m32 -O1 -Wl,--large-address-aware -o addr_test.exe addr_test.c
 * The native build needs (see lockstep.c / thread_window.h): the PS1 RAM at 0x80000000 (2 MiB), the scratchpad at 0x1f800000, the thread and main stacks at
 * 0x80400000..0x80a00000, the trampoline area at 0x10000000. All must be readable, writable and executable (trampolines live inside the RAM image). */
#include <windows.h>
#include <stdio.h>

static int try_region(const char* what, unsigned addr, unsigned len) {
    void* p = VirtualAlloc((void*)(ULONG_PTR)addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    MEMORY_BASIC_INFORMATION mbi;
    if (p == (void*)(ULONG_PTR)addr) {
        volatile unsigned char* q = (volatile unsigned char*)p;
        q[0] = 0xc3; q[len - 1] = 1;                                              /* touch both ends */
        printf("  %-28s 0x%08x + 0x%07x  OK\n", what, addr, len);
        return 1;
    }
    VirtualQuery((void*)(ULONG_PTR)addr, &mbi, sizeof mbi);
    printf("  %-28s 0x%08x + 0x%07x  FAILED (error %lu; the address is %s, allocation base 0x%08lx)\n", what, addr, len, GetLastError(),
           mbi.State == MEM_FREE ? "free" : mbi.State == MEM_RESERVE ? "reserved" : "in use", (unsigned long)(ULONG_PTR)mbi.AllocationBase);
    return 0;
}

int main(void) {
    SYSTEM_INFO si;
    int ok = 1;
    GetSystemInfo(&si);
    printf("highest user address 0x%08lx, allocation granularity 0x%lx\n", (unsigned long)(ULONG_PTR)si.lpMaximumApplicationAddress, si.dwAllocationGranularity);
    ok &= try_region("PS1 RAM", 0x80000000u, 0x200000u);
    ok &= try_region("scratchpad", 0x1f800000u, 0x10000u);
    ok &= try_region("thread + main stacks", 0x80400000u, 0x600000u);
    ok &= try_region("trampolines (THUNK_BASE)", 0x10000000u, 0x100000u);
    {   /* and code placed in the RAM image runs: a `ret` at 0x80000000 */
        void (*f)(void) = (void (*)(void))(ULONG_PTR)0x80000000u;
        if (ok) { f(); printf("  executing code at 0x80000000: OK\n"); }
    }
    printf(ok ? "all regions available\n" : "SOME REGIONS ARE NOT AVAILABLE\n");
    return ok ? 0 : 1;
}
