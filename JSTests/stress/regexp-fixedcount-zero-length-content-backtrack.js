// Test that FixedCount patterns with empty alternatives and content backtracking work correctly.
// Regression test for zero-length match detection in FixedCount content backtracking.

// Pattern from slow.js that was hanging:
// (?:[^(?!)]||){23}z - FixedCount with 3 alternatives, 2 empty
// Bun: upstream expects test() to return false. The search is abandoned at Options::regExpMatchLimit
// before it reaches the match at index 14, and this fork throws for that instead of reporting no match.
var abandoned = null;
try {
    /(?:[^(?!)]||){23}z/.test("/(?:[^(?!)]||){23}z/");
} catch (e) {
    abandoned = e;
}
if (!(abandoned instanceof RangeError) || abandoned.message !== "Regular expression backtracking limit exceeded")
    throw new Error("Expected RangeError: Regular expression backtracking limit exceeded, got " + abandoned);

// FixedCount with empty alternatives - should allow zero-length iterations
if (!/(?:a||){5}z/.test("aaaz"))
    throw new Error("Should match: 3 iterations of 'a', 2 of empty, then 'z'");

// Content backtracking should still work for non-empty alternatives
var result = /(?:a|b){3}c/.exec("abbc");
if (!result || result[0] !== "abbc")
    throw new Error("Expected 'abbc', got " + result);

// FixedCount with all empty alternatives followed by required match
if (!/(?:||){10}z/.test("z"))
    throw new Error("Should match 'z' after 10 empty iterations");

// Another test: empty alternative should work
if (!/(?:a|){2}b/.test("b"))
    throw new Error("Should match: 2 empty iterations then 'b'");
