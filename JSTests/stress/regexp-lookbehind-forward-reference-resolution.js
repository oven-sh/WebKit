// Inside a lookbehind (matched right to left) a reference written before its group is a real
// backreference. That must also hold when an enclosing group is quantified (its body is copied)
// and for a duplicate-named group whose instances all closed before the lookbehind.
function shouldBe(actual, expected) { if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error("expected " + JSON.stringify(expected) + " got " + JSON.stringify(actual)); }
const ex = (re, s) => { const m = re.exec(s); return m && [m.index, ...m]; };
shouldBe(ex(/(?<=(?:\1(a))+)b/, "xab"), null);
shouldBe(ex(/(?<=(?:\1(a))+)b/, "aaab"), [3, "b", "a"]);
shouldBe(ex(/(?<=(?:\1(a)){1,3})b/, "xab"), null);
shouldBe(ex(/(?<=(?:\2(a)(a))+)b/, "aab"), null);
shouldBe(ex(/(?<=(?:\1a)+)b/, "aab"), null);
shouldBe(ex(/(?<=\1(a))b/, "aab"), [2, "b", "a"]);
shouldBe(ex(/(?<=(?:\k<q>(?<q>a))+)b/, "xab"), null);
shouldBe(ex(/(?<=(?:\k<q>(?<q>a))+)b/, "aab"), [2, "b", "a"]);
shouldBe(ex(/(?:(?<n>a)|(?<n>b))(?<=x\k<n>)/, "xa"), [1, "a", "a", undefined]);
shouldBe(ex(/(?:(?<n>a)|(?<n>b))(?<=x\k<n>)/, "xb"), [1, "b", undefined, "b"]);
shouldBe(ex(/(?:(?<n>a)|(?<n>b))(?<=\k<n>{2})/, "a"), null);
shouldBe(ex(/(?:(?<n>a)|(?<n>b))(?<=\k<n>{2})/, "aa"), [1, "a", "a", undefined]);
// The reference is copied (its enclosing group quantified) BEFORE its target closes.
shouldBe(ex(/(?<=(?:\1x){1,3}(a))b/, "yxab"), null);
shouldBe(ex(/(?<=(?:\1x){1,3}(a))b/, "axab"), [3, "b", "a"]);
shouldBe(ex(/(?<=(?:\k<q>x)+(?<q>a))b/, "yxab"), null);
shouldBe(ex(/(?<=(\2x)+(a))b/, "axab"), [3, "b", "ax", "a"]);
// A later duplicate-named group closing inside the lookbehind.
shouldBe(ex(/(?<=(?<a>q)|(?:\k<a>(?<a>a))+)b/, "xab"), null);
shouldBe(ex(/(?<=(?<a>q)|(?:\k<a>(?<a>a))+)b/, "aab"), [2, "b", undefined, "a"]);
