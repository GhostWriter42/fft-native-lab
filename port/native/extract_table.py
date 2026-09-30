"""Extract a byte table from a disc file into a C initializer: extract_table.py FILE LOAD_ADDR SYMBOL_ADDR LENGTH OUT.inc"""
import sys
path, load, addr, length, out = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4]), sys.argv[5]
data = open(path, 'rb').read()
off = addr - load
chunk = data[off:off + length]
assert len(chunk) == length, 'table runs past end of file'
open(out, 'w').write(', '.join(f'0x{b:02x}' for b in chunk) + '\n')
print(f'{path}: {length} bytes at file offset 0x{off:x} (addr 0x{addr:08x}) -> {out}: {list(chunk)}')
