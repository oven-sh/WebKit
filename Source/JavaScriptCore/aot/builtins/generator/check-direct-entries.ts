import { describeOverload } from "./check-overloads.ts";
import type { Problems } from "./declarations.ts";
import type { ModelClass, ModelOverload, ModelParameter, ModelType, TableModel } from "./model.ts";
import {
  directValuesInTwoRegisters,
  directValueTypes,
  maximumDirectParameterCount,
  type DirectParameterSpec,
  type DirectValue,
  type OperandUse,
  type Primitive,
} from "./vocabulary.ts";

const cIdentifierPattern = /^[A-Za-z_][A-Za-z0-9_]*$/;
const int32Range = { minimum: -(2 ** 31), maximum: 2 ** 31 - 1 };
const maximumGeneralRegisterCount = 6;

function holdsOnlyPrimitive(type: ModelType, primitive: Primitive): boolean {
  const { isAnything, hasAnyObject, hasAnyFunction, hasPlainData, primitives, integers, stringLiterals, parts } = type;
  if (isAnything || hasAnyObject || hasAnyFunction || hasPlainData || parts.length > 0) return false;
  return integers === null && stringLiterals.length === 0 && primitives.every(held => held === primitive);
}

function holdsOnlyInt32(type: ModelType): boolean {
  const { primitives, integers } = type;
  if (integers === null || primitives.length > 0 || !holdsOnlyPrimitive({ ...type, integers: null }, "Number"))
    return false;
  return integers.minimum.value >= int32Range.minimum && integers.maximum.value <= int32Range.maximum;
}

function holdsOnlyInstances({
  isAnything,
  hasAnyObject,
  hasAnyFunction,
  hasPlainData,
  primitives,
  integers,
  stringLiterals,
  parts,
}: ModelType): boolean {
  if (isAnything || hasAnyObject || hasAnyFunction || hasPlainData || integers !== null) return false;
  return (
    primitives.length === 0 &&
    stringLiterals.length === 0 &&
    parts.length > 0 &&
    parts.every(({ part }) => part === "Instance")
  );
}

const bytesOfReceiver: readonly DirectValue[] = ["BytesOfReceiver", "BytesOfReceiverInRange"];
const bytesInRange: readonly DirectValue[] = ["BytesInRange", "BytesOfReceiverInRange"];
const takesReceiver = ({ value }: DirectParameterSpec) =>
  value === "NativeObjectOfReceiver" || value === "Receiver" || bytesOfReceiver.includes(value) || value === "FrameOfValues";
const takesArgument = (parameter: DirectParameterSpec) =>
  parameter.value === "FrameOfValues" || (parameter.value !== "Realm" && !takesReceiver(parameter));

// The address of a cell is handed over where the function declares a pointer, so nothing else may come.
function fitsPointer(cppType: string, accepted: ModelType): boolean {
  const { isAnything, hasAnyObject, hasAnyFunction, hasPlainData, integers, parts } = accepted;
  const primitives = accepted.primitives.filter(primitive => primitive !== "NotPassed");
  const holdsObjects = hasAnyObject || hasAnyFunction || hasPlainData || parts.length > 0;
  if (isAnything || integers !== null) return false;
  switch (cppType) {
    case "JSC::JSString*":
      return !holdsObjects && primitives.every(primitive => primitive === "String");
    case "JSC::JSObject*":
      return primitives.length === 0 && accepted.stringLiterals.length === 0;
    default:
      return primitives.every(primitive => primitive === "String" || primitive === "Symbol");
  }
}

const isViewClass = (builtinClass: ModelClass | null): boolean =>
  builtinClass !== null &&
  (builtinClass.token === "TypedArray" || builtinClass.token === "DataView" || isViewClass(builtinClass.parent));

function holdsOnlyViews(type: ModelType): boolean {
  return holdsOnlyInstances(type) && type.parts.every(part => part.part === "Instance" && isViewClass(part.class));
}

// What a function can do to an operand of which it has nothing but the memory, and only until it returns.
const usesOfContentsDuringCall: readonly OperandUse[] = ["Inspects", "ReadsContents", "WritesContents"];

function runsNoCodeOf({ protocols, namedMembers, calledMembers, convertedMembers }: ModelParameter): boolean {
  return [protocols, namedMembers, calledMembers, convertedMembers].every(list => list.length === 0);
}

// The uses of the row decide. A function that keeps, returns or calls an operand, or reads it after it has returned, gets the operand itself.
function findContentsProblems({ value }: DirectParameterSpec, parameter: ModelParameter): string[] {
  const { name, accepted, uses, roles } = parameter;
  switch (value) {
    case "Bytes":
    case "BytesOfReceiver":
    case "BytesInRange":
    case "BytesOfReceiverInRange": {
      const others = uses.filter(use => !usesOfContentsDuringCall.includes(use));
      if (others.length > 0 || !runsNoCodeOf(parameter))
        return [`${name}: ${value} hands over memory for the time of the call, and the row says more: ${others.join(", ") || "it runs code of the operand"}`];
      return holdsOnlyViews(accepted) ? [] : [`${name}: ${value} is for typed arrays and DataViews, and for nothing else`];
    }
    case "Characters":
      if (uses.some(use => use !== "Inspects") || !runsNoCodeOf(parameter) || roles.filter(role => role !== "DescribedWhenRefused").join() !== "ReadAsCharacters")
        return [`${name}: Characters is for a string that is read as characters, and for nothing else`];
      return holdsOnlyPrimitive({ ...accepted, stringLiterals: [] }, "String") ? [] : [`${name}: Characters is for strings`];
    default:
      return [];
  }
}

function isHandedOverAs({ value, cppType }: DirectParameterSpec, { accepted }: ModelParameter): boolean {
  switch (value) {
    case "Value":
    case "ValueOrEmpty":
      return cppType === "JSC::EncodedJSValue" || fitsPointer(cppType, accepted);
    case "Boolean":
      return holdsOnlyPrimitive(accepted, "Boolean");
    case "Int32":
      return holdsOnlyInt32(accepted);
    case "Double":
      return holdsOnlyPrimitive(accepted, "Number") || holdsOnlyInt32(accepted);
    default:
      return true;
  }
}

function findArgumentProblems(direct: DirectParameterSpec, parameter: ModelParameter, isLast: boolean): string[] {
  const problems: string[] = [];
  if (parameter.conversion !== "None") {
    problems.push(`${parameter.name} is converted, and the function takes its operands as they are`);
  }
  if ((parameter.mode === "Rest") !== (direct.value === "RestOfValues") || (parameter.mode === "Rest" && !isLast)) {
    problems.push(`${parameter.name}: RestOfValues is for the operands that are left, and for nothing else`);
  }
  if ((parameter.mode === "Optional") !== (direct.value === "ValueOrEmpty")) {
    problems.push(`${parameter.name}: ValueOrEmpty is for an operand that can be left out, and for nothing else`);
  }
  if (!isHandedOverAs(direct, parameter))
    problems.push(`${parameter.name} can be something that ${direct.value} does not hand over`);
  return [...problems, ...findContentsProblems(direct, parameter)];
}

function findFormProblems(symbol: string, direct: readonly DirectParameterSpec[]): string[] {
  const problems: string[] = [];
  for (const { value, cppType } of direct) {
    const types: readonly string[] | undefined = directValueTypes[value];
    if (types === undefined) problems.push(`${value} is not one of ${Object.keys(directValueTypes).join(", ")}`);
    else if (!types.includes(cppType)) problems.push(`${cppType} is not handed over as ${value}`);
  }
  if (direct.length > maximumDirectParameterCount) {
    problems.push(`${symbol} has more than ${maximumDirectParameterCount} parameters`);
  }
  const generalRegisters = direct.reduce(
    (count, { value }) => count + (value === "Double" ? 0 : directValuesInTwoRegisters.includes(value) ? 2 : 1),
    0,
  );
  if (generalRegisters > maximumGeneralRegisterCount) {
    problems.push(`${symbol} takes more than fits the ${maximumGeneralRegisterCount} general argument registers`);
  }
  return problems;
}

function findReceiverProblems({ mode, receiver, code: { directEntry } }: ModelOverload): string[] {
  const { symbol, parameters, offsetOfNativeObject } = directEntry!;
  const takesNativeObject = parameters.some(({ value }) => value === "NativeObjectOfReceiver");
  if (takesNativeObject !== (offsetOfNativeObject !== undefined)) {
    return [`${symbol}: offsetOfNativeObject goes with a parameter that is NativeObjectOfReceiver`];
  }
  if (!parameters.some(takesReceiver)) {
    // A receiver that the function only tests is tested by whoever picked the overload.
    const isOnlyTested =
      receiver !== null && receiver.conversion === "None" && receiver.uses.join() === "Inspects" && holdsOnlyInstances(receiver.accepted);
    return mode === "Call" && (receiver === null || isOnlyTested)
      ? []
      : [`${symbol} takes the arguments of a call and no receiver`];
  }
  if (receiver === null) return [`${symbol} takes the receiver, and the overload has none`];
  const takingBytes = parameters.find(({ value }) => bytesOfReceiver.includes(value));
  if (takingBytes !== undefined) return findContentsProblems(takingBytes, receiver);
  if (takesNativeObject && (receiver.conversion !== "None" || !holdsOnlyInstances(receiver.accepted))) {
    return [`${symbol} takes what the receiver wraps, so the receiver is an instance of a class`];
  }
  return [];
}

const valuesWithoutRealm: readonly DirectValue[] = [
  "Bytes",
  "BytesOfReceiver",
  "BytesInRange",
  "BytesOfReceiverInRange",
  "Characters",
  "Boolean",
  "Int32",
  "Double",
];

// Two registers come back. Nothing says that the function failed, so it has no realm to throw in or to allocate in.
function findFieldsProblems({ result, code: { directEntry } }: ModelOverload): string[] {
  const { symbol, parameters, returnsFields = false } = directEntry!;
  if (!returnsFields) return [];
  const [part] = result.type.parts;
  const isOneLayout = result.type.parts.length === 1 && part.part === "Layout" && result.type.primitives.length === 0;
  if (!isOneLayout || part.part !== "Layout" || part.layout.isTuple) return [`${symbol} returns fields, so the row returns one layout`];
  const { fields } = part.layout;
  const problems: string[] = [];
  if (fields.length > 2 || fields.some(({ isOptional, type }) => isOptional || type.integers === null || type.primitives.length > 0))
    problems.push(`${symbol} returns fields in two registers, so the layout has at most two fields, and they are integers`);
  if (parameters.some(({ value }) => !valuesWithoutRealm.includes(value)))
    problems.push(`${symbol} returns fields, so it takes nothing with which it could throw, allocate or call`);
  return problems;
}

// Returns a number. The function has no way to report failure, and compiled code sets up no frame for the call.
function findNumberProblems({ result, effects, flags, code: { directEntry } }: ModelOverload): string[] {
  const { symbol, returnsFields = false, returnsNumber = false } = directEntry!;
  if (!returnsNumber) return [];
  const problems: string[] = [];
  if (returnsFields) problems.push(`${symbol} returns a number or fields, not both`);
  if (!holdsOnlyPrimitive({ ...result.type, integers: null }, "Number")) problems.push(`${symbol} returns a number, so the row returns nothing else`);
  if (effects.capability !== "none" || flags.includes("Throws"))
    problems.push(`${symbol} returns a number, so it neither throws nor allocates nor calls`);
  return problems;
}

function findProblems(overload: ModelOverload): string[] {
  const {
    builtin,
    parameters,
    code: { directEntry, implementation },
  } = overload;
  if (directEntry === null) return [];
  const { symbol } = directEntry;
  const problems = [...findFormProblems(symbol, directEntry.parameters), ...findReceiverProblems(overload)];
  if (!cIdentifierPattern.test(symbol)) problems.push(`${JSON.stringify(symbol)} is not the name of a C function`);
  if (builtin.scope === "Module" && builtin.attachments.nativeFunction === null) {
    problems.push(
      `compiled code knows the function that ${symbol} stands in for by its native code, so the entry has functionObjectCode`,
    );
  }
  if (builtin.origin !== "Host") {
    problems.push(`${symbol} is bound with the functions of the host, and the entry is the engine's`);
  }
  if (implementation.how !== "HostCall")
    problems.push(`${symbol} stands in for native code of the host, so the overload is a call of the host`);
  problems.push(...findFieldsProblems(overload), ...findNumberProblems(overload));
  const taken = directEntry.parameters.filter(takesArgument);
  if (taken.length === 1 && taken[0].value === "FrameOfValues") return problems;
  // The bounds of each part of a view come last, and go into the address and the count of that view.
  const numberOfBounds = 2 * directEntry.parameters.filter(({ value }) => bytesInRange.includes(value)).length;
  if (taken.length + numberOfBounds !== parameters.length) {
    return [
      ...problems,
      `${symbol} takes ${taken.length + numberOfBounds} arguments, and the overload has ${parameters.length}`,
    ];
  }
  return [
    ...problems,
    ...parameters
      .slice(0, taken.length)
      .flatMap((parameter, index) => findArgumentProblems(taken[index], parameter, index === taken.length - 1)),
    ...parameters
      .slice(taken.length)
      .flatMap(({ name, mode, uses }) =>
        mode === "Required" && uses.join() === "Inspects" ? [] : [`${name}: a bound of a part of a view is only looked at`],
      ),
  ];
}

const columnsOfParameter = (parameter: ModelParameter): Record<string, string> => ({
  "accepted type": parameter.accepted.key,
  mode: parameter.mode,
  conversion: parameter.conversion,
  uses: parameter.uses.join(),
  roles: parameter.roles.join(),
  protocols: parameter.protocols.join(),
  "named members": [parameter.namedMembers, parameter.calledMembers, parameter.convertedMembers]
    .map(names => names.map(({ text }) => text).join())
    .join(" | "),
});

// Both enter the same C function with the same memory. What the compiler believes about a call must not change when the call moves from one to the other.
function findProblemsOfFormInRanges({ parameters, effects, result, code: { inRanges } }: ModelOverload): string[] {
  if (inRanges == null) return [];
  const form = inRanges.overload;
  const differs = (what: string, ofRow: string, ofForm: string) =>
    ofRow === ofForm ? [] : [`its form in ranges has other ${what}: ${ofForm || "none"}, and the row has ${ofRow || "none"}`];
  return [
    ...differs("reads", effects.reads.join(), form.effects.reads.join()),
    ...differs("writes", effects.writes.join(), form.effects.writes.join()),
    ...differs("capability", effects.capability, form.effects.capability),
    ...differs("result", result.type.key, form.result.type.key),
    ...parameters.flatMap((parameter, index) => {
      const ofRow = columnsOfParameter(parameter);
      const ofForm = columnsOfParameter(form.parameters[index]);
      return Object.keys(ofRow).flatMap(column => differs(`${column} of ${parameter.name}`, ofRow[column], ofForm[column]));
    }),
  ];
}

// Compiled code enters the C function where it would have called the function object, which stays the way in for every other caller.
export function checkDirectEntries({ overloads }: Pick<TableModel, "overloads">, problems: Problems): void {
  for (const overload of overloads) {
    // What is written for a native binding describes the function that it makes, whose entry has the same overloads.
    if (overload.builtin.kind === "NativeBinding") continue;
    for (const problem of [...findProblems(overload), ...findProblemsOfFormInRanges(overload)])
      problems.add(overload.source, `${describeOverload(overload)}: ${problem}`);
  }
}
