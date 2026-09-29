# The same as programs/hashes-of-callables.py, of what is JavaScript's: hash(x), x.__hash__() and type(x).__hash__(x) are one int, and so it can be a key. It goes by where things are in memory, so a great many are tried.
import js


def agree(x):
    # What a class has by that name is for its instances.
    a, b, c = hash(x), hash(x) if isinstance(x, type) else x.__hash__(), type(x).__hash__(x)
    return type(a) is int and type(b) is int and type(c) is int and a == b == c and {x: 1}[x] == 1 and x in {x}


def check(label, things):
    things = list(things)
    # Which kinds there is no hash of is part of what is looked at.
    without = sorted({type(x).__name__ for x in things if type(x).__hash__ is None})
    things = [x for x in things if type(x).__hash__ is not None]
    print(label, "=>", len(things) > 0, [x for x in things if not agree(x)][:3], without)


N = 3000
check("functions that are there already", [js.parseInt, js.parseFloat, js.isNaN, js.Math.max, js.Math.min, js.JSON.stringify, js.Array.isArray, js.Object.keys, js.Reflect.apply])
check("constructors", [js.Object, js.Array, js.Map, js.Set, js.Date, js.Promise, js.Error, js.Function, js.RegExp, js.Uint8Array])
check("functions made now", [js.Function("return %d" % i) for i in range(N)])
maps = [js.Map() for _ in range(N)]
check("objects", maps)
check("methods of objects", (m.get for m in maps))
check("bound by JavaScript", [js.Function("return this").bind(m) for m in maps[:300]])
check("plain objects, dates and the rest", [f() for _ in range(300) for f in (js.Object, js.Date, js.Set, js.WeakMap)])
check("symbols", [js.Symbol("s%d" % i) for i in range(300)] + [js.Symbol.iterator])

print("---- what is equal has the same")
m, n = js.Map(), js.Map()
print(m.get == m.get, hash(m.get) == hash(m.get), len({m.get, m.get, m.set}), m.get == n.get, len({x.get for x in maps}) == N)
print(js.parseInt == js.parseInt, hash(js.parseInt) == hash(js.parseInt), len({js.parseInt, js.parseInt, js.parseFloat}))
d = {m.get: "get", m.set: "set", js.parseInt: "parseInt", js.Map: "Map", m: "m"}
print(d[m.get], d[m.set], d[js.parseInt], d[js.Map], d[m], n.get in d, n in d)
print("---- never -1")
print(all(hash(x) != -1 and x.__hash__() != -1 for x in [x.get for x in maps]))
