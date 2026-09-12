// A pointer to the table of a Map or a Set is not a reference to the cell that precedes the table in
// memory. The conservative scan treated it as one, as a possible past-the-end butterfly pointer. That
// is costly for these tables: an obsolete table refers to the table that replaced it, so one marked
// obsolete table keeps every later table of its owner alive, with the keys and the values.

function churn(collection, add, rounds)
{
    for (let i = 0; i < rounds; ++i) {
        add(collection, new Array(1024).fill(i)); // About 8KB.
        collection.clear(); // Replaces the table.
    }
}

function test(Collection, add)
{
    // Allocate the tables in pairs, so that the table of pinned[i] follows the table of churned[i] in memory.
    const pairCount = 64;
    const churned = [];
    const pinned = [];
    for (let i = 0; i < pairCount; ++i) {
        const first = new Collection;
        const second = new Collection;
        add(first, 0);
        add(second, 0);
        churned.push(first);
        pinned.push(second);
    }

    const rounds = 64;
    function nest(i)
    {
        if (i === pairCount) {
            const before = fullGC();
            for (const collection of churned)
                churn(collection, add, rounds);
            return fullGC() - before;
        }
        let result;
        // forEach keeps the table of pinned[i] in its frame while the callback runs.
        pinned[i].forEach(() => {
            result = nest(i + 1);
        });
        return result;
    }

    // 64 collections * 64 rounds * 8KB = 32MB when the first obsolete table of each collection stays alive.
    const growth = nest(0);
    if (growth > 8 * 1024 * 1024)
        throw new Error(`${Collection.name}: the heap grew by ${growth} bytes`);
}

test(Map, (map, value) => map.set(value, value));
test(Set, (set, value) => set.add(value));
