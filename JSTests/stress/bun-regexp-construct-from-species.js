function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(f, constructor, label) {
    let error;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(`${label}: expected a ${constructor.name} but got ${String(error)}`);
}

function recordReads(regExp, reads, flags) {
    Object.defineProperty(regExp, "constructor", { get() { reads.push("constructor"); return RegExp; } });
    Object.defineProperty(regExp, "flags", { get() { reads.push("flags"); return flags; } });
    Object.defineProperty(regExp, Symbol.match, { get() { reads.push("@@match"); return true; } });
    return regExp;
}

function testSplitReadOrder() {
    const reads = [];
    shouldBe("aBc".split(recordReads(/b/, reads, "i")).join("|"), "a|c", "split with flags from a getter");
    shouldBe(reads.join(), "constructor,flags,@@match", "reads of split");
}

function testMatchAllReadOrder() {
    const reads = [];
    const matches = [...recordReads(/b/, reads, "gi")[Symbol.matchAll]("aBcb")];
    shouldBe(matches.map(match => match[0]).join(), "B,b", "matchAll with flags from a getter");
    shouldBe(reads.join(), "constructor,flags,@@match", "reads of matchAll");
}

function testMatchAllLastIndex() {
    const regExp = /\d/g;
    regExp.tag = 1;
    regExp.lastIndex = 1;
    shouldBe([...regExp[Symbol.matchAll]("1a2b3")].map(match => match[0]).join(), "2,3", "matchAll from lastIndex");
    shouldBe(regExp.lastIndex, 1, "lastIndex of the receiver");
}

function testPatternReadAfterMatchGetter() {
    const regExp = /,/;
    Object.defineProperty(regExp, Symbol.match, { get() { regExp.compile(";"); return true; } });
    shouldBe("a,b;c".split(regExp).join("|"), "a,b|c", "split after the @@match getter recompiled the receiver");
}

function testSyntaxErrors() {
    const invalidFlags = /,/;
    Object.defineProperty(invalidFlags, "flags", { value: "z" });
    shouldThrow(() => "a,b".split(invalidFlags), SyntaxError, "split with invalid flags");
    shouldThrow(() => invalidFlags[Symbol.matchAll]("a,b"), SyntaxError, "matchAll with invalid flags");

    const invalidInUnicodeMode = /\-/;
    Object.defineProperty(invalidInUnicodeMode, "flags", { value: "u" });
    shouldThrow(() => "a-b".split(invalidInUnicodeMode), SyntaxError, "split with a pattern that is invalid with the new flags");
}

function testDefaultConstructor() {
    const regExp = /,/;
    regExp.constructor = undefined;
    shouldBe("a,b".split(regExp).join("|"), "a|b", "split with an undefined constructor");
    shouldBe([...regExp[Symbol.matchAll]("a,b")].length, 1, "matchAll with an undefined constructor");
}

function testSubclassWithRegExpSpecies() {
    class Derived extends RegExp {
        static get [Symbol.species]() { return RegExp; }
    }
    shouldBe("a,b".split(new Derived(",")).join("|"), "a|b", "split of a subclass");
    shouldBe([...new Derived(",", "g")[Symbol.matchAll]("a,b,c")].length, 2, "matchAll of a subclass");
}

function testReceiverWithoutRegExpMatcher() {
    const receiver = { constructor: undefined, flags: "", source: ",", [Symbol.match]: true };
    shouldBe(RegExp.prototype[Symbol.split].call(receiver, "a,b").join("|"), "a|b", "split of a plain object");
}

function split(string, separator) {
    return string.split(separator);
}
noInline(split);

function testOptimizedCaller() {
    const regExp = /,/;
    regExp.tag = 1;
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(split("a,b", regExp).join("|"), "a|b", "split from an optimized caller");
}

function testConstructedObject() {
    const receivers = [];
    const originalExec = RegExp.prototype.exec;
    RegExp.prototype.exec = function (string) {
        receivers.push(this);
        return originalExec.call(this, string);
    };
    const regExp = /,/i;
    let pieces;
    try {
        pieces = "a,b".split(regExp);
    } finally {
        RegExp.prototype.exec = originalExec;
    }
    shouldBe(pieces.join("|"), "a|b", "split through a replaced exec");
    shouldBe(receivers.length > 0, true, "calls of the replaced exec");
    for (const receiver of receivers) {
        shouldBe(receiver, receivers[0], "one splitter");
        shouldBe(receiver === regExp, false, "the splitter is a new object");
    }
    const splitter = receivers[0];
    shouldBe(Object.getPrototypeOf(splitter), RegExp.prototype, "prototype of the splitter");
    shouldBe(splitter.source, ",", "source of the splitter");
    shouldBe(splitter.flags, "iy", "flags of the splitter");
    shouldBe(splitter.compile("x").source, "x", "legacy features of the splitter");
}

testSplitReadOrder();
testMatchAllReadOrder();
testMatchAllLastIndex();
testPatternReadAfterMatchGetter();
testSyntaxErrors();
testDefaultConstructor();
testSubclassWithRegExpSpecies();
testReceiverWithoutRegExpMatcher();
testOptimizedCaller();
testConstructedObject();
