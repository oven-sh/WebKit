var assert = function (result, expected, message) {
    if (result !== expected) {
        throw new Error('Error in assert. Expected "' + expected + '" but was "' + result + '":' + message );
    }
};

var assertThrow = function (cb, expected) {
    let error = null;
    try {
        cb();
    } catch(e) {
        error = e;  
    }
    if (error === null) {
        throw new Error('Error is expected. Expected "' + expected + '" but error was not thrown."');
    }
    if (error.toString() !== expected) {
        throw new Error('Error is expected. Expected "' + expected + '" but error was "' + error + '"');
    }
}

function foo() {
    {
        let f = 20;
        eval(" { function f() { value = 20; }; }");
        assert(f, 20);
    }
    assertThrow(() => f, "ReferenceError: f is not defined");
}


for (var i = 0; i < testLoopCount; i++){
    foo();
    assertThrow(() => f, "ReferenceError: f is not defined");
}

function boo() {
    {
        var l = 20;
        eval(" { function l() { value = 20; }; }");
        assert(typeof l, 'function');
    }
    assert(typeof l, 'function');
}

for (var i = 0; i < testLoopCount; i++){
    boo();
    assertThrow(() => l, "ReferenceError: l is not defined");
}

function goo() {
    {
        let g = 20;
        eval(" for(var j=0; j < testLoopCount; j++){ function g() { }; } ");
        assert(typeof g, 'number');
    }
    assertThrow(() => g, "ReferenceError: g is not defined");
}

goo();
assertThrow(() => g, "ReferenceError: g is not defined");
