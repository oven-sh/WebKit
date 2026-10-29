//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0", "--useRegExpImageMode=1")
//@ runDefault

// A pattern that is one set, or a run of one set, takes a path of its own on flat programs. "|(?!)" sends the same pattern through the machine.
function check(source, flags, text) {
  const expected = new RegExp(source + "|(?!)", flags);
  const actual = new RegExp(source, flags);
  for (let i = 0; i < 2; i++) {
    const a = actual.exec(text), b = expected.exec(text);
    const sa = a && a.index + ":" + a[0] + ":" + actual.lastIndex, sb = b && b.index + ":" + b[0] + ":" + expected.lastIndex;
    if (sa !== sb) throw new Error(`${source}/${flags} on ${JSON.stringify(text)}: ${sa} instead of ${sb}`);
  }
  if (text.replace(new RegExp(source, flags), "#") !== text.replace(new RegExp(source + "|(?!)", flags), "#")) throw new Error("replace " + source);
  if (text.search(new RegExp(source, flags)) !== text.search(new RegExp(source + "|(?!)", flags))) throw new Error("search " + source);
  if (String(text.split(new RegExp(source, flags))) !== String(text.split(new RegExp(source + "|(?!)", flags)))) throw new Error("split " + source);
}
const sources = ["\\d+", "\\d", "[aeiou]", "[aeiou]+", "\\s+", "\\w+", "\\W", "[^a]", "[^a]+", ".", ".+", "[\\u0100-\\u0200]+", "\\S+", "x+", "[x]", "[xX]", "[Aa]+"];
const texts = ["", "a", "1", "ab12 cd", "   ", "\n\n", "aeiou", "xxXX", "ĀāaȀ", "é", "abc😀def", "12345678901234567890123456789012345678", "a".repeat(40) + "1", " ".repeat(33) + "x"];
for (const s of sources) for (const f of ["", "g", "i", "m", "s", "gi", "y", "d"]) for (const t of texts) check(s, f, t);
