function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " (expected " + expected + ")");
}

// A BM search range must never span a position that matches every character. setAll() records
// such a position in its count alone and leaves its map empty, so merging one drops every
// character it matches out of the range's union, and the search then skips past positions that
// can start a match. Every case below therefore expects a MATCH: the failure is a false negative,
// so an expectation of null could not catch it. Each one is written with single dots rather than
// `.{n}`, because a quantified count aborts BM collection before any position is recorded and
// would leave nothing for the search to get wrong.
for (var i = 0; i < 1e2; ++i) {
    shouldBe(JSON.stringify("bab".match(/.(a)./)), '["bab","a"]');
    shouldBe(JSON.stringify("bab".match(/.(a)./u)), '["bab","a"]');
    shouldBe(JSON.stringify("bab".match(/.(a)./du).indices), "[[0,3],[1,2]]");
    shouldBe(JSON.stringify("xaz".match(/.a./)), '["xaz"]');
    shouldBe(JSON.stringify("qqzaaqq".match(/z.a/)), '["zaa"]');
    shouldBe(JSON.stringify("aaaXb".match(/X./)), '["Xb"]');
    shouldBe(JSON.stringify("abcQdef".match(/...Q.../)), '["abcQdef"]');
    shouldBe(JSON.stringify("zzza1b2c".match(/a.b.c/)), '["a1b2c"]');
    shouldBe(JSON.stringify("zzq12r".match(/q..r/)), '["q12r"]');
    // A dot that cannot match a newline, and inverted classes, are all-set positions too.
    shouldBe(JSON.stringify("a\nbXc".match(/b.c/)), '["bXc"]');
    shouldBe(JSON.stringify("aaaaK".match(/[^0-9]K/)), '["aK"]');
    shouldBe(JSON.stringify(("z".repeat(37) + "yP").match(/[^P]P/)), '["yP"]');
    // A range wider than the map turns into an all-set position the same way.
    shouldBe(JSON.stringify("aaaŐeaaa".match(/[Ā-Ȁ]e/)), '["Őe"]');
}

// The search is available to unicode patterns whose subject is Latin1, where no surrogate pair
// exists for a code-unit stride to land inside. These cover that newly reachable path; they too
// expect matches, so an over-skipping search fails them.
var subject = "the quick brown fox jumps over the lazy dog. ".repeat(64);
for (var i = 0; i < 1e2; ++i) {
    shouldBe(JSON.stringify(("xxqbravo" + subject + "zalpha").match(/zalpha|qbravo|xcharlie/gu)), '["qbravo","zalpha"]');
    shouldBe(subject.match(/\bfox\b/gu).length, 64);
    shouldBe(JSON.stringify("abcd".match(new RegExp("[\\q{abc}d]", "gv"))), '["abc","d"]');
}
