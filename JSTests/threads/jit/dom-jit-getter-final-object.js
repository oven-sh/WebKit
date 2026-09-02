//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// The DOMJIT getter of $vm.createDOMJITGetterBaseJSObject() is annotated with
// JSObject::info(), and here it runs with a JSFinalObject as |this|. The DFG
// emits CheckJSCast(JSObject) for it. speculationFromClassInfoInheritance(JSObject)
// used to be SpecObjectOther only, so the first CFA found a contradiction there,
// constant folding then removed the check (the structure does inherit JSObject),
// and the blocks after it stayed marked unreachable: the compiled code trapped
// with DFGUnreachableBasicBlock. With useJSThreads the LLInt put_by_id transition
// cache is off, which keeps objects like the one in
// stress/dom-jit-with-poly-proto.js mono-proto and sends them down this path.

const proto = $vm.createDOMJITGetterBaseJSObject();
const object = Object.create(proto);
object.field = 25;

function validate(x, expected) {
    if (x.customGetter !== expected)
        throw new Error("bad customGetter: " + x.customGetter);
}
noInline(validate);

for (let i = 0; i < testLoopCount * 10; ++i)
    validate(object, proto);
