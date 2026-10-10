#!/usr/bin/env bun
import { mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

const usage = `type-coverage.ts <coverage file> [options]

Reports how much of a program the ahead-of-time compiler specialized, like a code coverage report: an operation is covered
if it compiled to inline code. The coverage file is written by the compiler with --aotTypeCoveragePath=<file>.

Every operation is one of:
  inline    no call on the usual path: a typed field, a direct or inlined call, arithmetic on known numbers, ...
  stub      calls a shared stub that has an inline cache or a fast path
  runtime   calls into the C++ runtime

An operation that was compiled several times (inlined, or in both copies of a split loop) counts once, as its worst copy;
the generic copy of a split loop counts only where there is no other. A function compiled both for calls and for \`new\` counts
once, each operation as the better of the two, since one of them usually never runs.

Every opcode counts. Those that no kind lists (moves, jumps, returns, checks) are "other"; they have nothing to specialize, so
the row "all but other" and --fail-under leave them out.

The last table checks the report against the bytecode: how many instructions of each kind the compiled functions have, and how
many of them the compiler reported. It reports all of them: an instruction that is not in its graph (a move, or code that is
unreachable or folded) has the outcome not-in-graph.

Options:
  --source-map <file>      map positions back to the original sources; may be repeated, one per module
  --reasons <file>         reasons.txt of the type table: names the reasons why an access has no type
  --lcov <file>            write an lcov tracefile (a line is hit if everything on it is inline; each operation is a branch)
  --html <directory>       write a report to browse: index.html, and a page per source file that marks every operation
  --by <what>              also break down by: reason, property, function, opcode, outcome (may be repeated)
  --kind <kinds>           restrict breakdowns to these kinds, comma separated (reads, writes, elements, calls, arithmetic,
                           comparisons, conditions, iteration, allocation, variables, exceptions, other)
  --explain <file>[:line]  list the operations of a file or a line, with what each compiled to and why
  --top <n>                rows per table (default 25)
  --fail-under <percent>   exit with status 1 if less than this is inline
`;

const opcodesOfKind = {
    reads: "get_by_id get_by_id_direct get_by_id_with_this try_get_by_id in_by_id get_length get_private_name",
    writes: `put_by_id put_by_id_with_this put_private_name define_data_property del_by_id put_getter_by_id put_setter_by_id
        put_getter_setter_by_id put_getter_by_val put_setter_by_val define_accessor_property put_internal_field set_function_name`,
    elements: `get_by_val put_by_val put_by_val_direct in_by_val del_by_val get_by_val_with_this get_argument_by_val
        enumerator_put_by_val`,
    calls: `call call_ignore_result construct tail_call call_varargs construct_varargs tail_call_varargs call_direct_eval
        tail_call_forward_arguments super_construct super_construct_varargs`,
    arithmetic: `add sub mul div mod pow negate inc dec bitand bitor bitxor bitnot lshift rshift urshift unsigned to_number
        to_numeric to_string to_primitive to_property_key to_property_key_or_number strcat to_object`,
    comparisons: `eq neq stricteq nstricteq less lesseq greater greatereq below beloweq jeq jneq jstricteq jnstricteq jless
        jlesseq jgreater jgreatereq jnless jnlesseq jngreater jngreatereq jbelow jbeloweq instanceof typeof
        eq_null neq_null jeq_null jneq_null jundefined_or_null jnundefined_or_null is_callable is_object
        typeof_is_object typeof_is_function typeof_is_undefined is_undefined_or_null is_cell_with_type
        is_number is_boolean is_big_int is_empty is_constructor jeq_ptr jneq_ptr`,
    conditions: "jtrue jfalse not to_boolean switch_imm switch_char switch_string",
    iteration: "iterator_open iterator_next get_property_enumerator enumerator_next enumerator_get_by_val spread",
    allocation: `new_object create_this new_array new_array_with_size new_array_with_spread new_array_buffer new_func
        new_func_exp new_generator_func new_generator_func_exp new_async_func new_async_func_exp new_reg_exp
        new_reg_exp_shared new_async_generator_func new_async_generator_func_exp
        create_lexical_environment create_rest create_direct_arguments create_scoped_arguments
        create_cloned_arguments create_generator create_promise create_async_generator new_promise new_generator`,
    variables: `get_from_scope put_to_scope resolve_scope to_this get_argument argument_count get_scope get_parent_scope
        get_internal_field resolve_and_get_from_scope`,
    exceptions: "throw throw_static_error catch",
};

type Kind = keyof typeof opcodesOfKind | "other";
const allKinds = [...Object.keys(opcodesOfKind), "other"] as Kind[];
const listedKindOf = new Map<string, Kind>(
    Object.entries(opcodesOfKind).flatMap(([kind, names]) => names.split(/\s+/).map(name => [`op_${name}`, kind as Kind] as const)),
);
const kindOf = (opcode: string): Kind => listedKindOf.get(opcode) ?? "other";
const propertyAccesses: ReadonlySet<string> = new Set(["reads", "writes"]);

type Level = 0 | 1 | 2;
const [runtime, stub, inline] = [0, 1, 2] as const;
const levelNames = ["runtime", "stub", "inline"] as const;

const Flag = { inGenericCopy: 1, rarelyExecuted: 2, inLoop: 4, inlined: 8, elided: 16, hasTypeTag: 32, notInGraph: 64 } as const;

const outcomesOfKnownCallee = new Set([
    "direct-call",
    "direct-construct",
    "inlined-call",
    "inlined-construct",
    "inlined-closure",
    "inlined-builtin",
    "inlined-call-with-callback",
    "lowered-builtin",
]);

interface Copy {
    line: number;
    column: number;
    opcode: string;
    flags: number;
    reason: string;
    property: string;
    function: string;
    outcomes: string[];
}

interface Instruction {
    module: string;
    functionStart: number;
    functionKind: number;
    offset: number;
    copies: Copy[];
}

interface Coverage {
    modules: Map<string, string>;
    instructions: Instruction[];
    inBytecode: Map<string, { positioned: number; unpositioned: number }>;
    unreached?: { functions: number; bytes: number };
}

interface Position {
    file: string;
    line: number;
    column: number;
}

interface Operation extends Position {
    opcode: string;
    kind: Kind;
    level: Level;
    typed: boolean;
    flags: number;
    reason: string;
    property: string;
    function: string;
    outcomes: string[];
    copies: number;
    offset: number;
    weight: number;
}

interface SourceMap {
    sources: string[];
    contents: (string | null)[];
    segmentsOfLine: number[][];
}

type Counts = [runtime: number, stub: number, inline: number, typed: number];
type Row = [name: string, counts: Counts];
type KeyOf = (operation: Operation) => string | undefined;

const breakdowns = ["reason", "property", "function", "opcode", "outcome"] as const;
type Breakdown = (typeof breakdowns)[number];

interface Options {
    path: string;
    sourceMaps: string[];
    reasons?: string;
    lcov?: string;
    html?: string;
    by: Breakdown[];
    kinds?: Set<string>;
    explain?: string;
    top: number;
    failUnder?: number;
}

function parseOptions(args: string[]): Options {
    const options: Options = { path: "", sourceMaps: [], by: [], top: 25 };
    const rest = [...args];
    const value = (option: string) => rest.shift() ?? fail(`${option} needs a value`);
    for (let argument = rest.shift(); argument !== undefined; argument = rest.shift()) {
        if (argument === "--source-map") options.sourceMaps.push(value(argument));
        else if (argument === "--reasons") options.reasons = value(argument);
        else if (argument === "--lcov") options.lcov = value(argument);
        else if (argument === "--html") options.html = value(argument);
        else if (argument === "--by") options.by.push(breakdownNamed(value(argument)));
        else if (argument === "--kind") options.kinds = new Set(value(argument).split(","));
        else if (argument === "--explain") options.explain = value(argument);
        else if (argument === "--top") options.top = Number.parseInt(value(argument), 10);
        else if (argument === "--fail-under") options.failUnder = Number(value(argument));
        else options.path = argument;
    }
    return options.path ? options : fail("no coverage file given");
}

function breakdownNamed(name: string): Breakdown {
    return breakdowns.find(breakdown => breakdown === name) ?? fail(`--by ${name}: expected one of ${breakdowns.join(", ")}`);
}

function fail(message: string): never {
    console.error(`type-coverage: ${message}`);
    process.exit(2);
}

function linesOf(path: string): string[] {
    const text = readFileSync(path, "utf8");
    return text.includes("\r") ? text.split(/\r\n?|\n/) : text.split("\n");
}

function readReasons(path: string | undefined): Map<string, string> {
    const fields = path ? linesOf(path).filter(Boolean).map(line => line.split("\t")) : [];
    return new Map(fields.map(([number, ...text]) => [number, text.join("\t")]));
}

function readCoverage(path: string): Coverage {
    const coverage: Coverage = { modules: new Map(), instructions: [], inBytecode: new Map() };
    const instructionOf = new Map<string, Instruction>();
    for (const line of linesOf(path)) {
        const fields = line.split("\t");
        if (fields[0] === "O") copiesOf(fields, instructionOf).push(copyOf(fields));
        else if (fields[0] === "M") coverage.modules.set(fields[1], fields[2]);
        else if (fields[0] === "T") coverage.inBytecode.set(fields[1], { positioned: +fields[2], unpositioned: +fields[3] });
        else if (fields[0] === "U") coverage.unreached = { functions: +fields[1], bytes: +fields[2] };
    }
    coverage.instructions = [...instructionOf.values()].sort(compareInstructions);
    return coverage;
}

function copiesOf(fields: string[], instructionOf: Map<string, Instruction>): Copy[] {
    const key = `${fields[1]}\t${fields[10]}\t${fields[11]}\t${fields[12]}`;
    let instruction = instructionOf.get(key);
    if (!instruction) {
        instruction = { module: fields[1], functionStart: +fields[10], functionKind: +fields[11], offset: +fields[12], copies: [] };
        instructionOf.set(key, instruction);
    }
    return instruction.copies;
}

function copyOf(fields: string[]): Copy {
    return {
        line: +fields[2],
        column: +fields[3],
        opcode: fields[4],
        flags: +fields[5],
        reason: fields[6],
        property: fields[7],
        function: fields[8],
        outcomes: fields[9] ? fields[9].split(" ") : [],
    };
}

const compareText = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);

function compareLists(a: string[], b: string[]): number {
    const different = a.findIndex((item, index) => item !== b[index]);
    return different < 0 ? a.length - b.length : different < b.length ? compareText(a[different], b[different]) : 1;
}

function compareInstructions(a: Instruction, b: Instruction): number {
    return (
        compareText(a.module, b.module) ||
        a.functionStart - b.functionStart ||
        a.functionKind - b.functionKind ||
        a.offset - b.offset
    );
}

function compareOperations(a: Operation, b: Operation): number {
    return (
        compareText(a.file, b.file) ||
        a.line - b.line ||
        a.column - b.column ||
        compareText(a.opcode, b.opcode) ||
        compareText(a.kind, b.kind) ||
        a.level - b.level ||
        Number(a.typed) - Number(b.typed) ||
        a.flags - b.flags ||
        compareText(a.reason, b.reason) ||
        compareText(a.property, b.property) ||
        compareText(a.function, b.function) ||
        compareLists(a.outcomes, b.outcomes) ||
        a.copies - b.copies
    );
}

function levelOf(kind: Kind, flags: number, outcomes: string[]): Level {
    if (flags & (Flag.elided | Flag.notInGraph)) return inline;
    if (kind === "calls" && outcomes.some(outcome => outcomesOfKnownCallee.has(outcome.split(":", 1)[0]))) return inline;
    const called = outcomes.filter(outcome => outcome.startsWith("calls:"));
    if (called.some(outcome => /^calls:(operation|Operation|PlainOperation)/.test(outcome))) return runtime;
    return called.length || kind === "calls" ? stub : inline;
}

const isTyped = (outcomes: string[]) => outcomes.some(outcome => outcome.startsWith("typed-"));
const rankOf = (kind: Kind, copy: Copy) => levelOf(kind, copy.flags, copy.outcomes) * 2 + Number(isTyped(copy.outcomes));

const base64Digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const valueOfBase64Digit = new Uint8Array(128);
for (let value = 0; value < base64Digits.length; value++) valueOfBase64Digit[base64Digits.charCodeAt(value)] = value;

function readSourceMap(path: string): SourceMap {
    const data = JSON.parse(readFileSync(path, "utf8"));
    const root = dirname(resolve(path));
    return {
        sources: data.sources.map((source: string) => resolve(root, data.sourceRoot || "", source)),
        contents: data.sourcesContent ?? [],
        segmentsOfLine: decodeMappings(data.mappings),
    };
}

function decodeMappings(mappings: string): number[][] {
    const semicolon = 59;
    const comma = 44;
    const segmentsOfLine: number[][] = [];
    const fields = [0, 0, 0, 0, 0];
    let segments: number[] = [];
    let generatedColumn = 0;
    let source = 0;
    let line = 0;
    let column = 0;
    let fieldCount = 0;
    let value = 0;
    let shift = 0;
    for (let index = 0; index <= mappings.length; index++) {
        const code = index < mappings.length ? mappings.charCodeAt(index) : semicolon;
        if (code !== semicolon && code !== comma) {
            const digit = valueOfBase64Digit[code];
            value |= (digit & 31) << shift;
            shift += 5;
            if (digit & 32) continue;
            fields[fieldCount++] = value & 1 ? -(value >> 1) : value >> 1;
            value = shift = 0;
            continue;
        }
        if (fieldCount) generatedColumn += fields[0];
        if (fieldCount >= 4) segments.push(generatedColumn, (source += fields[1]), (line += fields[2]), (column += fields[3]));
        fieldCount = 0;
        if (code === semicolon) {
            segmentsOfLine.push(segments);
            segments = [];
            generatedColumn = 0;
        }
    }
    return segmentsOfLine;
}

function originalPosition(map: SourceMap, line: number, column: number): Position | undefined {
    const segments = line > map.segmentsOfLine.length ? undefined : map.segmentsOfLine.at(line - 1);
    if (!segments) return undefined;
    let low = 0;
    let high = segments.length / 4;
    while (low < high) {
        const middle = (low + high) >> 1;
        if (segments[middle * 4] < column) low = middle + 1;
        else high = middle;
    }
    if (!low) return undefined;
    const [, source, originalLine, originalColumn] = segments.slice(low * 4 - 4, low * 4);
    return { file: map.sources[source], line: originalLine + 1, column: originalColumn + 1 };
}

const baseName = (path: string) => path.slice(path.lastIndexOf("/") + 1);

function sourceMapsOfModules(modules: Map<string, string>, paths: string[]): Map<string, SourceMap> {
    const pathNamed = new Map(paths.map(path => [baseName(path).slice(0, -4), path]));
    const only = paths.length === 1 && modules.size === 1 ? paths[0] : undefined;
    const loaded = new Map<string, SourceMap>();
    const mapOfModule = new Map<string, SourceMap>();
    for (const [module, url] of modules) {
        const path = pathNamed.get(baseName(url)) || only;
        if (!path) continue;
        if (!loaded.has(path)) loaded.set(path, readSourceMap(path));
        mapOfModule.set(module, loaded.get(path)!);
    }
    return mapOfModule;
}

const first = <T>(items: T[], isBetter: (item: T, than: T) => boolean) =>
    items.reduce((chosen, item) => (isBetter(item, chosen) ? item : chosen));

interface Variant {
    module: string;
    opcode: string;
    offset: number;
    copy: Copy;
    copies: number;
}

function worstCopyOf({ module, offset, copies }: Instruction): Variant {
    const opcode = copies[0].opcode;
    const kind = kindOf(opcode);
    const specialized = copies.filter(copy => !(copy.flags & Flag.inGenericCopy));
    const counted = specialized.length ? specialized : copies;
    return { module, opcode, offset, copy: first(counted, (a, b) => rankOf(kind, a) < rankOf(kind, b)), copies: counted.length };
}

function variantsOf(instructions: Instruction[]): Iterable<Variant[]> {
    const ordinals = new Map<string, number>();
    const variants = new Map<string, Variant[]>();
    for (const instruction of instructions) {
        const variant = worstCopyOf(instruction);
        const { line, column } = variant.copy;
        const place = `${line}\t${column}\t${variant.opcode}\t${instruction.functionStart}\t${instruction.module}`;
        const placeInCodeBlock = `${instruction.functionKind}\t${place}`;
        const ordinal = (ordinals.get(placeInCodeBlock) ?? 0) + 1;
        ordinals.set(placeInCodeBlock, ordinal);
        const key = `${ordinal}\t${instruction.functionKind >> 1}\t${place}`;
        const found = variants.get(key);
        if (found) found.push(variant);
        else variants.set(key, [variant]);
    }
    return variants.values();
}

function operationsOf(coverage: Coverage, mapOfModule: Map<string, SourceMap>): Operation[] {
    const operations: Operation[] = [];
    for (const variants of variantsOf(coverage.instructions)) {
        const { module, opcode } = variants[0];
        const kind = kindOf(opcode);
        const { copy, copies, offset } = first(variants, (a, b) => rankOf(kind, a.copy) > rankOf(kind, b.copy));
        const map = mapOfModule.get(module);
        const { line, column } = variants[0].copy;
        const position = map
            ? (originalPosition(map, line, column) ?? { file: "(no original position)", line: 0, column: 0 })
            : { file: coverage.modules.get(module) ?? module, line, column };
        operations.push({
            ...position,
            opcode,
            kind,
            level: levelOf(kind, copy.flags, copy.outcomes),
            typed: isTyped(copy.outcomes),
            flags: copy.flags,
            reason: copy.reason,
            property: copy.property,
            function: copy.function,
            outcomes: copy.outcomes,
            copies,
            offset,
            weight: 1,
        });
    }
    return operations;
}

function tally(operations: Operation[], keyOf: KeyOf, chosen?: ReadonlySet<string>): Map<string, Counts> {
    const countsOf = new Map<string, Counts>();
    for (const operation of operations) {
        if (chosen && !chosen.has(operation.kind)) continue;
        const key = keyOf(operation);
        if (key === undefined) continue;
        let counts = countsOf.get(key);
        if (!counts) countsOf.set(key, (counts = [0, 0, 0, 0]));
        counts[operation.level] += operation.weight;
        if (operation.typed) counts[3] += operation.weight;
    }
    return countsOf;
}

const sumOf = (rows: Row[]): Counts => rows.reduce<Counts>((sum, [, counts]) => sum.map((n, i) => n + counts[i]) as Counts, [0, 0, 0, 0]);
const weightOf = (operations: Operation[]) => operations.reduce((sum, operation) => sum + operation.weight, 0);
const operationCount = (counts: Counts) => counts[runtime] + counts[stub] + counts[inline];
const notInline = (counts: Counts) => counts[runtime] + counts[stub];
const worstFirst = (countsOf: Map<string, Counts>): Row[] => [...countsOf].sort((a, b) => notInline(b[1]) - notInline(a[1]));

function rowsByKind(operations: Operation[]): { rows: Row[]; allButOther: Counts; all: Counts } {
    const countsOf = tally(operations, operation => operation.kind);
    const rows = allKinds.filter(kind => countsOf.has(kind)).map((kind): Row => [kind, countsOf.get(kind)!]);
    return { rows, allButOther: sumOf(rows.filter(([kind]) => kind !== "other")), all: sumOf(rows) };
}

const unnumberedReasons = new Map([
    ["typed", "(typed)"],
    ["unused", "(has a type that was not used)"],
    ["none", "(no type given)"],
]);

function reasonKeyOf(operation: Operation): string {
    if (operation.typed) return "typed";
    return operation.reason || (operation.flags & Flag.hasTypeTag ? "unused" : "none");
}

function reasonNamer(reasons: Map<string, string>): (key: string) => string {
    return key => unnumberedReasons.get(key) ?? reasons.get(key) ?? `reason ${key}`;
}

function isFile(path: string): boolean {
    return statSync(path, { throwIfNoEntry: false })?.isFile() ?? false;
}

function sharedPrefix(a: string[], b: string[]): string[] {
    const different = a.findIndex((part, index) => part !== b[index]);
    return different < 0 ? a : a.slice(0, different);
}

function fileNameShortener(operations: Operation[]): (file: string) => string {
    const files = [...new Set(operations.map(operation => operation.file))];
    const paths = files.filter(file => file.startsWith("/")).map(file => [...file.split("/").filter(part => part && part !== "."), "x"]);
    const shared = dirname(`/${(paths.length ? paths : [["x"]]).reduce(sharedPrefix).join("/")}`);
    const root = isFile(shared) || files.length === 1 ? dirname(shared) : shared;
    return file => (file.startsWith("/") && root !== "/" ? relative(root, file) || "." : file);
}

const lengthOf = (text: string) => [...text].length;
const lastCharacters = (text: string, count: number) => [...text].slice(-count).join("");
const padEnd = (text: string, width: number) => text + " ".repeat(Math.max(0, width - lengthOf(text)));
const grouped = (n: number) => n.toLocaleString("en-US");

// printf rounds a value that is exactly halfway to the even digit; toFixed rounds it up.
const oneDecimal = (value: number) => ((value * 4) % 4 === 1 ? value - 0.05 : value).toFixed(1);
const percent = (part: number, whole: number) => (whole ? `${oneDecimal((100 * part) / whole).padStart(5)}%` : "     -");

function table(title: string, rows: Row[], top: number): string[] {
    if (!rows.length) return [];
    const shown = rows.slice(0, top);
    const width = Math.min(90, Math.max(lengthOf(title), ...shown.map(([name]) => lengthOf(name))));
    return [
        "",
        `${padEnd(title, width)}  operations   inline     stub  runtime    typed`,
        ...shown.map(([name, counts]) => {
            const n = operationCount(counts);
            const shares = [counts[inline], counts[stub], counts[runtime], counts[3]].map(part => percent(part, n));
            return `${padEnd(lastCharacters(name, width), width)}  ${grouped(n).padStart(10)}  ${shares.join("  ")}`;
        }),
        ...(rows.length > top ? [`... and ${rows.length - top} more`] : []),
    ];
}

const inParentheses = (kinds?: ReadonlySet<string>) => (kinds ? ` (${[...kinds].sort().join(", ")})` : "");

function explain(operations: Operation[], wanted: string, reasonOf: (operation: Operation) => string): string[] {
    const colon = wanted.indexOf(":");
    const suffix = colon < 0 ? wanted : wanted.slice(0, colon);
    const line = colon < 0 ? "" : wanted.slice(colon + 1);
    return operations
        .filter(operation => operation.file.endsWith(suffix) && (!line || operation.line === Number.parseInt(line, 10)))
        .sort(compareOperations)
        .flatMap(operation => {
            const name = baseName(operation.file);
            const what = operation.opcode.slice(3) + (operation.property ? ` .${operation.property}` : "");
            const outcomes = operation.outcomes.join(" ") || (operation.flags & Flag.elided ? "elided" : "-");
            const level = levelNames[operation.level];
            return [
                `${name}:${operation.line}:${operation.column}  ${padEnd(what, 18)} ${padEnd(level, 7)} ${outcomes}`,
                ...(propertyAccesses.has(operation.kind) && !operation.typed
                    ? [`${" ".repeat(lengthOf(name) + 8)} not typed: ${reasonOf(operation)}`]
                    : []),
            ];
        });
}

function outcomeTable(operations: Operation[], kinds: Set<string> | undefined, top: number): string[] {
    const seen = new Map<string, number>();
    for (const operation of operations) {
        if (kinds && !kinds.has(operation.kind)) continue;
        for (const outcome of operation.outcomes.length ? operation.outcomes : ["(nothing)"])
            seen.set(outcome, (seen.get(outcome) ?? 0) + operation.weight);
    }
    const mostCommon = [...seen].sort((a, b) => b[1] - a[1]).slice(0, top);
    return ["", "Outcome", ...mostCommon.map(([name, n]) => `${grouped(n).padStart(10)}  ${name}`)];
}

function breakdownTable(what: Breakdown, operations: Operation[], options: Options, reasonOf: KeyOf): string[] {
    if (what === "outcome") return outcomeTable(operations, options.kinds, options.top);
    const [keyOf, chosen]: [KeyOf, ReadonlySet<string> | undefined] = {
        reason: [reasonOf, options.kinds ?? propertyAccesses],
        property: [(operation: Operation) => operation.property || undefined, options.kinds ?? propertyAccesses],
        function: [(operation: Operation) => operation.function || "(top level)", options.kinds],
        opcode: [(operation: Operation) => operation.opcode.slice(3), options.kinds],
    }[what] as [KeyOf, ReadonlySet<string> | undefined];
    const title = what[0].toUpperCase() + what.slice(1) + inParentheses(chosen);
    return table(title, worstFirst(tally(operations, keyOf, chosen)), options.top);
}

function groupBy<T, K>(items: Iterable<T>, keyOf: (item: T) => K): Map<K, T[]> {
    const groups = new Map<K, T[]>();
    for (const item of items) {
        const group = groups.get(keyOf(item));
        if (group) group.push(item);
        else groups.set(keyOf(item), [item]);
    }
    return groups;
}

const isInline = (operation: Operation) => operation.level === inline;

function lcovRecord(file: string, operations: Operation[]): string {
    const lines = [...groupBy(operations, operation => operation.line)].sort((a, b) => a[0] - b[0]);
    const branches = lines.flatMap(([line, onLine]) =>
        onLine.sort(compareOperations).map((operation, index) => `BRDA:${line},0,${index},${Number(isInline(operation))}`),
    );
    return [
        "TN:",
        `SF:${file}`,
        ...branches,
        `BRF:${operations.length}`,
        `BRH:${operations.filter(isInline).length}`,
        ...lines.map(([line, onLine]) => `DA:${line},${Number(onLine.every(isInline))}`),
        `LF:${lines.length}`,
        `LH:${lines.filter(([, onLine]) => onLine.every(isInline)).length}`,
        "end_of_record\n",
    ].join("\n");
}

function lcovOf(operations: Operation[]): string {
    const positioned = groupBy(
        operations.filter(operation => operation.line),
        operation => operation.file,
    );
    return [...positioned]
        .sort((a, b) => compareText(a[0], b[0]))
        .map(([file, inFile]) => lcovRecord(file, inFile))
        .join("");
}

interface Completeness {
    kind: string;
    inBytecode: number;
    reported: number;
    inClassFieldCode: number;
}

function completenessOf({ instructions, inBytecode }: Coverage): Completeness[] {
    const reported = new Map<string, number>();
    for (const { copies } of instructions) reported.set(copies[0].opcode, (reported.get(copies[0].opcode) ?? 0) + 1);
    const rows = [...allKinds, "all"].map((kind): Completeness => ({ kind, inBytecode: 0, reported: 0, inClassFieldCode: 0 }));
    for (const [opcode, { positioned, unpositioned }] of inBytecode) {
        for (const row of rows.filter(row => row.kind === kindOf(opcode) || row.kind === "all")) {
            row.inBytecode += positioned;
            row.reported += reported.get(opcode) ?? 0;
            row.inClassFieldCode += unpositioned;
        }
    }
    return inBytecode.size ? rows : [];
}

function completenessTable(rows: Completeness[]): string[] {
    const format = (cells: string[]) =>
        [padEnd(cells[0], 18), ...cells.slice(1).map((cell, index) => cell.padStart([12, 12, 10, 22][index]))].join(" ");
    if (!rows.length) return [];
    return [
        "",
        format(["Completeness", "in bytecode", "reported", "", "in class field code"]),
        ...rows.map(({ kind, inBytecode, reported, inClassFieldCode }) =>
            format([kind, grouped(inBytecode), grouped(reported), percent(reported, inBytecode), grouped(inClassFieldCode)]),
        ),
    ];
}

function unreachedLine(unreached: Coverage["unreached"]): string[] {
    if (!unreached) return [];
    return [
        `${grouped(unreached.functions)} functions (${grouped(unreached.bytes)} bytes of bytecode) were not compiled: nothing reaches them`,
    ];
}

interface Report {
    title: string;
    operations: Operation[];
    shortNameOf: (file: string) => string;
    nameOfReason: (key: string) => string;
    sourceOf: (file: string) => string | undefined;
    completeness: Completeness[];
    unreached?: Coverage["unreached"];
    top: number;
}

interface FilePage {
    lines: string[];
    reasons: [key: string, name: string][];
    strings: string[];
    outcomes: string[][];
    details: number[][];
    operations: { line: number[]; column: number[]; detail: number[]; function: number[]; offset: number[]; weight: number[] };
}

const specializable: ReadonlySet<string> = new Set(allKinds.filter(kind => kind !== "other"));
const everyLevel = [inline, stub, runtime] as const;
const letterOfLevel = ["R", "S", "I"] as const;
const meaningOfLevel = [
    "calls into the C++ runtime",
    "calls a shared stub that has an inline cache or a fast path",
    "no call on the usual path",
] as const;
const nameOfFlag: [bit: number, name: string][] = [
    [Flag.inLoop, "in a loop"],
    [Flag.rarelyExecuted, "rarely executed"],
    [Flag.inGenericCopy, "in the generic copy of a split loop"],
    [Flag.inlined, "inlined"],
    [Flag.elided, "elided"],
    [Flag.hasTypeTag, "has a type tag"],
];

function sourceReader(maps: SourceMap[]): (file: string) => string | undefined {
    const contentOf = new Map(maps.flatMap(map => map.sources.map((source, index) => [source, map.contents[index]] as const)));
    return file => contentOf.get(file) ?? (isFile(file) ? readFileSync(file, "utf8") : undefined);
}

const unescaped = (text: string) => text.replace(/\\u([0-9a-f]{4})/g, (_, digits) => String.fromCharCode(Number.parseInt(digits, 16)));
const readable = (text: string) =>
    unescaped(text).replace(/[\0-\x1f]/g, control => String.fromCharCode(0x2400 + control.charCodeAt(0)));
const escapeHtml = (text: string) => text.replace(/[&<>"]/g, character => `&#${character.charCodeAt(0)};`);
const share = (part: number, whole: number) => percent(part, whole).trim();

function pagePathsOf(names: string[]): Map<string, string> {
    const taken = new Set<string>();
    return new Map(
        names.map(name => {
            const parts = name.split("/").map(part => part.replace(/[^\w.-]+/g, "_"));
            const wanted = parts.filter(part => /[^.]/.test(part)).join("/") || "unnamed";
            let path = wanted;
            for (let n = 2; taken.has(path.toLowerCase()); n++) path = `${wanted}-${n}`;
            taken.add(path.toLowerCase());
            return [name, `files/${path}.html`];
        }),
    );
}

function bar(counts: Counts, shown: readonly Level[], whole: number): string {
    const segments = shown
        .filter(level => counts[level])
        .map(level => {
            const title = `${levelNames[level]}: ${grouped(counts[level])}`;
            return `<i class="${levelNames[level]}" style="flex-grow:${counts[level]}" title="${title}"></i>`;
        });
    const width = (100 * shown.reduce<number>((sum, level) => sum + counts[level], 0)) / (whole || 1);
    return `<span class="bar" style="width:${width.toFixed(2)}%">${segments.join("")}</span>`;
}

const numberCell = (value: number, text = grouped(value)) => `<td data-value="${value}">${text}</td>`;
const shareCell = (part: number, whole: number) => numberCell(whole ? part / whole : -1, share(part, whole));
const keyOfLevel = (level: Level) => `<b class="key ${levelNames[level]}">${letterOfLevel[level]}</b>`;

function tableOf(className: string, titles: string[], rows: string[], totals: string[] = []): string {
    const heads = titles.map(title => `<th scope="col">${title && `<button type="button">${title}</button>`}</th>`);
    const foot = totals.length ? `<tfoot>${totals.join("\n")}</tfoot>` : "";
    return `<table class="${className}"><thead><tr>${heads.join("")}</tr></thead>\n<tbody>${rows.join("\n")}</tbody>${foot}</table>`;
}

function sharesRow(name: string, counts: Counts, more = ""): string {
    const n = operationCount(counts);
    const shares = everyLevel.map(level => shareCell(counts[level], n)).join("");
    return `<tr><th scope="row">${name}</th><td class="chart">${bar(counts, everyLevel, n)}</td>${numberCell(n)}${shares}${more}</tr>`;
}

function tile(label: string, value: string, note: string): string {
    return `<div class="tile"><dt>${label}</dt><dd>${value}</dd><p>${note}</p></div>`;
}

function headline(report: Report, byKind: ReturnType<typeof rowsByKind>): string {
    const { rows, allButOther, all } = byKind;
    const n = operationCount(allButOther);
    const accesses = sumOf(rows.filter(([kind]) => propertyAccesses.has(kind)));
    const everything = report.completeness.find(row => row.kind === "all");
    const legend = everyLevel.map(
        level =>
            `<li>${keyOfLevel(level)} <strong>${share(allButOther[level], n)}</strong> ${levelNames[level]}
            <small>${grouped(allButOther[level])}: ${meaningOfLevel[level]}</small></li>`,
    );
    const tiles = [
        tile(
            "Typed property accesses",
            share(accesses[3], operationCount(accesses)),
            `${grouped(accesses[3])} of ${grouped(operationCount(accesses))} reads and writes`,
        ),
        tile("Operations", grouped(n), `and ${grouped(operationCount(all) - n)} moves, jumps, returns and checks (&ldquo;other&rdquo;)`),
        everything &&
            tile(
                "Completeness",
                share(everything.reported, everything.inBytecode),
                `${grouped(everything.reported)} of ${grouped(everything.inBytecode)} instructions in bytecode are reported`,
            ),
        report.unreached &&
            tile(
                "Functions not compiled",
                grouped(report.unreached.functions),
                `${grouped(report.unreached.bytes)} bytes of bytecode: nothing reaches them`,
            ),
    ];
    return `<section class="headline">
<div class="hero">
<p><strong>${share(allButOther[inline], n)}</strong> of operations compiled to inline code</p>
<div class="chart">${bar(allButOther, everyLevel, n)}</div>
<ul class="legend">${legend.join("\n")}</ul>
</div>
<dl class="tiles">${tiles.filter(Boolean).join("\n")}</dl>
</section>`;
}

function incompleteWarning(completeness: Completeness[]): string {
    const incomplete = completeness.filter(row => row.kind !== "all" && row.reported !== row.inBytecode);
    if (!incomplete.length) return "";
    const kinds = incomplete.map(row => `${row.kind} ${share(row.reported, row.inBytecode)}`).join(", ");
    return `<p class="warning" role="alert"><b>!</b> <strong>This report is incomplete.</strong> The compiler did not report every
instruction of the functions it compiled (${kinds}). That is a bug in the compiler: see <a href="#completeness">Completeness</a>.</p>`;
}

function kindsSection({ rows, allButOther, all }: ReturnType<typeof rowsByKind>): string {
    const row = ([kind, counts]: Row) => sharesRow(kind, counts, shareCell(counts[3], operationCount(counts)));
    return `<section id="kinds"><h2>Kinds of operation</h2>
${tableOf(
    "sortable",
    ["Kind", "", "Operations", "Inline", "Stub", "Runtime", "Typed"],
    rows.map(row),
    [row(["all but other", allButOther]), row(["all", all])],
)}
</section>`;
}

function fileName(name: string): string {
    const slash = name.lastIndexOf("/") + 1;
    return `<span class="directory">${escapeHtml(name.slice(0, slash))}</span>${escapeHtml(name.slice(slash))}`;
}

function filesSection(report: Report, pathOf: Map<string, string>): string {
    const nameOf = (operation: Operation) => report.shortNameOf(operation.file);
    const accessesOf = tally(report.operations, nameOf, propertyAccesses);
    const rows = worstFirst(tally(report.operations, nameOf, specializable)).map(([name, counts]) => {
        const accesses = accessesOf.get(name) ?? [0, 0, 0, 0];
        const more = numberCell(notInline(counts)) + shareCell(accesses[3], operationCount(accesses));
        return sharesRow(`<a href="${pathOf.get(name)}">${fileName(name)}</a>`, counts, more);
    });
    return `<section id="files"><h2>Files</h2>
<p class="caption">Not counting &ldquo;other&rdquo;. Typed is the share of the property reads and writes of the file.
<input type="search" id="file-filter" placeholder="filter files" aria-label="filter files"></p>
${tableOf("sortable files", ["File", "", "Operations", "Inline", "Stub", "Runtime", "Not inline", "Typed"], rows)}
</section>`;
}

const largestFirst = (countsOf: Map<string, Counts>): Row[] =>
    [...countsOf].sort((a, b) => operationCount(b[1]) - operationCount(a[1]));

function reasonDetails(report: Report, key: string, operations: Operation[], pathOf: Map<string, string>): string {
    const files = largestFirst(tally(operations, operation => report.shortNameOf(operation.file))).slice(0, 10);
    const properties = largestFirst(tally(operations, operation => operation.property || undefined)).slice(0, 20);
    const count = (counts: Counts) => `<small>${grouped(operationCount(counts))}</small>`;
    return `<div class="where">
<div><h3>Files</h3><ul>${files
        .map(([name, counts]) => `<li><a href="${pathOf.get(name)}#reason=${key}">${fileName(name)}</a> ${count(counts)}</li>`)
        .join("")}</ul></div>
<div><h3>Properties</h3><ul class="properties">${properties
        .map(([name, counts]) => `<li><code>.${escapeHtml(readable(name))}</code> ${count(counts)}</li>`)
        .join("")}</ul></div>
</div>`;
}

function reasonsSection(report: Report, pathOf: Map<string, string>): string {
    const accesses = report.operations.filter(operation => propertyAccesses.has(operation.kind));
    const untyped = groupBy(
        accesses.filter(operation => !operation.typed),
        reasonKeyOf,
    );
    const rows = largestFirst(tally(accesses, operation => (operation.typed ? undefined : reasonKeyOf(operation))));
    const whole = weightOf(accesses);
    const largest = rows.length ? operationCount(rows[0][1]) : 0;
    return `<section id="reasons"><h2>Why accesses are not typed</h2>
<p class="caption">Property reads and writes by the reason the type table gave them no type, with their share of all reads and
writes. Open a reason to see where it occurs most.</p>
${rows
    .map(
        ([key, counts]) => `<details class="reason"><summary>
<span class="name"><small>${unnumberedReasons.has(key) ? "" : key}</small> ${escapeHtml(report.nameOfReason(key))}</span>
<span class="chart">${bar(counts, everyLevel, largest)}</span>
<span class="number">${grouped(operationCount(counts))}</span>
<span class="number">${share(operationCount(counts), whole)}</span>
</summary>${reasonDetails(report, key, untyped.get(key)!, pathOf)}</details>`,
    )
    .join("\n")}
</section>`;
}

function busiestPlace(operations: Operation[]): Position {
    const inBusiestFile = first([...groupBy(operations, operation => operation.file).values()], (a, b) => weightOf(a) > weightOf(b));
    return first(inBusiestFile, (a, b) => a.line < b.line);
}

function worstSection(id: string, column: string, caption: string, rows: Row[], linkOf: (name: string) => string): string {
    const largest = rows.length ? notInline(rows[0][1]) : 0;
    const row = ([name, counts]: Row) => {
        const numbers = [notInline(counts), counts[stub], counts[runtime], operationCount(counts)].map(n => numberCell(n));
        const chart = bar(counts, [stub, runtime], largest);
        return `<tr><th scope="row">${linkOf(name)}</th><td class="chart">${chart}</td>${numbers.join("")}</tr>`;
    };
    return `<section id="${id}"><h2>Top ${id}</h2>
<p class="caption">${caption}</p>
${tableOf("sortable", [column, "", "Not inline", "Stub", "Runtime", "Operations"], rows.map(row))}
</section>`;
}

function worstSections(report: Report, pathOf: Map<string, string>): string {
    const slow = report.operations.filter(operation => !isInline(operation));
    const pageOf = (position: Position) => pathOf.get(report.shortNameOf(position.file))!;
    const propertyOf: KeyOf = operation => operation.property || undefined;
    const functionOf: KeyOf = operation => operation.function || "(top level)";
    const slowAccessesOf = groupBy(
        slow.filter(operation => propertyAccesses.has(operation.kind)),
        propertyOf,
    );
    const slowIn = groupBy(slow, functionOf);
    const atLeastOne = ([, counts]: Row) => notInline(counts) > 0;
    return `<div class="columns">
${worstSection(
    "properties",
    "Property",
    "Reads and writes that are not inline. The link shows them in the file that has the most.",
    worstFirst(tally(report.operations, propertyOf, propertyAccesses)).filter(atLeastOne).slice(0, report.top),
    name => {
        const hash = `#name=.${encodeURIComponent(readable(name))}&kind=reads,writes&level=runtime,stub`;
        return `<a href="${pageOf(busiestPlace(slowAccessesOf.get(name)!))}${hash}"><code>.${escapeHtml(readable(name))}</code></a>`;
    },
)}
${worstSection(
    "functions",
    "Function",
    "Operations that are not inline, by the name of the function they were compiled into.",
    worstFirst(tally(report.operations, functionOf, specializable)).filter(atLeastOne).slice(0, report.top),
    name => {
        const place = busiestPlace(slowIn.get(name)!);
        const where = escapeHtml(`${baseName(place.file)}:${place.line}`);
        return `<a href="${pageOf(place)}#L${place.line}"><code>${escapeHtml(readable(name))}</code> <small>${where}</small></a>`;
    },
)}
</div>`;
}

function completenessSection(completeness: Completeness[]): string {
    if (!completeness.length) return "";
    const row = ({ kind, inBytecode, reported, inClassFieldCode }: Completeness) => {
        const cells = [numberCell(inBytecode), numberCell(reported), shareCell(reported, inBytecode), numberCell(inClassFieldCode)];
        return `<tr${reported === inBytecode ? "" : ' class="incomplete"'}><th scope="row">${kind}</th>${cells.join("")}</tr>`;
    };
    return `<section id="completeness"><h2>Completeness</h2>
<p class="caption">How many instructions of each kind the compiled functions have in bytecode, and how many of them the compiler
reported. It should report all of them.</p>
${tableOf(
    "sortable narrow",
    ["Kind", "In bytecode", "Reported", "Share", "In class field code"],
    completeness.slice(0, -1).map(row),
    completeness.slice(-1).map(row),
)}
</section>`;
}

function htmlPage(title: string, root: string, className: string, body: string, script: string): string {
    return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(title)}</title>
<link rel="stylesheet" href="${root}report.css">
</head>
<body class="${className}">
${body}
<script src="${root}${script}"></script>
</body>
</html>
`;
}

function indexPageOf(report: Report, pathOf: Map<string, string>): string {
    const byKind = rowsByKind(report.operations);
    return htmlPage(
        `Type coverage of ${report.title}`,
        "",
        "index",
        `<header><h1>Type coverage</h1><p>${escapeHtml(report.title)} &middot; ${grouped(pathOf.size)} files</p>
<nav><a href="#kinds">Kinds</a> <a href="#files">Files</a> <a href="#reasons">Reasons</a> <a href="#properties">Properties</a>
<a href="#functions">Functions</a> <a href="#completeness">Completeness</a></nav></header>
${incompleteWarning(report.completeness)}
${headline(report, byKind)}
${kindsSection(byKind)}
${filesSection(report, pathOf)}
${reasonsSection(report, pathOf)}
${worstSections(report, pathOf)}
${completenessSection(report.completeness)}`,
        "index.js",
    );
}

function interner<T>() {
    const values: T[] = [];
    const indexOf = new Map<string, number>();
    const index = (key: string, make: () => T) => {
        let found = indexOf.get(key);
        if (found === undefined) indexOf.set(key, (found = values.push(make()) - 1));
        return found;
    };
    return { values, index };
}

interface Placed {
    operation: Operation;
    line: number;
    column: number;
}

const upToProperty = /[\w$]*[\s!)\]]*\??\.\s*/y;

function columnToMark(text: string, { column, opcode, property }: Operation): number {
    const name = opcode === "op_get_length" ? "length" : unescaped(property);
    upToProperty.lastIndex = column - 1;
    if (!name || !upToProperty.test(text)) return column;
    const start = upToProperty.lastIndex;
    return text.startsWith(name, start) && !/[\w$]/.test(text.charAt(start + name.length)) ? start + 1 : column;
}

function placeInSource(operations: Operation[], source: string): { lines: string[]; placed: Placed[] } {
    const lines = source.split(/\r\n?|\n/);
    const placed = operations.map(operation => {
        const line = Math.max(1, operation.line);
        return { operation, line, column: columnToMark(lines[line - 1] ?? "", operation) };
    });
    const lastLine = placed.reduce((last, { line }) => Math.max(last, line), 0);
    while (lines.length < lastLine) lines.push("");
    return { lines, placed: placed.sort((a, b) => a.line - b.line || a.column - b.column) };
}

function placeInListing(operations: Operation[]): { lines: string[]; placed: Placed[] } {
    const sorted = [...operations].sort((a, b) => a.line - b.line || a.column - b.column);
    const prefixes = sorted.map(({ line, column }) => `${line}:${column}`.padEnd(12));
    return {
        lines: sorted.map(({ opcode, property }, index) => `${prefixes[index]}${opcode.slice(3)}${property && ` .${readable(property)}`}`),
        placed: sorted.map((operation, index) => ({ operation, line: index + 1, column: prefixes[index].length + 1 })),
    };
}

function filePageOf(operations: Operation[], source: string | undefined, nameOfReason: (key: string) => string): FilePage {
    const { lines, placed } = source === undefined ? placeInListing(operations) : placeInSource(operations, source);
    const reasons = interner<[string, string]>();
    const strings = interner<string>();
    const outcomes = interner<string[]>();
    const details = interner<number[]>();
    const stringIndex = (text: string) => strings.index(text, () => readable(text));
    const detailIndex = (operation: Operation) => {
        const reason = reasonKeyOf(operation);
        const detail = [
            stringIndex(operation.opcode.slice(3)),
            allKinds.indexOf(operation.kind),
            operation.level,
            Number(operation.typed),
            operation.flags,
            propertyAccesses.has(operation.kind) ? reasons.index(reason, () => [reason, nameOfReason(reason)]) : -1,
            stringIndex(operation.property),
            outcomes.index(operation.outcomes.join(" "), () => operation.outcomes.map(readable)),
            operation.copies,
        ];
        return details.index(detail.join(), () => detail);
    };
    stringIndex("");
    return {
        lines,
        operations: {
            line: placed.map(({ line }, index) => line - (index ? placed[index - 1].line : 0)),
            column: placed.map(({ column }) => column),
            detail: placed.map(({ operation }) => detailIndex(operation)),
            function: placed.map(({ operation }) => stringIndex(operation.function)),
            offset: placed.map(({ operation }) => operation.offset),
            weight: placed.map(({ operation }) => operation.weight),
        },
        reasons: reasons.values,
        strings: strings.values,
        outcomes: outcomes.values,
        details: details.values,
    };
}

function filePageHtml(name: string, path: string, page: FilePage, hasSource: boolean): string {
    const root = "../".repeat(path.split("/").length - 1);
    return htmlPage(
        `${baseName(name)}: type coverage`,
        root,
        "file",
        `<header>
<div class="title"><a href="${root}index.html">Type coverage</a> / <h1>${fileName(name)}</h1>
<span class="chart" id="shares"></span><output id="numbers"></output></div>
<form id="filters">
<fieldset id="levels" aria-label="levels"></fieldset>
<span class="chip legend"><span class="sample inline typed">typed</span></span>
<select id="reason" aria-label="reason an access is not typed"></select>
<input type="search" id="name" placeholder="property or function" title="part of the name of a property or a function,
or a whole property name after a dot: .kind" aria-label="property or function">
<label><input type="checkbox" id="loops"> in loops</label>
<label><input type="checkbox" id="often"> not rarely executed</label>
<span class="steps"><button type="button" id="previous" title="previous place that is not inline (p)">&uarr;</button>
<output id="where"></output>
<button type="button" id="next" title="next place that is not inline (n)">&darr;</button></span>
<fieldset id="kinds" aria-label="kinds of operation"><button type="button" id="all-kinds">all</button>
<button type="button" id="no-kinds">none</button></fieldset>
</form>
${hasSource ? "" : '<p class="caption">The source text is not available: this lists the operations by position.</p>'}
</header>
<main><div id="code" tabindex="0"><div id="lines"><div id="popover" hidden></div></div></div>
<aside id="minimap"><canvas></canvas><div id="viewport"></div></aside></main>
<script type="application/json" id="data">${JSON.stringify(page).replace(/</g, "\\u003c")}</script>`,
        "file.js",
    );
}

const styleSheet = `:root {
    color-scheme: light dark;
    --page: #f9f9f7; --surface: #fcfcfb; --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781; --hairline: #e1e0d9; --axis: #c3c2b7;
    --inline: #2a78d6; --stub: #eda100; --runtime: #c2255c; --stub-line: #a06c00; --link: #1c5cab;
    --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    --sans: system-ui, -apple-system, "Segoe UI", sans-serif;
}
@media (prefers-color-scheme: dark) {
    :root {
        --page: #0d0d0d; --surface: #1a1a19; --ink: #fff; --ink-2: #c3c2b7; --hairline: #2c2c2a; --axis: #383835;
        --inline: #3987e5; --stub: #c98500; --runtime: #e0457b; --stub-line: #e0a030; --link: #86b6ef;
    }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--page); color: var(--ink); font: 14px/1.45 var(--sans); }
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
code { font-family: var(--mono); font-size: .93em; }
small, .caption, .directory { color: var(--ink-2); }
small { font-size: .86em; }
h1, h2, h3, p, ul, ol, dl, dd, fieldset { margin: 0; padding: 0; border: 0; }
ul, ol { list-style: none; }
button, input, select { font: inherit; color: inherit; }
input[type=search], select { background: var(--surface); border: 1px solid var(--axis); border-radius: 5px; padding: 1px 6px; }

.bar { display: flex; gap: 2px; height: 10px; }
.bar > i { flex: 0 0 0; min-width: 2px; }
.bar > i:last-child { border-radius: 0 3px 3px 0; }
.bar > .inline, .key.inline { background-color: var(--inline); }
.bar > .stub, .key.stub { background-color: var(--stub); }
.bar > .runtime, .key.runtime { background-color: var(--runtime); }
.key {
    display: inline-block; width: 1.5em; border-radius: 3px; color: #fff; font: 700 .78em/1.5 var(--mono); text-align: center;
}
.key.stub { color: #0b0b0b; }
@media (forced-colors: active), print {
    .bar > i, .key { forced-color-adjust: none; print-color-adjust: exact; }
    .bar > .stub { background-image: repeating-linear-gradient(135deg, #0006 0 1px, #0000 1px 4px); }
    .bar > .runtime { background-image: repeating-linear-gradient(45deg, #fff9 0 1px, #fff0 1px 4px); }
}

.index { max-width: 1180px; margin: 0 auto; padding: 24px 24px 64px; }
.index > header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 16px; }
.index h1 { font-size: 22px; }
.index > header p { color: var(--ink-2); }
.index nav { margin-left: auto; display: flex; gap: 14px; }
.index section { margin-top: 36px; }
.index h2 { font-size: 16px; margin-bottom: 2px; }
.index h3 { font-size: 12px; color: var(--ink-2); font-weight: 600; margin-bottom: 2px; }
.caption { display: flex; gap: 16px; align-items: center; justify-content: space-between; margin-bottom: 8px; font-size: 13px; }
.warning {
    margin-top: 16px; padding: 10px 14px; border: 2px solid var(--runtime); border-radius: 6px; background: var(--surface);
}
.warning > b {
    display: inline-block; width: 1.5em; margin-right: 6px; border-radius: 50%; background: var(--runtime); color: #fff;
    text-align: center;
}

.headline { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 32px; margin-top: 20px !important; }
.hero p { color: var(--ink-2); font-size: 15px; }
.hero p strong { display: block; color: var(--ink); font-size: 52px; line-height: 1.1; font-weight: 650; }
.hero .bar { height: 16px; margin: 10px 0 12px; }
.legend li { display: grid; grid-template-columns: 1.6em 4em 4.5em 1fr; align-items: baseline; }
.tiles { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.tile { padding: 10px 14px; background: var(--surface); border: 1px solid var(--hairline); border-radius: 8px; }
.tile dt { color: var(--ink-2); font-size: 13px; }
.tile dd { font-size: 24px; font-weight: 650; }
.tile p { color: var(--ink-2); font-size: 12px; }

table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
table.narrow { width: auto; min-width: 50%; }
th, td { padding: 2px 8px; text-align: right; white-space: nowrap; }
th:first-child { text-align: left; font-weight: 400; overflow-wrap: anywhere; white-space: normal; }
thead th { border-bottom: 1px solid var(--axis); color: var(--ink-2); font-size: 12px; font-weight: 600 !important; }
thead button { all: unset; cursor: pointer; }
thead button:hover, thead button:focus-visible { color: var(--ink); text-decoration: underline; }
th[aria-sort=ascending] button::after { content: " \\25B2"; }
th[aria-sort=descending] button::after { content: " \\25BC"; }
tbody tr:hover, .reason summary:hover { background: color-mix(in srgb, var(--ink) 5%, transparent); }
tfoot tr:first-child > * { border-top: 1px solid var(--axis); }
tfoot th, tfoot td { font-weight: 600 !important; }
td.chart { width: 26%; min-width: 120px; }
tr.incomplete td { color: var(--runtime); font-weight: 700; }
tr.incomplete th::after { content: " (incomplete)"; font-weight: 700; }

.reason summary {
    display: grid; grid-template-columns: minmax(0, 5fr) minmax(0, 4fr) 5em 4em; gap: 8px; align-items: center;
    padding: 2px 8px; cursor: pointer; list-style: none;
}
.reason summary::-webkit-details-marker { display: none; }
.reason .name::before { content: "\\25B8"; display: inline-block; width: 1.1em; color: var(--muted); }
.reason[open] .name::before { content: "\\25BE"; }
.reason .name small { display: inline-block; min-width: 1.6em; text-align: right; font-variant-numeric: tabular-nums; }
.number { text-align: right; font-variant-numeric: tabular-nums; }
.where {
    display: grid; grid-template-columns: 1fr 1fr; gap: 24px; margin: 4px 8px 12px 3.4em; padding: 8px 12px;
    background: var(--surface); border: 1px solid var(--hairline); border-radius: 6px;
}
.properties { display: flex; flex-wrap: wrap; gap: 0 14px; }
.columns { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 32px; }
.columns th:first-child { width: 40%; max-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.columns td.chart { width: 20%; min-width: 60px; }
@media (max-width: 900px) { .headline, .columns, .where { grid-template-columns: minmax(0, 1fr); } }

.file { height: 100vh; display: flex; flex-direction: column; overflow: hidden; }
.file > header { padding: 6px 12px; background: var(--surface); border-bottom: 1px solid var(--axis); font-size: 13px; }
.title { display: flex; align-items: center; gap: 8px; }
.file h1 { font-size: 14px; }
.title .chart { width: 180px; margin-left: auto; }
#filters { display: flex; flex-wrap: wrap; align-items: center; gap: 4px 10px; margin-top: 4px; }
#filters fieldset { display: flex; flex-wrap: wrap; gap: 4px; }
#kinds { flex-basis: 100%; }
#name { width: 13em; }
#reason { max-width: 20em; }
.chip {
    display: inline-flex; align-items: center; gap: 5px; padding: 0 8px; border: 1px solid var(--axis); border-radius: 999px;
    background: var(--page); cursor: pointer; user-select: none;
}
.chip input { position: absolute; opacity: 0; pointer-events: none; }
.chip:has(input:not(:checked)) { opacity: .5; border-style: dashed; background: none; }
.chip:has(input:focus-visible) { outline: 2px solid var(--link); }
.chip.empty small { visibility: hidden; }
.chip.legend { cursor: default; border-color: transparent; background: none; }
#filters button { padding: 0 8px; border: 1px solid var(--axis); border-radius: 5px; background: var(--page); cursor: pointer; }
.steps { display: inline-flex; align-items: center; gap: 6px; margin-left: auto; font-variant-numeric: tabular-nums; }

main { flex: 1; min-height: 0; display: flex; }
#code { flex: 1; min-width: 0; overflow: auto; outline: none; font: 12.5px/18px var(--mono); tab-size: 4; }
#lines { position: relative; width: max-content; min-width: 100%; }
.chunk { content-visibility: auto; }
.l { height: 18px; white-space: pre; }
.l::before {
    content: attr(data-n) "  "; position: sticky; left: 0; display: inline-block; width: calc((var(--digits) + 4) * 1ch);
    margin-right: 1ch; padding-right: .5ch; border-right: 4px solid transparent; background: var(--page); color: var(--muted);
    text-align: right; cursor: pointer;
}
.l.inline::before { border-right-color: color-mix(in srgb, var(--inline) 40%, transparent); }
.l.stub::before { content: attr(data-n) " S"; border-right-color: var(--stub); color: var(--ink-2); }
.l.runtime::before { content: attr(data-n) " R"; border-right-color: var(--runtime); color: var(--ink-2); }
.l.selected { background: color-mix(in srgb, var(--inline) 14%, transparent); }
.l > i, .sample {
    font-style: normal; border-radius: 2px; text-decoration: underline 1px; text-underline-offset: 3px;
    text-decoration-skip-ink: none;
}
.l > i { cursor: pointer; }
.l > i:empty::after { content: "\\00B7"; }
.l > .inline, .sample.inline { text-decoration-style: dotted; text-decoration-color: var(--inline); }
.l > .stub, .sample.stub {
    background: color-mix(in srgb, var(--stub) 24%, transparent); text-decoration-style: dashed;
    text-decoration-color: var(--stub-line); text-decoration-thickness: 2px;
}
.l > .runtime, .sample.runtime {
    background: color-mix(in srgb, var(--runtime) 20%, transparent); text-decoration-style: wavy;
    text-decoration-color: var(--runtime);
}
.l > .typed, .sample.typed { box-shadow: inset 0 2px 0 var(--inline); }
.l > i:hover, .l > i.open { outline: 1px solid var(--ink-2); }

#minimap {
    position: relative; flex: none; width: 64px; border-left: 1px solid var(--hairline); background: var(--surface);
    cursor: pointer; touch-action: none;
}
#minimap canvas { display: block; width: 100%; height: 100%; }
#viewport {
    position: absolute; left: 0; right: 0; border: 1px solid var(--ink-2);
    background: color-mix(in srgb, var(--ink) 8%, transparent); pointer-events: none;
}

#popover {
    position: absolute; z-index: 1; width: max-content; min-width: 260px; max-width: 680px; max-height: 320px; overflow: auto;
    padding: 2px 0; background: var(--surface); border: 1px solid var(--axis); border-radius: 6px;
    box-shadow: 0 6px 24px rgb(0 0 0 / .22); font: 12px/1.45 var(--sans);
    white-space: normal;
}
#popover > p { padding: 2px 10px; color: var(--ink-2); }
#popover ol { display: grid; grid-template-columns: auto auto auto 1fr; }
#popover li { display: contents; }
#popover li > * { padding: 2px 0 2px 10px; border-top: 1px solid var(--hairline); }
#popover li > small { text-align: right; font-variant-numeric: tabular-nums; }
#popover .key { width: auto; padding: 0 5px; }
#popover li > .what { font-weight: 600; white-space: nowrap; }
#popover li > div { padding-right: 10px; overflow-wrap: anywhere; }
#popover li > div > * { display: block; }
`;

function indexPageScript() {
    const valueOf = (row: HTMLTableRowElement, column: number) => {
        const cell = row.cells[column];
        return cell.dataset.value === undefined ? cell.textContent! : Number(cell.dataset.value);
    };

    function sortBy(header: HTMLTableCellElement) {
        const body = header.closest("table")!.tBodies[0];
        const sorted = header.getAttribute("aria-sort");
        const ascending = sorted ? sorted === "descending" : typeof valueOf(body.rows[0], header.cellIndex) === "string";
        for (const other of header.parentElement!.children) other.removeAttribute("aria-sort");
        header.setAttribute("aria-sort", ascending ? "ascending" : "descending");
        const rows = [...body.rows].sort((a, b) => {
            const [x, y] = [valueOf(a, header.cellIndex), valueOf(b, header.cellIndex)];
            return (x < y ? -1 : x > y ? 1 : 0) * (ascending ? 1 : -1);
        });
        body.append(...rows);
    }

    document.addEventListener("click", event => {
        const header = (event.target as Element).closest("table.sortable thead button")?.closest("th");
        if (header) sortBy(header);
    });

    const filter = document.getElementById("file-filter") as HTMLInputElement;
    filter.addEventListener("input", () => {
        for (const row of document.querySelectorAll<HTMLTableRowElement>("table.files tbody tr"))
            row.hidden = !row.cells[0].textContent!.toLowerCase().includes(filter.value.toLowerCase());
    });
}

interface PageConstants {
    Flag: Record<string, number>;
    nameOfFlag: [bit: number, name: string][];
    kinds: string[];
    levels: string[];
    letterOfLevel: string[];
}

function filePageScript(
    { Flag, nameOfFlag, kinds, levels, letterOfLevel }: PageConstants,
    oneDecimal: (value: number) => string,
    grouped: (n: number) => string,
) {
    const lineHeight = 18;
    const linesPerChunk = 100;
    const inline = levels.indexOf("inline");
    const Facet = { kind: 1, level: 2, reason: 4, rest: 8 };

    const byId = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
    const page: FilePage = JSON.parse(byId("data").textContent!);
    const { lines, reasons, strings, operations } = page;
    const details = page.details.map(([opcode, kind, level, typed, flags, reason, property, outcomes, copies]) => ({
        opcode,
        kind,
        level,
        typed,
        flags,
        reason,
        property,
        outcomes,
        copies,
    }));
    const total = operations.detail.length;
    const detailOf = (index: number) => details[operations.detail[index]];

    const lineOf = new Int32Array(total);
    const firstOnLine = new Int32Array(lines.length + 2);
    const failed = new Uint8Array(total);
    const stale = new Set<number>();
    let targets: number[] = [];
    let current = -1;
    let selectedLine = 0;
    let pinned = false;
    let closing = 0;
    let filling = false;
    let mapHeight = 0;

    const code = byId("code");
    const linesElement = byId("lines");
    const popover = byId("popover");
    const minimap = byId("minimap");
    const canvas = minimap.querySelector("canvas")!;
    const reasonSelect = byId<HTMLSelectElement>("reason");
    const nameInput = byId<HTMLInputElement>("name");
    const loopsBox = byId<HTMLInputElement>("loops");
    const oftenBox = byId<HTMLInputElement>("often");

    function element(tag: string, className = "", text = "") {
        const made = document.createElement(tag);
        made.className = className;
        made.textContent = text;
        return made;
    }

    function addChip(container: HTMLElement, name: string, letter = "") {
        const chip = container.appendChild(element("label", "chip"));
        const box = chip.appendChild(element("input")) as HTMLInputElement;
        box.type = "checkbox";
        if (letter) chip.append(element("b", `key ${name}`, letter));
        chip.append(element("span", letter && `sample ${name}`, name));
        const count = chip.appendChild(element("small"));
        const show = (n: number) => {
            count.textContent = grouped(n);
            chip.classList.toggle("empty", !n);
        };
        return { box, show };
    }

    const bestFirst = [...levels.keys()].reverse();
    const levelChips = bestFirst.map(level => addChip(byId("levels"), levels[level], letterOfLevel[level])).reverse();
    const kindChips = kinds.map(name => addChip(byId("kinds"), name));
    const reasonOptions = reasons.map(([key]) => new Option("", key));
    const chunks = Array.from({ length: Math.ceil(lines.length / linesPerChunk) }, (_, index) => {
        const chunk = linesElement.appendChild(element("div", "chunk"));
        chunk.style.height = `${Math.min(linesPerChunk, lines.length - index * linesPerChunk) * lineHeight}px`;
        return chunk;
    });

    function indexOperations() {
        let line = 0;
        for (let index = 0; index < total; index++) {
            lineOf[index] = line += operations.line[index];
            firstOnLine[line + 1]++;
        }
        for (let number = 1; number < firstOnLine.length; number++) firstOnLine[number] += firstOnLine[number - 1];
    }

    function applyFilters() {
        const isExact = nameInput.value.startsWith(".");
        const named = strings.map(text => (isExact ? `.${text}` === nameInput.value : text.includes(nameInput.value)));
        const failedOfDetail = details.map(
            ({ kind, level, reason, flags }) =>
                (kindChips[kind].box.checked ? 0 : Facet.kind) |
                (levelChips[level].box.checked ? 0 : Facet.level) |
                (!reasonSelect.value || reasons[reason]?.[0] === reasonSelect.value ? 0 : Facet.reason) |
                ((loopsBox.checked && !(flags & Flag.inLoop)) || (oftenBox.checked && flags & Flag.rarelyExecuted) ? Facet.rest : 0),
        );
        for (let index = 0; index < total; index++) {
            const isNamed = named[detailOf(index).property] || (!isExact && named[operations.function[index]]);
            failed[index] = failedOfDetail[operations.detail[index]] | (isNamed ? 0 : Facet.rest);
        }
    }

    function showCounts() {
        const ofKind = kinds.map(() => 0);
        const ofLevel = levels.map(() => 0);
        const ofReason = reasons.map(() => 0);
        const shown = levels.map(() => 0);
        for (let index = 0; index < total; index++) {
            const { kind, level, reason } = detailOf(index);
            const weight = operations.weight[index];
            if (!(failed[index] & ~Facet.kind)) ofKind[kind] += weight;
            if (!(failed[index] & ~Facet.level)) ofLevel[level] += weight;
            if (!(failed[index] & ~Facet.reason) && reason >= 0) ofReason[reason] += weight;
            if (!failed[index]) shown[level] += weight;
        }
        kindChips.forEach((chip, kind) => chip.show(ofKind[kind]));
        levelChips.forEach((chip, level) => chip.show(ofLevel[level]));
        reasonOptions.forEach((option, reason) => (option.text = `${reasons[reason][1]} (${grouped(ofReason[reason])})`));
        showShares(shown);
    }

    function showShares(shown: number[]) {
        const whole = shown.reduce((sum, n) => sum + n, 0);
        const bar = element("span", "bar");
        for (const level of bestFirst.filter(level => shown[level]))
            bar.appendChild(element("i", levels[level])).style.flexGrow = `${shown[level]}`;
        byId("shares").replaceChildren(bar);
        const shares = bestFirst.map(level => `${whole ? oneDecimal((100 * shown[level]) / whole) : "-"}% ${levels[level]}`);
        byId("numbers").textContent = `${grouped(whole)} operations: ${shares.join(", ")}`;
    }

    function placeOf(index: number): [start: number, end: number] {
        const samePlace = (other: number) => lineOf[other] === lineOf[index] && operations.column[other] === operations.column[index];
        let start = index;
        let end = index + 1;
        while (start > 0 && samePlace(start - 1)) start--;
        while (end < total && samePlace(end)) end++;
        return [start, end];
    }

    function findTargets() {
        const places: number[] = [];
        const slowPlaces: number[] = [];
        for (let start = 0, end = 0; start < total; start = end) {
            [, end] = placeOf(start);
            const shown = [];
            for (let index = start; index < end; index++) if (!failed[index]) shown.push(index);
            if (shown.length) places.push(shown[0]);
            if (shown.some(index => detailOf(index).level !== inline)) slowPlaces.push(shown[0]);
        }
        return slowPlaces.length ? slowPlaces : places;
    }

    function marksOn(number: number) {
        const marks: { first: number; column: number; level: number; typed: number }[] = [];
        for (let index = firstOnLine[number]; index < firstOnLine[number + 1]; index++) {
            if (failed[index]) continue;
            const { level, typed } = detailOf(index);
            const last = marks.at(-1);
            if (last?.column !== operations.column[index]) marks.push({ first: index, column: operations.column[index], level, typed });
            else Object.assign(last, { level: Math.min(last.level, level), typed: last.typed | typed });
        }
        return marks;
    }

    const token = /[\p{L}\p{N}_$]+|./uy;
    const escapeHtml = (text: string) => text.replace(/[&<>]/g, character => `&#${character.charCodeAt(0)};`);

    function lineHtml(number: number) {
        const text = lines[number - 1];
        const marks = marksOn(number);
        let html = "";
        let cursor = 0;
        marks.forEach(({ first, column, level, typed }, index) => {
            const start = Math.max(cursor, Math.min(column - 1, text.length));
            const limit = index + 1 < marks.length ? Math.max(start, marks[index + 1].column - 1) : text.length;
            token.lastIndex = start;
            const end = Math.min(limit, token.test(text) ? token.lastIndex : start);
            const classes = levels[level] + (typed ? " typed" : "");
            html += escapeHtml(text.slice(cursor, start));
            html += `<i class="${classes}" data-o="${first}">${escapeHtml(text.slice(start, end))}</i>`;
            cursor = end;
        });
        const worst = levels[Math.min(...marks.map(mark => mark.level))] ?? "";
        const classes = `l ${worst}${number === selectedLine ? " selected" : ""}`;
        return `<div class="${classes}" data-n="${number}">${html}${escapeHtml(text.slice(cursor))}</div>`;
    }

    function renderChunk(index: number) {
        const firstLine = index * linesPerChunk + 1;
        const lastLine = Math.min(lines.length, firstLine + linesPerChunk - 1);
        let html = "";
        for (let number = firstLine; number <= lastLine; number++) html += lineHtml(number);
        chunks[index].innerHTML = html;
        stale.delete(index);
    }

    function renderVisible() {
        const chunkHeight = linesPerChunk * lineHeight;
        const firstChunk = Math.max(0, Math.floor(code.scrollTop / chunkHeight) - 1);
        const lastChunk = Math.min(chunks.length - 1, Math.floor((code.scrollTop + code.clientHeight) / chunkHeight) + 1);
        for (let index = firstChunk; index <= lastChunk; index++) if (stale.has(index)) renderChunk(index);
    }

    function renderRestWhenIdle() {
        if (filling || !stale.size) return;
        filling = true;
        const soon = (callback: IdleRequestCallback) => setTimeout(callback, 1, { timeRemaining: () => 0 });
        (window.requestIdleCallback ?? soon)(deadline => {
            filling = false;
            do renderChunk(stale.values().next().value!);
            while (stale.size && deadline.timeRemaining() > 4);
            renderRestWhenIdle();
        });
    }

    function nearlyLargest(values: Float64Array) {
        const sorted = values.filter(Boolean).sort();
        return sorted[Math.floor((sorted.length - 1) * 0.98)];
    }

    function drawMinimap() {
        const { clientWidth: width, clientHeight: height } = minimap;
        const buckets = Math.max(1, Math.min(height, lines.length));
        const thickness = Math.min(3, height / buckets);
        const sums = levels.map(() => new Float64Array(buckets));
        for (let index = 0; index < total; index++) {
            const bucket = Math.floor(((lineOf[index] - 1) * buckets) / lines.length);
            if (!failed[index]) sums[detailOf(index).level][bucket] += operations.weight[index];
        }
        const slow = sums[inline].map((ofInline, bucket) => sums.reduce((sum, ofLevel) => sum + ofLevel[bucket], -ofInline));
        const scale = width / (nearlyLargest(slow) || nearlyLargest(sums[inline]) || 1);
        const style = getComputedStyle(document.documentElement);
        const context = canvas.getContext("2d")!;
        mapHeight = buckets * thickness;
        canvas.width = width * devicePixelRatio;
        canvas.height = height * devicePixelRatio;
        context.scale(devicePixelRatio, devicePixelRatio);
        for (let bucket = 0; bucket < buckets; bucket++) {
            let x = 0;
            levels.forEach((name, level) => {
                context.fillStyle = style.getPropertyValue(`--${name}`);
                context.globalAlpha = level === inline ? 0.3 : 1;
                context.fillRect(x, bucket * thickness, sums[level][bucket] * scale, thickness);
                x += sums[level][bucket] * scale;
            });
        }
        showViewport();
    }

    function showViewport() {
        const scale = mapHeight / (lines.length * lineHeight);
        byId("viewport").style.top = `${code.scrollTop * scale}px`;
        byId("viewport").style.height = `${Math.max(6, Math.min(code.clientHeight, lines.length * lineHeight) * scale)}px`;
    }

    function scrollToPointer(event: PointerEvent) {
        if (!event.buttons) return;
        const y = event.clientY - minimap.getBoundingClientRect().top;
        code.scrollTop = (y / mapHeight) * lines.length * lineHeight - code.clientHeight / 2;
    }

    const functionOf = (index: number) => strings[operations.function[index]] || "(top level)";

    function entryOf(index: number, namesFunction: boolean) {
        const { opcode, level, typed, flags, reason, property, outcomes, copies } = detailOf(index);
        const entry = element("li");
        const pill = element("span");
        const found = element("div");
        const notes = [
            ...nameOfFlag.filter(([bit]) => flags & bit).map(([, name]) => name),
            ...(copies > 1 ? [`${copies} copies`] : []),
            ...(namesFunction ? [`in ${functionOf(index)}`] : []),
        ];
        pill.append(element("b", `key ${levels[level]}`, levels[level]));
        found.append(...page.outcomes[outcomes].map(outcome => element("code", "", outcome)));
        if (reason >= 0) found.append(element("p", "", typed ? "typed" : `not typed: ${reasons[reason][1]}`));
        if (notes.length) found.append(element("small", "", notes.join(", ")));
        entry.append(
            element("small", "", `bc#${operations.offset[index]}`),
            pill,
            element("code", "what", strings[opcode] + (property ? ` .${strings[property]}` : "")),
            found,
        );
        return entry;
    }

    function closePopover() {
        popover.hidden = true;
        pinned = false;
        linesElement.querySelector("i.open")?.classList.remove("open");
    }

    function openPopover(mark: HTMLElement, pin: boolean) {
        closePopover();
        const [start, end] = placeOf(Number(mark.dataset.o));
        const shown = Array.from({ length: end - start }, (_, index) => start + index).filter(index => !failed[index]);
        const inOneFunction = shown.every(index => operations.function[index] === operations.function[shown[0]]);
        const hidden = end - start - shown.length;
        const listed = shown.slice(0, 200);
        const title = [
            `line ${lineOf[start]}`,
            ...(inOneFunction ? [`in ${functionOf(start)}`] : []),
            ...(listed.length < shown.length ? [`the first ${listed.length} of ${grouped(shown.length)} operations`] : []),
            ...(hidden ? [`${grouped(hidden)} more hidden by the filters`] : []),
        ];
        const list = element("ol");
        list.append(...listed.map(index => entryOf(index, !inOneFunction)));
        popover.replaceChildren(element("p", "", title.join(", ")), list);
        popover.hidden = false;
        pinned = pin;
        mark.classList.add("open");
        const bottomOfLine = lineOf[start] * lineHeight;
        const fitsBelow = bottomOfLine + popover.offsetHeight + 4 < code.scrollTop + code.clientHeight;
        const left = mark.getBoundingClientRect().left - linesElement.getBoundingClientRect().left;
        const rightmost = code.scrollLeft + code.clientWidth - popover.offsetWidth - 8;
        popover.style.top = `${fitsBelow ? bottomOfLine + 3 : bottomOfLine - lineHeight - popover.offsetHeight - 3}px`;
        popover.style.left = `${Math.max(code.scrollLeft, Math.min(left, rightmost))}px`;
    }

    function showStep() {
        byId("where").textContent = `${current < 0 ? "–" : grouped(current + 1)} / ${grouped(targets.length)}`;
    }

    function reveal(first: number) {
        const chunk = Math.floor((lineOf[first] - 1) / linesPerChunk);
        if (stale.has(chunk)) renderChunk(chunk);
        const mark = chunks[chunk].querySelector<HTMLElement>(`[data-o="${first}"]`)!;
        mark.scrollIntoView({ block: "center", inline: "nearest" });
        renderVisible();
        openPopover(mark, true);
    }

    function step(direction: 1 | -1) {
        if (!targets.length) return;
        const top = code.scrollTop / lineHeight + 1;
        const bottom = top + code.clientHeight / lineHeight;
        const isInView = current >= 0 && lineOf[targets[current]] >= top && lineOf[targets[current]] <= bottom;
        if (isInView) current += direction;
        else if (direction > 0) current = targets.findIndex(first => lineOf[first] >= top);
        else current = targets.findLastIndex(first => lineOf[first] < top);
        if (current < 0 || current >= targets.length) current = direction > 0 ? 0 : targets.length - 1;
        showStep();
        reveal(targets[current]);
    }

    function selectLine(number: number) {
        selectedLine = number;
        linesElement.querySelector(".l.selected")?.classList.remove("selected");
        linesElement.querySelector(`.l[data-n="${number}"]`)?.classList.add("selected");
        writeHash();
    }

    function writeHash() {
        const checked = (chips: typeof kindChips, names: string[]) => names.filter((_, index) => chips[index].box.checked).join();
        const parts = [
            selectedLine ? `L${selectedLine}` : "",
            levelChips.every(chip => chip.box.checked) ? "" : `level=${checked(levelChips, levels)}`,
            kindChips.every((chip, kind) => chip.box.checked === (kinds[kind] !== "other")) ? "" : `kind=${checked(kindChips, kinds)}`,
            reasonSelect.value && `reason=${reasonSelect.value}`,
            nameInput.value && `name=${encodeURIComponent(nameInput.value)}`,
            loopsBox.checked ? "loops" : "",
            oftenBox.checked ? "often" : "",
        ];
        history.replaceState(null, "", `#${parts.filter(Boolean).join("&")}`);
    }

    function readHash() {
        const values = new Map(
            location.hash
                .slice(1)
                .split("&")
                .map(part => part.split(/=(.*)/s) as [string, string | undefined]),
        );
        const chosen = (key: string) => values.get(key)?.split(",");
        levelChips.forEach(({ box }, level) => (box.checked = chosen("level")?.includes(levels[level]) ?? true));
        kindChips.forEach(({ box }, kind) => (box.checked = chosen("kind")?.includes(kinds[kind]) ?? kinds[kind] !== "other"));
        reasonSelect.value = values.get("reason") ?? "";
        reasonSelect.selectedIndex = Math.max(0, reasonSelect.selectedIndex);
        nameInput.value = decodeURIComponent(values.get("name") ?? "");
        loopsBox.checked = values.has("loops");
        oftenBox.checked = values.has("often");
        selectedLine = Number([...values.keys()].find(key => /^L\d+$/.test(key))?.slice(1) ?? 0);
    }

    function refresh() {
        applyFilters();
        showCounts();
        targets = findTargets();
        current = -1;
        showStep();
        closePopover();
        chunks.forEach((_, index) => stale.add(index));
        renderVisible();
        renderRestWhenIdle();
        drawMinimap();
        writeHash();
    }

    function followHash() {
        const isFiltered = /[a-z]/.test(location.hash);
        readHash();
        refresh();
        if (selectedLine) code.scrollTop = (selectedLine - 1) * lineHeight - code.clientHeight / 3;
        else if (isFiltered) step(1);
        renderVisible();
    }

    function setKinds(checked: boolean) {
        for (const { box } of kindChips) box.checked = checked;
        refresh();
    }

    function onClick(event: MouseEvent) {
        const target = event.target as HTMLElement;
        if (target.closest("#popover")) return;
        const mark = target.closest<HTMLElement>("[data-o]");
        if (mark) return openPopover(mark, true);
        closePopover();
        const line = target.closest<HTMLElement>(".l");
        const gutter = line && code.getBoundingClientRect().left + Number.parseFloat(getComputedStyle(line, "::before").width);
        if (line && event.clientX < gutter!) selectLine(Number(line.dataset.n));
    }

    function onPointerOver(event: PointerEvent) {
        const target = event.target as HTMLElement;
        const mark = target.closest<HTMLElement>("[data-o]");
        if (mark || target.closest("#popover")) clearTimeout(closing);
        if (mark && !pinned) openPopover(mark, false);
    }

    function onKeyDown(event: KeyboardEvent) {
        if (event.metaKey || event.ctrlKey || event.altKey || (event.target as Element).matches("input[type=search], select")) return;
        if (event.key === "n") step(1);
        else if (event.key === "p") step(-1);
        else if (event.key === "Escape") closePopover();
    }

    indexOperations();
    reasonSelect.append(new Option("typed or not, for any reason", ""), ...reasonOptions);
    linesElement.style.setProperty("--digits", `${String(lines.length).length}`);
    byId("filters").addEventListener("input", refresh);
    byId("filters").addEventListener("submit", event => event.preventDefault());
    byId("all-kinds").addEventListener("click", () => setKinds(true));
    byId("no-kinds").addEventListener("click", () => setKinds(false));
    byId("next").addEventListener("click", () => step(1));
    byId("previous").addEventListener("click", () => step(-1));
    code.addEventListener("scroll", () => (renderVisible(), showViewport()), { passive: true });
    linesElement.addEventListener("click", onClick);
    linesElement.addEventListener("pointerover", onPointerOver);
    linesElement.addEventListener("pointerout", () => {
        if (!pinned) closing = window.setTimeout(closePopover, 200);
    });
    minimap.addEventListener("pointerdown", event => (minimap.setPointerCapture(event.pointerId), scrollToPointer(event)));
    minimap.addEventListener("pointermove", scrollToPointer);
    document.addEventListener("keydown", onKeyDown);
    window.addEventListener("hashchange", followHash);
    matchMedia("(prefers-color-scheme: dark)").addEventListener("change", drawMinimap);
    new ResizeObserver(() => (renderVisible(), drawMinimap())).observe(minimap);
    followHash();
}

function writeFile(path: string, text: string) {
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, text);
}

function writeHtmlReport(directory: string, report: Report) {
    const operationsOf = groupBy(report.operations, operation => operation.file);
    const pathOf = pagePathsOf([...operationsOf.keys()].map(report.shortNameOf));
    const constants = JSON.stringify({ Flag, nameOfFlag, kinds: allKinds, levels: levelNames, letterOfLevel });
    writeFile(join(directory, "index.html"), indexPageOf(report, pathOf));
    writeFile(join(directory, "report.css"), styleSheet);
    writeFile(join(directory, "index.js"), `(${indexPageScript})();\n`);
    writeFile(join(directory, "file.js"), `(${filePageScript})(${constants}, ${oneDecimal}, ${grouped});\n`);
    for (const [file, operations] of operationsOf) {
        const name = report.shortNameOf(file);
        const source = report.sourceOf(file);
        const page = filePageOf(operations, source, report.nameOfReason);
        writeFile(join(directory, pathOf.get(name)!), filePageHtml(name, pathOf.get(name)!, page, source !== undefined));
    }
}

function print(lines: string[]) {
    process.stdout.write(lines.map(line => `${line}\n`).join(""));
}

function main(args: string[]): number {
    if (!args.length || args[0] === "-h" || args[0] === "--help") {
        print([usage]);
        return 0;
    }
    const options = parseOptions(args);
    const nameOfReason = reasonNamer(readReasons(options.reasons));
    const reasonOf = (operation: Operation) => nameOfReason(reasonKeyOf(operation));
    const coverage = readCoverage(options.path);
    const mapOfModule = sourceMapsOfModules(coverage.modules, options.sourceMaps);
    const operations = operationsOf(coverage, mapOfModule);
    if (options.explain) {
        print(explain(operations, options.explain, reasonOf));
        return 0;
    }

    const shortNameOf = fileNameShortener(operations);
    const { rows, allButOther, all } = rowsByKind(operations);
    const files = tally(operations, operation => shortNameOf(operation.file), options.kinds);
    print([
        ...table("Kind of operation", [...rows, ["all but other", allButOther], ["all", all]], 99),
        ...table(`File${inParentheses(options.kinds)}`, worstFirst(files), options.top),
        ...options.by.flatMap(what => breakdownTable(what, operations, options, reasonOf)),
    ]);
    if (options.lcov) {
        writeFileSync(options.lcov, lcovOf(operations));
        print(["", `wrote ${options.lcov}`]);
    }
    const completeness = completenessOf(coverage);
    if (options.html) {
        writeHtmlReport(options.html, {
            title: baseName(options.path),
            operations,
            shortNameOf,
            nameOfReason,
            sourceOf: sourceReader([...mapOfModule.values()]),
            completeness,
            unreached: coverage.unreached,
            top: options.top,
        });
        print(["", `wrote ${join(options.html, "index.html")}`]);
    }
    print([...completenessTable(completeness), ...unreachedLine(coverage.unreached)]);

    const inlineShare = (100 * allButOther[inline]) / operationCount(allButOther);
    if (options.failUnder !== undefined && inlineShare < options.failUnder) {
        print(["", `${oneDecimal(inlineShare)}% is inline, less than ${oneDecimal(options.failUnder)}%`]);
        return 1;
    }
    return 0;
}

process.exitCode = main(process.argv.slice(2));
