import { cppMarker, indentation, pack } from "./lines.ts";
import type { Entry, Schema } from "./model.ts";
import { entriesOf, isLegalIn } from "./model.ts";
import { cppEmitterClassName, cppGenerateFunctionName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";

const entriesWithoutEmitter: ReadonlySet<string> = new Set(
  `Convert Check Expect Upcast NarrowByField NarrowByTest
   AppendSpread IteratorOpen IteratorResume IteratorClose StepArrayIterator StepMapIterator StepSetIterator
   CreateIteratorResult Await Yield InitialYield ReadScope EvaluateModule StringBuilderAppend`.split(/\s+/),
);

function requireNamesInSchema(
  listName: string,
  names: ReadonlySet<string>,
  declared: readonly { readonly name: string }[],
): void {
  const declaredNames = new Set(declared.map(({ name }) => name));
  const unknown = [...names].filter(name => !declaredNames.has(name));
  if (unknown.length > 0) throw new Error(`${listName} has names that are not in the schema: ${unknown.join(", ")}`);
}

function hasEmitter(entry: Entry): boolean {
  return isLegalIn(entry, "Placed") && !entriesWithoutEmitter.has(entry.name);
}

function dispatchCase(enumName: string, name: string, family: string, argument: string): string {
  const call = `emitter<${cppEmitterClassName(family)}>().${cppGenerateFunctionName(name)}(${argument});`;
  return `${indentation}case ${enumName}::${name}: ${call} break;`;
}

function unreachableCases(enumName: string, names: readonly string[]): string[] {
  if (names.length === 0) return [];
  const labels = names.map(name => `case ${enumName}::${name}:`);
  return [...pack(labels, indentation), `${indentation}${indentation}RELEASE_ASSERT_NOT_REACHED();`];
}

function dispatchInc(entries: readonly Entry[], enumName: string, argument: string): string[] {
  const generated = entries.filter(entry => hasEmitter(entry));
  const unreachable = entries.filter(entry => !hasEmitter(entry));
  return [
    cppMarker,
    "",
    ...generated.map(entry => dispatchCase(enumName, entry.name, entry.family, argument)),
    ...unreachableCases(
      enumName,
      unreachable.map(entry => entry.name),
    ),
  ];
}

function inlineDispatchInc(schema: Schema): string[] {
  return [
    cppMarker,
    "",
    ...schema.inlineBuiltins.map(builtin => dispatchCase("InlineBuiltin", builtin.name, builtin.family, "call")),
  ];
}

export function emitCppDispatch(context: EmitContext): OutputFile[] {
  const { schema } = context;
  requireNamesInSchema("entriesWithoutEmitter", entriesWithoutEmitter, entriesOf(schema));
  return [
    { name: "AOTSchemaOperationDispatch.inc", lines: dispatchInc(schema.operations, "Opcode", "value") },
    { name: "AOTSchemaTerminatorDispatch.inc", lines: dispatchInc(schema.terminators, "TerminatorKind", "terminator") },
    { name: "AOTSchemaInlineBuiltinDispatch.inc", lines: inlineDispatchInc(schema) },
  ];
}
