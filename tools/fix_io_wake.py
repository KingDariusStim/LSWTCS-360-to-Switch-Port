"""Wake the game's file loader as soon as its read-ahead block is ready (kernel_stubs.cpp, LSWTCS_IOWAKE).

Usage: python tools/fix_io_wake.py "Convert 360/LSWTCS/output"

Why (2026-10-11): the game streams files through an I/O thread (sub_824FD9A8): per block it reads a 12-byte LZ2K
header and the compressed data into a 32 KB slot and sets the flag [0x83284760]. The loader (sub_825048C8, and the
other reader sub_824F6F10) clears the flag, signals the I/O thread, decodes the previous block, then polls the flag
with Sleep(1). On the 360 the disc read took longer than the poll anyway. Here the read is done long before, yet each
block still costs at least one host sleep (~1.9 ms on Windows): during the PC hub load the loader slept 4.8 s of every
5 s ([DELAY] lr=82504960, ~500 sleeps/s) and the process was 90 % idle. The same wait paces the Switch.

Edits (marker LSW_IOWAKE): the two poll sites call lsw_io_wait(base) instead of Sleep(1) (it releases the GIL and waits
on a host condition variable until the flag is set, 2 ms at most; returns false when LSWTCS_IOWAKE=0, which keeps
the guest Sleep), and the two places that set the flag call lsw_io_flag_set() right after the store.
Idempotent; asserts each anchor matches exactly once. Keeps CRLF/LF as found.
"""
import os
import sys

DECL = ('extern "C" bool lsw_io_wait(uint8_t* base);   // LSW_IOWAKE: kernel_stubs.cpp\n'
        'extern "C" void lsw_io_flag_set(void);   // LSW_IOWAKE: kernel_stubs.cpp\n')


def poll(lr):
    old = ('\t// li r3,1\n'
           '\tctx.r3.s64 = 1;\n'
           '\t// bl 0x82533af0\n'
           f'\tctx.lr = 0x{lr};\n'
           '\tsub_82533AF0(ctx, base);\n'
           f'loc_{lr}:\n')
    new = ('\t// li r3,1\n'
           '\tctx.r3.s64 = 1;\n'
           '\t// bl 0x82533af0\n'
           f'\tctx.lr = 0x{lr};\n'
           '\tif (lsw_io_wait(base)) ctx.r3.u64 = 0;   // LSW_IOWAKE: wait for the read-ahead flag, not Sleep(1)\n'
           '\telse sub_82533AF0(ctx, base);\n'
           f'loc_{lr}:\n')
    return old, new


def setter(store):
    return store, store + '\tlsw_io_flag_set();   // LSW_IOWAKE: wake the loader waiting on [0x83284760]\n'


EDITS = {
    'ppc_recomp.417.cpp': [
        poll('82504960'),   # sub_825048C8: compressed-block loader
        setter('\t// stw r8,18272(r28)\n\tPPC_STORE_U32(r28.u32 + 18272, ctx.r8.u32);\n'),   # I/O thread sub_824FD9A8
    ],
    'ppc_recomp.415.cpp': [
        poll('824F6F30'),   # sub_824F6F10: the other reader
        setter('\t// stw r30,18272(r10)\n\tPPC_STORE_U32(ctx.r10.u32 + 18272, r30.u32);\n'),   # sub_824F6E38
    ],
}


def main(out_dir):
    for name, edits in EDITS.items():
        path = os.path.join(out_dir, name)
        raw = open(path, 'rb').read()
        crlf = b'\r\n' in raw
        s = raw.decode('utf-8', errors='surrogateescape').replace('\r\n', '\n')
        if 'LSW_IOWAKE' in s:
            print(f'{name}: already applied')
            continue
        for old, new in edits:
            n = s.count(old)
            assert n == 1, f'{name}: anchor found {n} times:\n{old}'
            s = s.replace(old, new)
        inc = '#include "ppc_recomp_shared.h"\n'
        assert s.count(inc) == 1, f'{name}: include anchor'
        s = s.replace(inc, inc + DECL)
        if crlf:
            s = s.replace('\n', '\r\n')
        open(path, 'wb').write(s.encode('utf-8', errors='surrogateescape'))
        print(f'{name}: patched')


if __name__ == '__main__':
    main(sys.argv[1])
