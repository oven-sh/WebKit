# To JavaScript an exception has a `stack`, with the frames of both languages in it. Code in Python makes nothing of that, and raises and catches exceptions as a matter of course, so it is not found out where one is until
# it comes to something that may ask. What is then found is to be what would have been found where it was raised.
import js
import weakref

names = js.eval("""e => e.stack.split("\\n").map(l => l.replace(/@.*$/, "")).filter(l => l && !/^(module code|global code|<module>|evaluate|async.*|\\[native code\\])$/.test(l)).join(" < ")""")
catch = js.eval("f => { try { f(); return 'nothing was thrown'; } catch (e) { return e; } }")
js_calls = js.eval("(function jsCalls(f) { return f(); })")
js_calls_again = js.eval("(function jsCallsAgain(f) { return f(); })")
js_makes = js.eval("(function jsMakes(C) { return new C('made by JavaScript'); })")
js_throws = js.eval("(function jsThrows(e) { throw e; })")
cause_of = js.eval("e => e.cause")
collect = js.eval("() => { $vm.completeAllJITPlans(); fullGC(); }")


def often(label, f):
    "The first time, and every time after that"
    first = f()
    for i in range(400):
        now = f()
        if now != first:
            print(label, "=>", ("AT FIRST", first, "AND THEN", now))
            return
    print(label, "=>", first)


def raises():
    raise ValueError("x")


def passes_on():
    raises()


def looks_up():
    return {}["missing"]


print("---- thrown into JavaScript")
often("straight out", lambda: names(catch(raises)))
often("through another", lambda: names(catch(passes_on)))
often("out of something built in", lambda: names(catch(looks_up)))
often("one language and then the other", lambda: names(catch(lambda: js_calls(lambda: js_calls_again(passes_on)))))


def cleans_up():
    try:
        passes_on()
    finally:
        pass


def raises_it_again():
    try:
        passes_on()
    except ValueError:
        raise


def raises_another():
    try:
        passes_on()
    except ValueError as e:
        raise KeyError("y") from e


often("with something to be done on the way", lambda: names(catch(cleans_up)))
often("caught and raised again", lambda: names(catch(raises_it_again)))
often("caught, and another raised", lambda: names(catch(raises_another)))
often("and what that one came of", lambda: names(cause_of(catch(raises_another))))

print("---- caught by Python, and handed over")


def caught(f):
    try:
        f()
    except Exception as e:
        return e


often("one that has been raised", lambda: names(caught(passes_on)))
often("out of something built in", lambda: names(caught(looks_up)))
often("that went through JavaScript", lambda: names(caught(lambda: js_calls(passes_on))))
often("that went through something built in", lambda: names(caught(lambda: sorted([1, 2], key=lambda x: passes_on()))))
often("asked twice", lambda: (lambda e: names(e) == names(e))(caught(passes_on)))


def asks():
    return names(ValueError("never raised"))


often("one that never was", asks)


def raised_later():
    e = caught(passes_on)
    return names(catch(lambda: js_throws(e)))


often("and then thrown by JavaScript", raised_later)


def keeps():
    e = caught(passes_on)
    first = names(e)
    def elsewhere():
        raise e
    return first == names(catch(elsewhere))


often("what it has said, it goes on saying", keeps)

print("---- made by JavaScript")
often("where it was made", lambda: names(js_makes(ValueError)))
often("and raised by Python", lambda: names(catch(lambda: (_ for _ in ()).throw(js_makes(ValueError)))))

print("---- it is not what keeps anything")


def made_for_the_occasion(how):
    namespace = {}
    exec("def short_lived():\n    raise ValueError('x')", namespace)
    f = namespace["short_lived"]
    e = how(f)
    e.__traceback__ = None
    return e, weakref.ref(f)


for label, how in (("thrown into JavaScript", catch), ("through something built in", lambda f: caught(lambda: sorted([1], key=lambda x: f())))):
    kept = [made_for_the_occasion(how) for i in range(20)]
    collect()
    collect()
    print(label, "=>", sum(1 for e, r in kept if r() is None) >= 15, {names(e).split(" < ")[0] for e, r in kept})
