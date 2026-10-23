// Chains of WeakMap entries, where each value is (or leads to) the key of the next entry.
// Each builder returns { head, roots, walk }: walk(node) gives the next node of the chain, and
// the chain is reachable only through `head` and the WeakMaps in `roots`.
function ephemeronChainShapes(count) {
    return {
        direct() {
            const map = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                map.set(tail, next);
                tail = next;
            }
            return { head, roots: [map], walk: node => map.get(node) };
        },
        // Allocated in the opposite order of the walk: the last node allocated is the root.
        reverse() {
            const map = new WeakMap();
            const nodes = [];
            for (let id = 0; id <= count; id++)
                nodes.push({ id });
            for (let id = count; id > 0; id--)
                map.set(nodes[id], nodes[id - 1]);
            return { head: nodes[count], roots: [map], walk: node => map.get(node) };
        },
        twoMaps() {
            const a = new WeakMap();
            const b = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                (id % 2 ? a : b).set(tail, next);
                tail = next;
            }
            return { head, roots: [a, b], walk: node => a.get(node) ?? b.get(node) };
        },
        manyMaps() {
            const maps = [];
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                const map = new WeakMap();
                map.set(tail, next);
                maps.push(map);
                tail = next;
            }
            return { head, roots: maps, walk: node => maps[node.id]?.get(node) };
        },
        wrapper() {
            const map = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                map.set(tail, { next });
                tail = next;
            }
            return { head, roots: [map], walk: node => map.get(node)?.next };
        },
        closure() {
            const map = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                map.set(tail, () => next);
                tail = next;
            }
            return { head, roots: [map], walk: node => map.get(node)?.() };
        },
        prototype() {
            const map = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                map.set(tail, Object.create(next));
                tail = next;
            }
            return {
                head,
                roots: [map],
                walk: node => {
                    const value = map.get(node);
                    return value ? Object.getPrototypeOf(value) : undefined;
                },
            };
        },
        // Each map is reachable only through the value of the map before it.
        lateMaps() {
            const first = new WeakMap();
            const head = { id: 0 };
            let map = first;
            let key = head;
            for (let id = 1; id <= count; id++) {
                const nextMap = new WeakMap();
                const nextKey = { id };
                map.set(key, { map: nextMap, key: nextKey });
                map = nextMap;
                key = nextKey;
            }
            return {
                head: { node: head, map: first },
                roots: [],
                walk: ({ node, map }) => {
                    const value = map.get(node);
                    return value ? { node: value.key, map: value.map } : undefined;
                },
            };
        },
        symbolKeys() {
            const map = new WeakMap();
            const head = Symbol("0");
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = Symbol(String(id));
                map.set(tail, next);
                tail = next;
            }
            return { head, roots: [map], walk: node => map.get(node) };
        },
    };
}
