import { blocks, cppHeader, indent } from "./lines.ts";
import type { PackedCondition, PackedField, PackedRecord, PackedWord, Schema, SchemaEnum } from "./model.ts";
import { cppColumnFunctionName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { fieldsOfWord, isFoundByColumns } from "./resolve-packed.ts";

const bitsInWord = 64;
const bitsInByteOfVarint = 7;

function fieldsOf(record: PackedRecord): PackedField[] {
  return record.words.flatMap(fieldsOfWord);
}

function typeOf({ kind }: PackedField): string {
  switch (kind.tag) {
    case "Bool":
      return "bool";
    case "CodeOffset":
      return "uint32_t";
    case "Signed":
      return "int64_t";
    case "Number":
    case "Id":
    case "ExternalEnum":
      return kind.cppType;
    case "Enum":
      return kind.type.name;
    case "Record":
      return kind.element.name;
    case "List":
      return `Vector<${kind.element.name}, 1>`;
    case "Bytes":
      return "std::span<const uint8_t>";
  }
}

function initializerOf({ kind }: PackedField): string {
  switch (kind.tag) {
    case "Bool":
      return " { false }";
    case "CodeOffset":
    case "Signed":
    case "Number":
      return " { 0 }";
    case "Enum":
      return ` { ${kind.type.name}::${kind.type.members[0].name} }`;
    case "Id":
    case "ExternalEnum":
    case "Record":
    case "Bytes":
      return " { }";
    case "List":
      return "";
  }
}

function conditionText({ field, column, isNegated }: PackedCondition, owner: string): string {
  const text =
    column === null ? `${owner}${field.name}` : `${cppColumnFunctionName(column.name)}(${owner}${field.name})`;
  return isNegated ? `!${text}` : text;
}

function saysNothingText({ name, kind }: PackedField): string {
  switch (kind.tag) {
    case "List":
      return `${name}.isEmpty()`;
    case "Bytes":
      return `${name}.empty()`;
    case "Enum":
      return `${name} == ${kind.type.name}::${kind.type.members[0].name}`;
    case "Record":
      return `${name} == ${kind.element.name} { }`;
    case "ExternalEnum":
      return `${name} == ${kind.cppType} { }`;
    default:
      return `!${name}`;
  }
}

function rawBitsOf({ name, kind }: PackedField): string {
  if (kind.tag === "Id") return `static_cast<uint64_t>(${name}.index())`;
  if (kind.tag === "CodeOffset") return `static_cast<uint64_t>(${name} / codeOffsetUnit)`;
  if (kind.tag === "Signed") return `zigZagEncode(${name})`;
  return kind.tag === "Number" && kind.cppType === "uint64_t" ? name : `static_cast<uint64_t>(${name})`;
}

/** How many bits a value of the kind can need where nothing bounds it. */
function bitsOfKind(kind: PackedField["kind"]): number {
  return kind.tag === "Signed" || (kind.tag === "Number" && kind.cppType === "uint64_t") ? bitsInWord : 32;
}

function mayBeWrittenEmpty({ kind }: PackedField): boolean {
  return kind.tag === "Number" || kind.tag === "CodeOffset" || kind.tag === "Signed" || kind.tag === "Record";
}

/** Nothing says anything where it is not written. But for numbers and rows, what is written says something. */
function checksOf(word: PackedWord): string[] {
  const all = fieldsOfWord(word);
  const offsets = all.filter(field => field.kind.tag === "CodeOffset");
  const wide = all.filter(({ kind, shift, width }) => width === null && bitsOfKind(kind) + shift > bitsInWord);
  const narrow = all.filter(({ kind, width }) => width !== null && kind.tag !== "Bool" && kind.tag !== "Enum");
  const condition = word.condition;
  return [
    ...offsets.map(({ name }) => `RELEASE_ASSERT(!(${name} % codeOffsetUnit));`),
    ...wide.map(field => `RELEASE_ASSERT(!(${rawBitsOf(field)} >> ${bitsInWord - field.shift}));`),
    ...narrow.map(field => `RELEASE_ASSERT(!(${rawBitsOf(field)} >> ${field.width}));`),
    ...(condition === null ? [] : word.fields).map(field => {
      const isWritten = conditionText(condition!, "");
      return `RELEASE_ASSERT(${isWritten} ${mayBeWrittenEmpty(field) ? "||" : "!="} ${saysNothingText(field)});`;
    }),
    ...(word.arms?.whenTrue ?? []).map(
      field => `RELEASE_ASSERT(${word.arms!.condition.name} || ${saysNothingText(field)});`,
    ),
    ...(word.arms?.whenFalse ?? []).map(
      field => `RELEASE_ASSERT(!${word.arms!.condition.name} || ${saysNothingText(field)});`,
    ),
  ];
}

function bitsOf(field: PackedField): string {
  return field.shift ? `${rawBitsOf(field)} << ${field.shift}` : rawBitsOf(field);
}

/** The lines of all words. Neighbors that are there under one condition share its test. */
function guardedLines(
  words: readonly PackedWord[],
  owner: string,
  linesOf: (word: PackedWord, position: number) => string[],
): string[] {
  const groups: { test: string | null; body: string[] }[] = [];
  words.forEach((word, position) => {
    const test = word.condition === null ? null : conditionText(word.condition, owner);
    const last = groups.at(-1);
    if (last !== undefined && last.test === test) last.body.push(...linesOf(word, position));
    else groups.push({ test, body: linesOf(word, position) });
  });
  return groups.flatMap(({ test, body }) => {
    if (test === null) return body;
    return body.length === 1 ? [`if (${test})`, ...indent(body)] : [`if (${test}) {`, ...indent(body), "}"];
  });
}

function appendLinesOf(word: PackedWord): string[] {
  const [first] = word.fields;
  if (first.kind.tag === "List") {
    return [
      `appendVarint(bytes, ${first.name}.size());`,
      `for (const ${first.kind.element.name}& element : ${first.name})`,
      ...indent(["element.append(bytes);"]),
    ];
  }
  if (first.kind.tag === "Bytes") return [`appendVarint(bytes, ${first.name}.size());`, `bytes.append(${first.name});`];
  if (first.kind.tag === "Record") return [`${first.name}.append(bytes);`];
  const common = word.fields.map(bitsOf).join(" | ");
  if (word.arms === null) return [`appendVarint(bytes, ${common});`];
  const { condition, whenTrue, whenFalse } = word.arms;
  const [taken, other] = [whenTrue, whenFalse].map(fields => fields.map(bitsOf).join(" | "));
  return [`appendVarint(bytes, ${common} | (${condition.name} ? ${taken} : ${other}));`];
}

function valueFrom(field: PackedField, word: string): string {
  const { kind, shift, width } = field;
  const shifted = shift ? `${word} >> ${shift}` : word;
  const bits = width === null ? shifted : `${shifted} & ${(1n << BigInt(width)) - 1n}`;
  switch (kind.tag) {
    case "Bool":
      return `!!(${bits})`;
    case "CodeOffset":
      return `static_cast<uint32_t>(${shift ? `(${bits})` : bits} * codeOffsetUnit)`;
    case "Id":
      return `${kind.cppType}(static_cast<uint32_t>(${bits}))`;
    case "Signed":
      return `zigZagDecode(${bits})`;
    case "Number":
      if (kind.cppType === "uint64_t") return bits;
      break;
    default:
      break;
  }
  return `static_cast<${typeOf(field)}>(${bits})`;
}

function readLinesOf(word: PackedWord, position: number): string[] {
  const [first] = word.fields;
  if (first.kind.tag === "List") {
    return [
      "for (uint64_t count = readVarint(cursor); count--;)",
      ...indent([`result.${first.name}.append(${first.kind.element.name}::read(cursor));`]),
    ];
  }
  if (first.kind.tag === "Bytes") {
    return [
      `size_t ${first.name}Size = static_cast<size_t>(readVarint(cursor));`,
      `result.${first.name} = unsafeMakeSpan(cursor, ${first.name}Size);`,
      `cursor += ${first.name}Size;`,
    ];
  }
  if (first.kind.tag === "Record") return [`result.${first.name} = ${first.kind.element.name}::read(cursor);`];
  if (word.fields.length === 1 && word.arms === null)
    return [`result.${first.name} = ${valueFrom(first, "readVarint(cursor)")};`];
  const name = position ? `word${position + 1}` : "word";
  const reads = (fields: readonly PackedField[]) =>
    fields.map(field => `result.${field.name} = ${valueFrom(field, name)};`);
  const common = [`uint64_t ${name} = readVarint(cursor);`, ...reads(word.fields)];
  if (word.arms === null) return common;
  const { condition, whenTrue, whenFalse } = word.arms;
  const [open, close] = whenTrue.length > 1 || whenFalse.length > 1 ? [" {", ["}"]] : ["", []];
  return [
    ...common,
    `if (result.${condition.name})${open}`,
    ...indent(reads(whenTrue)),
    `${close.length > 0 ? "} " : ""}else${open}`,
    ...indent(reads(whenFalse)),
    ...close,
  ];
}

/** Null: the row holds as many rows or bytes as it likes. */
function maxSizeOf(record: PackedRecord): number | null {
  let size = 0;
  const endOf = ({ shift, width, kind }: PackedField) => shift + (width ?? bitsOfKind(kind));
  for (const word of record.words) {
    const last = fieldsOfWord(word).reduce((widest, field) => (endOf(field) > endOf(widest) ? field : widest));
    if (last.kind.tag === "List" || last.kind.tag === "Bytes") return null;
    const nested = last.kind.tag === "Record" ? maxSizeOf(last.kind.element) : 0;
    if (nested === null) return null;
    const bits = Math.min(bitsInWord, last.shift + (last.width ?? bitsOfKind(last.kind)));
    size += last.kind.tag === "Record" ? nested : Math.ceil(bits / bitsInByteOfVarint);
  }
  return size;
}

function isComparableRecord(record: PackedRecord): boolean {
  return fieldsOf(record).every(({ kind }) => {
    if (kind.tag === "Record" || kind.tag === "List") return isComparableRecord(kind.element);
    return kind.tag !== "Bytes";
  });
}

function structLines(record: PackedRecord): string[] {
  const { name, words } = record;
  const isComparable = isComparableRecord(record);
  const equality = `friend bool operator==(const ${name}&, const ${name}&) = default;`;
  const maxSize = maxSizeOf(record);
  const body = blocks(
    maxSize === null ? [] : [`static constexpr unsigned maxSize = ${maxSize};`],
    isComparable ? [equality] : [],
    [
      "void append(Vector<uint8_t>& bytes) const",
      "{",
      ...indent([...words.flatMap(checksOf), ...guardedLines(words, "", appendLinesOf)]),
      "}",
    ],
    [
      `static ${name} read(const uint8_t*& cursor)`,
      "{",
      ...indent([`${name} result;`, ...guardedLines(words, "result.", readLinesOf), "return result;"]),
      "}",
    ],
    fieldsOf(record).map(field => `${typeOf(field)} ${field.name}${initializerOf(field)};`),
  );
  return [`struct ${name} {`, ...indent(body), "};"];
}

function lookupLines(type: SchemaEnum): string[] {
  const { name, columns } = type;
  const values = columns.map(column => `${cppColumnFunctionName(column.name)}(value)`).join(", ");
  return blocks(
    [
      `struct ${name}Columns {`,
      ...indent(
        blocks(
          [`friend constexpr bool operator==(const ${name}Columns&, const ${name}Columns&) = default;`],
          columns.map(column => `bool ${column.name} { false };`),
        ),
      ),
      "};",
    ],
    [`constexpr ${name}Columns columnsOf(${name} value) { return { ${values} }; }`],
    [
      `constexpr std::optional<${name}> find${name}(const ${name}Columns& columns)`,
      "{",
      `    for (unsigned number = 0; number < numberOfEnumerators<${name}>; ++number) {`,
      `        if (columnsOf(static_cast<${name}>(number)) == columns)`,
      `            return static_cast<${name}>(number);`,
      "    }",
      "    return std::nullopt;",
      "}",
    ],
  );
}

function enumsIn(schema: Schema): SchemaEnum[] {
  const kinds = schema.packedRecords.flatMap(fieldsOf).map(field => field.kind);
  return [...new Set(kinds.flatMap(kind => (kind.tag === "Enum" ? [kind.type] : [])))];
}

export function emitCppPacked({ schema }: EmitContext): OutputFile[] {
  const enums = enumsIn(schema);
  const body = blocks(
    ["WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN"],
    ["namespace JSC { namespace AOT {"],
    enums.map(type => `using Typed::${type.name};`),
    ...enums.filter(isFoundByColumns).map(lookupLines),
    ...schema.packedRecords.map(structLines),
    ["} } // namespace JSC::AOT"],
    ["WTF_ALLOW_UNSAFE_BUFFER_USAGE_END"],
  );
  const includes = [
    '"AOTImageFormat.h"',
    '"AOTRecordTypes.h"',
    '"AOTVarint.h"',
    '"HandlerInfo.h"',
    "<optional>",
    "<span>",
  ];
  return [{ name: "AOTSchemaPackedRecords.h", lines: cppHeader(includes, body) }];
}
