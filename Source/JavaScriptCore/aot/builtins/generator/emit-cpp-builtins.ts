import { collectNativeReads, type NativeReads } from "./check-host-uses.ts";
import { GeneratorError, Problems } from "./declarations.ts";
import { hashOfHostBindings } from "./emit-cpp-bindings.ts";
import { collectArrayDeclarations } from "./emit-cpp-ids.ts";
import { textNamesArray, textNameSlotsArray } from "./emit-cpp-names.ts";
import {
  checkDenseIds,
  emitArray,
  formatAggregate,
  formatEnumerator,
  formatId,
  formatName,
  formatOptionSet,
  formatUnlessDefault,
  formatUnlessEmpty,
  formatUnlessZero,
  hashOfText,
  joinBlocks,
  Pool,
  wrapFile,
  type AggregateMember,
} from "./emit-cpp-support.ts";
import {
  builtinFlags,
  noHostNumber,
  type ModelBuiltin,
  type ModelClass,
  type ModelClosedKeys,
  type ModelFunction,
  type ModelLayout,
  type ModelLayoutField,
  type ModelModule,
  type ModelRealmStructure,
  type ModelSlot,
  type TableModel,
} from "./model.ts";
import { builtinMarks, targetPlatforms, type TargetPlatform } from "./vocabulary.ts";

interface DeclaredArray {
  cppType: string;
  count: number;
}

type DeclaredArrays = ReadonlyMap<string, DeclaredArray>;
type SpanMember = readonly [member: string, array: string];
type Flag = readonly [flag: string, isSet: boolean];

const maxHostNumber = noHostNumber - 1;

const tableSpans: readonly SpanMember[] = [
  ["builtins", "builtinTableBuiltins"],
  ["overloads", "builtinTableOverloads"],
  ["dispatches", "builtinTableDispatches"],
  ["dispatchNodes", "builtinTableDispatchNodes"],
  ["types", "builtinTableTypes"],
  ["classes", "builtinTableClasses"],
  ["slots", "builtinTableSlots"],
  ["layouts", "builtinTableLayouts"],
];

const nameSpans: readonly SpanMember[] = [
  ["textNames", textNamesArray],
  ["symbolNames", "builtinTableSymbolNames"],
  ["registeredSymbolNames", "builtinTableRegisteredSymbolNames"],
  ["symbolConstants", "builtinTableSymbolConstants"],
  ["classesByMemberName", "builtinTableClassesByMemberName"],
  ["protocolKeys", "builtinTableProtocolKeys"],
];

const tokenSpans: readonly SpanMember[] = [
  ["builtinsWithQualifiedTokens", "builtinTableBuiltinsWithQualifiedTokens"],
  ["classes", "builtinTableClassTokens"],
  ["slots", "builtinTableSlotTokens"],
  ["layouts", "builtinTableLayoutTokens"],
  ["modules", "builtinTableModuleTokens"],
];

const attachmentSpans: readonly SpanMember[] = [
  ["featureUses", "builtinTableFeatureUses"],
  ["unseenCode", "builtinTableUnseenCode"],
  ["instancesThatAnswerEveryName", "builtinTableInstancesThatAnswerEveryName"],
  ["moduleLoads", "builtinTableModuleLoads"],
  ["startupLoads", "builtinTableStartupLoads"],
  ["calledFunctions", "builtinTableCalledFunctions"],
  ["startupCalledFunctions", "builtinTableStartupCalledFunctions"],
  ["hostObjects", "builtinTableHostObjects"],
  ["nativeCellsFilledByCalledFunctions", "builtinTableNativeCellsFilledByCalledFunctions"],
  ["forms", "builtinTableForms"],
  ["boxedCalls", "builtinTableBoxedCalls"],
  ["numberConstants", "builtinTableNumberConstants"],
  ["stringConstants", "builtinTableStringConstants"],
  ["overloadSummaries", "builtinTableOverloadSummaries"],
];

const runtimeSpans: readonly SpanMember[] = [
  ["modules", "builtinTableRuntimeModules"],
  ["functions", "builtinTableRuntimeFunctions"],
  ["startupModules", "builtinTableStartupModules"],
  ["functionsCalledWithStartupLoads", "builtinTableFunctionsCalledWithStartupLoads"],
  ["namesOfHostMethodsCallingStartupLoads", "builtinTableNamesOfHostMethodsCallingStartupLoads"],
  ["moduleLoaderFunctions", "builtinTableModuleLoaderFunctions"],
  ["lockedObjects", "builtinTableLockedObjects"],
  ["marks", "builtinTableMarks"],
  ["loadersOfMachineCode", "builtinTableLoadersOfMachineCode"],
  ["closedKeys", "builtinTableClosedKeys"],
];

function checkAscendingIds(cppType: string, items: readonly { id: number }[], problems: Problems): void {
  const position = items.findIndex((item, index) => index > 0 && item.id <= items[index - 1].id);
  if (position !== -1) problems.add(null, `${cppType}(${items[position].id}) does not ascend in its list`);
}

function hasConsecutiveIds(items: readonly { id: number }[]): boolean {
  return items.every((item, index) => item.id === items[0].id + index);
}

function checkBuiltin(builtin: ModelBuiltin, problems: Problems): void {
  if (!hasConsecutiveIds(builtin.members))
    problems.add(null, `${builtin.text}: the ids of its members are not consecutive`);
  if (!hasConsecutiveIds(builtin.overloads))
    problems.add(null, `${builtin.text}: the ids of its overloads are not consecutive`);
  if (builtin.hostNumber > maxHostNumber && builtin.hostNumber !== noHostNumber)
    problems.add(null, `${builtin.text}: hostNumber ${builtin.hostNumber} is above ${maxHostNumber}`);
}

function checkModel(model: TableModel): void {
  const problems = new Problems();
  checkDenseIds("BuiltinID", model.builtins, problems);
  checkDenseIds("BuiltinClassID", model.classes, problems);
  checkDenseIds("BuiltinSlotID", model.slots, problems);
  checkDenseIds("BuiltinLayoutID", model.layouts, problems);
  checkDenseIds("RealmStructureID", model.realmStructures, problems);
  checkDenseIds("RuntimeModuleID", model.modules, problems);
  checkAscendingIds("RuntimeFunctionID", model.functions, problems);
  for (const builtin of model.builtins) checkBuiltin(builtin, problems);
  problems.throwIfAny();
}

function formatUnlessFalse(value: boolean): string | null {
  return value ? "true" : null;
}

function formatHeap(heap: string | null): string | null {
  return heap === null ? null : formatEnumerator("AbstractHeap", heap);
}

function formatFlags(cppEnum: string, flags: readonly Flag[]): string | null {
  return formatOptionSet(
    cppEnum,
    flags.filter(([, isSet]) => isSet).map(([flag]) => flag),
  );
}

function formatPlatforms(platforms: readonly TargetPlatform[]): string | null {
  return formatOptionSet(
    "BuiltinPlatform",
    targetPlatforms.filter(platform => platforms.includes(platform)),
  );
}

function formatBuiltinIds(builtins: readonly ModelBuiltin[]): string[] {
  return builtins.map(builtin => formatId("BuiltinID", builtin));
}

function isLockedObject(builtin: ModelBuiltin): boolean {
  return builtin.flags.includes("IsLockedObject");
}

function formatBuiltin(builtin: ModelBuiltin): string {
  const { prototype, members, overloads } = builtin;
  const reads = overloads.filter(overload => overload.mode === "Get");
  const isReadWithoutType =
    builtin.kind === "DataProperty" && reads.length > 0 && reads.every(read => read.result.type.isAnything);
  return formatAggregate([
    ["kind", formatUnlessDefault("BuiltinKind", builtin.kind, "Namespace")],
    ["origin", formatUnlessDefault("BuiltinOrigin", builtin.origin, "Engine")],
    ["scope", formatUnlessDefault("BuiltinScope", builtin.scope, "Realm")],
    ["mutability", formatUnlessDefault("BuiltinMutability", builtin.mutability, "ChangesOnItsOwn")],
    ["prototypeLink", formatUnlessDefault("BuiltinPrototypeLink", prototype.link, "Unknown")],
    ["platforms", formatPlatforms(builtin.platforms)],
    [
      "flags",
      formatOptionSet(
        "BuiltinFlag",
        builtinFlags.filter(flag => builtin.flags.includes(flag)),
      ),
    ],
    [
      "marks",
      builtin.marks.length === 0
        ? null
        : builtinMarks
            .filter(mark => builtin.marks.includes(mark))
            .map(mark => `bitOfMark(${formatEnumerator("BuiltinMark", mark)})`)
            .join(" | "),
    ],
    ["holder", formatId("BuiltinID", builtin.holder)],
    ["name", formatName(builtin.name)],
    ["inheritsFrom", formatId("BuiltinID", prototype.link === "Builtin" ? prototype.builtin : null)],
    ["instanceClass", formatId("BuiltinClassID", builtin.instanceClass)],
    ["sameObjectAs", formatId("BuiltinID", builtin.sameObjectAs)],
    ["firstMember", formatId("BuiltinID", members.length === 0 ? null : members[0])],
    ["memberCount", formatUnlessZero(members.length)],
    ["firstOverload", formatId("BuiltinOverloadID", overloads.length === 0 ? null : overloads[0])],
    ["overloadCount", formatUnlessZero(overloads.length)],
    ["isNamedByText", builtin.hostNumber === noHostNumber ? null : "true"],
    ["isAddedAtFirstRead", builtin.isAddedAtFirstRead ? "true" : null],
    ["isAddedAsEnumerable", builtin.isAddedAsEnumerable ? "true" : null],
    ["canBeListed", builtin.canBeListed ? "true" : null],
    ["isReadWithoutType", isReadWithoutType ? "true" : null],
  ]);
}

interface ClassPools {
  slots: Pool;
  names: Pool;
}

function formatClass(builtinClass: ModelClass, pools: ClassPools): string {
  const { slots: classSlots, names: classNames } = pools;
  const names = builtinClass.namesDefinedOnNewInstances?.toSorted((left, right) => left.number - right.number);
  return formatAggregate([
    ["id", formatId("BuiltinClassID", builtinClass)],
    ["origin", formatUnlessDefault("BuiltinOrigin", builtinClass.origin, "Engine")],
    ["parent", formatId("BuiltinClassID", builtinClass.parent)],
    ["constructor", formatId("BuiltinID", builtinClass.constructor)],
    ["prototype", formatId("BuiltinID", builtinClass.prototype)],
    ["type", formatUnlessDefault("JSType", builtinClass.jsType, "CellType")],
    ["instanceTest", formatUnlessDefault("BuiltinInstanceTest", builtinClass.instanceTest, "None")],
    ["sharingMode", formatUnlessDefault("ArrayBufferSharingMode", builtinClass.sharingMode ?? "Default", "Default")],
    ["isPrimitiveWrapper", formatUnlessFalse(builtinClass.isPrimitiveWrapper)],
    ["isNeverPristine", formatUnlessFalse(builtinClass.isNeverPristine)],
    ["pristineTest", formatUnlessDefault("BuiltinPristineTest", builtinClass.pristineTest, "Mark")],
    ["hasClosureInstances", formatUnlessFalse(builtinClass.hasClosureInstances)],
    ["isFollowedBesideUnseenObject", formatUnlessFalse(builtinClass.isFollowedBesideUnseenObject)],
    ["instancesAreEntries", formatUnlessFalse(builtinClass.instancesAreEntries)],
    ["keepsStructuresOfClassesBelow", formatUnlessFalse(builtinClass.keepsStructuresOfClassesBelow)],
    ["definesCharactersOnNewInstances", formatUnlessFalse(builtinClass.definesCharactersOnNewInstances)],
    ["listsNamesDefinedOnNewInstances", formatUnlessFalse(names !== undefined)],
    ["namesDefinedOnNewInstances", names === undefined ? null : classNames.add(names.map(name => formatName(name)))],
    [
      "constructorOnlyReadsPrototypeOfNewTarget",
      formatUnlessFalse(builtinClass.constructorOnlyReadsPrototypeOfNewTarget),
    ],
    ["constructorAdder", formatName(builtinClass.constructorAdder)],
    ["slots", classSlots.add(builtinClass.slots.map(slot => formatId("BuiltinSlotID", slot)))],
    ["stateHeap", formatHeap(builtinClass.stateHeap)],
  ]);
}

function formatSlot(slot: ModelSlot): string {
  const { contentsSlot, indexInType } = slot;
  return formatAggregate([
    ["id", formatId("BuiltinSlotID", slot)],
    ["owner", formatId("BuiltinClassID", slot.owner)],
    ["name", formatName(slot.name)],
    ["indexInClass", formatUnlessZero(slot.indexInClass)],
    ["contentsSlot", contentsSlot === null ? null : formatEnumerator("ContentsSlot", contentsSlot)],
    ["indexInType", indexInType === null ? null : String(indexInType)],
    ["heap", formatHeap(slot.heap)],
    ["holds", formatId("BuiltinTypeID", slot.holds)],
    ["internalField", slot.internalField === null ? null : String(slot.internalField)],
    ["holdsStepValues", formatUnlessFalse(slot.holdsStepValues)],
    ["holdsWhatIsThrown", formatUnlessFalse(slot.holdsWhatIsThrown)],
    ["wordOfCell", formatUnlessDefault("BuiltinWordOfCell", slot.wordOfCell, "None")],
  ]);
}

function formatLayoutField(field: ModelLayoutField): string {
  return formatAggregate([
    ["name", formatName(field.name)],
    ["type", formatId("BuiltinTypeID", field.type)],
    ["isOptional", formatUnlessFalse(field.isOptional)],
  ]);
}

function formatLayout(layout: ModelLayout, layoutFields: Pool): string {
  return formatAggregate([
    ["id", formatId("BuiltinLayoutID", layout)],
    ["isTuple", formatUnlessFalse(layout.isTuple)],
    ["fields", layoutFields.add(layout.fields.map(formatLayoutField))],
  ]);
}

function formatRealmStructure({ getter, names }: ModelRealmStructure, realmStructureNames: Pool): string {
  return formatAggregate([
    ["findInitialStructure", `[](JSGlobalObject* realm) -> Structure* { return realm->${getter}(); }`],
    ["names", realmStructureNames.add(names.map(name => formatName(name)))],
  ]);
}

function formatModule(
  module: ModelModule,
  exportsHeldByNamespaces: Pool,
  nativeReads: NativeReads | undefined,
  exportsReadByNativeCode: Pool,
): string {
  return formatAggregate([
    ["id", formatId("RuntimeModuleID", module)],
    ["namespaceObject", formatId("BuiltinID", module.namespace)],
    [
      "flags",
      formatFlags("RuntimeModuleFlag", [
        ["HasSource", module.hasSource],
        ["HasOpenNamespace", module.hasOpenNamespace],
        ["ListsAllExports", module.listsAllExports],
        ["IsPublic", module.isPublic],
        ["NativeCodeReadsEveryExport", nativeReads?.readsEveryExport ?? false],
      ]),
    ],
    ["exportsHeldByNamespace", exportsHeldByNamespaces.add(module.exportsHeldByNamespace.map(name => formatName(name)))],
    [
      "exportsReadByNativeCode",
      exportsReadByNativeCode.add(
        [...(nativeReads?.names.values() ?? [])]
          .sort((left, right) => left.number - right.number)
          .map(name => formatName(name)),
      ),
    ],
    [
      "memberOnlyExports",
      exportsReadByNativeCode.add(
        [...(nativeReads?.names.values() ?? [])]
          .filter(name => !nativeReads!.readsEveryExport && !nativeReads!.namesWithOtherUses.has(name.number))
          .sort((left, right) => left.number - right.number)
          .map(name => formatName(name)),
      ),
    ],
  ]);
}

function formatFunction(runtimeFunction: ModelFunction, implementedBuiltins: Pool): string {
  return formatAggregate([
    ["id", formatId("RuntimeFunctionID", runtimeFunction)],
    ["role", formatUnlessDefault("RuntimeFunctionRole", runtimeFunction.role, "EngineBuiltin")],
    [
      "flags",
      formatFlags("RuntimeFunctionFlag", [
        ["IsHiddenInStackTraces", runtimeFunction.isHiddenInStackTraces],
        ["IsNeverInlined", runtimeFunction.isNeverInlined],
        ["UsesStubConvention", runtimeFunction.usesStubConvention],
      ]),
    ],
    ["platforms", formatPlatforms(runtimeFunction.platforms)],
    ["implements", implementedBuiltins.add(formatBuiltinIds(runtimeFunction.implements))],
  ]);
}

function formatClosedKeys({ holder, keys, openNames }: ModelClosedKeys, closedKeyNames: Pool): string {
  return formatAggregate([
    ["holder", formatId("BuiltinID", holder)],
    ["names", closedKeyNames.add(keys.map(key => formatName(key)))],
    ["openNames", closedKeyNames.add(openNames.map(name => formatName(name)))],
  ]);
}

function collectDeclaredArrays(model: TableModel): DeclaredArrays {
  const arrays = new Map<string, DeclaredArray>();
  for (const [cppType, name, count] of collectArrayDeclarations(model)) arrays.set(name, { cppType, count });
  return arrays;
}

function findDeclaredArray(arrays: DeclaredArrays, name: string): DeclaredArray {
  const found = arrays.get(name);
  if (found === undefined) throw new GeneratorError(`AOTBuiltinTableArrays.h does not declare ${name}`);
  return found;
}

function checkArrayReferences(arrays: DeclaredArrays): void {
  const spans = [...tableSpans, ...nameSpans, ...tokenSpans, ...attachmentSpans, ...runtimeSpans];
  const referenced = new Set(spans.map(([, array]) => array));
  const missing = [...arrays.keys()].filter(name => !referenced.has(name));
  if (missing.length > 0) throw new GeneratorError(`builtinTableData does not reference ${missing.join(", ")}`);
}

function emitTableArray(arrays: DeclaredArrays, name: string, elements: readonly string[]): string {
  const { cppType, count } = findDeclaredArray(arrays, name);
  if (elements.length !== count)
    throw new GeneratorError(`${name} has ${elements.length} elements, and AOTBuiltinTableArrays.h declares ${count}`);
  return emitArray(`constinit const ${cppType} ${name}`, elements);
}

function formatSpanMembers(arrays: DeclaredArrays, spans: readonly SpanMember[]): AggregateMember[] {
  return spans.map(([member, array]): AggregateMember => [
    member,
    findDeclaredArray(arrays, array).count === 0 ? null : array,
  ]);
}

function emitTableData(arrays: DeclaredArrays, lockedMembers: string | null, realmStructures: string | null): string {
  const textNameSlots = findDeclaredArray(arrays, textNamesArray).count === 0 ? null : textNameSlotsArray;
  const data = formatAggregate([
    ...formatSpanMembers(arrays, tableSpans),
    ["names", formatUnlessEmpty([...formatSpanMembers(arrays, nameSpans), ["textNameSlots", textNameSlots]])],
    ["tokens", formatUnlessEmpty(formatSpanMembers(arrays, tokenSpans))],
    ["attachments", formatUnlessEmpty(formatSpanMembers(arrays, attachmentSpans))],
    [
      "runtime",
      formatUnlessEmpty([
        ...formatSpanMembers(arrays, runtimeSpans),
        ["lockedMembers", lockedMembers],
        ["realmStructures", realmStructures],
      ]),
    ],
  ]);
  return `constinit const BuiltinTableData builtinTableData = ${data};`;
}

function emitDescription(model: TableModel, textsOfTable: readonly string[]): string {
  const description = formatAggregate([
    ["hashOfBuiltinTokens", "builtinTableHashOfBuiltinTokens"],
    ["hashOfTable", `0x${hashOfText(textsOfTable.join("\0")).toString(16)}`],
    ["hashOfHostBindings", `0x${hashOfHostBindings(model).toString(16)}`],
  ]);
  return `extern "C" AOT_KEPT_BY_EVERY_LINKER constinit const BuiltinTableDescription describedBuiltinTable = ${description};`;
}

export function emitCppBuiltins(model: TableModel, otherFilesOfTable: readonly string[]): string {
  checkModel(model);
  const arrays = collectDeclaredArrays(model);
  checkArrayReferences(arrays);
  const classSlots = new Pool("BuiltinSlotID", "classSlots");
  const classNames = new Pool("BuiltinName", "classNames");
  const classPools: ClassPools = {
    slots: classSlots,
    names: classNames,
  };
  const layoutFields = new Pool("BuiltinLayoutField", "layoutFields");
  const implementedBuiltins = new Pool("BuiltinID", "implementedBuiltins");
  const exportsHeldByNamespaces = new Pool("BuiltinName", "exportsHeldByNamespaces");
  const exportsReadByNativeCode = new Pool("BuiltinName", "exportsReadByNativeCode");
  const nativeReads = collectNativeReads(model);
  const lockedMembers = new Pool("BuiltinID", "lockedMembers");
  const lockedMemberSpan = lockedMembers.add(formatBuiltinIds(model.membersLockedByName));
  const closedKeyNames = new Pool("BuiltinName", "closedKeyNames");
  const realmStructureNames = new Pool("BuiltinName", "realmStructureNames");
  const realmStructures = new Pool("BuiltinRealmStructure", "realmStructures");
  const realmStructureSpan = realmStructures.add(
    model.realmStructures.map(structure => formatRealmStructure(structure, realmStructureNames)),
  );
  const { startup } = model;
  const tableArrays = [
    emitTableArray(arrays, "builtinTableBuiltins", model.builtins.map(formatBuiltin)),
    emitTableArray(
      arrays,
      "builtinTableClasses",
      model.classes.map(builtinClass => formatClass(builtinClass, classPools)),
    ),
    emitTableArray(arrays, "builtinTableSlots", model.slots.map(formatSlot)),
    emitTableArray(
      arrays,
      "builtinTableLayouts",
      model.layouts.map(layout => formatLayout(layout, layoutFields)),
    ),
    emitTableArray(
      arrays,
      "builtinTableRuntimeModules",
      model.modules.map(module =>
        formatModule(module, exportsHeldByNamespaces, nativeReads.get(module), exportsReadByNativeCode),
      ),
    ),
    emitTableArray(
      arrays,
      "builtinTableRuntimeFunctions",
      model.functions.map(runtimeFunction => formatFunction(runtimeFunction, implementedBuiltins)),
    ),
    emitTableArray(
      arrays,
      "builtinTableStartupModules",
      startup.modules.map(module => formatId("RuntimeModuleID", module)),
    ),
    emitTableArray(
      arrays,
      "builtinTableFunctionsCalledWithStartupLoads",
      startup.functionsCalledWithLoads.map(called => formatId("RuntimeFunctionID", called.function)),
    ),
    emitTableArray(
      arrays,
      "builtinTableNamesOfHostMethodsCallingStartupLoads",
      startup.namesOfMethodsCallingLoads.map(name => formatName(name)),
    ),
    emitTableArray(
      arrays,
      "builtinTableModuleLoaderFunctions",
      startup.moduleLoaderFunctions.map(runtimeFunction => formatId("RuntimeFunctionID", runtimeFunction)),
    ),
    emitTableArray(arrays, "builtinTableLockedObjects", formatBuiltinIds(model.builtins.filter(isLockedObject))),
    emitTableArray(
      arrays,
      "builtinTableMarks",
      builtinMarks.flatMap(mark =>
        model.builtins
          .filter(({ marks }) => marks.includes(mark))
          .map(builtin =>
            formatAggregate([
              ["mark", formatEnumerator("BuiltinMark", mark)],
              ["builtin", formatId("BuiltinID", builtin)],
            ]),
          ),
      ),
    ),
    emitTableArray(
      arrays,
      "builtinTableLoadersOfMachineCode",
      model.builtins.flatMap(builtin =>
        builtin.loadsMachineCodeAs === null
          ? []
          : [
              formatAggregate([
                ["callee", formatId("BuiltinID", builtin)],
                ["named", formatId("BuiltinID", builtin.loadsMachineCodeAs)],
              ]),
            ],
      ),
    ),
    emitTableArray(
      arrays,
      "builtinTableClosedKeys",
      model.closedKeys.map(closedKeys => formatClosedKeys(closedKeys, closedKeyNames)),
    ),
  ];
  const blocks = [
    classSlots.emit(),
    classNames.emit(),
    layoutFields.emit(),
    implementedBuiltins.emit(),
    exportsHeldByNamespaces.emit(),
    exportsReadByNativeCode.emit(),
    lockedMembers.emit(),
    closedKeyNames.emit(),
    realmStructureNames.emit(),
    realmStructures.emit(),
    `static_assert(${model.realmStructures.length} <= RealmStructureID::hashTableDeletedIndex);`,
    ...tableArrays,
    emitTableData(arrays, lockedMemberSpan, realmStructureSpan),
  ];
  const body = joinBlocks([...blocks, emitDescription(model, [...otherFilesOfTable, ...blocks])]);
  return wrapFile(["AOTBuiltinTable.h", "AOTBuiltinTableArrays.h", "AOTRuntimeDescription.h", "JSCInlines.h"], body);
}
