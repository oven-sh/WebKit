import { casePattern } from "./emit-rust-records.ts";
import { kindText } from "./hash.ts";
import { blocks, indent, indentation, pack, rustMarker } from "./lines.ts";
import type { Field, Kind, Schema, SchemaRecord, SchemaType, Variant, VariantCase } from "./model.ts";
import { rustIdentifier, rustTypeName, snakeCase } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";
import { externalPathsOf, idsImport, listedTypesOf, rustLiteral } from "./rust-types.ts";

function textTypes(schema: Schema): SchemaType[] {
  return listedTypesOf(schema).filter(type => !(type.tag === "Record" && type.record.isCustomText));
}

function followsFields(field: Field): boolean {
  return field.kind.tag === "Record" && field.kind.record.isCustomText;
}

function isByte(kind: Kind): boolean {
  return kind.tag === "Scalar" && kind.scalar === "Byte";
}

function failWithoutSpelling(kind: Kind): never {
  throw new Error(`${kindText(kind)} has no spelling in the text of a record`);
}

export function canBeZero(schema: Schema, type: SchemaType): boolean {
  const fields = type.tag === "Record" ? type.record.wireFields : type.variant.cases[0].fields;
  return fields.every(field => canBeZeroField(schema, field));
}

function canBeZeroField(schema: Schema, field: Field): boolean {
  const kind = field.kind;
  switch (kind.tag) {
    case "Id":
      return field.isOptional;
    case "External":
      return field.isOptional || kind.external.form === "enum";
    case "Record":
    case "Variant":
      return !followsFields(field) && canBeZero(schema, kind);
    case "Scalar":
    case "PlacedWord":
    case "Enum":
    case "Flags":
    case "Span":
    case "List":
      return true;
  }
}

function printValue(schema: Schema, kind: Kind, value: string): string {
  switch (kind.tag) {
    case "Id":
      if (kind.id.record?.isCustomText) return `printer.print_${snakeCase(kind.id.name)}(${value});`;
      return `print_display(printer, ${value});`;
    case "External":
      if (kind.external.isNamedInText) return `printer.out.push_str(${value}.name());`;
      return `print_display(printer, ${value}.to_wire());`;
    case "Enum":
      return `printer.out.push_str(${value}.name());`;
    case "Flags":
      return `print_flags(printer, &${rustTypeName(kind.type.name)}::NAMES, ${value}.0);`;
    case "Record":
      return `${value}.print_nested(printer);`;
    case "Variant":
      return `${value}.print_variant(printer);`;
    case "Scalar":
      if (kind.scalar === "Bool") return `printer.out.push_str(if ${value} { "true" } else { "false" });`;
      return kind.scalar === "Double" ? `printer.print_double(${value});` : `print_display(printer, ${value});`;
    case "PlacedWord":
    case "Span":
    case "List":
      return failWithoutSpelling(kind);
  }
}

function zeroTest(schema: Schema, field: Field, value: string, byReference: boolean): string | null {
  const kind = field.kind;
  const plain = byReference ? `*${value}` : value;
  if (kind.tag === "Span") return `${value}.is_empty()`;
  if (kind.tag === "Record" || kind.tag === "Variant") return canBeZero(schema, kind) ? `${value}.is_zero()` : null;
  if (field.isOptional) return `${value}.is_none()`;
  switch (kind.tag) {
    case "Id":
      return null;
    case "External":
      return kind.external.form === "id" ? null : `${value}.to_wire() == 0`;
    case "Enum":
      return `${plain} == ${rustTypeName(kind.type.name)}::default()`;
    case "Flags":
      return `${value}.is_empty()`;
    case "Scalar":
      if (kind.scalar === "Bool") return `!${plain}`;
      return kind.scalar === "Double" ? `${value}.to_bits() == 0` : `${plain} == 0`;
    case "PlacedWord":
    case "List":
      return failWithoutSpelling(kind);
  }
}

function negated(test: string): string {
  if (test.startsWith("!")) return test.slice(1);
  if (test.includes(" == ")) return test.replaceAll(" == ", " != ");
  return `!${test}`;
}

function printedOperand(kind: Kind, value: string, byReference: boolean): string {
  const standsAlone = kind.tag === "Id" || kind.tag === "Scalar";
  return byReference && standsAlone ? `*${value}` : value;
}

function printField(schema: Schema, field: Field, value: string, byReference: boolean): string[] {
  const kind = field.kind;
  const begin = `begin_field(printer, &mut needs_blank, ${rustLiteral(field.name)});`;
  const printedIf = (condition: string, statements: readonly string[]) => [
    `if ${condition} {`,
    ...indent([begin, ...statements]),
    "}",
  ];
  if (kind.tag === "Span") {
    const source = byReference ? value : `&${value}`;
    if (isByte(kind.element)) return printedIf(`!${value}.is_empty()`, [`printer.print_bytes(${source});`]);
    const printElement = printValue(schema, kind.element, printedOperand(kind.element, "element", true)).slice(0, -1);
    return printedIf(`!${value}.is_empty()`, [
      "printer.out.push('[');",
      `printer.print_separated(${source}, |printer, element| ${printElement});`,
      "printer.out.push(']');",
    ]);
  }
  if (field.isOptional)
    return printedIf(`let Some(value) = ${byReference ? `*${value}` : value}`, [printValue(schema, kind, "value")]);
  const statement = printValue(schema, kind, printedOperand(kind, value, byReference));
  const test = zeroTest(schema, field, value, byReference);
  if (test === null) return [begin, statement];
  const isBool = kind.tag === "Scalar" && kind.scalar === "Bool";
  return printedIf(negated(test), [isBool ? 'printer.out.push_str("true");' : statement]);
}

function textFields(record: SchemaRecord): Field[] {
  return record.wireFields.filter(field => !followsFields(field));
}

function isZeroFunction(schema: Schema, record: SchemaRecord): string[] {
  if (!canBeZero(schema, { tag: "Record", record })) return [];
  const tests = textFields(record).flatMap(
    field => zeroTest(schema, field, `self.${rustIdentifier(field.name)}`, false) ?? [],
  );
  const conjuncts = (tests.length > 0 ? tests : ["true"]).map((test, index, all) =>
    index + 1 < all.length ? `${test} &&` : test,
  );
  return [
    `${indentation}pub(super) fn is_zero(&self) -> bool {`,
    ...pack(conjuncts, indentation.repeat(2)),
    `${indentation}}`,
  ];
}

function recordTextImpl(schema: Schema, record: SchemaRecord): string[] {
  const name = rustTypeName(record.name);
  const fields = textFields(record);
  const following = record.wireFields.filter(followsFields).map(field => ({
    member: rustIdentifier(field.name),
    suffix: snakeCase(kindText(field.kind)),
  }));
  return [
    `impl ${name} {`,
    ...indent([
      "pub fn print_fields(&self, printer: &mut Printer<'_>) {",
      `${indentation}self.print_fields_after(printer, true);`,
      ...following.map(({ member, suffix }) => `${indentation}printer.print_${suffix}(&self.${member});`),
      "}",
      "pub(super) fn print_nested(&self, printer: &mut Printer<'_>) {",
      ...indent(["printer.out.push('{');", "self.print_fields_after(printer, false);", "printer.out.push('}');"]),
      "}",
      "fn print_fields_after(&self, printer: &mut Printer<'_>, mut needs_blank: bool) {",
      ...indent(fields.flatMap(field => printField(schema, field, `self.${rustIdentifier(field.name)}`, false))),
      "}",
    ]),
    ...isZeroFunction(schema, record),
    "}",
  ];
}

function printCaseArm(schema: Schema, variant: Variant, variantCase: VariantCase): string[] {
  const pattern = casePattern(variant, variantCase);
  if (variantCase.fields.length === 0) return [`${pattern} => printer.out.push_str(${rustLiteral(variantCase.name)}),`];
  return [
    `${pattern} => {`,
    ...indent([
      `printer.out.push_str(${rustLiteral(`${variantCase.name}{`)});`,
      "let mut needs_blank = false;",
      ...variantCase.fields.flatMap(field => printField(schema, field, rustIdentifier(field.name), true)),
      "printer.out.push('}');",
    ]),
    "}",
  ];
}

function variantTextImpl(schema: Schema, variant: Variant): string[] {
  const name = rustTypeName(variant.name);
  return [
    `impl ${name} {`,
    ...indent([
      "pub(super) fn is_zero(&self) -> bool {",
      `${indentation}matches!(self, ${name}::${variant.cases[0].name})`,
      "}",
      "pub(super) fn print_variant(&self, printer: &mut Printer<'_>) {",
      ...indent([
        "match self {",
        ...indent(variant.cases.flatMap(variantCase => printCaseArm(schema, variant, variantCase))),
        "}",
      ]),
      "}",
    ]),
    "}",
  ];
}

const recordHelpers = `pub(super) fn begin_field(printer: &mut Printer<'_>, needs_blank: &mut bool, name: &str) {
    if *needs_blank {
        printer.out.push(' ');
    }
    *needs_blank = true;
    printer.out.push_str(name);
    printer.out.push('=');
}

pub(super) fn print_display(printer: &mut Printer<'_>, value: impl Display) {
    let _ = write!(printer.out, "{value}");
}

pub(super) fn print_flags(printer: &mut Printer<'_>, names: &[&str], bits: u32) {
    printer.out.push('[');
    let names_of_set_bits = names.iter().enumerate().filter(|&(bit, _)| bits >> bit & 1 != 0).map(|(_, &name)| name);
    printer.print_separated(names_of_set_bits, |printer, name| printer.out.push_str(name));
    printer.out.push(']');
}`.split("\n");

function recordTextRs(schema: Schema): string[] {
  const types = textTypes(schema);
  const impls = types.map(type =>
    type.tag === "Record" ? recordTextImpl(schema, type.record) : variantTextImpl(schema, type.variant),
  );
  return [
    rustMarker,
    "",
    "use core::fmt::{Display, Write};",
    "",
    "use super::entries::OpaqueKind;",
    "use super::enums::*;",
    "use super::program_enums::*;",
    "use super::records::*;",
    ...externalPathsOf(types).map(path => `use ${path};`),
    idsImport,
    "use crate::ir::print::Printer;",
    "",
    ...blocks(recordHelpers, ...impls),
  ];
}

export function emitRustText(context: EmitContext): OutputFile[] {
  return [{ name: "record_text.rs", lines: recordTextRs(context.schema) }];
}
