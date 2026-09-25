function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const config = null;

class Base {
    field = config.url;
    constructor(name) { // line 10
        this.name = name;
        this.ready = true;
    }
}

class WithPrivateBrand {
    #method() { }
    field = config.url;
    constructor() { // line 19
        this.x = 1;
    }
}

class Thrower {
    field = (() => { throw new Error("from an initializer"); })();
    constructor() { // line 26
        this.y = 2;
        this.z = 3;
    }
}

function lineOfConstructorFrame(Class) {
    try {
        new Class("a");
    } catch (error) {
        let frame = error.stack.split("\n").find(line => line.startsWith(Class.name + "@"));
        return Number(/:(\d+):\d+$/.exec(frame)[1]);
    }
    throw new Error("did not throw");
}

// The constructor calls the field initializer before its first statement, so its frame is where it starts.
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(lineOfConstructorFrame(Base), 10);
    shouldBe(lineOfConstructorFrame(WithPrivateBrand), 19);
    shouldBe(lineOfConstructorFrame(Thrower), 26);
}
