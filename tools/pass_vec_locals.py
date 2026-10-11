"""Keep the volatile vector registers v0-v13 in locals inside recompiled functions (2026-10-11).

Locals mode (XenonRecomp *_as_local) made r0/r2/r11/r12, r14-r31, f0, f14-f31, v14-v127 locals, but v0-v13 (the
VMX argument / scratch registers) still live in PPCContext: every simde operation on them loads and stores through
ctx, and a guest memory store (a plain SIMD store through PPC_HOST) may alias ctx as far as the compiler knows, so
they are reloaded after each one. Hot animation / skinning functions (sub_8231ED30, sub_8231D470) are full of them.

This pass promotes them per function without changing the calling convention, so ctx-mode semantics are kept:
  - at entry, each ctx.vN the function uses is copied to a local vN;
  - before every line that hands ctx to other code (a direct call, PPC_CALL_INDIRECT_FUNC, a host helper taking
    ctx) and before every return (and the closing brace), the locals are written back to ctx;
  - after every such line, they are reloaded (the callee may change any of them).
Only functions where this saves work are changed: (references to ctx.vN) >= MIN_GAIN x (copies the pass adds).
Functions it cannot rewrite safely are skipped: setjmp / longjmp / try (locals may be stale after a non-local
jump), a call or return in the body of a braceless if/else/loop, a line that uses a promoted register after a
call on the same line, a name clash with an existing vN.

Usage: python pass_vec_locals.py <output_dir> [--dry-run] [--min-gain X] [--report FILE]
Edits ppc_recomp.*.cpp in place (back up the directory first). Idempotent guard: refuses to run twice.
"""
import re, sys, os, glob

MARK = '// LSW_VECLOCAL_PASS'
FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\) \{')
CTXV_RE = re.compile(r'\bctx\.v(\d+)\b')
ESCAPE_RE = re.compile(r'\bctx\b(?!\.)|PPC_CALL_INDIRECT_FUNC|PPC_CALL_FUNC')
RETURN_RE = re.compile(r'\breturn\b')
COND_RETURN_RE = re.compile(r'^(\s*)if \((.*)\) return;\s*$')
BRACELESS_RE = re.compile(r'^\s*(?:\}\s*)?(?:if\b|else\b|for\b|while\b).*[^;{}\s]\s*$')
UNSAFE_RE = re.compile(r'setjmp|longjmp|\btry\b|\bcatch\b')
DECL_RE = re.compile(r'^\t(?:PPC\w*Register|uint32_t) \w+\{\};$|^\tPPC_FUNC_PROLOGUE\(\);$|^\t\w+ = g_lsw_live\.\w+;')


def code_line_before(lines, i, lo):
    j = i - 1
    while j >= lo:
        s = lines[j].strip()
        if s and not s.startswith('//'):
            return lines[j]
        j -= 1
    return ''


def process_function(lines, start, end, min_gain, stats):
    """lines[start] = PPC_FUNC_IMPL line, lines[end] = closing brace. Returns new body lines or None."""
    name = FUNC_RE.match(lines[start]).group(1)
    body = lines[start + 1:end]
    refs = 0
    used = set()
    for ln in body:
        for m in CTXV_RE.finditer(ln):
            used.add(int(m.group(1))); refs += 1
    if not used:
        return None
    if max(used) > 13:
        stats['skip_other_v'] += 1; return None
    text = '\n'.join(body)
    if UNSAFE_RE.search(text):
        stats['skip_unsafe'] += 1; return None
    code = '\n'.join(ln for ln in body if not ln.lstrip().startswith('//'))   # asm comments name v0..v13
    for n in used:
        if re.search(r'(?<!ctx\.)\bv%d\b' % n, code):
            stats['skip_clash'] += 1; return None
    # entry point for the loads: after the declaration / prologue / live-in lines
    k = start + 1
    while k < end and DECL_RE.match(lines[k]):
        k += 1
    sites = []   # (index, is_escape)
    for i in range(k, end):
        ln = lines[i]
        if ln.lstrip().startswith('//'):
            continue
        esc = bool(ESCAPE_RE.search(ln))
        ret = bool(RETURN_RE.search(ln))
        if not esc and not ret:
            continue
        if BRACELESS_RE.match(code_line_before(lines, i, k)):
            stats['skip_braceless'] += 1; return None
        if esc:
            after = ESCAPE_RE.split(ln, 1)[1] if ESCAPE_RE.search(ln) else ''
            if CTXV_RE.search(after):
                stats['skip_sameline'] += 1; return None
        sites.append((i, esc))
    nu = len(used)
    cost = nu + sum(2 * nu if esc else nu for _, esc in sites) + nu   # entry + sites + closing brace
    if refs < min_gain * cost:
        stats['skip_gain'] += 1; stats['refs_skipped'] += refs; return None
    regs = sorted(used)
    out = []
    site_set = dict(sites)
    for i in range(start + 1, end):
        if i == k:
            ind = '\t'
            out.append(ind + MARK)
            out.extend('%sPPCVRegister v%d = ctx.v%d;' % (ind, n, n) for n in regs)
        ln = lines[i]
        cond_ret = COND_RETURN_RE.match(ln)
        if i in site_set and cond_ret and not site_set[i]:
            # if (c) return;  ->  if (c) { <write back>; return; }  (no stores on the path that continues)
            spill = ' '.join('ctx.v%d = v%d;' % (n, n) for n in regs)
            out.append('%sif (%s) { %s return; }' % (cond_ret.group(1), CTXV_RE.sub(lambda m: 'v' + m.group(1), cond_ret.group(2)), spill))
            continue
        if i in site_set:
            ind = re.match(r'^\s*', ln).group(0) or '\t'
            out.extend('%sctx.v%d = v%d;' % (ind, n, n) for n in regs)
            out.append(CTXV_RE.sub(lambda m: 'v' + m.group(1), ln))
            if site_set[i]:
                out.extend('%sv%d = ctx.v%d;' % (ind, n, n) for n in regs)
            continue
        out.append(CTXV_RE.sub(lambda m: 'v' + m.group(1), ln))
    if k == end:
        return None
    # falling off the end returns too
    out.extend('\tctx.v%d = v%d;' % (n, n) for n in regs)
    stats['done'] += 1; stats['refs_done'] += refs
    stats['funcs'].append((refs, name, len(sites)))
    return out


def main():
    args = sys.argv[1:]
    d = args[0]
    dry = '--dry-run' in args
    min_gain = float(args[args.index('--min-gain') + 1]) if '--min-gain' in args else 1.0
    report = args[args.index('--report') + 1] if '--report' in args else None
    stats = dict(done=0, refs_done=0, refs_skipped=0, skip_gain=0, skip_unsafe=0, skip_clash=0,
                 skip_braceless=0, skip_sameline=0, skip_other_v=0, funcs=[])
    total_refs = 0
    files = sorted(glob.glob(os.path.join(d, 'ppc_recomp.*.cpp')))
    for path in files:
        text = open(path, encoding='utf-8', newline='').read()
        if MARK in text:
            sys.exit('already applied: ' + path)
        total_refs += len(CTXV_RE.findall(text))
        lines = text.split('\n')
        new, i, changed = [], 0, False
        while i < len(lines):
            if FUNC_RE.match(lines[i]):
                e = i + 1
                while e < len(lines) and lines[e] != '}':
                    e += 1
                body = process_function(lines, i, e, min_gain, stats)
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
    s = stats
    print('ctx.v refs: %d total; promoted %d in %d functions; skipped: gain %d (%d refs), unsafe %d, clash %d, '
          'braceless %d, same-line %d, other v %d' % (total_refs, s['refs_done'], s['done'], s['skip_gain'],
          s['refs_skipped'], s['skip_unsafe'], s['skip_clash'], s['skip_braceless'], s['skip_sameline'], s['skip_other_v']))
    if report:
        with open(report, 'w') as f:
            for refs, name, nsites in sorted(s['funcs'], reverse=True):
                f.write('%s refs=%d sites=%d\n' % (name, refs, nsites))


if __name__ == '__main__':
    main()
