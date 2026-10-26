import { blocks, cppHeader, indent } from "./lines.ts";
import type { PackedCondition, PackedField, PackedRecord, PackedWord, Schema, SchemaEnum } from "./model.ts";
import { cppColumnFunctionName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { isFoundByColumns } from "./resolve-packed.ts";

const bitsInWord = 64;

function fieldsOf(record: PackedRecord): PackedField[] {
  return record.words.flatMap(word => word.fields);
}

function typeOf({ kind }: PackedField): string {
  switch (kind.tag) {
    case "Bool":
      return "bool";
    case "CodeOffset":
      return "uint32_t";
    case "Number":
    case "Id":
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
    case "Number":
      return " { 0 }";
    case "Enum":
      return ` { ${kind.type.name}::${kind.type.members[0].name} }`;
    case "Id":
    case "Record":
    case "Bytes":
      return " { }";
    case "List":
      return "";
  }
}

function conditionText({ field, column }: PackedCondition, owner: string): string {
  return column === null ? `${owner}${field.name}` : `${cppColumnFunctionName(column.name)}(${owner}${field.name})`;
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
    default:
      return `!${name}`;
  }
}

function mayBeWrittenEmpty({ kind }: PackedField): boolean {
  return kind.tag === "Number" || kind.tag === "CodeOffset" || kind.tag === "Record";
}

/** Nothing says anything where it is not written. But for numbers and rows, what is written says something. */
function checksOf(word: PackedWord): string[] {
  const offsets = word.fields.filter(field => field.kind.tag === "CodeOffset");
  const wide = word.fields.filter(({ kind, shift }) => kind.tag === "Number" && kind.cppType === "uint64_t" && shift);
  const condition = word.condition;
  return [
    ...offsets.map(({ name }) => `RELEASE_ASSERT(!(${name} % codeOffsetUnit));`),
    ...wide.map(({ name, shift }) => `RELEASE_ASSERT(!(${name} >> ${bitsInWord - shift}));`),
    ...(condition === null ? [] : word.fields).map(field => {
      const isWritten = conditionText(condition!, "");
      return `RELEASE_ASSERT(${isWritten} ${mayBeWrittenEmpty(field) ? "||" : "!="} ${saysNothingText(field)});`;
    }),
  ];
}

function bitsOf({ name, kind, shift }: PackedField): string {
  const value =
    kind.tag === "Id"
      ? `static_cast<uint64_t>(${name}.index())`
      : kind.tag === "CodeOffset"
        ? `static_cast<uint64_t>(${name} / codeOffsetUnit)`
        : kind.tag === "Number" && kind.cppType === "uint64_t"
          ? name
          : `static_cast<uint64_t>(${name})`;
  return shift ? `${value} << ${shift}` : value;
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
  return [`appendVarint(bytes, ${word.fields.map(bitsOf).join(" | ")});`];
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
  if (word.fields.length === 1) return [`result.${first.name} = ${valueFrom(first, "readVarint(cursor)")};`];
  const name = position ? `word${position + 1}` : "word";
  return [
    `uint64_t ${name} = readVarint(cursor);`,
    ...word.fields.map(field => `result.${field.name} = ${valueFrom(field, name)};`),
  ];
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
  const body = blocks(
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
  const includes = ['"AOTImageFormat.h"', '"AOTRecordTypes.h"', '"AOTVarint.h"', "<optional>", "<span>"];
  return [{ name: "AOTSchemaPackedRecords.h", lines: cppHeader(includes, body) }];
}
