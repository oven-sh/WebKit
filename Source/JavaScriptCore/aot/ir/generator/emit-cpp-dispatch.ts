import { cppMarker, indentation, pack } from "./lines.ts";
import type { Entry, InlineBuiltin, Schema } from "./model.ts";
import { entriesOf, isLegalIn } from "./model.ts";
import { cppEmitterClassName, cppGenerateFunctionName } from "./names.ts";
import type { EmitContext, OutputFile } from "./outputs.ts";

// No class of codegen/AOTEmitterList.h has generate<Name>() for these.
// The list must equal opcodesWithoutEmitter() of walk/AOTCode.cpp, which keeps them out of Code.
const entriesWithoutEmitter: ReadonlySet<string> = new Set(
  `Convert Check Expect AssumeType Upcast NarrowByField NarrowByTest
   AppendSpread IteratorOpen IteratorResume IteratorClose StepArrayIterator StepMapIterator StepSetIterator
   CreateIteratorResult Await Yield InitialYield ReadScope EvaluateModule StringBuilderAppend
   AllGuards Entered Specified
   RecordCallee KeepCalleeInFrame KeepEnvironmentOfCalleeInFrame AreFunctionNamesUnchanged
   IsPrototypeOfClass
   ReadAboveFreshObject DidReadAbove
   ReportMismatch
   TryPutDoubleIndex HasSpareArrayCapacity
   ToEnumeratorPosition
   IsBuiltinLocationUnchanged NewHeapFrame GetResumeIndex SuspendInHeapFrame SubstringOrUndefined`.split(/\s+/),
);

// The same for the rows of inline-builtins.def.
const inlineBuiltinsWithoutEmitter: ReadonlySet<string> = new Set(
  `ArraySplicePrefix ArrayWithLength ArrayOf
   OwnPropertyDescriptorFields
   StringSplitPrefix RegExpExecPrefix StringMatchPrefix`.split(/\s+/),
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

function inlineBuiltinHasEmitter(builtin: InlineBuiltin): boolean {
  return !builtin.isFormOnly && !inlineBuiltinsWithoutEmitter.has(builtin.name);
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
  const generated = schema.inlineBuiltins.filter(builtin => inlineBuiltinHasEmitter(builtin));
  const unreachable = schema.inlineBuiltins.filter(builtin => !inlineBuiltinHasEmitter(builtin));
  return [
    cppMarker,
    "",
    ...generated.map(builtin => dispatchCase("InlineBuiltin", builtin.name, builtin.family, "call")),
    ...unreachableCases(
      "InlineBuiltin",
      unreachable.map(builtin => builtin.name),
    ),
  ];
}

export function emitCppDispatch(context: EmitContext): OutputFile[] {
  const { schema } = context;
  requireNamesInSchema("entriesWithoutEmitter", entriesWithoutEmitter, entriesOf(schema));
  requireNamesInSchema("inlineBuiltinsWithoutEmitter", inlineBuiltinsWithoutEmitter, schema.inlineBuiltins);
  return [
    { name: "AOTSchemaOperationDispatch.inc", lines: dispatchInc(schema.operations, "Opcode", "value") },
    { name: "AOTSchemaTerminatorDispatch.inc", lines: dispatchInc(schema.terminators, "TerminatorKind", "terminator") },
    { name: "AOTSchemaInlineBuiltinDispatch.inc", lines: inlineDispatchInc(schema) },
  ];
}
