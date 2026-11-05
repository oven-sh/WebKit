import { screamingCase } from "../../ir/generator/names.ts";
import { GeneratorError } from "./declarations.ts";
import { DependencyClosure, type CallsForTexts, type LoadedModule } from "./dependency-closure.ts";
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
  formatOption,
  formatRuntimeFunction,
  formatString,
  formatStruct,
  formatWellKnownSymbol,
  sortModules,
  sortUniqueNumbers,
} from "./emit-rust-ids.ts";
import {
  builtinFlags,
  type ModelBuiltin,
  type ModelClass,
  type ModelFunction,
  type ModelHostRead,
  type ModelModule,
  type ModelName,
  type ModelPrototype,
  type ModelResult,
  type ModelStartup,
  type ModelTypeName,
  type NameKind,
  type PropertyActionKind,
  type TableModel,
} from "./model.ts";
import { openRealmOf } from "./open-realm.ts";
import { compareBytes } from "./paths.ts";
import { targetPlatforms, type ContentsSlot, type TargetPlatform } from "./vocabulary.ts";

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

  add(loads: string, calls: string, callsForTexts: string, implementedBy: string, guessed: string): number {
    const element = formatStruct("Dependencies", [
      `loads: ${loads}`,
      `calls: ${calls}`,
      `calls_for_texts: ${callsForTexts}`,
      `implemented_by: ${implementedBy}`,
      `guessed: ${guessed}`,
    ]);
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

function formatEntry(builtin: ModelBuiltin, context: EntryContext): string {
  const { loads, calls, callsForTexts, implementedBy, certain } = context.closure.dependenciesOf(builtin);
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
    `dependencies: ${context.dependencies.add(
      context.moduleLoadLists.add(formatModuleLoads(loads)),
      context.functionLists.add(formatFunctions(calls.intersection(certain))),
      context.callsForTextsLists.add(formatCallsForTexts(callsForTexts, context)),
      context.functionLists.add(formatFunctions(implementedBy.intersection(certain))),
      context.functionLists.add(formatFunctions(calls.union(implementedBy).difference(certain))),
    )}`,
  ]);
}

function emitEntries(model: TableModel): string {
  const context: EntryContext = {
    closure: new DependencyClosure(model),
    moduleOfNamespace: findModulesByNamespace(model.modules),
    moduleLoadLists: new StaticListPool("MODULE_LOAD_LIST", "ModuleLoad"),
    functionLists: new StaticListPool("FUNCTION_LIST", "RuntimeFunction"),
    nameLists: new StaticListPool("NAME_LIST", "BuiltinName"),
    callsForTextsLists: new StaticListPool("CALLS_FOR_TEXTS_LIST", "CallsForTexts"),
    dependencies: new DependencyPool(),
  };
  const entries = model.builtins.map(builtin => formatEntry(builtin, context));
  return [
    "static BUILTINS_IN_ORDER: [BuiltinId; BuiltinId::COUNT as usize] = BuiltinId::ALL;\n",
    context.moduleLoadLists.emit(),
    context.functionLists.emit(),
    context.nameLists.emit(),
    context.callsForTextsLists.emit(),
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
      if (to.place === "ResultField") add(namesOfResultField(result, to.index));
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
    `specifier: ${formatString(module.specifier)}`,
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

function formatHostRead({ context, exportName, use }: ModelHostRead): string {
  return formatStruct("HostRead", [
    `context: HostReadContext::${context}`,
    `export_name: ${formatBuiltinName(exportName.number)}`,
    `read_use: ${formatHostReadUse(use)}`,
  ]);
}

function emitEntriesLeadingToStartupLoads(model: TableModel): string {
  const closure = new DependencyClosure(model);
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
  return [
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
  const closure = new DependencyClosure(model);
  const { calls, implementedBy } = closure.dependenciesOfAnyOperand();
  return [
    emitSlice(
      "pub static ENTRIES_THAT_MAY_HAND_OUT_ANY_HOST_OBJECT: &[BuiltinId]",
      model.builtins.filter(builtin => closure.dependenciesOf(builtin).mayHandOutAnyHostObject).map(formatBuiltinId),
    ),
    emitSlice(
      "pub static FUNCTIONS_FOUND_ON_ANY_OPERAND: &[RuntimeFunction]",
      formatFunctions(calls.union(implementedBy)),
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
  const { calls, implementedBy } = new DependencyClosure(model).dependenciesOfAnyHostObject();
  return emitSlice(
    "pub static FUNCTIONS_FOR_ANY_HOST_OBJECT: &[RuntimeFunction]",
    formatFunctions(calls.union(implementedBy)),
  );
}

export function emitRustTable(model: TableModel): string {
  return [
    fileHeader,
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
    emitSlice("static HOST_READS: &[HostRead]", model.hostReads.map(formatHostRead)),
  ].join("");
}
