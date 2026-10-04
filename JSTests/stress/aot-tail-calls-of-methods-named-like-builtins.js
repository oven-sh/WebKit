//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault
"use strict";

let depth = 0, next = "";
const key = { };
const chain = {
    charCodeAt() { return this[next](); },
    charAt() { return this[next](); },
    codePointAt() { return this[next](); },
    startsWith() { return this[next](); },
    endsWith() { return this[next](); },
    lastIndexOf() { return this[next](); },
    substring() { return this[next](); },
    trim() { return this[next](); },
    trimStart() { return this[next](); },
    trimEnd() { return this[next](); },
    toLowerCase() { return this[next](); },
    toUpperCase() { return this[next](); },
    localeCompare() { return this[next](); },
    replace() { return this[next](); },
    replaceAll() { return this[next](); },
    split() { return this[next](); },
    match() { return this[next](); },
    search() { return this[next](); },
    at() { return this[next](); },
    includes() { return this[next](); },
    indexOf() { return this[next](); },
    slice() { return this[next](); },
    concat() { return this[next](); },
    valueOf() { return this[next](); },
    toString() { return this[next](); },
    push() { return this[next](); },
    pop() { return this[next](); },
    shift() { return this[next](); },
    unshift() { return this[next](); },
    join() { return this[next](); },
    splice() { return this[next](); },
    get() { return this[next](); },
    has() { return this[next](); },
    set() { return this[next](); },
    delete() { return this[next](); },
    add() { return this[next](); },
    test() { return this[next](); },
    exec() { return this[next](); },
    hasOwnProperty() { return this[next](); },
    getTime() { return this[next](); },
    getFullYear() { return this[next](); },
    getMonth() { return this[next](); },
    getDate() { return this[next](); },
    getDay() { return this[next](); },
    getHours() { return this[next](); },
    getMinutes() { return this[next](); },
    getSeconds() { return this[next](); },
    getMilliseconds() { return this[next](); },
    getTimezoneOffset() { return this[next](); },
    getUTCFullYear() { return this[next](); },
    getUTCMonth() { return this[next](); },
    getUTCDate() { return this[next](); },
    getUTCDay() { return this[next](); },
    getUTCHours() { return this[next](); },
    getUTCMinutes() { return this[next](); },
    getUTCSeconds() { return this[next](); },
    getUTCMilliseconds() { return this[next](); },
    viaCharCodeAt() { return depth-- > 0 ? this.charCodeAt(0) : "done"; },
    viaCharAt() { return depth-- > 0 ? this.charAt(0) : "done"; },
    viaCodePointAt() { return depth-- > 0 ? this.codePointAt(0) : "done"; },
    viaStartsWith() { return depth-- > 0 ? this.startsWith("a") : "done"; },
    viaEndsWith() { return depth-- > 0 ? this.endsWith("a") : "done"; },
    viaLastIndexOf() { return depth-- > 0 ? this.lastIndexOf("a") : "done"; },
    viaSubstring() { return depth-- > 0 ? this.substring(1) : "done"; },
    viaTrim() { return depth-- > 0 ? this.trim() : "done"; },
    viaTrimStart() { return depth-- > 0 ? this.trimStart() : "done"; },
    viaTrimEnd() { return depth-- > 0 ? this.trimEnd() : "done"; },
    viaToLowerCase() { return depth-- > 0 ? this.toLowerCase() : "done"; },
    viaToUpperCase() { return depth-- > 0 ? this.toUpperCase() : "done"; },
    viaLocaleCompare() { return depth-- > 0 ? this.localeCompare("a") : "done"; },
    viaReplace() { return depth-- > 0 ? this.replace("a", "b") : "done"; },
    viaReplaceAll() { return depth-- > 0 ? this.replaceAll("a", "b") : "done"; },
    viaSplit() { return depth-- > 0 ? this.split(",") : "done"; },
    viaMatch() { return depth-- > 0 ? this.match(/a/) : "done"; },
    viaSearch() { return depth-- > 0 ? this.search(/a/) : "done"; },
    viaAt() { return depth-- > 0 ? this.at(0) : "done"; },
    viaIncludes() { return depth-- > 0 ? this.includes("a") : "done"; },
    viaIndexOf() { return depth-- > 0 ? this.indexOf("a") : "done"; },
    viaSlice() { return depth-- > 0 ? this.slice(1) : "done"; },
    viaConcat() { return depth-- > 0 ? this.concat("a") : "done"; },
    viaValueOf() { return depth-- > 0 ? this.valueOf() : "done"; },
    viaToString() { return depth-- > 0 ? this.toString() : "done"; },
    viaPush() { return depth-- > 0 ? this.push(1) : "done"; },
    viaPushWith2Arguments() { return depth-- > 0 ? this.push(1, 2) : "done"; },
    viaPop() { return depth-- > 0 ? this.pop() : "done"; },
    viaShift() { return depth-- > 0 ? this.shift() : "done"; },
    viaUnshift() { return depth-- > 0 ? this.unshift(1) : "done"; },
    viaJoin() { return depth-- > 0 ? this.join(",") : "done"; },
    viaSplice() { return depth-- > 0 ? this.splice(0, 1) : "done"; },
    viaGet() { return depth-- > 0 ? this.get(key) : "done"; },
    viaHas() { return depth-- > 0 ? this.has(key) : "done"; },
    viaSet() { return depth-- > 0 ? this.set(key, 1) : "done"; },
    viaDelete() { return depth-- > 0 ? this.delete(key) : "done"; },
    viaAdd() { return depth-- > 0 ? this.add(key) : "done"; },
    viaTest() { return depth-- > 0 ? this.test("a") : "done"; },
    viaExec() { return depth-- > 0 ? this.exec("a") : "done"; },
    viaHasOwnProperty() { return depth-- > 0 ? this.hasOwnProperty("a") : "done"; },
    viaGetTime() { return depth-- > 0 ? this.getTime() : "done"; },
    viaGetFullYear() { return depth-- > 0 ? this.getFullYear() : "done"; },
    viaGetMonth() { return depth-- > 0 ? this.getMonth() : "done"; },
    viaGetDate() { return depth-- > 0 ? this.getDate() : "done"; },
    viaGetDay() { return depth-- > 0 ? this.getDay() : "done"; },
    viaGetHours() { return depth-- > 0 ? this.getHours() : "done"; },
    viaGetMinutes() { return depth-- > 0 ? this.getMinutes() : "done"; },
    viaGetSeconds() { return depth-- > 0 ? this.getSeconds() : "done"; },
    viaGetMilliseconds() { return depth-- > 0 ? this.getMilliseconds() : "done"; },
    viaGetTimezoneOffset() { return depth-- > 0 ? this.getTimezoneOffset() : "done"; },
    viaGetUTCFullYear() { return depth-- > 0 ? this.getUTCFullYear() : "done"; },
    viaGetUTCMonth() { return depth-- > 0 ? this.getUTCMonth() : "done"; },
    viaGetUTCDate() { return depth-- > 0 ? this.getUTCDate() : "done"; },
    viaGetUTCDay() { return depth-- > 0 ? this.getUTCDay() : "done"; },
    viaGetUTCHours() { return depth-- > 0 ? this.getUTCHours() : "done"; },
    viaGetUTCMinutes() { return depth-- > 0 ? this.getUTCMinutes() : "done"; },
    viaGetUTCSeconds() { return depth-- > 0 ? this.getUTCSeconds() : "done"; },
    viaGetUTCMilliseconds() { return depth-- > 0 ? this.getUTCMilliseconds() : "done"; },
};

const failures = [];
for (const [via, isLowered] of [["viaCharCodeAt", false], ["viaCharAt", false], ["viaCodePointAt", false], ["viaStartsWith", true], ["viaEndsWith", true], ["viaLastIndexOf", true], ["viaSubstring", true], ["viaTrim", true], ["viaTrimStart", true], ["viaTrimEnd", true], ["viaToLowerCase", true], ["viaToUpperCase", true], ["viaLocaleCompare", true], ["viaReplace", true], ["viaReplaceAll", false], ["viaSplit", true], ["viaMatch", true], ["viaSearch", true], ["viaAt", false], ["viaIncludes", false], ["viaIndexOf", false], ["viaSlice", false], ["viaConcat", false], ["viaValueOf", false], ["viaToString", true], ["viaPush", false], ["viaPushWith2Arguments", true], ["viaPop", false], ["viaShift", true], ["viaUnshift", true], ["viaJoin", true], ["viaSplice", true], ["viaGet", false], ["viaHas", false], ["viaSet", false], ["viaDelete", false], ["viaAdd", false], ["viaTest", true], ["viaExec", true], ["viaHasOwnProperty", false], ["viaGetTime", true], ["viaGetFullYear", true], ["viaGetMonth", true], ["viaGetDate", true], ["viaGetDay", true], ["viaGetHours", true], ["viaGetMinutes", true], ["viaGetSeconds", true], ["viaGetMilliseconds", true], ["viaGetTimezoneOffset", true], ["viaGetUTCFullYear", true], ["viaGetUTCMonth", true], ["viaGetUTCDate", true], ["viaGetUTCDay", true], ["viaGetUTCHours", true], ["viaGetUTCMinutes", true], ["viaGetUTCSeconds", true], ["viaGetUTCMilliseconds", true]]) {
    depth = 300001;
    next = via;
    let outcome;
    try {
        outcome = chain[via]();
    } catch (error) {
        outcome = error.constructor.name;
    }
    if (outcome !== "done")
        failures.push(via + ": " + outcome);
    const remarks = typeof aotRemarks === "function" ? aotRemarks(via) : null;
    if (remarks && isLowered && !remarks.some(remark => remark.startsWith("lowered-builtin:")))
        failures.push(via + " is not lowered");
}
if (failures.length)
    throw new Error(failures.length + " failures:\n" + failures.join("\n"));
