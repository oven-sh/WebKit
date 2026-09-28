import m from "./mixed-classes.py";
function show(label, f) { try { print(label, "=>", f()); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
const s = v => JSON.stringify(v);

// ---- py -> js
class Dog extends m.Animal {
    constructor(name) { m.log.push("Dog before"); super(name); m.log.push("Dog after"); this.tricks = 1; }
    sound() { return "woof"; }
    extra() { return "extra " + this.name; }
    get upper() { return this.name.toUpperCase(); }
    set upper(v) { this.name = v.toLowerCase(); }
    parent() { return super.sound(); }
    static create() { return new Dog("static"); }
    static count = 7;
}
let d;
show("still JavaScript's", () => [typeof Dog, Object.getPrototypeOf(Dog) === m.Animal, Object.getPrototypeOf(Dog.prototype) === m.Animal, Dog.name, Dog.kind].join(" "));
show("new", () => { d = new Dog("rex"); return [d.name, d.tricks, d instanceof Dog, d instanceof m.Animal, Object.getPrototypeOf(d) === Dog.prototype, d.constructor === Dog].join(" "); });
show("order", () => s(m.take_log()));
show("JS calls", () => [d.sound(), d.speak(), d.extra(), d.kind, d.parent(), d.upper, d.loud].join(" | "));
show("Python sees", () => s(m.describe(d)));
show("the class is the class", () => [m.type_of(d) === Dog, m.type_of(Dog) === m.type_of(m.Animal), String(m.type_of(d)).slice(0, 9)].join(" "));
show("Python calls what only JS has", () => [m.call(d, "extra"), m.call(d, "parent")].join(" | "));
show("classmethod", () => { const x = Dog.make("cm"); return [x instanceof Dog, x.speak(), x.tricks].join(" "); });
show("statics", () => [Dog.create().name, Dog.count].join(" "));
show("accessor from Python", () => { m.setattr(d, "upper", "MAX"); return [m.getattr(d, "upper"), d.name].join(" "); });
m.take_log();

// ---- js -> py -> js, and py again below that
class Base {
    constructor(x) { m.log.push("Base"); this.base = x; }
    who() { return "base"; }
    onlyBase() { return "only base " + this.base; }
    static tag = "T";
    static st() { return "static of " + this.name; }
}
const Middle = m.derive(Base);
class Top extends Middle {
    constructor(x) { m.log.push("Top before"); super(x); m.log.push("Top after"); this.top = x; }
    who() { return "top:" + super.who(); }
}
const Bottom = m.derive_again(Top);
for (const [label, C] of [["Base", Base], ["Middle", Middle], ["Top", Top], ["Bottom", Bottom]]) {
    show(label + " mro", () => s(m.names(C)));
    show(label + " new", () => { const o = new C(5); return [o.who(), o.onlyBase(), s(Object.keys(o)), s(m.take_log())].join(" | "); });
    show(label + " called by Python", () => { const o = m.call(m, "type_of", 0) && m.make(C, 6); return [o.who(), m.call(o, "who"), m.type_of(o) === C, s(m.take_log())].join(" | "); });
    show(label + " instanceof", () => { const o = new C(1); return [Base, Middle, Top, Bottom].map(K => (o instanceof K ? 1 : 0) + "" + (m.isinstance(o, K) ? 1 : 0)).join(" "); });
    show(label + " issubclass", () => [Base, Middle, Top, Bottom].map(K => m.issubclass(C, K) ? 1 : 0).join(" "));
    show(label + " statics", () => [C.tag, C.st(), m.getattr(C, "tag")].join(" "));
    m.take_log();
}
show("only_middle", () => [new Top(3).only_middle(), new Bottom(4).only_middle()].join(" | "));
