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


function bar() {
    {
        let f = 20;
        let value = 10; 
        eval("function f() { value = 20; }; f();");
    }
}


for (var i = 0; i < testLoopCount; i++){
    assertThrow(() => bar(), "SyntaxError: Can't create duplicate variable in eval: 'f'");
    assertThrow(() => f, "ReferenceError: f is not defined");
}

function baz() {
    {
        var l = 20;
        var value = 10;
        eval("function l() { value = 20; }; l();");
        assert(typeof l, 'function');
        assert(value, 20);
    }
    assert(typeof l, 'function');
}

for (var i = 0; i < testLoopCount; i++){
    baz();
    assertThrow(() => l, "ReferenceError: l is not defined");
}

function foobar() {
    {
        let g = 20;
        let value = 10;
        eval("function l() { value = 30; }; l();");
        assert(typeof g, 'number');
        assert(value, 30);
    }
    assertThrow(() => g, "ReferenceError: g is not defined");
}

foobar();
assertThrow(() => g, "ReferenceError: g is not defined");

(function() {
    try {
        let b;
        eval('var a; var b;');
    } catch (e) {
        var error = e;
    }

    assert(error.toString(), "SyntaxError: Can't create duplicate variable in eval: 'b'");
    assertThrow(() => a, "ReferenceError: a is not defined");
    assertThrow(() => b, "ReferenceError: b is not defined");
})();

(function() {
    try {
        let x1;
        eval('function x1() {} function x2() {} function x3() {}');
    } catch (e) {
        var error = e;
    }

    assert(error.toString(), "SyntaxError: Can't create duplicate variable in eval: 'x1'");
    assertThrow(() => x1, "ReferenceError: x1 is not defined");
    assertThrow(() => x2, "ReferenceError: x2 is not defined");
    assertThrow(() => x3, "ReferenceError: x3 is not defined");
})();

(function() {
    var x3;
    try {
        let x2;
        eval('function x1() {} function x2() {} function x3() {}');
    } catch (e) {
        var error = e;
    }

    assert(error.toString(), "SyntaxError: Can't create duplicate variable in eval: 'x2'");
    assertThrow(() => x1, "ReferenceError: x1 is not defined");
    assertThrow(() => x2, "ReferenceError: x2 is not defined");
    assert(x3, undefined);
})();
