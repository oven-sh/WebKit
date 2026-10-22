export function entries(object) {
    "use strict";

    const ownProperties = { __proto__: null, ...object };
    const result = [];
    for (const key in ownProperties)
        $arrayPush(result, [key, ownProperties[key]]);
    return result;
}

export function fromEntries(iterable) {
    "use strict";

    // No literal: what the call answers has no layout of this file.
    const object = Object.create(Object.prototype);
    for (const entry of iterable)
        $putByValDirect(object, entry[0], entry[1]);
    return object;
}

export function groupBy(items, callback) {
    "use strict";

    const groups = Object.create(null);
    let index = 0;
    for (const item of items) {
        const group = [];
        $putByValDirect(groups, $toPropertyKey(callback(item, index++)), group);
        $arrayPush(group, item);
    }
    return groups;
}

export function toLocaleString() {
    "use strict";

    return this.toString();
}

export function toClass(target, name, base) {
    "use strict";

    // The native code defines each property: it asks no object above for a setter.
    const prototype = Object.create(base ? base.prototype : Object.prototype);
    Object.defineProperty(prototype, "constructor", { value: target, writable: true, configurable: true });
    Object.setPrototypeOf(target, base ? base : Function.prototype);
    Object.defineProperty(target, "prototype", { value: prototype, writable: true, configurable: true });
    Object.defineProperty(target, "name", { value: name });
}
