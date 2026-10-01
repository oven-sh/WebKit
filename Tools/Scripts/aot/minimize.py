#!/usr/bin/env python3
# minimize.py <jsc> <finding.js>: makes a finding of fuzz.py smaller while the two runs go on differing in the same way (same kind, same assertion). Writes <finding>.min.js.
import os,re,sys,subprocess
HERE=os.path.dirname(os.path.abspath(__file__)); STRESS=os.path.realpath(os.path.join(HERE,'..','..','..','JSTests','stress')); CAP=HERE+'/capped.py'
JSC,path=os.path.abspath(sys.argv[1]),os.path.abspath(sys.argv[2])
text=open(path).read(); head=[l for l in text.split('\n') if l.startswith('//')]
aot=next(l for l in head if l.startswith('// jsc '))[7:].split()
ref=[a for a in aot if not re.search(r'AOT|compileMainScript|validateGraph|forceGCSlowPaths|slowPathAllocs',a)]+['--useImmutableIntrinsics=true']
body=text[text.index('var fuzzBefore'):]
tmp=path+'.tmp.js'
def clean(b): return re.sub(r'0x[0-9a-f]{6,}','0xADDR',re.sub(r'\[capped\][^\n]*\n?','',b.decode('utf-8','replace')))
def go(args):
    try:
        p=subprocess.run(['python3',CAP,'1.5','20',JSC]+args+[tmp],cwd=STRESS,capture_output=True,timeout=40)
        return ('killed' if b'[capped] killed' in p.stderr else p.returncode),clean(p.stdout),clean(p.stderr)
    except subprocess.TimeoutExpired: return 'killed','',''
def verdict(s):
    open(tmp,'w').write(s)
    c0,o0,e0=go(ref)
    if c0 not in (0,3) or 'SyntaxError' in e0[:400]: return None
    c1,o1,e1=go(aot)
    first=lambda t:'\n'.join(t.strip().split('\n')[:3])
    if (c1,o1,first(e1))==(c0,o0,first(e0)): return None
    if any(x in o0+e0+o1+e1 for x in ('Maximum call stack','Out of memory')): return None
    if 'ASSERTION FAILED' in e1: return 'ASSERT '+(re.findall(r'ASSERTION FAILED: ([^\n]*)',e1)+[''])[0][:60]
    if 'inferred type' in e1: return 'TYPE'
    return 'HANG' if c1=='killed' else 'CRASH' if c1 not in (0,3) else 'DIFF'
want=verdict(body)
if not want: print('does not reproduce'); os.remove(tmp); sys.exit(1)
print('reproduces:',want,'|',len(body),'bytes')
lines=body.split('\n'); n=max(1,len(lines)//2)
while n>=1:
    i=0; changed=False
    while i<len(lines):
        t=lines[:i]+lines[i+n:]
        if verdict('\n'.join(t))==want: lines=t; changed=True
        else: i+=n
    if n==1 and not changed: break
    n=max(1,n//2) if not (n==1 and changed) else 1
s='\n'.join(lines)
# Then pieces of lines: whatever is between matching brackets, arguments, statements on one line.
for pattern in (r'\{[^{}]*\}',r'\([^()]*\)',r'\[[^\[\]]*\]',r'[^;{}]+;',r',[^,()]+'):
    again=True
    while again:
        again=False
        for m in reversed(list(re.finditer(pattern,s))):
            for repl in ('','{}' if pattern.startswith(r'\{') else '()' if pattern.startswith(r'\(') else '[]' if pattern.startswith(r'\[') else ''):
                t=s[:m.start()]+repl+s[m.end():]
                if t!=s and verdict(t)==want: s=t; again=True; break
open(path[:-3]+'.min.js','w').write('// '+want+'\n// jsc '+' '.join(aot)+'\n'+s+'\n'); os.remove(tmp)
print(len(s),'bytes:',path[:-3]+'.min.js')
