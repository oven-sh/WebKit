import {
  engineRecordNameOf,
  engineStructNameOf,
  engineTypeOf,
  engineTypesInDependencyOrder,
  isEmbeddedInEngineRecords,
  listFirstWord,
} from "./cpp-types.ts";
import {
  blocks,
  constexprSwitch,
  cppHeader,
  groupedCases,
  inNamespace,
  indent,
  indentation,
  type CaseBody,
  type Lines,
} from "./lines.ts";
import type { Entry, Field, IdKind, Kind, Member, Schema, SchemaRecord, SchemaType, Variant } from "./model.ts";
import { fieldsOf, functionLocalIdKinds, innermostKindOf, recordsOf, roleEnumNames } from "./model.ts";
import { cppIdTypeName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";

interface HolderKind {
  readonly concept: "HoldsOpcode" | "HoldsTerminatorKind";
  readonly enumName: "Opcode" | "TerminatorKind";
  readonly tag: "opcode" | "kind";
  readonly tagOf: "opcodeOf" | "terminatorKindOf";
  readonly referenceWalker: "walkValue" | "walkTerminator";
  readonly placedWordWalker: "walkPlacedWordsOfValue" | "walkPlacedWordsOfTerminator";
  readonly entriesIn: (schema: Schema) => readonly Entry[];
}

type PayloadConcept = "HoldsPayloadWords" | "HoldsMutablePayloadWords";

const holderKinds: readonly HolderKind[] = [
  {
    concept: "HoldsOpcode",
    enumName: "Opcode",
    tag: "opcode",
    tagOf: "opcodeOf",
    referenceWalker: "walkValue",
    placedWordWalker: "walkPlacedWordsOfValue",
    entriesIn: schema => schema.operations,
  },
  {
    concept: "HoldsTerminatorKind",
    enumName: "TerminatorKind",
    tag: "kind",
    tagOf: "terminatorKindOf",
    referenceWalker: "walkTerminator",
    placedWordWalker: "walkPlacedWordsOfTerminator",
    entriesIn: schema => schema.terminators,
  },
];

const payloadConcepts = `// Records and variants of the schema have members named kind, so HoldsTerminatorKind alone would take them.
// The cursors of AOTBody.h hold no payload words: they have forEachReference() of their own.
template<typename Holder>
concept HoldsPayloadWords = requires(const Holder& holder, unsigned word) {
    { holder.payloadWordAt(word) } -> std::same_as<uint32_t>;
    { holder.payloadListFrom(word) } -> std::same_as<std::span<const uint32_t>>;
};

template<typename Holder>
concept HoldsMutablePayloadWords = HoldsPayloadWords<Holder> && requires(Holder& holder, unsigned word, uint32_t value, std::span<const uint32_t> words) {
    holder.setPayloadWordAt(word, value);
    holder.setPayloadListFrom(word, words);
};`.split("\n");

function walkedTypes(schema: Schema): SchemaType[] {
  return engineTypesInDependencyOrder(schema);
}

function embeddedAbiTypes(schema: Schema): SchemaRecord[] {
  return recordsOf(schema).filter(
    record => record.form === "abi" && isEmbeddedInEngineRecords(schema, { tag: "Record", record }),
  );
}

function referenceKinds(schema: Schema): IdKind[] {
  return schema.idKinds.filter(kind => !functionLocalIdKinds.has(kind.name));
}

function referenceKindsWithoutRoles(schema: Schema): IdKind[] {
  return referenceKinds(schema).filter(kind => !roleEnumNames.has(kind.name));
}

function forEachReferenceDeclarations(typeName: string): string[] {
  return [
    `template<typename Visitor> void forEachReference(${typeName}&, Visitor&&);`,
    `template<typename Visitor> void forEachReference(const ${typeName}&, Visitor&&);`,
  ];
}

function holderHead(holder: HolderKind, payload: PayloadConcept): string {
  return `template<${holder.concept} Holder, typename Visitor> requires ${payload}<Holder>`;
}

function forEachReferenceDeclarationsOfHolder(holder: HolderKind): string[] {
  return [
    holderHead(holder, "HoldsMutablePayloadWords"),
    "void forEachReference(Holder&, Visitor&&);",
    holderHead(holder, "HoldsPayloadWords"),
    "void forEachReference(const Holder&, Visitor&&);",
  ];
}

function walkersHeader(schema: Schema): string[] {
  const declarations = blocks(
    payloadConcepts,
    holderKinds.flatMap(holder => forEachReferenceDeclarationsOfHolder(holder)),
    walkedTypes(schema).flatMap(type => forEachReferenceDeclarations(engineStructNameOf(type))),
    [
      ...referenceKindsWithoutRoles(schema).map(
        kind =>
          `template<typename Subject, typename Functor> void forEach${kind.name}(const Subject&, const Functor&);`,
      ),
      "template<typename Subject, typename Functor> void forEachFunctionReference(const Subject&, const Functor&);",
      "template<typename Subject, typename Functor> void forEachModuleReference(const Subject&, const Functor&);",
      "template<typename Subject, typename Visitor> void forEachReferenceWithRole(const Subject&, Visitor&&);",
    ],
    holderKinds.flatMap(holder => [
      holderHead(holder, "HoldsPayloadWords"),
      "void forEachPlacedWord(const Holder&, Visitor&&);",
    ]),
  );
  return cppHeader(
    [
      '"AOTIRFunction.h"',
      '"AOTLayout.h"',
      '"AOTOpcode.h"',
      '"AOTSchemaSupport.h"',
      "<concepts>",
      "<span>",
      "<wtf/Hasher.h>",
    ],
    [...inNamespace("Typed", declarations), "", '#include "AOTSchemaWalkersInlines.h"'],
  );
}

export function isReference(kind: Kind): boolean {
  return kind.tag === "Id" && !functionLocalIdKinds.has(kind.id.name);
}

function lastMemberOf(schema: Schema, enumName: string): Member {
  const type = schema.enums.find(candidate => candidate.name === enumName);
  if (type === undefined) throw new Error(`the schema has no enum ${enumName}`);
  return type.members[type.members.length - 1];
}

export function roleArgument(schema: Schema, kind: Kind): string {
  if (kind.tag !== "Id") return "";
  const enumName = roleEnumNames.get(kind.id.name);
  if (enumName === undefined) return "";
  return `, ${enumName}::${(kind.role ?? lastMemberOf(schema, enumName)).name}`;
}

export function holdsReferences(kind: Kind, seen: Set<SchemaRecord | Variant> = new Set()): boolean {
  const inner = innermostKindOf(kind);
  if (isReference(inner)) return true;
  if (inner.tag !== "Record" && inner.tag !== "Variant") return false;
  const type = inner.tag === "Record" ? inner.record : inner.variant;
  if (seen.has(type)) return false;
  seen.add(type);
  return fieldsOf(inner).some(field => holdsReferences(field.kind, seen));
}

function visitStatement(schema: Schema, kind: Kind, expression: string, isInAbi: boolean): string {
  if (!isReference(kind)) return `walkReferences(${expression}, visitor);`;
  if (isInAbi) return `visitReferenceWord<${engineTypeOf(kind)}>(${expression}, visitor${roleArgument(schema, kind)});`;
  return `visitReference(${expression}, visitor${roleArgument(schema, kind)});`;
}

function walkField(schema: Schema, field: Field, base: string, isInAbi: boolean): string[] {
  const kind = field.kind;
  const isCustomField = kind.tag === "Record" && kind.record.isCustomEngine;
  if (isCustomField || !holdsReferences(kind)) return [];
  const expression = `${base}.${field.name}`;
  const isArray = field.arrayLength !== null || field.isOnePerList;
  const element = kind.tag === "Span" || kind.tag === "List" ? kind.element : isArray ? kind : null;
  if (element === null) return [visitStatement(schema, kind, expression, isInAbi)];
  return [`for (auto& element : ${expression})`, indentation + visitStatement(schema, element, "element", isInAbi)];
}

function bothConstnesses(typeName: string, walkerName: string, isPublic = true): string[] {
  const internal = [
    `template<typename Visitor> void walkReferences(${typeName}& subject, Visitor& visitor) { ${walkerName}(subject, visitor); }`,
    `template<typename Visitor> void walkReferences(const ${typeName}& subject, Visitor& visitor) { ${walkerName}(subject, visitor); }`,
  ];
  if (!isPublic) return internal;
  return [
    ...internal,
    `template<typename Visitor> void forEachReference(${typeName}& subject, Visitor&& visitor) { walkWithoutRoles(subject, visitor); }`,
    `template<typename Visitor> void forEachReference(const ${typeName}& subject, Visitor&& visitor) { walkWithoutRoles(subject, visitor); }`,
  ];
}

function holderWalkers(holder: HolderKind): string[] {
  const mutableHead = holderHead(holder, "HoldsMutablePayloadWords");
  const constHead = holderHead(holder, "HoldsPayloadWords");
  return [
    mutableHead,
    `void walkReferences(Holder& subject, Visitor& visitor) { ${holder.referenceWalker}(subject, visitor); }`,
    constHead,
    `void walkReferences(const Holder& subject, Visitor& visitor) { ${holder.referenceWalker}(subject, visitor); }`,
    mutableHead,
    "void forEachReference(Holder& subject, Visitor&& visitor) { walkWithoutRoles(subject, visitor); }",
    constHead,
    "void forEachReference(const Holder& subject, Visitor&& visitor) { walkWithoutRoles(subject, visitor); }",
  ];
}

function walker(name: string, parameter: string, indentedBody: Lines, subjectConstraint = "typename"): string[] {
  const body =
    indentedBody.length > 0 ? indentedBody : indent([`UNUSED_PARAM(${parameter});`, "UNUSED_PARAM(visitor);"]);
  return [
    `template<${subjectConstraint} Subject, typename Visitor>`,
    `void ${name}(Subject& ${parameter}, Visitor& visitor)`,
    "{",
    ...body,
    "}",
  ];
}

function exhaustiveSwitch(subject: string, enumName: string, cases: readonly CaseBody[]): string[] {
  return [
    ...indent([`switch (${subject}) {`]),
    ...groupedCases(enumName, cases),
    ...indent(["}", "RELEASE_ASSERT_NOT_REACHED();"]),
  ];
}

function recordWalker(schema: Schema, record: SchemaRecord): string[] {
  const isAbi = record.form === "abi";
  const typeName = engineRecordNameOf(record);
  const body = record.fields.flatMap(field => walkField(schema, field, "record", isAbi));
  return [
    ...walker(`walk${typeName}`, "record", indent(body)),
    "",
    ...bothConstnesses(typeName, `walk${typeName}`, !isAbi),
  ];
}

function variantWalker(schema: Schema, variant: Variant): string[] {
  const cases = variant.cases.map(variantCase => {
    const statements = variantCase.fields.flatMap(field => walkField(schema, field, "value", false));
    if (statements.length === 0) return { label: variantCase.name, body: ["return;"] };
    const value = `auto& value = variant.template as<${variant.name}::${variantCase.name}>();`;
    return { label: variantCase.name, body: ["{", ...indent([value, ...statements, "return;"]), "}"] };
  });
  const hasReferences = cases.some(caseBody => caseBody.body.length > 1);
  const body = hasReferences ? exhaustiveSwitch("variant.kind()", `${variant.name}::Kind`, cases) : [];
  return [
    ...walker(`walk${variant.name}`, "variant", body),
    "",
    ...bothConstnesses(variant.name, `walk${variant.name}`),
  ];
}

const helpers = `template<typename Reference, typename Visitor, typename... Role>
void visitReference(Reference& reference, Visitor& visitor, Role... role)
{
    if (reference)
        visitor(reference, role...);
}

template<typename Reference, typename Word, typename Visitor, typename... Role>
void visitReferenceWord(Word& word, Visitor& visitor, Role... role)
{
    Reference reference = decodePayloadWord<Reference>(word);
    if (!reference)
        return;
    visitor(reference, role...);
    if constexpr (!std::is_const_v<Word>)
        word = encodePayloadWord(reference);
}

template<typename Reference, typename Holder, typename Visitor, typename... Role>
void visitPayloadReference(Holder& holder, unsigned word, Visitor& visitor, Role... role)
{
    Reference reference = decodePayloadWord<Reference>(holder.payloadWordAt(word));
    if (!reference)
        return;
    visitor(reference, role...);
    if constexpr (!std::is_const_v<Holder>)
        holder.setPayloadWordAt(word, encodePayloadWord(reference));
}

template<typename Reference, typename Holder, typename Visitor, typename... Role>
void visitPayloadReferenceList(Holder& holder, unsigned firstWord, Visitor& visitor, Role... role)
{
    if constexpr (std::is_const_v<Holder>) {
        for (const uint32_t& word : holder.payloadListFrom(firstWord))
            visitReferenceWord<Reference>(word, visitor, role...);
    } else {
        Vector<uint32_t, 16> words(holder.payloadListFrom(firstWord));
        if (words.isEmpty())
            return;
        for (uint32_t& word : words)
            visitReferenceWord<Reference>(word, visitor, role...);
        holder.setPayloadListFrom(firstWord, words.span());
    }
}

template<typename Record, typename Holder, typename Visitor>
void visitPayloadRecord(Holder& holder, unsigned firstWord, Visitor& visitor)
{
    Record record = decodePayloadRecord<Record>(holder, firstWord);
    walkReferences(record, visitor);
    if constexpr (!std::is_const_v<Holder>)
        encodePayloadRecord(holder, firstWord, record);
}

template<typename Record, typename Holder, typename Visitor>
void visitPayloadRecordList(Holder& holder, unsigned firstWord, Visitor& visitor)
{
    constexpr unsigned wordsPerElement = PayloadWords<Record>::count;
    if constexpr (std::is_const_v<Holder>) {
        std::span<const uint32_t> words = holder.payloadListFrom(firstWord);
        for (size_t offset = 0; offset < words.size(); offset += wordsPerElement) {
            Record record = PayloadWords<Record>::decode(words.subspan(offset).template first<wordsPerElement>());
            walkReferences(record, visitor);
        }
    } else {
        Vector<uint32_t, 16> words(holder.payloadListFrom(firstWord));
        if (words.isEmpty())
            return;
        for (size_t offset = 0; offset < words.size(); offset += wordsPerElement) {
            std::span<uint32_t, wordsPerElement> element = words.mutableSpan().subspan(offset).template first<wordsPerElement>();
            Record record = PayloadWords<Record>::decode(element);
            walkReferences(record, visitor);
            PayloadWords<Record>::encode(record, element);
        }
        holder.setPayloadListFrom(firstWord, words.span());
    }
}

template<typename Word, typename Holder, typename Visitor>
void visitPlacedWord(const Holder& holder, unsigned word, Visitor& visitor)
{
    if (auto placed = decodePayloadWord<std::optional<Word>>(holder.payloadWordAt(word)))
        visitor(*placed);
}

template<typename Wanted, typename Functor>
struct ReferenceFilter {
    template<typename Reference, typename... Role>
    void operator()(const Reference& reference, Role... role) const
    {
        if constexpr (std::is_same_v<Reference, Wanted>)
            functor(reference, role...);
    }

    const Functor& functor;
};

template<typename Visitor>
struct VisitorWithoutRoles {
    template<typename Reference, typename... Role>
    void operator()(Reference& reference, Role...) const { visitor(reference); }

    Visitor& visitor;
};

template<typename Subject, typename Visitor>
void walkWithoutRoles(Subject& subject, Visitor& visitor)
{
    VisitorWithoutRoles<Visitor> withoutRoles { visitor };
    walkReferences(subject, withoutRoles);
}

template<typename Wanted, typename Subject, typename Functor>
void walkFiltered(const Subject& subject, const Functor& functor)
{
    ReferenceFilter<Wanted, Functor> filter { functor };
    walkReferences(subject, filter);
}`.split("\n");

function walkEntry(schema: Schema, entry: Entry): string[] {
  const statements = entry.payload.flatMap(field => {
    if (!holdsReferences(field.kind)) return [];
    const isList = field.kind.tag === "List";
    const kind = field.kind.tag === "List" ? field.kind.element : field.kind;
    const helper = `visitPayload${isReference(kind) ? "Reference" : "Record"}${isList ? "List" : ""}`;
    return [
      `${helper}<${engineTypeOf(kind)}>(subject, ${isList ? listFirstWord(entry) : field.firstWord}, visitor${roleArgument(schema, kind)});`,
    ];
  });
  return [...statements, "return;"];
}

function placedWordsOfEntry(entry: Entry): string[] {
  const statements = entry.payload.flatMap(field =>
    field.kind.tag === "PlacedWord"
      ? [`visitPlacedWord<${field.kind.word}>(subject, ${field.firstWord}, visitor);`]
      : [],
  );
  return [...statements, "return;"];
}

function entryWalker(name: string, holder: HolderKind, schema: Schema, bodyOf: (entry: Entry) => Lines): string[] {
  const { enumName, tag } = holder;
  const groups = [...Map.groupBy(holder.entriesIn(schema), entry => bodyOf(entry).join("\n")).values()];
  if (groups.length > 256) throw new Error(`${name} has more cases than a byte can number`);
  const bodies = groups.map(group => bodyOf(group[0]));
  const caseOf = `caseOf${name[0].toUpperCase()}${name.slice(1)}`;
  const cases = groups.flatMap((group, index) => group.map(entry => ({ label: entry.name, value: String(index) })));
  const signature = `uint8_t ${caseOf}(${enumName} ${tag})`;
  return [
    ...constexprSwitch({ signature, subject: tag, enumName, cases, defaultValue: null }),
    "",
    ...walker(
      name,
      "subject",
      indent([
        ...(bodies.every(body => body.length === 1) ? ["UNUSED_PARAM(visitor);"] : []),
        `switch (CaseTable<${enumName}, ${caseOf}>::at(${holder.tagOf}(subject))) {`,
        ...bodies.flatMap((body, index) => [`case ${index}:`, ...indent(body)]),
        "default:",
        ...indent(["WTF_UNREACHABLE();"]),
        "}",
      ]),
      holder.concept,
    ),
  ];
}

function filteredWalk(name: string, idKind: string): string[] {
  return [
    "template<typename Subject, typename Functor>",
    `void ${name}(const Subject& subject, const Functor& functor) { walkFiltered<${cppIdTypeName(idKind)}>(subject, functor); }`,
  ];
}

function walkersInlines(schema: Schema): string[] {
  const abiRecords = embeddedAbiTypes(schema);
  const referencesOf = (entry: Entry) => walkEntry(schema, entry);
  const body = blocks(
    abiRecords.flatMap(record => [
      `template<typename Visitor> void walkReferences(${record.name}&, Visitor&);`,
      `template<typename Visitor> void walkReferences(const ${record.name}&, Visitor&);`,
    ]),
    helpers,
    ...abiRecords.map(record => recordWalker(schema, record)),
    ...walkedTypes(schema).map(type =>
      type.tag === "Record" ? recordWalker(schema, type.record) : variantWalker(schema, type.variant),
    ),
    ...holderKinds.flatMap(holder => [
      entryWalker(holder.referenceWalker, holder, schema, referencesOf),
      holderWalkers(holder),
    ]),
    referenceKindsWithoutRoles(schema).flatMap(kind => filteredWalk(`forEach${kind.name}`, kind.name)),
    [...filteredWalk("forEachFunctionReference", "Function"), ...filteredWalk("forEachModuleReference", "Module")],
    [
      "template<typename Subject, typename Visitor>",
      "void forEachReferenceWithRole(const Subject& subject, Visitor&& visitor) { walkReferences(subject, visitor); }",
    ],
    ...holderKinds.map(holder => entryWalker(holder.placedWordWalker, holder, schema, placedWordsOfEntry)),
    holderKinds.flatMap(holder => [
      holderHead(holder, "HoldsPayloadWords"),
      `void forEachPlacedWord(const Holder& subject, Visitor&& visitor) { ${holder.placedWordWalker}(subject, visitor); }`,
    ]),
  );
  return cppHeader(['"AOTVector.h"', "<type_traits>"], inNamespace("Typed", body));
}

export function emitCppWalkers(context: EmitContext): OutputFile[] {
  return [
    { name: "AOTSchemaWalkers.h", lines: walkersHeader(context.schema) },
    { name: "AOTSchemaWalkersInlines.h", lines: walkersInlines(context.schema) },
  ];
}
