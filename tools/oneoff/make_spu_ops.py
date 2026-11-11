"""One-off refactor: turn the SPU interpreter's switch cases into inline per-op functions (spu_ops.h)."""
import re
import sys

src = open(sys.argv[1]).read()
start = src.index('        switch (g_spu_decode[w >> 21]) {')
end = src.index('            case SPU_INVALID:')
body = src[start:end].split('\n', 1)[1]

# split into case groups at 12-space indentation
lines = body.split('\n')
groups, cur_labels, cur_body = [], [], []
label_re = re.compile(r'^            case (SPU_\w+):(.*)$')
for ln in lines:
    m = label_re.match(ln)
    if m:
        # several labels may sit on one line: "case SPU_a: case SPU_b: {"
        rest = ln.strip()
        labels = re.findall(r'case (SPU_\w+):', rest)
        tail = re.sub(r'(case SPU_\w+:\s*)+', '', rest, count=1)
        if cur_body and any(x.strip() for x in cur_body):
            groups.append((cur_labels, cur_body))
            cur_labels, cur_body = [], []
        cur_labels += labels
        if tail:
            cur_body.append('            ' + tail)
    elif ln.strip().startswith('//'):
        continue  # divider comments between case groups
    else:
        cur_body.append(ln)
if cur_labels:
    groups.append((cur_labels, cur_body))

out = ['// Generated once from spu_interp.cpp by tools/oneoff/make_spu_ops.py, then maintained by hand.',
       '// One inline function per SPU instruction, shared by the interpreter and the SPU recompiler.',
       '// exec_<op>(t, w, pc, npc): returns 0 to continue (npc = next pc), or SPU_EXIT(code) to stop.',
       '#pragma once', '', '#include "spu_helpers.h"', '']
for labels, b in groups:
    text = '\n'.join(b).rstrip()
    # strip one level of braces if the body is "{ ... }"
    t = text.strip()
    if t.startswith('{') and t.endswith('}'):
        t = t[1:-1]
    t = t.strip()
    # terminal break -> return 0
    if t.endswith('break;'):
        t = t[: -len('break;')].rstrip() + '\n    return 0;'
    # remaining breaks would be a problem (inside loops they are fine only if not case-terminating)
    t = re.sub(r'return (0xFFFFFFF[DEF]|w & 0x3FFF|0);', lambda m: f'return SPU_EXIT({m.group(1)});', t)
    for lab in labels:
        name = lab[4:]
        bb = t.replace('g_spu_decode[w >> 21]', lab)
        out.append(f'SPU_IMPL({name}) {{')
        out.append('    SPU_PROLOGUE;')
        for ln in bb.split('\n'):
            ln = ln.rstrip()
            if not ln:
                continue
            # normalise indentation to 4 spaces inside the function
            stripped = ln.lstrip(' ')
            indent = len(ln) - len(stripped)
            extra = max(0, indent - 16) if indent >= 16 else 0
            out.append('    ' + ' ' * extra + stripped)
        if not bb.rstrip().endswith('return 0;') and 'return SPU_EXIT' not in bb.split('\n')[-1]:
            out.append('    return 0;')
        out.append('}')
        out.append('')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
print(f"{sum(len(l) for l, _ in groups)} ops in {len(groups)} groups")
