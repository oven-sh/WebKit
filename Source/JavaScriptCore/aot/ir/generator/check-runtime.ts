import { kindText } from "./hash.ts";
import { entriesOf } from "./model.ts";
import type { Entry, HeapReference, InlineBuiltin, RuntimeOperation, RuntimeParameter, Schema, SchemaEnum } from "./model.ts";
import type { ErrorList } from "./source.ts";

export const runtimeValueKinds: ReadonlyMap<string, string> = new Map([
  ["void", "Void"],
  ["size_t", "Size"],
  ["int32_t", "Int32"],
  ["uint32_t", "UInt32"],
  ["int64_t", "Int64"],
  ["uint64_t", "UInt64"],
  ["double", "Double"],
  ["EncodedJSValue", "EncodedJSValue"],
  ["JSCell*", "Cell"],
  ["UGPRPair", "Pair"],
  ["SiteCache", "SiteCache"],
]);

export const argumentBufferPointees: ReadonlyMap<string, boolean> = new Map([
  ["EncodedJSValue", true],
  ["uint32_t", true],
  ["uint64_t", true],
  ["JSCell", false],
  ["UniquedStringImpl", false],
  ["Latin1Character", false],
  ["char16_t", false],
  ["uint8_t", false],
  ["void", false],
]);

const pointerTypes: ReadonlySet<string> = new Set([
  "UniquedStringImpl*",
  "void*",
  "uint8_t*",
  "const uint8_t*",
  "const Latin1Character*",
  "const char16_t*",
  "EncodedJSValue*",
  "const EncodedJSValue*",
  "uint64_t*",
  "const uint64_t*",
  "uint32_t*",
  "const uint32_t*",
]);

const resultOnlyTypes: ReadonlySet<string> = new Set(["void", "UGPRPair"]);
const constPrefix = "const ";
const maxRuntimeParameters = 6;
const maxDoubleParameters = 4;
const maxInlineParameters = 4;
const maxInlineEnumMembers = 256;
const maxInlineFlagWordMembers = 8;

function isListedType(type: string): boolean {
  return runtimeValueKinds.has(type) || pointerTypes.has(type);
}

export function pointeeOf(type: string): string | null {
  if (!type.endsWith("*")) return null;
  const pointee = type.slice(0, -1);
  return pointee.startsWith(constPrefix) ? pointee.slice(constPrefix.length) : pointee;
}

function checkRuntimeParameter(
  operation: RuntimeOperation,
  parameter: RuntimeParameter,
  position: number,
  errors: ErrorList,
): void {
  const { type } = parameter;
  const description = `parameter ${parameter.name ?? position} of runtime ${operation.name}`;
  const pointee = pointeeOf(type);
  if (type === "void") {
    errors.add(operation.location, `${description} is void`);
  } else if (pointee !== null && !argumentBufferPointees.has(pointee)) {
    const problem = `points to ${pointee}, which is not listed as inside or outside the argument buffer`;
    errors.add(operation.location, `${description} ${problem}`);
  } else if (!isListedType(type) || resultOnlyTypes.has(type)) {
    errors.add(operation.location, `type ${type} of runtime ${operation.name} is not a listed C type`);
  }
}

function checkLeafEffects(operation: RuntimeOperation, errors: ErrorList): void {
  const { effects, location, name } = operation;
  if (effects.isUnknownCode || effects.throws || effects.collects) {
    errors.add(location, `leaf runtime operation ${name} throws, collects or calls unknown code`);
  }
  for (const { heap } of effects.writes) {
    if (!heap.marks.has("holdsValues")) continue;
    errors.add(location, `leaf runtime operation ${name} writes ${heap.name}, which holds values`);
  }
}

function checkRuntimeOperation(operation: RuntimeOperation, errors: ErrorList): void {
  const { location, name, parameters, resultType } = operation;
  if (!isListedType(resultType)) errors.add(location, `type ${resultType} of runtime ${name} is not a listed C type`);
  parameters.forEach((parameter, index) => checkRuntimeParameter(operation, parameter, index + 1, errors));
  if (parameters.length > maxRuntimeParameters) {
    errors.add(location, `runtime ${name} has more than ${maxRuntimeParameters} parameters`);
  }
  if (parameters.filter(parameter => parameter.type === "double").length > maxDoubleParameters) {
    errors.add(location, `runtime ${name} has more than ${maxDoubleParameters} double parameters`);
  }
  if (operation.isLeaf) checkLeafEffects(operation, errors);
}

function checkInlineBuiltin(builtin: InlineBuiltin, parameterTypes: Set<SchemaEnum>, errors: ErrorList): void {
  const { location, name, parameterKinds } = builtin;
  if (parameterKinds.length > maxInlineParameters) {
    errors.add(location, `inline ${name} has more than ${maxInlineParameters} parameters`);
  }
  for (const kind of parameterKinds) {
    const isByte = kind.tag === "Scalar" && kind.scalar === "UInt8";
    const isExternalEnum = kind.tag === "External" && kind.external.form === "enum";
    if (kind.tag === "Enum" || kind.tag === "Flags") {
      parameterTypes.add(kind.type);
    } else if (kind.tag === "External" && isExternalEnum && kind.external.below === null) {
      errors.add(location, `inline ${name}: external ${kind.external.name} has no below`);
    } else if (!isByte && !isExternalEnum) {
      errors.add(location, `unknown parameter kind ${kindText(kind)} of inline ${name}`);
    }
  }
}

function checkInlineParameterType(type: SchemaEnum, errors: ErrorList): void {
  const limit = type.isFlags ? maxInlineFlagWordMembers : maxInlineEnumMembers;
  if (type.members.length <= limit) return;
  const description = type.isFlags ? "flag word" : "enum";
  errors.add(type.location, `inline parameter ${description} ${type.name} has more than ${limit} members`);
}

const justifiedDifferences: ReadonlyMap<string, string> = new Map([
  ["PrivateSet writes MemberGuards", "DefinesProperty speaks for every definer; a private name is no member, and a guard ends with a member"],
  ["PrivateDefine writes MemberGuards", "as PrivateSet"],
  ["InitializeBuiltin writes ExternalState", "it orders the operation among others in the IR; generated code has no load of that heap"],
  ["PutSlotOrAdd leaves out ShapeWords", "a creation steps the key order of the Structure and may take the mark of birth off; the number of the shape is never rewritten and no mark is set, so a test that has passed still passes"],
  ["PutSlotOrAdd names an index of ObjectFields", "the function lists the field under the key that it is handed; the value is stored in line"],
  ["GetDeferredClosureSlot names an index of ObjectFields", "the function fills the slot that it is asked for"],
  ["GetDeferredClosureSlotOrUndefined names an index of ObjectFields", "as GetDeferredClosureSlot"],
  ["AppendHole names an index of Elements", "a row of the run time has no index; the receiver is a list"],
  ["AppendElements names an index of Elements", "as AppendHole"],
  ["AppendMembers names an index of Elements", "as AppendHole"],
  ["AppendValuesOfMap names an index of Elements", "as AppendHole"],
  ["SetArrayLength names an index of Elements", "as AppendHole"],
  ["TruncateNewArray names an index of Elements", "as AppendHole"],
  ["SortNumbers names an index of Elements", "as AppendHole"],
  ["SetMappedArgument names an index of Elements", "a row of the run time has no index; the receiver is an arguments object"],
  ["NewArray leaves out throws", "for want of memory alone: the cell is null and the next use faults"],
  ["CreateRest leaves out throws", "as NewArray"],
  ["NewRegExp leaves out throws", "as NewArray"],
  ["NewTemplateObject leaves out throws", "as NewArray"],
  ["CreateUnmappedArguments leaves out throws", "as NewArray"],
  ["CreateMappedArguments leaves out throws", "as NewArray"],
  ["EnumeratorGetByVal leaves out throws", "as NewArray: the key is a name of an enumerator that is current, and such an enumerator lists no accessor"],
  ["Strcat leaves out StringBuilderContents", "the buffer behind a growing rope is the run time's own; it is no builder that compiled code holds"],
]);

function heapsLeftOut(declared: readonly HeapReference[], byFunction: readonly HeapReference[]): string[] {
  return declared.filter(({ heap }) => !byFunction.some(other => other.heap === heap)).map(({ heap }) => heap.name);
}

function checkFunctionCoversOperation(entry: Entry, applied: Set<string>, errors: ErrorList): void {
  const column = entry.columns.get("runtime");
  if (column?.tag !== "RuntimeOperation") return;
  const { effects: byFunction, name } = column.operation;
  if (byFunction.isUnknownCode) return;
  const differences: string[] = [];
  if (entry.effects.isUnknownCode) differences.push("calls unknownCode");
  if (!byFunction.writesAllHeaps) {
    if (entry.effects.writesAllHeaps) differences.push("writes all");
    for (const heap of heapsLeftOut(entry.effects.writes, byFunction.writes)) differences.push(`writes ${heap}`);
  }
  if (!byFunction.readsAllHeaps) {
    if (entry.effects.readsAllHeaps) differences.push("reads all");
    for (const heap of heapsLeftOut(entry.effects.reads, byFunction.reads)) differences.push(`reads ${heap}`);
  }
  for (const difference of differences) {
    const line = `${entry.name} ${difference}`;
    if (justifiedDifferences.has(line)) applied.add(line);
    else errors.add(entry.location, `${line}, and runtime ${name} is declared not to: generated code is told only what both say`);
  }
}

const bookkeepingHeaps: ReadonlySet<string> = new Set(["AllocatorState", "RuntimeState"]);

function heapsNamedByIndexAlone(declared: readonly HeapReference[], byFunction: readonly HeapReference[]): string[] {
  const isWhole = (references: readonly HeapReference[], heap: HeapReference["heap"]) =>
    references.some(other => other.heap === heap && other.indexes.length === 0);
  const heaps = new Set(declared.map(({ heap }) => heap));
  return [...heaps].filter(heap => !isWhole(declared, heap) && isWhole(byFunction, heap)).map(heap => heap.name);
}

function checkOperationCoversFunction(entry: Entry, applied: Set<string>, errors: ErrorList): void {
  const column = entry.columns.get("runtime");
  if (column?.tag !== "RuntimeOperation") return;
  const { effects: byFunction, name } = column.operation;
  if (byFunction.isUnknownCode || byFunction.writesAllHeaps || entry.effects.isUnknownCode || entry.effects.writesAllHeaps) return;
  for (const heap of heapsLeftOut(byFunction.writes, entry.effects.writes)) {
    if (bookkeepingHeaps.has(heap)) continue;
    const line = `${entry.name} leaves out ${heap}`;
    if (justifiedDifferences.has(line)) applied.add(line);
    else errors.add(entry.location, `runtime ${name} writes ${heap}, and ${entry.name} does not say so: what is remembered of that heap would outlive the operation`);
  }
  for (const heap of heapsNamedByIndexAlone(entry.effects.writes, byFunction.writes)) {
    const line = `${entry.name} names an index of ${heap}`;
    if (justifiedDifferences.has(line)) applied.add(line);
    else errors.add(entry.location, `runtime ${name} writes all of ${heap}, and ${entry.name} names an index: what is remembered under another index would outlive the operation`);
  }
  for (const deed of ["throws", "collects"] as const) {
    if (!byFunction[deed] || entry.effects[deed]) continue;
    const line = `${entry.name} leaves out ${deed}`;
    if (justifiedDifferences.has(line)) applied.add(line);
    else errors.add(entry.location, `runtime ${name} ${deed}, and ${entry.name} does not say so`);
  }
}

function checkFunctionsCoverOperations(schema: Schema, errors: ErrorList): void {
  const applied = new Set<string>();
  const entries = entriesOf(schema);
  for (const entry of entries) checkFunctionCoversOperation(entry, applied, errors);
  for (const entry of entries) checkOperationCoversFunction(entry, applied, errors);
  for (const line of justifiedDifferences.keys()) {
    if (applied.has(line)) continue;
    const entry = entries.find(entry => line.startsWith(`${entry.name} `));
    const location = entry?.location ?? schema.runtimeOperations[0]!.location;
    errors.add(location, `"${line}" is listed in justifiedDifferences in check-runtime.ts, but the rows no longer differ that way`);
  }
}

export function checkRuntime(schema: Schema, errors: ErrorList): void {
  for (const operation of schema.runtimeOperations) checkRuntimeOperation(operation, errors);
  checkFunctionsCoverOperations(schema, errors);
  const parameterTypes = new Set<SchemaEnum>();
  for (const builtin of schema.inlineBuiltins) checkInlineBuiltin(builtin, parameterTypes, errors);
  for (const type of parameterTypes) checkInlineParameterType(type, errors);
}
