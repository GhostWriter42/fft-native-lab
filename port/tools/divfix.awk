# divfix.awk -- give integer division the MIPS R3000 semantics for a zero (or -1) divisor.
#
# Reads x86 (AT&T) assembly from `gcc -m32 -S` on stdin and expands every idivl/divl (a division by a NON-constant divisor; GCC
# turns division by a constant into shifts and multiplies) into a guarded sequence:
#   divisor == 0   MIPS: lo (quotient) = 1 if the dividend is negative else -1, hi (remainder) = the dividend; unsigned: lo = 0xffffffff, hi = dividend
#   divisor == -1  (signed) quotient = -dividend (INT_MIN / -1 = INT_MIN), remainder = 0     [x86 would raise #DE]
#   otherwise      the original instruction
# x86 raises #DE (SIGFPE) on a zero divisor and ARM returns 0; on the PlayStation the result is defined and the game (and any
# mod) can observe it, so a native build must reproduce it. Registers used: only eax/edx (the division's own operands).
{
    if (match($0, /^[ \t]+(idivl|divl)[ \t]+/)) {
        insn = $1
        op = $0
        sub(/^[ \t]+(idivl|divl)[ \t]+/, "", op)
        sub(/[ \t]*#.*$/, "", op)
        n++
        print "\tcmpl\t$0, " op
        print "\tje\t.Lpsd_z" n
        if (insn == "idivl") {
            print "\tcmpl\t$-1, " op
            print "\tje\t.Lpsd_m" n
        }
        print "\t" insn "\t" op
        print "\tjmp\t.Lpsd_d" n
        print ".Lpsd_z" n ":"
        if (insn == "idivl") {
            print "\tmovl\t%eax, %edx"
            print "\tsarl\t$31, %eax"
            print "\tnotl\t%eax"
            print "\torl\t$1, %eax"
            print "\tjmp\t.Lpsd_d" n
            print ".Lpsd_m" n ":"
            print "\tnegl\t%eax"
            print "\txorl\t%edx, %edx"
        } else {
            print "\tmovl\t%eax, %edx"
            print "\tmovl\t$-1, %eax"
        }
        print ".Lpsd_d" n ":"
        next
    }
    print
}
