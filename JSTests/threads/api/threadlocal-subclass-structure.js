//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// The ThreadLocal constructor honors newTarget (a subclass instance gets the
// subclass prototype, Reflect.construct gets newTarget.prototype) and every
// plain instance shares the realm's one cached Structure instead of minting
// a fresh one per `new ThreadLocal()`.
load("../harness.js", "caller relative");

class TL extends ThreadLocal {
    describe() { return "tl:" + String(this.value); }
}
const sub = new TL();
shouldBeTrue(sub instanceof TL, "subclass instance is an instanceof the subclass");
shouldBeTrue(sub instanceof ThreadLocal);
shouldBe(Object.getPrototypeOf(sub), TL.prototype);
sub.value = 7;
shouldBe(sub.describe(), "tl:7");
shouldBe(new Thread(() => sub.describe()).join(), "tl:undefined", "subclass instance is still a per-thread slot");

function Other() {}
const reflected = Reflect.construct(ThreadLocal, [], Other);
shouldBe(Object.getPrototypeOf(reflected), Other.prototype, "Reflect.construct uses newTarget.prototype");
const valueDescriptor = Object.getOwnPropertyDescriptor(ThreadLocal.prototype, "value");
valueDescriptor.set.call(reflected, 1);
shouldBe(valueDescriptor.get.call(reflected), 1, "the cell is a real ThreadLocal");

function NoProto() {}
NoProto.prototype = null;
shouldBe(Object.getPrototypeOf(Reflect.construct(ThreadLocal, [], NoProto)), ThreadLocal.prototype,
    "a non-object newTarget.prototype falls back to ThreadLocal.prototype");

const a = new ThreadLocal();
const b = new ThreadLocal();
shouldBe(Object.getPrototypeOf(a), ThreadLocal.prototype);
shouldBe($vm.getStructureTransitionList(a)[0], $vm.getStructureTransitionList(b)[0], "plain instances share one Structure");
