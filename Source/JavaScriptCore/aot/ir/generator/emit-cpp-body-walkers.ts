import { engineTypeOf, listFirstWord } from "./cpp-types.ts";
import { holdsReferences, roleArgument } from "./emit-cpp-walkers.ts";
import { blocks, constexprSwitch, cppHeader, groupedCases, inNamespace, indent, type Lines } from "./lines.ts";
import type { Entry, Kind, Operand, Parameter, PayloadField, Schema, SchemaRecord } from "./model.ts";
import {
  functionLocalIdKinds,
  isNested,
  isWireType,
  nameOf,
  payloadWordsOf,
  wireFlagsOf,
  wireParametersOf,
} from "./model.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { SchemaError } from "./source.ts";
import { fieldWordsText, hasFixedSize, offsetText, wireSlotsOf } from "./wire-layout.ts";

interface Cursor {
  readonly className: "BodyValue" | "BodyTerminator";
  readonly enumName: "Opcode" | "TerminatorKind";
  readonly tag: "opcode" | "kind";
  readonly parameter: "value" | "terminator";
  readonly entries: readonly Entry[];
}

interface Placed<Part extends Parameter> {
  readonly part: Part;
  readonly offset: string;
}

interface Walker {
  readonly name: string;
  readonly head: (cursor: Cursor) => Lines;
  readonly isMember: boolean;
  readonly answerForParameter: string;
  readonly bodyOf: (entry: Entry) => Lines;
}

type Translation =
  | { readonly tag: "None" }
  | { readonly tag: "Id" }
  | { readonly tag: "Record"; readonly record: SchemaRecord };

interface ReferenceInRecord {
  readonly word: number;
  readonly kind: Kind;
  readonly isOptional: boolean;
}

const variableSizeMarker = 0xff;
const maxCasesPerTable = 256;
const markOfFlagsWordInCase = 128;
const notOnWire: Lines = ["RELEASE_ASSERT_NOT_REACHED();"];

const helpers = `static_assert(Wire::none == absentPayloadWord);

inline constexpr uint8_t variableNumberOfFieldWords = ${variableSizeMarker};

template<typename Functor>
ALWAYS_INLINE void visitBodyOperand(ValueID operand, uint32_t operandIndex, const Functor& functor)
{
    if (operand)
        functor(operand, operandIndex);
}

template<typename Functor>
ALWAYS_INLINE void visitVariadicBodyOperands(std::span<const ValueID> operands, uint32_t firstOperandIndex, const Functor& functor)
{
    for (ValueID operand : operands)
        visitBodyOperand(operand, firstOperandIndex++, functor);
}

inline ValueID variadicBodyOperandAt(std::span<const ValueID> operands, uint32_t firstOperandIndex, uint32_t operandIndex)
{
    if (operandIndex < firstOperandIndex || operandIndex - firstOperandIndex >= operands.size())
        return { };
    return operands[operandIndex - firstOperandIndex];
}

// A null id means the field names a record that no committed file defines. It is still visited, because the Facts scan reports it.
template<typename IDType, typename Visitor, typename... Role>
ALWAYS_INLINE void visitBodyReference(IDType id, Visitor& visitor, Role... role)
{
    if constexpr (std::is_invocable_v<Visitor&, IDType, Role...>)
        visitor(id, role...);
    else
        visitor(id);
}

template<typename IDType, typename Visitor, typename... Role>
ALWAYS_INLINE void visitOptionalBodyReference(const BodyFields& fields, uint32_t offset, Visitor& visitor, Role... role)
{
    if (fields.word(offset) != Wire::none)
        visitBodyReference(fields.idAt<IDType>(offset), visitor, role...);
}

template<OperationHolder Holder, typename Functor>
ALWAYS_INLINE void copyBodyOperand(ValueID operand, uint32_t operandIndex, Holder& holder, const Functor& operandOf)
{
    if (operand)
        holder.setOperand(operandIndex, operandOf(operand));
}

template<OperationHolder Holder, typename Functor>
void copyVariadicBodyOperands(std::span<const ValueID> operands, Holder& holder, const Functor& operandOf)
{
    holder.reserveVariadicOperands(static_cast<uint32_t>(operands.size()));
    for (ValueID operand : operands)
        holder.appendVariadicOperand(operand ? operandOf(operand) : typename Holder::OperandID());
}

template<typename IDType, OperationHolder Holder>
void copyBodyIDList(BodyList<IDType> ids, Holder& holder, unsigned firstWord)
{
    Vector<uint32_t, 16> words;
    words.reserveInitialCapacity(ids.size());
    for (IDType id : ids)
        words.append(encodePayloadWord(id));
    holder.setPayloadListFrom(firstWord, words.span());
}

template<typename Record, OperationHolder Holder>
void copyBodyRecordList(BodyList<Record> records, Holder& holder, unsigned firstWord)
{
    constexpr unsigned wordsPerElement = PayloadWords<Record>::count;
    Vector<uint32_t, 16> words(FillWith { }, static_cast<size_t>(records.size()) * wordsPerElement, 0u);
    for (uint32_t index = 0; index < records.size(); ++index)
        PayloadWords<Record>::encode(records[index], words.mutableSpan().subspan(static_cast<size_t>(index) * wordsPerElement).template first<wordsPerElement>());
    holder.setPayloadListFrom(firstWord, words.span());
}`.split("\n");

function refuse(entry: Entry, field: PayloadField, problem: string): never {
  throw new SchemaError(field.location, `field ${field.name} of ${entry.name}: ${problem}`);
}

function placedParametersOf(entry: Entry): Placed<Parameter>[] {
  const parameters = wireParametersOf(entry);
  const slots = wireSlotsOf(entry);
  if (slots.length !== parameters.length) {
    const counts = `${parameters.length} fields on the wire and ${slots.length} slots`;
    throw new SchemaError(entry.location, `${entry.name} has ${counts}`);
  }
  return parameters.map((part, index) => ({ part, offset: offsetText(slots[index]) }));
}

function placedOperandsOf(entry: Entry): Placed<Operand>[] {
  return placedParametersOf(entry).flatMap(({ part, offset }) => (part.tag === "Operand" ? [{ part, offset }] : []));
}

function placedPayloadOf(entry: Entry): Placed<PayloadField>[] {
  return placedParametersOf(entry).flatMap(({ part, offset }) => (part.tag === "Payload" ? [{ part, offset }] : []));
}

function offsetPlus(offset: string, words: number): string {
  if (words === 0) return offset;
  return /^\d+$/.test(offset) ? String(Number(offset) + words) : `${offset} + ${words}`;
}

function translationOf(entry: Entry, field: PayloadField, kind: Kind): Translation {
  if (kind.tag === "Span" || kind.tag === "List") refuse(entry, field, "a list can only be a whole field");
  if (kind.tag === "Id") {
    if (functionLocalIdKinds.has(kind.id.name)) {
      refuse(entry, field, `a ${kind.id.name} id cannot be payload on the wire`);
    }
    return { tag: "Id" };
  }
  if (!isNested(kind)) return { tag: "None" };
  if (kind.tag === "Record" && isWireType(kind)) return { tag: "Record", record: kind.record };
  if (isWireType(kind)) refuse(entry, field, `no cursor reads the wire variant ${nameOf(kind)}`);
  if (holdsReferences(kind)) {
    refuse(entry, field, `${nameOf(kind)} is not a wire record, so its ids would not be translated`);
  }
  return { tag: "None" };
}

function fixedNumberOfFieldWordsOf(entry: Entry): number | null {
  if (!entry.isWire || !hasFixedSize(entry)) return null;
  const text = fieldWordsText(entry);
  const words = Number(text);
  if (!Number.isInteger(words) || words < 0) {
    const problem = `its word count is not a whole number: ${text}`;
    throw new SchemaError(entry.location, `${entry.name} has fixed-size fields, but ${problem}`);
  }
  return words < variableSizeMarker ? words : null;
}

function numberOfFieldWordsFunction(cursor: Cursor): string[] {
  const { className, enumName, tag } = cursor;
  const sizeOf = `fixedNumberOfFieldWordsOf${className}`;
  const sizes = cursor.entries.map(entry => ({ entry, size: fixedNumberOfFieldWordsOf(entry) }));
  const cases = sizes.map(({ entry, size }) => ({
    label: entry.name,
    value: size === null ? "variableNumberOfFieldWords" : String(size),
  }));
  const counted = sizes
    .filter(({ entry, size }) => entry.isWire && size === null)
    .map(({ entry }) => ({ label: entry.name, body: [`return ${fieldWordsText(entry)};`] }));
  const signature = `uint8_t ${sizeOf}(${enumName} ${tag})`;
  return blocks(
    constexprSwitch({ signature, subject: tag, enumName, cases, defaultValue: "most common" }),
    [
      `inline uint32_t numberOfCountedFieldWords(${enumName} ${tag}, [[maybe_unused]] const uint32_t* fields)`,
      "{",
      ...indent([
        "[[maybe_unused]] auto word = [fields](uint32_t offset) { return fields[offset]; };",
        `switch (${tag}) {`,
      ]),
      ...groupedCases(enumName, counted),
      ...indent(["default:", ...indent(["RELEASE_ASSERT_NOT_REACHED();"]), "}"]),
      "}",
    ],
    [
      `ALWAYS_INLINE uint32_t numberOfFieldWords(${enumName} ${tag}, const uint32_t* fields)`,
      "{",
      ...indent([
        `uint8_t fixed = CaseTable<${enumName}, ${sizeOf}>::at(${tag});`,
        "if (fixed != variableNumberOfFieldWords) [[likely]]",
        ...indent(["return fixed;"]),
        `return numberOfCountedFieldWords(${tag}, fields);`,
      ]),
      "}",
    ],
  );
}

function numberOfWordsBehindHeadFunction(operations: readonly Entry[]): string[] {
  const cases = operations.map(entry => {
    const fields = fixedNumberOfFieldWordsOf(entry);
    const words = fields === null ? variableSizeMarker : fields + (wireFlagsOf(entry).length > 0 ? 1 : 0);
    return { label: entry.name, value: words < variableSizeMarker ? String(words) : "variableNumberOfFieldWords" };
  });
  return blocks(
    constexprSwitch({
      signature: "uint8_t fixedNumberOfWordsBehindHeadOfBodyValue(Opcode opcode)",
      subject: "opcode",
      enumName: "Opcode",
      cases,
      defaultValue: "most common",
    }),
    [
      "ALWAYS_INLINE uint32_t numberOfWordsBehindHead(Opcode opcode, const uint32_t* words)",
      "{",
      ...indent([
        "uint8_t fixed = CaseTable<Opcode, fixedNumberOfWordsBehindHeadOfBodyValue>::at(opcode);",
        "if (fixed != variableNumberOfFieldWords) [[likely]]",
        ...indent(["return fixed;"]),
        "uint32_t numberOfFlagsWords = CaseTable<Opcode, hasFlagsWordOnWire>::at(opcode);",
        "return numberOfFlagsWords + numberOfFieldWords(opcode, words + numberOfFlagsWords);",
      ]),
      "}",
    ],
  );
}

function visitOperands(entry: Entry): string[] {
  const visits = placedOperandsOf(entry).map(({ part, offset }) =>
    part.isVariadic
      ? `visitVariadicBodyOperands(fields.operandsAt(${offset}), ${entry.namedOperands.length}, functor);`
      : `visitBodyOperand(fields.operandAt(${offset}), ${part.index}, functor);`,
  );
  return [...visits, "return;"];
}

function findOperand(entry: Entry): string[] {
  const operands = placedOperandsOf(entry);
  const named = operands.filter(({ part }) => !part.isVariadic);
  const variadic = operands.find(({ part }) => part.isVariadic);
  const first = entry.namedOperands.length;
  const otherwise =
    variadic === undefined
      ? "return { };"
      : `return variadicBodyOperandAt(fields.operandsAt(${variadic.offset}), ${first}, operandIndex);`;
  if (named.length === 0) return [otherwise];
  return [
    "switch (operandIndex) {",
    ...named.flatMap(({ part, offset }) => [`case ${part.index}:`, `    return fields.operandAt(${offset});`]),
    "default:",
    ...indent([otherwise]),
    "}",
  ];
}

const numberOfOperandsWithPlace = 4;
const placeOfAbsentOperand = 0xfe;
const noPlace = 0xff;

function placeOfOperand(entry: Entry, operandIndex: number): number {
  if (!entry.isWire) return noPlace;
  const operands = placedOperandsOf(entry);
  const named = operands.find(({ part }) => !part.isVariadic && part.index === operandIndex);
  if (named === undefined) {
    const isVariadic = operandIndex >= entry.namedOperands.length && operands.some(({ part }) => part.isVariadic);
    return isVariadic ? noPlace : placeOfAbsentOperand;
  }
  if (!/^\d+$/.test(named.offset)) return noPlace;
  const place = Number(named.offset) + (wireFlagsOf(entry).length > 0 ? 1 : 0);
  return place < placeOfAbsentOperand ? place : noPlace;
}

function placesOfOperands(entry: Entry): string {
  const places = Array.from({ length: numberOfOperandsWithPlace }, (_, index) => placeOfOperand(entry, index));
  return `0x${places
    .reverse()
    .map(place => place.toString(16).padStart(2, "0"))
    .join("")}`;
}

function operandByPlace(operations: readonly Entry[]): string[] {
  const everyPlaceIsUnknown = `0x${noPlace.toString(16).repeat(numberOfOperandsWithPlace)}`;
  return blocks(
    [
      `inline constexpr uint32_t numberOfBodyOperandsWithPlace = ${numberOfOperandsWithPlace};`,
      `inline constexpr uint32_t placeOfAbsentBodyOperand = ${placeOfAbsentOperand};`,
    ],
    constexprSwitch({
      signature: "uint32_t placesOfOperandsOfBodyValue(Opcode opcode)",
      subject: "opcode",
      enumName: "Opcode",
      cases: operations.map(entry => ({ label: entry.name, value: placesOfOperands(entry) })),
      defaultValue: everyPlaceIsUnknown,
    }),
    [
      "ALWAYS_INLINE ValueID BodyValue::operand(uint32_t operandIndex) const",
      "{",
      ...indent([
        "if (operandIndex < numberOfBodyOperandsWithPlace) [[likely]] {",
        ...indent([
          "uint32_t place = ColumnTable<Opcode, uint32_t, placesOfOperandsOfBodyValue>::at(m_opcode) >> (operandIndex * 8) & 0xff;",
          "if (place < placeOfAbsentBodyOperand) [[likely]]",
          ...indent(["return ValueID::fromBits(m_words[numberOfHeadWords + place]);"]),
          "if (place == placeOfAbsentBodyOperand)",
          ...indent(["return { };"]),
        ]),
        "}",
        "return operandFoundByCase(operandIndex);",
      ]),
      "}",
    ],
  );
}

function listVariadicOperands(entry: Entry): string[] {
  const variadic = placedOperandsOf(entry).find(({ part }) => part.isVariadic);
  return [variadic === undefined ? "return { };" : `return fields.operandsAt(${variadic.offset});`];
}

function readID(kind: Kind, isOptional: boolean, offset: string): string {
  return `fields.${isOptional ? "optionalIDAt" : "idAt"}<${engineTypeOf(kind)}>(${offset})`;
}

function referencesInRecord(
  entry: Entry,
  owner: PayloadField,
  record: SchemaRecord,
  firstWord = 0,
): ReferenceInRecord[] {
  const references: ReferenceInRecord[] = [];
  let word = firstWord;
  for (const field of record.wireFields) {
    if (holdsReferences(field.kind)) {
      if (field.arrayLength !== null || field.isOnePerList) {
        refuse(entry, owner, `${field.name} of ${record.name} is an array that holds ids`);
      }
      const translation = translationOf(entry, owner, field.kind);
      if (translation.tag === "Id") references.push({ word, kind: field.kind, isOptional: field.isOptional });
      if (translation.tag === "Record") references.push(...referencesInRecord(entry, owner, translation.record, word));
    }
    word += field.wireWords;
  }
  return references;
}

function visitReference(schema: Schema, kind: Kind, id: string): string {
  return `visitBodyReference(${id}, visitor${roleArgument(schema, kind)});`;
}

function visitReferenceAt(schema: Schema, kind: Kind, isOptional: boolean, offset: string): string {
  if (!isOptional) return visitReference(schema, kind, readID(kind, false, offset));
  return `visitOptionalBodyReference<${engineTypeOf(kind)}>(fields, ${offset}, visitor${roleArgument(schema, kind)});`;
}

function visitReferencesInRecord(schema: Schema, references: readonly ReferenceInRecord[], offset: string): string[] {
  return references.map(({ word, kind, isOptional }) =>
    visitReferenceAt(schema, kind, isOptional, offsetPlus(offset, word)),
  );
}

function loop(head: string, statements: Lines): string[] {
  if (statements.length === 1) return [head, ...indent(statements)];
  return [`${head} {`, ...indent(statements), "}"];
}

function visitReferencesOfField(schema: Schema, entry: Entry, { part, offset }: Placed<PayloadField>): string[] {
  const isList = part.kind.tag === "List";
  const element = part.kind.tag === "List" ? part.kind.element : part.kind;
  const translation = translationOf(entry, part, element);
  switch (translation.tag) {
    case "None":
      return [];
    case "Id":
      if (!isList) return [visitReferenceAt(schema, element, part.isOptional, offset)];
      return loop(`for (auto id : fields.listAt<${engineTypeOf(element)}>(${offset}))`, [
        visitReference(schema, element, "id"),
      ]);
    case "Record": {
      const references = referencesInRecord(entry, part, translation.record);
      if (!isList) return visitReferencesInRecord(schema, references, offset);
      if (references.length === 0) return [];
      const words = translation.record.wireWords;
      const bounds = `uint32_t element = ${offsetPlus(offset, 1)}, end = element + word(${offset}) * ${words}`;
      return loop(
        `for (${bounds}; element < end; element += ${words})`,
        visitReferencesInRecord(schema, references, "element"),
      );
    }
  }
}

function visitReferences(schema: Schema, entry: Entry): string[] {
  return [...placedPayloadOf(entry).flatMap(field => visitReferencesOfField(schema, entry, field)), "return;"];
}

function copyList(entry: Entry, field: PayloadField, element: Kind, offset: string): string {
  const firstWord = listFirstWord(entry);
  const list = `fields.listAt<${engineTypeOf(element)}>(${offset})`;
  switch (translationOf(entry, field, element).tag) {
    case "Id":
      return `copyBodyIDList(${list}, holder, ${firstWord});`;
    case "Record":
      return `copyBodyRecordList(${list}, holder, ${firstWord});`;
    case "None": {
      const words = payloadWordsOf(element);
      if (words === 1) return `holder.setPayloadListFrom(${firstWord}, fields.numbersAt(${offset}));`;
      const first = `fields.words() + ${offsetPlus(offset, 1)}`;
      return `holder.setPayloadListFrom(${firstWord}, unsafeMakeSpan(${first}, word(${offset}) * ${words}));`;
    }
  }
}

function copyPayload(entry: Entry, { part, offset }: Placed<PayloadField>): string[] {
  const { kind, firstWord } = part;
  if (kind.tag === "List") return [copyList(entry, part, kind.element, offset)];
  if (firstWord === null) throw new Error(`payload field ${part.name} of ${entry.name} has no payload word`);
  switch (translationOf(entry, part, kind).tag) {
    case "Id":
      return [`holder.setPayloadWordAt(${firstWord}, encodePayloadWord(${readID(kind, part.isOptional, offset)}));`];
    case "Record":
      return [`encodePayloadRecord(holder, ${firstWord}, fields.recordAt<${engineTypeOf(kind)}>(${offset}));`];
    case "None":
      return Array.from(
        { length: part.payloadWords },
        (_, index) => `holder.setPayloadWordAt(${firstWord + index}, word(${offsetPlus(offset, index)}));`,
      );
  }
}

function copyEntry(entry: Entry): string[] {
  const statements = placedParametersOf(entry).flatMap(({ part, offset }) => {
    if (part.tag === "Payload") return copyPayload(entry, { part, offset });
    if (part.isVariadic) return [`copyVariadicBodyOperands(fields.operandsAt(${offset}), holder, operandOf);`];
    return [`copyBodyOperand(fields.operandAt(${offset}), ${part.index}, holder, operandOf);`];
  });
  return [...statements, "return;"];
}

function walkersOf(schema: Schema): Walker[] {
  return [
    {
      name: "ForEachOperand",
      head: ({ className }) => [
        "template<typename Functor>",
        `void ${className}::forEachOperand(const Functor& functor) const`,
      ],
      isMember: true,
      answerForParameter: "return;",
      bodyOf: visitOperands,
    },
    {
      name: "Operand",
      head: ({ className }) => [
        `inline ValueID ${className}::${className === "BodyValue" ? "operandFoundByCase" : "operand"}(uint32_t operandIndex) const`,
      ],
      isMember: true,
      answerForParameter: "return { };",
      bodyOf: findOperand,
    },
    {
      name: "VariadicOperands",
      head: ({ className }) => [`inline std::span<const ValueID> ${className}::variadicOperands() const`],
      isMember: true,
      answerForParameter: "return { };",
      bodyOf: listVariadicOperands,
    },
    {
      name: "ForEachReference",
      head: ({ className }) => [
        "template<typename Visitor>",
        `void ${className}::forEachReference(Visitor&& visitor) const`,
      ],
      isMember: true,
      answerForParameter: "return;",
      bodyOf: entry => visitReferences(schema, entry),
    },
    {
      name: "CopyFields",
      head: ({ className, parameter }) => [
        "template<OperationHolder Holder, typename Functor>",
        `void copyFields(const ${className}& ${parameter}, Holder& holder, const Functor& operandOf)`,
      ],
      isMember: false,
      answerForParameter: "return;",
      bodyOf: copyEntry,
    },
  ];
}

function defineWalker(cursor: Cursor, walker: Walker): string[] {
  const { className, enumName, tag } = cursor;
  const isValue = className === "BodyValue";
  const receiver = walker.isMember ? "" : `${cursor.parameter}.`;
  const walked = cursor.entries.map(entry => ({ entry, body: entry.isWire ? walker.bodyOf(entry) : notOnWire }));
  const groups = [...Map.groupBy(walked, ({ body }) => body.join("\n")).values()];
  if (groups.length > (isValue ? markOfFlagsWordInCase : maxCasesPerTable)) {
    throw new Error(`${walker.name} of ${className} has more cases than its table can number`);
  }
  const caseOf = `caseOf${walker.name}Of${className}`;
  const markOf = (entry: Entry) => (isValue && entry.isWire && wireFlagsOf(entry).length > 0 ? markOfFlagsWordInCase : 0);
  const cases = groups.flatMap((group, index) =>
    group.map(({ entry }) => ({ label: entry.name, value: String(index + markOf(entry)) })),
  );
  const signature = `uint8_t ${caseOf}(${enumName} ${tag})`;
  const caseInTable = `CaseTable<${enumName}, ${caseOf}>::at(${receiver}${tag}())`;
  const head = isValue
    ? [
        `if (${receiver}isParameter())`,
        ...indent([walker.answerForParameter]),
        `uint8_t caseWithMark = ${caseInTable};`,
        `BodyFields fields = ${receiver}wordsBehindHead(caseWithMark / ${markOfFlagsWordInCase});`,
      ]
    : [`BodyFields fields = ${receiver}fieldWords();`];
  return blocks(constexprSwitch({ signature, subject: tag, enumName, cases, defaultValue: "most common" }), [
    ...walker.head(cursor),
    "{",
    ...indent([
      ...head,
      "[[maybe_unused]] auto word = [&fields](uint32_t offset) { return fields.word(offset); };",
      `switch (${isValue ? `caseWithMark % ${markOfFlagsWordInCase}` : caseInTable}) {`,
      ...groups.flatMap((group, index) => [`case ${index}:`, ...indent(group[0].body)]),
      "default:",
      ...indent(["WTF_UNREACHABLE();"]),
      "}",
    ]),
    "}",
  ]);
}

function bodyWalkersHeader(schema: Schema): string[] {
  for (const entry of schema.terminators) {
    if (!entry.isWire || wireFlagsOf(entry).length === 0) continue;
    const problem = "has flags on the wire, but BodyTerminator reads no flags word";
    throw new SchemaError(entry.location, `${entry.name} ${problem}`);
  }
  const cursors: readonly Cursor[] = [
    { className: "BodyValue", enumName: "Opcode", tag: "opcode", parameter: "value", entries: schema.operations },
    {
      className: "BodyTerminator",
      enumName: "TerminatorKind",
      tag: "kind",
      parameter: "terminator",
      entries: schema.terminators,
    },
  ];
  const body = blocks(
    helpers,
    ...cursors.map(numberOfFieldWordsFunction),
    numberOfWordsBehindHeadFunction(schema.operations),
    ...walkersOf(schema).flatMap(walker => cursors.map(cursor => defineWalker(cursor, walker))),
    operandByPlace(schema.operations),
  );
  return cppHeader(['"AOTBody.h"', "<algorithm>", "<type_traits>"], inNamespace("Typed", body));
}

export function emitCppBodyWalkers(context: EmitContext): OutputFile[] {
  return [{ name: "AOTSchemaBodyWalkers.h", lines: bodyWalkersHeader(context.schema) }];
}
