function f(){g();g();g();g();g();g();g();g();}
function g() { }

load("./driver/driver.js");

// Its constructor is in a text of the engine's own, and the offsets of that text are not offsets into this file.
class NeverConstructed { }

f();
checkBasicBlock(f, "g()", ShouldHaveExecuted);
