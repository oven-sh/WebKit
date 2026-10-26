import type { Field, Member, OptionMember, OptionWordAction, SchemaEnum, SchemaRecord } from "./model.ts";
import { optionsRecordName } from "./model.ts";
import type { DeclarationInFile } from "./parse.ts";
import type { Scope } from "./resolve.ts";
import type { Name, OptionWordSyntax } from "./syntax.ts";

function findOptionMember(scope: Scope, written: Name): OptionMember | null {
  const path: Field[] = [];
  let record: SchemaRecord | undefined = scope.records.get(optionsRecordName);
  for (const name of written.text.split("::")) {
    const field = record?.fields.find(candidate => candidate.name === name);
    if (field === undefined) {
      scope.errors.add(written.location, `${written.text} is not a member of ${optionsRecordName}`);
      return null;
    }
    path.push(field);
    record = field.kind.tag === "Record" ? field.kind.record : undefined;
  }
  return path;
}

function enumeratedTypeOf(scope: Scope, target: OptionMember, written: Name): SchemaEnum | null {
  const { kind } = target[target.length - 1];
  if (kind.tag === "Enum" || kind.tag === "Flags") return kind.type;
  scope.errors.add(written.location, `${written.text} is neither an enum nor a flag word`);
  return null;
}

function findMember(scope: Scope, type: SchemaEnum, name: Name): Member | null {
  const member = type.members.find(candidate => candidate.name === name.text);
  if (member === undefined) scope.errors.add(name.location, `${name.text} is not a member of ${type.name}`);
  return member ?? null;
}

function resolveNames(
  scope: Scope,
  syntax: OptionWordSyntax,
  target: OptionMember,
  type: SchemaEnum,
  names: readonly Name[],
): OptionWordAction | null {
  if (names.length === 0) return { tag: "Names", target, type };
  const texts = (syntax.valueSyntax ?? "").split("|");
  if (type.isFlags || texts.length !== names.length) {
    scope.errors.add(syntax.location, `${syntax.key}: each alternative of the value stands for one member of an enum`);
    return null;
  }
  const members = names.map(name => findMember(scope, type, name));
  const alternatives = texts.flatMap((text, index) => {
    const member = members[index];
    return member === null ? [] : [{ text, member }];
  });
  return { tag: "NamesAlternative", target, type, alternatives };
}

function resolveAction(scope: Scope, syntax: OptionWordSyntax): OptionWordAction | null {
  const { action } = syntax;
  if (action.form === "frontEnd") return { tag: "FrontEnd" };
  if (action.form === "custom") return { tag: "Custom", apply: action.apply.text, list: action.list?.text ?? null };
  const target = findOptionMember(scope, action.path);
  if (target === null) return null;
  if (action.form === "number") {
    const { kind, arrayLength } = target[target.length - 1];
    if (kind.tag === "Scalar" && kind.scalar === "UInt32" && arrayLength === null)
      return { tag: "Number", target, sentenceForZero: action.sentenceForZero };
    scope.errors.add(action.path.location, `${action.path.text} is not a UInt32`);
    return null;
  }
  const type = enumeratedTypeOf(scope, target, action.path);
  if (type === null) return null;
  if (action.form === "names") return resolveNames(scope, syntax, target, type, action.members);
  const member = findMember(scope, type, action.member);
  return member === null ? null : { tag: "Sets", target, type, member };
}

function takesValue(action: OptionWordAction): boolean | null {
  switch (action.tag) {
    case "Sets":
      return false;
    case "Names":
    case "NamesAlternative":
    case "Number":
      return true;
    case "Custom":
    case "FrontEnd":
      return null;
  }
}

export function resolveOptionWords(scope: Scope, declarations: readonly DeclarationInFile[]): void {
  for (const { declaration } of declarations) {
    if (declaration.declares !== "word") continue;
    const { key, valueSyntax, noun, location } = declaration;
    const action = resolveAction(scope, declaration);
    if (action === null) continue;
    const expected = takesValue(action);
    if (expected !== null && expected !== (valueSyntax !== null))
      scope.errors.add(location, `${key} ${expected ? "needs" : "cannot have"} a syntax for its value`);
    if ((action.tag === "Names" || action.tag === "NamesAlternative") && noun === null)
      scope.errors.add(location, `${key} needs a noun for "unknown <noun> x"`);
    scope.declare(scope.optionWords, { text: key, location }, { key, valueSyntax, noun, action, location });
  }
}
