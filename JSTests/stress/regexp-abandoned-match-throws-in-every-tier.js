//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0")
//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--useRegExpJIT=0")

// A search that is abandoned at Options::regExpMatchLimit throws on every call, in every tier. The
// RegExp and the string are constants here, which is what DFG strength reduction folds: it must
// never turn these calls into false, null, -1 or the unchanged string.

const limitMessage = "Regular expression backtracking limit exceeded";
const constantRegExp = /(?:a|aa)+b|c/;

function testLiteral() {
    return /(?:a|aa)+b|c/.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac");
}
noInline(testLiteral);

function execLiteral() {
    return /(?:a|aa)+b|c/.exec("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac");
}
noInline(execLiteral);

function testConstant() {
    return constantRegExp.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac");
}
noInline(testConstant);

function searchLiteral() {
    return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".search(/(?:a|aa)+b|c/);
}
noInline(searchLiteral);

function replaceLiteral() {
    return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".replace(/(?:a|aa)+b|c/, "X");
}
noInline(replaceLiteral);

function replaceGlobalLiteral() {
    return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".replace(/(?:a|aa)+b|c/g, "");
}
noInline(replaceGlobalLiteral);

function matchLiteral() {
    return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".match(/(?:a|aa)+b|c/);
}
noInline(matchLiteral);

const functions = [testLiteral, execLiteral, testConstant, searchLiteral, replaceLiteral, replaceGlobalLiteral, matchLiteral];

for (let i = 0; i < testLoopCount; i++) {
    for (const run of functions) {
        let result;
        let error = null;
        try {
            result = run();
        } catch (e) {
            error = e;
        }
        if (!(error instanceof RangeError) || error.message !== limitMessage)
            throw new Error(run.name + " at iteration " + i + ": expected the RangeError, got " + (error || JSON.stringify(result)));
    }
}
