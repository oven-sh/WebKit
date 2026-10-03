#!/usr/bin/env python3
"""Generates programs and compares the interpreter with ahead-of-time compiled code.

  fuzz-programs.py <jsc> <output directory> [--generator closures|objects|loops|numbers|classes|all] [--seconds N] [--jobs N] [--seed N]
  fuzz-programs.py <jsc> --minimize <finding.js> [--limit SECONDS]

closures: functions nested up to four deep (declarations, expressions, arrows, methods, async functions, generators), parameters
with default values (which split a function's scope in two), let, const and var in blocks and loops, variables assigned before and
after the closures that read them are made, closures that are called directly, handed to a local helper that calls them (and is
inlined) or kept in an array and called later, try, catch and finally, and now and then a direct eval. Calls burn fuel, so every
program ends.

objects: a few functions that call each other without cycles, over objects of several shapes (literals, constructors, classes with
accessors, arrays with and without holes, typed arrays, objects that inherit their properties). The same object is often passed
twice. Properties are read before and after calls that may write them, in loops and in closures; objects are made that may or may
not escape; and shapes change under way: properties are deleted, become accessors or read-only, prototypes are replaced, objects
are frozen. The functions run for up to thirty rounds, so that caches fill before something changes.

loops: loops of a dozen forms over arrays of every indexing type, with and without holes, typed arrays, their subclasses, views of
buffers that can be resized, array-likes, strings and arguments objects. Elements are read and written at and around the index,
and in some iteration of some round the array changes: it grows, shrinks, gets another kind of element, an accessor or a length of
its own, another prototype, or its buffer is resized or detached. Loops that may grow what they run over count their steps.

numbers: expressions of every operator and many functions of Math over values at the edges of int32, uint32 and double, signed
zeros, strings that are and are not numbers, objects that convert, and now and then a BigInt; variables that are counted up and
down past those edges in loops, stored to typed arrays and switched on; functions whose arguments are of another kind each round;
and a few small expressions over every pair of the edge values, in a loop, where arithmetic is compiled inline.

classes: classes that extend each other, built-in classes, a plain function or null, with public, private and static fields,
private methods, accessors, static blocks, constructors that log before and after super() or return another object, methods that
are static, async or generators, and super in all of them and in object literals; iterators whose return() is logged, left by
break, continue, return and throw; destructuring with defaults and rests; spread; generators resumed, closed and thrown into;
thenables; tagged templates; and operands that log when they are evaluated.

What happens goes to a log, errors that are caught by the name of their class and the start of their message (a long expression
that a message quotes is cut short in an image). A program counts if the interpreter runs it to an end without
running out of stack, which happens at another depth in other code. It is a finding if compiled code prints something else, ends
otherwise, crashes or hangs: <output directory>/<kind>-<seed>.js, whose first line has the options. Half of the programs run as
modules, and the options rotate through the validating and testing modes. Everything runs under capped.py (1.5 GB, 20 s).

--minimize removes lines while the interpreter still runs the program to an end and compiled code still fails in the same way, and
writes <finding>.min.js. Give a hang a --limit just above what the interpreter needs.
"""
import argparse, os, random, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capped.py')
REFERENCE = ['--useJIT=false', '--useDollarVM=true', '--useImmutableIntrinsics=true', '--hideFunctionSourceForTesting=true']
COMPILED = ['--useJIT=false', '--useDollarVM=true', '--compileMainScriptAheadOfTime=true']
CONFIGS = [[], ['--validateAOTInferredTypes=true', '--validateGraphAtEachPhase=true'], ['--maxAOTFunctionNumberInTypesForTesting=3', '--validateAOTInferredTypes=true'], ['--useAOTDataStubs=false'],
           ['--slowPathAllocsBetweenGCs=31'], ['--useAOTLoopSplitting=false'], ['--useAOTInlining=false'], ['--validateAOTInferredTypes=true', '--slowPathAllocsBetweenGCs=47']]


class Closures:
    """Scopes, and the closures that keep them."""

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0
        self.useEval = self.r.random() < 0.12

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def pick(self, scopes, kinds=('v', 'c', 'p')):
        names = [n for s in scopes for n, k in s if k in kinds]
        return self.r.choice(names) if names else None

    def value(self, scopes, depth=0):
        r = self.r
        k = r.randrange(12)
        v = self.pick(scopes)
        if k < 5 and v:
            return v
        if k == 5:
            return str(r.randrange(-3, 100))
        if k == 6:
            return '"s%d"' % r.randrange(9)
        if k == 7 and v and depth < 2:
            return '(%s + %s)' % (v, self.value(scopes, depth + 1))
        if k == 8:
            return r.choice(['undefined', 'null', 'true', '1.5', '[1, 2]', '{ a: 1 }'])
        f = self.pick(scopes, ('f',))
        if k in (9, 10) and f and depth < 2:
            return 'tryCall(() => %s(%s))' % (f, ', '.join(self.value(scopes, depth + 1) for _ in range(r.randrange(3))))
        return v or '0'

    def closure(self, scopes, level, indent):
        """An expression that makes a closure."""
        r = self.r
        kind = r.randrange(7)
        if kind < 3 or level >= 4:
            body = r.randrange(4)
            v = self.pick(scopes) or '0'
            p = self.name('a')
            if body == 0:
                return '() => %s' % v
            if body == 1:
                return '%s => show(%s) + show(%s)' % (p, p, self.value(scopes))
            w = self.pick(scopes, ('v', 'p'))
            if body == 2 and w:
                return '%s => { %s = %s; return %s; }' % (p, w, r.choice([p, '%s + 1' % w, self.value(scopes)]), v)
            return '() => [%s].map(show).join()' % ', '.join(self.value(scopes) for _ in range(r.randrange(1, 4)))
        head = {3: 'function ', 4: 'async function ', 5: 'function* ', 6: 'async '}[kind]
        return self.function(scopes, level + 1, indent, head, expression=True)

    def function(self, scopes, level, indent, head='function ', name=None, expression=False):
        r = self.r
        pad = '    ' * indent
        params, own = [], []
        for i in range(r.randrange(4)):
            p = self.name('p')
            if own and r.random() < 0.3:
                params.append('%s = %s' % (p, r.choice([own[-1][0], self.value(scopes + [own]), '() => %s' % own[-1][0]])))
            else:
                params.append(p)
            own.append((p, 'p'))
        isArrow = head == 'async '
        isGenerator = head == 'function* '
        isAsync = head.startswith('async')
        text = '%s%s(%s)%s {\n' % (head if not isArrow else 'async ', '' if isArrow else (name or ''), ', '.join(params), ' =>' if isArrow else '')
        text += pad + '    burn();\n'
        text += self.block(scopes + [own], level, indent + 1, r.randrange(2, 7), isAsync, isGenerator)
        shown = [n for n, k in own] + [self.pick(scopes) or '0']
        text += pad + '    return [%s].map(show).join("|");\n' % ', '.join(shown[:5])
        text += pad + '}'
        return text

    def block(self, scopes, level, indent, count, isAsync=False, isGenerator=False):
        r = self.r
        pad = '    ' * indent
        own = scopes[-1]
        text = ''
        for _ in range(count):
            k = r.randrange(22)
            if k < 4:
                n = self.name('v')
                kw = r.choice(['let', 'let', 'const', 'var'])
                text += pad + '%s %s = %s;\n' % (kw, n, self.value(scopes))
                own.append((n, 'c' if kw == 'const' else 'v'))
            elif k < 7:
                n = self.name('f')
                text += pad + 'const %s = %s;\n' % (n, self.closure(scopes, level, indent))
                own.append((n, 'f'))
            elif k == 7 and level < 4:
                n = self.name('f')
                own.append((n, 'f'))
                text += pad + self.function(scopes, level + 1, indent, r.choice(['function ', 'function ', 'async function ', 'function* ']), n) + '\n'
            elif k < 10:
                w = self.pick(scopes, ('v', 'p'))
                if w:
                    text += pad + '%s = %s;\n' % (w, r.choice([self.value(scopes), '%s + 1' % w, w]))
            elif k < 12:
                f = self.pick(scopes, ('f',))
                if f:
                    text += pad + 'note(tryCall(() => %s(%s)));\n' % (f, ', '.join(self.value(scopes) for _ in range(r.randrange(3))))
            elif k == 12:
                f = self.pick(scopes, ('f',))
                text += pad + 'kept.push(%s);\n' % (f if f and r.random() < 0.5 else self.closure(scopes, level, indent))
            elif k == 13:
                text += pad + 'note(mapLike([1, 2], %s));\n' % self.closure(scopes, 4, indent)
            elif k == 14:
                i = self.name('i')
                inner = [(i, 'v')]
                text += pad + 'for (let %s = 0; %s < %d; %s++) {\n' % (i, i, r.randrange(1, 4), i) + self.block(scopes + [inner], level, indent + 1, r.randrange(1, 4), isAsync, isGenerator) + pad + '}\n'
            elif k == 15:
                text += pad + '{\n' + self.block(scopes + [[]], level, indent + 1, r.randrange(1, 4), isAsync, isGenerator) + pad + '}\n'
            elif k == 16:
                e = self.name('e')
                text += pad + 'try {\n' + self.block(scopes + [[]], level, indent + 1, r.randrange(1, 4), isAsync, isGenerator)
                if r.random() < 0.4:
                    text += pad + '    throw %s;\n' % self.value(scopes)
                text += pad + '} catch (%s) {\n' % e + self.block(scopes + [[(e, 'v')]], level, indent + 1, r.randrange(1, 3), isAsync, isGenerator) + pad + '}'
                if r.random() < 0.3:
                    text += ' finally {\n' + self.block(scopes + [[]], level, indent + 1, 1, isAsync, isGenerator) + pad + '}'
                text += '\n'
            elif k == 17 and isAsync:
                n = self.name('v')
                text += pad + 'const %s = await %s;\n' % (n, self.value(scopes))
                own.append((n, 'c'))
            elif k == 17 and isGenerator:
                text += pad + 'yield %s;\n' % self.value(scopes)
            elif k == 18:
                text += pad + 'if (%s) {\n' % self.value(scopes) + self.block(scopes + [[]], level, indent + 1, r.randrange(1, 3), isAsync, isGenerator) + pad + '}\n'
            elif k == 19 and self.useEval and r.random() < 0.5:
                v = self.pick(scopes) or '0'
                text += pad + 'note(tryCall(() => eval("%s")));\n' % r.choice([v, 'typeof %s' % v, '%s + 1' % v])
            elif k == 20:
                n = self.name('o')
                m = self.name('m')
                text += pad + 'const %s = { %s() { burn(); return show(%s); }, get g() { return %s; } };\n' % (n, m, self.value(scopes), self.value(scopes))
                text += pad + 'note(%s.%s() + show(%s.g));\n' % (n, m, n)
            else:
                text += pad + 'note(%s);\n' % self.value(scopes)
        return text

    def program(self):
        head = '''let fuel = 700;
const log = [], kept = [];
class OutOfFuel extends Error { }
function burn() { if (--fuel < 0) throw new OutOfFuel("fuel"); }
function show(x) {
    if (typeof x === "function") return "function";
    if (typeof x === "object" && x !== null) {
        if (typeof x.then === "function") { x.then(v => log.push("then " + show(v)), e => log.push("rejected " + show(e))); return "promise"; }
        if (typeof x.next === "function") { let s = "generator"; for (let i = 0; i < 3; i++) { const t = tryCall(() => x.next()); if (typeof t !== "object" || t === null || t.done) break; s += ":" + show(t.value); } return s; }
        if (x instanceof Error) return x.constructor.name;
        if (Array.isArray(x)) return "[" + x.map(show).join() + "]";
        return "{" + Object.keys(x).join() + "}";
    }
    return typeof x === "string" ? x.slice(0, 40) : String(x);
}
function note(x) { log.push(show(x)); }
function tryCall(f) { try { burn(); return f(); } catch (e) { return e instanceof Error ? e.constructor.name + ": " + e.message.slice(0, 32) : "thrown " + show(e); } }
function main() {
    function mapLike(array, f) { const result = []; for (let i = 0; i < array.length; i++) result.push(tryCall(() => f(array[i], i))); return result; }
'''
        body = self.block([[('mapLike', 'x')]], 1, 1, self.r.randrange(6, 14))
        tail = '''}
for (let round = 0; round < 3; round++) {
    note(tryCall(main));
    for (let i = 0; i < kept.length && i < 40; i++) note(tryCall(() => kept[i](round, i)));
    kept.length = 0;
    drainMicrotasks();
}
print(log.join("\\n"));
'''
        return head + body + tail


class Objects:
    """Objects of a few shapes, aliases of them, and functions that read and write their properties around calls."""
    PROPERTIES = ['x', 'y', 'z']

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def primitive(self):
        return self.r.choice(['0', '1', '2', '-1', '7', '1.5', '-0', '2147483647', '"s"', '"1"', 'undefined', 'null', 'true', 'NaN'])

    def make(self):
        r = self.r
        p = self.primitive
        return r.choice([
            lambda: '{ x: %s, y: %s }' % (p(), p()), lambda: '{ y: %s, x: %s }' % (p(), p()), lambda: '{ x: %s }' % p(), lambda: '{ x: %s, y: %s, z: %s }' % (p(), p(), p()),
            lambda: 'new Point(%s, %s)' % (p(), p()), lambda: 'new Base(%s)' % p(), lambda: 'new Derived(%s, %s)' % (p(), p()), lambda: 'Object.create(shared)',
            lambda: '[%s, %s]' % (p(), p()), lambda: '[%s, , %s]' % (p(), p()), lambda: '{ x: %s, get y() { getterCalls++; return this.x; } }' % p(),
            lambda: '{ __proto__: shared, y: %s }' % p(), lambda: 'new Float64Array([1.5, 2])', lambda: 'Object.freeze({ x: %s, y: %s })' % (p(), p()),
        ])()

    def key(self):
        return self.r.choice(['"x"', '"y"', '"z"', '0', '1', '"length"', 'key'])

    def read(self, objects):
        r = self.r
        o = r.choice(objects)
        k = r.randrange(12)
        if k < 6:
            return '%s.%s' % (o, r.choice(self.PROPERTIES))
        if k < 8:
            return '%s[%s]' % (o, self.key())
        return r.choice(['%s?.x', '%s.length', '%s.twice', '%s.sum?.()', '("y" in %s)', 'Object.keys(%s).join()', 'typeof %s.y', '%s.hasOwnProperty("x")']) % o

    def expression(self, objects, values, depth=0):
        r = self.r
        k = r.randrange(10)
        if k < 4 or depth > 1:
            return self.read(objects)
        if k == 4 and values:
            return r.choice(values)
        if k == 5:
            return self.primitive()
        if k == 6:
            return '(%s === %s)' % (r.choice(objects), r.choice(objects))
        return '(%s %s %s)' % (self.expression(objects, values, depth + 1), r.choice(['+', '-', '*', '|', '===', '<', '&&', '??']), self.expression(objects, values, depth + 1))

    def call(self, callable, objects):
        return '%s(%s)' % (self.r.choice(callable), ', '.join(self.r.choice(objects) for _ in range(3)))

    def change(self, o):
        p = self.r.choice(self.PROPERTIES)
        return self.r.choice([
            'delete %s.%s' % (o, p), '%s.added = %s' % (o, self.primitive()), 'Object.freeze(%s)' % o, 'Object.preventExtensions(%s)' % o, 'Object.setPrototypeOf(%s, other)' % o,
            'Object.setPrototypeOf(%s, null)' % o, 'Object.defineProperty(%s, "%s", { get() { getterCalls++; return getterCalls; }, configurable: true })' % (o, p),
            'Object.defineProperty(%s, "%s", { set(v) { setterCalls += 1; }, configurable: true })' % (o, p), 'Object.defineProperty(%s, "%s", { value: %s, writable: false, configurable: true })' % (o, p, self.primitive()),
            'Object.defineProperty(%s, "%s", { value: %s, enumerable: false, writable: true, configurable: true })' % (o, p, self.primitive()), '%s.length = %d' % (o, self.r.randrange(4)), '%s[%d] = %s' % (o, self.r.randrange(6), self.primitive()),
        ])

    def block(self, objects, values, callable, indent, count, depth=0):
        r = self.r
        pad = '    ' * indent
        objects, values = list(objects), list(values)
        text = ''
        for _ in range(count):
            k = r.randrange(24)
            o = r.choice(objects)
            p = r.choice(self.PROPERTIES)
            if k < 3:
                v = self.name('t')
                text += pad + 'const %s = %s;\n' % (v, self.expression(objects, values))
                values.append(v)
            elif k < 6:
                text += pad + r.choice(['%s.%s = %s;', '%s.%s += %s;', '%s.%s ??= %s;']) % (o, p, self.expression(objects, values)) + '\n'
            elif k == 6:
                text += pad + '%s[%s] = %s;\n' % (o, self.key(), self.expression(objects, values))
            elif k < 9:
                text += pad + 'note(%s);\n' % self.expression(objects, values)
            elif k < 12 and callable:
                before = self.name('t')
                text += pad + 'const %s = %s.%s;\n' % (before, o, p) + pad + 'note(%s);\n' % self.call(callable, objects) + pad + 'note(show(%s) + "," + show(%s.%s));\n' % (before, o, p)
                values.append(before)
            elif k == 12 and depth < 2:
                i = self.name('i')
                text += pad + 'for (let %s = 0; %s < %d; %s++) {\n' % (i, i, r.randrange(1, 5), i) + self.block(objects, values + [i], callable, indent + 1, r.randrange(1, 5), depth + 1) + pad + '}\n'
            elif k == 13:
                text += pad + 'tryCall(() => %s);\n' % self.change(o)
            elif k < 16:
                n = self.name('o')
                text += pad + 'const %s = %s;\n' % (n, r.choice([self.make(), self.make(), '{ x: %s, y: %s }' % (self.read(objects), self.read(objects)), '{ ...%s, z: %s }' % (o, self.primitive()), '(%s ? %s : %s)' % (self.expression(objects, values), o, r.choice(objects))]))
                objects.append(n)
                if r.random() < 0.25:
                    text += pad + 'kept.push(%s);\n' % n
            elif k == 16:
                a, b, c = self.name('t'), self.name('t'), self.name('t')
                text += pad + 'const { x: %s, y: %s = %s, ...%s } = %s;\n' % (a, b, self.primitive(), c, o) + pad + 'note([%s, %s, %s]);\n' % (a, b, c)
                values += [a, b]
            elif k == 17:
                f = self.name('f')
                text += pad + 'const %s = () => %s;\n' % (f, self.expression(objects, values)) + pad + '%s.%s = %s;\n' % (o, p, self.primitive()) + pad + 'note(%s());\n' % f
            elif k == 18 and depth < 2:
                text += pad + 'if (%s) {\n' % self.expression(objects, values) + self.block(objects, values, callable, indent + 1, r.randrange(1, 4), depth + 1) + pad + '}\n'
            elif k == 19:
                j = self.name('k')
                text += pad + 'for (const %s in %s) note(%s + ":" + show(%s[%s]));\n' % (j, o, j, o, j)
            elif k == 20:
                text += pad + 'note(tryCall(() => %s.%s));\n' % (o, r.choice(['sum()', 'bump()', 'twice', 'push(%s)' % self.primitive(), 'pop()', 'twice = %s' % self.primitive(), 'map(v => v + 1)', 'constructor.name']))
            elif k == 21 and depth < 2:
                e = self.name('e')
                text += pad + 'try {\n' + self.block(objects, values, callable, indent + 1, r.randrange(1, 4), depth + 1) + pad + '} catch (%s) {\n' % e + pad + '    note(%s);\n' % e + pad + '}\n'
            elif k == 22:
                text += pad + 'if (round === %d) tryCall(() => %s);\n' % (r.randrange(2, 24), self.change(r.choice(objects + ['Point.prototype', 'Base.prototype', 'Derived.prototype', 'shared', 'other'])))
            else:
                text += pad + 'note(%s);\n' % o
        return text

    def program(self):
        r = self.r
        text = """const log = [], kept = [];
let getterCalls = 0, setterCalls = 0, round = 0, key = "x";
function show(x, depth = 0) {
    if (typeof x === "function") return "function";
    if (typeof x !== "object" || x === null) return Object.is(x, -0) ? "-0" : typeof x === "string" ? JSON.stringify(x.slice(0, 30)) : String(x);
    if (x instanceof Error) return x.constructor.name;
    if (depth > 1) return "...";
    if (Array.isArray(x) || ArrayBuffer.isView(x)) { let s = "["; for (let i = 0; i < x.length && i < 8; i++) s += (i in x ? show(x[i], depth + 1) : "hole") + ","; return s + "]" + x.length; }
    return "{" + Object.keys(x).map(k => k + ":" + show(x[k], depth + 1)).join() + "}";
}
function note(x) { if (log.length < 6000) log.push(show(x)); }
function tryCall(f) { try { return f(); } catch (e) { return e instanceof Error ? e.constructor.name + ": " + e.message.slice(0, 32) : e; } }
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.sum = function () { return this.x + this.y; };
class Base {
    constructor(x) { this.x = x; this.z = 0; }
    get twice() { getterCalls++; return this.x * 2; }
    set twice(v) { setterCalls++; this.x = v; }
    sum() { return this.x + this.z; }
    bump() { this.x++; return this; }
}
class Derived extends Base {
    constructor(x, y) { super(x); this.y = y; }
    sum() { return super.sum() + this.y; }
}
const shared = { x: 10, y: 20, sum() { return this.x; } }, other = { z: 30, get y() { getterCalls++; return 40; } };
"""
        functions = []
        for _ in range(r.randrange(3, 8)):
            f = self.name('f')
            text += 'function %s(a, b, c) {\n' % f + self.block(['a', 'b', 'c'], [], functions, 1, r.randrange(2, 9)) + '    return %s;\n}\n' % self.expression(['a', 'b', 'c'], [])
            functions.append(f)
        pool = ['o%d' % i for i in range(r.randrange(2, 6))]
        outside = r.random() < 0.5
        made = ''.join('%sconst %s = %s;\n' % ('' if outside else '    ', o, self.make()) for o in pool)
        text += (made if outside else '') + 'for (round = 0; round < %d; round++) {\n' % r.choice([3, 12, 30]) + ('' if outside else made) + '    key = ["x", "y", "z", 0][round & 3];\n'
        for _ in range(r.randrange(2, 7)):
            text += '    note(tryCall(() => %s));\n' % self.call(functions, pool)
        text += ''.join('    note(%s);\n' % o for o in pool) + '    note(getterCalls + "," + setterCalls);\n    for (const o of kept.splice(0, 20)) note(o);\n}\nprint(log.join("\\n"));\n'
        return text


class Loops:
    """Loops over arrays of every kind, typed arrays, array-likes and strings, which change while the loop runs."""

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def make(self):
        return self.r.choice([
            '[1, 2, 3, 4, 5]', '[1.5, 2.5, 3.5, 4.5]', '[1, "a", null, { x: 1 }, undefined]', '[1, , 3, , 5]', 'new Array(4)', '[]', '[0, -0, NaN, 2147483647, -2147483648]', '[7]',
            'new Uint8Array([1, 2, 3, 250, 5])', 'new Int8Array([1, -2, 3])', 'new Uint16Array([1, 65535, 3, 4])', 'new Int32Array([1, -2, 2147483647])', 'new Uint32Array([1, 4294967295, 3])',
            'new Float32Array([1.5, 2, NaN])', 'new Float64Array([1.5, -0, 3])', 'new Uint8ClampedArray([1, 255, 3])', 'new Uint8Array(0)', 'new Sub([1, 2, 3, 4])', 'new Deep([5, 6, 7])', 'new Deeper([8, 9])',
            'withOwnLength(new Sub([1, 2, 3, 4]), 2)', 'new Uint8Array(new ArrayBuffer(4, { maxByteLength: 16 }))', 'new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }), 2)', 'new Uint8Array(new ArrayBuffer(8), 2, 4)',
            '{ length: 3, 0: 1, 1: 2, 2: 3 }', '{ length: 2, 0: "a" }', '"hello"', '"h\u00e9llo\u4e16"', 'argumentsOf(1, 2, 3)', 'Object.freeze([1, 2, 3])', 'new List(1, 2, 3)', 'sparse()', 'Object.create([1, 2, 3])',
        ])

    def index(self, i, a):
        return self.r.choice([i, i, i, i, i, '%s + 1' % i, '%s - 1' % i, '%s * 2' % i, '%s | 0' % i, '%s >>> 0' % i, '%s.length - 1 - %s' % (a, i), '0', '-1', '%s + 0.5' % i, '"%d"' % self.r.randrange(3), '%s %% 3' % i])

    def value(self, i, arrays):
        r = self.r
        return r.choice(['1', '0', '-1', '1.5', '300', '"s"', 'null', 'undefined', '{ x: 1 }', 'NaN', i, '%s * 2' % i, '%s + 0.5' % i, '%s[%s]' % (r.choice(arrays), self.index(i, r.choice(arrays))), 'sum'])

    def change(self, a, i):
        """A statement that changes the array, and whether it may make it longer."""
        return self.r.choice([
            ('%s.push(9)' % a, True), ('%s.push(1.5)' % a, True), ('%s.pop()' % a, False), ('%s.shift()' % a, False), ('%s.unshift(0)' % a, True), ('%s.length = 1' % a, False), ('%s.length = 0' % a, False),
            ('%s.length = 7' % a, True), ('%s[12] = 1' % a, True), ('%s[%s] = 1.5' % (a, i), False), ('%s[%s] = "s"' % (a, i), False), ('delete %s[%s]' % (a, i), False), ('%s[2000] = 1' % a, True),
            ('%s.buffer.resize(2)' % a, False), ('%s.buffer.resize(12)' % a, True), ('%s.buffer.transfer()' % a, False), ('withOwnLength(%s, 1)' % a, False), ('withOwnLength(%s, 6)' % a, True),
            ('Object.setPrototypeOf(%s, Other.prototype)' % a, False), ('Object.setPrototypeOf(%s, null)' % a, False), ('Object.setPrototypeOf(%s, Array.prototype)' % a, False), ('Object.freeze(%s)' % a, False),
            ('%s.reverse()' % a, False), ('%s.fill(3)' % a, False), ('%s.splice(1, 1)' % a, False), ('%s.sort()' % a, False), ('changes(%s)' % a, True), ('Object.defineProperty(%s, %s, { get() { return 42; }, configurable: true })' % (a, i), False),
        ])

    def body(self, i, a, arrays, indent, depth):
        r = self.r
        pad = '    ' * indent
        text, grows = '', False
        for _ in range(r.randrange(1, 5)):
            k = r.randrange(17)
            b = r.choice(arrays)
            if k < 3:
                text += pad + r.choice(['sum += %s[%s];', 'sum = (sum + %s[%s]) | 0;', 'text += show(%s[%s]);', 'sum ^= %s[%s];', 'sum = Math.max(sum, %s[%s]);']) % (a, self.index(i, a)) + '\n'
            elif k < 5:
                text += pad + '%s[%s] = %s;\n' % (b, self.index(i, b), self.value(i, arrays))
            elif k == 5:
                text += pad + 'if (%s[%s] === %s) %s;\n' % (a, i, r.choice(['3', '2.5', '"a"', 'undefined', '250', '"l"']), r.choice(['break', 'continue', 'sum++']))
            elif k < 8:
                change, longer = self.change(b, i)
                grows |= longer
                text += pad + 'if (%s === %d%s) tryCall(() => %s);\n' % (i, r.randrange(4), r.choice(['', '', ' && round === %d' % r.randrange(12)]), change)
            elif k == 8:
                text += pad + 'note(%s[%s]);\n' % (a, self.index(i, a))
            elif k == 9:
                text += pad + '%s[%s]%s;\n' % (b, i, r.choice(['++', '--', ' += 1', ' *= 2', ' |= 1', ' += 0.5', ' += "x"']))
            elif k == 10 and depth < 1:
                inner, g = self.loop(arrays, indent, depth + 1)
                text += inner
                grows |= g
            elif k == 11:
                text += pad + 'if (%s in %s) sum++;\n' % (self.index(i, a), b)
            elif k == 12:
                text += pad + 'sum += %s.length;\n' % b
            elif k == 13:
                text += pad + 'if (%s === %d) %s = %s;\n' % (i, r.randrange(3), a, r.choice(arrays))
            elif k == 14:
                text += pad + 'sum += reads(%s, %s);\n' % (b, self.index(i, b))
            else:
                text += pad + 'if (%s[%s] %s %s[%s]) sum++;\n' % (a, i, r.choice(['<', '===', '==', '>=', '!==']), b, self.index(i, b))
        return text, grows

    def loop(self, arrays, indent, depth=0):
        r = self.r
        pad = '    ' * indent
        a = r.choice(arrays)
        i = self.name('i')
        body, grows = self.body(i, a, arrays, indent + 1, depth)
        guard = pad + '    if (++steps > 3000) throw new TooManySteps();\n' if grows or r.random() < 0.2 else ''
        k = r.randrange(13)
        if k < 3:
            head = 'for (let %s = 0; %s < %s.length; %s++) {' % (i, i, a, i)
        elif k == 3:
            head = 'for (let %s = 0, n = %s.length; %s < n; %s++) {' % (i, a, i, i)
        elif k == 4:
            head = 'for (let %s = %s.length - 1; %s >= 0; %s--) {' % (i, a, i, i)
        elif k == 5:
            head = 'for (let %s = 0; %s < %s.length; %s += 2) {' % (i, i, a, i)
        elif k == 6:
            head = 'for (let %s = %d; %s <= %d; %s++) {' % (i, r.randrange(-2, 2), i, r.randrange(2, 8), i)
        elif k == 7:
            head = 'for (var %s = 0; %s !== %s.length && %s < 9; ++%s) {' % (i, i, a, i, i)
        elif k == 8:
            return pad + 'let %s = 0;\n' % i + pad + 'while (%s < %s.length) {\n' % (i, a) + pad + '    if (++steps > 3000) throw new TooManySteps();\n' + body.replace('continue;', 'sum--;') + pad + '    %s++;\n' % i + pad + '}\n', grows
        elif k == 9:
            v = self.name('v')
            return pad + 'let %s = 0;\n' % i + pad + 'for (const %s of %s) {\n' % (v, a) + pad + '    if (++steps > 3000) throw new TooManySteps();\n' + pad + '    text += show(%s);\n' % v + body.replace('continue;', 'sum--;') + pad + '    %s++;\n' % i + pad + '}\n', grows
        elif k == 10:
            return pad + 'for (const %s in %s) {\n' % (i, a) + pad + '    if (++steps > 3000) throw new TooManySteps();\n' + body + pad + '}\n', grows
        elif k == 11:
            v = self.name('v')
            return pad + 'Array.prototype.%s.call(%s, (%s, %s) => {\n' % (r.choice(['forEach', 'map', 'some', 'filter']), a, v, i) + pad + '    if (++steps > 3000) throw new TooManySteps();\n' + pad + '    text += show(%s);\n' % v + body.replace('continue;', 'return;').replace('break;', 'return true;') + pad + '});\n', grows
        else:
            head = 'for (let %s = 0; %s < Math.min(%s.length, 6); %s++) {' % (i, i, a, i)
        return pad + head + '\n' + guard + body + pad + '}\n', grows

    def program(self):
        r = self.r
        text = """const log = [];
let steps = 0, round = 0;
class TooManySteps extends Error { }
class Sub extends Uint8Array { }
class Deep extends Sub { }
class Deeper extends Deep { }
class Other extends Uint8Array { get length() { return 2; } }
class List extends Array { }
function withOwnLength(a, n) { Object.defineProperty(a, "length", { value: n, configurable: true, writable: true }); return a; }
function argumentsOf() { return arguments; }
function sparse() { const a = [1, 2]; a[5000] = 3; a.length = 4; return a; }
function changes(a) { if (round % 3 === 1) a[a.length] = round; return a; }
function reads(a, i) { const v = a[i]; return typeof v === "number" ? v : 0; }
function show(x, depth = 0) {
    if (typeof x !== "object" || x === null) return Object.is(x, -0) ? "-0" : typeof x === "string" ? JSON.stringify(x.slice(0, 60)) : String(x);
    if (x instanceof Error) return x.constructor.name;
    if (depth > 1) return "...";
    const n = tryCall(() => x.length);
    if (typeof n === "number") { let s = "["; for (let i = 0; i < n && i < 14; i++) s += (i in x ? show(x[i], depth + 1) : "hole") + ","; return s + "]" + n; }
    return "{" + Object.keys(x).join() + "}";
}
function note(x) { if (log.length < 6000) log.push(show(x)); }
function tryCall(f) { try { return f(); } catch (e) { return e instanceof Error ? e.constructor.name + ": " + e.message.slice(0, 32) : e; } }
"""
        functions = []
        for _ in range(r.randrange(2, 6)):
            f = self.name('f')
            text += 'function %s(a, b) {\n    let sum = 0, text = "";\n' % f
            for _ in range(r.randrange(1, 4)):
                text += self.loop(['a', 'b'], 1)[0]
            text += '    return show(sum) + text;\n}\n'
            functions.append(f)
        text += 'for (round = 0; round < %d; round++) {\n    steps = 0;\n' % r.choice([2, 8, 20])
        pool = ['o%d' % i for i in range(r.randrange(2, 6))]
        text += ''.join('    const %s = %s;\n' % (o, self.make()) for o in pool)
        for _ in range(r.randrange(2, 8)):
            text += '    note(tryCall(() => %s(%s, %s)));\n' % (r.choice(functions), r.choice(pool), r.choice(pool))
        text += ''.join('    note(%s);\n' % o for o in pool) + '}\nprint(log.join("\\n"));\n'
        return text


class Numbers:
    """Arithmetic at the edges of int32, uint32 and double, conversions, and variables whose kind of value changes."""
    EDGES = ['0', '1', '-1', '2', '3', '7', '-0', '0.5', '-1.5', '255', '256', '65535', '65536', '2147483647', '2147483646', '-2147483648', '-2147483647', '2147483648', '4294967295', '4294967296', '-4294967296',
             '1073741824', '46341', '9007199254740991', '9007199254740992', '-9007199254740991', '1e21', '1e-7', '5e-324', '1.7976931348623157e308', 'NaN', 'Infinity', '-Infinity', '0.1', '0.30000000000000004']
    OTHERS = ['"5"', '"-0"', '""', '" 12 "', '"0x10"', '"1e3"', '"abc"', 'true', 'false', 'null', 'undefined', '[]', '[7]', '{ valueOf() { return 3; } }', '{ toString() { return "4"; } }', '10n', '-3n']
    BINARY = ['+', '-', '*', '/', '%', '**', '|', '&', '^', '<<', '>>', '>>>', '<', '<=', '>', '>=', '==', '!=', '===', '!==', '&&', '||', '??']
    MATH = ['abs', 'floor', 'ceil', 'round', 'trunc', 'sign', 'sqrt', 'fround', 'clz32', 'cbrt', 'log2', 'exp']
    MATH2 = ['min', 'max', 'imul', 'pow', 'atan2', 'hypot']

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def constant(self):
        return self.r.choice(self.EDGES) if self.r.random() < 0.85 else self.r.choice(self.OTHERS)

    def expression(self, values, depth=0):
        r = self.r
        k = r.randrange(20)
        if depth > 3 or k < 5:
            return r.choice(values) if values and r.random() < 0.7 else '(%s)' % self.constant()
        e = lambda: self.expression(values, depth + 1)
        if k < 12:
            return '(%s %s %s)' % (e(), r.choice(self.BINARY), e())
        if k == 12:
            return '(%s%s)' % (r.choice(['-', '+', '~', '!', '- -', 'typeof ']), e())
        if k == 13:
            return 'Math.%s(%s)' % (r.choice(self.MATH), e())
        if k == 14:
            return 'Math.%s(%s, %s)' % (r.choice(self.MATH2), e(), e())
        if k == 15:
            return '(%s %s)' % (e(), r.choice(['| 0', '>>> 0', '>> 0', '& 255', '& 0xffff', '% 2', '* 1', '- 0', '+ ""', '/ 1', '<< 1', '>>> 31', '& -1']))
        if k == 16:
            return '(%s ? %s : %s)' % (e(), e(), e())
        if k == 17:
            return r.choice(['Number(%s)', 'String(%s)', 'parseInt(%s)', 'parseFloat(%s)', 'Number.isInteger(%s)', 'Number.isSafeInteger(%s)', 'Object.is(%s, -0)', 'isNaN(%s)', 'Boolean(%s)', 'Math.floor(%s / 2)']) % e()
        if k == 18:
            return '(%s).%s' % (e(), r.choice(['toString()', 'toFixed(2)', 'toString(2)', 'toString(16)', 'valueOf()', 'toPrecision(3)']))
        return '(%s %s %s %s %s)' % (e(), r.choice(['+', '-', '*']), e(), r.choice(['+', '-', '|', '>>>']), e())

    def block(self, values, variables, callable, indent, count, depth=0):
        r = self.r
        pad = '    ' * indent
        values, variables = list(values), list(variables)
        text = ''
        for _ in range(count):
            k = r.randrange(16)
            if k < 3:
                v = self.name('v')
                text += pad + 'let %s = %s;\n' % (v, self.expression(values))
                values.append(v)
                variables.append(v)
            elif k < 6 and variables:
                v = r.choice(variables)
                text += pad + r.choice(['%s = %%s;', '%s += %%s;', '%s -= %%s;', '%s *= %%s;', '%s |= %%s;', '%s >>>= %%s;', '%s <<= %%s;', '%s %%%%= %%s;', '%s ^= %%s;', '%s /= %%s;', '%s **= %%s;', '%s ??= %%s;']) % v % self.expression(values) + '\n'
            elif k == 6 and variables:
                text += pad + r.choice(['%s++;', '%s--;', '++%s;', '--%s;', 'note(%s++);', 'note(--%s);']) % r.choice(variables) + '\n'
            elif k < 9:
                text += pad + 'note(%s);\n' % self.expression(values)
            elif k < 11 and depth < 2:
                i = self.name('i')
                head = r.choice(['for (let %s = 0; %s < %d; %s++) {' % (i, i, r.randrange(1, 40), i), 'for (let %s = %s; %s > %s - %d; %s--) {' % (i, r.choice(['-2147483640', '5', '0', '2147483647']), i, r.choice(['-2147483640', '5', '0', '2147483647']), 6, i),
                                 'for (let %s = 2147483640; %s < 2147483650; %s++) {' % (i, i, i), 'for (let %s = 1; %s < 1e10; %s *= %d) {' % (i, i, i, r.choice([2, 3, 10, 65536])), 'for (let %s = 0; %s < 3; %s += 0.5) {' % (i, i, i),
                                 'for (let %s = 4294967290; %s < 4294967300; %s += 3) {' % (i, i, i), 'for (let %s = -3; %s <= 3; %s++) {' % (i, i, i)])
                if ' > ' in head and head.count(head.split('= ')[1].split(';')[0]) < 2:
                    head = 'for (let %s = 5; %s > -1; %s--) {' % (i, i, i)
                text += pad + head + '\n' + self.block(values + [i], variables, callable, indent + 1, r.randrange(1, 5), depth + 1) + pad + '}\n'
            elif k == 11 and depth < 2:
                text += pad + 'if (%s) {\n' % self.expression(values) + self.block(values, variables, callable, indent + 1, r.randrange(1, 4), depth + 1) + pad + '} else {\n' + self.block(values, variables, callable, indent + 1, r.randrange(1, 3), depth + 1) + pad + '}\n'
            elif k == 12 and callable:
                text += pad + 'note(%s(%s, %s));\n' % (r.choice(callable), self.expression(values), self.expression(values))
            elif k == 13:
                text += pad + 'switch (%s) {\n' % self.expression(values) + ''.join(pad + 'case %s: note(%d);%s\n' % (self.constant(), j, r.choice([' break;', ' break;', ''])) for j in range(r.randrange(1, 5))) + pad + 'default: note("default");\n' + pad + '}\n'
            elif k == 14:
                v = self.name('t')
                text += pad + 'const %s = new %s(2); %s[0] = %s; note(%s[0]);\n' % (v, r.choice(['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array']), v, self.expression(values), v)
            else:
                text += pad + 'note(tryCall(() => %s));\n' % self.expression(values)
        return text

    def program(self):
        r = self.r
        text = """const log = [];
function show(x) {
    if (typeof x === "number") return Object.is(x, -0) ? "-0" : String(x);
    if (typeof x === "bigint") return x + "n";
    if (typeof x === "string") return JSON.stringify(x.slice(0, 60));
    if (typeof x === "object" && x !== null) return x instanceof Error ? x.constructor.name : "object";
    return String(x);
}
function note(x) { if (log.length < 12000) log.push(show(x)); }
function tryCall(f) { try { return f(); } catch (e) { return e instanceof Error ? e.constructor.name + ": " + e.message.slice(0, 32) : e; } }
"""
        functions = []
        for _ in range(r.randrange(2, 7)):
            f = self.name('f')
            text += 'function %s(a, b) {\n' % f + self.block(['a', 'b'], ['a', 'b'], functions, 1, r.randrange(2, 9)) + '    return %s;\n}\n' % self.expression(['a', 'b'])
            functions.append(f)
        text += 'const edges = [%s];\n' % ', '.join(self.EDGES)
        for _ in range(r.randrange(1, 4)):
            f = self.name('pairs')
            simple = '(x %s y)' % r.choice(self.BINARY[:12])
            text += 'function %s() {\n    for (let i = 0; i < edges.length; i++) {\n        const x = edges[i];\n        for (let j = 0; j < edges.length; j++) {\n            const y = edges[j];\n            note(%s);\n        }\n    }\n}\n%s();\n' % (
                f, r.choice([simple, simple, '(%s %s)' % (simple, r.choice(['| 0', '>>> 0', '+ 1', '* y', '- x'])), self.expression(['x', 'y'], 2)]), f)
        kinds = r.choice([self.EDGES, self.EDGES, self.EDGES + self.OTHERS, ['0', '1', '2', '3', '100', '-5'], ['0.5', '1.5', '-2.5', '1e10']])
        text += 'const inputs = [%s];\n' % ', '.join(r.choice(kinds) for _ in range(r.randrange(3, 12)))
        text += 'for (let round = 0; round < %d; round++) {\n    for (const x of inputs) {\n        const y = inputs[(round + 1) %% inputs.length];\n' % r.choice([1, 3, 10])
        for _ in range(r.randrange(1, 5)):
            text += '        note(tryCall(() => %s(%s, %s)));\n' % (r.choice(functions), r.choice(['x', 'y', 'round', self.constant()]), r.choice(['x', 'y', 'round', self.constant()]))
        return text + '    }\n}\nprint(log.join("\\n"));\n'


class Classes:
    """Classes with members of every kind, the iterator protocol, destructuring, generators, async functions, and the order in which things are evaluated."""
    FIELDS = ['f0', 'f1', 'f2']
    METHODS = ['m0', 'm1', 'm2', 'm3']

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0
        self.classes = []

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def primitive(self):
        return self.r.choice(['0', '1', '2', '-1', '1.5', '"s"', '"t"', 'null', 'undefined', 'true', '[1, 2]', '{ f0: 5 }'])

    def expression(self, c, depth=0):
        """c: what can be named here: 'values', and inside a class 'this', 'privates', 'privateMethods', 'super'."""
        r = self.r
        k = r.randrange(24)
        e = lambda: self.expression(c, depth + 1)
        v = r.choice(c['values']) if c['values'] else '(0)'
        if depth > 2 or k < 4:
            return v if r.random() < 0.6 else self.primitive()
        if k < 7 and c.get('this'):
            return 'this.%s' % r.choice(self.FIELDS)
        if k == 7 and c.get('privates'):
            return 'this.%s' % r.choice(c['privates'])
        if k == 8 and c.get('privateMethods'):
            return 'this.%s(%s)' % (r.choice(c['privateMethods']), e())
        if k == 9 and c.get('this'):
            return 'this.%s(%s)' % (r.choice(self.METHODS), e())
        if k == 10 and c.get('super'):
            return r.choice(['super.%s(%s)' % (r.choice(self.METHODS), e()), 'super.%s' % r.choice(self.FIELDS + ['acc']), '(() => super.%s(%s))()' % (r.choice(self.METHODS), e())])
        if k == 11 and self.classes:
            return 'new %s(%s)' % (r.choice(self.classes), ', '.join(e() for _ in range(r.randrange(3))))
        if k == 12:
            return '(%s %s %s)' % (e(), r.choice(['+', '+', '-', '===', '??', '||', '&&', '<']), e())
        if k == 13:
            return '`${%s}-${%s}`' % (e(), e())
        if k == 14 and self.classes:
            return '(%s instanceof %s)' % (v, r.choice(self.classes + ['Array', 'Error', 'Object']))
        if k == 15 and c.get('privates'):
            return '(%s in Object(%s))' % (r.choice(c['privates']), v)
        if k == 16:
            return '%s?.%s' % (v, r.choice(self.FIELDS + ['acc', 'm0?.(1)', 'm1?.()', 'length', 'size']))
        if k == 17 and c.get('this'):
            return '(() => this.%s)()' % r.choice(self.FIELDS)
        if k == 18:
            return 'seq(%d, %s)' % (r.randrange(9), e())
        if k == 19:
            return '[...iterate(%s)].length' % v
        if k == 20:
            return 'tag`a${%s}b${%s}`' % (e(), e())
        if k == 21:
            return '%s.%s' % (v, r.choice(['acc', 'constructor?.name', 'toString?.()', self.r.choice(self.FIELDS)]))
        if k == 22:
            return 'String(%s)' % e()
        return 'typeof %s' % e()

    def statements(self, c, indent, count, depth=0, kind=''):
        r = self.r
        pad = '    ' * indent
        c = dict(c, values=list(c['values']))
        text = ''
        for _ in range(count):
            k = r.randrange(26)
            e = lambda: self.expression(c)
            v = r.choice(c['values']) if c['values'] else '(0)'
            if k < 3:
                n = self.name('v')
                text += pad + '%s %s = %s;\n' % (r.choice(['const', 'let']), n, e())
                c['values'].append(n)
            elif k < 6:
                text += pad + 'note(%s);\n' % e()
            elif k < 8 and c.get('this'):
                text += pad + 'this.%s %s %s;\n' % (r.choice(self.FIELDS + ['acc']), r.choice(['=', '=', '+=', '??=', '||=']), e())
            elif k == 8 and c.get('privates'):
                text += pad + 'this.%s %s %s;\n' % (r.choice(c['privates']), r.choice(['=', '+=', '??=']), e())
            elif k == 9 and c.get('super'):
                text += pad + 'super.%s = %s;\n' % (r.choice(self.FIELDS + ['acc']), e())
            elif k == 10:
                a, b, d = self.name('v'), self.name('v'), self.name('v')
                form = r.randrange(4)
                if form == 0:
                    text += pad + 'const { f0: %s, f1: %s = seq(1, 7), ...%s } = Object(%s);\n' % (a, b, d, v)
                elif form == 1:
                    text += pad + 'const [%s, %s = seq(2, 8), ...%s] = iterate(%s);\n' % (a, b, d, v)
                elif form == 2:
                    text += pad + 'const { f0: %s = seq(3, 1), acc: %s, f2: { f0: %s } = { f0: 9 } } = Object(%s);\n' % (a, b, d, v)
                else:
                    text += pad + 'const [%s, , %s = 3] = iterate(%s), %s = 0;\n' % (a, b, v, d)
                text += pad + 'note([%s, %s, %s]);\n' % (a, b, d)
                c['values'] += [a, b]
            elif k == 11 and depth < 2:
                n = self.name('x')
                inner = self.statements(dict(c, values=c['values'] + [n]), indent + 1, r.randrange(1, 4), depth + 1, kind)
                leave = r.choice(['', pad + '    if (%s === %s) break;\n' % (n, self.primitive()), pad + '    if (%s) continue;\n' % n, pad + '    if (%s === 2) throw new Error("left");\n' % n, pad + '    if (%s === 1) return %s;\n' % (n, n) if kind != 'static' else ''])
                text += pad + 'for (const %s of iterate(%s)) {\n' % (n, v) + leave + inner + pad + '}\n'
            elif k == 12 and depth < 2:
                n = self.name('e')
                text += pad + 'try {\n' + self.statements(c, indent + 1, r.randrange(1, 4), depth + 1, kind) + pad + '} catch (%s) {\n' % n + pad + '    note(%s);\n' % n + pad + '}' + (' finally {\n' + pad + '    note("finally");\n' + pad + '}' if r.random() < 0.5 else '') + '\n'
            elif k == 13 and kind in ('generator', 'asyncGenerator'):
                text += pad + r.choice(['yield %s;', 'note(yield %s);', 'yield* iterate(%s);']) % e() + '\n'
            elif k == 14 and kind in ('async', 'asyncGenerator'):
                text += pad + r.choice(['note(await %s);', 'await null;', 'note(await thenable(%s));', 'note(await Promise.all([%s, thenable(1)]));']).replace('%s', e(), 1) + '\n'
            elif k == 15:
                text += pad + 'note(tryCall(() => %s));\n' % e()
            elif k == 16 and depth < 2:
                text += pad + 'if (%s) {\n' % e() + self.statements(c, indent + 1, r.randrange(1, 3), depth + 1, kind) + pad + '}\n'
            elif k == 17:
                text += pad + 'seq(1, %s)[seq(2, "f0")] = seq(3, %s);\n' % ('Object(%s)' % v, e())
            elif k == 18:
                text += pad + 'note(sum(seq(1, 1), ...iterate(%s), seq(2, 2)));\n' % v
            elif k == 19:
                g = self.name('g')
                text += pad + 'const %s = tryCall(() => %s.gen?.(%s));\n' % (g, v, e()) + pad + 'note(tryCall(() => [%s?.next?.(), %s?.%s, %s?.next?.()]));\n' % (g, g, r.choice(['next?.(5)', 'return?.(6)', 'throw?.(new Error("in"))']), g)
            elif k == 20:
                text += pad + 'switch (%s) {\n' % e() + ''.join(pad + 'case %s: note(%d);%s\n' % (self.primitive() if r.random() < 0.7 else e(), j, r.choice([' break;', ''])) for j in range(r.randrange(1, 4))) + pad + 'default: note("default");\n' + pad + '}\n'
            elif k == 21:
                text += pad + 'kept.push(%s);\n' % r.choice(['() => %s' % e(), v, 'async () => %s' % e()])
            elif k == 22 and depth < 1:
                o = self.name('o')
                text += pad + 'const %s = { __proto__: Object(%s), f1: %s, m1(a) { burn(); return [super.m1?.(a), super.f0, this.f1]; }, get acc() { return seq(4, super.acc); }, [seq(5, "k")]: %s };\n' % (o, v, e(), e())
                c['values'].append(o)
            else:
                text += pad + 'note(%s);\n' % v
        return text

    def classText(self):
        r = self.r
        name = self.name('C')
        parent = r.choice(self.classes + self.classes + [None, None, 'Array', 'Error', 'Map', 'Plain', 'null']) if self.classes or r.random() < 0.4 else None
        derived = parent is not None and parent != 'null'
        privates = ['#p%d' % i for i in range(r.randrange(3))]
        privateMethods = ['#q%d' % i for i in range(r.randrange(2))]
        c = {'values': [], 'this': True, 'privates': privates, 'privateMethods': privateMethods, 'super': derived}
        text = 'class %s%s {\n' % (name, ' extends %s' % parent if parent else '')
        for p in privates:
            text += '    %s = %s;\n' % (p, self.expression(c))
        for f in r.sample(self.FIELDS, r.randrange(len(self.FIELDS) + 1)):
            text += '    %s = %s;\n' % (f, r.choice([self.expression(c), '() => this.%s' % r.choice(self.FIELDS), 'seq(6, %s)' % self.primitive()]))
        if r.random() < 0.4:
            text += '    static count = seq(7, 0);\n    static #hidden = 1;\n    static peek(o) { return [%s.#hidden, %s]; }\n' % (name, '%s in Object(o)' % privates[0] if privates else '0')
        if r.random() < 0.3:
            text += '    static {\n        try {\n' + self.statements({'values': [], 'this': True, 'super': derived}, 3, r.randrange(1, 3), 1, 'static') + '        } catch (e) {\n            note(e);\n        }\n    }\n'
        if r.random() < 0.7 and parent != 'null':
            text += '    constructor(a, b) {\n        burn();\n'
            before = self.statements({'values': ['a', 'b']}, 2, r.randrange(2), 1)
            text += (before + '        super(%s);\n' % r.choice(['', 'a', 'a, b', '...iterate(b)']) if derived else '') + '        note(new.target === %s);\n' % name + self.statements(dict(c, values=['a', 'b']), 2, r.randrange(4), 1)
            text += r.choice(['', '', '', '        return { f0: "replaced" };\n']) + '    }\n'
        for q in privateMethods:
            text += '    %s(a) {\n        burn();\n' % q + self.statements(dict(c, values=['a']), 2, r.randrange(1, 3), 1) + '        return %s;\n    }\n' % self.expression(dict(c, values=['a']))
        for m in r.sample(self.METHODS, r.randrange(1, len(self.METHODS) + 1)):
            kind = r.choice(['', '', '', 'async', 'static'])
            head = {'': '', 'async': 'async ', 'static': 'static '}[kind]
            text += '    %s%s(a) {\n        burn();\n' % (head, m) + self.statements(dict(c, values=['a'], privates=[] if kind == 'static' else privates, privateMethods=[] if kind == 'static' else privateMethods), 2, r.randrange(1, 5), 0, kind) + '        return %s;\n    }\n' % self.expression(dict(c, values=['a'], privates=[] if kind == 'static' else privates, privateMethods=[] if kind == 'static' else privateMethods))
        if r.random() < 0.6:
            text += '    get acc() { return seq(8, %s); }\n' % self.expression(c)
            if r.random() < 0.6:
                text += '    set acc(a) { note("set"); this.f2 = %s; }\n' % self.expression(dict(c, values=['a']))
        if r.random() < 0.5:
            kind = r.choice(['generator', 'generator', 'asyncGenerator'])
            text += '    %s*gen(a) {\n        burn();\n' % ('async ' if kind == 'asyncGenerator' else '') + '        try {\n' + self.statements(dict(c, values=['a']), 3, r.randrange(1, 5), 1, kind) + '        } finally {\n            note("generator left");\n        }\n    }\n'
        if r.random() < 0.3 and parent not in ('Array', 'Map'):
            text += '    *[Symbol.iterator]() { burn(); yield this.f0; yield* iterate(this.f1); }\n'
        if r.random() < 0.3:
            text += '    [Symbol.toPrimitive](hint) { note(hint); return %s; }\n' % r.choice(['1', '"p"', 'this.f0', 'hint'])
        self.classes.append(name)
        return text + '}\n'

    def program(self):
        r = self.r
        text = """let fuel = 500;
const log = [], kept = [];
class OutOfFuel extends Error { }
function burn() { if (--fuel < 0) throw new OutOfFuel("fuel"); }
function show(x, depth = 0) {
    if (typeof x === "function") return "function";
    if (typeof x === "symbol") return "symbol";
    if (typeof x !== "object" || x === null) return typeof x === "string" ? JSON.stringify(x.slice(0, 40)) : String(x);
    if (x instanceof Error) return x.constructor.name;
    if (typeof x.then === "function") { tryCall(() => x.then(v => note(["then", v]), e => note(["rejected", e]))); return "thenable"; }
    if (depth > 2) return "...";
    if (Array.isArray(x)) return "[" + x.slice(0, 8).map(v => show(v, depth + 1)).join() + "]";
    if (typeof x.next === "function") return "iterator";
    return (x.constructor?.name ?? "null") + "{" + Object.keys(x).slice(0, 8).map(k => k + ":" + show(tryCall(() => x[k]), depth + 1)).join() + "}";
}
function note(x) { if (log.length < 5000) log.push(show(x)); }
function tryCall(f) { try { burn(); return f(); } catch (e) { return e instanceof Error ? e.constructor.name + ": " + e.message.slice(0, 32) : e; } }
function seq(n, x) { if (log.length < 5000) log.push("#" + n); return x; }
function sum(...all) { return all.length + ":" + all.map(v => show(v)).join("+"); }
function tag(strings, ...values) { tag.seen ??= new Set(); const again = tag.seen.has(strings); tag.seen.add(strings); return strings.raw.join("|") + again + values.map(v => show(v)).join(); }
function thenable(x) { return { then(resolve) { note("then called"); resolve(x); } }; }
function Plain(a) { this.f0 = a; }
Plain.prototype.m0 = function (a) { return ["plain", a, this.f0]; };
function iterate(x) {
    if (x !== null && x !== undefined && typeof x[Symbol.iterator] === "function" && typeof x !== "string") return x;
    let i = 0;
    return { [Symbol.iterator]() { return this; }, next() { burn(); note("next"); return i < 3 ? { value: i++, done: false } : { value: undefined, done: true }; }, return(v) { note("return called"); return { value: v, done: true }; } };
}
"""
        for _ in range(r.randrange(2, 6)):
            text += self.classText()
        functions = []
        for _ in range(r.randrange(1, 4)):
            f = self.name('f')
            kind = r.choice(['', '', 'async', 'generator'])
            text += '%sfunction%s %s(a, b) {\n    burn();\n' % ('async ' if kind == 'async' else '', '*' if kind == 'generator' else '', f) + self.statements({'values': ['a', 'b']}, 1, r.randrange(2, 8), 0, kind) + '    return %s;\n}\n' % self.expression({'values': ['a', 'b']})
            functions.append(f)
        text += 'for (let round = 0; round < %d; round++) {\n    fuel = 500;\n' % r.choice([1, 3, 8])
        pool = []
        for _ in range(r.randrange(2, 5)):
            o = self.name('o')
            text += '    const %s = tryCall(() => new %s(%s, %s));\n' % (o, r.choice(self.classes), self.primitive(), r.choice(pool) if pool else self.primitive())
            pool.append(o)
        for _ in range(r.randrange(3, 9)):
            o = r.choice(pool)
            text += '    note(tryCall(() => %s));\n' % r.choice(['%s(%s, %s)' % (r.choice(functions), o, r.choice(pool)), '%s.%s(%s)' % (o, r.choice(self.METHODS), r.choice(pool)), '%s.%s(%s)' % (r.choice(self.classes), r.choice(self.METHODS + ['peek']), o), '%s.acc' % o, '[...%s]' % o, '`${%s}`' % o, '%s + 1' % o, '%s.acc = %s' % (o, self.primitive())])
        text += ''.join('    note(%s);\n' % o for o in pool) + '    for (const f of kept.splice(0, 20)) note(tryCall(() => typeof f === "function" ? f() : f));\n    drainMicrotasks();\n}\nprint(log.join("\\n"));\n'
        return text


def run(jsc, options, path, limit=20):
    p = subprocess.run([sys.executable, CAP, '1.5', str(limit), jsc, *options, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    err = '\n'.join(l for l in p.stderr.decode('utf-8', 'replace').split('\n') if not l.startswith('[capped]'))
    return p.returncode, p.stdout.decode('utf-8', 'replace'), err, b'[capped] killed' in p.stderr


def compare(jsc, path, extra, module, limit=20):
    """None if the program does not count, '' if both runs agree, else the kind of finding; and what compiled code said first."""
    c0, o0, e0, k0 = run(jsc, REFERENCE + module, path, limit)
    if k0 or c0 or 'SyntaxError' in o0 + e0 or 'RangeError' in o0 + e0:
        return None, ''
    c1, o1, e1, k1 = run(jsc, COMPILED + extra + module, path, limit)
    kind = 'hang' if k1 else 'crash' if c1 not in (0, 3) else 'differs' if (c0, o0) != (c1, o1) else ''
    return kind, 'exit %d: %s' % (c1, (e1 or o1).strip().split('\n')[0][:200])


def fuzz(args):
    out = os.path.abspath(args.output)
    os.makedirs(out, exist_ok=True)
    generators = {'closures': [Closures], 'objects': [Objects], 'loops': [Loops], 'numbers': [Numbers], 'classes': [Classes], 'all': [Closures, Objects, Loops, Numbers, Classes]}[args.generator]
    stats = {'programs': 0, 'do not count': 0, 'findings': 0}
    lock = threading.Lock()
    deadline = time.time() + args.seconds

    def work(worker):
        i = 0
        while time.time() < deadline:
            seed = args.seed * 1000003 + worker * 100003 + i
            i += 1
            text = generators[seed // 16 % len(generators)](seed).program()
            path = '%s/case-%d.js' % (out, worker)
            open(path, 'w').write(text)
            module = ['-m'] if seed % 2 else []
            extra = CONFIGS[seed // 2 % len(CONFIGS)]
            kind, said = compare(args.jsc, path, extra, module)
            with lock:
                stats['programs' if kind is not None else 'do not count'] += 1
                stats['findings'] += bool(kind)
            if kind:
                open('%s/%s-%d.js' % (out, kind, seed), 'w').write('// %s\n// %s\n%s' % (' '.join(extra + module), said, text))
        os.remove('%s/case-%d.js' % (out, worker))

    with ThreadPoolExecutor(args.jobs) as pool:
        list(pool.map(work, range(args.jobs)))
    print(stats)
    return 1 if stats['findings'] else 0


def minimize(args):
    lines = open(args.minimize).read().split('\n')
    options = lines[0][2:].split()
    module = ['-m'] if '-m' in options else []
    extra = [o for o in options if o != '-m']
    body = lines[2:]
    result = args.minimize[:-3] + '.min.js'
    tmp = result + '.tmp.js'

    def kind(candidate):
        open(tmp, 'w').write('\n'.join(candidate))
        return compare(args.jsc, tmp, extra, module, args.limit)[0]

    want = kind(body)
    print('%s, %d lines' % (want or 'not a finding', len(body)))
    if not want:
        return 1
    n = 2
    while len(body) >= 2:
        size = max(1, len(body) // n)
        for i in range(0, len(body), size):
            candidate = body[:i] + body[i + size:]
            if candidate and kind(candidate) == want:
                body = candidate
                n = max(n - 1, 2)
                break
        else:
            if size == 1:
                break
            n = min(n * 2, len(body))
    open(result, 'w').write('\n'.join(lines[:2] + body))
    os.remove(tmp)
    print('%d lines left: %s' % (len(body), result))
    return 0


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('jsc', type=os.path.abspath)
parser.add_argument('output', nargs='?')
parser.add_argument('--seconds', type=float, default=300)
parser.add_argument('--jobs', type=int, default=os.cpu_count() // 2)
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--generator', choices=['closures', 'objects', 'loops', 'numbers', 'classes', 'all'], default='all')
parser.add_argument('--minimize', metavar='FINDING')
parser.add_argument('--limit', type=float, default=20, help='seconds a run may take while minimizing')
args = parser.parse_args()
if not args.minimize and not args.output:
    parser.error('an output directory or --minimize')
sys.exit(minimize(args) if args.minimize else fuzz(args))
