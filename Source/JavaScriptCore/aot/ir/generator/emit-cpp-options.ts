import { namespaceOf } from "./cpp-types.ts";
import { cppHeader, cppLiteral, cppMarker, inNamespace, indent } from "./lines.ts";
import type { Field, OptionMember, OptionWord, Schema, SchemaEnum, SchemaRecord } from "./model.ts";
import { optionsRecordName, recordsOf } from "./model.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";

const applyHead =
  "[](Wire::CompileOptions& options, const OptionWord& word, StringView value) -> std::optional<String> {";
const applyHeadWithoutValue =
  "[](Wire::CompileOptions& options, const OptionWord&, StringView) -> std::optional<String> {";
const applyHeadWithoutWord =
  "[](Wire::CompileOptions& options, const OptionWord&, StringView value) -> std::optional<String> {";
const listHead = "[](PrintStream& out) {";

function lvalueOf(target: OptionMember): string {
  return ["options", ...target.map(field => field.name)].join(".");
}

function typeName(type: SchemaEnum): string {
  return `${namespaceOf(type)}::${type.name}`;
}

function literalOrNone(text: string | null): string {
  return text === null ? "{ }" : cppLiteral(text);
}

function listMembers(type: SchemaEnum): string[] {
  return [
    listHead,
    `    for (auto member : allMembersOf<${typeName(type)}>)`,
    "        listValue(out, nameOf(member));",
    "}",
  ];
}

/** The function that applies the word, and the function that lists what its value can be. */
function functionsOf(word: OptionWord): readonly [string[], string[]] {
  const { action } = word;
  switch (action.tag) {
    case "FrontEnd":
      return [["nullptr"], ["nullptr"]];
    case "Custom":
      return [[action.apply], [action.list ?? "nullptr"]];
    case "Sets": {
      const value = `${typeName(action.type)}::${action.member.name}`;
      const statement = action.type.isFlags
        ? `addFlag(${lvalueOf(action.target)}, ${value});`
        : `${lvalueOf(action.target)} = ${value};`;
      return [[applyHeadWithoutValue, `    ${statement}`, "    return std::nullopt;", "}"], ["nullptr"]];
    }
    case "Number":
      return [
        [
          applyHeadWithoutWord,
          "    auto number = parseNumber<uint32_t>(value);",
          "    if (!number)",
          "        return notANumberSentence(value);",
          ...(action.sentenceForZero === null
            ? []
            : ["    if (!*number)", `        return ${cppLiteral(action.sentenceForZero)};`]),
          `    ${lvalueOf(action.target)} = *number;`,
          "    return std::nullopt;",
          "}",
        ],
        ["nullptr"],
      ];
    case "NamesAlternative":
      return [
        [
          applyHead,
          ...action.alternatives.flatMap(({ text, member }) => [
            `    if (matchesName(value, ${cppLiteral(text)})) {`,
            `        ${lvalueOf(action.target)} = ${typeName(action.type)}::${member.name};`,
            "        return std::nullopt;",
            "    }",
          ]),
          "    return unknownNameSentence(word.noun, value);",
          "}",
        ],
        [listHead, ...action.alternatives.map(({ text }) => `    listValue(out, ${cppLiteral(text)});`), "}"],
      ];
    case "Names": {
      const find = (name: string) => [
        `auto member = findMember<${typeName(action.type)}>(${name});`,
        "if (!member)",
        `    return unknownNameSentence(word.noun, ${name});`,
      ];
      if (!action.type.isFlags) {
        const body = [...find("value"), `${lvalueOf(action.target)} = *member;`, "return std::nullopt;"];
        return [[applyHead, ...indent(body), "}"], listMembers(action.type)];
      }
      const body = [...find("name"), `addFlag(${lvalueOf(action.target)}, *member);`, "return std::nullopt;"];
      return [
        [
          applyHead,
          "    return forEachListedName(word.key, value, [&](StringView name) -> std::optional<String> {",
          ...indent(body, 2),
          "    });",
          "}",
        ],
        listMembers(action.type),
      ];
    }
  }
}

function rowOf(word: OptionWord): string[] {
  const [apply, list] = functionsOf(word);
  const texts = [cppLiteral(word.key), literalOrNone(word.valueSyntax), literalOrNone(word.noun)].join(", ");
  if (apply.length === 1 && list.length === 1) return [`{ ${texts}, ${apply[0]}, ${list[0]} },`];
  const withComma = (lines: readonly string[]) => [...lines.slice(0, -1), `${lines[lines.length - 1]},`];
  return ["{", ...indent([`${texts},`, ...withComma(apply), ...withComma(list)]), "},"];
}

function wordsInc(schema: Schema): string[] {
  return [
    cppMarker,
    "",
    "static constexpr OptionWord optionWords[] = {",
    ...indent(schema.optionWords.flatMap(rowOf)),
    "};",
  ];
}

function enumeratedMembersOf(record: SchemaRecord, prefix: string): string[] {
  return record.fields.flatMap((field: Field) => {
    const lvalue = `${prefix}.${field.name}`;
    if (field.kind.tag === "Record") return enumeratedMembersOf(field.kind.record, lvalue);
    return field.kind.tag === "Enum" || field.kind.tag === "Flags" ? [lvalue] : [];
  });
}

function membersHeader(schema: Schema): string[] {
  const record = recordsOf(schema).find(candidate => candidate.name === optionsRecordName);
  if (record === undefined) throw new Error(`the schema has no record ${optionsRecordName}`);
  return cppHeader(
    ['"AOTWireRecords.h"'],
    inNamespace("Wire", [
      "// Every member that is an enumeration or a flag word, those of the records inside too.",
      "template<typename Functor>",
      `void forEachEnumeratedMember(${optionsRecordName}& options, const Functor& functor)`,
      "{",
      ...enumeratedMembersOf(record, "options").map(lvalue => `    functor(${lvalue});`),
      "}",
    ]),
  );
}

export function emitCppOptions(context: EmitContext): OutputFile[] {
  return [
    { name: "AOTSchemaOptionWords.inc", lines: wordsInc(context.schema) },
    { name: "AOTWireOptionMembers.h", lines: membersHeader(context.schema) },
  ];
}
