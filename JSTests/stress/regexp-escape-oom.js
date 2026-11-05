//@ memoryHog!
//@ runDefault

var input = '퀀'.repeat?.(2 ** 30);
var result;
var exception;
try {
    result = RegExp.escape(input);
} catch (e) {
    exception = e;
}
// Bun: upstream expects exception == 'RangeError: Out of memory'. There a StringBuilder whose capacity is 2**30 asks for String::MaxLength characters next, which no buffer of 16-bit characters can have. Here it asks for as many as one can have (maxCapacity in wtf/text/StringBuilder.cpp), and nothing in this string is escaped.
if (exception !== undefined || result !== input)
  throw 'FAILED';

// Bun: the rest is the fork's own, and is what upstream's test is for: a StringBuilder that overflows in RegExp.escape(). Each of these characters becomes six. Upstream throws the same.
input = result = undefined;
try {
    RegExp.escape('\u2028'.repeat(2 ** 30));
} catch (e) {
    exception = e;
}
if (exception != 'RangeError: Out of memory')
  throw 'FAILED';
