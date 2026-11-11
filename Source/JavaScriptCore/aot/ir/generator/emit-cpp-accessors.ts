import { engineTypeOf, listFirstWord, twoWordScalars } from "./cpp-types.ts";
import { blocks, braced, cppHeader, cppMarker, indent, indentation, lineWidth, pack } from "./lines.ts";
import type { Entry, Kind, PayloadField, Schema } from "./model.ts";
import { entriesOf } from "./model.ts";
import { cppOperandGetterName, cppSetterName, upperFirst } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { SchemaError } from "./source.ts";

interface User {
  readonly name: string;
  readonly entry: Entry;
  readonly position: string;
}

interface PayloadUser extends User {
  readonly field: PayloadField;
}

type UsersByName<Part extends User = User> = ReadonlyMap<string, readonly Part[]>;

interface NamedParts {
  readonly payload: UsersByName<PayloadUser>;
  readonly flags: UsersByName;
  readonly operands: UsersByName;
  readonly variadicOperands: UsersByName;
}

interface EntryEnum {
  readonly isTerminator: boolean;
  readonly enumName: string;
  readonly tag: string;
  readonly entrySetPrefix: string;
}

interface Accessors {
  readonly getter: string;
  readonly setter: string;
}

interface AccessorsWithEntrySet extends Accessors {
  readonly entrySet: readonly string[];
}

interface AccessorsOfEntries {
  readonly payloadAndFlags: readonly AccessorsWithEntrySet[];
  readonly operands: readonly Accessors[];
}

const opcodes: EntryEnum = {
  isTerminator: false,
  enumName: "Opcode",
  tag: "opcode",
  entrySetPrefix: "OpcodeSet opcodesWith",
};

const terminatorKinds: EntryEnum = {
  isTerminator: true,
  enumName: "TerminatorKind",
  tag: "kind",
  entrySetPrefix: "TerminatorKindSet kindsWith",
};

const bitsOfFlagsInCode = 8;

function flagUsersOf(entry: Entry): User[] {
  return entry.flags.map(flag => {
    if (flag.bit >= bitsOfFlagsInCode) {
      const problem = `flag ${flag.name} of ${entry.name} is bit ${flag.bit}: a record of Code has ${bitsOfFlagsInCode} bits of flags`;
      throw new SchemaError(flag.location, problem);
    }
    return { name: flag.name, entry, position: `1 << ${flag.bit}` };
  });
}

function collect(schema: Schema): NamedParts {
  const entries = entriesOf(schema);
  const byName = <Part extends User>(partsOf: (entry: Entry) => readonly Part[]): UsersByName<Part> =>
    Map.groupBy(
      entries.flatMap(entry => partsOf(entry)),
      part => part.name,
    );
  return {
    payload: byName(entry =>
      entry.payload.map(field => {
        const position = field.firstWord === null ? listFirstWord(entry) : String(field.firstWord);
        return { name: field.name, entry, field, position };
      }),
    ),
    flags: byName(flagUsersOf),
    operands: byName(entry =>
      entry.namedOperands.map(operand => ({ name: operand.name, entry, position: String(operand.index) })),
    ),
    variadicOperands: byName(entry => {
      const operand = entry.variadicOperand;
      return operand === null ? [] : [{ name: operand.name, entry, position: String(operand.index) }];
    }),
  };
}

const leastUsersOfSharedTable = 24;

function lookup(name: string, users: readonly User[]): string[] {
  const functions: string[][] = [];
  for (const { isTerminator, enumName, tag } of [opcodes, terminatorKinds]) {
    const own = users.filter(user => user.entry.isTerminator === isTerminator);
    const isShared = own.length >= leastUsersOfSharedTable;
    const signature = `constexpr std::optional<unsigned> ${isShared ? `${name}AsDeclared` : name}(${enumName} ${tag})`;
    if (own.length === 0) continue;
    if (own.length === 1) {
      const [{ entry, position }] = own;
      functions.push([
        `${signature} { return ${tag} == ${enumName}::${entry.name} ? std::optional<unsigned>(${position}) : std::nullopt; }`,
      ]);
      continue;
    }
    const lines = [signature, "{", `${indentation}switch (${tag}) {`];
    for (const [position, group] of Map.groupBy(own, user => user.position)) {
      const labels = pack(
        group.map(user => `case ${enumName}::${user.entry.name}:`),
        indentation,
      );
      const result = `return ${position};`;
      const last = labels.length - 1;
      if (labels[last].length + 1 + result.length <= lineWidth) labels[last] += ` ${result}`;
      else labels.push(`${indentation}${indentation}${result}`);
      lines.push(...labels);
    }
    lines.push(`${indentation}default: return std::nullopt;`, `${indentation}}`, "}");
    functions.push(lines);
    if (isShared) {
      const table = `PositionTable<${enumName}, ${name}AsDeclared>`;
      functions.push([`constexpr std::optional<unsigned> ${name}(${enumName} ${tag}) { return ${table}::at(${tag}); }`]);
    }
  }
  return blocks(...functions);
}

function positionsHeader(namespace: string, table: UsersByName): string[] {
  return cppHeader(
    [],
    [
      `namespace JSC { namespace AOT { namespace Typed { namespace ${namespace} {`,
      "",
      ...blocks(...[...table].map(([name, users]) => lookup(name, users))),
      "",
      `} } } } // namespace JSC::AOT::Typed::${namespace}`,
    ],
  );
}

function listAccessors(name: string, element: Kind, position: string): Accessors {
  const type = engineTypeOf(element);
  const setter = `void ${cppSetterName(name)}(std::span<const ${type}> ${name})`;
  const isTwoWordScalar = element.tag === "Scalar" && twoWordScalars.has(element.scalar);
  if (element.tag === "Record" || element.tag === "Variant" || isTwoWordScalar) {
    return {
      getter: `PayloadListView<${type}> ${name}() const LIFETIME_BOUND { return PayloadListView<${type}>(payloadListFrom(${position})); }`,
      setter: `${setter} { setPayloadListFrom(${position}, encodePayloadList(${name}).span()); }`,
    };
  }
  return {
    getter: `std::span<const ${type}> ${name}() const LIFETIME_BOUND { return payloadListOf<${type}>(${position}); }`,
    setter: `${setter} { setPayloadListOf<${type}>(${position}, ${name}); }`,
  };
}

function payloadAccessors(name: string, field: PayloadField, isOptional: boolean, tag: string): Accessors {
  const { kind, heldAs } = field;
  const setter = cppSetterName(name);
  const position = `declaredPosition(PayloadWord::${name}(${tag}))`;
  if (heldAs !== null) {
    const read = `decodePayloadWord<TypeID>(payloadWordAt(${position}))`;
    return {
      getter:
        heldAs === "ProvenType"
          ? `TypeID ${name}() const { return ${read}; }`
          : `${heldAs} ${name}() const { return ${heldAs}(${read}); }`,
      setter: `void ${setter}(${heldAs} ${name}) { setPayloadWordAt(${position}, encodePayloadWord(${name}.payload())); }`,
    };
  }
  if (kind.tag === "List") return listAccessors(name, kind.element, position);
  if (kind.tag === "Scalar" && twoWordScalars.has(kind.scalar)) {
    const type = engineTypeOf(kind);
    const cast = kind.scalar === "Double" ? "std::bit_cast" : "static_cast";
    return {
      getter: `${type} ${name}() const { return ${cast}<${type}>(payloadWordPairAt(${position})); }`,
      setter: `void ${setter}(${type} ${name}) { setPayloadWordPairAt(${position}, ${cast}<uint64_t>(${name})); }`,
    };
  }
  const type = engineTypeOf(kind, isOptional);
  if (kind.tag === "Record" || kind.tag === "Variant") {
    return {
      getter: `${type} ${name}() const { return decodePayloadRecord<${type}>(*this, ${position}); }`,
      setter: `void ${setter}(const ${type}& ${name}) { encodePayloadRecord(*this, ${position}, ${name}); }`,
    };
  }
  return {
    getter: `${type} ${name}() const { return decodePayloadWord<${type}>(payloadWordAt(${position})); }`,
    setter: `void ${setter}(${type} ${name}) { setPayloadWordAt(${position}, encodePayloadWord(${name})); }`,
  };
}

const flagsThatRestOnPassedTests = new Set(["isInsideView", "isInBounds", "isPresent"]);

function flagAccessors(name: string, tag: string): Accessors {
  const mask = `declaredPosition(FlagMask::${name}(${tag}))`;
  const getter = `bool ${name}() const { return flags & ${mask}; }`;
  if (flagsThatRestOnPassedTests.has(name))
    return { getter, setter: `void ${cppSetterName(name)}(const PassedTest& passed) { flags |= ${mask}; restOn(passed); }` };
  return {
    getter,
    setter: `void ${cppSetterName(name)}(bool ${name}) { flags = ${name} ? flags | ${mask} : flags & ~${mask}; }`,
  };
}

function operandAccessors(name: string, tag: string): Accessors {
  const getter = cppOperandGetterName(name, false);
  const position = `declaredPosition(OperandPosition::${name}(${tag}))`;
  return {
    getter: `OperandID ${getter}() const { return operand(${position}); }`,
    setter: `void ${cppSetterName(getter)}(OperandID ${name}) { setOperand(${position}, ${name}); }`,
  };
}

function variadicOperandAccessors(name: string, tag: string): Accessors {
  const getter = cppOperandGetterName(name, true);
  const assertion = `RELEASE_ASSERT(VariadicOperandPosition::${name}(${tag}));`;
  return {
    getter: `std::span<const OperandID> ${getter}() const LIFETIME_BOUND { ${assertion} return variadicOperands(); }`,
    setter: `void ${cppSetterName(getter)}(std::span<const OperandID> ${name}) { ${assertion} setVariadicOperands(${name}); }`,
  };
}

function entrySet(entryEnum: EntryEnum, name: string, users: readonly User[]): string[] {
  return braced(
    `static constexpr ${entryEnum.entrySetPrefix}${upperFirst(name)}`,
    users.map(user => `${entryEnum.enumName}::${user.entry.name}`),
    "};",
    indentation,
  );
}

function accessorsOf(parts: NamedParts, entryEnum: EntryEnum): AccessorsOfEntries {
  const { tag } = entryEnum;
  const ownUsersOf = (users: readonly User[]): User[] =>
    users.filter(user => user.entry.isTerminator === entryEnum.isTerminator);
  const payloadAndFlags: AccessorsWithEntrySet[] = [];
  for (const [name, users] of parts.payload) {
    const own = ownUsersOf(users);
    if (own.length === 0) continue;
    const isOptional = users.some(user => user.field.isOptional);
    const accessors = payloadAccessors(name, users[0].field, isOptional, tag);
    payloadAndFlags.push({ ...accessors, entrySet: entrySet(entryEnum, name, own) });
  }
  for (const [name, users] of parts.flags) {
    const own = ownUsersOf(users);
    if (own.length === 0) continue;
    payloadAndFlags.push({ ...flagAccessors(name, tag), entrySet: entrySet(entryEnum, name, own) });
  }
  const operands: Accessors[] = [];
  for (const [name, users] of parts.operands) {
    if (ownUsersOf(users).length > 0) operands.push(operandAccessors(name, tag));
  }
  for (const [name, users] of parts.variadicOperands) {
    if (ownUsersOf(users).length > 0) operands.push(variadicOperandAccessors(name, tag));
  }
  return { payloadAndFlags, operands };
}

function gettersInc({ payloadAndFlags, operands }: AccessorsOfEntries): string[] {
  return blocks(
    [cppMarker],
    ...payloadAndFlags.map(accessors => [...indent([accessors.getter]), ...accessors.entrySet]),
    indent(operands.map(accessors => accessors.getter)),
  );
}

const inheritedOperandSetter = "using DraftWords::setOperand;";

function settersInc({ payloadAndFlags, operands }: AccessorsOfEntries): string[] {
  return blocks(
    [cppMarker],
    indent([inheritedOperandSetter]),
    indent(payloadAndFlags.map(accessors => accessors.setter)),
    indent(operands.map(accessors => accessors.setter)),
  );
}

export function emitCppAccessors(context: EmitContext): OutputFile[] {
  const parts = collect(context.schema);
  const ofOperations = accessorsOf(parts, opcodes);
  const ofTerminators = accessorsOf(parts, terminatorKinds);
  return [
    { name: "AOTSchemaPayloadPositions.h", lines: positionsHeader("PayloadWord", parts.payload) },
    { name: "AOTSchemaFlagMasks.h", lines: positionsHeader("FlagMask", parts.flags) },
    { name: "AOTSchemaOperandPositions.h", lines: positionsHeader("OperandPosition", parts.operands) },
    {
      name: "AOTSchemaVariadicOperandPositions.h",
      lines: positionsHeader("VariadicOperandPosition", parts.variadicOperands),
    },
    { name: "AOTSchemaOperationGetters.inc", lines: gettersInc(ofOperations) },
    { name: "AOTSchemaOperationSetters.inc", lines: settersInc(ofOperations) },
    { name: "AOTSchemaTerminatorGetters.inc", lines: gettersInc(ofTerminators) },
    { name: "AOTSchemaTerminatorSetters.inc", lines: settersInc(ofTerminators) },
  ];
}
