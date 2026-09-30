/* Replay of one fuzz trial with call tracing on BOTH machines (included by harness_fuzz.c when built with -DONLY_FN=<index>
 * -DONLY_TRIAL=<t> and the game compiled with -finstrument-functions; see fuzz.ps1 -Replay).
 *
 * The interpreter records every call target (jal/jalr/j) with a hash of all data RAM and the first three argument registers at
 * the moment the callee is ENTERED (after the jal's delay slot); the native side records every instrumented function entry with
 * the same hash. The two call sequences are printed side by side up to the first difference in callee or state, followed by the
 * RAM words that differ at that call: that names the function whose effect diverged, and the words it got wrong. */

static unsigned __attribute__((no_instrument_function)) hash_data(const unsigned char* ram_of_ps1_addr, int is_native) {
    unsigned h = 2166136261u, a;
    int r;
    for (r = 0; r < ndata; r++)
        for (a = data_lo[r]; a < data_hi[r]; a += 4) {
            unsigned w;
            if (ignored_word(a)) continue;
            w = is_native ? *(const unsigned*)a : *(const unsigned*)(ram_of_ps1_addr + (a & 0x1fffff));
            h = (h ^ w) * 16777619u;
        }
    return h;
}
static unsigned interp_hash[1024], interp_args[1024][3];
static unsigned char snap_ram[0x200000];
static int snap_at_raw = -1, native_snap_raw = -1;
static void __attribute__((no_instrument_function)) interp_call_hook(r3k_t* c, unsigned target) {
    unsigned i = c->call_n - 1;
    if (c->call_n <= 1024) {
        interp_hash[i] = hash_data(interp_ram, 0);
        interp_args[i][0] = c->r[4]; interp_args[i][1] = c->r[5]; interp_args[i][2] = c->r[6];
    }
    if ((int)i == snap_at_raw) { unsigned k; for (k = 0; k < 0x200000; k += 4) *(unsigned*)(snap_ram + k) = *(unsigned*)(interp_ram + k); }
}
void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void* fn, void* site) {
    if (shared && shared->trace_on && shared->trace_n < 1000) {
        if ((int)shared->trace_n == native_snap_raw) {                      /* RAM at this call against the interpreter's snapshot at the same call */
            unsigned a, n = 0;
            int r;
            for (r = 0; r < ndata; r++)
                for (a = data_lo[r]; a < data_hi[r]; a += 4) {
                    unsigned x = *(unsigned*)a, y = *(unsigned*)(snap_ram + (a & 0x1fffff));
                    if (x != y && !ignored_word(a)) { if (n < 40) { shared->addr[n] = a; shared->nat[n] = x; shared->orig[n] = y; } n++; }
                }
            shared->ndiff = n;
        }
        shared->trace_hash[shared->trace_n] = hash_data(0, 1);
        shared->trace[shared->trace_n++] = (unsigned)fn;
    }
}
void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void* fn, void* site) { }

static const char* name_of_ps1(unsigned a) {
    int i;
    for (i = 0; i < g_orig_func_count; i++) if (g_orig_funcs[i].addr == a) return g_orig_funcs[i].name;
    return 0;
}
static const char* name_of_native(unsigned a) {
    int i;
    for (i = 0; i < g_native_stub_count; i++) if ((unsigned)g_native_stubs[i].native == a) return name_of_ps1(g_native_stubs[i].ps1_addr);
    return 0;
}
static int ignored_name(const char* n) { return !n || streq(n, "rand") || streq(n, "srand"); }

static void replay_one(int fi, int t) {
    const struct fuzz_fn* f = &g_fuzz_fns[fi];
    unsigned args[10];
    const char* an[1024]; const char* bn[1024];
    unsigned ah[1024], bh[1024];
    int araw[1024], braw[1024];
    int na = 0, nb = 0, i, first = -1, rows, pass;
    static gte_state_t gte0;
    long pid, status = 0;
    out("replay of ["); outnum(fi); out("] "); out(f->name); out(" trial "); outnum(t); out("\n");
    for (pass = 0; pass < 2; pass++) {                                  /* pass 0: find the first divergence; pass 1: snapshot RAM there */
        unsigned n, q;
        void* d;
        setup_trial(fi, t, args);
        gte0 = g_gte;
        copy_ram_to_interp();
        cpu.io_reads = cpu.io_writes = 0; cpu.code_reads = cpu.code_writes = 0; cpu.wild = 0; cpu.sdk_hits = 0; cpu.div_zero = cpu.div_overflow = 0;
        cpu.rand_calls = 0; cpu.bios_rand_seed = g_rng; cpu.call_n = 0; cpu.pending_call = 0; cpu.trace_calls = 1; cpu.call_hook = interp_call_hook;
        for (q = 1; q < 32; q++) cpu.r[q] = 0;
        cpu.hi = cpu.lo = 0;
        n = 0x10000 / 4; d = interp_ram + 0x1f0000;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(0) : "memory");
        r3k_call(&cpu, f->addr, args, f->nargs, MAX_STEPS);
        cpu.trace_calls = 0; cpu.call_hook = 0;
        g_gte = gte0;
        if (pass == 0) {
            for (i = 0; i < (int)cpu.call_n; i++) {
                const char* nm = name_of_ps1(cpu.call_trace[i]);
                if (!ignored_name(nm)) { araw[na] = i; ah[na] = interp_hash[i]; an[na++] = nm; }
            }
        }
        shared->trace_n = 0; shared->trace_on = 0; shared->marker = 0; shared->ndiff = 0;
        g_bios_rand_seed = g_rng; g_bios_rand_calls = 0; g_stub_calls = 0;
        pid = sys3(2, 0, 0, 0);
        if (pid == 0) {
            unsigned a, cnt = 0;
            int rg;
            sys3(27, 5, 0, 0);
            shared->trace_on = 1;
            ((gen10_t)f->nat)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
            shared->trace_on = 0;
            if (pass == 0)
                for (rg = 0; rg < ndata; rg++)
                    for (a = data_lo[rg]; a < data_hi[rg]; a += 4) {
                        unsigned x = *(unsigned*)a, y = *(unsigned*)(interp_ram + (a & 0x1fffff));
                        if (x != y && !ignored_word(a)) { if (cnt < 40) { shared->addr[cnt] = a; shared->nat[cnt] = x; shared->orig[cnt] = y; } cnt++; }
                    }
            if (pass == 0) shared->ndiff = cnt;
            shared->marker = 0xd0d0d0d0u;
            sys3(1, 0, 0, 0);
        }
        sys4(114, pid, (long)&status, 0, 0);
        if (pass == 0) {
            int nd = (int)shared->ndiff;
            out("final RAM words that differ: "); outnum(nd); out("\n");
            for (i = 0; i < nd && i < 40; i++) { out("    "); label(shared->addr[i]); out("  native "); outhex(shared->nat[i]); out("  original "); outhex(shared->orig[i]); out("\n"); }
            if ((status & 0x7f) || shared->marker != 0xd0d0d0d0u) {
                out("native process ended abnormally (status "); outnum(status); out(")");
                if (shared->marker == 0xdeadfa17u) { out(": signal "); outnum(shared->fault_sig); out(" at eip "); outhex(shared->fault_eip); out(" address "); outhex(shared->fault_addr); }
                out("\n");
            }
            for (i = 1; i < (int)shared->trace_n; i++) {                /* [0] is the function itself */
                const char* nm = name_of_native(shared->trace[i]);
                if (!ignored_name(nm)) { braw[nb] = i; bh[nb] = shared->trace_hash[i]; bn[nb++] = nm; }
            }
            out("calls: original "); outnum(na); out(", native "); outnum(nb); out("\n");
            for (i = 0; i < na && i < nb; i++) if (an[i] != bn[i] || ah[i] != bh[i]) { first = i; break; }
            if (first < 0 && na != nb) first = na < nb ? na : nb;
            if (first < 0) { out("call sequences and states are identical at every call\n"); return; }
            rows = first + 8;
            for (i = first > 10 ? first - 10 : 0; i < rows && (i < na || i < nb); i++) {
                out(i == first ? ">>> " : "    "); outnum(i); out("  original: "); out(i < na ? an[i] : "(none)");
                if (i < na) { out("  state "); outhex(ah[i]); out("  args "); outhex(interp_args[araw[i]][0]); out(" "); outhex(interp_args[araw[i]][1]); out(" "); outhex(interp_args[araw[i]][2]); }
                out("\n        native:   "); out(i < nb ? bn[i] : "(none)"); if (i < nb) { out("  state "); outhex(bh[i]); } out("\n");
            }
            if (first >= na || first >= nb) return;
            snap_at_raw = araw[first]; native_snap_raw = braw[first];
        } else {
            out("RAM words that differ when call "); outnum(first); out(" ("); out(an[first]); out(") is entered: "); outnum(shared->ndiff); out("\n");
            for (i = 0; i < (int)shared->ndiff && i < 40; i++) { out("    "); label(shared->addr[i]); out("  native "); outhex(shared->nat[i]); out("  original "); outhex(shared->orig[i]); out("\n"); }
        }
    }
}
