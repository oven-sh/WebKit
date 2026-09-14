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
};

function foo() {
    {   
        assertThrow(() => f, "ReferenceError: f is not defined");
        eval('eval(" { function f() { }; } ")');
        assert(typeof f, "function");
    }
    assert(typeof f, "function");
}

for (var i = 0; i < testLoopCount; i++){
    foo();
    assertThrow(() => f, "ReferenceError: f is not defined");
}

function boo() {
    {
        assertThrow(() => l, "ReferenceError: l is not defined");
        eval('{ var l = 15; eval(" { function l() { }; } ")}');
        assert(typeof l, "function", "#3");
    }
    assert(typeof l, 'function', "#4");
}

for (var i = 0; i < testLoopCount; i++){
    boo();
    assertThrow(() => l, "ReferenceError: l is not defined");
}

function hoo() {
    {
        assertThrow(() => h, "ReferenceError: h is not defined");
        eval('eval(" if (false){ function h() { }; } ");');
        assert(h, undefined, '');
    }
    assert(h, undefined, '');
}

for (var i = 0; i < testLoopCount; i++){
    hoo();
    assertThrow(() => h, "ReferenceError: h is not defined");
}

function joo() {
    {
        assertThrow(() => h, "ReferenceError: h is not defined");
        eval('eval(" if (true){ function h() { }; } ")');
        assert(typeof h, "function" );
    }
    assert(typeof h, "function", "#10");
}

for (var i = 0; i < testLoopCount; i++){
    joo();
    assertThrow(() => h, "ReferenceError: h is not defined");
}

function koo() {
    {
        var k = 20;
        eval('var k = 15; eval(" if (true){ function k() { }; } ")');
        assert(typeof k, "function" );
    }
    assert(typeof k, "function", "#12");
}

for (var i = 0; i < testLoopCount; i++){
    koo();
    assertThrow(() => k, "ReferenceError: k is not defined");
}
