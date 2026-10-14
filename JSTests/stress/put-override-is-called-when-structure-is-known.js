//@ requireOptions("--useDollarVM=1")

// put() of this object converts the value to a string before storing it, and disables caching on its slot.
const object = $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
object.property = 0;

function store(value) {
    object.property = value;
    return object.property;
}
noInline(store);

for (let i = 0; i < testLoopCount; ++i) {
    const stored = store(i);
    if (stored !== String(i))
        throw new Error(`iteration ${i}: stored ${typeof stored} ${stored}`);
}
