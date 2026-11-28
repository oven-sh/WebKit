import type { Problems } from "./declarations.ts";
import { closureOf, type RuntimeDependencies } from "./dependency-closure.ts";
import type { ModelBuiltin, ModelFunction, ModelModule, TableModel } from "./model.ts";

const maximumHostFunctionCount = 20;
const undeclaredNativeCode = "%UndeclaredNativeCode%";

const describeFunction = ({ file, exportName }: ModelFunction): string => `${exportName} of ${file}`;

function hostFunctionsOf({ calls, implementedBy }: RuntimeDependencies): ModelFunction[] {
  return [...calls.union(implementedBy)]
    .filter(({ role }) => role === "HostBuiltin")
    .sort((left, right) => left.id - right.id);
}

function checkLoadsNothing(dependencies: RuntimeDependencies, consequence: string, problems: Problems): void {
  const modules = [...dependencies.loads.keys()].sort((left: ModelModule, right: ModelModule) => left.id - right.id);
  for (const module of modules) {
    problems.add(null, `${dependencies.namedBy.get(module)?.text} loads ${module.specifier}, and ${consequence}`);
  }
}

function checkHostFunctionCount(dependencies: RuntimeDependencies, owner: string, problems: Problems): void {
  const functions = hostFunctionsOf(dependencies);
  if (functions.length <= maximumHostFunctionCount) return;
  const named = functions.map(
    called => `  ${describeFunction(called)}, for ${dependencies.namedBy.get(called)?.text}\n`,
  );
  problems.add(
    null,
    `${owner} needs ${functions.length} of Bun's own functions, more than ${maximumHostFunctionCount}:\n${named.join("")}` +
      "declare the results of the entries that lead to them",
  );
}

function checkUndeclaredNativeCode(model: TableModel, problems: Problems): void {
  if (!model.builtins.some(builtin => builtin.text === undeclaredNativeCode)) {
    problems.add(null, `there is no ${undeclaredNativeCode}`);
  }
}

export function checkDependencies(model: TableModel, problems: Problems): void {
  const closure = closureOf(model);
  const anyHostObject = closure.dependenciesOfAnyHostObject();
  checkLoadsNothing(
    anyHostObject,
    "an instance of a host class leads to it, so every program in which a value may be anything would load the module",
    problems,
  );
  checkHostFunctionCount(anyHostObject, "an instance of any host class", problems);
  const everyProgram = closure.dependenciesOfEveryProgram();
  checkLoadsNothing(
    everyProgram,
    "an object of the language leads to it, so every program would load the module",
    problems,
  );
  checkHostFunctionCount(everyProgram, "every program", problems);
  checkUndeclaredNativeCode(model, problems);
}

export function describeUndescribedResults(model: TableModel): string {
  return closureOf(model)
    .undescribedResults()
    .map(({ builtin, reason }) => `${builtin.text}: ${reason}\n`)
    .join("");
}

export function undescribedProperties(model: Pick<TableModel, "builtins" | "modules">): ModelBuiltin[] {
  const writtenInJavaScript = new Set(model.modules.filter(({ hasSource }) => hasSource).map(module => module.namespace));
  const isExportOfSource = ({ holder }: ModelBuiltin): boolean => holder !== null && writtenInJavaScript.has(holder);
  return model.builtins.filter(
    builtin =>
      builtin.kind === "DataProperty" &&
      !isExportOfSource(builtin) &&
      builtin.overloads.some(({ mode, result }) => mode === "Get" && result.type.isAnything),
  );
}

export function summarizeUndescribedResults(model: TableModel): string {
  const closure = closureOf(model);
  const { loads, calls, implementedBy } = closure.dependenciesOfAnyHostObject();
  return (
    `${closure.undescribedResults().length} host entries have a result that is not described; ` +
    `${undescribedProperties(model).length} properties that native code fills hold a value that is not described; ` +
    `a program in which a value may be anything holds what any host class needs: ${loads.size} modules, ${calls.union(implementedBy).size} functions`
  );
}
