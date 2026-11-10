import { defaultWordsOf, listFirstWord, namedExternalMemberOf, payloadTypesOf } from "./cpp-types.ts";
import { kindText } from "./hash.ts";
import {
  blocks,
  braced,
  constexprSwitch,
  cppHeader,
  cppLiteral,
  cppSource,
  inNamespace,
  indentation,
  macroLines,
} from "./lines.ts";
import type {
  Entry,
  EntryColumn,
  EntryColumnValue,
  Field,
  Flag,
  Kind,
  Member,
  Operand,
  PayloadField,
  ResultRule,
  Schema,
  SchemaEnum,
  SchemaType,
  Target,
  Variant,
  VariantCase,
} from "./model.ts";
import { entriesOf, fieldsOf, isLegalIn, payloadWordsOf, wireFlagsOf } from "./model.ts";
import { cppColumnFunctionName, cppFamilyMacroName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";

type EntryEnumName = "Opcode" | "TerminatorKind";

type EntryEmitter = (entries: readonly Entry[], enumName: EntryEnumName, parameter: string) => string[];

type EntryContents = readonly (readonly [Entry, readonly string[]])[];

interface SharedArrays {
  readonly declarations: readonly string[];
  readonly byEntry: ReadonlyMap<Entry, string>;
}

interface Parts {
  readonly array: string;
  readonly count: number;
}

const wirePredicates =
  `constexpr bool isWire(Opcode opcode) { return static_cast<unsigned>(opcode) < numberOfWireOpcodes; }
constexpr bool isWire(TerminatorKind kind) { return static_cast<unsigned>(kind) < numberOfWireTerminatorKinds; }`.split(
    "\n",
  );

const columnTable = `// What find() answers for each member, in one table that every caller reads. Of a number that is no member's find() itself is asked.
template<typename E, typename T, T (*find)(E)>
struct ColumnTable {
    static constexpr std::array<T, numberOfEnumerators<E>> tabulate()
    {
        std::array<T, numberOfEnumerators<E>> values { };
        for (E member : allMembersOf<E>)
            values[static_cast<size_t>(member)] = find(member);
        return values;
    }

    static constexpr std::array<T, numberOfEnumerators<E>> values = tabulate();

    static ALWAYS_INLINE constexpr T at(E member)
    {
        size_t index = static_cast<size_t>(member);
        if (index < values.size()) [[likely]]
            return values[index];
        return find(member);
    }
};`.split("\n");

const noteOnStandingInCode =
  "// By the schema alone: the entry is legal in the form Placed. Code::append() also turns away opcodesThatStateTypes and opcodesWithoutEmitter().";

const noteOnUnknownResults =
  "// Of results(..): bit n is set where the schema writes `unknown` for result n, whatever it writes behind `=>`. The last rule of an open list speaks for every later result, and bit 31 for those from 31 on.";

const numberOfBitsForUnknownResults = 32;

const noteOnUsesOfNamedOperands = "// OperandInfo::uses of every operand but the variadic one, united.";

const noteOnTargetsTakingProofs = "// Bit n is set where TargetInfo::takesProofs of targetInfosOf(kind)[n] is.";

const numberOfBitsForTargets = 31;

function unknownResults(entry: Entry): string {
  const rule = entry.result.unproven;
  if (rule.rule !== "Results") return "0x0";
  const isUnknown = rule.results.map(result => result.unproven.rule === "Unknown");
  if (isUnknown.length > numberOfBitsForUnknownResults)
    throw new Error(`${entry.name} lists more results than unknownResultsOf() has bits`);
  const isUnknownBehindList = rule.isOpen && isUnknown.at(-1) === true;
  let mask = 0n;
  for (let index = 0; index < numberOfBitsForUnknownResults; ++index) {
    if (index < isUnknown.length ? isUnknown[index] : isUnknownBehindList) mask |= 1n << BigInt(index);
  }
  return `0x${mask.toString(16)}`;
}

function entryInfoDeclarations(schema: Schema): string[] {
  const externals = ["None", ...schema.externals.map(external => external.name)];
  return `JS_EXPORT_PRIVATE std::span<const uint32_t> defaultPayloadOf(TerminatorKind);
// By the number of the opcode: the words that a fresh draft has, and how many named operands.
inline constexpr unsigned numberOfFreshPayloadWordsAtHand = 12;
struct FreshWordsOfOperation {
    const uint32_t* payload { nullptr };
    uint16_t numberOfPayloadWords { 0 };
    uint16_t numberOfNamedOperands { 0 };
    std::array<uint32_t, numberOfFreshPayloadWordsAtHand> firstPayloadWords { };
};
JS_EXPORT_PRIVATE extern const std::array<FreshWordsOfOperation, numberOfEnumerators<Opcode>> freshWordsByOpcode;
ALWAYS_INLINE const FreshWordsOfOperation& freshWordsOf(Opcode opcode) { return freshWordsByOpcode[static_cast<unsigned>(opcode)]; }
ALWAYS_INLINE std::span<const uint32_t> defaultPayloadOf(Opcode opcode) { return { freshWordsOf(opcode).payload, freshWordsOf(opcode).numberOfPayloadWords }; }

struct OperandInfo {
    ASCIILiteral name;
    OperandKind kind { OperandKind::Any };
    bool isOptional { false };
    bool isVariadic { false };
    bool isEngineOnly { false };
    OperandUses uses;
    Protocols protocols;
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    std::optional<uint8_t> retainingOperand;
    std::optional<uint8_t> assignedFromOperand;
    std::span<const BuiltinName> namedMembers;
};

// By the number of the entry: where its operands are described, the variadic one last.
struct OperandInfosOfEntry {
    const OperandInfo* first { nullptr };
    uint32_t size { 0 };
};
JS_EXPORT_PRIVATE extern const std::array<OperandInfosOfEntry, numberOfEnumerators<Opcode>> operandInfosByOpcode;
JS_EXPORT_PRIVATE extern const std::array<OperandInfosOfEntry, numberOfEnumerators<TerminatorKind>> operandInfosByTerminatorKind;

ALWAYS_INLINE const OperandInfo& operandInfoIn(const OperandInfosOfEntry& infos, uint32_t operandIndex)
{
    if (operandIndex < infos.size) [[likely]]
        return infos.first[operandIndex];
    RELEASE_ASSERT(infos.size && infos.first[infos.size - 1].isVariadic);
    return infos.first[infos.size - 1];
}
ALWAYS_INLINE const OperandInfo& operandInfoOf(Opcode opcode, uint32_t operandIndex) { return operandInfoIn(operandInfosByOpcode[static_cast<unsigned>(opcode)], operandIndex); }
ALWAYS_INLINE const OperandInfo& operandInfoOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoIn(operandInfosByTerminatorKind[static_cast<unsigned>(kind)], operandIndex); }

ALWAYS_INLINE OperandKind operandKindOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).kind; }
inline OperandKind operandKindOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).kind; }
inline OperandUses operandUsesOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).uses; }
inline OperandUses operandUsesOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).uses; }
inline Protocols protocolsOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).protocols; }
inline Protocols protocolsOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).protocols; }
inline std::span<const BuiltinName> namedMembersOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).namedMembers; }
inline std::span<const BuiltinName> namedMembersOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).namedMembers; }
inline RetainedIn retainedInOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).retainedIn; }
inline RetainedIn retainedInOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).retainedIn; }
inline std::optional<uint8_t> retainingOperandOf(Opcode opcode, uint32_t operandIndex) { return operandInfoOf(opcode, operandIndex).retainingOperand; }
inline std::optional<uint8_t> retainingOperandOf(TerminatorKind kind, uint32_t operandIndex) { return operandInfoOf(kind, operandIndex).retainingOperand; }

// The externals of AOTSchema.def in the order in which they are declared.
${braced("enum class PayloadExternal : uint8_t", externals).join("\n")}

struct PayloadFieldInfo {
    ASCIILiteral name;
    PayloadKind kind { PayloadKind::UInt32 };
    PayloadKind elementKind { PayloadKind::UInt32 };
    uint8_t firstWord { 0 };
    uint8_t numberOfWords { 0 };
    bool isOptional { false };
    bool isEngineOnly { false };

    // The members below speak of the value, and of one element where the field is a list.
    PayloadExternal external { PayloadExternal::None };
    uint8_t numberOfWordsOfValue { 0 };
    uint8_t numberOfParts { 0 };
    // Of a record: its fields in declared order. The firstWord of a part counts from the first word of the record.
    // Of a variant: one part a case, in the order of Kind: a Record with the name of the case, firstWord 1, the words of its fields, and its fields as parts.
    const PayloadFieldInfo* firstPart { nullptr };
    // Where the kind is Enum: the names of the members by number. Where it is Flags: by bit.
    std::span<const ASCIILiteral> memberNames;

    std::span<const PayloadFieldInfo> parts() const { return unsafeMakeSpan(firstPart, numberOfParts); }
};

JS_EXPORT_PRIVATE std::span<const PayloadFieldInfo> payloadFieldsOf(Opcode);
JS_EXPORT_PRIVATE std::span<const PayloadFieldInfo> payloadFieldsOf(TerminatorKind);
JS_EXPORT_PRIVATE std::span<const ASCIILiteral> flagNamesOf(Opcode);
JS_EXPORT_PRIVATE std::span<const ASCIILiteral> flagNamesOf(TerminatorKind);

struct TargetInfo {
    ASCIILiteral name;
    bool isVariadic { false };
    bool takesProofs { false };
    uint8_t numberOfResults { 0 };
};

JS_EXPORT_PRIVATE std::span<const TargetInfo> targetInfosOf(TerminatorKind);`.split("\n");
}

const declaredPosition = `constexpr unsigned declaredPosition(std::optional<unsigned> position)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(position);
    return *position;
}

// What find() answers for each member, in one table that every caller reads.
template<typename E, std::optional<unsigned> (*find)(E)>
struct PositionTable {
    static constexpr uint8_t absent = UINT8_MAX;

    static constexpr std::array<uint8_t, numberOfEnumerators<E>> tabulate()
    {
        std::array<uint8_t, numberOfEnumerators<E>> positions { };
        for (E member : allMembersOf<E>) {
            std::optional<unsigned> position = find(member);
            RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(!position || *position < absent);
            positions[static_cast<size_t>(member)] = position ? static_cast<uint8_t>(*position) : absent;
        }
        return positions;
    }

    static constexpr std::array<uint8_t, numberOfEnumerators<E>> positions = tabulate();

    static constexpr std::optional<unsigned> at(E member)
    {
        size_t index = static_cast<size_t>(member);
        uint8_t position = index < positions.size() ? positions[index] : absent;
        return position == absent ? std::nullopt : std::optional<unsigned>(position);
    }
};

// Which case of a switch each member takes, in one table that every copy of the switch reads.
template<typename E, uint8_t (*find)(E)>
struct CaseTable {
    static constexpr size_t numberOfPlaces = std::bit_ceil<size_t>(numberOfEnumerators<E>);

    static constexpr std::array<uint8_t, numberOfPlaces> tabulate()
    {
        std::array<uint8_t, numberOfPlaces> cases { };
        cases.fill(std::numeric_limits<uint8_t>::max());
        for (E member : allMembersOf<E>)
            cases[static_cast<size_t>(member)] = find(member);
        return cases;
    }

    static constexpr std::array<uint8_t, numberOfPlaces> cases = tabulate();

    static uint8_t at(E member) { return cases[static_cast<size_t>(member) % numberOfPlaces]; }
};`.split("\n");

const positionIncludes = `#include "AOTSchemaFlagMasks.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSchemaOperationColumns.h"
#include "AOTSchemaPayloadPositions.h"
#include "AOTSchemaRuntimeColumns.h"
#include "AOTSchemaVariadicOperandPositions.h"`.split("\n");

const replacedEntryDeclarations = `JS_EXPORT_PRIVATE std::span<const Opcode> replacedOperationsOf(Opcode);
JS_EXPORT_PRIVATE std::span<const Opcode> replacedOperationsOf(TerminatorKind);
JS_EXPORT_PRIVATE std::span<const TerminatorKind> replacedTerminatorsOf(Opcode);
JS_EXPORT_PRIVATE std::span<const TerminatorKind> replacedTerminatorsOf(TerminatorKind);`.split("\n");

function forOperations(schema: Schema, emit: EntryEmitter): string[] {
  return emit(schema.operations, "Opcode", "opcode");
}

function forTerminators(schema: Schema, emit: EntryEmitter): string[] {
  return emit(schema.terminators, "TerminatorKind", "kind");
}

function both(schema: Schema, emit: EntryEmitter): string[] {
  return blocks(forOperations(schema, emit), forTerminators(schema, emit));
}

const leastLabelsOfColumnTable = 16;

function nameOfSwitchBehindTable(name: string): string {
  return name.endsWith("Of") ? `${name.slice(0, -"Of".length)}InSchemaOf` : `${name}InSchema`;
}

function lookup(
  type: string,
  name: string,
  valueOf: (entry: Entry) => string,
  defaultValue: string | null,
): EntryEmitter {
  return (entries, enumName, parameter) => {
    const cases = entries.map(entry => ({ label: entry.name, value: valueOf(entry) }));
    if (defaultValue !== null && cases.every(({ value }) => value === defaultValue))
      return [`constexpr ${type} ${name}(${enumName}) { return ${defaultValue}; }`];
    const sizesOfGroups = [...Map.groupBy(cases, ({ value }) => value).values()].map(group => group.length);
    if (cases.length - Math.max(...sizesOfGroups) < leastLabelsOfColumnTable) {
      const signature = `${type} ${name}(${enumName} ${parameter})`;
      return constexprSwitch({ signature, subject: parameter, enumName, cases, defaultValue, qualifier: "ALWAYS_INLINE constexpr" });
    }
    const inSchema = nameOfSwitchBehindTable(name);
    const signature = `${type} ${inSchema}(${enumName} ${parameter})`;
    const table = `ColumnTable<${enumName}, ${type}, ${inSchema}>`;
    return blocks(constexprSwitch({ signature, subject: parameter, enumName, cases, defaultValue }), [
      `ALWAYS_INLINE constexpr ${type} ${name}(${enumName} ${parameter}) { return ${table}::at(${parameter}); }`,
    ]);
  };
}

function operandsOf(entry: Entry): Operand[] {
  return entry.variadicOperand === null ? [...entry.namedOperands] : [...entry.namedOperands, entry.variadicOperand];
}

function initializer(parts: readonly (string | number)[]): string {
  return `{ ${parts.join(", ")} }`;
}

function entryMacrosHeader(schema: Schema): string[] {
  const namesOf = (entries: readonly Entry[]) => entries.map(entry => entry.name);
  const familyMacros = (prefix: "OPCODE" | "TERMINATOR_KIND", entries: readonly Entry[]) =>
    schema.families
      .map(family => ({ family, members: namesOf(entries.filter(entry => entry.family === family)) }))
      .filter(({ members }) => members.length > 0)
      .map(({ family, members }) => macroLines(cppFamilyMacroName(prefix, family), members));
  return cppHeader(
    [],
    blocks(
      macroLines("FOR_EACH_AOT_OPCODE", namesOf(schema.operations)),
      ...familyMacros("OPCODE", schema.operations),
      macroLines("FOR_EACH_AOT_TERMINATOR_KIND", namesOf(schema.terminators)),
      ...familyMacros("TERMINATOR_KIND", schema.terminators),
      macroLines("FOR_EACH_AOT_OPERATION_FAMILY", schema.families),
    ),
  );
}

function operandIndexesHeader(schema: Schema): string[] {
  const namespaces = entriesOf(schema).flatMap(entry => {
    const constants = operandsOf(entry).map(operand => `inline constexpr uint32_t ${operand.name} = ${operand.index};`);
    return constants.length === 0 ? [] : [`namespace ${entry.name} { ${constants.join(" ")} }`];
  });
  return cppHeader(
    ["<cstdint>"],
    [
      "namespace JSC { namespace AOT { namespace Typed { namespace OperandIndex {",
      "",
      ...namespaces,
      "",
      "} } } } // namespace JSC::AOT::Typed::OperandIndex",
    ],
  );
}

function operationsHeader(schema: Schema): string[] {
  const family = (entry: Entry) => `OperationFamily::${entry.family}`;
  const mayStandInCode = (entry: Entry) => truth(isLegalIn(entry, "Placed"));
  const rule = (result: ResultRule) => `ResultRule::${result.rule}`;
  const provenRule = (entry: Entry) => (entry.result.proven === null ? "std::nullopt" : rule(entry.result.proven));
  const effectRule = (entry: Entry) => `EffectRule::${entry.effectRule}`;
  const namedOperandCount = (entry: Entry) => String(entry.namedOperands.length);
  const usesOfNamedOperands = (entry: Entry) => {
    const uses = new Map<string, Member>();
    for (const operand of entry.namedOperands) {
      for (const use of operand.uses.uses) uses.set(use.name, use);
    }
    const inOrder = [...uses.values()].sort((left, right) => left.number - right.number);
    return `OperandUses ${setText("OperandUse", inOrder)}`;
  };
  const unitedOverOperands =
    (type: string, enumName: string, membersOf: (operand: Operand) => readonly Member[]) => (entry: Entry) => {
      const united = new Map<string, Member>();
      for (const operand of operandsOf(entry)) {
        for (const member of membersOf(operand)) united.set(member.name, member);
      }
      const inOrder = [...united.values()].sort((left, right) => left.number - right.number);
      return `${type} ${setText(enumName, inOrder)}`;
    };
  const usesOfOperands = unitedOverOperands("OperandUses", "OperandUse", operand => operand.uses.uses);
  const protocolsOfOperands = unitedOverOperands("Protocols", "Protocol", operand => operand.uses.protocols);
  const flagMask = (flags: readonly Flag[]) => `0x${((1 << flags.length) - 1).toString(16)}`;
  const fixedTargets = (entry: Entry) =>
    entry.targets.some(target => target.isVariadic) ? "std::nullopt" : String(entry.targets.length);
  const targetsTakingProofs = (entry: Entry) => {
    if (entry.targets.length > numberOfBitsForTargets)
      throw new Error(`${entry.name} lists more targets than targetsTakingProofsOf() has bits`);
    const mask = entry.targets.reduce((bits, target, index) => (target.takesProofs ? bits | (1 << index) : bits), 0);
    return `0x${mask.toString(16)}`;
  };
  const declarations = blocks(
    wirePredicates,
    columnTable,
    [noteOnStandingInCode, ...both(schema, lookup("bool", "mayStandInCode", mayStandInCode, null))],
    both(schema, lookup("OperationFamily", "familyOf", family, "most common")),
    forOperations(
      schema,
      lookup("ResultRule", "resultRuleOf", entry => rule(entry.result.unproven), "most common"),
    ),
    forOperations(schema, lookup("std::optional<ResultRule>", "provenResultRuleOf", provenRule, "std::nullopt")),
    [noteOnUnknownResults, ...forOperations(schema, lookup("uint32_t", "unknownResultsOf", unknownResults, "0x0"))],
    both(schema, lookup("EffectRule", "effectRuleOf", effectRule, "EffectRule::Declared")),
    both(schema, lookup("uint32_t", "numberOfNamedOperandsOf", namedOperandCount, "most common")),
    both(
      schema,
      lookup("bool", "hasVariadicOperand", entry => truth(entry.variadicOperand !== null), "false"),
    ),
    [
      noteOnUsesOfNamedOperands,
      ...forOperations(schema, lookup("OperandUses", "usesOfNamedOperandsOf", usesOfNamedOperands, "OperandUses { }")),
    ],
    both(schema, lookup("OperandUses", "usesOfOperandsOf", usesOfOperands, "OperandUses { }")),
    both(schema, lookup("Protocols", "protocolsOfOperandsOf", protocolsOfOperands, "Protocols { }")),
    both(
      schema,
      lookup("uint16_t", "declaredFlagsOf", entry => flagMask(entry.flags), "0x0"),
    ),
    both(
      schema,
      lookup("uint16_t", "wireFlagsOf", entry => flagMask(wireFlagsOf(entry)), "0x0"),
    ),
    entryInfoDeclarations(schema),
    forTerminators(schema, lookup("std::optional<uint32_t>", "numberOfFixedTargetsOf", fixedTargets, null)),
    [noteOnTargetsTakingProofs, ...forTerminators(schema, lookup("uint32_t", "targetsTakingProofsOf", targetsTakingProofs, "0x0"))],
    declaredPosition,
  );
  const includes = [
    '"AOTBuiltinIDTypes.h"',
    '"AOTSchemaEntryMacros.h"',
    '"AOTSchemaEnums.h"',
    '"AOTSchemaOperandIndexes.h"',
    "<algorithm>",
    "<array>",
    "<bit>",
    "<limits>",
    "<optional>",
    "<span>",
    "<wtf/StdLibExtras.h>",
  ];
  return cppHeader(includes, [...inNamespace("Typed", declarations), "", ...positionIncludes]);
}

function columnFunctions(schema: Schema, column: EntryColumn): string[] {
  const kind = column.kind;
  const functions = (type: string, unset: string, textOf: (value: EntryColumnValue) => string | null) => {
    const valueOf = (entry: Entry) => {
      const value = entry.columns.get(column.name);
      return (value === undefined ? null : textOf(value)) ?? unset;
    };
    return both(schema, lookup(type, cppColumnFunctionName(column.name), valueOf, unset));
  };
  switch (kind.tag) {
    case "Bool":
      return functions("bool", "false", value => (value.tag === "Bool" ? truth(value.value) : null));
    case "UInt32":
      return functions("uint32_t", "0", value => (value.tag === "UInt32" ? String(value.value) : null));
    case "Operand":
      return functions("std::optional<uint8_t>", "std::nullopt", value =>
        value.tag === "Operand" ? String(value.operand.index) : null,
      );
    case "RuntimeOperation":
      return functions("std::optional<RuntimeOperation>", "std::nullopt", value =>
        value.tag === "RuntimeOperation" ? `RuntimeOperation::${value.operation.name}` : null,
      );
    case "Enum": {
      const type = kind.type.name;
      return functions(type, `${type}::${kind.type.members[0].name}`, value =>
        value.tag === "Member" ? `${type}::${value.member.name}` : null,
      );
    }
    case "EnumSet": {
      const element = kind.type.name;
      return functions(`EnumeratorBitSet<${element}, numberOfEnumerators<${element}>>`, "{ }", value =>
        value.tag === "Members" ? setText(element, value.members) : null,
      );
    }
    case "Flags": {
      const flagWord = kind.type.name;
      const type = `OptionSet<${flagWord}>`;
      return functions(type, `${type} ${setText(flagWord, [])}`, value =>
        value.tag === "Members" ? `${type} ${setText(flagWord, value.members)}` : null,
      );
    }
    case "Operations":
      return replacedEntryDeclarations;
    case "Parameters":
      return [];
  }
}

function namesRuntimeOperation(column: EntryColumn): boolean {
  return column.kind.tag === "RuntimeOperation";
}

function columnsHeader(schema: Schema, columns: readonly EntryColumn[]): string[] {
  const functions = columns.map(column => columnFunctions(schema, column));
  return cppHeader([], inNamespace("Typed", blocks(...functions)));
}

function staticArray(elementType: string, name: string, elements: readonly string[]): string[] {
  if (elements.length === 0) return [];
  const head = `static constexpr ${elementType} ${name}[] =`;
  if (elements[0].startsWith("{")) return [`${head} {`, ...elements.map(element => `${indentation}${element},`), "};"];
  return braced(head, elements);
}

function sharedArrays(elementType: string, prefix: string, contents: EntryContents): SharedArrays {
  const names = new Map<string, string>();
  const declarations: string[] = [];
  const byEntry = new Map<Entry, string>();
  for (const [entry, elements] of contents) {
    if (elements.length === 0) continue;
    const key = elements.join("\n");
    let name = names.get(key);
    if (name === undefined) {
      name = `${prefix}${names.size}`;
      names.set(key, name);
      declarations.push(...staticArray(elementType, name, elements));
    }
    byEntry.set(entry, name);
  }
  return { declarations, byEntry };
}

function spanFunction(
  signature: string,
  subject: string,
  enumName: EntryEnumName,
  entries: readonly Entry[],
  byEntry: ReadonlyMap<Entry, string>,
): string[] {
  return constexprSwitch({
    signature,
    subject,
    enumName,
    cases: entries.map(entry => ({ label: entry.name, value: byEntry.get(entry) ?? "{ }" })),
    defaultValue: "{ }",
    qualifier: "",
  });
}

function operandUses(operand: Operand): string {
  const { uses, protocols, retainedIn, retainingOperand, assignedFromOperand } = operand.uses;
  return [
    setText("OperandUse", uses),
    setText("Protocol", protocols),
    `RetainedIn::${retainedIn}`,
    retainingOperand ?? "std::nullopt",
    assignedFromOperand ?? "std::nullopt",
  ].join(", ");
}

function setText(enumName: string, members: readonly Member[]): string {
  return members.length === 0 ? "{ }" : initializer(members.map(member => `${enumName}::${member.name}`));
}

function truth(value: boolean): string {
  return value ? "true" : "false";
}

function operandInfo(operand: Operand, namedMembers: string): string {
  return initializer([
    cppLiteral(operand.name),
    `OperandKind::${operand.kind.name}`,
    truth(operand.isOptional),
    truth(operand.isVariadic),
    truth(operand.isEngine),
    operandUses(operand),
    namedMembers,
  ]);
}

function valueKindOf(kind: Kind): Kind {
  return kind.tag === "List" ? kind.element : kind;
}

function payloadKindOf(schema: Schema, kind: Kind): string {
  const ownName = kind.tag === "Id" ? kind.id.name : kindText(kind);
  const members = schema.enums.find(type => type.name === "PayloadKind")?.members ?? [];
  const name = [ownName, kind.tag].find(candidate => members.some(member => member.name === candidate));
  if (name === undefined) throw new Error(`${kindText(kind)} stands in a payload, and PayloadKind has no member for it`);
  return name;
}

function spelledEnumOf(schema: Schema, kind: Kind): SchemaEnum | null {
  const value = valueKindOf(kind);
  if (value.tag !== "Enum" && value.tag !== "Flags") return null;
  return payloadKindOf(schema, value) === value.tag ? value.type : null;
}

function partsOf(value: Kind): Parts {
  if (value.tag === "Record") return { array: `partsOf${value.record.name}`, count: value.record.fields.length };
  if (value.tag === "Variant") return { array: `casesOf${value.variant.name}`, count: value.variant.cases.length };
  return { array: "nullptr", count: 0 };
}

function partsOfCase(variant: Variant, variantCase: VariantCase): Parts {
  return { array: `partsOf${variant.name}${variantCase.name}`, count: variantCase.fields.length };
}

function partsInfo({ array, count }: Parts): (string | number)[] {
  return [count, count === 0 ? "nullptr" : array];
}

function valueInfo(schema: Schema, kind: Kind): (string | number)[] {
  const value = valueKindOf(kind);
  const spelled = spelledEnumOf(schema, value);
  return [
    `PayloadExternal::${value.tag === "External" ? value.external.name : "None"}`,
    payloadWordsOf(value),
    ...partsInfo(partsOf(value)),
    spelled === null ? "{ }" : `memberNamesOf${spelled.name}`,
  ];
}

function payloadFieldInfo(schema: Schema, entry: Entry, field: PayloadField): string {
  const kind = field.payloadKind.name;
  const elementKind = field.elementPayloadKind?.name ?? kind;
  return initializer([
    cppLiteral(field.name),
    `PayloadKind::${kind}`,
    `PayloadKind::${elementKind}`,
    field.firstWord ?? listFirstWord(entry),
    field.kind.tag === "List" ? 0 : field.payloadWords,
    truth(field.isOptional),
    truth(field.isEngine),
    ...valueInfo(schema, field.kind),
  ]);
}

function partInfos(schema: Schema, fields: readonly Field[]): string[] {
  let firstWord = 0;
  return fields.map(field => {
    const kind = `PayloadKind::${payloadKindOf(schema, field.kind)}`;
    const info = initializer([
      cppLiteral(field.name),
      kind,
      kind,
      firstWord,
      field.payloadWords,
      truth(field.isOptional),
      truth(field.isEngine),
      ...valueInfo(schema, field.kind),
    ]);
    firstWord += field.payloadWords;
    return info;
  });
}

function caseInfo(variant: Variant, variantCase: VariantCase): string {
  const words = variantCase.fields.reduce((sum, field) => sum + field.payloadWords, 0);
  return initializer([
    cppLiteral(variantCase.name),
    "PayloadKind::Record",
    "PayloadKind::Record",
    1,
    words,
    truth(false),
    truth(false),
    "PayloadExternal::None",
    words,
    ...partsInfo(partsOfCase(variant, variantCase)),
    "{ }",
  ]);
}

function partArrays(schema: Schema, type: SchemaType): string[] {
  const array = (parts: Parts, infos: readonly string[]) => staticArray("PayloadFieldInfo", parts.array, infos);
  if (type.tag === "Record") return array(partsOf(type), partInfos(schema, type.record.fields));
  const { variant } = type;
  return [
    ...variant.cases.flatMap(variantCase =>
      array(partsOfCase(variant, variantCase), partInfos(schema, variantCase.fields)),
    ),
    ...array(
      partsOf(type),
      variant.cases.map(variantCase => caseInfo(variant, variantCase)),
    ),
  ];
}

function memberNameArrays(schema: Schema): string[] {
  const kinds = [
    ...entriesOf(schema).flatMap(entry => entry.payload.map(field => field.kind)),
    ...payloadTypesOf(schema).flatMap(type => fieldsOf(type).map(field => field.kind)),
  ];
  const spelled = new Set(kinds.flatMap(kind => spelledEnumOf(schema, kind) ?? []));
  return [...spelled].flatMap(type =>
    staticArray(
      "ASCIILiteral",
      `memberNamesOf${type.name}`,
      type.members.map(member => cppLiteral(member.name)),
    ),
  );
}

function defaultPayload(entry: Entry): string[] {
  const scalarFields = entry.payload.filter(field => field.kind.tag !== "List");
  return scalarFields.flatMap(field => defaultWordsOf(field.kind, field.isOptional));
}

function targetInfo(target: Target): string {
  return initializer([
    cppLiteral(target.name),
    truth(target.isVariadic),
    truth(target.takesProofs),
    target.results.length,
  ]);
}

function replacedEntries(entry: Entry, enumName: EntryEnumName): string[] {
  return [...entry.columns.values()]
    .flatMap(value => (value.tag === "Operations" ? value.entries : []))
    .filter(replaced => replaced.isTerminator === (enumName === "TerminatorKind"))
    .map(replaced => `${enumName}::${replaced.name}`);
}

function operandInfoTable(enumName: EntryEnumName): string[] {
  return [
    `constinit const std::array<OperandInfosOfEntry, numberOfEnumerators<${enumName}>> operandInfosBy${enumName} = [] {`,
    `    std::array<OperandInfosOfEntry, numberOfEnumerators<${enumName}>> table;`,
    "    for (unsigned index = 0; index < table.size(); ++index) {",
    `        std::span<const OperandInfo> infos = operandInfosOf(static_cast<${enumName}>(index));`,
    "        table[index] = { infos.data(), static_cast<uint32_t>(infos.size()) };",
    "    }",
    "    return table;",
    "}();",
  ];
}

function operationsSource(schema: Schema): string[] {
  const namedMembersOf = (operand: Operand) => {
    const [member, ...others] = operand.uses.namedMembers;
    if (member === undefined) return "{ }";
    if (others.length > 0) {
      throw new Error(
        `${operand.name}: an operand names one member at the most: ${operand.uses.namedMembers.join(", ")}`,
      );
    }
    const constant = namedExternalMemberOf(schema, "BuiltinName", member);
    return `std::span<const BuiltinName>(&builtinTableNamedNames[${constant}.position()], 1)`;
  };
  const contents = (entries: readonly Entry[], elementsOf: (entry: Entry) => string[]): EntryContents =>
    entries.map(entry => [entry, elementsOf(entry)]);
  const arrays = (elementType: string, prefix: string, elementsOf: (entry: Entry) => string[]) =>
    sharedArrays(elementType, prefix, contents(entriesOf(schema), elementsOf));
  const operands = arrays("OperandInfo", "operandInfos", entry =>
    operandsOf(entry).map(operand => operandInfo(operand, namedMembersOf(operand))),
  );
  const defaults = arrays("uint32_t", "defaultPayload", defaultPayload);
  const payload = arrays("PayloadFieldInfo", "payloadFields", entry =>
    entry.payload.map(field => payloadFieldInfo(schema, entry, field)),
  );
  const flags = arrays("ASCIILiteral", "flagNames", entry => entry.flags.map(flag => cppLiteral(flag.name)));
  const targetContents = contents(schema.terminators, entry => entry.targets.map(targetInfo));
  const targets = sharedArrays("TargetInfo", "targetInfos", targetContents);
  const replacedOperations = arrays("Opcode", "replacedOperations", entry => replacedEntries(entry, "Opcode"));
  const replacedTerminators = arrays("TerminatorKind", "replacedTerminators", entry =>
    replacedEntries(entry, "TerminatorKind"),
  );
  const spans =
    (type: string, name: string, shared: SharedArrays): EntryEmitter =>
    (entries, enumName, parameter) =>
      spanFunction(`${type} ${name}(${enumName} ${parameter})`, parameter, enumName, entries, shared.byEntry);
  const pair = (elementType: string, name: string, shared: SharedArrays) =>
    both(schema, spans(`std::span<const ${elementType}>`, name, shared));
  const definitions = blocks(
    operands.declarations,
    both(schema, spans("static constexpr std::span<const OperandInfo>", "operandInfosOf", operands)),
    operandInfoTable("Opcode"),
    operandInfoTable("TerminatorKind"),
    defaults.declarations,
    forOperations(schema, spans("static constexpr std::span<const uint32_t>", "defaultPayloadInSchemaOf", defaults)),
    [
      "constinit const std::array<FreshWordsOfOperation, numberOfEnumerators<Opcode>> freshWordsByOpcode = [] {",
      "    std::array<FreshWordsOfOperation, numberOfEnumerators<Opcode>> table;",
      "    for (unsigned index = 0; index < table.size(); ++index) {",
      "        std::span<const uint32_t> payload = defaultPayloadInSchemaOf(static_cast<Opcode>(index));",
      "        table[index] = { payload.data(), static_cast<uint16_t>(payload.size()), static_cast<uint16_t>(numberOfNamedOperandsOf(static_cast<Opcode>(index))), { } };",
      "        for (size_t word = 0; word < payload.size() && word < numberOfFreshPayloadWordsAtHand; ++word)",
      "            table[index].firstPayloadWords[word] = payload[word];",
      "    }",
      "    return table;",
      "}();",
    ],
    forTerminators(schema, spans("std::span<const uint32_t>", "defaultPayloadOf", defaults)),
    memberNameArrays(schema),
    payloadTypesOf(schema).flatMap(type => partArrays(schema, type)),
    payload.declarations,
    pair("PayloadFieldInfo", "payloadFieldsOf", payload),
    flags.declarations,
    pair("ASCIILiteral", "flagNamesOf", flags),
    targets.declarations,
    forTerminators(schema, spans("std::span<const TargetInfo>", "targetInfosOf", targets)),
    replacedOperations.declarations,
    pair("Opcode", "replacedOperationsOf", replacedOperations),
    replacedTerminators.declarations,
    pair("TerminatorKind", "replacedTerminatorsOf", replacedTerminators),
  );
  return cppSource(['"AOTOpcode.h"', '"AOTBuiltinIDTypes.h"', "<algorithm>"], inNamespace("Typed", definitions));
}

export function emitCppOperations(context: EmitContext): OutputFile[] {
  const schema = context.schema;
  return [
    { name: "AOTSchemaEntryMacros.h", lines: entryMacrosHeader(schema) },
    { name: "AOTSchemaOperandIndexes.h", lines: operandIndexesHeader(schema) },
    { name: "AOTSchemaOperations.h", lines: operationsHeader(schema) },
    {
      name: "AOTSchemaOperationColumns.h",
      lines: columnsHeader(
        schema,
        schema.entryColumns.filter(column => !namesRuntimeOperation(column)),
      ),
    },
    {
      name: "AOTSchemaRuntimeColumns.h",
      lines: columnsHeader(schema, schema.entryColumns.filter(namesRuntimeOperation)),
    },
    { name: "AOTSchemaOperations.cpp", lines: operationsSource(schema) },
  ];
}
