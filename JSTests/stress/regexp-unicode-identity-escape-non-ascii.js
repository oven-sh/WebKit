// Under the u and v flags, IdentityEscape only allows SyntaxCharacter or '/'
// (plus ClassSetReservedPunctuator inside v-mode class sets). All of those are
// ASCII, so a non-ASCII character after '\' must be a SyntaxError.

function shouldThrowSyntaxError(source, flags)
{
    let threw = false;
    try {
        new RegExp(source, flags);
    } catch (e) {
        threw = e instanceof SyntaxError;
    }
    if (!threw)
        throw new Error("new RegExp(" + JSON.stringify(source) + ", \"" + flags + "\") should have thrown a SyntaxError");
}

function shouldCompile(source, flags)
{
    new RegExp(source, flags);
}

shouldThrowSyntaxError("\\Ç", "u");
shouldThrowSyntaxError("\\Ç", "v");
shouldThrowSyntaxError("\\é", "u");
shouldThrowSyntaxError("\\é", "v");
shouldThrowSyntaxError("\\字", "u");
shouldThrowSyntaxError("\\𝒳", "u");
shouldThrowSyntaxError("\\𝒳", "v");
shouldThrowSyntaxError("[\\Ç]", "u");
shouldThrowSyntaxError("[\\Ç]", "v");
shouldThrowSyntaxError("[\\q{\\Ç}]", "v");
shouldThrowSyntaxError("\\q", "u");
shouldThrowSyntaxError("\\\u0000", "u");

shouldCompile("\\$", "u");
shouldCompile("\\/", "u");
shouldCompile("\\.", "v");
shouldCompile("[\\]]", "u");
shouldCompile("[\\-]", "u");
shouldCompile("[\\&]", "v");
shouldCompile("\\Ç", "");
shouldCompile("Ç", "u");
shouldCompile("\\u00C7", "u");

if (!new RegExp("\\Ç").test("Ç"))
    throw new Error("non-unicode /\\Ç/ should match \"Ç\"");
if (!new RegExp("Ç", "u").test("Ç"))
    throw new Error("/Ç/u should match \"Ç\"");
if (!new RegExp("\\u00C7", "u").test("Ç"))
    throw new Error("/\\u00C7/u should match \"Ç\"");
