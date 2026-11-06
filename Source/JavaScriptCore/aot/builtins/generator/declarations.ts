import type { TargetArchitecture } from "./model.ts";
import type {
  BuiltinKind,
  BuiltinPath,
  CalledFunctionSpec,
  CallMode,
  Capability,
  ClassColumns,
  ClassName,
  ConstructedOperand,
  EntryColumns,
  FeatureName,
  FunctionRef,
  HostCallSpec,
  HostObjectFieldSpec,
  HostObjectInternalFieldSpec,
  HostReadContext,
  HostUseSpec,
  HostValueSpec,
  IntrinsicPosition,
  IntrinsicSpec,
  LayoutName,
  ModuleLoadSpec,
  NativeCallerSpec,
  NativeCallKind,
  NativeMentionSpec,
  OperandName,
  Origin,
  ParameterType,
  Scope,
  SignatureColumns,
  StaticTableRef,
  TargetPlatform,
  TypeSpec,
} from "./vocabulary.ts";

export const proofs = ["Structure", "ByConstruction", "HandWritten"] as const;
export type Proof = (typeof proofs)[number];

export interface Source {
  file: string;
  line: number | null;
  origin: Origin;
  proof: Proof;
}

export interface ParameterDeclaration {
  name: string;
  type: ParameterType;
}

export interface ResultPropertyDeclaration {
  name: string;
  type: TypeSpec;
}

export interface PlatformResultDeclaration {
  platform: TargetPlatform;
  result: TypeSpec;
}

export interface SignatureDeclaration extends SignatureColumns {
  mode: CallMode;
  receiver: ParameterType | null;
  parameters: readonly ParameterDeclaration[];
  result: TypeSpec;
  resultsByPlatform?: readonly PlatformResultDeclaration[];
  resultProperties?: readonly ResultPropertyDeclaration[];
  answersWhetherInstance?: { operand: OperandName; type: TypeSpec };
  capability: Capability;
  throws: boolean;
  onlyPropagatesErrors?: boolean;
  isUndeclared: boolean;
  constructs?: ConstructedOperand;
}

export type DispatchDeclaration =
  { signature: number } | { argument: number; branches: readonly { tested: TypeSpec; then: DispatchDeclaration }[] };

export interface ModeDispatchDeclaration {
  mode: CallMode;
  root: DispatchDeclaration;
}

export interface TargetTextSpec {
  text: string;
  platforms: readonly TargetPlatform[];
  architectures: readonly TargetArchitecture[];
}

export type ConstantSpec =
  | { constant: "Number"; value: number; cppExpression: string | null }
  | { constant: "String"; texts: readonly TargetTextSpec[] }
  | { constant: "Symbol" }
  | { constant: "Undefined" };

export interface NamespaceDeclaration {
  declares: "Namespace";
  path: BuiltinPath;
  scope: Scope;
  isLocked: boolean;
  locksMembers: boolean;
  isGlobalObject: boolean;
  listsAllMembers: boolean;
  columns: EntryColumns;
}

export interface StaticTableDeclaration {
  declares: "StaticTable";
  holder: BuiltinPath;
  table: StaticTableRef;
}

export interface ClassDeclaration {
  declares: "Class";
  path: BuiltinPath;
  name: ClassName;
  columns: ClassColumns;
  entryColumns: EntryColumns;
}

export interface MemberDeclaration {
  declares: "Member";
  path: BuiltinPath;
  kind: Exclude<BuiltinKind, "Namespace" | "Prototype" | "NativeBinding">;
  isOwnPropertyOfInstances: boolean;
  isLazilyCreated: boolean;
  signatures: readonly SignatureDeclaration[];
  dispatches: readonly ModeDispatchDeclaration[];
  constant: ConstantSpec | null;
  length: number | null;
  columns: EntryColumns;
}

export const symbolsNaming = ({ columns: { nativeSymbols = [], creationSymbol } }: MemberDeclaration): string[] =>
  creationSymbol === undefined ? [...nativeSymbols] : [...nativeSymbols, creationSymbol];

export interface ExtensionDeclaration {
  declares: "Extension";
  path: BuiltinPath;
  columns: EntryColumns;
}

export interface LayoutFieldDeclaration {
  name: string | null;
  isOptional: boolean;
  type: TypeSpec;
}

export interface LayoutDeclaration {
  declares: "Layout";
  name: LayoutName;
  isTuple: boolean;
  fields: readonly LayoutFieldDeclaration[];
}

export interface SameObjectDeclaration {
  declares: "SameObject";
  path: BuiltinPath;
  original: BuiltinPath;
}

export interface InheritanceDeclaration {
  declares: "Inheritance";
  path: BuiltinPath;
  prototype: BuiltinPath | null;
  isUnknown?: boolean;
  namesFoundAbove?: readonly string[];
}

export interface NameDeclaration {
  declares: "Name";
  text: string;
}

export interface OwnPropertyDeclaration {
  name: string;
  isAccessor: boolean;
  isFixed: boolean;
  isCreatedInSomeRuns: boolean;
}

export interface OwnPropertiesDeclaration {
  declares: "OwnProperties";
  holder: BuiltinPath;
  isOpen: boolean;
  properties: readonly OwnPropertyDeclaration[];
}

export type ModuleExportHolder = "ModuleObject" | "Property" | "Namespace";

export interface ModuleExportDeclaration {
  name: string;
  holder: ModuleExportHolder;
  platforms: readonly TargetPlatform[];
}

export interface ModuleDeclaration {
  declares: "Module";
  specifier: string;
  registryId: number;
  registryField: string | null;
  hasSource: boolean;
  isEsModule: boolean;
  hasOpenNamespace: boolean;
  listsAllExports: boolean;
  isPublic: boolean;
  isFunction: boolean | null;
  namespacePath: BuiltinPath | null;
  exports: readonly ModuleExportDeclaration[];
  exportsHeldByNamespace: readonly string[];
}

export const runtimeFunctionRoles = ["EngineBuiltin", "HostBuiltin", "CompilerFunction"] as const;
export type RuntimeFunctionRole = (typeof runtimeFunctionRoles)[number];

export interface RuntimeFunctionDeclaration {
  declares: "RuntimeFunction";
  id: number;
  role: RuntimeFunctionRole;
  function: FunctionRef;
  exportName: string;
  cppCodeName: string | null;
  isHiddenInStackTraces: boolean;
  isNeverInlined: boolean;
  platforms: readonly TargetPlatform[];
}

export interface NativeBindingDeclaration {
  declares: "NativeBinding";
  kind: NativeCallKind;
  file: string;
  symbol: string;
  hostNumber: number | null;
  answersFunction: boolean;
  isHandedByNativeCode?: boolean;
}

export interface NativeLoadDeclaration {
  declares: "NativeLoad";
  registryField: string;
  onBehalfOf: BuiltinPath | "Startup";
  exports: readonly string[] | null;
}

export interface NativeCallsDeclaration {
  declares: "NativeCalls";
  through: readonly string[];
  calls: readonly (FunctionRef | { function: FunctionRef; uses: readonly HostUseSpec[] })[];
  otherMentions: readonly NativeMentionSpec[];
  callers: readonly NativeCallerSpec[];
}

export interface IntrinsicDeclaration {
  declares: "Intrinsic";
  name: string;
  position: IntrinsicPosition;
  isPrefixedGlobal: boolean;
  meaning: IntrinsicSpec;
}

export interface ExcludedFunctionDeclaration {
  declares: "ExcludedFunction";
  function: FunctionRef;
}

export interface HandwrittenFileDeclaration {
  declares: "HandwrittenFile";
  file: string;
}

export interface HostRoleDeclaration {
  declares: "HostRole";
  path: BuiltinPath;
}

export interface HostClassRoleDeclaration {
  declares: "HostClassRole";
  name: ClassName;
}

export interface UnfilledHostRolesDeclaration {
  declares: "UnfilledHostRoles";
  path: BuiltinPath;
  class: ClassName;
}

export interface LeftOutDeclaration {
  declares: "LeftOut";
  schemas: readonly string[];
  paths: readonly BuiltinPath[];
  definedNames: readonly string[];
  modules: readonly string[];
  mayBeNamed: boolean;
}

export interface FrontEndFunctionDeclaration {
  declares: "FrontEndFunction";
  function: FunctionRef;
}

export interface EngineFeaturesDeclaration {
  declares: "EngineFeatures";
  names: readonly FeatureName[];
}

export type HostReadUse = Extract<HostUseSpec, { read: "Inspected" | "PassedTo" }> | { read: "Called" };

export interface HostReadDeclaration {
  declares: "HostRead";
  context: HostReadContext;
  exportName: string;
  use: HostReadUse;
  namesLookedFor: readonly string[];
}

export interface HostObjectDeclaration {
  declares: "HostObject";
  cell: string;
  function: BuiltinPath | null;
  prototype: BuiltinPath | null;
  fields: readonly HostObjectFieldSpec[];
  internalFields: readonly HostObjectInternalFieldSpec[];
  keepsStringsOfStoredValues: boolean;
  nativeAccessors: readonly string[];
  lockedNames: readonly string[];
  holder: BuiltinPath | null;
  platforms: readonly TargetPlatform[];
}

export interface StartupDeclaration {
  declares: "Startup";
  modules: readonly string[];
  loads: readonly ModuleLoadSpec[];
  functionsCalledWithLoads: readonly CalledFunctionSpec[];
  namesOfMethodsCallingLoads: readonly string[];
  functionsLayingSuchMethods: readonly FunctionRef[];
  moduleLoaderFunctions: readonly FunctionRef[];
}

export interface SeenCodeOnlyDeclaration {
  declares: "SeenCodeOnly";
  holders: readonly BuiltinPath[];
  nativeFiles: readonly string[];
}

export interface InstancesWithoutOwnNamesDeclaration {
  declares: "InstancesWithoutOwnNames";
  classes: readonly ClassName[];
}

export type Declaration =
  | NamespaceDeclaration
  | StaticTableDeclaration
  | ClassDeclaration
  | MemberDeclaration
  | ExtensionDeclaration
  | LayoutDeclaration
  | SameObjectDeclaration
  | InheritanceDeclaration
  | NameDeclaration
  | OwnPropertiesDeclaration
  | ModuleDeclaration
  | RuntimeFunctionDeclaration
  | NativeBindingDeclaration
  | NativeLoadDeclaration
  | NativeCallsDeclaration
  | IntrinsicDeclaration
  | ExcludedFunctionDeclaration
  | HandwrittenFileDeclaration
  | HostRoleDeclaration
  | UnfilledHostRolesDeclaration
  | LeftOutDeclaration
  | FrontEndFunctionDeclaration
  | EngineFeaturesDeclaration
  | HostClassRoleDeclaration
  | HostReadDeclaration
  | HostObjectDeclaration
  | StartupDeclaration
  | SeenCodeOnlyDeclaration
  | InstancesWithoutOwnNamesDeclaration;

export interface SourcedDeclaration {
  declaration: Declaration;
  source: Source;
}

export const leftOutOf = (declarations: readonly SourcedDeclaration[]): LeftOutDeclaration[] =>
  declarations.flatMap(({ declaration }) => (declaration.declares === "LeftOut" ? [declaration] : []));

export class DeclarationFile {
  readonly declarations: readonly Declaration[];
  constructor(declarations: readonly Declaration[]) {
    this.declarations = declarations;
  }
}

export class GeneratorError extends Error {}

export class Problems {
  readonly #lines: string[] = [];

  add(source: Source | null, message: string): void {
    if (source === null) this.#lines.push(message);
    else this.#lines.push(`${source.file}${source.line === null ? "" : `:${source.line}`}: ${message}`);
  }

  get isEmpty(): boolean {
    return this.#lines.length === 0;
  }

  get lines(): readonly string[] {
    return this.#lines;
  }

  throwIfAny(): void {
    if (!this.isEmpty) throw new GeneratorError(this.#lines.join("\n"));
  }
}
