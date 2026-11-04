class ReferenceRegExp {
    static Unsupported = class Unsupported extends Error { };

    static #codeUnitCount = 0x10000;
    static #dotlessI = 0x0131;
    static #decimalDigits = [[0x0030, 0x0039]];
    static #basicWordCharacters = [[0x0030, 0x0039], [0x0041, 0x005A], [0x005F, 0x005F], [0x0061, 0x007A]];
    static #extraWordCharacters = [[0x017F, 0x017F], [0x212A, 0x212A]];
    static #lineTerminators = [[0x000A, 0x000A], [0x000D, 0x000D], [0x2028, 0x2029]];
    static #whiteSpace = [
        [0x0009, 0x000D], [0x0020, 0x0020], [0x00A0, 0x00A0], [0x1680, 0x1680], [0x2000, 0x200A],
        [0x2028, 0x2029], [0x202F, 0x202F], [0x205F, 0x205F], [0x3000, 0x3000], [0xFEFF, 0xFEFF],
    ];
    static #characterClassEscapes = new Map([
        ["d", ["digit", false]], ["D", ["digit", true]], ["w", ["word", false]],
        ["W", ["word", true]], ["s", ["space", false]], ["S", ["space", true]],
    ]);
    static #controlEscapes = new Map([["t", 0x0009], ["n", 0x000A], ["v", 0x000B], ["f", 0x000C], ["r", 0x000D]]);
    static #caseVariantTables = new Map();
    static #accept = (end, captures) => ({ end, captures });

    #source;
    #ignoreCase;
    #multiline;
    #dotAll;
    #unicodeSets;
    #hasEitherUnicodeFlag;
    #sticky;
    #position = 0;
    #groupCount = 0;
    #groupNames = new Map();
    #backreferences = [];
    #root;
    #subject = "";

    constructor(source, flags) {
        this.#source = source;
        for (const flag of flags) {
            if (!"imsuvygd".includes(flag))
                throw this.#unsupported(`flag ${flag}`);
        }
        this.#ignoreCase = flags.includes("i");
        this.#multiline = flags.includes("m");
        this.#dotAll = flags.includes("s");
        this.#unicodeSets = flags.includes("v");
        this.#hasEitherUnicodeFlag = flags.includes("u") || flags.includes("v");
        this.#sticky = flags.includes("y");
        this.#root = this.#parseDisjunction();
        if (this.#position < source.length)
            throw this.#unsupported("an unmatched )");
        for (const { number, name } of this.#backreferences) {
            if (name === undefined ? number > this.#groupCount : !this.#groupNames.has(name))
                throw this.#unsupported("a backreference without a group");
        }
    }

    exec(subject, lastIndex = 0) {
        if (this.#hasEitherUnicodeFlag) {
            for (let index = 0; index < subject.length; ++index) {
                const unit = subject.charCodeAt(index);
                if (unit >= 0xD800 && unit <= 0xDFFF)
                    throw this.#unsupported("a surrogate in the subject under flag u or v");
            }
        }
        this.#subject = subject;
        const unsetCaptures = new Array(this.#groupCount + 1).fill(undefined);
        for (let start = lastIndex; start <= subject.length; ++start) {
            const result = this.#match(this.#root, start, unsetCaptures, ReferenceRegExp.#accept);
            if (result !== null) {
                const captures = result.captures.map(capture => capture === undefined ? undefined : subject.slice(capture[0], capture[1]));
                captures[0] = subject.slice(start, result.end);
                return { index: start, captures };
            }
            if (this.#sticky)
                break;
        }
        return null;
    }

    static createResult(match) {
        return match === null ? null : { index: match.index, captures: Array.from(match) };
    }

    static isSameResult(actual, expected) {
        if (actual === null || expected === null)
            return actual === expected;
        return actual.index === expected.index
            && actual.captures.length === expected.captures.length
            && actual.captures.every((capture, number) => capture === expected.captures[number]);
    }

    static createMismatchError(operation, text, start, actual, expected) {
        return new Error(`${operation} on ${JSON.stringify(text)} from index ${start} returned ${JSON.stringify(actual)} instead of ${JSON.stringify(expected)}`);
    }

    static checkYarr(regexp, subject, start, expected) {
        for (const [text, note] of [[subject, ""], [to16Bit(subject), " (16-bit subject)"]]) {
            regexp.lastIndex = start;
            const result = ReferenceRegExp.createResult(regexp.exec(text));
            if (!ReferenceRegExp.isSameResult(result, expected))
                throw ReferenceRegExp.createMismatchError(`${regexp}.exec${note}`, text, start, result, expected);
            regexp.lastIndex = start;
            const isMatch = regexp.test(text);
            if (isMatch !== (expected !== null))
                throw ReferenceRegExp.createMismatchError(`${regexp}.test${note}`, text, start, isMatch, expected !== null);
        }
    }

    static #createSet(ranges, builtin, isInverted) {
        return { kind: "set", ranges, builtin, isInverted };
    }

    static #createCharacterSet(unit) {
        return ReferenceRegExp.#createSet([[unit, unit]], null, false);
    }

    static #isDecimalDigit(character) {
        return character >= "0" && character <= "9";
    }

    static #isInRanges(ranges, unit) {
        return ranges.some(range => range[0] <= unit && unit <= range[1]);
    }

    static #isLineTerminator(unit) {
        return ReferenceRegExp.#isInRanges(ReferenceRegExp.#lineTerminators, unit);
    }

    // Approximates simple case folding. U+0131 uppercases to I but has no folding. Wrong for the status S entries of CaseFolding.txt other than U+1E9E: they are not folded.
    static #foldCase(unit, usesSimpleCaseFolding) {
        const upper = String.fromCharCode(unit).toUpperCase();
        if (upper.length !== 1)
            return unit;
        if (usesSimpleCaseFolding) {
            if (unit === ReferenceRegExp.#dotlessI)
                return unit;
            const lower = upper.toLowerCase();
            return lower.length === 1 ? lower.charCodeAt(0) : unit;
        }
        const upperUnit = upper.charCodeAt(0);
        return unit >= 128 && upperUnit < 128 ? unit : upperUnit;
    }

    static #ensureCaseVariantTable(usesSimpleCaseFolding) {
        let table = ReferenceRegExp.#caseVariantTables.get(usesSimpleCaseFolding);
        if (table !== undefined)
            return table;
        table = new Map();
        for (let unit = 0; unit < ReferenceRegExp.#codeUnitCount; ++unit) {
            const canonical = ReferenceRegExp.#foldCase(unit, usesSimpleCaseFolding);
            const variants = table.get(canonical);
            if (variants === undefined)
                table.set(canonical, [unit]);
            else
                variants.push(unit);
        }
        ReferenceRegExp.#caseVariantTables.set(usesSimpleCaseFolding, table);
        return table;
    }

    #unsupported(construct) {
        return new ReferenceRegExp.Unsupported(`the reference matcher does not support ${construct}: /${this.#source}/`);
    }

    #peek() {
        return this.#source[this.#position];
    }

    #next() {
        if (this.#position === this.#source.length)
            throw this.#unsupported("a pattern that ends early");
        return this.#source[this.#position++];
    }

    #consume(text) {
        if (!this.#source.startsWith(text, this.#position))
            return false;
        this.#position += text.length;
        return true;
    }

    #expect(text) {
        if (!this.#consume(text))
            throw this.#unsupported(`a missing ${text}`);
    }

    #parseDisjunction() {
        const alternatives = [this.#parseAlternative()];
        while (this.#consume("|"))
            alternatives.push(this.#parseAlternative());
        return alternatives.length === 1 ? alternatives[0] : { kind: "alternation", alternatives };
    }

    #parseAlternative() {
        const terms = [];
        while (this.#position < this.#source.length && this.#peek() !== "|" && this.#peek() !== ")")
            terms.push(this.#parseTerm());
        return { kind: "sequence", terms };
    }

    #parseTerm() {
        const firstGroup = this.#groupCount;
        const atom = this.#parseAtom();
        if (atom.kind === "lineStart" || atom.kind === "lineEnd" || atom.kind === "boundary")
            return atom;
        const bounds = this.#parseQuantifierBounds();
        if (bounds === null)
            return atom;
        const [min, max] = bounds;
        const isGreedy = !this.#consume("?");
        return { kind: "repeat", body: atom, min, max, isGreedy, firstGroup, groupCount: this.#groupCount - firstGroup };
    }

    #parseQuantifierBounds() {
        if (this.#consume("*"))
            return [0, Infinity];
        if (this.#consume("+"))
            return [1, Infinity];
        if (this.#consume("?"))
            return [0, 1];
        if (!this.#consume("{"))
            return null;
        const min = this.#parseDecimal();
        let max = min;
        if (this.#consume(","))
            max = this.#peek() === "}" ? Infinity : this.#parseDecimal();
        this.#expect("}");
        if (min > max)
            throw this.#unsupported("quantifier bounds out of order");
        return [min, max];
    }

    #parseDecimal() {
        const start = this.#position;
        while (ReferenceRegExp.#isDecimalDigit(this.#peek()))
            ++this.#position;
        if (this.#position === start)
            throw this.#unsupported("a missing decimal digit");
        return Number(this.#source.slice(start, this.#position));
    }

    #parseHexadecimal(digitCount) {
        const digits = this.#source.slice(this.#position, this.#position + digitCount);
        if (digits.length !== digitCount || !Array.from(digits).every(digit => "0123456789abcdefABCDEF".includes(digit)))
            throw this.#unsupported("a missing hexadecimal digit");
        this.#position += digitCount;
        return Number.parseInt(digits, 16);
    }

    #parseAtom() {
        const character = this.#next();
        switch (character) {
        case "^":
            return { kind: "lineStart" };
        case "$":
            return { kind: "lineEnd" };
        case ".":
            return ReferenceRegExp.#createSet([], "any", false);
        case "(":
            return this.#parseGroup();
        case "[":
            return this.#parseClass();
        case "\\":
            return this.#parseAtomEscape();
        case "*":
        case "+":
        case "?":
        case "{":
            throw this.#unsupported(`${character} with nothing to repeat`);
        }
        return ReferenceRegExp.#createCharacterSet(character.charCodeAt(0));
    }

    #parseGroup() {
        const node = this.#parseGroupContents();
        this.#expect(")");
        return node;
    }

    #parseGroupContents() {
        if (this.#consume("?:"))
            return this.#parseDisjunction();
        if (this.#consume("?="))
            return { kind: "lookahead", body: this.#parseDisjunction(), isNegative: false };
        if (this.#consume("?!"))
            return { kind: "lookahead", body: this.#parseDisjunction(), isNegative: true };
        if (this.#consume("?<=") || this.#consume("?<!"))
            throw this.#unsupported("lookbehind");
        const number = ++this.#groupCount;
        if (this.#consume("?<")) {
            const name = this.#parseGroupName();
            if (this.#groupNames.has(name))
                throw this.#unsupported("a duplicate group name");
            this.#groupNames.set(name, number);
        } else if (this.#peek() === "?")
            throw this.#unsupported("modifiers");
        return { kind: "group", body: this.#parseDisjunction(), number };
    }

    #parseGroupName() {
        const end = this.#source.indexOf(">", this.#position);
        if (end === -1)
            throw this.#unsupported("a missing >");
        const name = this.#source.slice(this.#position, end);
        this.#position = end + 1;
        return name;
    }

    #parseClass() {
        if (this.#unicodeSets)
            throw this.#unsupported("a class under flag v");
        const isInverted = this.#consume("^");
        const parts = [];
        while (!this.#consume("]")) {
            const low = this.#parseClassAtom();
            if (this.#peek() !== "-" || this.#source[this.#position + 1] === "]") {
                parts.push(low);
                continue;
            }
            this.#expect("-");
            const high = this.#parseClassAtom();
            if (low.builtin !== null || high.builtin !== null) {
                parts.push(low, ReferenceRegExp.#createCharacterSet(0x002D), high);
                continue;
            }
            const range = [low.ranges[0][0], high.ranges[0][0]];
            if (range[0] > range[1])
                throw this.#unsupported("a class range out of order");
            parts.push(ReferenceRegExp.#createSet([range], null, false));
        }
        return { kind: "class", parts, isInverted };
    }

    #parseClassAtom() {
        const character = this.#next();
        if (character !== "\\")
            return ReferenceRegExp.#createCharacterSet(character.charCodeAt(0));
        const escaped = this.#next();
        if (escaped === "b")
            return ReferenceRegExp.#createCharacterSet(0x0008);
        if (escaped >= "1" && escaped <= "9")
            throw this.#unsupported("a legacy octal escape");
        return this.#parseCharacterEscape(escaped);
    }

    #parseAtomEscape() {
        if (this.#peek() >= "1" && this.#peek() <= "9")
            return this.#addBackreference({ kind: "backreference", number: this.#parseDecimal() });
        const escaped = this.#next();
        if (escaped === "b" || escaped === "B")
            return { kind: "boundary", isNegative: escaped === "B" };
        if (escaped !== "k")
            return this.#parseCharacterEscape(escaped);
        this.#expect("<");
        return this.#addBackreference({ kind: "backreference", name: this.#parseGroupName() });
    }

    #addBackreference(node) {
        this.#backreferences.push(node);
        return node;
    }

    #parseCharacterEscape(escaped) {
        const classEscape = ReferenceRegExp.#characterClassEscapes.get(escaped);
        if (classEscape !== undefined)
            return ReferenceRegExp.#createSet([], classEscape[0], classEscape[1]);
        switch (escaped) {
        case "0":
            if (ReferenceRegExp.#isDecimalDigit(this.#peek()))
                throw this.#unsupported("a legacy octal escape");
            return ReferenceRegExp.#createCharacterSet(0x0000);
        case "x":
            return ReferenceRegExp.#createCharacterSet(this.#parseHexadecimal(2));
        case "u":
            return ReferenceRegExp.#createCharacterSet(this.#parseHexadecimal(4));
        case "c":
        case "p":
        case "P":
            throw this.#unsupported(`\\${escaped}`);
        }
        return ReferenceRegExp.#createCharacterSet(ReferenceRegExp.#controlEscapes.get(escaped) ?? escaped.charCodeAt(0));
    }

    #match(node, end, captures, continuation) {
        switch (node.kind) {
        case "sequence":
            return this.#matchTerms(node.terms, 0, end, captures, continuation);
        case "alternation":
            for (const alternative of node.alternatives) {
                const result = this.#match(alternative, end, captures, continuation);
                if (result !== null)
                    return result;
            }
            return null;
        case "set":
        case "class":
            if (end === this.#subject.length || !this.#isMember(node, this.#subject.charCodeAt(end)))
                return null;
            return continuation(end + 1, captures);
        case "group":
            return this.#match(node.body, end, captures, (bodyEnd, bodyCaptures) => {
                const updatedCaptures = bodyCaptures.slice();
                updatedCaptures[node.number] = [end, bodyEnd];
                return continuation(bodyEnd, updatedCaptures);
            });
        case "repeat":
            return this.#matchRepeat(node, node.min, node.max, end, captures, continuation);
        case "lookahead": {
            const result = this.#match(node.body, end, captures, ReferenceRegExp.#accept);
            if (node.isNegative)
                return result === null ? continuation(end, captures) : null;
            return result === null ? null : continuation(end, result.captures);
        }
        case "backreference":
            return this.#matchBackreference(node, end, captures, continuation);
        case "boundary":
        case "lineStart":
        case "lineEnd":
            return this.#isSatisfied(node, end) ? continuation(end, captures) : null;
        }
        throw new Error(`unknown node kind ${node.kind}`);
    }

    #matchTerms(terms, index, end, captures, continuation) {
        if (index === terms.length)
            return continuation(end, captures);
        return this.#match(terms[index], end, captures, (termEnd, termCaptures) => this.#matchTerms(terms, index + 1, termEnd, termCaptures, continuation));
    }

    #matchRepeat(node, min, max, end, captures, continuation) {
        if (max === 0)
            return continuation(end, captures);
        const again = (bodyEnd, bodyCaptures) => {
            if (min === 0 && bodyEnd === end)
                return null;
            return this.#matchRepeat(node, min === 0 ? 0 : min - 1, max - 1, bodyEnd, bodyCaptures, continuation);
        };
        const clearedCaptures = captures.slice().fill(undefined, node.firstGroup + 1, node.firstGroup + node.groupCount + 1);
        if (min > 0)
            return this.#match(node.body, end, clearedCaptures, again);
        if (!node.isGreedy)
            return continuation(end, captures) ?? this.#match(node.body, end, clearedCaptures, again);
        return this.#match(node.body, end, clearedCaptures, again) ?? continuation(end, captures);
    }

    #matchBackreference(node, end, captures, continuation) {
        const capture = captures[node.name === undefined ? node.number : this.#groupNames.get(node.name)];
        if (capture === undefined)
            return continuation(end, captures);
        const [captureStart, captureEnd] = capture;
        const length = captureEnd - captureStart;
        if (end + length > this.#subject.length)
            return null;
        for (let offset = 0; offset < length; ++offset) {
            if (this.#canonicalize(this.#subject.charCodeAt(captureStart + offset)) !== this.#canonicalize(this.#subject.charCodeAt(end + offset)))
                return null;
        }
        return continuation(end + length, captures);
    }

    #isSatisfied(assertion, end) {
        const subject = this.#subject;
        switch (assertion.kind) {
        case "lineStart":
            return end === 0 || (this.#multiline && ReferenceRegExp.#isLineTerminator(subject.charCodeAt(end - 1)));
        case "lineEnd":
            return end === subject.length || (this.#multiline && ReferenceRegExp.#isLineTerminator(subject.charCodeAt(end)));
        case "boundary": {
            const isAfterWord = end > 0 && this.#isWordCharacter(subject.charCodeAt(end - 1));
            const isBeforeWord = end < subject.length && this.#isWordCharacter(subject.charCodeAt(end));
            return (isAfterWord !== isBeforeWord) !== assertion.isNegative;
        }
        }
        throw new Error(`unknown assertion kind ${assertion.kind}`);
    }

    #canonicalize(unit) {
        return this.#ignoreCase ? ReferenceRegExp.#foldCase(unit, this.#hasEitherUnicodeFlag) : unit;
    }

    #caseVariants(unit) {
        if (!this.#ignoreCase)
            return [unit];
        return ReferenceRegExp.#ensureCaseVariantTable(this.#hasEitherUnicodeFlag).get(this.#canonicalize(unit));
    }

    #isWordCharacter(unit) {
        return ReferenceRegExp.#isInRanges(ReferenceRegExp.#basicWordCharacters, unit)
            || (this.#ignoreCase && this.#hasEitherUnicodeFlag && ReferenceRegExp.#isInRanges(ReferenceRegExp.#extraWordCharacters, unit));
    }

    #isInBuiltin(builtin, unit) {
        switch (builtin) {
        case null:
            return false;
        case "digit":
            return ReferenceRegExp.#isInRanges(ReferenceRegExp.#decimalDigits, unit);
        case "word":
            return this.#isWordCharacter(unit);
        case "space":
            return ReferenceRegExp.#isInRanges(ReferenceRegExp.#whiteSpace, unit);
        case "any":
            return this.#dotAll || !ReferenceRegExp.#isLineTerminator(unit);
        }
        throw new Error(`unknown builtin set ${builtin}`);
    }

    #has(set, unit) {
        return (ReferenceRegExp.#isInRanges(set.ranges, unit) || this.#isInBuiltin(set.builtin, unit)) !== set.isInverted;
    }

    #isMember(node, unit) {
        const variants = this.#caseVariants(unit);
        if (node.kind === "set")
            return variants.some(variant => this.#has(node, variant));
        return variants.some(variant => node.parts.some(part => this.#has(part, variant))) !== node.isInverted;
    }
}

function to16Bit(subject) {
    return $vm.make16BitStringIfPossible(subject);
}

function stringsOver(alphabet, maxLength) {
    const strings = [""];
    let previousLengthStart = 0;
    for (let length = 1; length <= maxLength; ++length) {
        const previousLengthEnd = strings.length;
        for (let index = previousLengthStart; index < previousLengthEnd; ++index) {
            for (const character of alphabet)
                strings.push(strings[index] + character);
        }
        previousLengthStart = previousLengthEnd;
    }
    return strings;
}

function checkRegExp(source, flags, subjects, options = { }) {
    const extraFlags = options.allStartIndexes ? ["", "g", "y"] : [""];
    for (const extra of extraFlags) {
        const reference = new ReferenceRegExp(source, flags + extra);
        const regexp = new RegExp(source, flags + extra);
        for (const subject of subjects) {
            const lastStart = extra === "" ? 0 : subject.length;
            for (let start = 0; start <= lastStart; ++start)
                ReferenceRegExp.checkYarr(regexp, subject, start, reference.exec(subject, start));
        }
    }
}

function checkCases(cases, options = { }) {
    for (const [source, flags, subject, index, captures] of cases) {
        if (flags.includes("g") || flags.includes("y"))
            throw new Error(`checkCases does not accept flag g or y: /${source}/${flags}`);
        const regexp = new RegExp(source, flags);
        const expected = index === null ? null : { index, captures: captures.map(capture => capture ?? undefined) };
        if (!options.skipReference) {
            const result = new ReferenceRegExp(source, flags).exec(subject);
            if (!ReferenceRegExp.isSameResult(result, expected))
                throw ReferenceRegExp.createMismatchError(`the reference matcher disagrees with the expected result: ${regexp}`, subject, 0, result, expected);
        }
        ReferenceRegExp.checkYarr(regexp, subject, 0, expected);
    }
}
