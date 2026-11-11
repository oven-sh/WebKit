// In a derived constructor, an arrow function that has eval code or an arrow function with super(), and the body of an
// async arrow function, read |this| again before each use. The read needs a register. The value of an assignment to a
// super property, and the key of a for-of target, stay in their own registers while |this| is read.

function shouldBe(actual, expected, name)
{
    if (actual !== expected)
        throw new Error(`${name}: expected ${String(expected)} but got ${typeof actual === "object" ? Object.prototype.toString.call(actual) : String(actual)}`);
}

class Base {
    set setter(value) { this.stored = value; }
}

const settled = [];

class Derived extends Base {
    constructor()
    {
        super();
        const key = "setter";
        const withEval = () => { eval(""); return super[key] = 21; };
        const withEvalAndConstantKey = () => { eval(""); return super["setter"] = 22; };
        const withSuperCall = () => { const notCalled = () => { super(); }; return super[key] = 23; };
        const forOfWithEval = () => { eval(""); for (super["la" + "st"] of [5, 6]); return this.last; };
        const asyncBody = async () => { await 0; return super[key] = 24; };
        const forOfInAsyncBody = async () => { await 0; for (super["la" + "st"] of [7, 8]); return this.last; };

        shouldBe(withEval(), 21, "arrow function with eval code, value of the assignment");
        shouldBe(this.stored, 21, "arrow function with eval code, value that the setter got");
        shouldBe(withEvalAndConstantKey(), 22, "arrow function with eval code and a constant key, value of the assignment");
        shouldBe(this.stored, 22, "arrow function with eval code and a constant key, value that the setter got");
        shouldBe(withSuperCall(), 23, "arrow function with an arrow function that has super(), value of the assignment");
        shouldBe(this.stored, 23, "arrow function with an arrow function that has super(), value that the setter got");
        shouldBe(forOfWithEval(), 6, "arrow function with eval code, for-of to a super property");

        const name = "body of an async arrow function";
        asyncBody().then(value => {
            settled.push(name);
            shouldBe(value, 24, `${name}, value of the assignment`);
            shouldBe(this.stored, 24, `${name}, value that the setter got`);
            return forOfInAsyncBody();
        }).then(value => {
            settled.push(name);
            shouldBe(value, 8, `${name}, for-of to a super property`);
        }).catch(error => settled.push(error));
    }
}

for (let i = 0; i < testLoopCount; ++i)
    new Derived;

drainMicrotasks();
for (const result of settled) {
    if (result instanceof Error)
        throw result;
}
shouldBe(settled.length, 2 * testLoopCount, "async arrow functions settle");
