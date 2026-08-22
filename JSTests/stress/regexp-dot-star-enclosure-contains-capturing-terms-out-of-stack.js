// This tests that a .*-wrapped pattern nested far beyond the compiler's stack budget is
// rejected cleanly (originally through containsCapturingTerms()'s recursion guard; that
// analysis no longer recurses, so whichever pass reaches the depth first throws) instead of
// crashing. Under $memoryLimited depths nothing may throw at all, which is also fine.
//@ exclusive!
//@ requireOptions("-e", "let depth=25000") if $memoryLimited

depth = typeof(depth) === 'undefined' ? 200000 : depth;

// Which limit trips first depends on how far compilation gets: the pattern-size guard
// (SyntaxError) or, once the enclosure analysis no longer recurses, the same nesting
// limit any other deeply nested pattern reaches (RangeError). Either is a clean throw.
let expectedExceptions = [
    "SyntaxError: Invalid regular expression: regular expression too large",
    "RangeError: Out of memory: Invalid regular expression: too many nested disjunctions",
];

function test(source)
{
    try {
        new RegExp(source);
    } catch (e) {
        if (!expectedExceptions.includes(String(e)))
            throw "Expected one of " + JSON.stringify(expectedExceptions) + ", but got \"" + e + "\" for: " + source.slice(0, 30) + "...";
    }
}

test(".*" + "(?:".repeat(depth) + "a" + ")".repeat(depth) + ".*");
test("^.*" + "(?:".repeat(depth) + "a" + ")".repeat(depth) + ".*$");
