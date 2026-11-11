import type { Problems } from "./declarations.ts";
import { describeOperand, parameterAt } from "./dispatch.ts";
import type {
  ModelBuiltin,
  ModelCalledFunction,
  ModelHostCall,
  ModelHostUse,
  ModelHostUses,
  ModelHostValue,
  ModelModule,
  ModelModuleLoad,
  ModelName,
  ModelOperand,
  ModelOverload,
  ModelPlace,
  TableModel,
} from "./model.ts";
import type { BuiltinKind } from "./vocabulary.ts";

type CheckedModel = Pick<TableModel, "builtins" | "overloads" | "startup" | "hostObjects">;

interface DescribedUses {
  row: string;
  uses: ModelHostUses;
}

interface DescribedValue {
  row: string;
  value: ModelHostValue;
}

const maximumInternalFieldIndex = 0xff;
const maximumArgumentIndex = 0xff;

const kindsHoldingValues: readonly BuiltinKind[] = ["Constructor", "Function", "Method", "Accessor", "DataProperty"];

const describeOwner = (builtin: ModelBuiltin | null): string => (builtin === null ? "start-up" : builtin.text);

function usesOfLoads(owner: ModelBuiltin | null, loads: readonly ModelModuleLoad[]): DescribedUses[] {
  return loads.flatMap(({ module, exports }) =>
    (exports ?? []).map(({ name, uses }) => ({
      row: `${describeOwner(owner)} reads ${name.text} of ${module.specifier}`,
      uses,
    })),
  );
}

function usesOfCalledFunctions(owner: ModelBuiltin | null, called: readonly ModelCalledFunction[]): DescribedUses[] {
  return called.map(({ function: { file, name }, uses }) => ({
    row: `${describeOwner(owner)} uses ${name} of ${file}`,
    uses,
  }));
}

function collectWrittenUses(model: CheckedModel): DescribedUses[] {
  return [
    ...model.builtins.flatMap(builtin => [
      ...usesOfLoads(builtin, builtin.attachments.moduleLoads),
      ...usesOfCalledFunctions(builtin, builtin.attachments.calledFunctions),
    ]),
    ...usesOfLoads(null, model.startup.loads),
    ...usesOfCalledFunctions(null, model.startup.functionsCalledWithLoads),
    ...model.hostObjects.flatMap(({ cell, internalFields }) =>
      internalFields.map(({ index, uses }) => ({
        row: `internal field ${index} of the host object ${cell.text}`,
        uses,
      })),
    ),
  ];
}

const valuesOfCall = ({ thisValue, arguments: passed, otherArguments }: ModelHostCall): ModelHostValue[] => [
  thisValue,
  ...(passed ?? []),
  otherArguments,
];

function forEachUse(uses: ModelHostUses, visit: (use: ModelHostUse) => void): void {
  for (const use of uses ?? []) {
    visit(use);
    if (use.read === "Called" && use.call !== null) forEachUse(use.call.result, visit);
  }
}

function checkPassedArgument(row: string, builtin: ModelBuiltin, argument: number, problems: Problems): void {
  const position: ModelOperand = { operand: "Argument", index: argument };
  if (!Number.isInteger(argument) || argument < 0 || argument > maximumArgumentIndex) {
    problems.add(null, `${row}: ${argument} is no index of an argument`);
    return;
  }
  const takesArgument = builtin.overloads.some(
    overload => overload.mode === "Call" && parameterAt(overload, position) !== null,
  );
  if (takesArgument) return;
  problems.add(null, `${row}: it is passed to ${builtin.text}, and no Call overload has ${describeOperand(position)}`);
}

function checkCall(row: string, call: ModelHostCall, problems: Problems): void {
  if (call.arguments === null && call.otherArguments.place !== "None") {
    problems.add(null, `${row}: otherArgs follows a list of arguments, and the call has none`);
  }
  for (const value of valuesOfCall(call)) {
    if (value.place !== "NewValue") continue;
    if (value.holdsWhatIsThrown) problems.add(null, `${row}: what is thrown is said of an argument of a callback or of a handler of an overload alone`);
    const { isAnything, hasAnyObject, hasAnyFunction, hasPlainData, primitives, integers, stringLiterals, parts } =
      value.type;
    const isNever =
      !isAnything &&
      !hasAnyObject &&
      !hasAnyFunction &&
      !hasPlainData &&
      integers === null &&
      primitives.length + stringLiterals.length + parts.length === 0;
    if (isNever) problems.add(null, `${row}: native code passes a value that has no type`);
  }
}

function checkUse(row: string, use: ModelHostUse, problems: Problems): void {
  switch (use.read) {
    case "Inspected":
    case "Kept":
      return;
    case "Called":
      if (use.call !== null) checkCall(row, use.call, problems);
      return;
    case "PassedTo":
      checkPassedArgument(row, use.builtin, use.argument, problems);
      return;
    case "IsValueOf":
      if (kindsHoldingValues.includes(use.builtin.kind)) return;
      problems.add(
        null,
        `${row}: it is the value of ${use.builtin.text}, and a ${use.builtin.kind} holds no value that native code makes later`,
      );
      return;
  }
}

function cellOf(place: ModelPlace): ModelName[] {
  return place.place === "NativeCell" ? [place.cell] : [];
}

function cellsWrittenByOverloads(overloads: readonly ModelOverload[]): ModelName[] {
  return overloads.flatMap(({ flow }) => [
    ...flow.transfers.flatMap(({ to }) => cellOf(to)),
    ...flow.callbacks.flatMap(({ resultTo }) => cellOf(resultTo)),
  ]);
}

function collectWrittenCells(model: CheckedModel, written: readonly DescribedUses[]): Set<ModelName> {
  const cells = new Set([...cellsWrittenByOverloads(model.overloads), ...model.hostObjects.map(({ cell }) => cell)]);
  for (const { uses } of written) {
    forEachUse(uses, use => {
      if (use.read === "Kept") cells.add(use.cell);
    });
  }
  return cells;
}

export function cellsFilledByCalledFunctionsAlone(model: CheckedModel): ModelName[] {
  const filledOtherwise = new Set([...cellsWrittenByOverloads(model.overloads), ...model.hostObjects.map(({ cell }) => cell)]);
  const cells = new Set<ModelName>();
  for (const { uses } of collectWrittenUses(model)) {
    forEachUse(uses, use => {
      if (use.read === "Kept" && !filledOtherwise.has(use.cell)) cells.add(use.cell);
    });
  }
  return [...cells].sort((left, right) => left.number - right.number);
}

function collectReadValues(model: CheckedModel, written: readonly DescribedUses[]): DescribedValue[] {
  const values: DescribedValue[] = [];
  for (const { row, uses } of written) {
    forEachUse(uses, use => {
      if (use.read !== "Called" || use.call === null) return;
      for (const value of valuesOfCall(use.call)) values.push({ row, value });
    });
  }
  for (const { cell, fields } of model.hostObjects) {
    for (const { name, value } of fields) {
      values.push({
        row: `${name === null ? "the other names" : `the field ${name.text}`} of the host object ${cell.text}`,
        value,
      });
    }
  }
  return values;
}

function checkCells(model: CheckedModel, written: readonly DescribedUses[], problems: Problems): void {
  const writtenCells = collectWrittenCells(model, written);
  for (const { row, value } of collectReadValues(model, written)) {
    if (value.place !== "NativeCell" || writtenCells.has(value.cell)) continue;
    problems.add(
      null,
      `${row}: nothing puts a value into the native cell ${value.cell.text}: no b.keptIn, no b.hostObject and no operand of an overload`,
    );
  }
}

const placesOfOperands: readonly ModelPlace["place"][] = ["Operand", "Member", "OwnValues", "IteratedValues", "Slot"];

function checkCellsHoldingOnlyWhatIsStored(model: CheckedModel, written: readonly DescribedUses[], problems: Problems): void {
  const cells = new Set(
    model.overloads.flatMap(({ flow }) =>
      flow.transfers.flatMap(({ from }) => (from.place === "NativeCell" && from.holdsOnlyWhatIsStored ? [from.cell] : [])),
    ),
  );
  const refuse = (cell: ModelName, filler: string) => {
    if (cells.has(cell))
      problems.add(null, `the native cell ${cell.text} is said to hold only what rows store there out of their operands, and ${filler}`);
  };
  for (const { flow } of model.overloads) {
    for (const { from, to } of flow.transfers) {
      if (to.place === "NativeCell" && !placesOfOperands.includes(from.place)) refuse(to.cell, `a row stores ${from.place} there`);
    }
    for (const cell of flow.callbacks.flatMap(({ resultTo }) => cellOf(resultTo))) refuse(cell, "a row stores there what a callback answers");
    for (const cell of flow.propertyActions.flatMap(({ value }) => cellOf(value))) refuse(cell, "a row reads or stores a property through it");
  }
  const filled = new Set(cellsWrittenByOverloads(model.overloads));
  for (const cell of cells) {
    if (!filled.has(cell)) refuse(cell, "no row stores anything there");
  }
  for (const { cell } of model.hostObjects) refuse(cell, "a host object lies there");
  for (const { uses } of written) {
    forEachUse(uses, use => {
      if (use.read === "Kept") refuse(use.cell, "what a function answers that the host calls is kept there");
    });
  }
}

export interface NativeReads {
  readsEveryExport: boolean;
  names: Map<number, ModelName>;
  namesWithOtherUses: Set<number>;
}

export function collectNativeReads(model: Pick<TableModel, "builtins" | "startup">): Map<ModelModule, NativeReads> {
  const reads = new Map<ModelModule, NativeReads>();
  const loads = [...model.builtins.flatMap(builtin => builtin.attachments.moduleLoads), ...model.startup.loads];
  for (const { module, exports } of loads) {
    let read = reads.get(module);
    if (read === undefined)
      reads.set(module, (read = { readsEveryExport: false, names: new Map(), namesWithOtherUses: new Set() }));
    if (exports === null) read.readsEveryExport = true;
    else
      for (const { name, uses } of exports) {
        read.names.set(name.number, name);
        const isMemberOnly = !!uses?.length && uses.every(use => use.read === "Kept" && use.path.length > 0);
        if (!isMemberOnly) read.namesWithOtherUses.add(name.number);
      }
  }
  return reads;
}

function checkMemberOnlyCells(model: CheckedModel, problems: Problems): void {
  const flows = model.overloads.map(({ flow }) => flow);
  const called = new Set(flows.flatMap(({ handlerCalls }) => handlerCalls.flatMap(({ callee }) => cellOf(callee))));
  const cellsReadAsValues = new Set(
    flows.flatMap(({ transfers, callbacks, propertyActions, handlerCalls }) =>
      [
        ...transfers.map(({ from }) => from),
        ...callbacks.flatMap(callback => [callback.newTarget, callback.thisValue, callback.spreadArguments, ...callback.arguments]),
        ...propertyActions.map(({ value }) => value),
        ...handlerCalls.flatMap(handlerCall => [handlerCall.thisValue, ...handlerCall.arguments]),
      ].flatMap(cellOf),
    ),
  );
  const reads = collectNativeReads(model);
  const checkLoads = (owner: ModelBuiltin | null, loads: readonly ModelModuleLoad[]) => {
    for (const { module, exports } of loads) {
      const read = reads.get(module)!;
      for (const { name, uses } of exports ?? []) {
        if (read.readsEveryExport || read.namesWithOtherUses.has(name.number)) continue;
        for (const use of uses ?? []) {
          if (use.read !== "Kept") continue;
          const row = `${describeOwner(owner)} reads ${name.text} of ${module.specifier}: members of it alone are kept, in the native cell ${use.cell.text}`;
          if (!called.has(use.cell)) problems.add(null, `${row}, and no row calls what lies there`);
          if (cellsReadAsValues.has(use.cell)) problems.add(null, `${row}, and a row hands out what lies there`);
        }
      }
    }
  };
  for (const builtin of model.builtins) checkLoads(builtin, builtin.attachments.moduleLoads);
  checkLoads(null, model.startup.loads);
}

function checkHostObjects({ hostObjects }: CheckedModel, problems: Problems): void {
  const cells = new Set<ModelName>();
  for (const { cell, function: described, internalFields, source } of hostObjects) {
    if (cells.has(cell)) problems.add(source, `two host objects lie in the native cell ${cell.text}`);
    cells.add(cell);
    if (
      described !== null &&
      !described.overloads.some(({ mode, flags }) => mode === "Call" && !flags.includes("IsUndeclared"))
    ) {
      problems.add(
        source,
        `the host object ${cell.text} is a function, and ${described.text} has no Call overload that says what a call does`,
      );
    }
    const indexes = new Set<number>();
    for (const { index } of internalFields) {
      if (!Number.isInteger(index) || index < 0 || index > maximumInternalFieldIndex) {
        problems.add(source, `the host object ${cell.text}: ${index} is no index of an internal field`);
      }
      if (indexes.has(index)) problems.add(source, `the host object ${cell.text} lists internal field ${index} twice`);
      indexes.add(index);
    }
  }
}

function checkClassesWithHostMethods({ builtins, startup }: CheckedModel, problems: Problems): void {
  for (const { name, kind, holder, text } of builtins) {
    if (kind !== "DataProperty" || !startup.namesOfMethodsCallingLoads.includes(name)) continue;
    let isMarked = false;
    for (let current = holder?.instanceClass ?? null; current !== null; current = current.parent) {
      isMarked ||= current.constructor?.marks.includes("InstancesHaveMethodCalledWithStartupLoads") ?? false;
    }
    if (holder?.kind === "Prototype" && !isMarked) {
      problems.add(
        null,
        `${text} holds a native method that calls what the host loads at start-up, and the class is not marked InstancesHaveMethodCalledWithStartupLoads`,
      );
    }
  }
}

export function checkHostUses(model: CheckedModel, problems: Problems): void {
  const written = collectWrittenUses(model);
  for (const { row, uses } of written) forEachUse(uses, use => checkUse(row, use, problems));
  checkCells(model, written, problems);
  checkCellsHoldingOnlyWhatIsStored(model, written, problems);
  checkMemberOnlyCells(model, problems);
  checkHostObjects(model, problems);
  checkClassesWithHostMethods(model, problems);
}
