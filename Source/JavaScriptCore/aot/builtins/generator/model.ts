import type { HostReadUse, Proof, RuntimeFunctionRole, Source } from "./declarations.ts";
import type {
  BoxedStubName,
  BuiltinKind,
  BuiltinMark,
  CallbackAftermath,
  CallbackResultUse,
  CallbackTimes,
  CalleeKind,
  CallMode,
  Capability,
  ContentsSlot,
  Conversion,
  CppClassRef,
  DirectEntrySpec,
  DirectiveKind,
  ErrorKind,
  FeatureName,
  Freshness,
  Heap,
  HostReadContext,
  InlineBuiltinName,
  IntegrityLevel,
  IntrinsicPosition,
  IterationKind,
  KeyedProtocol,
  LinkedFunctionExpansion,
  Mutability,
  NativeCallKind,
  OperandRole,
  OperandUse,
  Origin,
  ParameterMode,
  Primitive,
  PristineAspect,
  PristineTest,
  PromiseStatus,
  Protocol,
  RangeRule,
  Scope,
  SharingMode,
  TargetPlatform,
  TypedResultForm,
  WordOfCell,
} from "./vocabulary.ts";

export const builtinFlags = [
  "ListsAllMembers",
  "IsGlobalObject",
  "IsLockedObject",
  "RequiresHolderAsThis",
  "IsDeterministic",
  "HasPrivateAlias",
  "IsOwnPropertyOfInstances",
  "IsLazilyCreated",
  "StartsInstance",
  "LoadsModuleByName",
  "EvaluatesText",
  "RunsUnseenCode",
  "IsCompilerHelper",
  "IsDefaultExport",
  "HasLockedMembers",
  "IsClosedToOtherNames",
] as const;
export type BuiltinFlag = (typeof builtinFlags)[number];

export const overloadFlags = [
  "IsUndeclared",
  "IsProvenByConstruction",
  "Throws",
  "RunsOnlyOperandCode",
  "ReadsStateBeforeConversions",
  "IsSpecialCase",
  "OnlyPropagatesErrors",
  "ResultIsDeclaredOnly",
  "ResultIsHeld",
] as const;
export type OverloadFlag = (typeof overloadFlags)[number];

export const instanceTests = ["None", "JSTypeEquals", "ClassInfoInherits", "SharingModeEquals"] as const;
export type InstanceTest = (typeof instanceTests)[number];

export const targetArchitectures = ["ARM64", "X86_64"] as const;
export type TargetArchitecture = (typeof targetArchitectures)[number];

export const propertyActionKinds = [
  "ReadsByKey",
  "StoresByKey",
  "DeletesByKey",
  "AssignsFrom",
  "DefinesName",
  "TakesDescriptor",
  "TakesDescriptorMap",
  "ChangesIntegrity",
  "ChangesPrototype",
  "BecomesPrototype",
  "DefinesGetter",
  "DefinesSetter",
  "ReadsGetter",
  "ReadsSetter",
  "TestsByKey",
  "DescribesOwnProperties",
  "DescribesResult",
  "DefinesFrom",
] as const;
export type PropertyActionKind = (typeof propertyActionKinds)[number];

export const typePartKinds = [
  "Instance",
  "InstanceOfReceiverClass",
  "TypedArrayOfReceiverElementType",
  "Layout",
  "Builtin",
  "Operand",
  "Slot",
  "IteratedValues",
  "CallbackResult",
  "Prototype",
] as const;

export const placeKinds = [
  "None",
  "Operand",
  "Slot",
  "IteratedValues",
  "OwnValues",
  "Result",
  "ResultSlot",
  "ResultField",
  "NewValue",
  "NativeMemory",
  "Prototype",
  "Member",
  "NativeCell",
] as const;

export const keyKinds = ["None", "Any", "Indexes", "Names", "Operand", "ElementsOf"] as const;

export const implementationKinds = [
  "Generic",
  "HostCall",
  "Compiled",
  "Inline",
  "Number",
  "Cell",
  "CachedGetter",
  "OwnSlot",
] as const;
export type ImplementationKind = (typeof implementationKinds)[number];

export const prototypeLinks = ["Unknown", "Null", "Builtin"] as const;
export type PrototypeLink = (typeof prototypeLinks)[number];

export const hostReadUses = ["Inspected", "Called", "PassedTo", "Kept", "IsValueOf"] as const;

export const noHostNumber = 0xffff;

export const nameKinds = ["Text", "WellKnownSymbol", "RegisteredSymbol"] as const;
export type NameKind = (typeof nameKinds)[number];

export interface ModelName {
  number: number;
  text: string;
  kind: NameKind;
  cppConstant: string | null;
}

export type ModelOperand =
  { operand: "Receiver" } | { operand: "Argument"; index: number } | { operand: "ArgumentsFrom"; index: number };

export interface ModelBound {
  value: number;
  cppExpression: string | null;
}

export interface ModelSlotContents {
  slot: ModelSlot;
  type: ModelType;
}

export type ModelTypePart =
  | {
      part: "Instance";
      class: ModelClass;
      pristine: readonly PristineAspect[];
      isOfClassItself: boolean;
      contents: readonly ModelSlotContents[];
    }
  | { part: "InstanceOfReceiverClass" }
  | { part: "TypedArrayOfReceiverElementType" }
  | { part: "Layout"; layout: ModelLayout }
  | { part: "Builtin"; builtin: ModelBuiltin }
  | { part: "Operand"; operand: ModelOperand }
  | { part: "Slot"; operand: ModelOperand; slot: ModelSlot }
  | { part: "IteratedValues"; operand: ModelOperand; isSettled: boolean }
  | { part: "CallbackResult"; operand: ModelOperand }
  | { part: "Prototype"; operand: ModelOperand };

export interface ModelType {
  id: number;
  key: string;
  isAnything: boolean;
  isMadeOfListedClasses: boolean;
  hasAnyObject: boolean;
  hasAnyFunction: boolean;
  hasPlainData: boolean;
  primitives: readonly Primitive[];
  integers: { minimum: ModelBound; maximum: ModelBound } | null;
  stringLiterals: readonly ModelName[];
  freshness: Freshness;
  parts: readonly ModelTypePart[];
}

export type ModelPlace =
  | { place: "None" }
  | { place: "Operand"; operand: ModelOperand }
  | { place: "Slot"; operand: ModelOperand; slot: ModelSlot }
  | { place: "IteratedValues"; operand: ModelOperand; isSettled: boolean }
  | { place: "OwnValues"; operand: ModelOperand }
  | { place: "Result" }
  | { place: "ResultSlot"; slot: ModelSlot }
  | { place: "ResultField"; index: number; holdsOnlyWhatIsStored: boolean }
  | { place: "NewValue"; type: ModelType; holdsWhatIsThrown: boolean }
  | { place: "NativeMemory" }
  | { place: "Prototype"; operand: ModelOperand }
  | { place: "Member"; operand: ModelOperand; path: readonly (ModelName | null)[]; firstStep: number }
  | { place: "NativeCell"; cell: ModelName; holdsOnlyWhatIsStored: boolean };

export type ModelKey =
  | { key: "None" }
  | { key: "Operand"; operand: ModelOperand }
  | { key: "ElementsOf"; operand: ModelOperand }
  | { key: "Names"; names: readonly ModelName[] }
  | { key: "Indexes" }
  | { key: "Any" };

export interface ModelTransfer {
  from: ModelPlace;
  to: ModelPlace;
}

export interface ModelCallback {
  callee: ModelOperand;
  times: CallbackTimes;
  afterwards: CallbackAftermath;
  constructs: boolean;
  newTarget: ModelPlace;
  thisValue: ModelPlace;
  arguments: readonly ModelPlace[];
  spreadArguments: ModelPlace;
  resultUse: CallbackResultUse;
  resultTo: ModelPlace;
}

export interface ModelHandlerCall {
  callee: ModelPlace;
  times: CallbackTimes;
  thisValue: ModelPlace;
  arguments: readonly ModelPlace[];
  resultUse: CallbackResultUse;
}

export interface ModelPropertyAction {
  action: PropertyActionKind;
  object: ModelOperand;
  key: ModelKey;
  value: ModelPlace;
  integrityLevel: IntegrityLevel | null;
}

export interface ModelParameter {
  name: string;
  operand: ModelOperand;
  mode: ParameterMode;
  accepted: ModelType;
  conversion: Conversion;
  converted: ModelType;
  refused: ModelType;
  admitted: ModelType | null;
  uses: readonly OperandUse[];
  roles: readonly OperandRole[];
  protocols: readonly Protocol[];
  namedMembers: readonly ModelName[];
  calledMembers: readonly ModelName[];
  convertedMembers: readonly ModelName[];
}

export interface ModelResultLayout {
  layout: ModelLayout;
  isOrdinaryObject: boolean;
}

export interface ModelPlatformType {
  platform: TargetPlatform;
  type: ModelType;
}

export interface ModelResult {
  type: ModelType;
  typesByPlatform: readonly ModelPlatformType[];
  layouts: readonly ModelResultLayout[];
  rangeRule: RangeRule;
  rangeOperand: ModelOperand | null;
  substringOf: ModelOperand | null;
  rangeOf: ModelOperand | null;
  typedResultForm: TypedResultForm;
  typedResultSource: ModelOperand | null;
  iteratedOperand: ModelOperand | null;
  iterationKind: IterationKind | null;
  prototypeSource: ModelOperand | null;
  testedOperand: ModelOperand | null;
  testedType: ModelType | null;
  properties: readonly ModelResultProperty[];
}

export interface ModelEffects {
  capability: Capability;
  reads: readonly Heap[];
  writes: readonly Heap[];
}

export interface ModelFlow {
  transfers: readonly ModelTransfer[];
  callbacks: readonly ModelCallback[];
  propertyActions: readonly ModelPropertyAction[];
  handlerCalls: readonly ModelHandlerCall[];
}

export interface ModelRealmStructure {
  id: number;
  getter: string;
  names: readonly ModelName[];
}

export type ModelImplementation =
  | { how: "Generic" }
  | { how: "HostCall"; callee: CalleeKind; function: ModelFunction | null; customGetter: string | null }
  | { how: "Compiled"; callee: CalleeKind; function: ModelFunction }
  | {
      how: "Inline";
      builtin: InlineBuiltinName;
      parameters: readonly string[];
      maximumArgumentCount: number | null;
      otherwise: Extract<ModelImplementation, { how: "Generic" | "HostCall" | "Compiled" }> | null;
    }
  | { how: "Number" }
  | { how: "Cell" }
  | { how: "CachedGetter"; cppClass: string; member: string }
  | { how: "OwnSlot"; structure: ModelRealmStructure; offset: number };

export interface ModelResultProperty {
  name: ModelName;
  type: ModelType;
}

export interface ModelCode {
  implementation: ModelImplementation;
  directEntry: DirectEntrySpec | null;
  model: ModelFunction | null;
}

export interface ModelOverload {
  id: number;
  indexInBuiltin: number;
  builtin: ModelBuiltin;
  mode: CallMode;
  source: Source;
  proof: Proof;
  flags: readonly OverloadFlag[];
  receiver: ModelParameter | null;
  parameters: readonly ModelParameter[];
  result: ModelResult;
  effects: ModelEffects;
  flow: ModelFlow;
  code: ModelCode;
}

export type ModelDispatchTarget = { node: ModelDispatchNode } | { overload: ModelOverload };

export interface ModelDispatchBranch {
  tested: ModelType;
  target: ModelDispatchTarget;
}

export interface ModelDispatchNode {
  id: number;
  operand: ModelOperand;
  branches: readonly ModelDispatchBranch[];
}

export interface ModelDispatch {
  builtin: ModelBuiltin;
  mode: CallMode;
  root: ModelDispatchTarget;
}

export interface ModelCondition {
  operand: ModelOperand | null;
  isOneOf: readonly ModelName[];
  startsWith: readonly ModelName[];
  calledOn: ModelBuiltin | null;
  hasProperty: ModelName | null;
  buildAdmitsRuntimeCode: boolean;
  callRunsOtherCode: boolean;
  objectNobodyKnowsIsWritten: boolean;
  testsReplaceBuiltins: boolean;
  programStores: ModelBuiltin | null;
  isAskedForOtherNames: ModelBuiltin | null;
  valuesServed: readonly ModelName[];
  methodsServed: readonly ModelName[];
}

export interface ModelFeatureUse {
  feature: FeatureName;
  overload: ModelOverload | null;
  isGuard: boolean;
  condition: ModelCondition | null;
}

export type ModelHostValue = Extract<ModelPlace, { place: "None" | "NewValue" | "NativeCell" | "NativeMemory" }>;

export interface ModelHostCall {
  times: CallbackTimes;
  thisValue: ModelHostValue;
  arguments: readonly ModelHostValue[] | null;
  otherArguments: ModelHostValue;
  result: ModelHostUses;
}

export type ModelHostUse = { path: readonly ModelName[] } & (
  | { read: "Inspected" }
  | { read: "Called"; call: ModelHostCall | null }
  | { read: "PassedTo"; builtin: ModelBuiltin; argument: number }
  | { read: "Kept"; cell: ModelName }
  | { read: "IsValueOf"; builtin: ModelBuiltin }
);

export type ModelHostUses = readonly ModelHostUse[] | null;

export interface ModelExportRead {
  name: ModelName;
  uses: ModelHostUses;
}

export interface ModelModuleLoad {
  module: ModelModule;
  exports: readonly ModelExportRead[] | null;
  condition: ModelCondition | null;
}

export interface ModelCalledFunction {
  function: ModelFunction;
  condition: ModelCondition | null;
  uses: ModelHostUses;
}

export interface ModelHostObjectField {
  name: ModelName | null;
  value: ModelHostValue;
}

export interface ModelHostObjectInternalField {
  index: number;
  uses: readonly ModelHostUse[];
}

export interface ModelHostObject {
  cell: ModelName;
  function: ModelBuiltin | null;
  prototype: ModelBuiltin | null;
  fields: readonly ModelHostObjectField[];
  internalFields: readonly ModelHostObjectInternalField[];
  keepsStringsOfStoredValues: boolean;
  nativeAccessors: readonly ModelName[];
  lockedNames: readonly ModelName[];
  holder: ModelBuiltin | null;
  platforms: readonly TargetPlatform[];
  source: Source;
}

export interface ModelNativeFunction {
  symbol: string;
  length: number;
  header: string | null;
  name: string | null;
}

export interface ModelAttachments {
  featureUses: readonly ModelFeatureUse[];
  unseenCodeCondition: ModelCondition | null;
  conditionOnInstancesThatAnswerEveryName: ModelCondition | null;
  moduleLoads: readonly ModelModuleLoad[];
  calledFunctions: readonly ModelCalledFunction[];
  literalPartsForm: { argument: ModelOperand; names: readonly ModelName[]; builtin: ModelBuiltin } | null;
  boxedCallStub: BoxedStubName | null;
  nativeFunction: ModelNativeFunction | null;
  nativeSymbols: readonly string[];
  creationSymbol: string | null;
  linkTimeConstant: string | null;
}

export type ModelPrototype = { link: "Unknown" } | { link: "Null" } | { link: "Builtin"; builtin: ModelBuiltin };

export interface ModelTargetText {
  platforms: readonly TargetPlatform[];
  architectures: readonly TargetArchitecture[];
  text: ModelName;
}

export type ModelConstant =
  | { constant: "Number"; value: number; cppExpression: string | null }
  | { constant: "String"; texts: readonly ModelTargetText[] }
  | { constant: "Symbol"; name: ModelName }
  | { constant: "Undefined" };

export interface ModelBuiltin {
  id: number;
  token: string;
  path: readonly string[];
  text: string;
  kind: BuiltinKind;
  origin: Origin;
  scope: Scope;
  mutability: Mutability;
  platforms: readonly TargetPlatform[];
  flags: readonly BuiltinFlag[];
  marks: readonly BuiltinMark[];
  loadsMachineCodeAs: ModelBuiltin | null;
  holder: ModelBuiltin | null;
  name: ModelName;
  prototype: ModelPrototype;
  instanceClass: ModelClass | null;
  sameObjectAs: ModelBuiltin | null;
  members: readonly ModelBuiltin[];
  overloads: readonly ModelOverload[];
  constant: ModelConstant | null;
  isAddedAtFirstRead: boolean;
  isAddedAsEnumerable: boolean;
  canBeListed: boolean;
  isCreatedInSomeRuns: boolean;
  namesFoundAbove: readonly ModelName[];
  hostNumber: number;
  attachments: ModelAttachments;
}

export interface ModelSlot {
  id: number;
  owner: ModelClass;
  name: ModelName;
  indexInClass: number;
  heap: Heap;
  contentsSlot: ContentsSlot | null;
  indexInType: number | null;
  holds: ModelType | null;
  internalField: number | null;
  holdsStepValues: boolean;
  holdsWhatIsThrown: boolean;
  wordOfCell: WordOfCell;
}

export interface ModelTypeName {
  path: readonly string[];
  isView: boolean;
  typeArguments: readonly ModelSlot[];
}

export interface ModelClass {
  id: number;
  token: string;
  origin: Origin;
  parent: ModelClass | null;
  constructor: ModelBuiltin | null;
  prototype: ModelBuiltin | null;
  jsType: string;
  instanceTest: InstanceTest;
  sharingMode: SharingMode | null;
  instanceCppClass: CppClassRef | null;
  prototypeCppExpression: string | null;
  instanceStructureCppExpression: string | null;
  isPrimitiveWrapper: boolean;
  isNeverPristine: boolean;
  pristineTest: PristineTest;
  hasClosureInstances: boolean;
  isFollowedBesideUnseenObject: boolean;
  instancesAreEntries: boolean;
  keepsStructuresOfClassesBelow: boolean;
  definesCharactersOnNewInstances: boolean;
  namesDefinedOnNewInstances: readonly ModelName[] | null;
  constructorOnlyReadsPrototypeOfNewTarget: boolean;
  constructorAdder: ModelName | null;
  slots: readonly ModelSlot[];
  stateHeap: Heap | null;
  types: readonly ModelTypeName[];
}

export interface ModelLayoutField {
  name: ModelName | null;
  isOptional: boolean;
  type: ModelType;
}

export interface ModelLayout {
  id: number;
  token: string;
  origin: Origin;
  isTuple: boolean;
  fields: readonly ModelLayoutField[];
}

export interface ModelModule {
  id: number;
  token: string;
  specifier: string;
  hasSource: boolean;
  isEsModule: boolean;
  hasOpenNamespace: boolean;
  listsAllExports: boolean;
  isPublic: boolean;
  namespace: ModelBuiltin;
  exportsHoldingModuleObject: readonly ModelName[];
  exportsHeldByNamespace: readonly ModelName[];
}

export type ModelFunctionRole = RuntimeFunctionRole | "Model";

export interface ModelFunction {
  id: number;
  token: string;
  role: ModelFunctionRole;
  file: string;
  name: string;
  exportName: string;
  cppCodeName: string | null;
  isHiddenInStackTraces: boolean;
  isNeverInlined: boolean;
  usesStubConvention: boolean;
  platforms: readonly TargetPlatform[];
  implements: readonly ModelBuiltin[];
}

export type ModelIntrinsicMeaning =
  | { intrinsic: "Operation"; opcode: string }
  | { intrinsic: "Conversion"; conversion: Exclude<Conversion, "None" | "Dictionary"> }
  | { intrinsic: "Throw"; error: ErrorKind }
  | { intrinsic: "IfAbruptCloseIterator" }
  | { intrinsic: "SymbolKey"; symbol: ModelName }
  | { intrinsic: "HasJSType"; first: string; last: string }
  | { intrinsic: "Builtin" | "Construct" | "IsHolderWithoutAddedNames" | "NativeBinding"; builtin: ModelBuiltin }
  | { intrinsic: "LinkTimeConstant"; constant: string }
  | { intrinsic: "Int"; value: number }
  | { intrinsic: "Bool"; value: boolean }
  | { intrinsic: "Double"; value: number }
  | { intrinsic: "Call"; isNeverInlined: boolean }
  | { intrinsic: "PutByValDirect" | "GetOwnProperty"; privateKey: boolean }
  | { intrinsic: "Directive"; directive: DirectiveKind }
  | { intrinsic: "PrependArgument"; callee: ModelName; argument: number }
  | { intrinsic: "HasPromiseStatus"; status: PromiseStatus }
  | { intrinsic: "NativeCall"; kind: NativeCallKind }
  | { intrinsic: "LinkedFunction"; function: ModelFunction; expansion?: LinkedFunctionExpansion }
  | {
      intrinsic:
        | "IsNullish"
        | "IsObject"
        | "IsCallable"
        | "Undefined"
        | "Apply"
        | "ArrayPush"
        | "ArgumentCount"
        | "Argument"
        | "PrivateGlobal"
        | "ReplaceablePrivateGlobal"
        | "PrivateName"
        | "ModulePrivateName"
        | "Erased"
        | "BundleError";
    };

export interface ModelIntrinsic {
  name: ModelName;
  position: IntrinsicPosition;
  meaning: ModelIntrinsicMeaning;
}

export interface ModelPrefixedGlobal {
  name: ModelName;
  meaning: ModelIntrinsicMeaning;
}

export interface ModelNativeBinding {
  kind: NativeCallKind;
  file: ModelName;
  symbol: ModelName;
  builtin: ModelBuiltin | null;
}

export interface ModelHostRead {
  context: HostReadContext;
  exportName: ModelName;
  use: Exclude<HostReadUse, { read: "PassedTo" }> | { read: "PassedTo"; builtin: ModelBuiltin; argument: number };
  namesLookedFor: readonly ModelName[];
}

export interface ModelProtocolKeys {
  protocol: KeyedProtocol;
  keys: readonly ModelName[];
}

export interface ModelClosedKeys {
  holder: ModelBuiltin;
  keys: readonly ModelName[];
  openNames: readonly ModelName[];
}

export interface ModelStartup {
  modules: readonly ModelModule[];
  loads: readonly ModelModuleLoad[];
  functionsCalledWithLoads: readonly ModelCalledFunction[];
  namesOfMethodsCallingLoads: readonly ModelName[];
  functionsLayingSuchMethods: readonly ModelFunction[];
  moduleLoaderFunctions: readonly ModelFunction[];
}

export interface ModelSyntax {
  intrinsics: readonly ModelIntrinsic[];
  prefixedGlobals: readonly ModelPrefixedGlobal[];
  nativeBindings: readonly ModelNativeBinding[];
  checkedJSTypes: readonly string[];
  linkTimeConstants: readonly string[];
}

export interface ModelEngineFile {
  name: string;
  text: string;
}

export interface TableFeature {
  name: FeatureName;
  index: number;
  isEngine: boolean;
}

export interface TableModel {
  names: readonly ModelName[];
  builtins: readonly ModelBuiltin[];
  membersLockedByName: readonly ModelBuiltin[];
  closedKeys: readonly ModelClosedKeys[];
  overloads: readonly ModelOverload[];
  dispatches: readonly ModelDispatch[];
  dispatchNodes: readonly ModelDispatchNode[];
  types: readonly ModelType[];
  classes: readonly ModelClass[];
  slots: readonly ModelSlot[];
  layouts: readonly ModelLayout[];
  realmStructures: readonly ModelRealmStructure[];
  modules: readonly ModelModule[];
  functions: readonly ModelFunction[];
  features: readonly FeatureName[];
  boxedStubs: readonly BoxedStubName[];
  protocolKeys: readonly ModelProtocolKeys[];
  hostReads: readonly ModelHostRead[];
  namesOfEngine: ReadonlySet<ModelName>;
  hostRoles: readonly { token: string; builtin: ModelBuiltin }[];
  hostClassRoles: readonly { token: string; builtinClass: ModelClass }[];
  hostObjects: readonly ModelHostObject[];
  checkedNumbers: readonly ModelBound[];
  startup: ModelStartup;
  syntax: ModelSyntax;
  engineFiles: readonly ModelEngineFile[];
}
