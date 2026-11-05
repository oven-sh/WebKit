//@ memoryHog!
var hugeString = "x";
for (i = 0; i < 25; ++i) {
    hugeString += hugeString;
}

var exception;
var weird = 'Â';
try {
    var f = new Function(hugeString, hugeString, hugeString, hugeString, hugeString, hugeString, hugeString,
      hugeString, hugeString, hugeString, hugeString, hugeString, hugeString, hugeString,
      hugeString, hugeString, hugeString, hugeString, hugeString, hugeString, hugeString,
      () => 42,
      "return 42;");
} catch (e) {
    exception = e;
}

// Bun: upstream expects "RangeError: Out of memory". The text of the arrow function has 16-bit characters (for `weird`, so has this file), and there a StringBuilder whose capacity has passed 2**30 asks for String::MaxLength of them next, which no buffer can have. Here it asks for as many as one can have (maxCapacity in wtf/text/StringBuilder.cpp), and the parser gets to see the text.
if (exception != "SyntaxError: Unexpected token '('. Expected a parameter pattern or a ')' in parameter list.")
    throw "FAIL";

// Bun: the rest is the fork's own, and is what upstream's test is for: a StringBuilder that overflows in the Function constructor. Upstream throws the same.
hugeString += hugeString;
hugeString += hugeString;
try {
    new Function(hugeString, hugeString, hugeString, hugeString, hugeString, hugeString, hugeString,
      hugeString, hugeString, hugeString, hugeString, hugeString, hugeString, hugeString,
      () => 42,
      hugeString, hugeString,
      "return 42;");
} catch (e) {
    exception = e;
}

if (exception != "RangeError: Out of memory")
    throw "FAIL";
