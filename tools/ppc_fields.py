"""Register fields a struct in ppc_context.h really has under the PPC_CONFIG_* macros of ppc_config.h (locals mode
compiles out the registers it keeps as locals: `#ifndef PPC_CONFIG_..._AS_LOCAL` blocks)."""
import os, re


def struct_fields(out_dir, struct):
    cfg = open(os.path.join(out_dir, 'ppc_config.h'), encoding='utf-8').read()
    defined = set(re.findall(r'^#define (PPC_CONFIG_\w+)', cfg, re.M))
    h = open(os.path.join(out_dir, 'ppc_context.h'), encoding='utf-8').read()
    a = h.index(struct)
    body = h[a:h.index('\n};', a)].split('\n')
    stack, out = [], set()
    for line in body:
        t = line.strip()
        m = re.match(r'#ifndef (\w+)', t)
        if m:
            stack.append(m.group(1) not in defined); continue
        m = re.match(r'#ifdef (\w+)', t)
        if m:
            stack.append(m.group(1) in defined); continue
        if t.startswith('#else'):
            stack[-1] = not stack[-1]; continue
        if t.startswith('#endif'):
            stack.pop(); continue
        if all(stack):
            out.update(re.findall(r'PPC\w*Register (\w+);', line))
    return out
