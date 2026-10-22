export function forEach(procedure) {
    "use strict";

    const next = this.next;
    let counter = 0;
    for (;;) {
        const result = next.$call(this);
        if (result.done)
            return;
        $ifAbruptCloseIterator(this, procedure(result.value, counter++));
    }
}

export function toArray() {
    "use strict";

    const next = this.next;
    const items = [];
    for (;;) {
        const result = next.$call(this);
        if (result.done)
            return items;
        $arrayPush(items, result.value);
    }
}
