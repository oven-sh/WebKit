import { defaultWordsOf, engineRecordNameOf, engineTypeOf, twoWordScalars } from "./cpp-types.ts";
import { kindText } from "./hash.ts";
import {
  blocks,
  constexprSwitch,
  cppHeader,
  cppMarker,
  groupedCases,
  indent,
  indentation,
  inNamespace,
  type Lines,
} from "./lines.ts";
import type { Entry, Field, Flag, Kind, Parameter, PayloadField, Schema, SchemaRecord } from "./model.ts";
import { entriesOf, functionLocalIdKinds, isNested, wireFlagsOf, wireParametersOf } from "./model.ts";
import { cppOperandGetterName, reservedAccessorNames, upperFirst } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { compareLocations, SchemaError, type Location } from "./source.ts";
import { offsetText, wireRecordsInFields, wireSlotsOf } from "./wire-layout.ts";

type WireForm = "ID" | "Word" | "WordPair" | "Record";

type Offsets = ReadonlyMap<Parameter, string>;

type Read = (accessor: string, offset: string) => string;

interface Shape {
  readonly type: string;
  readonly engineDefault: string;
}

interface User<Part> {
  readonly entry: Entry;
  readonly part: Part;
}

interface ReadAtPlace {
  readonly place: number;
  readonly value: string;
}

interface Answer {
  readonly entry: Entry;
  readonly value: string;
  readonly atPlace: ReadAtPlace | "engine default" | null;
}

interface NamedGetter {
  readonly name: string;
  readonly type: string;
  readonly answers: readonly Answer[];
  readonly location: Location;
}

interface Cursor {
  readonly isTerminator: boolean;
  readonly enumName: string;
  readonly tag: string;
}

interface GetterWithTable {
  readonly table: Lines;
  readonly getter: Lines;
}

const placeOfEngineDefault = 0xfe;
const noPlace = 0xff;
const fewestClustersOfJumpTable = 4;

const bodyValue: Cursor = { isTerminator: false, enumName: "Opcode", tag: "opcode()" };
const bodyTerminator: Cursor = { isTerminator: true, enumName: "TerminatorKind", tag: "kind()" };

function readThrough(fields: string): Read {
  return (accessor, offset) => `${fields}${accessor}(${offset.replaceAll("word(", `${fields}word(`)})`;
}

const readOwnFields = readThrough("");

function readFieldWordsOf(entry: Entry): Read {
  if (entry.isTerminator) return readThrough("fieldWords().");
  return readThrough(wireFlagsOf(entry).length > 0 ? "fieldWordsBehindFlagsWord()." : "fieldWordsBehindHead().");
}

function refuse(location: Location, subject: string, kind: Kind): never {
  throw new SchemaError(location, `${subject}: no cursor over a body reads ${kindText(kind)}`);
}

function wireFormOf(kind: Kind): WireForm | null {
  switch (kind.tag) {
    case "Id":
      return kind.id.isEngine || functionLocalIdKinds.has(kind.id.name) ? null : "ID";
    case "Enum":
    case "Flags":
    case "External":
      return "Word";
    case "Scalar":
      if (kind.scalar === "Bool" || kind.scalar === "UInt32" || kind.scalar === "Field") return "Word";
      return kind.scalar === "Int64" || kind.scalar === "Double" ? "WordPair" : null;
    case "Record": {
      const { form, isCustomEngine, isWireOnly, hasSpans } = kind.record;
      return form === "wire" && !isCustomEngine && !isWireOnly && !hasSpans ? "Record" : null;
    }
    case "PlacedWord":
    case "Variant":
    case "Span":
    case "List":
      return null;
  }
}

function isNumber(kind: Kind): boolean {
  return kind.tag === "Scalar" && (kind.scalar === "UInt32" || kind.scalar === "Field");
}

function hasPayloadWordsCodec(kind: Kind): boolean {
  return isNested(kind) || (kind.tag === "Scalar" && twoWordScalars.has(kind.scalar));
}

function offsetsOnWire(schema: Schema): Offsets {
  const offsets = new Map<Parameter, string>();
  for (const entry of entriesOf(schema)) {
    if (!entry.isWire) continue;
    const parameters = wireParametersOf(entry);
    const slots = wireSlotsOf(entry);
    if (slots.length !== parameters.length)
      throw new Error(`${entry.name} has ${parameters.length} parameters on the wire and ${slots.length} slots`);
    parameters.forEach((parameter, index) => offsets.set(parameter, offsetText(slots[index])));
  }
  return offsets;
}

function emptyOf(type: string): Shape {
  return { type, engineDefault: `${type}()` };
}

function shapeOfList(element: Kind, isOnWire: boolean): Shape {
  const type = engineTypeOf(element);
  if (isOnWire && !isNumber(element)) return emptyOf(`BodyList<${type}>`);
  if (!hasPayloadWordsCodec(element)) return emptyOf(`std::span<const ${type}>`);
  const view = `PayloadListView<${type}>`;
  return { type: view, engineDefault: `${view}(std::span<const uint32_t>())` };
}

function shapeOf(parameter: Parameter, isOptional: boolean, isOnWire: boolean): Shape {
  if (parameter.tag === "Operand") return emptyOf(parameter.isVariadic ? "std::span<const ValueID>" : "ValueID");
  const { kind, heldAs } = parameter;
  if (kind.tag === "List") return shapeOfList(kind.element, isOnWire);
  const words = defaultWordsOf(kind, isOptional);
  if (heldAs !== null) {
    const held = `decodePayloadWord<TypeID>(${words[0]})`;
    if (heldAs === "ProvenType") return { type: "TypeID", engineDefault: held };
    return { type: heldAs, engineDefault: `${heldAs}(${held})` };
  }
  const type = engineTypeOf(kind, isOptional);
  if (!hasPayloadWordsCodec(kind)) return { type, engineDefault: `decodePayloadWord<${type}>(${words[0]})` };
  const payload = `std::array<uint32_t, PayloadWords<${type}>::count> { ${words.join(", ")} }`;
  return { type, engineDefault: `PayloadWords<${type}>::decode(${payload})` };
}

function payloadOnWire(field: PayloadField, offset: string, read: Read): string {
  const { kind, heldAs, isOptional, location } = field;
  const subject = `payload field ${field.name}`;
  if (heldAs !== null) {
    const held = read("idAt<TypeID>", offset);
    return heldAs === "ProvenType" ? held : `${heldAs}(${held})`;
  }
  if (kind.tag === "List") {
    if (isNumber(kind.element)) return read("numbersAt", offset);
    const form = wireFormOf(kind.element);
    if (form === null || form === "Word") return refuse(location, subject, kind);
    return read(`listAt<${engineTypeOf(kind.element)}>`, offset);
  }
  const type = engineTypeOf(kind, isOptional);
  switch (wireFormOf(kind)) {
    case "ID":
      return read(`${isOptional ? "optionalIDAt" : "idAt"}<${type}>`, offset);
    case "Word":
      return read(`plainAt<${type}>`, offset);
    case "WordPair":
      return read(kind.tag === "Scalar" && kind.scalar === "Double" ? "doubleAt" : "int64At", offset);
    case "Record":
      return read(`recordAt<${type}>`, offset);
    case null:
      return refuse(location, subject, kind);
  }
}

function valueOnWire(parameter: Parameter, offset: string, read: Read): string {
  if (parameter.tag === "Payload") return payloadOnWire(parameter, offset, read);
  return read(parameter.isVariadic ? "operandsAt" : "operandAt", offset);
}

function flagOnWire(flag: Flag): string {
  return `flags() & 1 << ${flag.bit}`;
}

function readAtPlace(entry: Entry, parameter: Parameter, offset: string): ReadAtPlace | null {
  if (entry.isTerminator || !/^\d+$/.test(offset)) return null;
  const place = Number(offset) + (wireFlagsOf(entry).length > 0 ? 1 : 0);
  if (place >= placeOfEngineDefault) return null;
  return { place, value: valueOnWire(parameter, "place", readThrough("wordsBehindHead().")) };
}

function getterNameOf(parameter: Parameter): string {
  return parameter.tag === "Payload" ? parameter.name : cppOperandGetterName(parameter.name, parameter.isVariadic);
}

function formInRecord(field: Field): WireForm {
  return wireFormOf(field.kind) ?? refuse(field.location, `field ${field.name}`, field.kind);
}

function idInUnit(field: Field, word: number): string {
  const translate = field.isOptional ? "optionalProgramIDOf" : "programIDOf";
  return `${translate}<${engineTypeOf(field.kind)}>(unit, words[${word}])`;
}

function fieldFromWire(field: Field, firstWord: number): string {
  const { kind, isOptional } = field;
  switch (formInRecord(field)) {
    case "ID":
      return idInUnit(field, firstWord);
    case "Word":
      return `decodePayloadWord<${engineTypeOf(kind, isOptional)}>(words[${firstWord}])`;
    case "WordPair":
    case "Record":
      return `BodyElement<${engineTypeOf(kind)}>::decode(${firstWord === 0 ? "words" : `words + ${firstWord}`}, unit)`;
  }
}

function payloadWordsFromWire(record: SchemaRecord, firstWord: number): string[] {
  const payload: string[] = [];
  let word = firstWord;
  for (const field of record.fields) {
    if (field.isEngine) {
      payload.push(...defaultWordsOf(field.kind, field.isOptional));
      continue;
    }
    const form = formInRecord(field);
    if (field.kind.tag === "Record") payload.push(...payloadWordsFromWire(field.kind.record, word));
    else if (form === "ID") payload.push(`${idInUnit(field, word)}.bits()`);
    else payload.push(...Array.from({ length: field.wireWords }, (_, index) => `words[${word + index}]`));
    word += field.wireWords;
  }
  return payload;
}

function decodeStatements(record: SchemaRecord): string[] {
  const type = engineRecordNameOf(record);
  if (record.isHandwrittenStruct) {
    return [
      `std::array<uint32_t, PayloadWords<${type}>::count> payload { ${payloadWordsFromWire(record, 0).join(", ")} };`,
      `return PayloadWords<${type}>::decode(payload);`,
    ];
  }
  const statements = [`${type} record;`];
  let word = 0;
  for (const field of record.wireFields) {
    statements.push(`record.${field.name} = ${fieldFromWire(field, word)};`);
    word += field.wireWords;
  }
  return [...statements, "return record;"];
}

function elementStruct(record: SchemaRecord): string[] {
  const type = engineRecordNameOf(record);
  const statements = decodeStatements(record);
  const unit = statements.some(statement => /\(unit,|, unit\)/.test(statement)) ? " unit" : "";
  return [
    "template<>",
    `struct BodyElement<${type}> {`,
    `${indentation}static constexpr uint32_t numberOfWords = ${record.wireWords};`,
    `${indentation}static ${type} decode(const uint32_t* words, const UnitIDRemap&${unit})`,
    `${indentation}{`,
    ...indent(statements, 2),
    `${indentation}}`,
    "};",
  ];
}

function elementsInc(schema: Schema, tablesOfGetters: readonly Lines[]): string[] {
  const places = [
    `inline constexpr uint8_t placeOfEngineDefaultInBodyValue = ${placeOfEngineDefault};`,
    `inline constexpr uint8_t noPlaceInBodyValue = ${noPlace};`,
  ];
  return blocks([cppMarker], ...wireRecordsInFields(schema).map(elementStruct), places, ...tablesOfGetters);
}

function getterLine(type: string, name: string, value: string): string {
  return `${type} ${name}() const { return ${value}; }`;
}

function operationClass(entry: Entry, offsets: Offsets): string[] {
  const getters = [
    ...wireParametersOf(entry).map(parameter => {
      const offset = offsets.get(parameter);
      if (offset === undefined) throw new Error(`parameter ${parameter.name} of ${entry.name} has no offset`);
      const { type } = shapeOf(parameter, parameter.isOptional, true);
      return getterLine(type, getterNameOf(parameter), valueOnWire(parameter, offset, readOwnFields));
    }),
    ...wireFlagsOf(entry).map(flag => getterLine("bool", flag.name, flagOnWire(flag))),
  ];
  return [
    "template<>",
    `class BodyOperation<${entry.isTerminator ? "TerminatorKind" : "Opcode"}::${entry.name}> : public BodyFields {`,
    "public:",
    ...indent(blocks(["explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }"], getters)),
    "};",
  ];
}

function operationsHeader(schema: Schema, offsets: Offsets): string[] {
  const compare = compareLocations(schema.files);
  const entries = entriesOf(schema)
    .filter(entry => entry.isWire)
    .sort((left, right) => compare(left.location, right.location));
  return cppHeader([], inNamespace("Typed", blocks(...entries.map(entry => operationClass(entry, offsets)))));
}

function usersByName<Part extends { readonly name: string }>(
  entries: readonly Entry[],
  partsOf: (entry: Entry) => readonly Part[],
): User<Part>[][] {
  const users = entries.flatMap(entry => partsOf(entry).map(part => ({ entry, part })));
  return [...Map.groupBy(users, user => user.part.name).values()];
}

function gettersOfParameters(
  entries: readonly Entry[],
  offsets: Offsets,
  parametersOf: (entry: Entry) => readonly Parameter[],
): NamedGetter[] {
  return usersByName(entries, parametersOf).map(users => {
    const first = users[0].part;
    const isOptional = users.some(user => user.part.isOptional);
    const isOnWire = users.some(user => offsets.has(user.part));
    const { type, engineDefault } = shapeOf(first, isOptional, isOnWire);
    const answers = users.map(({ entry, part }) => {
      const offset = offsets.get(part);
      if (offset === undefined) return { entry, value: engineDefault, atPlace: "engine default" as const };
      const value = valueOnWire(part, offset, readFieldWordsOf(entry));
      return { entry, value, atPlace: readAtPlace(entry, part, offset) };
    });
    return { name: getterNameOf(first), type, answers, location: first.location };
  });
}

function gettersOfFlags(entries: readonly Entry[]): NamedGetter[] {
  return usersByName(entries, entry => entry.flags).map(users => {
    const { name, location } = users[0].part;
    const answers = users.map(({ entry, part }) => ({
      entry,
      value: entry.isWire && !part.isEngine ? `flagsWord() & 1 << ${part.bit}` : "false",
      atPlace: null,
    }));
    return { name, type: "bool", answers, location };
  });
}

function gettersByName(schema: Schema, offsets: Offsets): NamedGetter[] {
  const entries = entriesOf(schema);
  const getters = [
    ...gettersOfParameters(entries, offsets, entry => entry.payload),
    ...gettersOfFlags(entries),
    ...gettersOfParameters(entries, offsets, entry => entry.namedOperands),
    ...gettersOfParameters(entries, offsets, entry => (entry.variadicOperand === null ? [] : [entry.variadicOperand])),
  ];
  const names = new Set<string>();
  for (const { name, location } of getters) {
    if (reservedAccessorNames.has(name)) throw new SchemaError(location, `the getter ${name}() has a reserved name`);
    if (names.has(name)) throw new SchemaError(location, `two getters are named ${name}()`);
    names.add(name);
  }
  return getters;
}

function getterBySwitch(cursor: Cursor, getter: NamedGetter): string[] {
  const answers = getter.answers.filter(answer => answer.entry.isTerminator === cursor.isTerminator);
  if (answers.length === 0) return [];
  const inFunction = indentation.repeat(2);
  const cases = answers.map(({ entry, value }) => ({ label: entry.name, body: [`return ${value};`] }));
  return [
    `${indentation}${cursor.isTerminator ? "" : "ALWAYS_INLINE "}${getter.type} ${getter.name}() const`,
    `${indentation}{`,
    `${inFunction}switch (${cursor.tag}) {`,
    ...groupedCases(cursor.enumName, cases, inFunction),
    `${inFunction}default:`,
    `${inFunction}${indentation}RELEASE_ASSERT_NOT_REACHED();`,
    `${inFunction}}`,
    `${indentation}}`,
  ];
}

function numberOfClusters(operations: readonly Entry[], answers: readonly Answer[]): number {
  const inOrder = answers
    .map(({ entry, value }) => ({ number: operations.indexOf(entry), value }))
    .sort((left, right) => left.number - right.number);
  return inOrder.filter(({ number, value }, index) => {
    const previous = index === 0 ? null : inOrder[index - 1];
    return previous === null || previous.number + 1 !== number || previous.value !== value;
  }).length;
}

function valueReadAtPlace(answers: readonly Answer[]): string | null {
  if (answers.some(({ atPlace }) => atPlace === null)) return null;
  const reads = answers.flatMap(({ atPlace }) =>
    atPlace === null || atPlace === "engine default" ? [] : [atPlace.value],
  );
  const someIsOptional = reads.some(read => read.includes(".optionalIDAt<"));
  const distinct = new Set(reads.map(read => (someIsOptional ? read.replace(".idAt<", ".optionalIDAt<") : read)));
  const [only, ...others] = distinct;
  return only !== undefined && others.length === 0 ? only : null;
}

function getterByPlace(getter: NamedGetter, answers: readonly Answer[], value: string): GetterWithTable {
  const placeOf = `placeOf${upperFirst(getter.name)}InBodyValue`;
  const cases = answers.map(({ entry, atPlace }) => ({
    label: entry.name,
    value: atPlace === null || atPlace === "engine default" ? "placeOfEngineDefaultInBodyValue" : String(atPlace.place),
  }));
  const engineDefault = answers.find(({ atPlace }) => atPlace === "engine default")?.value;
  const answer =
    engineDefault === undefined
      ? ["RELEASE_ASSERT(place != noPlaceInBodyValue);", `return ${value};`]
      : [
          "if (place < placeOfEngineDefaultInBodyValue) [[likely]]",
          `${indentation}return ${value};`,
          "RELEASE_ASSERT(place == placeOfEngineDefaultInBodyValue);",
          `return ${engineDefault};`,
        ];
  return {
    table: constexprSwitch({
      signature: `uint8_t ${placeOf}(Opcode opcode)`,
      subject: "opcode",
      enumName: "Opcode",
      cases,
      defaultValue: "noPlaceInBodyValue",
    }),
    getter: indent([
      `ALWAYS_INLINE ${getter.type} ${getter.name}() const`,
      "{",
      ...indent([`uint8_t place = CaseTable<Opcode, ${placeOf}>::at(opcode());`, ...answer]),
      "}",
    ]),
  };
}

function getterByArm(getter: NamedGetter, answers: readonly Answer[]): GetterWithTable {
  const armOf = `armOf${upperFirst(getter.name)}InBodyValue`;
  const arms = [...Map.groupBy(answers, ({ value }) => value).entries()];
  const cases = arms.flatMap(([, members], index) =>
    members.map(({ entry }) => ({ label: entry.name, value: String(index + 1) })),
  );
  return {
    table: constexprSwitch({
      signature: `uint8_t ${armOf}(Opcode opcode)`,
      subject: "opcode",
      enumName: "Opcode",
      cases,
      defaultValue: "0",
    }),
    getter: indent([
      `${getter.type} ${getter.name}() const`,
      "{",
      ...indent([
        `switch (CaseTable<Opcode, ${armOf}>::at(opcode())) {`,
        ...arms.flatMap(([value], index) => [`case ${index + 1}:`, `${indentation}return ${value};`]),
        "default:",
        `${indentation}RELEASE_ASSERT_NOT_REACHED();`,
        "}",
      ]),
      "}",
    ]),
  };
}

function getterOfBodyValue(operations: readonly Entry[], getter: NamedGetter): GetterWithTable {
  const answers = getter.answers.filter(answer => !answer.entry.isTerminator);
  if (numberOfClusters(operations, answers) < fewestClustersOfJumpTable)
    return { table: [], getter: getterBySwitch(bodyValue, getter) };
  const value = valueReadAtPlace(answers);
  return value === null ? getterByArm(getter, answers) : getterByPlace(getter, answers, value);
}

function accessorsInc(getters: readonly Lines[]): string[] {
  return blocks([cppMarker], ...getters);
}

export function emitCppCursors(context: EmitContext): OutputFile[] {
  const { schema } = context;
  const offsets = offsetsOnWire(schema);
  const getters = gettersByName(schema, offsets);
  const ofBodyValue = getters.map(getter => getterOfBodyValue(schema.operations, getter));
  const ofBodyTerminator = getters.map(getter => getterBySwitch(bodyTerminator, getter));
  return [
    { name: "AOTSchemaBodyOperations.h", lines: operationsHeader(schema, offsets) },
    {
      name: "AOTSchemaBodyElements.inc",
      lines: elementsInc(
        schema,
        ofBodyValue.map(({ table }) => table),
      ),
    },
    { name: "AOTSchemaBodyValueAccessors.inc", lines: accessorsInc(ofBodyValue.map(({ getter }) => getter)) },
    { name: "AOTSchemaBodyTerminatorAccessors.inc", lines: accessorsInc(ofBodyTerminator) },
  ];
}
