//@ runDefault("--compileMainScriptAheadOfTime=1", "--thresholdForGlobalLexicalBindingEpoch=2")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}

first = 1;
second = 2;
third = 3;
function readsFirst() { return first; }
function readsSecond() { return second; }
function readsThird() { return third; }
function writesThird(value) { third = value; }
function typeOfFirst() { return typeof first; }
for (const f of [readsFirst, readsSecond, readsThird, writesThird, typeOfFirst])
    noInline(f);

repeat(() => {
    check(readsFirst(), 1, "a property of the global object");
    check(readsSecond(), 2, "another property of the global object");
    check(readsThird(), 3, "a property that stays");
    check(typeOfFirst(), "number", "typeof a property of the global object");
});

$.evalScript("const first = '41';");
repeat(() => {
    check(readsFirst(), "41", "a lexical binding that shadows the property");
    check(typeOfFirst(), "string", "typeof a lexical binding that shadows the property");
    check(readsSecond(), 2, "a property that is not shadowed yet");
});

$.evalScript("const second = 42;");
repeat(() => {
    check(readsFirst(), "41", "the first binding after the second");
    check(readsSecond(), 42, "the second binding");
    check(readsThird(), 3, "a property that is never shadowed");
});

for (let round = 0; round < 8; round++) {
    $.evalScript("let unrelated" + round + " = " + round + ";");
    repeat(i => {
        check(readsFirst(), "41", "the first binding in round " + round);
        check(readsSecond(), 42, "the second binding in round " + round);
        writesThird(round * 100 + i);
        check(readsThird(), round * 100 + i, "a property that is written in round " + round);
        check(globalThis.third, round * 100 + i, "the property itself in round " + round);
    });
}

$.evalScript("let third = 'lexical';");
repeat(i => {
    check(readsThird(), i ? i - 1 : "lexical", "a binding that shadows a property that was written");
    writesThird(i);
    check(globalThis.third, 799, "the property is no longer written");
});
