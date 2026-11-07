import { engineTypeOf, twoWordScalars } from "./cpp-types.ts";
import { kindText } from "./hash.ts";
import {
  blocks,
  constexprSwitch,
  cppHeader,
  cppSource,
  groupedCases,
  indent,
  indentation,
  inNamespace,
} from "./lines.ts";
import type { Entry, External, Kind, Parameter, PayloadField, Schema, SchemaEnum, SchemaRecord } from "./model.ts";
import { entriesOf, functionLocalIdKinds, innermostKindOf, wireParametersOf } from "./model.ts";
import { cppColumnFunctionName, cppOperandGetterName, lowerFirst } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { wireFieldsOf, wireTypesOf } from "./rust-types.ts";
import { SchemaError, type Location } from "./source.ts";
import { fieldWordsText, hasFixedSize, wordsPerElementOf } from "./wire-layout.ts";

interface Skip {
  readonly tag: "Skip";
  readonly words: number;
}

interface Take {
  readonly tag: "Take";
  readonly statements: readonly string[];
  readonly words: number | null;
}

type Step = Skip | Take;

interface Subject {
  readonly text: string;
  readonly location: Location;
}

interface Cursor {
  readonly enumName: "Opcode" | "TerminatorKind";
  readonly type: "BodyValue" | "BodyTerminator";
  readonly subject: "value" | "terminator";
  readonly tag: "opcode" | "kind";
  readonly reportedValue: string;
  readonly entriesIn: (schema: Schema) => readonly Entry[];
}

interface ExclusiveFlags {
  readonly flagWord: string;
  readonly flags: readonly [string, string];
  readonly error: string;
}

const cursors: readonly Cursor[] = [
  {
    enumName: "Opcode",
    type: "BodyValue",
    subject: "value",
    tag: "opcode",
    reportedValue: "value.id()",
    entriesIn: schema => schema.operations,
  },
  {
    enumName: "TerminatorKind",
    type: "BodyTerminator",
    subject: "terminator",
    tag: "kind",
    reportedValue: "{ }",
    entriesIn: schema => schema.terminators,
  },
];

const exclusiveFlags: readonly ExclusiveFlags[] = [
  { flagWord: "PositionFlags", flags: ["IsExpected", "IsInt32Promise"], error: "InvalidDeclaredType" },
];

function wireEntriesOf(schema: Schema): Entry[] {
  return entriesOf(schema).filter(entry => entry.isWire);
}

function wirePayloadOf(entry: Entry): PayloadField[] {
  return wireParametersOf(entry).flatMap(parameter => (parameter.tag === "Payload" ? [parameter] : []));
}

function externalsOnWire(schema: Schema): External[] {
  const kinds = [
    ...wireTypesOf(schema).flatMap(type => wireFieldsOf(type).map(field => field.kind)),
    ...wireEntriesOf(schema).flatMap(entry => wirePayloadOf(entry).map(field => field.kind)),
  ];
  const named = new Set<External>();
  for (const kind of kinds) {
    const inner = innermostKindOf(kind);
    if (inner.tag === "External") named.add(inner.external);
  }
  return schema.externals.filter(external => named.has(external));
}

function externalTest(external: External): string {
  const type = external.cppType;
  const head = `template<> inline bool isValidExternal<${type}>(uint32_t word, const BuiltinTable&`;
  if (external.form === "id") return `${head} builtins) { return builtins.contains(${type}(word)); }`;
  if (external.below !== null) return `${head}) { return word < ${external.below}; }`;
  return `${head} builtins) { return isValid${type.split("::").at(-1)}(word, builtins); }`;
}

function refuse(subject: Subject, kind: Kind): never {
  throw new SchemaError(subject.location, `${subject.text}: the verifier of bodies cannot check ${kindText(kind)}`);
}

function failureStatement(error: string): string {
  return `words.failAtSubject(InternalErrorKind::${error});`;
}

function takeWord(statement: string): Take {
  return { tag: "Take", statements: [statement], words: 1 };
}

function wordsOf(steps: readonly Step[]): number | null {
  let words = 0;
  for (const step of steps) {
    if (step.words === null) return null;
    words += step.words;
  }
  return words;
}

function requireWords(steps: readonly Step[], expected: number | null, subject: string): void {
  const taken = wordsOf(steps);
  if (taken === expected) return;
  const describe = (words: number | null) => (words === null ? "a counted number of words" : `${words} words`);
  const problem = `the verifier of bodies takes ${describe(taken)}, and the layout has ${describe(expected)}`;
  throw new Error(`${subject}: ${problem}`);
}

function statementsOf(steps: readonly Step[]): string[] {
  const statements: string[] = [];
  let skipped = 0;
  const skipWords = (): void => {
    if (skipped > 0) statements.push(`words.skip(${skipped});`);
    skipped = 0;
  };
  for (const step of steps) {
    if (step.tag === "Skip") {
      skipped += step.words;
      continue;
    }
    skipWords();
    statements.push(...step.statements);
  }
  skipWords();
  return statements;
}

function controlled(head: string, body: readonly string[]): string[] {
  return body.length === 1 ? [head, indentation + body[0]] : [`${head} {`, ...indent(body), "}"];
}

function exclusiveFlagsOf(type: SchemaEnum): ExclusiveFlags | null {
  const exclusive = exclusiveFlags.find(candidate => candidate.flagWord === type.name) ?? null;
  for (const flag of exclusive?.flags ?? []) {
    if (type.members.some(member => member.name === flag)) continue;
    throw new SchemaError(type.location, `flag word ${type.name} has no ${flag}, which the verifier of bodies names`);
  }
  return exclusive;
}

function exclusiveMaskNameOf(type: SchemaEnum): string {
  return `${lowerFirst(type.name)}ThatExcludeEachOther`;
}

function takeFlagWord(type: SchemaEnum): Take {
  const word = `words.template takeFlagWord<${type.name}>()`;
  const exclusive = exclusiveFlagsOf(type);
  if (exclusive === null) return takeWord(`${word};`);
  const mask = exclusiveMaskNameOf(type);
  const statements = controlled(`if ((${word} & ${mask}) == ${mask})`, [failureStatement(exclusive.error)]);
  return { tag: "Take", statements, words: 1 };
}

function stepsOfRecord(record: SchemaRecord, subject: Subject): Step[] {
  if (record.form !== "wire") return refuse(subject, { tag: "Record", record });
  const steps = record.wireFields.flatMap(field =>
    stepsOf(field.kind, field.isOptional, { text: `field ${field.name} of ${record.name}`, location: field.location }),
  );
  requireWords(steps, record.wireWords, `record ${record.name}`);
  return steps;
}

function takeList(element: Kind, subject: Subject): Take {
  const steps = stepsOf(element, false, subject);
  const wordsPerElement = wordsPerElementOf(element);
  requireWords(steps, wordsPerElement, `an element of ${subject.text}`);
  const count = `words.takeCount(${wordsPerElement})`;
  if (steps.every(step => step.tag === "Skip")) {
    const skipped = wordsPerElement === 1 ? count : `${count} * ${wordsPerElement}`;
    return { tag: "Take", statements: [`words.skip(${skipped});`], words: null };
  }
  const head = `for (uint32_t remaining = ${count}; remaining; --remaining)`;
  return { tag: "Take", statements: controlled(head, statementsOf(steps)), words: null };
}

function stepsOf(kind: Kind, isOptional: boolean, subject: Subject): Step[] {
  switch (kind.tag) {
    case "Scalar":
      if (kind.scalar === "Bool") return [takeWord("words.takeBoolean();")];
      if (kind.scalar === "UInt32" || kind.scalar === "Field") return [{ tag: "Skip", words: 1 }];
      return twoWordScalars.has(kind.scalar) ? [{ tag: "Skip", words: 2 }] : refuse(subject, kind);
    case "Id":
      if (kind.id.isEngine || functionLocalIdKinds.has(kind.id.name)) return refuse(subject, kind);
      return [takeWord(`words.template takeID<${engineTypeOf(kind)}>(${isOptional});`)];
    case "Enum":
      return [takeWord(`words.template takeEnumerator<${kind.type.name}>(${isOptional ? "true" : ""});`)];
    case "Flags":
      return [takeFlagWord(kind.type)];
    case "External":
      return [takeWord(`words.template takeExternal<${kind.external.cppType}>(${isOptional});`)];
    case "Record":
      return stepsOfRecord(kind.record, subject);
    case "List":
      return [takeList(kind.element, subject)];
    case "PlacedWord":
    case "Variant":
    case "Span":
      return refuse(subject, kind);
  }
}

function stepsOfParameter(entry: Entry, parameter: Parameter): Step[] {
  if (parameter.tag === "Payload") {
    const subject = { text: `field ${parameter.name} of ${entry.name}`, location: parameter.location };
    return stepsOf(parameter.kind, parameter.isOptional, subject);
  }
  if (parameter.isVariadic) return [{ tag: "Take", statements: ["words.takeVariadicOperands();"], words: null }];
  return [takeWord(`words.takeOperand(${parameter.isOptional});`)];
}

function wordStatementsOf(entry: Entry): string[] {
  if (!entry.isWire) return [failureStatement("EngineOnlyOnWire")];
  const steps = wireParametersOf(entry).flatMap(parameter => stepsOfParameter(entry, parameter));
  requireWords(steps, hasFixedSize(entry) ? Number(fieldWordsText(entry)) : null, entry.name);
  return statementsOf(steps);
}

function wordsFunction(cursor: Cursor, schema: Schema): string[] {
  const { enumName, tag } = cursor;
  const entries = cursor.entriesIn(schema);
  const cases = entries.map(entry => ({ label: entry.name, body: [...wordStatementsOf(entry), "return;"] }));
  return [
    "template<WordsReport report>",
    `ALWAYS_INLINE void verifyFieldWords(CheckedBodyWords<report>& words, ${enumName} ${tag})`,
    "{",
    `${indentation}switch (${tag}) {`,
    ...groupedCases(enumName, cases),
    `${indentation}}`,
    `${indentation}RELEASE_ASSERT_NOT_REACHED();`,
    "}",
  ];
}

function kindsWithin(kind: Kind): Kind[] {
  if (kind.tag === "List") return kindsWithin(kind.element);
  if (kind.tag === "Record") return kind.record.wireFields.flatMap(field => kindsWithin(field.kind));
  return [kind];
}

function kindsInBodies(schema: Schema): Kind[] {
  return wireEntriesOf(schema).flatMap(entry => wirePayloadOf(entry).flatMap(field => kindsWithin(field.kind)));
}

function assertionsOnIDs(kinds: readonly Kind[]): string[] {
  const types = new Set(kinds.flatMap(kind => (kind.tag === "Id" ? [engineTypeOf(kind)] : [])));
  if (types.size === 0) return [];
  const assertions = Array.from(types, type => {
    const hasOwnTable = `remappedIDKindOf<${type}> != RemappedIDKind::String`;
    return `static_assert(std::is_same_v<${type}, StringID> || ${hasOwnTable});`;
  });
  const note = "// For a kind of id without a table in the units, remappedIDKindOf answers with the table of strings.";
  return [note, ...assertions];
}

function masksOfExclusiveFlags(kinds: readonly Kind[]): string[] {
  const flagWords = new Set(kinds.flatMap(kind => (kind.tag === "Flags" ? [kind.type] : [])));
  return [...flagWords].flatMap(type => {
    const exclusive = exclusiveFlagsOf(type);
    if (exclusive === null) return [];
    const flags = exclusive.flags.map(flag => `${type.name}::${flag}`).join(", ");
    return [`inline constexpr uint32_t ${exclusiveMaskNameOf(type)} = OptionSet<${type.name}> { ${flags} }.toRaw();`];
  });
}

function columnFunctionOf(schema: Schema, column: string): string {
  if (schema.entryColumns.some(candidate => candidate.name === column)) return cppColumnFunctionName(column);
  throw new Error(`the schema has no column ${column}, which the verifier of bodies reads`);
}

function ruleChecker(schema: Schema): string[] {
  return `class BodyFieldRuleChecker {
public:
    BodyFieldRuleChecker(BodyVerifier& verifier, BlockID block, ValueID value)
        : m_verifier(verifier)
        , m_block(block)
        , m_value(value)
    {
    }

    template<typename Entry>
    void requireColumnsOf(Entry entry)
    {
        const Function& function = m_verifier.function();
        if (!function.flags.containsAll(${columnFunctionOf(schema, "requires")}(entry)))
            fail(InternalErrorKind::FunctionFlagMissing);
        // The model of a built-in is library code too.
        if (${columnFunctionOf(schema, "isIntrinsic")}(entry) && function.role == FunctionRole::Source)
            fail(InternalErrorKind::IntrinsicOutsideLibraryCode);
    }

    void requireExactlyOne(bool hasFirst, bool hasSecond)
    {
        if (hasFirst == hasSecond)
            fail(InternalErrorKind::NotExactlyOneParameter);
    }

    void requireField(LayoutID layout, uint32_t field)
    {
        if (!m_verifier.isValidField(layout, field))
            fail(InternalErrorKind::PayloadOutOfRange);
    }

private:
    void fail(InternalErrorKind kind) { m_verifier.fail(kind, m_block, m_value); }

    BodyVerifier& m_verifier;
    BlockID m_block;
    ValueID m_value;
};`.split("\n");
}

function getterOf(parameter: Parameter): string {
  if (parameter.tag === "Payload") return `operation.${parameter.name}()`;
  return `operation.${cppOperandGetterName(parameter.name, parameter.isVariadic)}()`;
}

function presenceOf(entry: Entry, parameter: Parameter): string {
  const isCounted = parameter.tag === "Operand" ? parameter.isVariadic : parameter.kind.tag === "List";
  if (parameter.isEngine || isCounted || !parameter.isOptional) {
    const problem = `${parameter.name} must be an optional wire parameter that is neither a list nor variadic`;
    throw new SchemaError(parameter.location, `exactlyOneOf of ${entry.name}: ${problem}`);
  }
  return `!!${getterOf(parameter)}`;
}

function ruleOfExclusiveParameters(entry: Entry): string[] {
  const value = entry.columns.get("exactlyOneOf");
  if (value?.tag !== "Parameters") return [];
  return [`rules.requireExactlyOne(${value.parameters.map(parameter => presenceOf(entry, parameter)).join(", ")});`];
}

function layoutBeside(entry: Entry, field: PayloadField): PayloadField {
  const layouts = wirePayloadOf(entry).filter(
    ({ kind, isOptional }) => kind.tag === "Id" && kind.id.name === "Layout" && !isOptional,
  );
  if (layouts.length === 1) return layouts[0];
  const problem = `a Field needs exactly one required Layout in the same entry, and this entry has ${layouts.length}`;
  throw new SchemaError(field.location, `field ${field.name} of ${entry.name}: ${problem}`);
}

function rulesOfFieldNumbers(entry: Entry): string[] {
  return wirePayloadOf(entry).flatMap(field => {
    const inner = innermostKindOf(field.kind);
    if (inner.tag !== "Scalar" || inner.scalar !== "Field") return [];
    const layout = getterOf(layoutBeside(entry, field));
    const getter = getterOf(field);
    if (field.kind.tag === "List")
      return controlled(`for (uint32_t field : ${getter})`, [`rules.requireField(${layout}, field);`]);
    if (field.isOptional) return controlled(`if (${getter})`, [`rules.requireField(${layout}, *${getter});`]);
    return [`rules.requireField(${layout}, ${getter});`];
  });
}

function fieldRulesOf(entry: Entry): string[] {
  return entry.isWire ? [...ruleOfExclusiveParameters(entry), ...rulesOfFieldNumbers(entry)] : [];
}

function rulesFunction(cursor: Cursor, schema: Schema): string[] {
  const { enumName, type, subject, tag, reportedValue } = cursor;
  const entries = cursor.entriesIn(schema).map(entry => ({ entry, rules: fieldRulesOf(entry) }));
  const casesWithRules = entries.flatMap(({ entry, rules }) => {
    if (rules.length === 0) return [];
    const operation = `auto operation = ${subject}.as<${enumName}::${entry.name}>();`;
    return [`case ${enumName}::${entry.name}: {`, ...indent([operation, ...rules, "return;"]), "}"];
  });
  const otherCases = entries
    .filter(({ rules }) => rules.length === 0)
    .map(({ entry }) => ({ label: entry.name, body: [entry.isWire ? "return;" : "RELEASE_ASSERT_NOT_REACHED();"] }));
  return [
    `void verifyFieldRules(BodyVerifier& verifier, const BodyBlock& block, const ${type}& ${subject})`,
    "{",
    ...indent([
      `BodyFieldRuleChecker rules(verifier, block.id(), ${reportedValue});`,
      `rules.requireColumnsOf(${subject}.${tag}());`,
      `switch (${subject}.${tag}()) {`,
      ...casesWithRules,
    ]),
    ...groupedCases(enumName, otherCases),
    ...indent(["}", "RELEASE_ASSERT_NOT_REACHED();"]),
    "}",
  ];
}

function operandKindsFunction(cursor: Cursor, schema: Schema): string[] {
  const { enumName, tag } = cursor;
  const cases = cursor.entriesIn(schema).map(entry => {
    const operands =
      entry.variadicOperand === null ? entry.namedOperands : [...entry.namedOperands, entry.variadicOperand];
    const numbers = new Map(operands.map((operand): [string, number] => [operand.kind.name, operand.kind.number]));
    const kinds = [...numbers].sort(([, left], [, right]) => left - right).map(([name]) => `OperandKind::${name}`);
    return { label: entry.name, value: kinds.length === 0 ? "{ }" : `{ ${kinds.join(", ")} }` };
  });
  const signature = `OperandKindSet kindsOfOperandsOf(${enumName} ${tag})`;
  return [
    "// Of all the operands that the schema gives the entry, the engine's among them.",
    ...constexprSwitch({ signature, subject: tag, enumName, cases, defaultValue: "most common" }),
  ];
}

function requiresFunctionFlags(entry: Entry): boolean {
  const value = entry.columns.get("requires");
  return value?.tag === "Members" && value.members.length > 0;
}

function rulesPredicate(cursor: Cursor, schema: Schema): string[] {
  const { enumName, tag } = cursor;
  const cases = cursor.entriesIn(schema).map(entry => ({
    label: entry.name,
    value: String(!entry.isWire || requiresFunctionFlags(entry) || fieldRulesOf(entry).length > 0),
  }));
  const signature = `bool hasFieldRulesInEveryFunction(${enumName} ${tag})`;
  return [
    "// No: verifyFieldRules() finds no fault with the entry, unless it is an intrinsic outside library code.",
    ...constexprSwitch({ signature, subject: tag, enumName, cases, defaultValue: "false" }),
  ];
}

function takenByTargetsOf(entry: Entry): string {
  const taken = new Set(
    entry.targets.map(target => (target.takesProofs ? "Proofs" : target.results.length > 0 ? "Results" : "Nothing")),
  );
  const isVariadicBehindFirst = entry.targets.some(target => target.isVariadic && target.index > 0);
  if (isVariadicBehindFirst || taken.size > 1 || taken.has("Results")) return "TakenByTargets::Differs";
  return taken.has("Proofs") ? "TakenByTargets::Proofs" : "TakenByTargets::Nothing";
}

function targetsFunction(schema: Schema): string[] {
  const cases = schema.terminators.map(entry => ({ label: entry.name, value: takenByTargetsOf(entry) }));
  const signature = "TakenByTargets takenByTargetsOf(TerminatorKind kind)";
  return constexprSwitch({ signature, subject: "kind", enumName: "TerminatorKind", cases, defaultValue: null });
}

function verifierHeader(schema: Schema): string[] {
  const kinds = kindsInBodies(schema);
  const definitions = blocks(
    externalsOnWire(schema).map(externalTest),
    assertionsOnIDs(kinds),
    masksOfExclusiveFlags(kinds),
    ...cursors.map(cursor => wordsFunction(cursor, schema)),
    ...cursors.map(cursor => operandKindsFunction(cursor, schema)),
    ...cursors.map(cursor => rulesPredicate(cursor, schema)),
    targetsFunction(schema),
  );
  return cppHeader(["<type_traits>"], inNamespace("Typed", definitions));
}

function verifierSource(schema: Schema): string[] {
  const definitions = blocks(ruleChecker(schema), ...cursors.map(cursor => rulesFunction(cursor, schema)));
  return cppSource(['"AOTBodyVerifierInternal.h"'], inNamespace("Typed", definitions));
}

export function emitCppBodyVerifier(context: EmitContext): OutputFile[] {
  return [
    { name: "AOTSchemaBodyVerifier.h", lines: verifierHeader(context.schema) },
    { name: "AOTSchemaBodyVerifier.cpp", lines: verifierSource(context.schema) },
  ];
}
