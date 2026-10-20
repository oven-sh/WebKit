export function mapFromEntries(entries) {
    "use strict";

    const map = $constructList($Map, new.target, []);
    for (const entry of entries)
        map.set(entry[0], entry[1]);
    return map;
}

export function weakMapFromEntries(entries) {
    "use strict";

    const map = $constructList($WeakMap, new.target, []);
    for (const entry of entries)
        map.set(entry[0], entry[1]);
    return map;
}

export function setFromValues(values) {
    "use strict";

    const set = $constructList($Set, new.target, []);
    for (const value of values)
        set.add(value);
    return set;
}

export function weakSetFromValues(values) {
    "use strict";

    const set = $constructList($WeakSet, new.target, []);
    for (const value of values)
        set.add(value);
    return set;
}
