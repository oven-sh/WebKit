import { casePattern, mostWordsOfShortRecord } from "./emit-rust-records.ts";
import { kindText } from "./hash.ts";
import { blocks, indent, indentation, rustMarker } from "./lines.ts";
import type {
  Field,
  Kind,
  ProgramList,
  ScalarKind,
  Schema,
  SchemaRecord,
  SchemaType,
  Variant,
  VariantCase,
} from "./model.ts";
import { nameOf, wireWordsOf } from "./model.ts";
import { programListMemberName, rustIdentifier, rustTypeName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import {
  externalPathsOf,
  holdsVector,
  idsImport,
  listedTypesOf,
  toWords,
  viewImports,
  viewOfSlices,
} from "./rust-types.ts";

const scalarsStoredAsWords: ReadonlySet<ScalarKind> = new Set(["Word", "CodeWord", "UInt32", "Field"]);

interface WriterStatements {
  readonly counts: readonly string[];
  readonly writes: readonly string[];
}

function holdsType(list: ProgramList, type: SchemaType): boolean {
  const element = list.element;
  if (type.tag === "Record") return element.tag === "Record" && element.record === type.record;
  return element.tag === "Variant" && element.variant === type.variant;
}

function listMember(schema: Schema, element: Kind): string {
  if (element.tag === "Scalar" && element.scalar === "Byte") return "Bytes";
  if (element.tag === "Scalar" && element.scalar === "CodeWord") return "Code";
  if (element.tag !== "Record" && element.tag !== "Variant") return "Words";
  const type: SchemaType = element;
  const list = schema.lists.find(candidate => holdsType(candidate, type));
  if (list === undefined) throw new Error(`${nameOf(type)} has no list`);
  return programListMemberName(list.name);
}

function pushesRecords(kind: Kind): boolean {
  return kind.tag === "Span" || ((kind.tag === "Record" || kind.tag === "Variant") && holdsVector(kind));
}

function countField(schema: Schema, field: Field, value: string, byReference: boolean): string[] {
  const kind = field.kind;
  if (kind.tag !== "Span") return pushesRecords(kind) ? [`${value}.count_records(counts);`] : [];
  const count = `counts[ProgramList::${listMember(schema, kind.element)} as usize] += ${value}.len() as u64;`;
  if (!pushesRecords(kind.element)) return [count];
  return [count, `for element in ${byReference ? value : `&${value}`} { element.count_records(counts); }`];
}

function collectElementWords(element: Kind, value: string, source: string): string[] {
  if (element.tag === "Record" || element.tag === "Variant") {
    return [
      `let mut element_words = Vec::with_capacity(${value}.len() * ${wireWordsOf(element)});`,
      `for element in ${source} { element.write_record(sections, &mut element_words); }`,

    ];
  }
  const words = toWords(element, "element", false, false);
  if (words.length !== 1) throw new Error(`a span of ${kindText(element)} has no list`);
  return [
    `let mut element_words: SmallVec<[u32; 8]> = SmallVec::with_capacity(${value}.len());`,
    `for &element in ${source} { element_words.push(${words[0]}); }`,
  ];
}

function writeSpan(schema: Schema, element: Kind, value: string, byReference: boolean): string[] {
  const source = byReference ? value : `&${value}`;
  if (element.tag === "Scalar" && element.scalar === "Byte") return [`let span = sections.push_bytes(${source});`];
  const list = `ProgramList::${listMember(schema, element)}`;
  const push = (words: string) => `sections.push_records(${list}, ${words}, ${value}.len())`;
  if (element.tag === "Scalar" && scalarsStoredAsWords.has(element.scalar)) return [`let span = ${push(source)};`];
  return [
    `let span = if ${value}.is_empty() {`,
    `${indentation}WireSpan { first: sections.next_index(${list}), count: 0 }`,
    "} else {",
    ...indent([...collectElementWords(element, value, source), push("&element_words")]),
    "};",
  ];
}

function writeFields(
  schema: Schema,
  fields: readonly Field[],
  base: string,
  byReference: boolean,
  holdsSlices = byReference,
): string[] {
  const statements: string[] = [];
  let run: string[] = [];
  const flush = () => {
    statements.push(...run.map(word => `record.push(${word});`));
    run = [];
  };
  for (const field of fields) {
    const value = `${base}${rustIdentifier(field.name)}`;
    const kind = field.kind;
    if (kind.tag === "Span") {
      flush();
      statements.push(
        "{",
        ...indent([
          ...writeSpan(schema, kind.element, value, holdsSlices),
          "record.push(span.first);",
          "record.push(span.count);",
        ]),
        "}",
      );
    } else if (kind.tag === "Record" || kind.tag === "Variant") {
      flush();
      statements.push(`${value}.write_record(sections, record);`);
    } else {
      run.push(...toWords(kind, value, field.isOptional, byReference));
    }
  }
  flush();
  return statements;
}

function recordStatements(schema: Schema, record: SchemaRecord, holdsSlices: boolean): WriterStatements {
  const valueOf = (field: Field) => `self.${rustIdentifier(field.name)}`;
  return {
    counts: record.wireFields.flatMap(field => countField(schema, field, valueOf(field), holdsSlices)),
    writes: writeFields(schema, record.wireFields, "self.", false, holdsSlices),
  };
}

function countArm(schema: Schema, variant: Variant, variantCase: VariantCase): string[] {
  const countedFields = variantCase.fields.filter(field => pushesRecords(field.kind));
  const pattern = casePattern(variant, variantCase, countedFields);
  if (countedFields.length === 0) return [`${pattern} => {}`];
  return [
    `${pattern} => {`,
    ...indent(countedFields.flatMap(field => countField(schema, field, rustIdentifier(field.name), true))),
    "}",
  ];
}

function writeArm(schema: Schema, variant: Variant, variantCase: VariantCase): string[] {
  const padding = variant.wireWords - 1 - variantCase.fields.reduce((words, field) => words + field.wireWords, 0);
  return [
    `${casePattern(variant, variantCase)} => {`,
    ...indent([
      `record.push(${variantCase.number});`,
      ...writeFields(schema, variantCase.fields, "", true),
      ...(padding !== 0 ? [`for _ in 0..${padding} { record.push(0); }`] : []),
    ]),
    "}",
  ];
}

function variantStatements(schema: Schema, variant: Variant): WriterStatements {
  return {
    counts: [
      "match self {",
      ...indent(variant.cases.flatMap(variantCase => countArm(schema, variant, variantCase))),
      "}",
    ],
    writes: [
      "match self {",
      ...indent(variant.cases.flatMap(variantCase => writeArm(schema, variant, variantCase))),
      "}",
    ],
  };
}

function writerImpl(schema: Schema, type: SchemaType): string[] {
  const view = viewOfSlices(type);
  const { counts, writes } =
    type.tag === "Record"
      ? recordStatements(schema, type.record, view !== null)
      : variantStatements(schema, type.variant);
  const isShort = type.tag === "Record" && !holdsVector(type) && type.record.wireWords <= mostWordsOfShortRecord;
  return [
    view === null ? `impl ${rustTypeName(nameOf(type))} {` : `impl<'a> ${view}<'a> {`,
    `${indentation}pub fn count_records(&self, counts: &mut ListCounts) {`,
    ...indent(counts, 2),
    `${indentation}}`,
    ...(isShort ? [`${indentation}#[inline]`] : []),
    view === null
      ? `${indentation}pub fn write_record<'a>(&'a self, sections: &mut SectionWriter<'a>, record: &mut Vec<u32>) {`
      : `${indentation}pub fn write_record(&self, sections: &mut SectionWriter<'a>, record: &mut Vec<u32>) {`,
    ...indent(writes, 2),
    `${indentation}}`,
    "}",
  ];
}

function recordWriterRs(schema: Schema): string[] {
  const types = listedTypesOf(schema);
  return [
    rustMarker,
    "",
    "use smallvec::SmallVec;",
    "",
    "use super::entries::{OpaqueKind, ProgramList};",
    "use super::enums::*;",
    "use super::program_enums::*;",
    "use super::records::*;",
    ...externalPathsOf(types).map(path => `use ${path};`),
    idsImport,
    "use crate::ir::sections::{ListCounts, SectionWriter, WireSpan};",
    ...viewImports(types),
    "",
    ...blocks(...types.map(type => writerImpl(schema, type))),
  ];
}

export function emitRustWriter(context: EmitContext): OutputFile[] {
  return [{ name: "record_writer.rs", lines: recordWriterRs(context.schema) }];
}
