#!/usr/bin/env python3
# fuzz.py <jsc> <output directory> <seconds> <jobs> <seed>: mutates WebKit's JSTests/stress and compares the interpreter with ahead-of-time compiled code.
# A case counts if the interpreter runs it to an end (normally or with an uncaught exception) within the limits. It is a FINDING if, compiled ahead of time, it prints
# something else, ends otherwise, crashes, fails an assertion or an inferred type check, or hangs. Findings: <output directory>/<kind>-<id>.js, with how to run it and both outputs in a comment on top.
# Everything runs under capped.py (1.5 GB, 20 s). minimize.py <file> makes a finding smaller.
import os,re,sys,random,subprocess,time,hashlib,threading,collections
from concurrent.futures import ThreadPoolExecutor
ROOT=os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','..')); STRESS=ROOT+'/JSTests/stress'; CAP=os.path.dirname(os.path.abspath(__file__))+'/capped.py'
JSC,HERE=os.path.abspath(sys.argv[1]),os.path.abspath(sys.argv[2]); seconds,jobs,seed=float(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5]); os.makedirs(HERE,exist_ok=True)
BASE=['--useDollarVM=true','--useFunctionDotArguments=true','--maxPerThreadStackUsage=1572864','--useJIT=false','--validateExceptionChecks=true']
REF=BASE+['--useImmutableIntrinsics=true']
AOT=BASE+['--compileMainScriptAheadOfTime=true']
EXTRA=[[],['--validateAOTInferredTypes=true'],['--validateAOTInferredTypes=true','--validateGraphAtEachPhase=true'],['--forceAOTVeneers=true'],['--numberOfAOTStubCopiesForTesting=3'],
       ['--maxAOTFunctionNumberInTypesForTesting=3','--validateAOTInferredTypes=true'],['--useAOTInlineFastPathsInLoops=true'],['--forceGCSlowPaths=true'],['--slowPathAllocsBetweenGCs=13'],
       ['--validateAOTInferredTypes=true','--slowPathAllocsBetweenGCs=29']]
UNSTABLE=re.compile(r'Date\.now|new Date\(\)|Math\.random|performance\.|preciseTime|currentTime|\bload\(|\brun\(|readFile|\$vm\.(value|indexingMode|dfg|ftl|llint|baseline)|describe\(|numberOfDFGCompiles|jscStack|edenGC|fullGC|gcHeapSize|heapSize|memoryUsage|WeakRef|FinalizationRegistry|Atomics\.wait|\bagent\b|\$262|sleepSeconds|setTimeout|drainMicrotasks|generateHeapSnapshot|SamplingProfiler|createGlobalObject|runString|Loader|checkModuleSyntax|import\s*\(|^\s*import\s|^\s*export\s|OSRExit|noFTL|noDFG|neverInlineFunction|optimizeNextInvocation|reoptimizationRetryCount|functionOverrides|\.stack\b|Error\.captureStackTrace|stackTraceLimit|toLocale|Intl\.|Temporal\.|timeZone|hasOwnLengthProperty|isHavingABadTime|haveABadTime',re.M)
skip=set(l.strip().split('/')[-1] for l in open(ROOT+'/JSTests/bun-tests-that-change-builtins.txt') if l.startswith('stress/'))
corpus=[]
for f in sorted(os.listdir(STRESS)):
    if not f.endswith('.js') or f in skip: continue
    s=open(STRESS+'/'+f,errors='replace').read()
    if len(s)>12000 or UNSTABLE.search(s) or re.search(r'//@ *(skip|slow|runBytecodeCache|runFTL|runNoFTL|requireOptions\([^)]*(useJIT|FTL|DFG|Wasm|thresholdFor|validate|watchdog|Profiler|forceDebugger|useLLInt|useRandomizing|maxPerThread|exception[A-Z]|fire[A-Z]))',s): continue
    opts=re.findall(r'"(--[^"]+)"',' '.join(re.findall(r'//@ *requireOptions\((.*?)\)',s)))
    corpus.append((f,s,[o for o in opts if 'compileMainScriptAheadOfTime' not in o]))
NUM=['0','-0','1','-1','2','0.5','-0.5','1.5','255','256','65535','65536','2147483647','2147483648','-2147483648','-2147483649','4294967295','4294967296','9007199254740991','9007199254740992','1e21','1e-7','1e308','NaN','Infinity','-Infinity','0x7fffffff','1n','-1n','0n','18446744073709551616n']
VAL=['undefined','null','true','false','""','"a"','"1"','"-0"','"length"','[]','[1]','[1.5]','[,1]','["a"]','{}','{a:1}','{length:3}','{valueOf(){return 1}}','{toString(){return "x"}}','{valueOf(){throw new Error("valueOf")}}','Symbol()','Symbol.iterator','1n','function(){}','()=>1','class{}','new Map','new Set','/a/g','new Int32Array(2)','new Float64Array(2)','new Proxy({},{})','new Proxy([],{})','Object.create(null)','new String("s")','new Number(1)','arguments','this','NaN','-0','new Array(3)','Object.freeze([1,2])','Object.freeze({a:1})']
GROUPS=[['+','-','*','/','%','**'],['|','&','^','<<','>>','>>>'],['<','<=','>','>='],['==','===','!=','!=='],['&&','||','??'],['+=','-=','*=','/=','%=','|=','&=','^=','<<=','>>=','>>>=','='],['++','--'],['let','var','const'],['in','instanceof'],['break','continue'],['typeof','void','!','-','+','~','delete'],['of','in']]
TOKEN=re.compile(r'''//[^\n]*|/\*.*?\*/|`(?:\\.|[^`\\])*`|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|0[xX][0-9a-fA-F]+n?|\d+\.?\d*(?:[eE][+-]?\d+)?n?|\.\d+(?:[eE][+-]?\d+)?|[A-Za-z_$][\w$]*|>>>=|\*\*=|<<=|>>=|>>>|===|!==|&&=|\|\|=|\?\?=|=>|\*\*|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\?\?|\?\.|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\.\.|\s+|.''',re.S)
KEYWORDS=set('break case catch class const continue debugger default delete do else export extends finally for function if import in instanceof let new return super switch this throw try typeof var void while with yield async await of static get set null true false undefined NaN Infinity arguments'.split())
def statements(s):
    """Spans of whole lines whose brackets balance: things that can be moved, copied or dropped."""
    out=[];depth=0;start=None;pos=0
    for line in s.split('\n'):
        if start is None: start=pos
        t=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`[^`]*`|//.*','',line)
        depth+=sum(t.count(c) for c in '({[')-sum(t.count(c) for c in ')}]')
        pos+=len(line)+1
        if depth<=0:
            if s[start:pos].strip() and not s[start:pos].lstrip().startswith('//@'): out.append((start,min(pos,len(s))))
            start=None;depth=0
    return out
def mutate(r,s):
    k=r.randrange(14)
    if k<8:
        toks=TOKEN.findall(s); idx=list(range(len(toks)))
        ids=[t for t in toks if re.match(r'[A-Za-z_$]',t) and t not in KEYWORDS]
        for _ in range(40):
            i=r.choice(idx); t=toks[i]
            if k in (0,1) and re.match(r'(\d|\.\d)',t): toks[i]=r.choice(NUM); break
            if k in (2,3):
                g=[g for g in GROUPS if t in g]
                if g: toks[i]=r.choice([x for x in r.choice(g) if x!=t]); break
            if k==4 and re.match(r'[A-Za-z_$]',t) and t not in KEYWORDS and ids and (i==0 or toks[i-1] not in ('.','?.')): toks[i]=r.choice(ids); break
            if k==5 and (re.match(r'(\d|\.\d|"|\')',t) or t in ('null','undefined','true','false')): toks[i]='('+r.choice(VAL)+')'; break
            if k==6 and re.match(r'[A-Za-z_$]',t) and t not in KEYWORDS and i>0 and toks[i-1] in ('(',',') : toks[i]='('+r.choice(VAL)+')'; break
            if k==7 and t in (')',']') : toks[i]=t+r.choice(['|0','>>>0','+""','+0.5','*1','??0','||1','&&0','.length','?.x','[0]','+1n','-0']); break
        return ''.join(toks)
    st=statements(s)
    if not st: return s
    a,b=r.choice(st); piece=s[a:b]
    if k==8: return s[:a]+s[b:]
    if k==9: return s[:b]+piece+s[b:]
    if k==10:
        c,d=r.choice(st); return s[:d]+piece+s[d:]
    if k==11: return s[:a]+r.choice(['try {\n%s} catch (e) { print("caught", String(e)); }\n','try {\n%s} finally { print("finally"); }\n','for (let fuzz = 0; fuzz < 3; fuzz++) {\n%s}\n','if (globalThis.nothing !== 1) {\n%s}\n','(function () {\n%s})();\n','(() => {\n%s})();\n','{\n%s}\n','label: {\n%s}\n','(function* () {\n%s})().next();\n','(async function () {\n%s})();\n'])%piece+s[b:]
    if k==12:
        o=r.choice(corpus)[1]; so=[x for x in statements(o) if re.match(r'\s*(function|class|const \w+ = (function|\(|class))',o[x[0]:x[1]])]
        if so: c,d=r.choice(so); return s[:a]+o[c:d]+s[a:]
        return s
    return s[:a]+r.choice(['gc();\n','"use strict";\n','print(typeof this);\n','var fuzzed = %s;\n'%r.choice(VAL)])+s[a:]
# What the program leaves behind is printed too: most tests say nothing unless something is wrong.
EPILOGUE='''
;(function () { try { var names = Object.getOwnPropertyNames(globalThis).filter(function (n) { return !fuzzBefore.has(n) && n !== "fuzzBefore"; }).sort(); for (var i = 0; i < names.length && i < 40; i++) { var v; try { v = globalThis[names[i]]; var t = typeof v; print("global", names[i], t, t === "function" ? v.length : t === "object" && v !== null ? Object.prototype.toString.call(v) + (Array.isArray(v) ? v.length + ":" + String(v.slice(0, 8)) : "") : t === "symbol" ? "" : String(v) + (Object.is(v, -0) ? "(-0)" : "")); } catch (e) { print("global", names[i], "threw", String(e)); } } } catch (e) { print("epilogue threw", String(e)); } })();
'''
PROLOGUE='var fuzzBefore = new Set(Object.getOwnPropertyNames(globalThis));\n'
def clean(b):
    t=b.decode('utf-8','replace'); t=re.sub(r'\[capped\][^\n]*\n?','',t); t=re.sub(r'0x[0-9a-f]{6,}','0xADDR',t)
    return t
def go(path,args):
    try:
        p=subprocess.run(['python3',CAP,'1.5','20',JSC]+args+[path],cwd=STRESS,capture_output=True,timeout=40)
        killed=b'[capped] killed' in p.stderr
        return ('killed' if killed else p.returncode),clean(p.stdout),clean(p.stderr)
    except subprocess.TimeoutExpired: return 'killed','',''
stats=collections.Counter(); lock=threading.Lock(); end=time.time()+seconds
def worker(w):
    r=random.Random(seed*1000+w); tmp='%s/tmp-%d-%d.js'%(HERE,seed,w)
    while time.time()<end:
        f,s,opts=r.choice(corpus); m=s
        for _ in range(r.choice([1,1,1,2,2,3,4,6])): m=mutate(r,m)
        if m==s: continue
        head='\n'.join(l for l in m.split('\n') if l.startswith('//@'))
        open(tmp,'w').write(PROLOGUE+m+EPILOGUE)
        c0,o0,e0=go(tmp,REF+opts)
        with lock: stats['cases']+=1
        if c0=='killed' or c0 not in (0,3) or 'SyntaxError' in e0[:400] or 'SyntaxError' in o0[-400:]:
            with lock: stats['unusable']+=1
            continue
        extra=r.choice(EXTRA); c1,o1,e1=go(tmp,AOT+opts+extra)
        first=lambda t: '\n'.join(t.strip().split('\n')[:3])
        if (c1,o1,first(e1))==(c0,o0,first(e0)):
            with lock: stats['same']+=1
            continue
        # Running out of stack or memory happens at another depth in other code.
        if any(x in o0+e0+o1+e1 for x in ('Maximum call stack','Out of memory','out of memory')):
            with lock: stats['stack or memory']+=1
            continue
        kind='ASSERT' if 'ASSERTION FAILED' in e1 or 'SHOULD NEVER BE REACHED' in e1 else 'TYPE' if 'inferred type' in e1 else 'HANG' if c1=='killed' else 'CRASH' if c1 not in (0,3) else 'DIFF'
        sig=(re.findall(r'ASSERTION FAILED: ([^\n]*)',e1) or re.findall(r'(inferred type[^\n]*)',e1) or [''])[0][:80]
        h=hashlib.sha1((PROLOGUE+m).encode()).hexdigest()[:10]
        with lock:
            stats[kind]+=1
            open('%s/%s-%s.js'%(HERE,kind,h),'w').write('// from %s; %s\n// jsc %s\n// REFERENCE exit=%s\n%s// AHEAD OF TIME exit=%s %s\n%s'%(f,sig,' '.join(AOT+opts+extra),c0,''.join('//   '+l+'\n' for l in (o0[-600:]+first(e0)).split('\n')),c1,'',''.join('//   '+l+'\n' for l in (o1[-600:]+e1[:900]).split('\n')))+PROLOGUE+m+EPILOGUE)
    try: os.remove(tmp)
    except OSError: pass
print(len(corpus),'tests in the corpus',flush=True)
with ThreadPoolExecutor(jobs) as pool: list(pool.map(worker,range(jobs)))
print(dict(stats))
