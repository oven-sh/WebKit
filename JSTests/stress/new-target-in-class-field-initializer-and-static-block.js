// new.target in a class field initializer or in a class static block is the new.target of that class element.
// It is undefined. It must not make the code around the class a user of new.target.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

function shouldBeAsync(promise, expected, message) {
    let result;
    let error;
    promise.then(value => { result = value; }, e => { error = e; });
    drainMicrotasks();
    if (error)
        throw error;
    shouldBe(result, expected, message);
}

// The class is in an arrow function in global code.
{
    const instanceField = () => { class A { x = typeof new.target; } return new A().x; };
    const staticField = () => { class A { static x = typeof new.target; } return A.x; };
    const staticBlock = () => { let r; class A { static { r = typeof new.target; } } return r; };
    const privateField = () => { class A { #x = typeof new.target; get x() { return this.#x; } } return new A().x; };
    const computedField = () => { class A { ["x"] = typeof new.target; } return new A().x; };
    const classExpression = () => new (class { x = typeof new.target; })().x;
    const derivedClass = () => { class B { } class A extends B { x = typeof new.target; } return new A().x; };
    const classWithConstructor = () => { class A { x = typeof new.target; constructor() { this.y = new.target; } } let a = new A(); return a.x + " " + (a.y === A); };
    const arrowInArrow = () => (() => { class A { x = typeof new.target; } return new A().x; })();
    const arrowInField = () => { class A { x = (() => typeof new.target)(); } return new A().x; };
    const arrowInStaticBlock = () => { let r; class A { static { r = (() => typeof new.target)(); } } return r; };
    const blockInStaticBlock = () => { let r; class A { static { if (true) { r = typeof new.target; } } } return r; };
    const keyOfClassInField = () => { class A { x = class { static [typeof new.target] = 1; }; } return Object.keys(new A().x)[0]; };
    const fieldOfClassInField = () => { class A { x = class { static y = typeof new.target; }; } return new A().x.y; };
    const fieldOfClassInStaticBlock = () => { let r; class A { static { class B { y = typeof new.target; } r = new B().y; } } return r; };
    const parameterOfArrowInField = () => { class A { x = ((y = typeof new.target) => y)(); } return new A().x; };
    const evalInField = () => { class A { x = eval("typeof new.target"); } return new A().x; };
    const evalInStaticBlock = () => { let r; class A { static { r = eval("typeof new.target"); } } return r; };

    for (let i = 0; i < 100; ++i) {
        shouldBe(instanceField(), "undefined", "instance field");
        shouldBe(staticField(), "undefined", "static field");
        shouldBe(staticBlock(), "undefined", "static block");
        shouldBe(privateField(), "undefined", "private field");
        shouldBe(computedField(), "undefined", "computed field");
        shouldBe(classExpression(), "undefined", "class expression");
        shouldBe(derivedClass(), "undefined", "derived class");
        shouldBe(classWithConstructor(), "undefined true", "class with constructor");
        shouldBe(arrowInArrow(), "undefined", "arrow function in arrow function");
        shouldBe(arrowInField(), "undefined", "arrow function in field");
        shouldBe(arrowInStaticBlock(), "undefined", "arrow function in static block");
        shouldBe(blockInStaticBlock(), "undefined", "block in static block");
        shouldBe(keyOfClassInField(), "undefined", "key of a class in a field");
        shouldBe(fieldOfClassInField(), "undefined", "field of a class in a field");
        shouldBe(fieldOfClassInStaticBlock(), "undefined", "field of a class in a static block");
        shouldBe(parameterOfArrowInField(), "undefined", "parameter of an arrow function in a field");
        shouldBe(evalInField(), "undefined", "eval in field");
        shouldBe(evalInStaticBlock(), "undefined", "eval in static block");
    }
}

// The class is in an async arrow function in global code. The call must not throw.
{
    const asyncArrow = async () => { class A { x = typeof new.target; } return new A().x; };
    const asyncArrowWithAwait = async () => { await 1; class A { static x = typeof new.target; static y; static { A.y = typeof new.target; } } return A.x + " " + A.y; };
    const asyncArrowWithParameters = async (a = 1, ...rest) => { class A { x = typeof new.target; } return new A().x; };

    shouldBeAsync(asyncArrow(), "undefined", "async arrow function");
    shouldBeAsync(asyncArrowWithAwait(), "undefined undefined", "async arrow function with await");
    shouldBeAsync(asyncArrowWithParameters(), "undefined", "async arrow function with parameters");
}

// The class is in an arrow function with parameters that are not simple.
{
    const arrowWithParameters = (a = 1, { b } = { b: 2 }, ...rest) => { class A { x = typeof new.target; static { A.y = typeof new.target; } } return new A().x + " " + A.y; };
    shouldBe(arrowWithParameters(), "undefined undefined", "arrow function with parameters");
}

// The class is in an arrow function in a function. The function does not save new.target for a static block.
{
    function staticBlockInArrowInFunction() {
        return (() => { let r; class A { static { r = typeof new.target; } } return r; })();
    }
    function fieldInArrowInFunction() {
        return (() => { class A { x = typeof new.target; static y = typeof new.target; } return new A().x + " " + A.y; })();
    }
    function Constructor() {
        this.staticBlock = (() => { let r = 1; class A { static { r = new.target; } } return r; })();
        this.field = (() => { class A { x = new.target; } return new A().x; })();
        this.target = (() => new.target)();
    }
    const method = { method() { return (() => { let r; class A { x = typeof new.target; static { r = typeof new.target; } } return new A().x + " " + r; })(); } }.method;

    for (let i = 0; i < 100; ++i) {
        shouldBe(staticBlockInArrowInFunction(), "undefined", "static block, arrow function in function");
        shouldBe(new staticBlockInArrowInFunction() instanceof staticBlockInArrowInFunction, true, "static block, arrow function in constructor");
        shouldBe(fieldInArrowInFunction(), "undefined undefined", "field, arrow function in function");
        let object = new Constructor();
        shouldBe(object.staticBlock, undefined, "static block, arrow function in constructor");
        shouldBe(object.field, undefined, "field, arrow function in constructor");
        shouldBe(object.target, Constructor, "arrow function in constructor");
        shouldBe(method(), "undefined undefined", "arrow function in method");
    }
}

// The class is in eval code.
{
    shouldBe((0, eval)("class A { x = typeof new.target; } new A().x"), "undefined", "field, indirect eval");
    shouldBe((0, eval)("class B { static x = typeof new.target; } B.x"), "undefined", "static field, indirect eval");
    shouldBe((0, eval)("var r; class C { static { r = typeof new.target; } } r"), "undefined", "static block, indirect eval");
    shouldBe((0, eval)("(() => { class A { x = typeof new.target; } return new A().x; })()"), "undefined", "arrow function, indirect eval");
    shouldBe(eval("class D { x = typeof new.target; } new D().x"), "undefined", "field, direct eval in global code");
    shouldBe((() => eval("class A { x = typeof new.target; static { A.y = typeof new.target; } } new A().x + ' ' + A.y"))(), "undefined undefined", "direct eval in arrow function");
    function directEvalInFunction() {
        return eval("class A { x = new.target; static { A.y = new.target; } } [new A().x, A.y, new.target]");
    }
    let [field, staticBlock, target] = new directEvalInFunction();
    shouldBe(field, undefined, "field, direct eval in constructor");
    shouldBe(staticBlock, undefined, "static block, direct eval in constructor");
    shouldBe(target, directEvalInFunction, "direct eval in constructor");
    shouldBe(new Function("return (() => { class A { x = typeof new.target; } return new A().x; })()")(), "undefined", "Function constructor");
}

// A function in a class element has its own new.target. A key and a heritage belong to the code around the class.
{
    const functionInField = () => { class A { f = function () { return new.target; }; } return new A().f; };
    const functionInStaticBlock = () => { let f; class A { static { f = function () { return new.target; }; } } return f; };
    function Keys() {
        class A { [new.target.name] = 1; static [new.target.name + "Static"] = 2; [new.target.name + "Method"]() { } }
        this.keys = Object.keys(new A()).concat(Object.keys(A), Object.getOwnPropertyNames(A.prototype));
    }
    function Heritage() {
        class A extends new.target.Base { }
        this.result = new A() instanceof Heritage.Base;
    }
    Heritage.Base = class { };
    function KeysInArrow() {
        this.keys = (() => { class A { [new.target.name] = typeof new.target; } let a = new A(); return Object.keys(a)[0] + " " + a.KeysInArrow; })();
    }

    for (let i = 0; i < 100; ++i) {
        let f = functionInField();
        shouldBe(f(), undefined, "function in field, call");
        shouldBe(new f(), f, "function in field, construct");
        let g = functionInStaticBlock();
        shouldBe(g(), undefined, "function in static block, call");
        shouldBe(new g(), g, "function in static block, construct");
        shouldBe(new Keys().keys.join(), "Keys,KeysStatic,constructor,KeysMethod", "keys");
        shouldBe(new Heritage().result, true, "heritage");
        shouldBe(new KeysInArrow().keys, "KeysInArrow undefined", "key and field in arrow function in constructor");
    }
}
