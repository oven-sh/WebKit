import * as b from "bun-builtins-table";

type ErrorOperands = Pick<b.SignatureOptions, "args" | "uses" | "implementation">;

const nativeErrorNames = ["EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"];

const message = b.string.loose.optional;
const keepsCause = [b.readByKey(b.keysNamed("cause"), b.nativeMemory), b.contentsStoredIn(b.nativeMemory)];
const header = b.named("name", "message");

const errorSignature = (className: string, operands: ErrorOperands) =>
  ({
    ...operands,
    returns: b.instance(className).unaliased,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
  }) satisfies b.FunctionField;

const messageSignature = (className: string) =>
  errorSignature(className, {
    args: { message, options: b.any.optional },
    uses: { options: keepsCause },
    implementation: b.inline("NewError", { parameters: [className], otherwise: b.hostCall }),
  });

const errorSubclass = (className: string, signature: b.FunctionField, namesDefinedOnNewInstances: readonly string[] = []) =>
  b.class(className, {
    extends: "Error",
    namesDefinedOnNewInstances,
    types: [b.viewType(className)],
    call: signature,
    construct: signature,
  });

export default b.declarations(
  b.class("Error", {
    jsType: "ErrorInstanceType",
    // The last two are the host's, which maps positions back to sources.
    namesDefinedOnNewInstances: ["message", "cause", "stack", "line", "column", "sourceURL", "originalLine", "originalColumn"],
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
          // A host that maps positions back to sources notes where the first frame was before.
          target: [b.definesName(["stack", "originalLine", "originalColumn"], b.any), b.retained, b.runs(header)],
          constructorOpt: b.inspected,
        },
        implementation: b.generic,
      },
      isError: {
        args: { value: b.any },
        returns: b.bool,
        capability: "heap",
        uses: { value: b.inspected },
        implementation: b.hostCall,
      },
      stackTraceLimit: b.property(),
    },
    proto: {
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

  ...nativeErrorNames.map(name => errorSubclass(name, messageSignature(name))),

  errorSubclass(
    "AggregateError",
    errorSignature("AggregateError", {
      args: { errors: b.any, message, options: b.any.optional },
      uses: { errors: [b.runs("Iterate"), b.retained], options: keepsCause },
      implementation: b.hostCall,
    }),
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
);
