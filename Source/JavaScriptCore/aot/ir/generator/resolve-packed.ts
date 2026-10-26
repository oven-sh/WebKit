import type { EnumColumn, PackedCondition, PackedField, PackedKind, PackedRecord, PackedWord } from "./model.ts";
import { cppIdTypeName } from "./names.ts";
import type { DeclarationInFile } from "./parse.ts";
import type { Scope } from "./resolve.ts";
import type { KindSyntax, Name, PackedConditionSyntax, PackedSyntax, PackedWordSyntax } from "./syntax.ts";

const numberTypes: Readonly<Record<string, "uint32_t" | "uint64_t">> = { UInt32: "uint32_t", UInt64: "uint64_t" };
const bitsInWord = 64;

function bitsToCount(numberOfValues: number): number {
  return Math.max(1, Math.ceil(Math.log2(numberOfValues)));
}

function findPackedKind(scope: Scope, syntax: KindSyntax): PackedKind | null {
  const word = syntax.name.text;
  if (word === "List" && syntax.element !== null) {
    const element = scope.packedRecords.get(syntax.element.name.text);
    return element === undefined ? null : { tag: "List", element };
  }
  if (syntax.element !== null || syntax.role !== null || syntax.arrayLength !== null) return null;
  if (word === "Bool" || word === "CodeOffset" || word === "Bytes") return { tag: word };
  if (word in numberTypes) return { tag: "Number", cppType: numberTypes[word] };
  const type = scope.enums.get(word);
  if (type !== undefined) return type.isFlags ? null : { tag: "Enum", type };
  const external = scope.externals.get(word);
  if (external !== undefined) return external.form === "id" ? { tag: "Id", cppType: external.cppType } : null;
  if (scope.idKinds.has(word)) return { tag: "Id", cppType: `Typed::${cppIdTypeName(word)}` };
  const element = scope.packedRecords.get(word);
  return element === undefined ? null : { tag: "Record", element };
}

/** How many bits of its word the field takes. Null: all that the fields in front of it leave. */
function widthOf(kind: PackedKind): number | null {
  if (kind.tag === "Bool") return 1;
  return kind.tag === "Enum" ? bitsToCount(kind.type.members.length) : null;
}

function fillsWordsOfItsOwn(kind: PackedKind): boolean {
  return kind.tag === "Record" || kind.tag === "List" || kind.tag === "Bytes";
}

function findBoolColumn(scope: Scope, field: PackedField, column: Name): EnumColumn | null {
  const found =
    field.kind.tag === "Enum" ? field.kind.type.columns.find(({ name }) => name === column.text) : undefined;
  if (found !== undefined && found.kind.tag === "Bool") return found;
  scope.errors.add(column.location, `${column.text} is not a Bool column of ${field.name}`);
  return null;
}

function resolveCondition(
  scope: Scope,
  syntax: PackedConditionSyntax,
  earlier: readonly PackedField[],
): PackedCondition | null {
  const field = earlier.find(({ name }) => name === syntax.field.text);
  if (field === undefined) {
    scope.errors.add(syntax.field.location, `${syntax.field.text} is not a field of an earlier word`);
    return null;
  }
  if (syntax.column !== null) {
    const column = findBoolColumn(scope, field, syntax.column);
    return column === null ? null : { field, column };
  }
  if (field.kind.tag === "Bool") return { field, column: null };
  scope.errors.add(syntax.field.location, `${field.name} is not a Bool`);
  return null;
}

function resolveWord(scope: Scope, syntax: PackedWordSyntax, earlier: readonly PackedField[]): PackedWord {
  const fields: PackedField[] = [];
  let shift: number | null = 0;
  for (const { name, kind: kindSyntax } of syntax.fields) {
    const kind = findPackedKind(scope, kindSyntax);
    if (kind === null) {
      scope.errors.add(kindSyntax.name.location, `${kindSyntax.name.text} cannot be packed`);
      continue;
    }
    if ([...earlier, ...fields].some(other => other.name === name.text))
      scope.errors.add(name.location, `${name.text} is declared twice`);
    if (shift === null || (fillsWordsOfItsOwn(kind) && syntax.fields.length > 1)) {
      scope.errors.add(name.location, `${name.text} needs a word of its own: write ';' in front of it`);
      continue;
    }
    const width = widthOf(kind);
    fields.push({ name: name.text, kind, shift, width, location: name.location });
    shift = width === null ? null : shift + width;
    if (shift !== null && shift > bitsInWord) scope.errors.add(name.location, `${name.text} does not fit its word`);
  }
  const condition = syntax.condition === null ? null : resolveCondition(scope, syntax.condition, earlier);
  return { fields, condition };
}

function resolvePackedRecord(scope: Scope, syntax: PackedSyntax): PackedRecord {
  const words: PackedWord[] = [];
  for (const word of syntax.words) {
    words.push(
      resolveWord(
        scope,
        word,
        words.flatMap(({ fields }) => fields),
      ),
    );
  }
  return { name: syntax.name.text, words, location: syntax.name.location };
}

function columnsAsText(type: Extract<PackedKind, { tag: "Enum" }>["type"], position: number): string {
  return type.members[position].values.map(value => (value.tag === "Bool" ? String(value.value) : "")).join(", ");
}

/** An enumerator is found by its columns, so no two may have the same. */
function checkEnumeratorsDiffer(scope: Scope, record: PackedRecord): void {
  for (const { kind } of record.words.flatMap(({ fields }) => fields)) {
    if (kind.tag !== "Enum" || !isFoundByColumns(kind.type)) continue;
    const seen = new Map<string, string>();
    kind.type.members.forEach((member, position) => {
      const columns = columnsAsText(kind.type, position);
      const other = seen.get(columns);
      if (other !== undefined) scope.errors.add(member.location, `${member.name} has the columns of ${other}`);
      seen.set(columns, member.name);
    });
  }
}

export function isFoundByColumns(type: Extract<PackedKind, { tag: "Enum" }>["type"]): boolean {
  return type.columns.length > 0 && type.columns.every(column => column.kind.tag === "Bool");
}

export function resolvePackedRecords(scope: Scope, declarations: readonly DeclarationInFile[]): void {
  for (const { declaration } of declarations) {
    if (declaration.declares !== "packed") continue;
    const record = resolvePackedRecord(scope, declaration);
    checkEnumeratorsDiffer(scope, record);
    scope.declare(scope.packedRecords, declaration.name, record, [scope.enums, scope.records, scope.variants]);
  }
}
