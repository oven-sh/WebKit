description(

"This test checks that an Array->string conversion that reaches the same array again, either directly or through another object's toString, substitutes the empty string for the repeat visit, as V8 and SpiderMonkey do (Bun keeps StringRecursionChecker for arrays)."

);

var arr = [1, 2];
var obj = {};
var originalToString = Object.prototype.toString;
Object.prototype.toString = function() { return "*" + arr + "*"; }
arr[2] = arr;
arr[3] = obj;

shouldBe("arr.join()", "'1,2,,**'");
shouldBe("arr.toString()", "'1,2,,**'");
shouldBe("String(arr)", "'1,2,,**'");

Object.prototype.toString = originalToString;

// Without a cycle the very same shape converts normally, and the array reached from the element's
// toString is converted rather than skipped.
var other = [1, 2];
var plain = {};
plain.toString = function() { return "*" + other + "*"; }

shouldBeEqualToString("[3, plain].join()", "3,*1,2*");
shouldBeEqualToString("[3, plain].toString()", "3,*1,2*");
