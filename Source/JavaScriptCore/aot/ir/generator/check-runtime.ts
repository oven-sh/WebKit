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

// Generated code is told what the operation AND the function that it calls are both declared to do: RuntimeCallEmitter::effectsOfCall() intersects the two. So a heap that
// the row of the function leaves out is lost, however right the row of the operation is, and a load of that heap may be carried across the call.
// Whether a function collects or enters the VM is not asked here: AOT_OPERATION_BEGIN and OperationValidationScope hold the body to its row.
//
// WHAT THIS DOES NOT SEE. It holds table against table, for the operations that name a function by `runtime =`: 250 of them when it was written.
// - 207 of the 344 functions with a narrow row are named by no operation: stubs and emitters call them. What a stub is taken to do is what all that it calls are declared to do
//   (StubCatalog::Node), so one function there that is declared to call unknown code hides what the rows of the others leave out.
// - Nobody reads the C++. A row that says less than its body is found here only if the operation says more than the row: where both say as little, both are wrong and this is silent.
//
// What an operation is declared to do and its function is not, and why the row of the function is right. A line that no longer applies is an error too.
const leftOutByRight: ReadonlyMap<string, string> = new Map([
  ["NewObject writes MemberGuards", "the first allocation of a class may begin a guard; only what ends one has to say so (GuardEmitter::holds())"],
  ["PrivateSet writes MemberGuards", "DefinesProperty speaks for every definer; a private name is no member, and a guard ends with a member"],
  ["PrivateDefine writes MemberGuards", "as PrivateSet"],
  ["InitializeBuiltin writes ExternalState", "it orders the operation among others in the IR; generated code has no load of that heap"],
  ["StringBuilderAppendNumber writes StringStorage", "AppendsToBuilder speaks for every part; a number resolves no rope"],
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
    if (leftOutByRight.has(line)) applied.add(line);
    else errors.add(entry.location, `${line}, and runtime ${name} is declared not to: generated code is told only what both say`);
  }
}

function checkFunctionsCoverOperations(schema: Schema, errors: ErrorList): void {
  const applied = new Set<string>();
  const entries = entriesOf(schema);
  for (const entry of entries) checkFunctionCoversOperation(entry, applied, errors);
  for (const line of leftOutByRight.keys()) {
    if (applied.has(line)) continue;
    const entry = entries.find(entry => line.startsWith(`${entry.name} `));
    const location = entry?.location ?? schema.runtimeOperations[0]!.location;
    errors.add(location, `"${line}" is listed as left out by right in check-runtime.ts, and the rows no longer differ so`);
  }
}

export function checkRuntime(schema: Schema, errors: ErrorList): void {
  for (const operation of schema.runtimeOperations) checkRuntimeOperation(operation, errors);
  checkFunctionsCoverOperations(schema, errors);
  const parameterTypes = new Set<SchemaEnum>();
  for (const builtin of schema.inlineBuiltins) checkInlineBuiltin(builtin, parameterTypes, errors);
  for (const type of parameterTypes) checkInlineParameterType(type, errors);
}
