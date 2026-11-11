import * as b from "bun-builtins-table";

type ErrorOperands = Pick<b.SignatureOptions, "args" | "uses" | "implementation">;

const nativeErrorNames = ["EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"];

const message = b.string.loose.optional;
const keepsCause = [b.readByKey(b.keysNamed("cause"), b.nativeMemory), b.contentsStoredIn(b.nativeMemory)];
const header = b.named("name", "message");

const errorSignature = (className: string, operands: ErrorOperands, throws: b.SignatureOptions["throws"] = b.propagated) =>
  ({
    ...operands,
    returns: b.instance(className).unaliased,
    capability: "js",
    throws,
    runsOnlyOperandCode: true,
  }) satisfies b.FunctionField;

const messageSignature = (className: string) =>
  errorSignature(className, {
    args: { message, options: b.any.optional },
    uses: { options: keepsCause },
    implementation: b.inline("NewError", { parameters: [className], otherwise: b.hostCall }),
  });

const ownPropertyOfNewErrors = (name: string, laidByConstructor: b.TableType, structure: string, after: readonly string[]) =>
  ({
    ...b.ownSlot(laidByConstructor, { structure, after }),
    resultIsDeclaredOnly: true,
    uses: { this: [b.readByKey(b.keysNamed(name)), b.runs("PrototypeChain")] },
  }) satisfies b.GetterField;

const ownPropertiesOfNewErrors = (structure: string) => ({
  message: ownPropertyOfNewErrors("message", b.string, structure, []),
  cause: ownPropertyOfNewErrors("cause", b.undefined, structure, ["message"]),
});

const textsOnPrototype = { name: b.property(b.string), message: b.property(b.string) };

type ErrorMembers = Pick<b.ClassOptions, "instance" | "proto">;

const errorSubclass = (
  className: string,
  signature: b.FunctionField,
  namesDefinedOnNewInstances: readonly string[] = [],
  members: ErrorMembers = { proto: textsOnPrototype },
) =>
  b.class(className, {
    extends: "Error",
    namesDefinedOnNewInstances,
    constructorOnlyReadsPrototypeOfNewTarget: true,
    types: [b.viewType(className)],
    call: signature,
    construct: signature,
    ...members,
  });

const nativeError = (className: string) =>
  errorSubclass(className, messageSignature(className), [], {
    instance: ownPropertiesOfNewErrors(`errorStructureWithErrorType<ErrorType::${className}>`),
    proto: { name: b.property(b.string) },
  });

export default b.declarations(
  b.class("Error", {
    jsType: "ErrorInstanceType",
    namesDefinedOnNewInstances: ["message", "cause", "stack", "line", "column", "sourceURL", "originalLine", "originalColumn"],
    constructorOnlyReadsPrototypeOfNewTarget: true,
    types: [b.viewType("Error")],
    prototypeTable: b.staticTable("runtime/ErrorPrototype.cpp", "errorPrototypeTable"),
    call: messageSignature("Error"),
    construct: messageSignature("Error"),
    static: {
      captureStackTrace: {
        args: { target: b.object, constructorOpt: b.any.optional },
        capability: "js",
        throws: true,
        uses: {
          target: [b.definesName(["stack", "originalLine", "originalColumn"], b.any), b.retained, b.runs(header)],
          constructorOpt: b.inspected,
        },
        implementation: b.generic,
      },
      isError: {
        args: { value: b.any },
        returns: b.bool,
        answersWhetherInstance: { value: b.instance("Error") },
        capability: "heap",
        uses: { value: b.inspected },
        implementation: b.hostCall,
      },
      stackTraceLimit: b.property(b.f64),
    },
    instance: ownPropertiesOfNewErrors("errorStructure"),
    proto: {
      name: b.property(b.string),
      toString: {
        this: b.object,
        returns: b.string,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: [b.runsTransitively(header, "ToPrimitive")] },
        implementation: b.hostCall,
      },
    },
  }),

  ...nativeErrorNames.map(nativeError),

  errorSubclass(
    "AggregateError",
    errorSignature(
      "AggregateError",
      {
        args: { errors: b.any, message, options: b.any.optional },
        uses: { errors: [b.runs("Iterate"), b.retained], options: keepsCause },
        implementation: b.hostCall,
      },
      true,
    ),
    ["errors"],
  ),

  errorSubclass(
    "SuppressedError",
    errorSignature("SuppressedError", {
      args: { error: b.any, suppressed: b.any, message },
      uses: { error: [b.retained], suppressed: [b.retained] },
      implementation: b.hostCall,
    }),
    ["error", "suppressed"],
  ),

  b.name("error", "suppressed"),
  b.extend("Error.stackTraceLimit", { canBeListed: true }),
);
