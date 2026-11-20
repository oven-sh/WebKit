function shouldThrow(source) {
    let error;
    try {
        new Function(source);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`expected a SyntaxError from ${source}, got ${String(error)}`);
    return String(error);
}

if ("Decimal" in globalThis)
    throw new Error("Decimal needs --useDecimal=1");

// Literals are the syntax error they have been.
for (const source of ["1m", "1.5m", ".5m", "1e3m", "-1m", "0m", "({ 1m: 2 })"]) {
    if (shouldThrow(source) !== "SyntaxError: No identifiers allowed directly after numeric literal")
        throw new Error(shouldThrow(source));
}
