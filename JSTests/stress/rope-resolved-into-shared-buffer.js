// A rope that is mostly its first string is resolved into a buffer that it shares with that string, if there is room after it and nothing has been written there. So strings that were made from one another by
// appending are prefixes of one buffer. None of them is ever to change, whatever is appended to any of them, in whatever order they are resolved.

let state = 12345;
function below(n)
{
    state = (Math.imul(state, 1103515245) + 12345) >>> 0;
    return (state >>> 8) % n;
}

// Each is a string, and what its code units should be, which is kept apart from it.
function make(units) { return { string: String.fromCharCode(...units), units }; }
function resolve(string) { return string.charCodeAt(string.length >> 1); }

function check(entry, label)
{
    const { string, units } = entry;
    if (string.length !== units.length)
        throw new Error(`${label}: length ${string.length}, expected ${units.length}`);
    for (let i = 0; i < units.length; ++i) {
        if (string.charCodeAt(i) !== units[i])
            throw new Error(`${label}: at ${i} of ${units.length} is ${string.charCodeAt(i)}, expected ${units[i]}`);
    }
}

const alphabets = [
    () => 97 + below(26),
    () => 0xA0 + below(0x60),
    () => 0x4E00 + below(100),
    () => below(4) ? 97 + below(26) : 0x4E00 + below(100),
    () => below(2) ? 0xD83D : 0xDE00 + below(4),
];

function piece(alphabet, length)
{
    const units = [];
    for (let i = 0; i < length; ++i)
        units.push(alphabet());
    return make(units);
}

function append(a, b) { return { string: a.string + b.string, units: a.units.concat(b.units) }; }
function append3(a, b, c) { return { string: `${a.string}${b.string}${c.string}`, units: a.units.concat(b.units, c.units) }; }

const holder = { };
for (let round = 0; round < 300; ++round) {
    const alphabet = alphabets[round % alphabets.length];
    const other = alphabets[below(alphabets.length)];
    let live = [piece(alphabet, 1 + below(40))];
    for (let step = 0; step < 120; ++step) {
        const from = live[below(live.length)];
        const small = () => piece(below(12) ? alphabet : other, 1 + below(4));
        let made;
        switch (below(12)) {
        case 0:
        case 1:
        case 2:
        case 3:
            // Added to and looked at, which is what it is all for.
            made = append(from, small());
            resolve(made.string);
            break;
        case 4:
            // Added to several times before it is looked at.
            made = append(append(append(from, small()), small()), small());
            if (below(2))
                resolve(made.string);
            break;
        case 5: {
            // The same one added to twice. Only one of the two can have what comes after it in the buffer.
            const first = append(from, small());
            const second = append(from, small());
            if (below(2)) {
                resolve(second.string);
                resolve(first.string);
            } else {
                resolve(first.string);
                resolve(second.string);
            }
            live.push(first);
            made = second;
            break;
        }
        case 6:
            made = append3(from, small(), small());
            resolve(made.string);
            break;
        case 7:
            // To itself, and to the front, which are not what it is for.
            made = below(2) ? append(from, from) : append(small(), from);
            resolve(made.string);
            break;
        case 8: {
            // A part of one, and that added to.
            const start = below(from.units.length);
            const end = start + below(from.units.length - start + 1);
            made = append({ string: from.string.substring(start, end), units: from.units.slice(start, end) }, small());
            resolve(made.string);
            break;
        }
        case 9:
            // As the name of a property, which makes an atom of it.
            made = append(from, small());
            holder[made.string] = 1;
            delete holder[made.string];
            break;
        case 10:
            // More than there was.
            made = append(from, piece(alphabet, from.units.length + below(20)));
            resolve(made.string);
            break;
        case 11:
            // What has been looked at is added to without being looked at, and something else is added to what it came from.
            made = append(from, small());
            live.push(append(made, small()));
            resolve(append(from, small()).string);
            break;
        }
        live.push(made);
        if (made.units.length > 3000 || live.length > 40)
            live = live.filter(() => below(3)).concat([piece(alphabet, 1 + below(10))]);
        if (!(step % 16)) {
            for (const entry of live)
                check(entry, `round ${round} step ${step}`);
        }
        if (!below(200))
            gc();
    }
    for (const entry of live)
        check(entry, `round ${round}`);
}

// A long one, a character at a time. Every one of them on the way is kept, and none is changed by what came after.
for (const unit of ["a", "é", "中", "😀"]) {
    const kept = [];
    let s = "";
    for (let i = 0; i < 20000; ++i) {
        s += unit;
        resolve(s);
        if (!(i % 997))
            kept.push(s);
    }
    if (s !== unit.repeat(20000))
        throw new Error("the last of them");
    kept.forEach((k, i) => {
        if (k !== unit.repeat(i * 997 + 1))
            throw new Error("one on the way: " + i);
    });
}

// It goes from narrow characters to wide ones part of the way through.
{
    let s = "";
    const units = [];
    for (let i = 0; i < 5000; ++i) {
        const unit = i < 3000 ? 97 + (i % 26) : 0x4E00 + (i % 50);
        s += String.fromCharCode(unit);
        units.push(unit);
        resolve(s);
    }
    check({ string: s, units }, "narrow to wide");
}
