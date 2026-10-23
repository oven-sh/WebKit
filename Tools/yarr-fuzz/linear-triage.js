// Triage of the differences that drive.mjs records between a "linear" configuration and a
// backtracking one (jit~linear, jit~linear-interp or jit~linear-verify in out/mismatch-<profile>.jsonl).
//
// A backtracking engine that reaches its match limit answers "no match", whatever the subject has.
// The non-backtracking matcher has no such limit, so the two differ on those cases, and that is
// not a bug of the matcher. This tells the two kinds apart: it runs the matcher and the bytecode
// interpreter on every prefix of each subject. On the short prefixes the interpreter finishes,
// and the two must agree. On the long ones it gives up, which the functions below report (in
// them its limit is low, so that takes milliseconds).
//
// Usage: jsc --useDollarVM=1 --useRegExpLinearEngine=1 linear-triage.js -- out/mismatch-mixed.jsonl [...]
// One line per record. Exits with 1 when the two engines differ on a prefix.

const decode = (text) => text.replace(/\\u([0-9a-f]{4})/g, (all, hex) => String.fromCharCode(parseInt(hex, 16)));

let differences = 0;
for (const path of arguments) {
    for (const line of read(path).split("\n")) {
        if (!line)
            continue;
        const record = JSON.parse(line);
        if (!record.a || !/~linear(?:-|$)/.test(record.pair))
            continue;
        const regExp = new RegExp(decode(record.a.p), record.a.f);
        const subject = decode(record.a.s);
        const where = record.profile + " seed " + record.seed + " case " + record.k + " " + regExp;
        if ($vm.regExpMatchStatistics(regExp, "", 0).engine !== "linear") {
            print(where + ": the matcher refuses this pattern, so both configurations ran a backtracking engine");
            continue;
        }

        let agreed = 0;
        let agreedOnAMatch = 0;
        let givenUp = 0;
        const differAt = [];
        for (let length = 0; length <= subject.length; ++length) {
            const index = $vm.regExpMatchOnBothEngines(regExp, subject.slice(0, length), 0);
            if (index === null)
                ++givenUp;
            else if (index === false)
                differAt.push(length);
            else {
                ++agreed;
                if (index >= 0)
                    ++agreedOnAMatch;
            }
        }
        differences += differAt.length;
        print(where + ": " + (subject.length + 1) + " prefixes, the two engines agree on " + agreed + " (" + agreedOnAMatch + " of them a match), the interpreter gives up on " + givenUp
            + (differAt.length ? ", and THEY DIFFER on the prefixes of length " + differAt.join(", ") : ""));

        // The same for the matches of a global loop over the whole subject (match, matchAll, replace, split): each
        // starts where the one before it ended. A backtracking engine that gives up on one of them ends the loop there.
        let matches = 0;
        let agreedInLoop = 0;
        let givenUpInLoop = 0;
        const differFrom = [];
        for (let start = 0; start <= subject.length;) {
            const index = $vm.regExpMatchOnBothEngines(regExp, subject, start);
            if (index === null)
                ++givenUpInLoop;
            else if (index === false)
                differFrom.push(start);
            else
                ++agreedInLoop;
            const captures = $vm.regExpMatchStatistics(regExp, subject, start).captures;
            if (!captures)
                break;
            ++matches;
            start = captures[1] > captures[0] ? captures[1] : captures[1] + 1;
        }
        differences += differFrom.length;
        print("    as a global loop: " + matches + " matches on the matcher, the two engines agree on " + agreedInLoop + " searches, the interpreter gives up on " + givenUpInLoop
            + (differFrom.length ? ", and THEY DIFFER on the searches from " + differFrom.join(", ") : ""));
    }
}
if (differences)
    quit(1);
