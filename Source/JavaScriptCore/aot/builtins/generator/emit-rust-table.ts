import { screamingCase } from "../../ir/generator/names.ts";
import { GeneratorError } from "./declarations.ts";
import { closureOf, type CallsForTexts, type DependencyClosure, type LoadedModule } from "./dependency-closure.ts";
import {
  emitArray,
  emitCount,
  emitSlice,
  fileHeader,
  formatBuiltinId,
  formatBuiltinName,
  formatByteString,
  formatClass,
  formatModule,
  formatName,
  formatOption,
  formatRuntimeFunction,
  formatString,
  formatStruct,
  formatWellKnownSymbol,
  sortModules,
  sortUniqueNumbers,
  usesNameBeforeNul,
} from "./emit-rust-ids.ts";
import {
  builtinFlags,
  type ModelBuiltin,
  type ModelClass,
  type ModelFlow,
  type ModelFunction,
  type ModelHostRead,
  type ModelModule,
  type ModelName,
  type ModelOverload,
  type ModelParameter,
  type ModelPlace,
  type ModelPrototype,
  type ModelResult,
  type ModelStartup,
  type ModelType,
  type ModelTypeName,
  type NameKind,
  type PropertyActionKind,
  type TableModel,
} from "./model.ts";
import { openRealmOf } from "./open-realm.ts";
import { compareBytes } from "./paths.ts";
import { targetPlatforms, type ContentsSlot, type Primitive, type TargetPlatform } from "./vocabulary.ts";

const moduleFlags = ["HasSource", "HasOpenNamespace", "ListsAllExports", "IsEsModule", "IsPublic"] as const;
type ModuleFlag = (typeof moduleFlags)[number];

const formatContentsSlot = (slot: ContentsSlot): string => `ContentsSlot::${slot}`;

const formatFunctions = (functions: Iterable<ModelFunction>): string[] =>
  sortUniqueNumbers(Array.from(functions, runtimeFunction => runtimeFunction.id)).map(formatRuntimeFunction);

function formatBits<Word extends string>(rustType: string, words: readonly Word[], set: readonly Word[]): string {
  let bits = 0;
  for (const word of set) {
    const position = words.indexOf(word);
    if (position < 0) throw new GeneratorError(`${rustType} has no bit for ${word}`);
    bits |= 1 << position;
  }
  return `${rustType}::from_bits_retain(0x${bits.toString(16)})`;
}

const formatPlatforms = (platforms: readonly TargetPlatform[]): string =>
  formatBits("Platforms", targetPlatforms, platforms);

function emitBitChecks(rustType: string, constants: readonly string[]): string {
  return constants
    .map((constant, position) => `const _: () = assert!(${rustType}::${constant}.bits() == 1 << ${position});\n`)
    .join("");
}

class StaticListPool {
  readonly #prefix: string;
  readonly #elementType: string;
  readonly #names = new Map<string, string>();
  readonly #definitions: string[] = [];

  constructor(prefix: string, elementType: string) {
    this.#prefix = prefix;
    this.#elementType = elementType;
  }

  add(elements: readonly string[]): string {
    if (elements.length === 0) return "&[]";
    const text = elements.join(", ");
    let name = this.#names.get(text);
    if (name === undefined) {
      name = `${this.#prefix}_${this.#names.size}`;
      this.#names.set(text, name);
      this.#definitions.push(`static ${name}: [${this.#elementType}; ${elements.length}] = [${text}];\n`);
    }
    return `&${name}`;
  }

  emit(): string {
    return this.#definitions.join("");
  }
}

class DependencyPool {
  readonly #numbers = new Map<string, number>();
  readonly #elements: string[] = [];

  add(lists: Readonly<Record<string, string>>): number {
    const element = formatStruct(
      "Dependencies",
      Object.entries(lists).map(([field, list]) => `${field}: ${list}`),
    );
    let number = this.#numbers.get(element);
    if (number === undefined) {
      number = this.#elements.length;
      if (number > 0xffff) throw new GeneratorError("there are more lists of dependencies than 16 bits number");
      this.#numbers.set(element, number);
      this.#elements.push(element);
    }
    return number;
  }

  emit(): string {
    return emitArray(`static DEPENDENCIES: [Dependencies; ${this.#elements.length}]`, this.#elements);
  }
}

const unlessLinkedWithEngine = '#[cfg(not(feature = "linked_names"))]\n';

function emitTexts(name: string, count: string, texts: readonly string[], attribute: string): string {
  const starts: number[] = [0];
  for (const text of texts) starts.push(starts[starts.length - 1] + Buffer.byteLength(text, "utf8"));
  return [
    `${attribute}static ${name}_BYTES: &[u8] = ${formatByteString(texts.join(""))};\n`,
    attribute + emitArray(`static ${name}_STARTS: [u32; ${count} + 1]`, starts.map(String)),
  ].join("");
}

interface EntryContext {
  closure: DependencyClosure;
  moduleOfNamespace: ReadonlyMap<ModelBuiltin, ModelModule>;
  moduleLoadLists: StaticListPool;
  functionLists: StaticListPool;
  nameLists: StaticListPool;
  callsForTextsLists: StaticListPool;
  writtenOperandLists: StaticListPool;
  quietCallLists: StaticListPool;
  dependencies: DependencyPool;
}

function findModulesByNamespace(modules: readonly ModelModule[]): Map<ModelBuiltin, ModelModule> {
  const moduleOfNamespace = new Map<ModelBuiltin, ModelModule>();
  for (const module of modules) {
    if (!moduleOfNamespace.has(module.namespace)) moduleOfNamespace.set(module.namespace, module);
  }
  return moduleOfNamespace;
}

function formatPrototype(prototype: ModelPrototype): string {
  switch (prototype.link) {
    case "Unknown":
    case "Null":
      return `Prototype::${prototype.link}`;
    case "Builtin":
      return `Prototype::Builtin(${formatBuiltinId(prototype.builtin)})`;
  }
}

function firstMemberOf({ members, text }: ModelBuiltin): number {
  if (members.length === 0) return 0;
  const first = members[0].id;
  members.forEach((member, index) => {
    if (member.id !== first + index) throw new GeneratorError(`the members of ${text} are not one run of ids`);
  });
  return first;
}

function formatExportsRead(names: readonly string[] | null): string {
  return names === null ? "None" : `Some(&[${names.map(formatString).sort().join(", ")}])`;
}

function formatModuleLoads(loads: ReadonlyMap<ModelModule, LoadedModule>): string[] {
  return sortModules([...loads.keys()]).map(module => {
    const { exportsRead } = loads.get(module)!;
    return formatStruct("ModuleLoad", [
      `module: ${formatModule(module)}`,
      `exports_read: ${formatExportsRead(exportsRead === null ? null : [...exportsRead])}`,
    ]);
  });
}

function formatCallsForTexts(listed: ReadonlyMap<string, CallsForTexts>, context: EntryContext): string[] {
  return [...listed.keys()].sort(compareBytes).map(key => {
    const { argument, texts, calls } = listed.get(key)!;
    return formatStruct("CallsForTexts", [
      `argument: ${argument}`,
      `texts: ${context.nameLists.add(texts.map(text => formatBuiltinName(text.number)))}`,
      `calls: ${context.functionLists.add(formatFunctions(calls))}`,
    ]);
  });
}

const holdsNoObject = (type: ModelType): boolean =>
  !type.isAnything && !type.hasAnyObject && !type.hasAnyFunction && !type.hasPlainData && type.parts.length === 0;

const holdsNothing = (type: ModelType): boolean =>
  holdsNoObject(type) && type.primitives.length === 0 && type.integers === null && type.stringLiterals.length === 0;

const isOfProgramOrPrimitive = (place: ModelPlace): boolean =>
  place.place === "None" || place.place === "Operand" || (place.place === "NewValue" && holdsNoObject(place.type));

function handsCodeOnlyWhatProgramHandedIn({ callbacks, handlerCalls, propertyActions }: ModelFlow): boolean {
  return (
    handlerCalls.length === 0 &&
    propertyActions.length === 0 &&
    callbacks.every(
      ({ constructs, arguments: passed, spreadArguments, resultUse }) =>
        !constructs &&
        (resultUse === "Ignored" || resultUse === "Tested") &&
        [...passed, spreadArguments].every(isOfProgramOrPrimitive),
    )
  );
}

function formatWrittenOperand({
  mode,
  accepted,
  conversion,
  refused,
}: Pick<ModelParameter, "mode" | "accepted" | "conversion" | "refused">): string | null {
  if (conversion !== "None" || !holdsNothing(refused)) return null;
  if (accepted.isAnything) return mode === "Rest" ? "WrittenOperand::AnyFurther" : "WrittenOperand::Any";
  const isOfOneKind = !accepted.hasAnyObject && !accepted.hasPlainData && accepted.parts.length === 0;
  if (mode === "Rest") {
    if (!holdsNoObject(accepted) || accepted.integers !== null || accepted.stringLiterals.length > 0) return null;
    const accepts = (primitive: Primitive): boolean => accepted.primitives.includes(primitive);
    return `WrittenOperand::PrimitivesFurther { null: ${accepts("Null")}, boolean: ${accepts("Boolean")}, number: ${accepts("Number")}, string: ${accepts("String")} }`;
  }
  if (accepted.hasAnyFunction) {
    return isOfOneKind && mode === "Required" && accepted.primitives.length === 0 ? "WrittenOperand::Function" : null;
  }
  if (accepted.integers === null || !isOfOneKind || accepted.stringLiterals.length > 0) return null;
  const { minimum, maximum } = accepted.integers;
  return `WrittenOperand::Integer { least: ${minimum.value}, most: ${maximum.value}, may_be_left_out: ${mode === "Optional"} }`;
}

function findOperandsCalledWithReceiver({ overloads }: ModelBuiltin): number[] {
  const called = overloads.flatMap(({ flow }) =>
    flow.callbacks.flatMap(({ callee, thisValue }) =>
      thisValue.place === "Operand" && thisValue.operand.operand === "Receiver" && callee.operand !== "Receiver"
        ? [callee.index]
        : [],
    ),
  );
  return [...new Set(called)].sort((left, right) => left - right);
}

function mayAnswerReceiver({ overloads }: ModelBuiltin): boolean {
  return overloads.some(({ result }) =>
    [result.type, ...result.typesByPlatform.map(({ type }) => type)].some(({ parts }) =>
      parts.some(part => part.part === "Operand" && part.operand.operand === "Receiver"),
    ),
  );
}

type OverloadJudgedForQuietCalls = Pick<ModelOverload, "mode" | "effects" | "receiver" | "flags" | "flow"> & {
  code?: Pick<ModelOverload["code"], "directEntry">;
  parameters: readonly Pick<ModelParameter, "mode" | "accepted" | "conversion" | "refused">[];
  result: Pick<ModelResult, "type">;
};

const standsInFrontForItsDoor = (
  { code, flags }: OverloadJudgedForQuietCalls,
  behind: readonly OverloadJudgedForQuietCalls[],
): boolean =>
  code?.directEntry != null &&
  flags.includes("RunsOnlyOperandCode") &&
  behind.some(({ parameters }) => parameters.some(({ mode, accepted }) => mode === "Rest" && accepted.isAnything));

export function listQuietCalls(
  overloads: readonly OverloadJudgedForQuietCalls[],
): { operands: string[]; answersPrimitive: boolean }[] {
  const calls = overloads.filter(({ mode }) => mode === "Call");
  const quiet: { operands: string[]; answersPrimitive: boolean }[] = [];
  let answersPrimitive = true;
  let isBehindDoor = false;
  for (const [index, overload] of calls.entries()) {
    const { parameters, receiver, flags, result, flow, effects } = overload;
    if (effects.capability === "js") {
      if (!standsInFrontForItsDoor(overload, calls.slice(index + 1))) break;
      answersPrimitive &&= holdsNoObject(result.type);
      isBehindDoor = true;
      continue;
    }
    answersPrimitive &&= holdsNoObject(result.type);
    if (receiver !== null || flags.includes("IsUndeclared") || !handsCodeOnlyWhatProgramHandedIn(flow)) break;
    if (isBehindDoor && !parameters.every(({ accepted }) => holdsNoObject(accepted))) break;
    const operands = parameters.map(formatWrittenOperand);
    if (operands.includes(null)) {
      if (parameters.every(({ accepted, conversion }) => conversion === "None" && holdsNoObject(accepted))) continue;
      break;
    }
    quiet.push({ operands: operands.map(operand => operand ?? ""), answersPrimitive });
  }
  return quiet;
}

function formatQuietCalls(builtin: ModelBuiltin, context: EntryContext): string[] {
  return listQuietCalls(builtin.overloads).map(
    ({ operands, answersPrimitive }) =>
      `QuietCall { operands: ${context.writtenOperandLists.add(operands)}, answers_primitive: ${answersPrimitive} }`,
  );
}

function formatEntry(builtin: ModelBuiltin, context: EntryContext): string {
  const dependencies = context.closure.dependenciesOf(builtin);
  const { loads, calls, callsForTexts, callsWhereCallRunsOtherCode, implementedBy, certain } = dependencies;
  const quietCalls = formatQuietCalls(builtin, context);
  return formatStruct("BuiltinEntry", [
    `kind: BuiltinKind::${builtin.kind}`,
    `origin: Origin::${builtin.origin}`,
    `scope: Scope::${builtin.scope}`,
    `mutability: Mutability::${builtin.mutability}`,
    `platforms: ${formatPlatforms(builtin.platforms)}`,
    `flags: ${formatBits("BuiltinFlags", builtinFlags, builtin.flags)}`,
    `is_added_at_first_read: ${builtin.isAddedAtFirstRead}`,
    `holder: ${formatOption(builtin.holder, formatBuiltinId)}`,
    `name: ${formatBuiltinName(builtin.name.number)}`,
    `prototype: ${formatPrototype(builtin.prototype)}`,
    `class: ${formatOption(builtin.instanceClass, formatClass)}`,
    `module: ${formatOption(context.moduleOfNamespace.get(builtin) ?? null, formatModule)}`,
    `same_object_as: ${formatOption(builtin.sameObjectAs, formatBuiltinId)}`,
    `first_member: ${firstMemberOf(builtin)}`,
    `member_count: ${builtin.members.length}`,
    `dependencies: ${context.dependencies.add({
      loads: context.moduleLoadLists.add(formatModuleLoads(loads)),
      calls: context.functionLists.add(formatFunctions(calls.intersection(certain))),
      calls_for_texts: context.callsForTextsLists.add(formatCallsForTexts(callsForTexts, context)),
      calls_where_call_runs_other_code: context.functionLists.add(formatFunctions(callsWhereCallRunsOtherCode)),
      calls_that_run_no_other_code: context.quietCallLists.add(quietCalls),
      implemented_by: context.functionLists.add(formatFunctions(implementedBy.intersection(certain))),
      guessed: context.functionLists.add(formatFunctions(calls.union(implementedBy).difference(certain))),
    })}`,
  ]);
}

function emitEntries(model: TableModel): string {
  const context: EntryContext = {
    closure: closureOf(model),
    moduleOfNamespace: findModulesByNamespace(model.modules),
    moduleLoadLists: new StaticListPool("MODULE_LOAD_LIST", "ModuleLoad"),
    functionLists: new StaticListPool("FUNCTION_LIST", "RuntimeFunction"),
    nameLists: new StaticListPool("NAME_LIST", "BuiltinName"),
    callsForTextsLists: new StaticListPool("CALLS_FOR_TEXTS_LIST", "CallsForTexts"),
    writtenOperandLists: new StaticListPool("WRITTEN_OPERAND_LIST", "WrittenOperand"),
    quietCallLists: new StaticListPool("QUIET_CALL_LIST", "QuietCall"),
    dependencies: new DependencyPool(),
  };
  const entries = model.builtins.map(builtin => formatEntry(builtin, context));
  return [
    "static BUILTINS_IN_ORDER: [BuiltinId; BuiltinId::COUNT as usize] = BuiltinId::ALL;\n",
    context.moduleLoadLists.emit(),
    context.functionLists.emit(),
    context.nameLists.emit(),
    context.callsForTextsLists.emit(),
    context.writtenOperandLists.emit(),
    context.quietCallLists.emit(),
    context.dependencies.emit(),
    emitArray("static ENTRIES: [BuiltinEntry; BuiltinId::COUNT as usize]", entries),
  ].join("");
}

function emitConstants(model: TableModel): string {
  const countOf = (kind: NameKind): number => model.names.filter(name => name.kind === kind).length;
  return [
    emitCount("TEXT_NAME_COUNT", countOf("Text")),
    emitCount("FIRST_REGISTERED_SYMBOL_NAME", countOf("Text") + countOf("WellKnownSymbol")),
    emitBitChecks(
      "Platforms",
      targetPlatforms.map(platform => platform.toUpperCase()),
    ),
    emitBitChecks("BuiltinFlags", builtinFlags.map(screamingCase)),
    emitBitChecks("ModuleFlags", moduleFlags.map(screamingCase)),
  ].join("");
}

function formatSymbolConstants(builtins: readonly ModelBuiltin[]): string[] {
  return builtins.flatMap(builtin => {
    const { constant } = builtin;
    if (constant === null || constant.constant !== "Symbol") return [];
    const { name } = constant;
    return [`(${formatBuiltinId(builtin)}, ${formatBuiltinName(name.number)}, ${formatWellKnownSymbol(name)})`];
  });
}

function emitNames(model: TableModel): string {
  return [
    emitTexts(
      "NAME_TEXT",
      "TEXT_NAME_COUNT",
      model.names.filter(name => name.kind === "Text").map(name => name.text),
      unlessLinkedWithEngine,
    ),
    emitTexts(
      "SYMBOL_NAME_TEXT",
      "(BuiltinName::COUNT as usize - TEXT_NAME_COUNT)",
      model.names.filter(name => name.kind !== "Text").map(name => name.text),
      "",
    ),
    emitSlice(
      "static SYMBOL_CONSTANTS: &[(BuiltinId, BuiltinName, WellKnownSymbol)]",
      formatSymbolConstants(model.builtins),
    ),
  ].join("");
}

const storingActions: readonly PropertyActionKind[] = [
  "StoresByKey",
  "DefinesName",
  "TakesDescriptor",
  "DefinesGetter",
  "DefinesSetter",
];

const namesOfResultField = ({ type }: ModelResult, index: number): (ModelName | null)[] =>
  type.parts.flatMap(part => (part.part === "Layout" ? [part.layout.fields[index].name] : []));

function findNamesStoredByNativeCode(model: TableModel): Set<ModelName> {
  const stored = new Set<ModelName>();
  const add = (names: Iterable<ModelName | null>): void => {
    for (const name of names) if (name !== null) stored.add(name);
  };
  add(model.builtins.filter(builtin => builtin.holder !== null).map(builtin => builtin.name));
  for (const { namesDefinedOnNewInstances } of model.classes) {
    if (namesDefinedOnNewInstances !== null) add(namesDefinedOnNewInstances);
  }
  for (const layout of model.layouts) add(layout.fields.map(field => field.name));
  for (const hostObject of model.hostObjects) add([...hostObject.fields.map(field => field.name), ...hostObject.nativeAccessors, ...hostObject.lockedNames]);
  for (const { result, flow } of model.overloads) {
    add(result.properties.map(property => property.name));
    for (const { to } of flow.transfers) {
      if (to.place === "ResultField" && !to.isOfElements) add(namesOfResultField(result, to.index));
    }
    for (const { action, key } of flow.propertyActions) {
      if (key.key === "Names" && storingActions.includes(action)) add(key.names);
    }
  }
  return stored;
}

function emitNamesStoredByNativeCode(model: TableModel): string {
  const bits = new Uint8Array(Math.ceil(model.names.length / 8));
  for (const { number } of findNamesStoredByNativeCode(model)) bits[number >> 3] |= 1 << (number & 7);
  const length = "(BuiltinName::COUNT as usize).div_ceil(8)";
  return `static NAMES_STORED_BY_NATIVE_CODE: [u8; ${length}] = [${bits.join(", ")}];\n`;
}

function formatTypeName({ path, isView, typeArguments }: ModelTypeName): string {
  const contentsSlots = typeArguments.map(slot => formatOption(slot.contentsSlot, formatContentsSlot));
  return formatStruct("TypeName", [
    `path: &[${path.map(component => formatString(component)).join(", ")}]`,
    `is_view: ${isView}`,
    `type_arguments: &[${contentsSlots.join(", ")}]`,
  ]);
}

function formatClassContents(builtinClass: ModelClass): string[] {
  return builtinClass.slots
    .flatMap(({ contentsSlot, indexInType }) =>
      contentsSlot === null || indexInType === null ? [] : [{ contentsSlot, indexInType }],
    )
    .sort((left, right) => left.indexInType - right.indexInType)
    .map(({ contentsSlot }) => formatContentsSlot(contentsSlot));
}

function formatClassData(builtinClass: ModelClass): string {
  return formatStruct("ClassData", [
    `parent: ${formatOption(builtinClass.parent, formatClass)}`,
    `constructor: ${formatOption(builtinClass.constructor, formatBuiltinId)}`,
    `prototype: ${formatOption(builtinClass.prototype, formatBuiltinId)}`,
    `has_instance_test: ${builtinClass.instanceTest !== "None"}`,
    `contents: &[${formatClassContents(builtinClass).join(", ")}]`,
    `type_names: &[${builtinClass.types.map(formatTypeName).join(", ")}]`,
  ]);
}

function flagsOfModule(module: ModelModule): ModuleFlag[] {
  const isSet: Record<ModuleFlag, boolean> = {
    HasSource: module.hasSource,
    HasOpenNamespace: module.hasOpenNamespace,
    ListsAllExports: module.listsAllExports,
    IsEsModule: module.isEsModule,
    IsPublic: module.isPublic,
  };
  return moduleFlags.filter(flag => isSet[flag]);
}

function formatModuleData(module: ModelModule): string {
  return formatStruct("ModuleData", [
    `namespace: ${formatBuiltinId(module.namespace)}`,
    `specifier: ${formatName(module.specifier)}`,
    `flags: ${formatBits("ModuleFlags", moduleFlags, flagsOfModule(module))}`,
    `exports_holding_module_object: &[${module.exportsHoldingModuleObject.map(name => formatBuiltinName(name.number)).join(", ")}]`,
    `exports_held_by_namespace: &[${module.exportsHeldByNamespace.map(name => formatBuiltinName(name.number)).join(", ")}]`,
  ]);
}

function emitClassesAndModules(model: TableModel): string {
  const modulesBySpecifier = [...model.modules].sort((left, right) => compareBytes(left.specifier, right.specifier));
  return [
    emitArray("static CLASSES: [ClassData; BuiltinClass::COUNT as usize]", model.classes.map(formatClassData)),
    emitArray("static MODULES: [ModuleData; RuntimeModule::COUNT as usize]", model.modules.map(formatModuleData)),
    emitArray(
      "static MODULES_BY_SPECIFIER: [RuntimeModule; RuntimeModule::COUNT as usize]",
      modulesBySpecifier.map(formatModule),
    ),
  ].join("");
}

function formatRuntimeFunctionData({ id, role, file, exportName, platforms }: ModelFunction): string {
  return formatStruct("RuntimeFunctionData", [
    `function: ${formatRuntimeFunction(id)}`,
    `role: RuntimeFunctionRole::${role}`,
    `file: ${formatString(file)}`,
    `export_name: ${formatString(exportName)}`,
    `platforms: ${formatPlatforms(platforms)}`,
  ]);
}

function formatStartupLoad({ module, exports }: ModelStartup["loads"][number]): string {
  return formatStruct("StartupLoad", [
    `module: ${formatModule(module)}`,
    `exports_read: ${formatExportsRead(exports?.map(({ name }) => name.text) ?? null)}`,
  ]);
}

function emitStartup({
  modules,
  loads,
  functionsCalledWithLoads,
  namesOfMethodsCallingLoads,
  moduleLoaderFunctions,
}: ModelStartup): string {
  return [
    emitSlice("pub static STARTUP_MODULES: &[RuntimeModule]", sortModules(modules).map(formatModule)),
    emitSlice("pub static STARTUP_LOADS: &[StartupLoad]", loads.map(formatStartupLoad)),
    emitSlice(
      "pub static STARTUP_FUNCTIONS: &[RuntimeFunction]",
      formatFunctions(functionsCalledWithLoads.map(called => called.function)),
    ),
    emitSlice(
      "pub static NAMES_OF_METHODS_CALLING_STARTUP_LOADS: &[BuiltinName]",
      namesOfMethodsCallingLoads.map(name => formatBuiltinName(name.number)),
    ),
    emitSlice("pub static MODULE_LOADER_FUNCTIONS: &[RuntimeFunction]", formatFunctions(moduleLoaderFunctions)),
  ].join("");
}

function formatHostReadUse(use: ModelHostRead["use"]): string {
  switch (use.read) {
    case "Inspected":
    case "Called":
      return `HostReadUse::${use.read}`;
    case "PassedTo":
      return `HostReadUse::PassedTo { builtin: ${formatBuiltinId(use.builtin)}, argument: ${use.argument} }`;
  }
}

function formatHostRead({ context, exportName, use, namesLookedFor }: ModelHostRead): string {
  return formatStruct("HostRead", [
    `context: HostReadContext::${context}`,
    `export_name: ${formatBuiltinName(exportName.number)}`,
    `read_use: ${formatHostReadUse(use)}`,
    `names_looked_for: &[${namesLookedFor.map(name => formatBuiltinName(name.number)).join(", ")}]`,
  ]);
}

function emitEntriesLeadingToStartupLoads(model: TableModel): string {
  const closure = closureOf(model);
  return emitSlice(
    "pub static ENTRIES_LEADING_TO_STARTUP_LOADS: &[BuiltinId]",
    model.builtins.filter(builtin => closure.dependenciesOf(builtin).hasMethodCalledWithStartupLoads).map(formatBuiltinId),
  );
}

const actionsHandingOutWhatLiesUnderKey: readonly PropertyActionKind[] = [
  "ReadsByKey",
  "ReadsGetter",
  "ReadsSetter",
  "DescribesOwnProperties",
];

function emitEntriesReadingUnderKeysTheyAreHanded(model: TableModel): string {
  const readsUnderSuchKey = ({ overloads }: ModelBuiltin): boolean =>
    overloads.some(({ flow }) =>
      flow.propertyActions.some(
        ({ action, key }) => actionsHandingOutWhatLiesUnderKey.includes(action) && key.key !== "Names" && key.key !== "Indexes",
      ),
    );
  const handsOutWhatLiesThere = ({ overloads }: ModelBuiltin): boolean =>
    overloads.some(({ flow }) =>
      flow.propertyActions.some(
        ({ action, key, value }) =>
          actionsHandingOutWhatLiesUnderKey.includes(action) &&
          key.key !== "Names" &&
          key.key !== "Indexes" &&
          value.place !== "None" &&
          value.place !== "NativeMemory",
      ),
    );
  const mayBeGlobalObject = (type: ModelType): boolean =>
    type.parts.some(part => part.part === "Builtin" && part.builtin.flags.includes("IsGlobalObject"));
  const isGlobalObject = (place: ModelPlace): boolean => place.place === "NewValue" && mayBeGlobalObject(place.type);
  const handsOutGlobalObject = ({ overloads }: ModelBuiltin): boolean =>
    overloads.some(
      ({ flow, result }) =>
        [
          result.type,
          ...result.typesByPlatform.map(({ type }) => type),
          ...result.properties.map(({ type }) => type),
        ].some(mayBeGlobalObject) ||
        flow.transfers.some(({ from }) => isGlobalObject(from)) ||
        flow.propertyActions.some(({ value }) => isGlobalObject(value)) ||
        flow.callbacks.some(call =>
          [call.newTarget, call.thisValue, ...call.arguments, call.spreadArguments].some(isGlobalObject),
        ) ||
        flow.handlerCalls.some(call => [call.thisValue, ...call.arguments].some(isGlobalObject)),
    );
  return [
    emitSlice(
      "pub static ENTRIES_THAT_HAND_OUT_THE_GLOBAL_OBJECT: &[BuiltinId]",
      model.builtins.filter(handsOutGlobalObject).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static ENTRIES_READING_UNDER_KEYS_THEY_ARE_HANDED: &[BuiltinId]",
      model.builtins.filter(readsUnderSuchKey).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static ENTRIES_HANDING_OUT_WHAT_LIES_UNDER_KEYS: &[BuiltinId]",
      model.builtins.filter(handsOutWhatLiesThere).map(formatBuiltinId),
    ),
  ].join("");
}

function emitWhatAnyOperandIsAsked(model: TableModel): string {
  const closure = closureOf(model);
  const { calls, implementedBy } = closure.dependenciesOfAnyOperand();
  return [
    emitSlice(
      "pub static ENTRIES_THAT_MAY_HAND_OUT_ANY_HOST_OBJECT: &[BuiltinId]",
      model.builtins.filter(builtin => closure.dependenciesOf(builtin).exposesAnyHostObject).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static FUNCTIONS_FOUND_ON_ANY_OPERAND: &[RuntimeFunction]",
      formatFunctions(calls.union(implementedBy)),
    ),
    emitSlice(
      "pub static OPERANDS_CALLED_WITH_RECEIVER: &[(BuiltinId, u32)]",
      model.builtins.flatMap(builtin =>
        findOperandsCalledWithReceiver(builtin).map(index => `(${formatBuiltinId(builtin)}, ${index})`),
      ),
    ),
    emitSlice(
      "pub static ENTRIES_THAT_MAY_ANSWER_RECEIVER: &[BuiltinId]",
      model.builtins.filter(mayAnswerReceiver).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static ENTRIES_THAT_MAKE_PROMISES: &[BuiltinId]",
      model.builtins.filter(builtin => closure.makesPromise(builtin)).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static PROTOTYPES_THAT_COME_WITH_FIRST_PROMISE: &[BuiltinId]",
      closure.prototypesThatComeWithFirstPromise().map(formatBuiltinId),
    ),
    `pub const ANY_HOST_OBJECT_LEADS_TO_STARTUP_LOADS: bool = ${closure.dependenciesOfAnyHostObject().hasMethodCalledWithStartupLoads};
`,
  ].join("");
}

function emitFunctionsOfOpenRealm(model: TableModel): string {
  const { functionsOfLanguage, functionsOfHost } = openRealmOf(model);
  return emitSlice(
    "pub static FUNCTIONS_OF_OPEN_REALM: &[RuntimeFunction]",
    formatFunctions([...functionsOfLanguage, ...functionsOfHost]),
  );
}

function emitFunctionsForAnyHostObject(model: TableModel): string {
  const anyHostObject = closureOf(model).dependenciesOfAnyHostObject();
  const { calls, callsWhereCallRunsOtherCode, implementedBy } = anyHostObject;
  return emitSlice(
    "pub static FUNCTIONS_FOR_ANY_HOST_OBJECT: &[RuntimeFunction]",
    formatFunctions(calls.union(callsWhereCallRunsOtherCode).union(implementedBy)),
  );
}

export function emitRustTable(model: TableModel): string {
  return [
    fileHeader,
    usesNameBeforeNul,
    emitConstants(model),
    emitEntries(model),
    emitNames(model),
    emitNamesStoredByNativeCode(model),
    emitClassesAndModules(model),
    emitSlice("static RUNTIME_FUNCTIONS: &[RuntimeFunctionData]", model.functions.map(formatRuntimeFunctionData)),
    emitStartup(model.startup),
    emitSlice(
      "pub static ACCESSOR_FUNCTIONS: &[RuntimeFunction]",
      formatFunctions(
        model.functions.filter(({ implements: implemented }) => implemented.some(({ kind }) => kind === "Accessor")),
      ),
    ),
    emitFunctionsForAnyHostObject(model),
    emitEntriesLeadingToStartupLoads(model),
    emitEntriesReadingUnderKeysTheyAreHanded(model),
    emitWhatAnyOperandIsAsked(model),
    emitFunctionsOfOpenRealm(model),
    emitSlice(
      "pub static MEMBERS_CREATED_IN_SOME_RUNS: &[BuiltinId]",
      model.builtins.filter(builtin => builtin.isCreatedInSomeRuns).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static NAMES_FOUND_ABOVE_PROTOTYPES_LINKED_ANEW: &[BuiltinName]",
      model.builtins.flatMap(builtin => builtin.namesFoundAbove.map(name => formatBuiltinName(name.number))),
    ),
    emitSlice("static HOST_READS: &[HostRead]", model.hostReads.map(formatHostRead)),
  ].join("");
}
