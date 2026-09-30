// A rope that is mostly its first string, if that is a long one, is resolved into a buffer that it shares with that string, if there is room after it and nothing has been written there. So strings that were made
// from one another by appending are prefixes of one buffer. None of them is ever to change, whatever is appended to any of them, in whatever order they are resolved.

// It is for a first string that is longer than a page, of which the largest that there are have 64K bytes.
const long = 70000;

let state = 12345;
function below(n)
{
    state = (Math.imul(state, 1103515245) + 12345) >>> 0;
    return (state >>> 8) % n;
}

// Each is a string, and what its code units should be, which is kept apart from it. Those that begin with something long have that in common, and it is not gone through a unit at a time: `start` is a string that
// is the same as it and was made apart from it.
function make(units) { return { string: String.fromCharCode(...units), units, start: "" }; }
function resolve(string) { return string.charCodeAt(string.length >> 1); }

function check(entry, label)
{
    const { string, units, start } = entry;
    if (string.length !== start.length + units.length)
        throw new Error(`${label}: length ${string.length}, expected ${start.length + units.length}`);
    if (!string.startsWith(start))
        throw new Error(`${label}: what it begins with has changed`);
    for (let i = 0; i < units.length; ++i) {
        if (string.charCodeAt(start.length + i) !== units[i])
            throw new Error(`${label}: at ${i} of ${units.length} after what it begins with is ${string.charCodeAt(start.length + i)}, expected ${units[i]}`);
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

// What is added has nothing long at the beginning of it.
function append(a, b) { return { string: a.string + b.string, units: a.units.concat(b.units), start: a.start }; }
function append3(a, b, c) { return { string: `${a.string}${b.string}${c.string}`, units: a.units.concat(b.units, c.units), start: a.start }; }

function longPiece(alphabet)
{
    const parts = [[], []];
    for (let i = 0; i < long; i += 1000) {
        const units = piece(alphabet, 1000).units;
        for (const part of parts)
            part.push(String.fromCharCode(...units));
    }
    const [string, start] = parts.map(part => part.join(""));
    resolve(string);
    resolve(start);
    return { string, units: [], start };
}

const holder = { };
for (let round = 0; round < 300; ++round) {
    const alphabet = alphabets[round % alphabets.length];
    const other = alphabets[below(alphabets.length)];
    // Two rounds in three begin with something long. The rest do not, and are resolved as ropes always were.
    const isLong = round % 3;
    const begin = () => isLong ? longPiece(alphabet) : piece(alphabet, 1 + below(40));
    let live = [begin()];
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
            // To itself, and to the front, which are not what it is for. What comes of it is only looked at here.
            if (isLong) {
                const twice = from.string + from.string;
                const behind = "<" + from.string;
                resolve(twice);
                resolve(behind);
                if (twice.length !== 2 * from.string.length || !twice.endsWith(from.string) || !twice.startsWith(from.string) || behind.slice(1) !== from.string)
                    throw new Error(`round ${round} step ${step}: to itself or to the front`);
                made = append(from, small());
            } else
                made = below(2) ? append(from, from) : append(small(), from);
            resolve(made.string);
            break;
        case 8: {
            // All but the end of one, and that added to. It begins where the buffer does, and does not end where what has been used of it ends.
            const kept = below(from.units.length + 1);
            made = append({ string: from.string.substring(0, from.start.length + kept), units: from.units.slice(0, kept), start: from.start }, small());
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
            made = append(from, piece(alphabet, (isLong ? 300 : from.units.length) + below(20)));
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
        if (made.units.length > 3000 || live.length > 40) {
            live = live.filter(() => below(3));
            if (!live.length || !isLong)
                live.push(isLong ? begin() : piece(alphabet, 1 + below(10)));
        }
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
    for (let i = 0; i < 3 * long; ++i) {
        s += unit;
        resolve(s);
        if (!(i % 997))
            kept.push(s);
    }
    if (s !== unit.repeat(3 * long))
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
    for (let i = 0; i < 3 * long; ++i) {
        const unit = i < 2 * long ? 97 + (i % 26) : 0x4E00 + (i % 50);
        s += String.fromCharCode(unit);
        units.push(unit);
        resolve(s);
    }
    check({ string: s, units, start: "" }, "narrow to wide");
}

// A string that has been appended to already is appended to again, so that the second result has to have a buffer of its own. Then the same is done to that one, and so on.
// Each buffer has room in proportion to the string that is put in it. If it were in proportion to the buffer before it, it would double each time, though the strings hardly grow.
{
    let s = "a".repeat(long) + "b";
    resolve(s);
    const rounds = 40;
    for (let i = 0; i < rounds; ++i) {
        // This goes after it, where it is kept.
        resolve(s + "x");
        // So this cannot.
        s += "y";
        resolve(s);
    }
    if (s !== "a".repeat(long) + "b" + "y".repeat(rounds))
        throw new Error("appended to twice, over and over");
    fullGC();
    if (gcHeapSize() > 100 * long)
        throw new Error("a string of " + s.length + " characters is kept, and the heap has " + gcHeapSize() + " bytes");
}
