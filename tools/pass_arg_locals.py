"""Keep the argument / stack registers r1, r3-r10 and f1-f13 in locals inside recompiled functions (2026-10-11).

Locals mode (XenonRecomp *_as_local) made r0/r2/r11/r12, r14-r31, f0, f14-f31 locals; pass_vec_locals.py added
v0-v13. r1 (stack pointer), r3-r10 (arguments / scratch) and f1-f13 still live in PPCContext: ~1.9 M references
in the generated code, each a load or store through ctx, reloaded after every guest memory store (which may alias
ctx as far as the compiler knows).

Same scheme as pass_vec_locals.py (ctx-mode semantics kept, no ABI change, no liveness needed for correctness):
  - at entry, each register the function uses is copied from ctx to a local;
  - before every line that hands ctx to other code (direct call, PPC_CALL_INDIRECT_FUNC, host helper taking ctx)
    and before every return / the closing brace, locals that may differ from ctx are written back;
  - after every such line, all of them are reloaded (the callee may change any; r1 too, because a split function
    fragment can run its parent's epilogue). Reloads the function never reads are dead and the compiler drops them.
Difference from the vector pass: calls are far more frequent, so only DIRTY registers are written back, i.e. those
assigned on some path since the last point where ctx and the locals were known equal. That is a forward dataflow
over the function's lines (successor = next line, plus goto targets; an unconditional goto / return at the top
level has no fall-through). A sync point nested inside a { } block does not clear the dirty set (the path that
skips the block still carries it). Over-approximating dirty is always safe (an extra store of an equal value).
Functions it cannot rewrite safely are skipped: setjmp / longjmp / try, general loops (while / do / for(;;)),
a call or return as the body of a braceless if/else/loop, a promoted register written on a sync line or used after
the call on it, a name clash with an existing local, a goto to an unknown label.

Usage: python pass_arg_locals.py <output_dir> [--dry-run] [--min-gain X] [--report FILE] [--regs r1,r3,...]
Edits ppc_recomp.*.cpp in place (back up the directory first). Idempotent guard: refuses to run twice.
"""
import re, sys, os, glob

MARK = '// LSW_ARGLOCAL_PASS'
ALL_REGS = ['r1'] + ['r%d' % n for n in range(3, 11)] + ['f%d' % n for n in range(1, 14)]
FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\) \{')
ESCAPE_RE = re.compile(r'\bctx\b(?!\.)|PPC_CALL_INDIRECT_FUNC|PPC_CALL_FUNC')
RETURN_RE = re.compile(r'\breturn\b')
COND_RETURN_RE = re.compile(r'^(\s*)if \((.*)\) return;\s*$')
BRACELESS_RE = re.compile(r'^\s*(?:\}\s*)?(?:if\b|else\b|for\b|while\b).*[^;{}\s]\s*$')
UNSAFE_RE = re.compile(r'setjmp|longjmp|\btry\b|\bcatch\b')
LOOP_RE = re.compile(r'^\s*(?:\}\s*)?(?:while\b|do\b)|for\s*\(\s*;\s*;')
DECL_RE = re.compile(r'^\t(?:PPC\w*Register|uint32_t) \w+\{\};$|^\tPPC_FUNC_PROLOGUE\(\);$|^\t\w+ = g_lsw_live\.\w+;')
LABEL_RE = re.compile(r'^(loc_[0-9A-F]+):')
GOTO_RE = re.compile(r'\bgoto (loc_[0-9A-F]+);')
UNCOND_RE = re.compile(r'^\s*(?:goto loc_[0-9A-F]+|return);\s*$')
CASE_RE = re.compile(r'^\s*(?:case\b.*|default):\s*$')
COMPLETE_RE = re.compile(r'[;{}:]\s*$')
STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def code_line_before(lines, i, lo):
    j = i - 1
    while j >= lo:
        s = lines[j].strip()
        if s and not s.startswith('//'):
            return lines[j]
        j -= 1
    return ''


def balanced(s):
    """Braces on the line open and close in order (code inserted before / after it stays at the same level)."""
    d = 0
    for c in s:
        if c == '{': d += 1
        elif c == '}':
            d -= 1
            if d < 0: return False
    return d == 0


def strip_code(ln):
    """Line without string / char literals and trailing // comment (for brace counting)."""
    s = STRING_RE.sub('""', ln)
    p = s.find('//')
    return s if p < 0 else s[:p]


class Pass:
    def __init__(self, regs, min_gain):
        self.regs = regs
        alt = '|'.join(sorted(regs, key=len, reverse=True))
        self.ref_re = re.compile(r'\bctx\.(%s)\b' % alt)
        # a write: ctx.rN.field = / op= / ++ / --, or the address taken (treated as a write)
        self.write_re = re.compile(r'(&\s*)?\bctx\.(%s)\.\w+(?:\[[^\]]*\])?\s*(=(?!=)|[-+*/%%&|^]=|<<=|>>=|\+\+|--)?' % alt)
        self.preinc_re = re.compile(r'(?:\+\+|--)\s*ctx\.(%s)\b' % alt)
        self.min_gain = min_gain
        self.stats = dict(done=0, refs_done=0, refs_skipped=0, skip_gain=0, skip_unsafe=0, skip_clash=0,
                          skip_braceless=0, skip_sameline=0, skip_loop=0, skip_label=0, skip_brace=0, skip_stmt=0, wb=0, wb_naive=0, funcs=[])

    def written(self, ln):
        w = set()
        for m in self.write_re.finditer(ln):
            if m.group(1) or m.group(3):
                w.add(m.group(2))
        for m in self.preinc_re.finditer(ln):
            w.add(m.group(1))
        return w

    def process_function(self, lines, start, end):
        st = self.stats
        name = FUNC_RE.match(lines[start]).group(1)
        body = lines[start + 1:end]
        refs = 0
        used = set()
        for ln in body:
            if ln.lstrip().startswith('//'):
                continue
            for m in self.ref_re.finditer(ln):
                used.add(m.group(1)); refs += 1
        if not used:
            return None
        code = '\n'.join(strip_code(ln) for ln in body if not ln.lstrip().startswith('//'))
        if UNSAFE_RE.search(code):
            st['skip_unsafe'] += 1; return None
        bare = self.ref_re.sub('', code)
        for r in used:
            if re.search(r'(?<![.\w])%s\b' % r, bare):
                st['skip_clash'] += 1; return None
        k = start + 1
        while k < end and DECL_RE.match(lines[k]):
            k += 1
        if k == end:
            return None
        n = end - k
        labels = {}
        info = []   # per line: (kind, written, gotos, depth, uncond) kind: None | 'esc' | 'ret' | 'cret'
        depth = 1
        for i in range(k, end):
            ln = lines[i]
            if ln.lstrip().startswith('//'):
                info.append((None, set(), [], depth, False)); continue
            m = LABEL_RE.match(ln)
            if m:
                labels[m.group(1)] = i - k
            if LOOP_RE.search(ln):
                st['skip_loop'] += 1; return None
            sc = strip_code(ln)
            esc = bool(ESCAPE_RE.search(sc))
            ret = bool(RETURN_RE.search(sc))
            wr = self.written(ln)
            kind = None
            if esc or ret:
                if not balanced(sc):
                    st['skip_brace'] += 1; return None
                prev = code_line_before(lines, i, k)
                # the site must be whole statements: code is inserted before and after the line
                if not COMPLETE_RE.search(sc) or (prev and not COMPLETE_RE.search(strip_code(prev))):
                    st['skip_stmt'] += 1; return None
                if BRACELESS_RE.match(prev):
                    st['skip_braceless'] += 1; return None
                if esc:
                    if wr:
                        st['skip_sameline'] += 1; return None
                    after = ESCAPE_RE.split(sc, 1)[1]
                    if self.ref_re.search(after):
                        st['skip_sameline'] += 1; return None
                    kind = 'esc'
                elif COND_RETURN_RE.match(ln):
                    kind = 'cret'
                else:
                    kind = 'ret'
            uncond = False
            if depth == 1 and UNCOND_RE.match(ln):
                prev = code_line_before(lines, i, k)
                uncond = not BRACELESS_RE.match(prev) and not CASE_RE.match(prev)
            info.append((kind, wr, GOTO_RE.findall(ln), depth, uncond))
            s = strip_code(ln)
            depth += s.count('{') - s.count('}')
        # predecessors
        preds = [[] for _ in range(n)]
        for j in range(n):
            kind, wr, gotos, d, uncond = info[j]
            if j + 1 < n and not uncond:
                preds[j + 1].append(j)
            for g in gotos:
                if g not in labels:
                    st['skip_label'] += 1; return None
                preds[labels[g]].append(j)
        # forward dataflow to a fixpoint (dirty sets as frozensets)
        empty = frozenset()
        din = [empty] * n
        dout = [None] * n
        changed = True
        while changed:
            changed = False
            for j in range(n):
                s = set()
                for p in preds[j]:
                    if dout[p] is not None:
                        s |= dout[p]
                s = frozenset(s)
                kind, wr, gotos, d, uncond = info[j]
                if kind == 'esc' and d == 1:
                    o = empty
                else:
                    o = s | wr
                if din[j] != s or dout[j] != o:
                    din[j] = s; dout[j] = o; changed = True
        regs = [r for r in ALL_REGS if r in used]
        # cost: write-backs emitted
        wb = 0
        for j in range(n):
            kind = info[j][0]
            if kind:
                wb += len(din[j] & used)
        last = n - 1
        while last >= 0 and lines[k + last].lstrip().startswith('//'):
            last -= 1
        tail_dirty = (dout[last] & used) if last >= 0 and not info[last][4] else empty
        wb += len(tail_dirty)
        if refs < self.min_gain * max(wb, 1):
            st['skip_gain'] += 1; st['refs_skipped'] += refs; return None
        sub = lambda ln: self.ref_re.sub(lambda m: m.group(1), ln)
        order = lambda ds: [r for r in regs if r in ds]
        out = [lines[s] for s in range(start + 1, k)]
        out.append('\t' + MARK)
        out.extend('\tPPCRegister %s = ctx.%s;' % (r, r) for r in regs)
        for j in range(n):
            i = k + j
            ln = lines[i]
            kind = info[j][0]
            if kind is None:
                out.append(sub(ln)); continue
            dirty = order(din[j])
            ind = re.match(r'^\s*', ln).group(0) or '\t'
            if kind == 'cret':
                m = COND_RETURN_RE.match(ln)
                if dirty:
                    spill = ' '.join('ctx.%s = %s;' % (r, r) for r in dirty)
                    out.append('%sif (%s) { %s return; }' % (m.group(1), sub(m.group(2)), spill))
                else:
                    out.append(sub(ln))
                continue
            out.extend('%sctx.%s = %s;' % (ind, r, r) for r in dirty)
            out.append(sub(ln))
            if kind == 'esc':
                out.extend('%s%s = ctx.%s;' % (ind, r, r) for r in regs)
        out.extend('\tctx.%s = %s;' % (r, r) for r in order(tail_dirty))
        st['done'] += 1; st['refs_done'] += refs; st['wb'] += wb
        st['funcs'].append((refs, name, wb))
        return out


def main():
    args = sys.argv[1:]
    d = args[0]
    dry = '--dry-run' in args
    min_gain = float(args[args.index('--min-gain') + 1]) if '--min-gain' in args else 1.0
    report = args[args.index('--report') + 1] if '--report' in args else None
    regs = args[args.index('--regs') + 1].split(',') if '--regs' in args else ALL_REGS
    P = Pass(regs, min_gain)
    total_refs = 0
    files = sorted(glob.glob(os.path.join(d, 'ppc_recomp.*.cpp')))
    for path in files:
        text = open(path, encoding='utf-8', newline='').read()
        if MARK in text:
            sys.exit('already applied: ' + path)
        lines = text.split('\n')
        total_refs += sum(len(P.ref_re.findall(ln)) for ln in lines if not ln.lstrip().startswith('//'))
        new, i, changed = [], 0, False
        while i < len(lines):
            if FUNC_RE.match(lines[i]):
                e = i + 1
                while e < len(lines) and lines[e] != '}':
                    e += 1
                body = P.process_function(lines, i, e)
                new.append(lines[i])
                if body is None:
                    new.extend(lines[i + 1:e])
                else:
                    new.extend(body); changed = True
                new.append(lines[e] if e < len(lines) else '')
                i = e + 1
                continue
            new.append(lines[i]); i += 1
        if changed and not dry:
            open(path, 'w', encoding='utf-8', newline='').write('\n'.join(new))
    s = P.stats
    print('ctx refs (%s): %d total; promoted %d in %d functions (%d write-backs); skipped: gain %d (%d refs), '
          'unsafe %d, clash %d, braceless %d, same-line %d, loop %d, label %d, brace %d, stmt %d'
          % (','.join(regs), total_refs, s['refs_done'], s['done'], s['wb'], s['skip_gain'], s['refs_skipped'],
             s['skip_unsafe'], s['skip_clash'], s['skip_braceless'], s['skip_sameline'], s['skip_loop'], s['skip_label'], s['skip_brace'], s['skip_stmt']))
    if report:
        with open(report, 'w') as f:
            for refs, name, wb in sorted(s['funcs'], reverse=True):
                f.write('%s refs=%d writebacks=%d\n' % (name, refs, wb))


if __name__ == '__main__':
    main()
