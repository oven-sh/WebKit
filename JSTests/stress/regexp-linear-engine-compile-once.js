//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--useRegExpJIT=0")

// A RegExp asks the non-backtracking matcher for a program once, and then has code for every
// subject: the program runs 8-bit and 16-bit strings, short and long ones. A RegExp that the
// matcher refused for its pattern does not ask again either. `linearCompiles` of
// $vm.regExpMatchStatistics() counts the asks of every RegExp.

// This RegExp asks in the first call, and has its program in the others.
let counted = /a RegExp that does not ask again/;
function asksSoFar()
{
    return $vm.regExpMatchStatistics(counted, "", 0).linearCompiles;
}

function expectAsks(name, since, expected)
{
    let asks = asksSoFar() - since;
    if (asks !== expected)
        throw new Error(name + ": the matcher was asked " + asks + " times, expected " + expected);
}

let accepted = /(\d{4})-(\d{2})-(\d{2})/;
let acceptedInALoop = /^(?:[a-z]+ ?)+$/;
let refused = /(\d{4})-(\d{2})-\2/;

let eightBit = "on 2026-10-10, at noon";
let sixteenBit = "on 2026-10-10, at noon \u2603";
let long = "word ".repeat(4000);
let longSixteenBit = long + "\u2603";

let atStart = asksSoFar();
for (let i = 0; i < 2000; ++i) {
    if (!accepted.test(eightBit) || accepted.exec(eightBit).index !== 3 || !accepted.test(sixteenBit) || accepted.exec(sixteenBit)[3] !== "10")
        throw new Error(accepted + " did not match");
    if (!refused.test(eightBit) || refused.exec(eightBit).index !== 3 || !refused.test(sixteenBit) || refused.exec(sixteenBit)[2] !== "10")
        throw new Error(refused + " did not match");
}
// One match that reads 20,000 characters.
if (!acceptedInALoop.test(long) || acceptedInALoop.test(longSixteenBit) || acceptedInALoop.exec(long).index)
    throw new Error(acceptedInALoop + " did not match");
for (let i = 0; i < 200; ++i)
    acceptedInALoop.test(eightBit);

for (let regExp of [accepted, acceptedInALoop]) {
    let statistics = $vm.regExpMatchStatistics(regExp, eightBit, 0);
    if (statistics.engine !== "linear")
        throw new Error(regExp + " ran on " + statistics.engine + " (" + statistics.refusal + ")");
}
if ($vm.regExpMatchStatistics(refused, eightBit, 0).refusal !== "backreference")
    throw new Error(refused + " was not refused for its backreference");
// One ask for each of the three. The JIT compiled the refused one for each width of character,
// and the matcher was asked before the first.
expectAsks("after the matches", atStart, 3);

// A RegExp whose code is deleted is compiled again, so an accepted one asks again. A pattern
// stays refused: nothing about it changed.
$vm.deleteAllCodeWhenIdle();
setTimeout(() => {
    // `counted` asks again here.
    let afterTheCodeWasDeleted = asksSoFar();
    for (let i = 0; i < 100; ++i) {
        if (!accepted.test(eightBit) || !accepted.test(sixteenBit) || !refused.test(eightBit) || !refused.test(sixteenBit))
            throw new Error("No match after the code was deleted");
    }
    expectAsks("after the code was deleted", afterTheCodeWasDeleted, 1);
}, 0);
