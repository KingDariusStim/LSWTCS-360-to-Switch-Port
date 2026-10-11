"""Point the game's two minimum-frame-time clamps at lsw_ft_min() (kernel_stubs.cpp, LSWTCS_FTFLOOR).

Usage: python tools/fix_ftfloor_sites.py "Convert 360/LSWTCS/output"

Why (2026-10-11): the FTFLOOR boot patch used to overwrite the game's minimum frame time [0x830AA980]
(1/60 in the image, 1/30 after sub_8229D8B8) with 1 ms, so that 60 fps frames are not rounded up. But the game
also uses that word as its nominal frame length: the boot intro (sub_82264AA8) caps each frame at 3 x [0x830AA980],
which became 3 ms, so the intro's timers ran at 18 % of real time and the intro took ~55 s instead of ~10 s
(Eden and PC); sub_8242F728 / sub_8242FA28 turn frame counts into seconds with it. Now the word keeps the game's
value and only the two places that use it as a minimum clamp call lsw_ft_min(floor):
  - main loop sub_82215AA0 at 0x82216B90 (ft = clamp(timer, floor, [0x830AA9B8]))
  - boot intro sub_82264AA8 at 0x822650A8 (ft = clamp(timer, floor, 3 * floor); the cap keeps the real floor)
Idempotent (marker LSW_FTFLOOR); asserts each anchor matches exactly once. Keeps CRLF/LF as found.
"""
import os
import sys

DECL = 'extern "C" double lsw_ft_min(double floor);   // LSW_FTFLOOR: kernel_stubs.cpp\n'

EDITS = {
    'ppc_recomp.219.cpp': [(
        # main loop: ft = timer; if (ft < floor) ft = floor; else if (ft > max) ft = max
        '\t// lfs f13,-22144(r11)\n'
        '\ttemp.u32 = PPC_LOAD_U32(r11.u32 + -22144);\n'
        '\tctx.f13.f64 = double(temp.f32);\n'
        '\t// fcmpu cr6,f0,f13\n'
        '\tcr6.compare(f0.f64, ctx.f13.f64);\n'
        '\t// blt cr6,0x82216bac\n',
        '\t// lfs f13,-22144(r11)\n'
        '\ttemp.u32 = PPC_LOAD_U32(r11.u32 + -22144);\n'
        '\tctx.f13.f64 = double(temp.f32);\n'
        '\tctx.f13.f64 = lsw_ft_min(ctx.f13.f64);   // LSW_FTFLOOR: minimum clamp only (fix_ftfloor_sites.py)\n'
        '\t// fcmpu cr6,f0,f13\n'
        '\tcr6.compare(f0.f64, ctx.f13.f64);\n'
        '\t// blt cr6,0x82216bac\n')],
    'ppc_recomp.286.cpp': [(
        # boot intro: if (ft < floor) ft = floor; else if (ft > 3 * floor) ft = 3 * floor
        '\t// fcmpu cr6,f13,f0\n'
        '\tcr6.compare(ctx.f13.f64, f0.f64);\n'
        '\t// blt cr6,0x822650bc\n'
        '\tif (cr6.lt) goto loc_822650BC;\n'
        '\t// fmuls f0,f0,f22\n',
        '\t// fcmpu cr6,f13,f0\n'
        '\tcr6.compare(ctx.f13.f64, f0.f64);\n'
        '\t// blt cr6,0x822650bc\n'
        '\tif (ctx.f13.f64 < lsw_ft_min(f0.f64)) { f0.f64 = lsw_ft_min(f0.f64); goto loc_822650BC; }   '
        '// LSW_FTFLOOR: minimum clamp only; the 3x cap below keeps the real floor (fix_ftfloor_sites.py)\n'
        '\t// fmuls f0,f0,f22\n')],
}


def main(out_dir):
    for name, edits in EDITS.items():
        path = os.path.join(out_dir, name)
        raw = open(path, 'rb').read()
        crlf = b'\r\n' in raw
        s = raw.decode('utf-8', errors='surrogateescape').replace('\r\n', '\n')
        if 'LSW_FTFLOOR' in s:
            print(f'{name}: already applied')
            continue
        for old, new in edits:
            n = s.count(old)
            assert n == 1, f'{name}: anchor found {n} times'
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
