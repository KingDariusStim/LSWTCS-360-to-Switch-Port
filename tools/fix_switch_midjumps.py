"""Second half of the `// ERROR` repair (after fix_switch_error_returns.py): jumps into the MIDDLE of another function.

XenonRecomp sized some functions too small (their switch case blocks or shared tails ended up in the next function(s)),
so a case or branch can target an address inside another recompiled function F. Such a jump is emitted as
`// ERROR <T>` + `return;` (2026-10-10: 474 sites, 146 targets, after the function / stub targets were fixed).

For each target T inside F:
  1. F gets a label `loc_T:` if it has none (the instruction is found by counting F's one-comment-per-instruction
     lines `\t// <insn>`; the count is checked against every label F already has, else T is skipped);
  2. a copy of F, `lsw_entry_T`, is added after F: same locals, loaded from the thread-local transfer struct
     g_lsw_jump (lsw_jump.h), then `goto loc_T;`. F's own live-in loads (LSW_LIVEIN_PASS) are dropped in the copy;
  3. every ERROR site jumping to T stores each of the copy's register locals into g_lsw_jump (the caller's local of
     that name, else ctx.<name> where PPCContext has it, else g_lsw_live.<name>) and calls lsw_entry_T, then returns.
This is what the original jump does: continue at T with the full register state.

Targets that are a plain `blr` are skipped (the existing `return;` is already right).

Usage: python fix_switch_midjumps.py <output_dir> <image.bin> [--dry-run]
Writes lsw_jump.h (transfer struct + prototypes) into <output_dir> and `#include "lsw_jump.h"` into edited files.
Marker: // LSW_SWITCH_MID
"""
import glob, os, re, sys, bisect, collections

FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{$')
DECL_RE = re.compile(r'^\t(PPC\w*Register) (\w+)\{\};$')
ERR_RE = re.compile(r'^(\s*)// ERROR(:)? (?:0x)?([0-9A-Fa-f]{8})\s*$')
LABEL_RE = re.compile(r'^loc_([0-9A-F]{8}):')
INSN_RE = re.compile(r'^\t// (?!ERROR)\S')
LIVEIN_LOAD_RE = re.compile(r'^\t\w+ = g_lsw_live\.\w+;  // LSW_LIVEIN_PASS$')
SCRATCH = {'temp', 'reserved'}
MARK = '// LSW_SWITCH_MID'
CTX_NAMES = set()    # register fields of PPCContext (locals mode drops the always-local ones), from ppc_context.h
LIVE_NAMES = set()   # fields of LswLiveRegs (pass_livein_regs.py)


class Func:
    pass


def parse(path):
    lines = open(path, encoding='utf-8', newline='').read().split('\n')
    funcs = []
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1
            continue
        f = Func()
        f.addr, f.path, f.start = int(m.group(1), 16), path, i
        j = i + 1
        while j < len(lines) and lines[j] != '}':
            j += 1
        f.end = j   # line index of the closing brace
        f.locals = collections.OrderedDict()
        for k in range(i + 1, j):
            d = DECL_RE.match(lines[k])
            if d:
                f.locals[d.group(2)] = d.group(1)
        funcs.append(f)
        i = j + 1
    return lines, funcs


def insn_index_map(lines, f):
    """Instruction comment line index for each instruction ordinal, and the labels' ordinals."""
    insns, labels = [], {}
    pending = []
    for k in range(f.start + 1, f.end):
        lm = LABEL_RE.match(lines[k])
        if lm:
            pending.append(int(lm.group(1), 16))
            continue
        if INSN_RE.match(lines[k]):
            for a in pending:
                labels[a] = len(insns)
            pending = []
            insns.append(k)
    return insns, labels


def main():
    out_dir = sys.argv[1]
    img = open(sys.argv[2], 'rb').read()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ppc_fields import struct_fields   # honours the PPC_CONFIG_*_AS_LOCAL #ifndefs
    CTX_NAMES.update(struct_fields(out_dir, 'struct alignas(0x40) PPCContext'))
    LIVE_NAMES.update(struct_fields(out_dir, 'struct LswLiveRegs'))
    dry = '--dry-run' in sys.argv
    paths = sorted(glob.glob(os.path.join(out_dir, 'ppc_recomp.*.cpp')))
    files = {}
    allf = []
    for p in paths:
        lines, funcs = parse(p)
        files[p] = lines
        allf += funcs
    allf.sort(key=lambda f: f.addr)
    starts = [f.addr for f in allf]

    # Remaining ERROR sites (not yet rewritten) and their targets.
    sites = collections.defaultdict(list)   # target -> [(path, line)]
    for p, lines in files.items():
        for k, t in enumerate(lines):
            e = ERR_RE.match(t)
            if e and MARK not in t and 'LSW_SWITCH_FIX' not in t and k + 1 < len(lines) and lines[k + 1].strip() == 'return;':
                sites[int(e.group(3), 16)].append((p, k))

    # Containing function, label (inserted if needed).
    plan = {}     # target -> Func
    skipped = []
    new_labels = collections.defaultdict(list)   # path -> [(line index, label text)]
    for t in sorted(sites):
        o = t - 0x82000000
        if 0 <= o <= len(img) - 4 and img[o:o + 4] == bytes.fromhex('4E800020'):
            continue   # blr: the plain return is right
        i = bisect.bisect_right(starts, t) - 1
        if i < 0:
            skipped.append((t, 'no function'))
            continue
        f = allf[i]
        lines = files[f.path]
        insns, labels = insn_index_map(lines, f)
        if t in labels:
            plan[t] = f
            continue
        ok = all(labels[a] == (a - f.addr) // 4 for a in labels if a >= f.addr)
        # Also the instruction count against the function's size (up to the next function), so a function without
        # labels to check against cannot get a misplaced one.
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        if nxt is not None and len(insns) != (nxt - f.addr) // 4:
            ok = False
        idx = (t - f.addr) // 4
        if not ok or idx >= len(insns) or (t - f.addr) % 4:
            skipped.append((t, 'instruction count does not match labels in sub_%08X' % f.addr))
            continue
        new_labels[f.path].append((insns[idx], 'loc_%08X:  %s' % (t, MARK)))
        plan[t] = f

    # Rewrite call sites (needs only the target function's locals).
    edited = set()
    for t, f in plan.items():
        names = [n for n in f.locals if n not in SCRATCH]
        for p, k in sites[t]:
            lines = files[p]
            # caller = the function containing line k
            caller = None
            for g in allf:
                if g.path == p and g.start < k < g.end:
                    caller = g
                    break
            ind = ERR_RE.match(lines[k]).group(1)
            new = []
            for n in names:
                if caller is not None and n in caller.locals:
                    new.append('%sg_lsw_jump.%s = %s;' % (ind, n, n))
                elif n in CTX_NAMES:
                    new.append('%sg_lsw_jump.%s = ctx.%s;' % (ind, n, n))
                elif n in LIVE_NAMES:   # always-local register the caller does not use: pass its own live-in through
                    new.append('%sg_lsw_jump.%s = g_lsw_live.%s;' % (ind, n, n))
            new += ['%slsw_entry_%08X(ctx, base);  %s' % (ind, t, MARK), '%sreturn;' % ind]
            lines[k] = lines[k] + '  ' + MARK
            lines[k + 1] = '\n'.join(new)   # joined now, split when the file is written
            edited.add(p)

    # Labels (inserted bottom-up per file so indices stay valid). Line indices refer to the original split; the call
    # site rewrite above kept line counts unchanged (one joined line per replaced 'return;').
    for p, labs in new_labels.items():
        lines = files[p]
        for k, text in sorted(labs, reverse=True):
            lines.insert(k, text)
        edited.add(p)
        # Function line ranges in this file shifted: re-parse later from the joined text.

    # Re-split and re-parse edited files, then append the entry copies after their functions.
    entry_types = {}
    for p in list(files):
        if p in edited:
            files[p] = '\n'.join(files[p]).split('\n')
    by_path = collections.defaultdict(list)
    for t, f in plan.items():
        by_path[f.path].append(t)
    for p, targets in by_path.items():
        lines = files[p]
        _, funcs = parse_lines(lines, p)
        fmap = {g.addr: g for g in funcs}
        inserts = []
        for t in targets:
            g = fmap[plan[t].addr]
            body = lines[g.start + 1:g.end]
            # Every brace-initialised declaration (register locals, `uint32_t ea{};` temporaries) goes above the goto:
            # C++ forbids jumping past an initialisation.
            isdecl = lambda l: DECL_RE.match(l) or re.match(r'^\t[\w:]+(?:<[^>]*>)? \w+\{\};$', l)
            decls = [l for l in body if isdecl(l)]
            rest = [l for l in body if not isdecl(l) and not LIVEIN_LOAD_RE.match(l) and l.strip() != 'PPC_FUNC_PROLOGUE();']
            names = [n for n in g.locals if n not in SCRATCH]
            for n in names:
                entry_types[n] = g.locals[n]
            copy = ['', '// Entry at 0x%08X inside sub_%08X (jumped to by XenonRecomp ERROR sites).  %s' % (t, g.addr, MARK),
                    'PPC_FUNC(lsw_entry_%08X) {' % t, '\tPPC_FUNC_PROLOGUE();'] + decls
            copy += ['\t%s = g_lsw_jump.%s;' % (n, n) for n in names]
            copy += ['\tgoto loc_%08X;' % t] + rest + ['}']
            inserts.append((g.end + 1, copy))
        for at, copy in sorted(inserts, key=lambda x: -x[0]):
            lines[at:at] = copy
        edited.add(p)

    # Transfer struct + prototypes.
    hdr = ['// Generated by tools/fix_switch_midjumps.py: register transfer for jumps into the middle of a function.',
           '#pragma once', '#include "ppc_context.h"', 'struct LswJumpRegs {']
    for n in sorted(entry_types, key=lambda s: (s.rstrip('0123456789'), int(re.sub(r'\D', '', s) or 0))):
        hdr.append('    %s %s;' % (entry_types[n], n))
    hdr += ['};', 'inline thread_local LswJumpRegs g_lsw_jump{};']
    hdr += ['PPC_FUNC(lsw_entry_%08X);' % t for t in sorted(plan)]

    print('targets: %d planned (%d need a new label), %d skipped; call sites rewritten: %d; files edited: %d' % (
        len(plan), sum(len(v) for v in new_labels.values()), len(skipped), sum(len(sites[t]) for t in plan), len(edited)))
    for t, why in skipped:
        print('  skipped %08X: %s' % (t, why))
    if dry:
        return
    for p in edited:
        text = '\n'.join(files[p])
        if '#include "lsw_jump.h"' not in text:
            text = text.replace('#include "ppc_recomp_shared.h"\n', '#include "ppc_recomp_shared.h"\n#include "lsw_jump.h"\n', 1)
        open(p, 'w', encoding='utf-8', newline='').write(text)
    open(os.path.join(out_dir, 'lsw_jump.h'), 'w', encoding='utf-8', newline='').write('\n'.join(hdr) + '\n')


def parse_lines(lines, path):
    tmp = Func()
    funcs = []
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1
            continue
        f = Func()
        f.addr, f.path, f.start = int(m.group(1), 16), path, i
        j = i + 1
        while j < len(lines) and lines[j] != '}':
            j += 1
        f.end = j
        f.locals = collections.OrderedDict()
        for k in range(i + 1, j):
            d = DECL_RE.match(lines[k])
            if d:
                f.locals[d.group(2)] = d.group(1)
        funcs.append(f)
        i = j + 1
    return tmp, funcs


if __name__ == '__main__':
    main()
