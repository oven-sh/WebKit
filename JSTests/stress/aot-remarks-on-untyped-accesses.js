//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function saysOnly(name, expected) {
    const remarks = aotRemarks(name);
    if (!remarks)
        return;
    const said = remarks.filter(remark => remark.startsWith("untyped-access-of:"));
    check(said.join(), "untyped-access-of:" + expected, "what is known of the object that " + name + " gets at");
}
(function () {
    function make(i) { return { a: i, b: i + 1 }; }
    function makeOther(i) { return { c: i, a: i }; }
    function readsPlainObject(o) { try { return o.a; } catch (e) { throw e; } }
    function readsOneOfTwoPlainObjects(o) { try { return o.a; } catch (e) { throw e; } }
    function readsPlainObjectOrNull(o) { try { return o.a; } catch (e) { return 0; } }
    function writesPlainObject(o) { try { o.a = 1; } catch (e) { throw e; } }
    function readsObject(o) { try { return o.a; } catch (e) { throw e; } }
    function writesObject(o) { try { o.a = 1; } catch (e) { throw e; } }
    function readsPrimitive(s) { try { return s.a; } catch (e) { throw e; } }
    function readsAnything(o) { try { return o.a; } catch (e) { throw e; } }
    function writesAnything(o) { try { o.a = 1; } catch (e) { throw e; } }
    function readsObjectOrPrimitive(o) { try { return o.a; } catch (e) { throw e; } }
    let sum = 0;
    for (let i = 0; i < 10; i++) {
        sum += readsPlainObject(make(i));
        sum += readsOneOfTwoPlainObjects(i & 1 ? make(i) : makeOther(i));
        sum += readsPlainObjectOrNull(i & 1 ? make(i) : null);
        writesPlainObject(make(i));
        sum += readsObject(i & 1 ? make(i) : [i]) ? 1 : 0;
        writesObject(i & 1 ? make(i) : [i]);
        sum += readsPrimitive("s" + i) ? 1 : 0;
        sum += readsAnything(JSON.parse('{"a":1}'));
        writesAnything(JSON.parse('{"a":1}'));
        sum += readsObjectOrPrimitive(i & 1 ? make(i) : "s") ? 1 : 0;
    }
    check(sum, 135, "the sum");
})();
saysOnly("readsPlainObject", "plain-object");
saysOnly("readsOneOfTwoPlainObjects", "plain-object");
saysOnly("readsPlainObjectOrNull", "plain-object");
saysOnly("writesPlainObject", "plain-object");
saysOnly("readsObject", "object");
saysOnly("writesObject", "object");
saysOnly("readsPrimitive", "primitive");
saysOnly("readsAnything", "anything");
saysOnly("writesAnything", "anything");
saysOnly("readsObjectOrPrimitive", "anything");
