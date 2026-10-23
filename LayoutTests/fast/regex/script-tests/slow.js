description(
'Test for expressions that would hang when evaluated due to exponential matching behavior. If the test does not hang it is a success.'
);

// Bun: upstream expects false. The search is abandoned at Options::regExpMatchLimit before it reaches the match at index 14, and this fork throws for that instead of reporting no match.
shouldThrow('/(?:[^(?!)]||){23}z/.test("/(?:[^(?!)]||){23}z/")', '"RangeError: Regular expression backtracking limit exceeded"');
