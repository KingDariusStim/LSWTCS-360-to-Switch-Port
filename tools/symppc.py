"""Symbolic evaluator for a PowerPC subset (reverb dissection). Integer registers are affine forms over symbols
(SP, S=state, IN0/IN1, OUT, loop vars) or opaque nodes; float registers are expression strings. Memory: stack slots
(SP-relative constant addresses) are tracked; other loads/stores become trace lines. Usage:
  python symppc.py asm.txt start_line end_line [--init file] ; prints the dataflow trace."""
import re, sys

class Aff:
    def __init__(self, d=None):
        self.d = {k: v for k, v in (d or {}).items() if v != 0}
    @staticmethod
    def c(v): return Aff({1: v})
    @staticmethod
    def s(name): return Aff({name: 1})
    def __add__(self, o):
        d = dict(self.d)
        for k, v in o.d.items(): d[k] = d.get(k, 0) + v
        return Aff(d)
    def neg(self): return Aff({k: -v for k, v in self.d.items()})
    def __sub__(self, o): return self + o.neg()
    def mul(self, k): return Aff({a: v * k for a, v in self.d.items()})
    def const(self): return self.d.get(1, 0) if set(self.d) <= {1} else None
    def __repr__(self):
        if not self.d: return '0'
        parts = []
        for k, v in sorted(self.d.items(), key=lambda kv: (kv[0] == 1, str(kv[0]))):
            if k == 1: parts.append('%#x' % v if abs(v) > 9 else str(v))
            else: parts.append(('' if v == 1 else '-' if v == -1 else '%d*' % v) + str(k))
        return ' + '.join(parts).replace('+ -', '- ')
    def key(self): return tuple(sorted((str(k), v) for k, v in self.d.items()))

def s32(v): v &= 0xFFFFFFFF; return v - (1 << 32) if v & 0x80000000 else v

class Node:   # opaque integer value
    def __init__(self, txt): self.txt = txt
    def __repr__(self): return '(' + self.txt + ')'

class St:
    def __init__(self):
        self.r = {i: Aff.s('in_r%d' % i) for i in range(32)}
        self.r[1] = Aff.s('SP')
        self.f = {i: 'in_f%d' % i for i in range(32)}
        self.v = {}
        self.stack = {}
        self.out = []
        self.tmp = 0
    def t(self, expr):
        self.tmp += 1
        name = 't%d' % self.tmp
        self.out.append('%s = %s' % (name, expr))
        return name

def addr_str(a):
    if isinstance(a, Aff) and 'SP' in a.d and a.d['SP'] == 1:
        rest = a - Aff.s('SP') + Aff.c(17872)
        return 'FR[' + repr(rest.mul(1)) .replace('0x', '0x') + ']' if True else repr(a)
    return repr(a) if isinstance(a, (Aff, Node)) else str(a)

def add(a, b):
    if isinstance(a, Aff) and isinstance(b, Aff): return a + b
    return Node('%s + %s' % (addr_str(a), addr_str(b)))

def run(lines, st, stop_at_branch=False):
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith('loc_') or ln.startswith('===='):
            if ln.startswith('loc_'): st.out.append('# ' + ln)
            continue
        op, _, args = ln.partition(' ')
        a = [x.strip() for x in args.split(',')] if args else []
        R = lambda x: int(x[1:])
        def imm(x): return int(x, 0)
        def mem(x):
            m = re.match(r'(-?\w+)\((r\d+)\)', x)
            return Aff.c(int(m.group(1), 0)) if False else (int(m.group(1), 0), R(m.group(2)))
        g = st.r
        if op in ('mflr', 'mtlr', 'dcbt', 'cmpwi', 'cmplwi', 'cmpw', 'cmplw', 'b', 'bne', 'beq', 'blt', 'bgt', 'bge', 'ble', 'bdnz', 'mtctr', 'blr', 'nop'):
            if op.startswith('b') and op != 'bl': st.out.append('# ' + ln)
            continue
        if op == 'bl':
            st.out.append('CALL ' + a[0] + '  (r3=%s r4=%s r5=%s r6=%s r7=%s)' % tuple(addr_str(g[i]) for i in (3, 4, 5, 6, 7)))
            for i in range(3, 13): g[i] = Aff.s('ret%d_%d' % (i, st.tmp)); st.tmp += 1
            for i in range(0, 14): st.f[i] = 'clob'
            continue
        if op == 'li': g[R(a[0])] = Aff.c(imm(a[1]))
        elif op == 'lis': g[R(a[0])] = Aff.c(imm(a[1]) << 16)
        elif op == 'mr': g[R(a[0])] = g[R(a[1])]
        elif op == 'addi': g[R(a[0])] = add(g[R(a[1])] if a[1] != 'r0' else Aff.c(0), Aff.c(imm(a[2])))
        elif op == 'addis': g[R(a[0])] = add(g[R(a[1])] if a[1] != 'r0' else Aff.c(0), Aff.c(imm(a[2]) << 16))
        elif op == 'ori':
            x = g[R(a[1])]
            if isinstance(x, Aff) and x.const() is not None: g[R(a[0])] = Aff.c(x.const() | imm(a[2]))
            else: g[R(a[0])] = Node('%s | %s' % (addr_str(x), a[2]))
        elif op == 'add': g[R(a[0])] = add(g[R(a[1])], g[R(a[2])])
        elif op == 'subf':   # rD = rB - rA
            x, y = g[R(a[2])], g[R(a[1])]
            g[R(a[0])] = x - y if isinstance(x, Aff) and isinstance(y, Aff) else Node('%s - %s' % (addr_str(x), addr_str(y)))
        elif op == 'subfic':
            x = g[R(a[1])]
            g[R(a[0])] = Aff.c(imm(a[2])) - x if isinstance(x, Aff) else Node('%s - %s' % (a[2], addr_str(x)))
        elif op == 'neg': x = g[R(a[1])]; g[R(a[0])] = x.neg() if isinstance(x, Aff) else Node('-' + addr_str(x))
        elif op in ('rlwinm', 'rotlwi', 'clrlwi', 'rlwinm.'):
            x = g[R(a[1])]
            if op == 'rotlwi': sh, mb, me = imm(a[2]), 0, 31
            elif op == 'clrlwi': sh, mb, me = 0, imm(a[2]), 31
            else: sh, mb, me = imm(a[2]), imm(a[3]), imm(a[4])
            mask = 0
            for bit in range(mb, me + 1): mask |= 1 << (31 - bit)
            if sh == 0 and mb == 0 and me == 31: g[R(a[0])] = x
            elif isinstance(x, Aff) and x.const() is not None:
                v = x.const() & 0xFFFFFFFF; v = ((v << sh) | (v >> (32 - sh))) & 0xFFFFFFFF if sh else v
                g[R(a[0])] = Aff.c(v & mask)
            elif mask == (0xFFFFFFFF << sh) & 0xFFFFFFFF and isinstance(x, Aff):   # plain shift left
                g[R(a[0])] = x.mul(1 << sh)
            elif mb == 0 and sh == 0 and isinstance(x, Aff):   # align down (rlwinm x,0,0,N): a new aligned base
                off = x - Aff.s('SP') + Aff.c(17872)          # frame-relative (r1 after stwu)
                g[R(a[0])] = Aff.s('AL%d' % (off.const() if off.const() is not None else -1))
            else:
                # (x << sh) & mask: circular index; keep as node idx(x, mask>>sh) * (1<<sh)
                if sh and (mask >> sh) << sh == mask: g[R(a[0])] = Node('((%s) & %#x)*%d' % (addr_str(x), mask >> sh, 1 << sh))
                else: g[R(a[0])] = Node('rot(%s,%d)&%#x' % (addr_str(x), sh, mask))
        elif op in ('lwz', 'stw', 'lfs', 'stfs', 'ld', 'std', 'stwu'):
            off, rb = mem(a[1])
            base = g[rb]
            ad = add(base, Aff.c(off))
            if op == 'stwu':
                g[1] = ad; continue
            if op in ('ld', 'std'): continue
            spk = ad.key() if isinstance(ad, Aff) and set(ad.d) <= {'SP', 1} else None
            if op == 'lwz':
                if spk is not None: g[R(a[0])] = st.stack.get(spk, Aff.s('stk%s' % addr_str(ad).replace(' ', '')))
                else: g[R(a[0])] = Node('LW[%s]' % addr_str(ad))
            elif op == 'stw':
                if spk is not None: st.stack[spk] = g[R(a[0])]
                else: st.out.append('SW[%s] = %s' % (addr_str(ad), addr_str(g[R(a[0])])))
            elif op == 'lfs':
                if spk is not None and spk in st.stack and isinstance(st.stack[spk], str): st.f[R(a[0])] = st.stack[spk]
                else: st.f[R(a[0])] = st.t('LF[%s]' % addr_str(ad))
            elif op == 'stfs':
                if spk is not None: st.stack[spk] = st.f[R(a[0])]
                st.out.append('SF[%s] = %s' % (addr_str(ad), st.f[R(a[0])]))
        elif op in ('lfsx', 'stfsx'):
            ad = add(g[R(a[1])] if a[1] != 'r0' else Aff.c(0), g[R(a[2])])
            if op == 'lfsx': st.f[R(a[0])] = st.t('LF[%s]' % addr_str(ad))
            else: st.out.append('SF[%s] = %s' % (addr_str(ad), st.f[R(a[0])]))
        elif op == 'fmr': st.f[R(a[0])] = st.f[R(a[1])]
        elif op == 'fmuls': st.f[R(a[0])] = st.t('%s * %s' % (st.f[R(a[1])], st.f[R(a[2])]))
        elif op == 'fadds': st.f[R(a[0])] = st.t('%s + %s' % (st.f[R(a[1])], st.f[R(a[2])]))
        elif op == 'fsubs': st.f[R(a[0])] = st.t('%s - %s' % (st.f[R(a[1])], st.f[R(a[2])]))
        elif op == 'fmadds': st.f[R(a[0])] = st.t('fma(%s, %s, %s)' % (st.f[R(a[1])], st.f[R(a[2])], st.f[R(a[3])]))
        elif op == 'fmsubs': st.f[R(a[0])] = st.t('fms(%s, %s, %s)' % (st.f[R(a[1])], st.f[R(a[2])], st.f[R(a[3])]))
        elif op == 'fnmsubs': st.f[R(a[0])] = st.t('-fms(%s, %s, %s)' % (st.f[R(a[1])], st.f[R(a[2])], st.f[R(a[3])]))
        elif op == 'fneg': st.f[R(a[0])] = st.t('-%s' % st.f[R(a[1])])
        else:
            st.out.append('?? ' + ln)
    return st

if __name__ == '__main__':
    lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
    s, e = int(sys.argv[2]), int(sys.argv[3])
    st = St()
    if len(sys.argv) > 4 and sys.argv[4] == '--pre':   # run lines [pre_s, pre_e) first (setup), silently
        ps, pe = int(sys.argv[5]), int(sys.argv[6])
        run(lines[ps - 1:pe - 1], st); st.out = []
    run(lines[s - 1:e], st)
    print('\n'.join(st.out))
    print('---- registers at end')
    for i in range(32):
        if repr(st.r[i]) != 'in_r%d' % i: print('r%d = %s' % (i, addr_str(st.r[i])))
