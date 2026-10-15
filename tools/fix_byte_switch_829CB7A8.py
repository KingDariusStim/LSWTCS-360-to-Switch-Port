"""Restore the event-type switch in sub_829CB7A8 (QNet connection dequeue: event -> message).

The switch is a byte-offset jump table XenonAnalyse does not detect:
    lbzx r0,r12,r11       ; r12 = 0x820CF088 (bytes 00 06 0B 10 15 1A 1F 23 27), r11 = type - 1
    rlwinm r0,r0,2,0,29   ; *4
    add r12,0x829CB818,r0 ; bctr
XenonRecomp emitted it as PPC_CALL_INDIRECT_FUNC(ctr) + return: the case code ran as a separate "function" on this
frame and returned to the caller with r1 still 0x90 lower, so every event (first seen: type 8 = new connection, the
first join packet on a host, Switch 2026-10-15) dropped the stack and re-ran the pump instead of handling it.
It is the only byte-table switch emitted this way in the image (scan: '// bctr' after '// lbzx' + indirect call).

Usage: python tools/fix_byte_switch_829CB7A8.py "Convert 360/LSWTCS/output"   (idempotent; marker LSW_BYTESWITCH_FIX)
"""
import os
import sys

FUNC = 'PPC_FUNC_IMPL(__imp__sub_829CB7A8) {'
# table byte -> case start (0x829CB818 + 4*byte); checked against image.bin 0x820CF088
CASES = [(0x00, 0x829CB818, 'bl 0x829cb390'), (0x06, 0x829CB830, 'bl 0x829c4928'), (0x0B, 0x829CB844, 'bl 0x829c4a20'),
         (0x10, 0x829CB858, 'bl 0x829c4ae8'), (0x15, 0x829CB86C, 'bl 0x829c8a28'), (0x1A, 0x829CB880, 'bl 0x829caad0'),
         (0x1F, 0x829CB894, 'bl 0x829cb6e0'), (0x23, 0x829CB8A4, 'bl 0x829cad10'), (0x27, 0x829CB8B4, 'bl 0x829cdfc8')]
OLD = ['\t// bctr ', '\tctx.r4 = r4;', '\tPPC_CALL_INDIRECT_FUNC(ctr.u32);', '\tr1 = ctx.r1;', '\tr3 = ctx.r3;',
       '\tr4 = ctx.r4;', '\tr5 = ctx.r5;', '\tr6 = ctx.r6;', '\treturn;']


def main(out_dir):
    path = os.path.join(out_dir, 'ppc_recomp.524.cpp')
    with open(path, encoding='utf-8', newline='') as fh:
        text = fh.read()
    if 'LSW_BYTESWITCH_FIX' in text:
        # v1 (2026-10-15 02:04) synced ctx.r4 only on the default path: CAD10 got a stale event pointer
        i = text.find('// LSW_BYTESWITCH_FIX: byte table')
        if text[i:text.find('switch (r0.u32)', i)].count('ctx.r4 = r4;') == 0:
            k = text.find('\n', i) + 1
            nl = '\r\n' if text[k - 2] == '\r' else '\n'
            text = text[:k] + '\tctx.r4 = r4;' + nl + text[k:]
            with open(path, 'w', encoding='utf-8', newline='') as fh:
                fh.write(text)
            print('upgraded v1 -> v2 (ctx.r4 before the switch)'); return 0
        print('already applied'); return 0
    nl = '\r\n' if '\r\n' in text else '\n'
    L = text.split(nl)
    f0 = L.index(FUNC)
    f1 = next(i for i in range(f0, len(L)) if L[i] == '}')
    b = next(i for i in range(f0, f1) if L[i:i + len(OLD)] == OLD)
    # case starts: walk back from each 'bl' comment to the line after the previous return/goto
    starts = []
    pos = b + len(OLD)
    for byte, addr, bl in CASES:
        j = next(i for i in range(pos, f1) if L[i].strip() == '// ' + bl)
        k = j
        while not (L[k - 1].strip() in ('return;',) or L[k - 1].strip().startswith('goto ')):
            k -= 1
        starts.append((k, addr))
        pos = j + 1
    assert starts[0][0] == b + len(OLD), 'case 1 must follow the old return'
    for k, addr in reversed(starts):
        L.insert(k, 'loc_%08X:  // LSW_BYTESWITCH_FIX case' % addr)
    # ctx.r4 = r4 stays ahead of the switch: the case handlers (sub_829CAD10 etc.) read the event from ctx.r4
    sw = ['\t// bctr  // LSW_BYTESWITCH_FIX: byte table 0x820CF088 -> 0x829CB818 + 4*byte', OLD[1], '\tswitch (r0.u32) {']
    for byte, addr, _ in CASES:
        sw += ['\tcase %d:' % (byte * 4), '\t\tgoto loc_%08X;' % addr]
    sw += ['\tdefault:', '\t\tbreak;', '\t}'] + OLD[1:]   # unknown type: old behaviour
    L[b:b + len(OLD)] = sw
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        fh.write(nl.join(L))
    print('patched', path, 'cases', len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join('Convert 360', 'LSWTCS', 'output')))
