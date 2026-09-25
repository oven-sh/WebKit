#!/usr/bin/env python3
"""auto_permode.py <unstripped jsc> <source root as built> <tree to edit> <symbols file> [--dry]

Compile the listed functions once per threads mode, in place (JSC_PER_THREADS_MODE_BEGIN / _END, ThreadsModePage.h).
<symbols file>: one demangled function name per line, as objdump -C prints it (gate-exec.py's third column).
The definition of each is found through the binary's line table; a function is skipped, with the reason, when the
definition is not one this tool can rewrite safely. Prints what it did; writes <symbols file>.done with the converted ones
as "file<TAB>first line of the signature<TAB>name"."""
import os, re, subprocess, sys, collections

jsc, built, tree, symfile = sys.argv[1:5]
dry = '--dry' in sys.argv
wanted = [l.rstrip('\n') for l in open(symfile) if l.strip()]

# demangled name -> address
addr = {}
for l in subprocess.run(['nm', '-C', '--defined-only', jsc], capture_output=True, text=True, errors='replace').stdout.splitlines():
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW':
        addr.setdefault(p[2], p[0])
found = [(n, addr[n]) for n in wanted if n in addr]
missing = [n for n in wanted if n not in addr]
out = subprocess.run([os.environ.get('LLVM_SYMBOLIZER', 'llvm-symbolizer'), '--verbose', '-e', jsc] + ['0x' + a for _, a in found], capture_output=True, text=True, errors='replace').stdout
loc = {}
groups = [g for g in out.split('\n\n') if g.strip()]
for (n, _), g in zip(found, groups):
    f = None; ln = None
    for l in g.split('\n'):
        l = l.strip()
        if l.startswith('Function start filename:'): f = l.split(':', 1)[1].strip()
        elif l.startswith('Function start line:'): ln = l.split(':', 1)[1].strip()
    if f and ln and ln.isdigit() and int(ln):
        loc[n] = (os.path.normpath(f), int(ln))
headers = '--headers' in sys.argv

KEYWORDS = r'\b(static|inline|ALWAYS_INLINE|NEVER_INLINE|NODELETE|JS_EXPORT_PRIVATE|WTF_EXPORT_PRIVATE|SYSV_ABI|JIT_OPERATION_ATTRIBUTES|JSC_HOST_CALL_ATTRIBUTES|virtual|explicit|SUPPRESS_ASAN|REFERENCED_FROM_ASM|WTF_INTERNAL|NO_RETURN|NO_RETURN_DUE_TO_CRASH|LIFETIME_BOUND|CONCURRENT_SAFE|SUPPRESS_TSAN)\b'
MACROS = {'JSC_DEFINE_HOST_FUNCTION': 'JSC::EncodedJSValue', 'JSC_DEFINE_COMMON_SLOW_PATH': None, 'LLINT_SLOW_PATH_DECL': None,
          'JSC_DEFINE_CUSTOM_GETTER': 'JSC::EncodedJSValue', 'JSC_DEFINE_CUSTOM_SETTER': 'bool'}

def first_arg_after_name(rest):
    depth = 0; r = ''
    for ch in rest:
        if ch in '<(': depth += 1
        if ch in '>)':
            depth -= 1
            if depth < 0: break
        if ch == ',' and depth == 0: break
        r += ch
    return r.strip()

def base_name(demangled):
    # the function's own name: last component before the parameter list, template arguments removed
    s = demangled
    depth = 0; cut = len(s)
    for i, ch in enumerate(s):
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        elif ch == '(' and depth == 0 and not s[:i].endswith('operator'):
            cut = i; break
    s = s[:cut]
    s = re.sub(r'<[^<>]*(<[^<>]*(<[^<>]*>[^<>]*)*>[^<>]*)*>', '', s)
    s = s.split(' ')[-1]
    return s.split('::')[-1], s

edits = collections.defaultdict(list)  # file -> list of (brace line index, close line index, indent, ret, name)
report = []
for n in wanted:
    if n not in loc:
        report.append(('skip', n, 'no address or line')); continue
    f, ln = loc[n]
    if not f.startswith(os.path.normpath(built) + os.sep):
        report.append(('skip', n, 'outside the tree: ' + f)); continue
    rel = os.path.relpath(f, built)
    if not rel.startswith('Source/JavaScriptCore/'):
        report.append(('skip', n, 'not JavaScriptCore: ' + rel)); continue
    if '/DerivedSources/' in f or 'WebKitBuild' in rel:
        report.append(('skip', n, 'generated: ' + rel)); continue
    path = os.path.join(tree, rel)
    try: lines = open(path).read().split('\n')
    except OSError:
        report.append(('skip', n, 'cannot read ' + rel)); continue
    if rel.endswith('.h') and not headers:
        report.append(('skip', n, 'defined in a header: ' + rel)); continue
    # the brace that opens the body: at or below the line the definition starts on
    b = None
    for i in range(max(ln - 1, 0), min(ln + 14, len(lines))):
        if re.fullmatch(r'\s*\{', lines[i]):
            b = i; break
        if i > ln - 1 and (lines[i].strip().endswith(';') or lines[i].strip() == '}'):
            break
    if b is None:
        report.append(('skip', n, 'no opening brace line near %s:%d' % (rel, ln))); continue
    indent = re.match(r'\s*', lines[b]).group(0)
    # the signature: the lines above the brace up to a blank line, a closing brace, a semicolon-terminated or a preprocessor line
    s = b - 1; sig = []
    while s >= 0:
        t = lines[s]
        st = t.strip()
        if not st or st.startswith('#') or st.endswith(';') or st.endswith('}') or st == '{' or st.startswith('//') or st.endswith(':') and re.fullmatch(r'(public|private|protected):', st):
            break
        sig.insert(0, t); s -= 1
    if not sig:
        report.append(('skip', n, 'no signature above %s:%d' % (rel, b + 1))); continue
    sigtext = ' '.join(x.strip() for x in sig)
    name, qualified = base_name(n)
    ret = None
    m = re.match(r'(JSC_DEFINE_[A-Z_]+|LLINT_SLOW_PATH_DECL)\((\w+)(.*)\)\s*$', sigtext)
    if m:
        kind = m.group(1)
        if kind in MACROS:
            ret = MACROS[kind]
            if ret is None:
                report.append(('skip', n, 'already per mode by macro')); continue
        elif kind in ('JSC_DEFINE_JIT_OPERATION', 'JSC_DEFINE_JIT_OPERATION_WITHOUT_WTF_INTERNAL'):
            ret = 'JSC::OperationReturnType<' + first_arg_after_name(m.group(3).lstrip(', ')) + '>'
        elif kind in ('JSC_DEFINE_NOEXCEPT_JIT_OPERATION',):
            ret = first_arg_after_name(m.group(3).lstrip(', '))
        else:
            report.append(('skip', n, 'macro ' + kind)); continue
    else:
        if re.search(r'\b(constexpr|consteval|operator|requires)\b', sigtext) or '->' in sigtext or sigtext.lstrip().startswith(':') or re.search(r'\)\s*:\s', sigtext) or ' : ' in sigtext.split('(')[0]:
            report.append(('skip', n, 'signature form: ' + sigtext[:80])); continue
        if re.search(r'\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?:\s*\S', sigtext) or re.match(r'\s*[:,]', sig[-1]):
            report.append(('skip', n, 'constructor with initializers')); continue
        # leading template clauses are not part of the return type
        while sigtext.lstrip().startswith('template'):
            t = sigtext.lstrip(); i = t.index('<'); depth = 0
            for j in range(i, len(t)):
                if t[j] == '<': depth += 1
                elif t[j] == '>':
                    depth -= 1
                    if not depth: break
            sigtext = t[j + 1:]
        # text before the function's name
        mm = re.search(r'(?:\b[A-Za-z_][\w]*(?:<[^()]*>)?::)*(~?' + re.escape(name) + r')\s*\(', sigtext)
        if not mm:
            report.append(('skip', n, 'name %s not in signature: %s' % (name, sigtext[:80]))); continue
        head = sigtext[:mm.start()]
        head = re.sub(r'^\s*(template\s*<[^{}]*?>\s*)+', '', head) if head.lstrip().startswith('template') else head
        head = re.sub(r'extern "C"', ' ', head)
        head = re.sub(KEYWORDS, ' ', head)
        head = re.sub(r'\[\[[^\]]*\]\]', ' ', head)
        head = ' '.join(head.split())
        if not head:
            ret = 'void /*none*/'  # constructor without initializers, or destructor
        elif head == 'auto' or 'decltype' in head:
            report.append(('skip', n, 'deduced return type')); continue
        else:
            ret = head
    # the closing brace: the next line that is exactly the indent and a brace
    c = None
    for i in range(b + 1, len(lines)):
        if lines[i] == indent + '}' or (lines[i].startswith(indent + '}') and lines[i][len(indent) + 1:].strip() in ('', ';')):
            c = i; break
        if indent and len(lines[i]) and not lines[i].startswith(indent) and lines[i].strip() and not lines[i].lstrip().startswith('#'):
            break
    if c is None:
        report.append(('skip', n, 'no closing brace for %s:%d' % (rel, b + 1))); continue
    body = '\n'.join(lines[b + 1:c])
    if 'PER_THREADS_MODE' in body or 'JSC_THREADS_MODE_BODY' in body:
        report.append(('skip', n, 'already per mode')); continue
    bad = re.search(r'\b(va_start|va_arg|co_await|co_return|co_yield|alloca|setjmp|__builtin_return_address|__builtin_frame_address|DECLARE_CALL_FRAME)\b', body)
    if bad and bad.group(1) not in ('DECLARE_CALL_FRAME', '__builtin_frame_address', '__builtin_return_address'):
        report.append(('skip', n, 'body uses ' + bad.group(1))); continue
    if c - b < 3:
        report.append(('skip', n, 'body of %d lines' % (c - b - 1))); continue
    if any(e[0] == b for e in edits[path]):
        report.append(('skip', n, 'same definition as another symbol')); continue
    edits[path].append((b, c, indent, ret, n, s + 1))
    report.append(('convert', n, '%s:%d -> %s' % (rel, b + 1, ret)))

done = []
for path, es in edits.items():
    lines = open(path).read().split('\n')
    for b, c, indent, ret, n, sigline in sorted(es, reverse=True):
        lines.insert(c, indent + ('    JSC_PER_THREADS_MODE_END_WITHOUT_RETURN' if ret.endswith('/*none*/') else '    JSC_PER_THREADS_MODE_END'))
        lines.insert(b + 1, indent + '    JSC_PER_THREADS_MODE_BEGIN(' + ret.replace(' /*none*/', '') + ')')
        done.append((os.path.relpath(path, tree), lines[sigline].strip(), n))
    if not dry:
        open(path, 'w').write('\n'.join(lines))
for k, n, why in report:
    print('%-8s %s\n         %s' % (k, n[:150], why))
print('%d converted, %d skipped, %d not in the binary' % (sum(1 for r in report if r[0] == 'convert'), sum(1 for r in report if r[0] == 'skip'), len(missing)))
if not dry:
    with open(symfile + '.done', 'a') as o:
        for d in done: o.write('\t'.join(d) + '\n')
