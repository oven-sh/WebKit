# The module gc. There is one collector here for everything of both languages, and it is not a program's to tune: collect() collects, and for the rest a program finds what it would find of a collector that has nothing to
# report. So this is about what is the same whatever there is to collect: what there is in the module, what it says of what it is given, what it remembers, and who is told when.
import gc
import inspect
import sys


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


print("---- what there is")
t("names", lambda: [n for n in dir(gc) if not n.startswith("__")])
t("constants", lambda: [(n, getattr(gc, n)) for n in dir(gc) if n.startswith("DEBUG")])
t("signatures", lambda: [(n, getattr(gc, n).__text_signature__) for n in dir(gc) if callable(getattr(gc, n)) and not n.startswith("__")])
t("what inspect makes of them", lambda: [str(inspect.signature(f)) for f in (gc.collect, gc.get_objects, gc.get_referrers, gc.is_tracked, gc.enable)])
t("what they are for", lambda: [(n, len(getattr(gc, n).__doc__), getattr(gc, n).__doc__.splitlines()[0]) for n in dir(gc) if callable(getattr(gc, n)) and not n.startswith("__")])
t("the module", lambda: (gc.__name__, gc.__spec__.origin, gc.__loader__.__name__, len(gc.__doc__), gc.__doc__.splitlines()[0], "gc" in sys.builtin_module_names, type(gc.collect).__name__, gc.collect.__module__))

print("---- to begin with")
t("state", lambda: (gc.isenabled(), gc.get_debug(), gc.get_threshold(), gc.garbage, gc.callbacks, gc.get_freeze_count()))
t("kinds", lambda: (type(gc.collect()).__name__, type(gc.get_count()).__name__, len(gc.get_count()), [type(n).__name__ for n in gc.get_count()], type(gc.get_objects()).__name__, type(gc.get_stats()).__name__))
t("statistics", lambda: [(sorted(d), [type(v).__name__ for v in d.values()]) for d in gc.get_stats()])

print("---- what it remembers")
t("disable and enable", lambda: (gc.disable(), gc.isenabled(), gc.disable(), gc.isenabled(), gc.enable(), gc.isenabled(), gc.enable(), gc.isenabled()))
t("collect while disabled", lambda: (gc.disable(), type(gc.collect()).__name__, gc.isenabled(), gc.enable()))
t("debug", lambda: (gc.set_debug(gc.DEBUG_SAVEALL), gc.get_debug(), gc.set_debug(-1), gc.get_debug(), gc.set_debug(True), gc.get_debug(), gc.set_debug(0), gc.get_debug()))
t("thresholds", lambda: (gc.set_threshold(5), gc.get_threshold(), gc.set_threshold(6, 7), gc.get_threshold(), gc.set_threshold(1, 2, 3), gc.get_threshold(), gc.set_threshold(0), gc.get_threshold(), gc.set_threshold(-1, -2, -3),
                         gc.get_threshold(), gc.set_threshold(2000, 10, 10), gc.get_threshold()))
t("thresholds when one will not do", lambda: (gc.set_threshold(1, 2, 3), gc.get_threshold()))
t("    the last", lambda: gc.set_threshold(9, 9, "a"))
t("    are as they were", lambda: (gc.get_threshold(), gc.set_threshold(2000, 10, 10)))
t("put away and put back", lambda: [(was, gc.disable(), gc.isenabled(), gc.enable() if was else None, gc.isenabled()) for was in [gc.isenabled()]])
t("freeze", lambda: (gc.freeze(), type(gc.get_freeze_count()).__name__, gc.unfreeze(), gc.get_freeze_count()))
t("the lists are the module's own", lambda: (gc.garbage is gc.garbage, gc.callbacks is gc.callbacks, type(gc.garbage).__name__, type(gc.callbacks).__name__))

print("---- what will not do")
for label, f in (("collect(3)", lambda: gc.collect(3)), ("collect(-1)", lambda: gc.collect(-1)), ("collect('a')", lambda: gc.collect("a")), ("collect(1.5)", lambda: gc.collect(1.5)), ("collect(None)", lambda: gc.collect(None)),
                 ("collect(2 ** 40)", lambda: gc.collect(2 ** 40)), ("collect(1, 2)", lambda: gc.collect(1, 2)), ("collect(gen=1)", lambda: gc.collect(gen=1)), ("collect(True)", lambda: type(gc.collect(True)).__name__),
                 ("get_objects(3)", lambda: gc.get_objects(3)), ("get_objects(-1)", lambda: type(gc.get_objects(-1)).__name__), ("get_objects(-2)", lambda: gc.get_objects(-2)), ("get_objects('a')", lambda: gc.get_objects("a")),
                 ("get_objects(2 ** 70)", lambda: gc.get_objects(2 ** 70)), ("get_objects(generation=None)", lambda: type(gc.get_objects(generation=None)).__name__), ("get_objects(0, 1)", lambda: gc.get_objects(0, 1)),
                 ("set_threshold()", lambda: gc.set_threshold()), ("set_threshold(1, 2, 3, 4)", lambda: gc.set_threshold(1, 2, 3, 4)), ("set_threshold('a')", lambda: gc.set_threshold("a")),
                 ("set_threshold(1.5)", lambda: gc.set_threshold(1.5)), ("set_threshold(2 ** 31)", lambda: gc.set_threshold(2 ** 31)), ("set_threshold(threshold0=1)", lambda: gc.set_threshold(threshold0=1)),
                 ("set_debug()", lambda: gc.set_debug()), ("set_debug(1, 2)", lambda: gc.set_debug(1, 2)), ("set_debug('a')", lambda: gc.set_debug("a")), ("set_debug(2 ** 40)", lambda: gc.set_debug(2 ** 40)),
                 ("set_debug(flags=1)", lambda: gc.set_debug(flags=1)), ("is_tracked()", lambda: gc.is_tracked()), ("is_tracked(1, 2)", lambda: gc.is_tracked(1, 2)), ("is_finalized()", lambda: gc.is_finalized()),
                 ("enable(1)", lambda: gc.enable(1)), ("isenabled(1)", lambda: gc.isenabled(1)), ("get_count(1)", lambda: gc.get_count(1)), ("get_stats(1)", lambda: gc.get_stats(1)), ("freeze(1)", lambda: gc.freeze(1)),
                 ("get_referrers(a=1)", lambda: gc.get_referrers(a=1)), ("get_referents(a=1)", lambda: gc.get_referents(a=1)), ("get_referrers()", lambda: gc.get_referrers()), ("get_referents()", lambda: gc.get_referents())):
    t(label, f)

print("---- who is told")
seen = []


def note(phase, info):
    seen.append((phase, sorted(info), info["generation"], type(info["collected"]).__name__, type(info["uncollectable"]).__name__))


def told(f):
    del seen[:]
    f()
    return seen[:]


gc.callbacks.append(note)
t("of each", lambda: told(lambda: (gc.collect(), gc.collect(0), gc.collect(generation=1))))
t("not of what will not do", lambda: told(lambda: [t("    ", lambda: gc.collect(5))]))
gc.callbacks.append(note)
t("twice over", lambda: [phase for phase, *rest in told(gc.collect)])
del gc.callbacks[:]


def collects_again(phase, info):
    seen.append((phase, gc.collect()))


gc.callbacks.append(collects_again)
t("one that collects", lambda: told(gc.collect))
del gc.callbacks[:]


def adds_another(phase, info):
    seen.append(phase)
    if len(gc.callbacks) < 3:
        gc.callbacks.append(adds_another)


gc.callbacks.append(adds_another)
t("one that adds another", lambda: told(gc.collect))
del gc.callbacks[:]
shown = []
hook = sys.unraisablehook
sys.unraisablehook = lambda u: shown.append((type(u.exc_value).__name__, str(u.exc_value), u.err_msg.split(" <")[0], u.object))


def raises(phase, info):
    raise KeyError(phase)


gc.callbacks.extend([raises, note, 5])
t("one that raises, and one that cannot be called", lambda: (type(gc.collect()).__name__, [s[0] for s in seen[-2:]], shown))
sys.unraisablehook = hook
del gc.callbacks[:]
t("the same dictionary for all", lambda: [(gc.callbacks.extend([lambda p, i: kept.append(i)] * 2), gc.collect(), kept[0] is kept[1], kept[0] is kept[2], gc.callbacks.clear()) for kept in [[]]])

print("---- what could refer to itself")


class C:
    pass


class S:
    __slots__ = ()


t("is_tracked", lambda: [(type(x).__name__, gc.is_tracked(x)) for x in (0, 2 ** 70, 1.5, "s", b"b", None, True, (), ([],), ((), ([],)), [], {}, {1: 2}, set(), frozenset(), object(), C(), C, int, type, len, [].append, lambda: 0, 1j,
                                                                        range(3), slice(1), Ellipsis, NotImplemented, bytearray(), iter([]), (i for i in ()), sys, ValueError(), property(), memoryview(b""))])


class T(tuple):
    pass


def nested(innermost, depth, width):
    for i in range(depth):
        innermost = (innermost,) * width
    return innermost


def settled(x):
    "CPython finds out that there is nothing in a tuple for it to look at when it next collects"
    gc.collect()
    gc.collect()
    return gc.is_tracked(x)


t("tuples of tuples", lambda: [settled(x) for x in (((1, "x"), 2.5, (2, 3)), ((1, "x"), 2.5, (2, [])), (T(),), T(), T((1,)), (C,), (int,))])
# With nothing at the bottom for it to look at, what CPython says goes by how many times it has collected. That there is an answer is the point.
t("a long way down", lambda: [type(settled(nested(1, 200000, 1))).__name__, settled(nested([], 200000, 1))])
t("come to by a great many ways", lambda: [type(settled(nested(1, 80, 2))).__name__, settled(nested([], 80, 2))])
t("is_finalized", lambda: [gc.is_finalized(x) for x in (0, [], C(), C)])

print("---- what is audited")
events = []
sys.addaudithook(lambda event, arguments: events.append((event, arguments)) if event.startswith("gc.") else None)
t("events", lambda: [gc.get_objects(), gc.get_objects(1), gc.get_objects(generation=None), gc.get_referrers(1, "a"), gc.get_referents(), gc.collect()] and events)
del events[:]
t("before it is looked at", lambda: gc.get_objects(7))
t("    ", lambda: events)
