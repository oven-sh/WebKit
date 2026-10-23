// Chains of WeakMap entries, where each value is (or leads to) the key of the next entry.
// Each builder returns { head, roots, walk }: walk(node) gives the next node of the chain, and
// the chain is reachable only through `head` and the WeakMaps in `roots`.
function ephemeronChainShapes(count) {
    function buildLateMaps(makeValue, keyOf) {
        const first = new WeakMap();
        const head = { id: 0 };
        let map = first;
        let key = head;
        for (let id = 1; id <= count; id++) {
            const nextMap = new WeakMap();
            const nextKey = { id };
            map.set(key, makeValue(nextMap, nextKey));
            map = nextMap;
            key = nextKey;
        }
        return {
            head: { node: head, map: first },
            roots: [],
            walk: ({ node, map }) => {
                const value = map.get(node);
                return value ? { node: keyOf(value), map: value.map } : undefined;
            },
        };
    }

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
        // The maps in the opposite order of the walk, so that a scan of the maps in allocation
        // order gets one link per scan.
        manyMapsReverse() {
            const nodes = [];
            const maps = [];
            for (let id = 0; id <= count; id++) {
                nodes.push({ id });
                maps.push(new WeakMap());
            }
            for (let id = count; id > 0; id--)
                maps[id].set(nodes[id], nodes[id - 1]);
            return { head: nodes[count], roots: maps, walk: node => maps[node.id]?.get(node) };
        },
        // The same, and each map also holds entries for live keys that are not in the chain.
        manyMapsWide() {
            const nodes = [];
            const maps = [];
            const others = [];
            for (let id = 0; id <= count; id++) {
                nodes.push({ id });
                const map = new WeakMap();
                for (let i = 0; i < 20; i++) {
                    const other = { id, i };
                    others.push(other);
                    map.set(other, { i });
                }
                maps.push(map);
            }
            for (let id = count; id > 0; id--)
                maps[id].set(nodes[id], nodes[id - 1]);
            return { head: nodes[count], roots: [maps, others], walk: node => maps[node.id]?.get(node) };
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
        // Each map is reachable only through the value of the map before it. The three
        // variants differ in whether the collector meets the next map before or after the next
        // key: the key comes first, the map comes first, or the key sits two objects deeper.
        lateMaps() {
            return buildLateMaps((nextMap, nextKey) => ({ key: nextKey, map: nextMap }), value => value.key);
        },
        lateMapsMapFirst() {
            return buildLateMaps((nextMap, nextKey) => ({ map: nextMap, key: nextKey }), value => value.key);
        },
        lateMapsKeyDeeper() {
            return buildLateMaps((nextMap, nextKey) => ({ map: nextMap, holder: { key: nextKey } }), value => value.holder.key);
        },
        // Each map also holds entries for keys that die, so it grows through several rehashes.
        lateMapsWide() {
            return buildLateMaps((nextMap, nextKey) => {
                for (let i = 0; i < 20; i++)
                    nextMap.set({ junk: i }, { junk: i });
                return { map: nextMap, key: nextKey };
            }, value => value.key);
        },
        // Each key is in two maps. The chain goes through the second one.
        twoEntriesPerKey() {
            const side = new WeakMap();
            const chain = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                side.set(tail, { side: id });
                chain.set(tail, next);
                tail = next;
            }
            return { head, roots: [side, chain], walk: node => side.get(node) ? chain.get(node) : undefined };
        },
        // A key in many maps, each reachable only through the chain.
        sharedKeyManyMaps() {
            const shared = { shared: true };
            const chain = new WeakMap();
            const head = { id: 0 };
            let tail = head;
            for (let id = 1; id <= count; id++) {
                const next = { id };
                const side = new WeakMap();
                side.set(shared, { id });
                chain.set(tail, { next, side });
                tail = next;
            }
            return { head, roots: [chain, shared], walk: node => chain.get(node)?.next };
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
