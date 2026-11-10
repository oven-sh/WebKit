import { closureOf } from "./dependency-closure.ts";
import type { ModelBuiltin, ModelFunction, ModelImplementation, TableModel } from "./model.ts";

export interface OpenRealm {
  language: readonly ModelBuiltin[];
  host: readonly ModelBuiltin[];
  functionsOfLanguage: readonly ModelFunction[];
  functionsOfHost: readonly ModelFunction[];
}

const flagsOfEvents = ["EvaluatesText", "LoadsModuleByName", "RunsUnseenCode"] as const;

function letsInUnseenCodeWhenCalled({ flags, attachments }: ModelBuiltin): boolean {
  const condition = attachments.unseenCodeCondition;
  if (condition !== null && condition.hasProperty !== null) return false;
  return condition !== null || flagsOfEvents.some(flag => flags.includes(flag));
}

function isLoaderOrEvaluator(builtin: ModelBuiltin): boolean {
  return (
    letsInUnseenCodeWhenCalled(builtin) ||
    builtin.loadsMachineCodeAs !== null ||
    builtin.marks.includes("HoldsLoader") ||
    builtin.marks.includes("ReadLoadsCodeByName") ||
    builtin.marks.includes("EvaluatesTextInGlobalScope") ||
    builtin.marks.includes("RunsCodeWithLoader") ||
    builtin.marks.includes("ExposesNativeRoots") ||
    builtin.overloads.some(({ result }) =>
      result.type.parts.some(
        part => part.part === "Builtin" && letsInUnseenCodeWhenCalled(part.builtin.sameObjectAs ?? part.builtin),
      ),
    )
  );
}

const isCanonical = ({ sameObjectAs }: ModelBuiltin): boolean => sameObjectAs === null;

const isOfLanguage = ({ origin, flags }: ModelBuiltin): boolean =>
  origin === "Engine" && !flags.includes("IsCompilerHelper");

function functionOf(implementation: ModelImplementation): ModelFunction | null {
  switch (implementation.how) {
    case "HostCall":
    case "Compiled":
      return implementation.function;
    case "Inline":
      return implementation.otherwise === null ? null : functionOf(implementation.otherwise);
    case "Generic":
    case "Number":
    case "Cell":
    case "CachedGetter":
    case "OwnSlot":
      return null;
  }
}

const ascending = <Item extends { id: number }>(items: Iterable<Item>): Item[] =>
  [...new Set(items)].sort((left, right) => left.id - right.id);

function functionsBehind(entries: readonly ModelBuiltin[], functions: readonly ModelFunction[]): ModelFunction[] {
  const held = new Set(entries);
  return ascending([
    ...entries.flatMap(({ overloads }) =>
      overloads.flatMap(({ code }) => functionOf(code.implementation) ?? []),
    ),
    ...functions.filter(({ implements: implemented }) =>
      implemented.some(builtin => held.has(builtin.sameObjectAs ?? builtin)),
    ),
  ]);
}

function computeOpenRealm(model: TableModel): OpenRealm {
  const isOutOfReach = (builtin: ModelBuiltin): boolean => builtin.scope !== "Realm" || isLoaderOrEvaluator(builtin);
  const language = model.builtins.filter(
    builtin => isCanonical(builtin) && isOfLanguage(builtin) && !isOutOfReach(builtin),
  );
  const ofLanguage = new Set(language);
  const host = closureOf(model).entriesInReachOf(
    language.flatMap(({ members }) => members.filter(({ origin }) => origin === "Host")),
    builtin => isOutOfReach(builtin) || ofLanguage.has(builtin) || builtin.flags.includes("IsCompilerHelper"),
  );
  return {
    language,
    host,
    functionsOfLanguage: functionsBehind(language, model.functions),
    functionsOfHost: functionsBehind(host, model.functions),
  };
}

const openRealms = new WeakMap<TableModel, OpenRealm>();

export function openRealmOf(model: TableModel): OpenRealm {
  let openRealm = openRealms.get(model);
  if (openRealm === undefined) {
    openRealm = computeOpenRealm(model);
    openRealms.set(model, openRealm);
  }
  return openRealm;
}
