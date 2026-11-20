function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function target(a, b, c) { return [this, a, b, c].join(); }
function bind(f, a) { return f.bind("this", a); }
noInline(bind);

for (let i = 0; i < testLoopCount; ++i) {
    let inner = bind(target, 1);
    let outer = bind(inner, 2);
    let outermost = bind(bind(outer, 3), 4);
    switch (i % 6) {
    case 0:
        break;
    case 1:
        Object.defineProperty(inner, "name", { value: "later" });
        Object.defineProperty(inner, "length", { value: 42 });
        break;
    case 2:
        delete inner.name;
        delete inner.length;
        break;
    case 3:
        shouldBe(inner.name, "bound target");
        shouldBe(inner.length, 2);
        break;
    case 4:
        Object.freeze(inner);
        break;
    case 5:
        Object.setPrototypeOf(inner, null);
        break;
    }
    shouldBe(outermost.name, "bound bound bound bound target");
    shouldBe(outermost.length, 0);
    shouldBe(outer.name, "bound bound target");
    shouldBe(outer.length, 1);
    shouldBe(outer(3), "this,1,2,3");
}

for (let i = 0; i < testLoopCount; ++i) {
    let inner = bind(target, 1);
    let name = "bound bound target";
    let length = 1;
    switch (i % 5) {
    case 0:
        Object.defineProperty(inner, "name", { value: "before" });
        name = "bound before";
        break;
    case 1:
        Object.defineProperty(inner, "length", { value: Infinity });
        length = Infinity;
        break;
    case 2:
        delete inner.name;
        delete inner.length;
        name = "bound ";
        length = 0;
        break;
    case 3: {
        let reads = 0;
        Object.defineProperty(inner, "name", { get() { return `read ${++reads}`; } });
        bind(inner, 2);
        name = "bound read 2";
        break;
    }
    case 4:
        inner.other = i;
        break;
    }
    let outer = bind(inner, 2);
    shouldBe(outer.name, name);
    shouldBe(outer.length, length);
}

{
    let renamed = function original(a, b) { };
    Object.defineProperty(renamed, "name", { value: "renamed" });
    Object.defineProperty(renamed, "length", { value: 7 });
    let outer = bind(bind(renamed, 1), 2);
    shouldBe(outer.name, "bound bound renamed");
    shouldBe(outer.length, 5);
    shouldBe(bind(bind(new Proxy(target, { }), 1), 2).name, "bound bound target");
    shouldBe(bind(bind(Map, 1), 2).name, "bound bound Map");
    shouldBe(Object.getPrototypeOf(bind(bind(class extends Map { }, 1), 2)), Map);
    shouldBe(bind(new Intl.Collator().compare, "a").name, "bound ");
    shouldBe(bind(new Intl.Collator().compare, "a").length, 1);
}
