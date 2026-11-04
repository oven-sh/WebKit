import type { MemberDeclaration, SignatureDeclaration, SourcedDeclaration } from "./declarations.ts";
import * as b from "./lib.ts";
import type { ModelOperand, TableModel } from "./model.ts";
import type { BuiltinPath, DirectParameterSpec, DirectValue } from "./vocabulary.ts";

const kindsInRanges: Partial<Record<DirectValue, DirectValue>> = {
  Bytes: "BytesInRange",
  BytesOfReceiver: "BytesOfReceiverInRange",
};

const kindsTakingReceiver: readonly DirectValue[] = ["NativeObjectOfReceiver", "Receiver", "BytesOfReceiver"];

// The compiler writes integers that are not negative here. An end beyond the view stands for the end of the view, whatever its length is when the call runs.
const boundInBytes = b.any.parameter;

function signatureInRanges(signature: SignatureDeclaration): SignatureDeclaration[] {
  const { directEntry } = signature;
  const numberOfViews = directEntry?.parameters.filter(({ value }) => value in kindsInRanges).length ?? 0;
  if (directEntry === undefined || numberOfViews === 0) return [];
  const bounds = Array.from({ length: numberOfViews }, (_, view) => [`startOfView${view}`, `endOfView${view}`]).flat();
  // A receiver that the C function does not take was tested by whoever picked the overload. The helper has none.
  const takesReceiver = directEntry.parameters.some(({ value }) => kindsTakingReceiver.includes(value));
  const { this: usesOfReceiver, ...usesOfArguments } = signature.uses ?? {};
  return [
    {
      ...signature,
      isSpecialCase: undefined,
      receiver: takesReceiver ? signature.receiver : null,
      parameters: [...signature.parameters, ...bounds.map(name => ({ name, type: boundInBytes }))],
      uses: {
        ...(takesReceiver ? { this: usesOfReceiver } : {}),
        ...usesOfArguments,
        ...Object.fromEntries(bounds.map(name => [name, b.inspected])),
      },
      directEntry: {
        ...directEntry,
        parameters: directEntry.parameters.map(parameter => ({
          ...parameter,
          value: kindsInRanges[parameter.value] ?? parameter.value,
        })),
      },
    },
  ];
}

const nameInRanges = (path: BuiltinPath) => `${path.replace(/[^A-Za-z0-9]+/g, "_")}_inRanges`;

// A native function that takes the bytes of a view does not care whether they are all of a view or a part of one: it gets an address and a count either way.
// So each row with such a direct entry is declared once more as a helper of the compiler: the same operands, uses, effects, result and C function, and behind the operands where the part of each view starts and ends.
// Compiled code calls the helper where the program passes `view.subarray(..)` to the function and does nothing else with it. That view is then never made.
export function declareFormsInRanges(declarations: readonly SourcedDeclaration[]): SourcedDeclaration[] {
  return declarations.flatMap(({ declaration, source }) => {
    if (declaration.declares !== "Member") return [];
    const signatures = declaration.signatures.flatMap(signatureInRanges);
    if (signatures.length === 0) return [];
    const path: BuiltinPath = `%CompilerHelpers%.${nameInRanges(declaration.path)}`;
    const helper: MemberDeclaration = {
      declares: "Member",
      path,
      kind: "Function",
      isOwnPropertyOfInstances: false,
      isLazilyCreated: false,
      signatures,
      dispatches: [],
      constant: null,
      length: null,
      columns: {},
    };
    return [{ declaration: helper, source }];
  });
}

const takesBytes = ({ value }: DirectParameterSpec) => value in kindsInRanges;

// Both overloads enter the same C function.
export function linkFormsInRanges({ overloads }: Pick<TableModel, "overloads">): void {
  const helpers = new Map(
    overloads.flatMap(overload => {
      const { directEntry } = overload.code;
      const isHelper = directEntry?.parameters.some(({ value }) => Object.values(kindsInRanges).includes(value));
      return isHelper ? [[directEntry!.symbol, overload] as const] : [];
    }),
  );
  for (const overload of overloads) {
    const { directEntry } = overload.code;
    const helper = directEntry?.parameters.some(takesBytes) ? helpers.get(directEntry.symbol) : undefined;
    if (directEntry === null || helper === undefined) continue;
    let numberOfArguments = 0;
    const views = directEntry.parameters.flatMap((parameter): ModelOperand[] => {
      if (parameter.value === "BytesOfReceiver") return [{ operand: "Receiver" }];
      if (parameter.value === "Realm" || kindsTakingReceiver.includes(parameter.value)) return [];
      const index = numberOfArguments++;
      return takesBytes(parameter) ? [{ operand: "Argument", index }] : [];
    });
    overload.code.inRanges = { overload: helper, views };
    // The helper stands for one overload of the function, which refuses only what none of its overloads takes.
    helper.parameters = helper.parameters.map((parameter, index) =>
      overload.parameters[index]?.roles.includes("DescribedWhenRefused") === false
        ? { ...parameter, roles: parameter.roles.filter(role => role !== "DescribedWhenRefused") }
        : parameter,
    );
  }
}
