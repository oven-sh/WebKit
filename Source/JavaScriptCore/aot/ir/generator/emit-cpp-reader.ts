import {
  engineFieldsOf,
  engineStructNameOf,
  engineTypeOf,
  hasEngineStruct,
  sectionElementTypeOf,
  twoWordScalars,
} from "./cpp-types.ts";
import { kindText } from "./hash.ts";
import { blocks, cppHeader, cppSource, indent, inNamespace } from "./lines.ts";
import type { Field, Kind, ScalarKind, Schema, SchemaRecord, SchemaType, Variant } from "./model.ts";
import { innermostKindOf, isNested, nameOf } from "./model.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { wireFieldsOf, wireTypesOf } from "./rust-types.ts";
import { SchemaError } from "./source.ts";

export { innermostKindOf as innermost } from "./model.ts";

const borrowedSpanReaders: Readonly<Partial<Record<ScalarKind, string>>> = {
  Byte: "bytes",
  Word: "words",
  CodeWord: "code",
};

const recursionHelpers = `// Nested records are read by recursion, before any verifier has seen them.
// A cycle among the spans would recurse forever, and spans that share records could be read exponentially often.
struct RecursiveRead {
    // Must equal MaxDataNestingDepth in limits.def, which the front end enforces.
    static constexpr uint32_t maxDepth = 2000;

    void enterSpan(size_t numberOfRecords)
    {
        RELEASE_ASSERT(depth < maxDepth);
        RELEASE_ASSERT(numberOfRecords <= remainingRecords);
        remainingRecords -= numberOfRecords;
        ++depth;
    }

    void leaveSpan() { --depth; }

    uint64_t remainingRecords { 0 };
    uint32_t depth { 0 };
};`.split("\n");

export function convertedTypes(schema: Schema): SchemaType[] {
  return wireTypesOf(schema).filter(hasEngineStruct);
}

function readerHeader(schema: Schema): string[] {
  return cppHeader(
    [],
    inNamespace(
      "Typed",
      convertedTypes(schema).map(
        type => `void read(RecordChecker&, const Wire::${nameOf(type)}&, ${engineStructNameOf(type)}&);`,
      ),
    ),
  );
}

export function isWireId(schema: Schema, name: string): boolean {
  return schema.idKinds.some(id => id.name === name && !id.isEngine);
}

function checkedWord(kind: Kind, isOptional: boolean, word: string): string {
  const type = engineTypeOf(kind);
  switch (kind.tag) {
    case "Id":
      if (kind.id.isEngine) return `${type}(${word})`;
      return `checker.${isOptional ? "optionalID" : "id"}<${type}>(${word})`;
    case "External": {
      const optionalReader = kind.external.form === "id" ? "optionalExternalID" : "optionalExternalEnumerator";
      return `checker.${isOptional ? optionalReader : "external"}<${type}>(${word})`;
    }
    case "Enum":
      return `checker.${isOptional ? "optionalEnumerator" : "enumerator"}<${type}>(${word})`;
    case "Flags":
      return `checker.flags<${kind.type.name}>(${word})`;
    case "PlacedWord":
      return `${type}(${word})`;
    case "Scalar":
      if (kind.scalar === "Bool") return `checker.boolean(${word})`;
      if (kind.scalar === "UInt32" || kind.scalar === "Field")
        return isOptional ? `checker.optionalNumber(${word})` : word;
      break;
    case "Record":
    case "Variant":
    case "Span":
    case "List":
      break;
  }
  throw new Error(`${kindText(kind)} is not one word`);
}

function containedTypes(type: SchemaType): SchemaType[] {
  return wireFieldsOf(type).flatMap(field => {
    const inner = innermostKindOf(field.kind);
    return isNested(inner) ? [inner] : [];
  });
}

function recursiveTypes(schema: Schema): SchemaType[] {
  const reaches = (start: string, type: SchemaType, seen: Set<string>): boolean =>
    containedTypes(type).some(inner => {
      const name = nameOf(inner);
      if (name === start) return true;
      if (seen.has(name)) return false;
      seen.add(name);
      return reaches(start, inner, seen);
    });
  return convertedTypes(schema).filter(type => reaches(nameOf(type), type, new Set()));
}

function readSpan(recursiveCallees: readonly string[], field: Field, source: string, target: string): string[] {
  const element = innermostKindOf(field.kind);
  const borrowedReader = element.tag === "Scalar" ? borrowedSpanReaders[element.scalar] : undefined;
  if (borrowedReader !== undefined) return [`${target} = checker.${borrowedReader}(${source});`];
  if (!isNested(element)) {
    return [
      "{",
      `    auto words = checker.words(${source});`,
      `    ${target}.reserveInitialCapacity(words.size());`,
      "    for (uint32_t word : words)",
      `        ${target}.append(${checkedWord(element, false, "word")});`,
      "}",
    ];
  }
  const isRecursive = recursiveCallees.includes(nameOf(element));
  return [
    "{",
    `    auto elements = checker.slice<Wire::${nameOf(element)}>(${source});`,
    ...(isRecursive ? ["    recursion.enterSpan(elements.size());"] : []),
    `    ${target}.reserveInitialCapacity(elements.size());`,
    `    ${target}.grow(elements.size());`,
    "    for (size_t i = 0; i < elements.size(); ++i)",
    `        read(checker, elements[i], ${target}[i]${isRecursive ? ", recursion" : ""});`,
    ...(isRecursive ? ["    recursion.leaveSpan();"] : []),
    "}",
  ];
}

function readField(recursiveCallees: readonly string[], field: Field): string[] {
  const kind = field.kind;
  const source = `wire.${field.name}`;
  const target = `record.${field.name}`;
  if (kind.tag === "Span") return readSpan(recursiveCallees, field, source, target);
  if (isNested(kind))
    return [`read(checker, ${source}, ${target}${recursiveCallees.includes(nameOf(kind)) ? ", recursion" : ""});`];
  if (kind.tag === "Scalar" && kind.scalar === "Double")
    return [`${target} = std::bit_cast<double>(static_cast<uint64_t>(${source}));`];
  if (kind.tag === "Scalar" && twoWordScalars.has(kind.scalar)) return [`${target} = ${source};`];
  return [`${target} = ${checkedWord(kind, field.isOptional, source)};`];
}

function readSignature(recursive: readonly string[], type: SchemaType): string {
  const parameters = `RecordChecker& checker, const Wire::${nameOf(type)}& wire, ${engineStructNameOf(type)}& record`;
  return recursive.includes(nameOf(type))
    ? `static void read(${parameters}, RecursiveRead& recursion)`
    : `void read(${parameters})`;
}

function readRecord(recursive: readonly string[], record: SchemaRecord): string[] {
  const recursiveCallees = recursive.includes(record.name) ? recursive : [];
  const fields = engineFieldsOf(record).filter(field => !field.isEngine);
  return [
    readSignature(recursive, { tag: "Record", record }),
    "{",
    ...indent(fields.flatMap(field => readField(recursiveCallees, field))),
    "    checker.didRead(record);",
    "}",
  ];
}

export function caseWords(schema: Schema, fields: readonly Field[]): [Field, number][] {
  const positions: [Field, number][] = [];
  let word = 0;
  for (const field of fields) {
    positions.push([field, word]);
    word += field.wireWords;
  }
  return positions;
}

function readCaseField(recursiveCallees: readonly string[], variant: Variant, field: Field, word: number): string[] {
  const kind = field.kind;
  const target = `value.${field.name}`;
  const low = `wire.words[${word}]`;
  const high = `wire.words[${word + 1}]`;
  if (kind.tag === "Span") return readSpan(recursiveCallees, field, `Wire::Span { ${low}, ${high} }`, target);
  if (isNested(kind)) {
    throw new SchemaError(
      field.location,
      `field ${field.name} of ${variant.name}: a case of a wire variant cannot hold ${nameOf(kind)}`,
    );
  }
  if (kind.tag === "Scalar" && kind.scalar === "Double")
    return [`${target} = std::bit_cast<double>(static_cast<uint64_t>(${high}) << 32 | ${low});`];
  if (kind.tag === "Scalar" && twoWordScalars.has(kind.scalar)) {
    return [`${target} = static_cast<${engineTypeOf(kind)}>(static_cast<uint64_t>(${high}) << 32 | ${low});`];
  }
  return [`${target} = ${checkedWord(kind, field.isOptional, low)};`];
}

function readVariant(schema: Schema, recursive: readonly string[], variant: Variant): string[] {
  const recursiveCallees = recursive.includes(variant.name) ? recursive : [];
  const cases = variant.cases.flatMap(variantCase => {
    const label = `case static_cast<uint32_t>(${variant.name}::Kind::${variantCase.name}):`;
    const caseType = `${variant.name}::${variantCase.name}`;
    if (variantCase.fields.length === 0) return [label, `    record = ${caseType} { };`, "    break;"];
    return [
      `${label} {`,
      `    ${caseType} value;`,
      ...indent(
        caseWords(schema, variantCase.fields).flatMap(([field, word]) =>
          readCaseField(recursiveCallees, variant, field, word),
        ),
      ),
      "    record = WTF::move(value);",
      "    break;",
      "}",
    ];
  });
  return [
    readSignature(recursive, { tag: "Variant", variant }),
    "{",
    "    switch (wire.kind) {",
    ...indent(cases),
    "    default:",
    "        RELEASE_ASSERT_NOT_REACHED();",
    "    }",
    "    checker.didRead(record);",
    "}",
  ];
}

function recursionSupport(recursive: readonly SchemaType[]): string[] {
  if (recursive.length === 0) return [];
  return [
    ...recursionHelpers,
    "",
    ...recursive.map(
      type =>
        `static void read(RecordChecker&, const Wire::${nameOf(type)}&, ${engineStructNameOf(type)}&, RecursiveRead&);`,
    ),
  ];
}

function recursionBudget(schema: Schema, recursive: readonly string[]): string {
  const lists = recursive.flatMap(name => schema.lists.find(list => sectionElementTypeOf(list.element) === name) ?? []);
  const sizes = lists.map(list => `sections.${list.name}.size()`);
  return sizes.map((size, index) => (index === 0 ? `static_cast<uint64_t>(${size})` : size)).join(" + ");
}

function recursionEntry(type: SchemaType, budget: string): string[] {
  return [
    readSignature([], type),
    "{",
    "    const Wire::Sections& sections = checker.reader().sections();",
    `    RecursiveRead recursion { ${budget} };`,
    "    read(checker, wire, record, recursion);",
    "}",
  ];
}

function recordReaderSource(schema: Schema): string[] {
  const recursiveStructs = recursiveTypes(schema);
  const recursive = recursiveStructs.map(type => nameOf(type));
  const budget = recursionBudget(schema, recursive);
  const functions = convertedTypes(schema).flatMap(type => {
    const read =
      type.tag === "Record" ? readRecord(recursive, type.record) : readVariant(schema, recursive, type.variant);
    return recursive.includes(nameOf(type)) ? [read, recursionEntry(type, budget)] : [read];
  });
  return cppSource(
    ['"AOTReaderInternal.h"', "<bit>"],
    inNamespace("Typed", blocks(recursionSupport(recursiveStructs), ...functions)),
  );
}

export function emitCppReader(context: EmitContext): OutputFile[] {
  return [
    { name: "AOTSchemaReader.h", lines: readerHeader(context.schema) },
    { name: "AOTSchemaRecordReader.cpp", lines: recordReaderSource(context.schema) },
  ];
}
