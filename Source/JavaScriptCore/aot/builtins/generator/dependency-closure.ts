import type {
  ModelBuiltin,
  ModelClass,
  ModelCondition,
  ModelFlow,
  ModelFunction,
  ModelHostCall,
  ModelModule,
  ModelName,
  ModelOperand,
  ModelOverload,
  ModelParameter,
  ModelPlace,
  ModelTransfer,
  ModelType,
  ModelTypePart,
  TableModel,
} from "./model.ts";
import { arrayClassName, functionClassName, objectClassName } from "./build-types.ts";
import { namesProbedOnIterators, operandUses, type ClassName, type FeatureName } from "./vocabulary.ts";

export interface LoadedModule {
  isConditional: boolean;
  exportsRead: ReadonlySet<string> | null;
}

export interface CallsForTexts {
  argument: number;
  texts: readonly ModelName[];
  calls: Set<ModelFunction>;
}

export interface RuntimeDependencies {
  loads: Map<ModelModule, LoadedModule>;
  calls: Set<ModelFunction>;
  callsForTexts: Map<string, CallsForTexts>;
  callsWhereCallRunsOtherCode: Set<ModelFunction>;
  implementedBy: Set<ModelFunction>;
  certain: Set<ModelFunction>;
  namedBy: Map<ModelModule | ModelFunction, ModelBuiltin>;
  hasMethodCalledWithStartupLoads: boolean;
  exposesAnyHostObject: boolean;
}

export interface UndescribedResult {
  builtin: ModelBuiltin;
  reason: string;
}

interface ExposedValues {
  objects: readonly ModelBuiltin[];
  undescribedValue: string | null;
  features: readonly FeatureName[];
  hasOnlyDescribedObjects: boolean;
}

const promiseClassName: ClassName = "Promise";
const proxyClassName: ClassName = "Proxy";

const noExposedValues: ExposedValues = { objects: [], undescribedValue: null, features: [], hasOnlyDescribedObjects: false };

function exposesOnlyDescribedObjects(builtin: ModelBuiltin): boolean {
  const standsForWhatLiesBelow = ({ kind }: ModelBuiltin): boolean => kind === "Constructor" || kind === "Prototype";
  for (let current: ModelBuiltin | null = builtin; current !== null; current = current.holder) {
    const isMarked = current.marks.includes("ExposesOnlyDescribedObjects");
    if (isMarked && (current === builtin || standsForWhatLiesBelow(current))) return true;
  }
  return false;
}

function createDependencies(): RuntimeDependencies {
  return {
    loads: new Map(),
    calls: new Set(),
    callsForTexts: new Map(),
    callsWhereCallRunsOtherCode: new Set(),
    implementedBy: new Set(),
    certain: new Set(),
    namedBy: new Map(),
    hasMethodCalledWithStartupLoads: false,
    exposesAnyHostObject: false,
  };
}

function addLoad(dependencies: RuntimeDependencies, module: ModelModule, load: LoadedModule): void {
  const known = dependencies.loads.get(module);
  if (known === undefined) {
    dependencies.loads.set(module, load);
    return;
  }
  dependencies.loads.set(module, {
    isConditional: known.isConditional && load.isConditional,
    exportsRead:
      known.exportsRead === null || load.exportsRead === null ? null : known.exportsRead.union(load.exportsRead),
  });
}

function addCallsForTexts(
  dependencies: RuntimeDependencies,
  key: string,
  { argument, texts, calls }: CallsForTexts,
): void {
  const known = dependencies.callsForTexts.get(key);
  if (known === undefined) dependencies.callsForTexts.set(key, { argument, texts, calls: new Set(calls) });
  else for (const called of calls) known.calls.add(called);
}

function textsDecidingCall(condition: ModelCondition): Pick<CallsForTexts, "argument" | "texts"> | null {
  const { operand, isOneOf, startsWith, hasProperty } = condition;
  const asksMore =
    startsWith.length > 0 ||
    hasProperty !== null ||
    condition.buildAdmitsRuntimeCode ||
    condition.callRunsOtherCode ||
    condition.objectNobodyKnowsIsWritten ||
    condition.testsReplaceBuiltins ||
    condition.programStores !== null ||
    condition.isAskedForOtherNames !== null;
  if (operand === null || operand.operand !== "Argument" || isOneOf.length === 0 || asksMore) return null;
  return { argument: operand.index, texts: isOneOf };
}

function mergeDependencies(into: RuntimeDependencies, from: RuntimeDependencies): void {
  for (const [module, load] of from.loads) addLoad(into, module, load);
  for (const called of from.calls) into.calls.add(called);
  for (const [key, listed] of from.callsForTexts) addCallsForTexts(into, key, listed);
  for (const called of from.callsWhereCallRunsOtherCode) into.callsWhereCallRunsOtherCode.add(called);
  for (const implementation of from.implementedBy) into.implementedBy.add(implementation);
  for (const needed of from.certain) into.certain.add(needed);
  into.hasMethodCalledWithStartupLoads ||= from.hasMethodCalledWithStartupLoads;
  into.exposesAnyHostObject ||= from.exposesAnyHostObject;
  for (const [dependency, builtin] of from.namedBy) {
    if (!into.namedBy.has(dependency)) into.namedBy.set(dependency, builtin);
  }
}

const asksOnlyWhetherCallRunsOtherCode = (condition: ModelCondition): boolean =>
  condition.callRunsOtherCode &&
  condition.operand === null &&
  condition.calledOn === null &&
  condition.hasProperty === null &&
  condition.programStores === null &&
  !condition.buildAdmitsRuntimeCode &&
  !condition.objectNobodyKnowsIsWritten &&
  !condition.testsReplaceBuiltins;

function collectCompiledFunctions(functions: readonly ModelFunction[]): Map<ModelBuiltin, ModelFunction[]> {
  const compiledFunctions = new Map<ModelBuiltin, ModelFunction[]>();
  for (const compiled of functions) {
    for (const builtin of compiled.implements) {
      compiledFunctions.set(builtin, [...(compiledFunctions.get(builtin) ?? []), compiled]);
    }
  }
  return compiledFunctions;
}

const isExposingTransfer = ({ from, to }: ModelTransfer): boolean =>
  to.place !== "NativeMemory" && (to.place !== "NativeCell" || from.place === "NewValue");

function exposedPlacesOf({ transfers, callbacks, propertyActions, handlerCalls }: ModelFlow): ModelPlace[] {
  return [
    ...transfers.filter(isExposingTransfer).map(({ from }) => from),
    ...handlerCalls.flatMap(call => [call.thisValue, ...call.arguments]),
    ...callbacks.flatMap(callback => [
      callback.newTarget,
      callback.thisValue,
      ...callback.arguments,
      callback.spreadArguments,
    ]),
    ...propertyActions.map(action => action.value),
  ];
}

const claimsEveryUse = ({ uses }: ModelParameter): boolean => uses.length === operandUses.length;

const objectsOfClass = ({ constructor, prototype }: ModelClass): ModelBuiltin[] =>
  [constructor, prototype].filter((object): object is ModelBuiltin => object !== null);

function collectClassObjects(classes: readonly ModelClass[]): Map<ModelClass, ModelBuiltin[]> {
  const objects = new Map(classes.map((builtinClass): [ModelClass, ModelBuiltin[]] => [builtinClass, []]));
  for (const derived of classes) {
    for (let ancestor: ModelClass | null = derived; ancestor !== null; ancestor = ancestor.parent) {
      objects.get(ancestor)?.push(...objectsOfClass(derived));
    }
  }
  return objects;
}

const featuresOf = ({ attachments }: ModelBuiltin): FeatureName[] =>
  attachments.featureUses.map(({ feature }) => feature);

function linkedObjectsOf({ prototype, instanceClass }: ModelBuiltin): ModelBuiltin[] {
  const inherited = prototype.link === "Builtin" ? [prototype.builtin] : [];
  return instanceClass === null ? inherited : [...inherited, ...objectsOfClass(instanceClass)];
}

const receiverObjectOf = ({ holder }: ModelBuiltin): ModelBuiltin | null =>
  holder === null || holder.kind === "Prototype" ? null : holder;

function rootOf(builtin: ModelBuiltin): ModelBuiltin {
  let root = builtin;
  while (root.holder !== null) root = root.holder;
  return root;
}

const isStoredValue = ({ kind, flags }: ModelBuiltin): boolean =>
  kind === "DataProperty" && !flags.includes("IsLazilyCreated");

const isKnownByNameOnly = (builtin: ModelBuiltin): boolean =>
  isStoredValue(builtin) && builtin.overloads.every(({ proof }) => proof === "Structure");

interface NamesAskedOfOperands {
  called: ReadonlySet<ModelName>;
  read: ReadonlySet<ModelName>;
}

function collectNamesAskedOfOperands({ overloads, protocolKeys }: TableModel): NamesAskedOfOperands[] {
  const asked = new Map<string, NamesAskedOfOperands>();
  for (const { receiver, parameters } of overloads) {
    for (const parameter of receiver === null ? parameters : [receiver, ...parameters]) {
      const called = new Set([
        ...protocolKeys.filter(({ protocol }) => parameter.protocols.includes(protocol)).flatMap(({ keys }) => keys),
        ...parameter.calledMembers,
      ]);
      const read = new Set([...parameter.namedMembers, ...parameter.convertedMembers]);
      if (called.size + read.size === 0) continue;
      const numbersOf = (names: ReadonlySet<ModelName>): string => Array.from(names, name => name.number).sort().join(",");
      asked.set(`${numbersOf(called)}:${numbersOf(read)}`, { called, read });
    }
  }
  return [...asked.values()];
}

class ExposedValueCollector {
  readonly objects = new Set<ModelBuiltin>();
  undescribedValue: string | null = null;
  readonly #classObjects: ReadonlyMap<ModelClass, readonly ModelBuiltin[]>;
  readonly #functionsInCells: ReadonlyMap<ModelName, readonly ModelBuiltin[]>;
  readonly #receiverObject: ModelBuiltin | null;
  readonly #visited = new Set<ModelType>();

  constructor(
    classObjects: ReadonlyMap<ModelClass, readonly ModelBuiltin[]>,
    functionsInCells: ReadonlyMap<ModelName, readonly ModelBuiltin[]>,
    receiverObject: ModelBuiltin | null,
  ) {
    this.#classObjects = classObjects;
    this.#functionsInCells = functionsInCells;
    this.#receiverObject = receiverObject;
  }

  addOverload(overload: ModelOverload): void {
    const { mode, effects, flags, receiver, parameters } = overload;
    this.#addType(overload.result.type, `the result of ${mode}`);
    for (const place of exposedPlacesOf(overload.flow)) this.#addPlace(place, `a value ${mode} passes on`);
    if (effects.capability === "js" && !flags.includes("RunsOnlyOperandCode")) {
      this.#addUndescribedValue(`${mode} can run JavaScript that no operand names`);
    }
    if ((receiver !== null && claimsEveryUse(receiver)) || parameters.some(claimsEveryUse)) {
      this.#addUndescribedValue(`${mode} has an operand with every use`);
    }
  }

  addValues(result: ModelType, places: readonly ModelPlace[]): void {
    this.#addType(result, "the result");
    for (const place of places) this.#addPlace(place, "a value that is passed on");
  }

  addCallByHost({ thisValue, arguments: passed, otherArguments }: ModelHostCall): void {
    for (const place of [thisValue, ...(passed ?? []), otherArguments]) {
      if (place.place === "NativeCell") this.#addPlace(place, "a value native code passes on");
    }
  }

  #addUndescribedValue(reason: string): void {
    this.undescribedValue ??= reason;
  }

  #addPlace(place: ModelPlace, role: string): void {
    switch (place.place) {
      case "NewValue":
        this.#addType(place.type, role);
        return;
      case "Slot":
        this.#addOperand(place.operand);
        if (place.slot.holds !== null) this.#addType(place.slot.holds, role);
        return;
      case "ResultSlot":
        if (place.slot.holds !== null) this.#addType(place.slot.holds, role);
        return;
      case "Operand":
      case "IteratedValues":
      case "OwnValues":
      case "Member":
        this.#addOperand(place.operand);
        return;
      case "Prototype":
        this.#addUndescribedValue(`${role} is the prototype of an operand`);
        return;
      case "None":
      case "Result":
      case "ResultField":
      case "NativeMemory":
        return;
      case "NativeCell":
        for (const made of this.#functionsInCells.get(place.cell) ?? []) this.objects.add(made);
        return;
    }
  }

  #addOperand(operand: ModelOperand): void {
    if (operand.operand === "Receiver" && this.#receiverObject !== null) this.objects.add(this.#receiverObject);
  }

  #addType(type: ModelType, role: string): void {
    if (this.#visited.has(type)) return;
    this.#visited.add(type);
    if (type.isAnything) this.#addUndescribedValue(`${role} is anything`);
    else if (type.hasAnyObject) this.#addUndescribedValue(`${role} is any object`);
    else if (type.hasAnyFunction) this.#addUndescribedValue(`${role} is any function`);
    if (type.hasPlainData) for (const token of [arrayClassName, objectClassName]) this.#addInstanceOf(token);
    for (const part of type.parts) this.#addPart(part, role);
  }

  #addInstanceOf(token: ClassName): void {
    for (const [builtinClass, objects] of this.#classObjects) {
      if (builtinClass.token === token) for (const object of objects) this.objects.add(object);
    }
  }

  #addPart(part: ModelTypePart, role: string): void {
    switch (part.part) {
      case "Instance":
        for (const object of this.#classObjects.get(part.class) ?? []) this.objects.add(object);
        for (const slot of part.class.slots) {
          const held = part.contents.find(contents => contents.slot === slot)?.type ?? slot.holds;
          const heldRole = `${slot.name.text} of the ${part.class.token} in ${role}`;
          if (held === null) this.#addUndescribedValue(`${heldRole} is not declared`);
          else this.#addType(held, heldRole);
        }
        return;
      case "Layout":
        this.#addInstanceOf(part.layout.isTuple ? arrayClassName : objectClassName);
        for (const field of part.layout.fields) this.#addType(field.type, role);
        return;
      case "Builtin":
        this.objects.add(part.builtin);
        return;
      case "Slot":
        this.#addOperand(part.operand);
        if (part.slot.holds !== null) this.#addType(part.slot.holds, role);
        return;
      case "Operand":
      case "IteratedValues":
        this.#addOperand(part.operand);
        return;
      case "Prototype":
        this.#addUndescribedValue(`${role} is the prototype of an operand`);
        return;
      case "InstanceOfReceiverClass":
      case "TypedArrayOfReceiverElementType":
      case "CallbackResult":
        return;
    }
  }
}

export class DependencyClosure {
  readonly #builtins: readonly ModelBuiltin[];
  readonly #classObjects: ReadonlyMap<ModelClass, readonly ModelBuiltin[]>;
  readonly #functionsInCells: ReadonlyMap<ModelName, readonly ModelBuiltin[]>;
  readonly #hostClassObjectsInEveryExecutable: readonly ModelBuiltin[];
  readonly #hostClassObjectsOfFeature: ReadonlyMap<FeatureName, readonly ModelBuiltin[]>;
  readonly #engineClassObjects: ReadonlySet<ModelBuiltin>;
  readonly #namespacesWrittenInJavaScript: ReadonlySet<ModelBuiltin>;
  readonly #compiledFunctions: ReadonlyMap<ModelBuiltin, readonly ModelFunction[]>;
  readonly #namesAskedOfOperands: readonly NamesAskedOfOperands[];
  readonly #namesOfMethodsCallingStartupLoads: readonly ModelName[];
  readonly #functionsLayingSuchMethods: ReadonlySet<ModelFunction>;
  readonly #promiseClass: ModelClass | null;
  readonly #enginePrototypes: readonly ModelBuiltin[];
  readonly #askedOfAnyOperand: readonly NamesAskedOfOperands[];
  readonly #foundOnInstances = new Map<string, RuntimeDependencies>();
  readonly #exposed = new Map<ModelBuiltin, ExposedValues>();
  readonly #reachable = new Map<string, RuntimeDependencies>();
  readonly #dependencies = new Map<ModelBuiltin, RuntimeDependencies>();

  constructor(model: TableModel) {
    const classesOf = (origin: ModelClass["origin"]): ModelClass[] =>
      model.classes.filter(builtinClass => builtinClass.origin === origin);
    this.#builtins = model.builtins;
    this.#classObjects = collectClassObjects(model.classes);
    const functionsInCells = new Map<ModelName, ModelBuiltin[]>();
    for (const { cell, function: made } of model.hostObjects) {
      if (made !== null) functionsInCells.set(cell, [...(functionsInCells.get(cell) ?? []), made]);
    }
    this.#functionsInCells = functionsInCells;
    const inEveryExecutable: ModelBuiltin[] = [];
    const ofFeature = new Map<FeatureName, ModelBuiltin[]>();
    for (const builtinClass of classesOf("Host")) {
      if (builtinClass.instancesAreEntries) continue;
      const objects = objectsOfClass(builtinClass);
      const features = new Set(objects.flatMap(featuresOf));
      if (features.size === 0) inEveryExecutable.push(...objects);
      for (const feature of features) {
        const listed = ofFeature.get(feature);
        if (listed === undefined) ofFeature.set(feature, [...objects]);
        else listed.push(...objects);
      }
    }
    this.#hostClassObjectsInEveryExecutable = inEveryExecutable;
    this.#hostClassObjectsOfFeature = ofFeature;
    this.#engineClassObjects = new Set(classesOf("Engine").flatMap(objectsOfClass));
    this.#namespacesWrittenInJavaScript = new Set(
      model.modules.filter(module => module.hasSource).map(module => module.namespace),
    );
    this.#compiledFunctions = collectCompiledFunctions(model.functions);
    this.#namesAskedOfOperands = collectNamesAskedOfOperands(model);
    this.#namesOfMethodsCallingStartupLoads = model.startup.namesOfMethodsCallingLoads;
    this.#functionsLayingSuchMethods = new Set(model.startup.functionsLayingSuchMethods);
    this.#promiseClass = model.classes.find(({ token }) => token === promiseClassName) ?? null;
    this.#enginePrototypes = classesOf("Engine").flatMap(({ prototype }) => prototype ?? []);
    const symbols = model.names.filter(({ kind }) => kind !== "Text");
    const probed = model.names.filter(({ kind, text }) => kind === "Text" && namesProbedOnIterators.includes(text));
    this.#askedOfAnyOperand = [
      {
        called: new Set([
          ...symbols,
          ...probed,
          ...model.protocolKeys.flatMap(({ keys }) => keys),
          ...this.#namesAskedOfOperands.flatMap(({ called }) => [...called]),
        ]),
        read: new Set([...symbols, ...this.#namesAskedOfOperands.flatMap(({ read }) => [...read])]),
      },
    ];
  }

  dependenciesOf(builtin: ModelBuiltin): RuntimeDependencies {
    let dependencies = this.#dependencies.get(builtin);
    if (dependencies === undefined) {
      dependencies = this.#computeDependencies(builtin);
      this.#dependencies.set(builtin, dependencies);
    }
    return dependencies;
  }

  #computeDependencies(builtin: ModelBuiltin): RuntimeDependencies {
    const dependencies = createDependencies();
    this.#addOwnDependencies(dependencies, builtin, null);
    mergeDependencies(dependencies, this.#reachableFrom(this.#exposedBy(builtin)));
    mergeDependencies(dependencies, this.#foundOnInstancesFrom(builtin));
    const holders = this.#holdersOfExposedClasses(builtin);
    for (const holder of holders) this.#addOwnDependencies(dependencies, holder, holders);
    if (this.#engineClassObjects.has(builtin)) {
      mergeDependencies(dependencies, this.#reachableFrom({ ...noExposedValues, objects: [builtin] }));
    }
    if (DependencyClosure.isInEveryProgram(builtin)) {
      const { calledFunctions } = builtin.attachments;
      const calledOnCondition = new Set(calledFunctions.flatMap(called => (called.condition === null ? [] : [called.function])));
      dependencies.certain = dependencies.calls.union(dependencies.implementedBy).difference(calledOnCondition);
      dependencies.exposesAnyHostObject = false;
    }
    for (const [key, listed] of dependencies.callsForTexts) {
      listed.calls = listed.calls.difference(dependencies.calls);
      if (listed.calls.size === 0) dependencies.callsForTexts.delete(key);
    }
    dependencies.callsWhereCallRunsOtherCode = dependencies.callsWhereCallRunsOtherCode.difference(
      dependencies.calls.union(dependencies.implementedBy),
    );
    return dependencies;
  }

  dependenciesOfEveryProgram(): RuntimeDependencies {
    const dependencies = createDependencies();
    for (const builtin of this.#builtins) {
      if (DependencyClosure.isInEveryProgram(builtin)) mergeDependencies(dependencies, this.dependenciesOf(builtin));
    }
    return dependencies;
  }

  makesPromise(builtin: ModelBuiltin): boolean {
    if (this.#promiseClass === null) return false;
    const ofPromises = objectsOfClass(this.#promiseClass).map(holder => holder.sameObjectAs ?? holder);
    const made = builtin.sameObjectAs ?? builtin;
    const holders = this.#holdersOfExposedClasses(made);
    return ofPromises.some(holder => holder === made || holders.has(holder));
  }

  prototypesThatComeWithFirstPromise(): ModelBuiltin[] {
    return this.#enginePrototypes.filter(prototype => prototype.members.some(member => this.makesPromise(member)));
  }

  dependenciesOfAnyOperand(): RuntimeDependencies {
    const leftOut = new Set(this.prototypesThatComeWithFirstPromise());
    const prototypes = this.#enginePrototypes.filter(prototype => !leftOut.has(prototype));
    return this.#foundOn(prototypes, this.#askedOfAnyOperand, "of any operand");
  }

  dependenciesOfAnyHostObject(): RuntimeDependencies {
    return this.#reachableFrom({ ...noExposedValues, objects: this.#hostClassObjectsInEveryExecutable });
  }

  entriesInReachOf(roots: readonly ModelBuiltin[], isLeftOut: (builtin: ModelBuiltin) => boolean): ModelBuiltin[] {
    const visited = new Set<ModelBuiltin>();
    const workList = [...roots];
    let holdsAnyHostObject = false;
    for (let next = workList.pop(); next !== undefined; next = workList.pop()) {
      const builtin = next.sameObjectAs ?? next;
      if (visited.has(builtin) || isLeftOut(builtin)) continue;
      visited.add(builtin);
      const { objects, undescribedValue } = this.#exposedBy(builtin);
      workList.push(...builtin.members, ...linkedObjectsOf(builtin), ...objects);
      if (undescribedValue === null || holdsAnyHostObject) continue;
      holdsAnyHostObject = true;
      workList.push(...this.#hostClassObjectsInEveryExecutable, ...this.#hostClassObjectsOfFeature.values().toArray().flat());
    }
    return [...visited].sort((left, right) => left.id - right.id);
  }

  undescribedResults(): UndescribedResult[] {
    return this.#builtins.flatMap(builtin => {
      const reason = this.#exposedBy(builtin).undescribedValue;
      return reason === null ? [] : [{ builtin, reason }];
    });
  }

  static isInEveryProgram(builtin: ModelBuiltin): boolean {
    return builtin.origin === "Engine" || builtin.flags.includes("IsCompilerHelper");
  }

  #isMadeByHostCode(builtin: ModelBuiltin): boolean {
    if (DependencyClosure.isInEveryProgram(builtin)) return false;
    if (this.#namespacesWrittenInJavaScript.has(rootOf(builtin))) return false;
    const { holder } = builtin;
    const isOnEngineObject = holder !== null && holder.origin === "Engine" && !holder.flags.includes("IsGlobalObject");
    return !(isOnEngineObject && isKnownByNameOnly(builtin));
  }

  #addOwnDependencies(
    dependencies: RuntimeDependencies,
    builtin: ModelBuiltin,
    reachedWith: ReadonlySet<ModelBuiltin> | null,
  ): void {
    const isReachedByValue = reachedWith !== null;
    const own = createDependencies();
    const namesOfMethods = this.#namesOfMethodsCallingStartupLoads;
    const { constant } = builtin;
    own.hasMethodCalledWithStartupLoads =
      (namesOfMethods.includes(builtin.name) && !builtin.marks.includes("IsCalledWithoutStartupLoads")) ||
      (constant !== null && constant.constant === "Symbol" && namesOfMethods.includes(constant.name)) ||
      builtin.instanceClass?.token === proxyClassName;
    for (const { module, exports, condition } of builtin.attachments.moduleLoads) {
      addLoad(own, module, {
        isConditional: isReachedByValue || condition !== null,
        exportsRead: exports === null ? null : new Set(exports.map(({ name }) => name.text)),
      });
    }
    for (const { function: called, condition } of builtin.attachments.calledFunctions) {
      const decided = condition === null ? null : textsDecidingCall(condition);
      if (
        condition !== null &&
        reachedWith === null &&
        asksOnlyWhetherCallRunsOtherCode(condition) &&
        !DependencyClosure.isInEveryProgram(builtin)
      ) {
        own.callsWhereCallRunsOtherCode.add(called);
      } else if (condition === null || decided === null) {
        own.calls.add(called);
        if (condition === null && !isReachedByValue) own.certain.add(called);
      } else if (reachedWith === null) {
        const key = `${decided.argument} ${decided.texts.map(text => text.number).join(",")}`;
        addCallsForTexts(own, key, { ...decided, calls: new Set([called]) });
      } else if (
        condition.calledOn === null ||
        reachedWith.has(condition.calledOn.sameObjectAs ?? condition.calledOn)
      ) {
        own.calls.add(called);
      }
    }
    for (const compiled of this.#compiledFunctions.get(builtin) ?? []) own.implementedBy.add(compiled);
    for (const { code } of builtin.overloads) {
      if (code.model !== null) own.implementedBy.add(code.model);
    }
    if (!isReachedByValue) own.certain = own.certain.union(own.implementedBy);
    own.hasMethodCalledWithStartupLoads ||= !own.calls.union(own.implementedBy).isDisjointFrom(this.#functionsLayingSuchMethods);
    for (const dependency of [...own.loads.keys(), ...own.calls, ...own.implementedBy]) {
      own.namedBy.set(dependency, builtin);
    }
    mergeDependencies(dependencies, own);
  }

  #foundOnInstancesFrom(builtin: ModelBuiltin): RuntimeDependencies {
    const isPrototypeOfLanguage = ({ kind, origin }: ModelBuiltin): boolean => kind === "Prototype" && origin === "Engine";
    const exposed = this.#instancesExposedBy(builtin);
    const makesOnlyWhatItSays =
      exposed.undescribedValue === null || DependencyClosure.isInEveryProgram(builtin) || builtin.kind === "DataProperty";
    const roots = makesOnlyWhatItSays ? exposed.objects : this.#enginePrototypes;
    const prototypes = [builtin, ...roots].filter(isPrototypeOfLanguage);
    const key = prototypes.map(prototype => prototype.id).join(",");
    return this.#foundOn(prototypes, this.#namesAskedOfOperands, key);
  }

  #foundOn(
    prototypes: readonly ModelBuiltin[],
    asked: readonly NamesAskedOfOperands[],
    key: string,
  ): RuntimeDependencies {
    const isPrototypeOfLanguage = ({ kind, origin }: ModelBuiltin): boolean => kind === "Prototype" && origin === "Engine";
    let dependencies = this.#foundOnInstances.get(key);
    if (dependencies !== undefined) return dependencies;
    dependencies = createDependencies();
    this.#foundOnInstances.set(key, dependencies);
    const found = new Set<ModelBuiltin>();
    for (const { called, read } of asked) {
      const visited = new Set<ModelBuiltin>();
      const workList = [...prototypes];
      for (let next = workList.pop(); next !== undefined; next = workList.pop()) {
        const prototype = next.sameObjectAs ?? next;
        if (visited.has(prototype) || !isPrototypeOfLanguage(prototype)) continue;
        visited.add(prototype);
        if (prototype.prototype.link === "Builtin") workList.push(prototype.prototype.builtin);
        for (const listed of prototype.members) {
          const member = listed.sameObjectAs ?? listed;
          const runs = called.has(listed.name) || (member.kind === "Accessor" && read.has(listed.name));
          if (!runs || member.kind === "Constructor") continue;
          found.add(member);
          workList.push(...this.#instancesExposedBy(member).objects);
        }
      }
    }
    for (const member of found) this.#addOwnDependencies(dependencies, member, found);
    return dependencies;
  }

  #holdersOfExposedClasses(builtin: ModelBuiltin): Set<ModelBuiltin> {
    const types = new Set<ModelType>();
    const classes = new Set<ModelClass>();
    for (const { result, flow, effects, flags } of builtin.overloads) {
      types.add(result.type);
      const places = [
        ...flow.transfers.map(({ from }) => from),
        ...flow.callbacks.flatMap(call => [call.newTarget, call.thisValue, call.spreadArguments, ...call.arguments]),
        ...flow.propertyActions.map(({ value }) => value),
        ...flow.handlerCalls.flatMap(call => [call.thisValue, ...call.arguments]),
      ];
      for (const place of places) if (place.place === "NewValue") types.add(place.type);
      const runsOtherCode = effects.capability === "js" && !flags.includes("RunsOnlyOperandCode");
      const returnsAnything = result.type.isAnything || result.type.hasAnyObject;
      const isNative = !DependencyClosure.isInEveryProgram(builtin);
      if (isNative && runsOtherCode && returnsAnything && this.#promiseClass !== null) {
        classes.add(this.#promiseClass);
      }
    }
    for (const type of types) {
      for (const part of type.parts) {
        if (part.part !== "Instance") continue;
        classes.add(part.class);
        for (const held of part.contents) types.add(held.type);
      }
    }
    const holders = new Set<ModelBuiltin>();
    for (const exposed of classes) {
      if (exposed.token === functionClassName && DependencyClosure.isInEveryProgram(builtin)) continue;
      for (const holder of objectsOfClass(exposed)) holders.add(holder.sameObjectAs ?? holder);
      if (exposed.parent !== null) classes.add(exposed.parent);
    }
    return holders;
  }

  #instancesExposedBy(builtin: ModelBuiltin): ExposedValueCollector {
    const collector = new ExposedValueCollector(this.#classObjects, this.#functionsInCells, null);
    for (const { result, flow } of builtin.overloads) collector.addValues(result.type, exposedPlacesOf(flow));
    return collector;
  }

  #exposedBy(builtin: ModelBuiltin): ExposedValues {
    let exposed = this.#exposed.get(builtin);
    if (exposed === undefined) {
      exposed = this.#isMadeByHostCode(builtin) ? this.#collectExposedValues(builtin) : noExposedValues;
      this.#exposed.set(builtin, exposed);
    }
    return exposed;
  }

  #collectExposedValues(builtin: ModelBuiltin): ExposedValues {
    const collector = new ExposedValueCollector(this.#classObjects, this.#functionsInCells, receiverObjectOf(builtin));
    for (const overload of builtin.overloads) collector.addOverload(overload);
    for (const { uses } of builtin.attachments.calledFunctions) {
      for (const use of uses ?? []) if (use.read === "Called" && use.call !== null) collector.addCallByHost(use.call);
    }
    return {
      objects: [...collector.objects].sort((left, right) => left.id - right.id),
      undescribedValue: collector.undescribedValue,
      features: collector.undescribedValue === null ? [] : featuresOf(builtin),
      hasOnlyDescribedObjects: exposesOnlyDescribedObjects(builtin),
    };
  }

  #reachableFrom(roots: ExposedValues): RuntimeDependencies {
    const key = `${roots.undescribedValue !== null}:${roots.hasOnlyDescribedObjects}:${roots.features.join(",")}:${roots.objects.map(object => object.id).join(",")}`;
    let dependencies = this.#reachable.get(key);
    if (dependencies === undefined) {
      dependencies = this.#computeReachable(roots);
      this.#reachable.set(key, dependencies);
    }
    return dependencies;
  }

  #computeReachable(roots: ExposedValues): RuntimeDependencies {
    const dependencies = createDependencies();
    const visited = new Set<ModelBuiltin>();
    const reached: ModelBuiltin[] = [];
    const workList: ModelBuiltin[] = [];
    const featuresAdded = new Set<FeatureName>();
    const addExposedValues = ({ objects, undescribedValue, features, hasOnlyDescribedObjects }: ExposedValues): void => {
      workList.push(...objects);
      if (undescribedValue === null || hasOnlyDescribedObjects) return;
      dependencies.exposesAnyHostObject = true;
      for (const feature of features) {
        if (featuresAdded.has(feature)) continue;
        featuresAdded.add(feature);
        workList.push(...(this.#hostClassObjectsOfFeature.get(feature) ?? []));
      }
    };
    addExposedValues(roots);
    for (let next = workList.pop(); next !== undefined; next = workList.pop()) {
      const builtin = next.sameObjectAs ?? next;
      if (visited.has(builtin) || builtin.flags.includes("IsGlobalObject")) continue;
      visited.add(builtin);
      if (DependencyClosure.isInEveryProgram(builtin)) {
        workList.push(...builtin.members.filter(member => !DependencyClosure.isInEveryProgram(member)));
        continue;
      }
      reached.push(builtin);
      workList.push(...builtin.members, ...linkedObjectsOf(builtin));
      addExposedValues(this.#exposedBy(builtin));
    }
    for (const builtin of reached) this.#addOwnDependencies(dependencies, builtin, visited);
    return dependencies;
  }
}

const closures = new WeakMap<TableModel, DependencyClosure>();

export function closureOf(model: TableModel): DependencyClosure {
  let closure = closures.get(model);
  if (closure === undefined) {
    closure = new DependencyClosure(model);
    closures.set(model, closure);
  }
  return closure;
}
