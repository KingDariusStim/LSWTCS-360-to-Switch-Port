"""Pass caller register values into functions that read register locals before writing them.

Locals mode (XenonRecomp *_as_local) gives every recompiled function its own zero-initialised
`PPCRegister rN{}` etc. That is only correct when every callee follows the PowerPC ABI. Some do not:

  - MSVC SEH __finally/__except funclets: called with r12 = parent frame pointer and the parent's
    r14-r31 still live (e.g. sub_822A1E6C releases the heap lock iff the parent's r22 != 0).
  - function fragments: XenonRecomp split one real function in two and the first tail-branches into
    the second with its registers live (e.g. sub_822A2E30 -> sub_822A34F0).
  - the guest CRT setjmp (saves every non-volatile register).

In ctx mode those registers travel through PPCContext. This pass restores exactly that for the
registers that need it: a CFG liveness analysis (interprocedural fixpoint over direct calls) finds
each function's live-in register locals; the function loads them from a thread-local mirror
(`g_lsw_live`, declared in ppc_context.h) at entry, and every direct call site stores the caller's
copies of the callee's live-ins into the mirror just before the call. A caller that has no local
for a register leaves the mirror slot as its own caller wrote it (= ctx-mode pass-through).

Over-approximation is safe (an extra copy is harmless), so unknown control flow is treated as
"may go anywhere it textually could". Stack spills of a register before it is defined
(`PPC_STORE_U64(ctx.r1.u32 + -N, rX.u64)`) are not uses: in locals mode the matching reload at exit
is dead (the caller's register cannot change).

Usage: python pass_livein_regs.py <output_dir> [--dry-run] [--report FILE]
Edits ppc_recomp.*.cpp and ppc_context.h in place. Idempotent guard: refuses to run twice.
"""
import re, sys, os, glob, collections

MARK = '// LSW_LIVEIN_PASS'
FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\) \{')
DECL_RE = re.compile(r'^\t(PPC\w*Register) (\w+)\{\};$')
SKIP_NAMES = {'xer', 'temp', 'vTemp', 'reserved'}          # scratch / per-instruction state
CALL_RE = re.compile(r'\b(?:__imp__)?(sub_[0-9A-F]{8})\(ctx, base\)')
LABEL_RE = re.compile(r'^(loc_[0-9A-F]+):')
GOTO_RE = re.compile(r'\bgoto (loc_[0-9A-F]+);')
SPILL_RE = re.compile(r'^\s*PPC_STORE_U(?:64|32)\(ctx\.r1\.u32 \+ -\d+, (\w+)\.u(?:64|32)\);\s*$')
VSPILL_RE = re.compile(r'^\s*simde_mm_store_si128\(\(simde__m128i\*\)\(PPC_HOST\(\(\(ctx\.r1\.u32 \+ (r\d+)\.u32\) & ~0xF\)\)\), '
                       r'simde_mm_shuffle_epi8\(simde_mm_load_si128\(\(simde__m128i\*\)(v\d+)\.u8\), simde_mm_load_si128\(\(simde__m128i\*\)VectorMaskL\)\)\);\s*$')


class Func:
    __slots__ = ('name', 'path', 'start', 'body_start', 'end', 'locals', 'types', 'lines',
                 'succ', 'use', 'kill', 'calls', 'livein')


def parse_file(path):
    text = open(path, encoding='utf-8', newline='').read()
    lines = text.split('\n')
    funcs = []
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1
            continue
        f = Func()
        f.name, f.path, f.start = m.group(1), path, i
        j = i + 1
        if j < len(lines) and lines[j].strip() == 'PPC_FUNC_PROLOGUE();':
            j += 1
        f.locals, f.types = [], {}
        while j < len(lines):
            d = DECL_RE.match(lines[j])
            if d:
                if d.group(2) not in SKIP_NAMES:
                    f.locals.append(d.group(2)); f.types[d.group(2)] = d.group(1)
                j += 1
            elif lines[j].startswith('\tuint32_t ea{};'):
                j += 1
            else:
                break
        f.body_start = j
        k = j
        while k < len(lines) and lines[k] != '}':
            k += 1
        f.end = k                                   # index of closing brace
        funcs.append(f)
        i = k + 1
    return lines, funcs


def word_re(name):
    return re.compile(r'(?<![\w.])' + re.escape(name) + r'(?!\w)')


def analyse_lines(f, lines):
    """Build per-line succ / use / kill bitmasks (bit = index in f.locals) and call list."""
    body = lines[f.body_start:f.end]
    n = len(body)
    idx = {nm: b for b, nm in enumerate(f.locals)}
    pats = [(b, word_re(nm), nm) for nm, b in idx.items()]
    labels = {}
    for li, t in enumerate(body):
        m = LABEL_RE.match(t)
        if m:
            labels[m.group(1)] = li
    # brace matching for '{' ... '}' lines inside the body
    close = {}
    stack = []
    for li, t in enumerate(body):
        s = t.strip()
        if s.startswith('//'):
            continue
        opens = s.count('{') - s.count('{}')
        closes = s.count('}') - s.count('{}')
        if s.startswith('}') and closes > 0:
            if stack:
                close[stack.pop()] = li
            closes -= 1
            if s.endswith('{'):                     # '} else {'
                stack.append(li); opens -= 1
        net = opens - closes
        if net > 0 and s.endswith('{'):
            stack.append(li)
    succ = [None] * n
    use = [0] * n
    kill = [0] * n
    calls = []
    for li, t in enumerate(body):
        s = t.strip()
        nxt = [li + 1] if li + 1 < n else []
        if not s or s.startswith('//'):
            succ[li] = nxt
            continue
        gotos = GOTO_RE.findall(s)
        if s.startswith('switch ('):
            c = close.get(li, n - 1)
            succ[li] = [x for x in range(li + 1, c) if body[x].strip().startswith(('case ', 'default:'))] or nxt
        elif s.startswith('goto ') and len(gotos) == 1:
            succ[li] = [labels[gotos[0]]] if gotos[0] in labels else []
        elif s == 'return;':
            succ[li] = []
        elif s.endswith('{') and li in close:
            succ[li] = nxt + ([close[li] + 1] if close[li] + 1 < n else [])
        else:
            succ[li] = nxt + [labels[g] for g in gotos if g in labels]
        # uses / kills
        if SPILL_RE.match(t):
            continue
        u = 0
        for b, p, nm in pats:
            if nm in t and p.search(t):
                u |= 1 << b
        m = VSPILL_RE.match(t)       # stvx vN,r1,rX prologue save: vN is not a use
        if m and m.group(2) in idx:
            u &= ~(1 << idx[m.group(2)])
        if u:   # every write form below also mentions the name, so u != 0 for it
            m = re.match(r'^\s*(\w+)\.(\w+)(?:\[\d+\])? = (.*);\s*$', t)   # element writes too (vslw: u32[0..3])
            if m and m.group(1) in idx and not word_re(m.group(1)).search(m.group(3)):
                kill[li] |= 1 << idx[m.group(1)]
                u &= ~(1 << idx[m.group(1)])
            m = re.match(r'^\s*(\w+) = (.*);\s*$', t)      # whole-register copy: v29 = vTemp;
            if m and m.group(1) in idx and not re.search(r'(?<![\w.])' + m.group(1) + r'\b', m.group(2)):
                kill[li] |= 1 << idx[m.group(1)]
                u &= ~(1 << idx[m.group(1)])
            m = re.match(r'^\s*(cr\d)\.(?:compare|setFromMask)[<(]', t)
            if m and m.group(1) in idx:
                b = idx[m.group(1)]
                rest = t.split('(', 1)[1]
                if not word_re(m.group(1)).search(rest):
                    kill[li] |= 1 << b; u &= ~(1 << b)
            m = re.match(r'^\s*(?:simde_)?_?mm_store\w*\(\(?[^,]*?\b(v\d+)\.\w+\)?, (.*)\);\s*$', t)
            if m and m.group(1) in idx and not word_re(m.group(1)).search(m.group(2)):
                kill[li] |= 1 << idx[m.group(1)]; u &= ~(1 << idx[m.group(1)])
        use[li] = u
        for cm in CALL_RE.finditer(t):
            calls.append((li, cm.group(1)))
    f.lines, f.succ, f.use, f.kill, f.calls = body, succ, use, kill, calls


def liveness(f, extra_use):
    n = len(f.succ)
    if n == 0:
        return 0
    use = f.use[:]
    for li, m in extra_use.items():
        use[li] |= m
    live_in = [0] * n
    pred = [[] for _ in range(n)]
    for i, ss in enumerate(f.succ):
        for s in ss:
            if 0 <= s < n:
                pred[s].append(i)
    work = list(range(n - 1, -1, -1))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        out = 0
        for s in f.succ[i]:
            if 0 <= s < n:
                out |= live_in[s]
        v = use[i] | (out & ~f.kill[i])
        if v != live_in[i]:
            live_in[i] = v
            for p in pred[i]:
                if not inq[p]:
                    inq[p] = True; work.append(p)
    return live_in[0]


def names(f, mask):
    return [nm for b, nm in enumerate(f.locals) if mask >> b & 1]


def main():
    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    ctxh = os.path.join(d, 'ppc_context.h')
    if MARK in open(ctxh, encoding='utf-8').read():
        sys.exit('already applied (marker in ppc_context.h)')
    files = sorted(glob.glob(os.path.join(d, 'ppc_recomp.*.cpp')),
                   key=lambda s: int(re.findall(r'\.(\d+)\.cpp$', s)[0]))
    filelines, allf = {}, {}
    for p in files:
        lines, funcs = parse_file(p)
        filelines[p] = lines
        for f in funcs:
            analyse_lines(f, lines)
            allf[f.name] = f
    print(f'{len(allf)} functions parsed', flush=True)
    # interprocedural fixpoint: livein(name) as set of register names
    livein = {nm: frozenset() for nm in allf}
    callers = collections.defaultdict(set)
    for f in allf.values():
        for _, c in f.calls:
            callers[c].add(f.name)
    todo = set(allf)
    rounds = 0
    while todo:
        rounds += 1
        nxt = set()
        for nm in todo:
            f = allf[nm]
            idx = {x: b for b, x in enumerate(f.locals)}
            extra = {}
            for li, c in f.calls:
                m = 0
                for r in livein.get(c, ()):
                    if r in idx:
                        m |= 1 << idx[r]
                if m:
                    extra[li] = extra.get(li, 0) | m
            li_set = frozenset(names(f, liveness(f, extra)))
            if li_set != livein[nm]:
                livein[nm] = li_set
                nxt |= callers[nm] & allf.keys()
        todo = nxt
        print(f'round {rounds}: {len(todo)} to revisit', flush=True)
    need = {nm: s for nm, s in livein.items() if s}
    by_reg = collections.Counter(r for s in need.values() for r in s)
    print(f'functions with live-in register locals: {len(need)}')
    print('by register:', dict(by_reg.most_common()))
    if report:
        with open(report, 'w') as fp:
            for nm in sorted(need):
                fp.write(f'{nm} {os.path.basename(allf[nm].path)}: {" ".join(sorted(need[nm]))}\n')
    if dry:
        return
    # struct of every register that ever needs passing, typed as the local is
    typ = {}
    for nm, s in need.items():
        for r in s:
            typ[r] = allf[nm].types[r]
    def key(r):
        m = re.match(r'([a-z]+)(\d*)$', r)
        return (m.group(1), int(m.group(2) or 0))
    members = ''.join(f'    {typ[r]} {r};\n' for r in sorted(typ, key=key))
    decl = (f'\n{MARK}: registers passed to functions that read register locals before writing them\n'
            '// (SEH funclets, split function fragments, guest setjmp). Written by the caller just before a\n'
            '// direct call, read by the callee at entry. See EXPERIMENTAL_REGEN/tools/pass_livein_regs.py.\n'
            f'struct LswLiveRegs {{\n{members}}};\n'
            'inline thread_local LswLiveRegs g_lsw_live{};\n')
    h = open(ctxh, encoding='utf-8', newline='').read()
    anchor = h.rfind('#endif')
    h = h[:anchor] + decl + '\n' + h[anchor:]
    open(ctxh, 'w', encoding='utf-8', newline='').write(h)
    # edit files: insertions keyed by absolute line index, applied bottom-up
    stores = loads = skipped = 0
    by_file = collections.defaultdict(list)
    for f in allf.values():
        by_file[f.path].append(f)
    for p, funcs in by_file.items():
        lines = filelines[p]
        ins = collections.defaultdict(list)       # abs index -> lines to insert before it
        for f in funcs:
            if f.name in need:
                ins[f.body_start].append('\t' + ' '.join(f'{r} = g_lsw_live.{r};' for r in sorted(need[f.name], key=key)) + '  ' + MARK)
                loads += 1
            local = set(f.locals)
            done = set()
            for li, c in f.calls:
                regs = [r for r in sorted(need.get(c, ()), key=key) if r in local]
                if not regs or li in done:
                    continue
                prev = f.lines[li - 1].rstrip() if li > 0 else '{'
                k = li - 1
                while k > 0 and f.lines[k].strip().startswith('//'):
                    k -= 1
                prev = f.lines[k].rstrip() if k >= 0 else '{'
                if not (prev.endswith((';', '{', '}', ':')) or prev == ''):
                    skipped += 1
                    print(f'WARN {f.name}: call to {c} at body line {li} after "{prev.strip()[:60]}" - not instrumented')
                    continue
                indent = re.match(r'^(\s*)', f.lines[li]).group(1)
                ins[f.body_start + li].append(indent + ' '.join(f'g_lsw_live.{r} = {r};' for r in regs) + '  ' + MARK)
                done.add(li)
                stores += 1
        if not ins:
            continue
        for a in sorted(ins, reverse=True):
            lines[a:a] = ins[a]
        open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    print(f'entry loads: {loads}, call-site stores: {stores}, skipped call sites: {skipped}')


if __name__ == '__main__':
    main()
