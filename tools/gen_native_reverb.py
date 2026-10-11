"""Generate native C++ for the game's reverb DSP (guest sub_827E2208 and the ten stage functions it calls) from the
original PowerPC instructions (kept as comments in the recompiled sources). Output: LSWTCSRuntime/source/
native_reverb_gen.inc, included by native_reverb.cpp.

Same arithmetic as the recompiled code, instruction for instruction, so the result is bit-identical:
  scalar single ops: double(float(a op b)) / fmadds double(float(a*b + c)); scalar FP with flush-to-zero off,
  vector FP (vmulfp128/vmaddfp/vaddfp/vsubfp, unfused) with flush-to-zero on (the recompiler's fpscr switches);
  vector registers byte-reversed like the recompiler's (element order [3,2,1,0] in host lanes).
What is faster than the recompiled code: registers are plain locals (no PPCContext traffic), memory accesses are not
volatile, and the 17 KB stack frame lives in a native buffer (addresses >= kFrameBase), so scratch buffers skip the
guest address translation. Memory goes through the LD32/ST32/LDF/STF/LDV/STV/LVLX/... macros of native_reverb.cpp,
which use a per-call local copy of the address map (Fast F) that byte stores cannot alias, so it stays in registers;
template S = the diagnostic redirect/trace path, compiled out of the normal S = false instance. The register save/restore helpers (0x8250F2xx, 0x8250FBE0/FC2C) and stack probes are
dropped: the native code does not touch the guest's registers.

Usage: python tools/gen_native_reverb.py   (reads the recompiled ppc_recomp.494.cpp)"""
import re, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'Convert 360', 'LSWTCS', 'output', 'ppc_recomp.494.cpp')
OUT = os.path.join(ROOT, 'LSWTCSRuntime', 'source', 'native_reverb_gen.inc')
FUNCS = ['827E0810', '827E0EA8', '827E0FD8', '827E10A8', '827E1330', '827E1400', '827E1728', '827E1890',
         '827E15C0', '827E1BF8', '827E2208']
SKIP_CALLS = re.compile(r'0x8250f[0-9a-f]{3}$')   # register save/restore helpers

src = open(SRC, encoding='latin-1').read()

def asm_of(fn):
    m = re.search(r'PPC_FUNC_IMPL\(__imp__sub_%s\) \{(.*?)\n\}\n' % fn, src, re.S)
    out = []
    for line in m.group(1).split('\n'):
        t = line.strip()
        if t.startswith('// '): out.append(('op', t[3:].strip()))
        elif re.match(r'loc_[0-9A-F]+:', t): out.append(('label', t[:-1]))
    return out

def reg(x): return int(x[1:])
def imm(x): return int(x, 0)
def memop(x):
    m = re.match(r'(-?\w+)\((r\d+)\)', x)
    return int(m.group(1), 0), reg(m.group(2))

def gen(fn):
    body = []
    used_labels = set()
    asm = asm_of(fn)
    for kind, t in asm:
        if kind == 'op':
            m = re.search(r'0x([0-9a-f]+)$', t)
            if m and t.split()[0] in ('b', 'beq', 'bne', 'blt', 'bgt', 'bge', 'ble'): used_labels.add('loc_%08X' % int(m.group(1), 16))
    E = body.append
    mode = [None]   # FP flush mode known at this point: 'scalar' (off), 'vector' (on) or None (unknown)
    def want(m):
        if mode[0] == m: return ''
        mode[0] = m
        return 'E.fp.disableFlushMode(); ' if m == 'scalar' else 'E.fp.enableFlushMode(); '
    for kind, t in asm:
        if kind == 'label':
            if t in used_labels: E('%s:;' % t)
            mode[0] = None   # reached by a branch: mode unknown
            continue
        op, _, rest = t.partition(' ')
        a = [x.strip() for x in rest.split(',')] if rest else []
        R = lambda i: 'r%d' % reg(a[i])
        RA0 = lambda i: '0u' if a[i] in ('r0', '0') else 'r%d' % reg(a[i])   # rA = 0 means literal 0 (addi/lvx forms)
        F = lambda i: 'f%d' % reg(a[i])
        V = lambda i: 'v%d' % int(a[i][1:])
        is_s = op in ('lfs', 'stfs', 'lfsx', 'stfsx', 'fmuls', 'fadds', 'fsubs', 'fmadds', 'fctidz')
        is_v = op in ('vmulfp128', 'vaddfp', 'vsubfp', 'vmaddfp')
        sflush = want('scalar') if is_s else ''
        vflush = want('vector') if is_v else ''
        if op in ('mflr', 'mtlr', 'dcbt', 'ld', 'std', 'nop'):
            continue
        if op == 'bl':
            target = a[0]
            if SKIP_CALLS.match(target): continue
            E('rv_%s<S>(E, r1, r3, r4, r5, r6, r7);' % target[2:].upper())
            mode[0] = None   # the callee may have switched it
            continue
        if op == 'b':
            target = a[0]
            if SKIP_CALLS.match(target): E('return;'); continue
            E('goto loc_%08X;' % int(target, 16)); continue
        if op == 'blr': E('return;'); continue
        if op in ('beq', 'bne', 'blt', 'bgt', 'bge', 'ble'):
            cr, target = (a[0], a[1]) if len(a) == 2 else ('cr0', a[0])
            cond = {'beq': 'eq', 'bne': '!eq', 'blt': 'lt', 'bgt': 'gt', 'bge': '!lt', 'ble': '!gt'}[op]
            neg = cond.startswith('!'); c = cond.lstrip('!')
            E('if (%s%s_%s) goto loc_%08X;' % ('!' if neg else '', cr, c, int(target, 16))); continue
        if op in ('cmpwi', 'cmplwi', 'cmpw', 'cmplw'):
            cr, x, y = a
            yv = 'r%d' % reg(y) if y.startswith('r') else '%su' % (imm(y) & 0xFFFFFFFF)
            if op in ('cmpwi', 'cmpw'):
                xs, ys = 'int32_t(r%d)' % reg(x), 'int32_t(%s)' % yv
            else:
                xs, ys = 'uint32_t(r%d)' % reg(x), 'uint32_t(%s)' % yv
            E('%s_lt = %s < %s; %s_gt = %s > %s; %s_eq = %s == %s;' % (cr, xs, ys, cr, xs, ys, cr, xs, ys)); continue
        if op == 'li': E('%s = %su;' % (R(0), imm(a[1]) & 0xFFFFFFFF)); continue
        if op == 'lis': E('%s = %su;' % (R(0), (imm(a[1]) << 16) & 0xFFFFFFFF)); continue
        if op == 'mr': E('%s = %s;' % (R(0), R(1))); continue
        if op == 'addi': E('%s = %s + %su;' % (R(0), RA0(1), imm(a[2]) & 0xFFFFFFFF)); continue
        if op == 'addis': E('%s = %s + %su;' % (R(0), RA0(1), (imm(a[2]) << 16) & 0xFFFFFFFF)); continue
        if op == 'add': E('%s = %s + %s;' % (R(0), R(1), R(2))); continue
        if op == 'subf': E('%s = %s - %s;' % (R(0), R(2), R(1))); continue
        if op == 'subfic': E('%s = %su - %s;' % (R(0), imm(a[2]) & 0xFFFFFFFF, R(1))); continue
        if op == 'neg': E('%s = 0u - %s;' % (R(0), R(1))); continue
        if op == 'ori': E('%s = %s | %su;' % (R(0), R(1), imm(a[2]))); continue
        if op in ('rlwinm', 'rotlwi', 'clrlwi'):
            if op == 'rotlwi': sh, mb, me = imm(a[2]), 0, 31
            elif op == 'clrlwi': sh, mb, me = 0, imm(a[2]), 31
            else: sh, mb, me = imm(a[2]), imm(a[3]), imm(a[4])
            mask = 0
            for b in range(mb, me + 1): mask |= 1 << (31 - b)
            E('%s = rotl32(%s, %d) & 0x%Xu;' % (R(0), R(1), sh, mask)); continue
        if op in ('lwz', 'stw', 'lfs', 'stfs', 'stwu'):
            off, rb = memop(a[1])
            ad = 'r%d + %su' % (rb, off & 0xFFFFFFFF)
            if op == 'lwz': E('%s = LD32(%s);' % (R(0), ad))
            elif op == 'stw': E('ST32(%s, %s);' % (ad, R(0)))
            elif op == 'stwu': E('ST32(%s, r%d); r%d = %s;' % (ad, reg(a[0]), rb, ad))
            elif op == 'lfs': E(sflush + '%s = double(LDF(%s));' % (F(0), ad))
            elif op == 'stfs': E(sflush + 'STF(%s, float(%s));' % (ad, F(0)))
            continue
        if op in ('lfsx', 'stfsx', 'stfiwx'):
            ad = '%s + %s' % (RA0(1), R(2))
            if op == 'lfsx': E(sflush + '%s = double(LDF(%s));' % (F(0), ad))
            elif op == 'stfsx': E(sflush + 'STF(%s, float(%s));' % (ad, F(0)))
            else: E('ST32(%s, uint32_t(bits64(%s)));' % (ad, F(0)))
            continue
        if op == 'fmr': E('%s = %s;' % (F(0), F(1))); continue
        if op == 'fmuls': E(sflush + '%s = double(float(%s * %s));' % (F(0), F(1), F(2))); continue
        if op == 'fadds': E(sflush + '%s = double(float(%s + %s));' % (F(0), F(1), F(2))); continue
        if op == 'fsubs': E(sflush + '%s = double(float(%s - %s));' % (F(0), F(1), F(2))); continue
        if op == 'fmadds': E(sflush + '%s = double(float(%s * %s + %s));' % (F(0), F(1), F(2), F(3))); continue
        if op == 'fctidz': E(sflush + '%s = from_bits64(fctidz(%s));' % (F(0), F(1))); continue
        # vector
        if op in ('lvx128', 'stvx128'):
            ad = '(%s + %s) & ~0xFu' % (RA0(1), R(2))
            if op == 'lvx128': E('LDV(%s, %s);' % (ad, V(0)))
            else: E('STV(%s, %s);' % (ad, V(0)))
            continue
        if op in ('lvlx', 'lvrx', 'stvlx', 'stvrx'):
            ad = '%s + %s' % (RA0(1), R(2))
            E('%s(%s, %s);' % (op.upper(), ad, V(0))); continue
        if op == 'vor': E('vor(%s, %s, %s);' % (V(0), V(1), V(2))); continue
        if op in ('vmulfp128', 'vaddfp', 'vsubfp'):
            sym = {'vmulfp128': '*', 'vaddfp': '+', 'vsubfp': '-'}[op]
            E(vflush + 'for (int k = 0; k < 4; ++k) %s.f[k] = %s.f[k] %s %s.f[k];' % (V(0), V(1), sym, V(2))); continue
        if op == 'vmaddfp':
            E(vflush + 'for (int k = 0; k < 4; ++k) %s.f[k] = %s.f[k] * %s.f[k] + %s.f[k];' % (V(0), V(1), V(2), V(3))); continue
        if op == 'vspltw':
            E('vspltw(%s, %s, %d);' % (V(0), V(1), imm(a[2]))); continue
        raise SystemExit('unhandled in %s: %s' % (fn, t))
    vregs = sorted(set(int(x) for x in re.findall(r'\bv(\d+)\b', '\n'.join(body))))
    head = ['template <bool S> static void rv_%s(Env& E, uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {' % fn,
            '    const Fast F = fast_of(E);',
            '    uint32_t r0 = 0, r2 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0, r16 = 0, r17 = 0,',
            '             r18 = 0, r19 = 0, r20 = 0, r21 = 0, r22 = 0, r23 = 0, r24 = 0, r25 = 0, r26 = 0, r27 = 0, r28 = 0,',
            '             r29 = 0, r30 = 0, r31 = 0;',
            '    double ' + ', '.join('f%d = 0' % i for i in range(32)) + ';',
            '    bool ' + ', '.join('cr%d_lt = false, cr%d_gt = false, cr%d_eq = false' % (c, c, c) for c in (0, 6)) + ';']
    if vregs: head.append('    Vec ' + ', '.join('v%d{}' % i for i in vregs) + ';')
    tail = ['}', '']
    return head + ['    ' + l if not l.startswith('loc_') else l for l in body] + tail

out = ['// GENERATED by tools/gen_native_reverb.py from the recompiled guest code. Do not edit by hand.', '']
for fn in FUNCS:
    out.append('template <bool S> static void rv_%s(Env& E, uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7);' % fn)
out.append('')
for fn in FUNCS:
    out += gen(fn)
open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('wrote', OUT, sum(1 for l in out), 'lines')
