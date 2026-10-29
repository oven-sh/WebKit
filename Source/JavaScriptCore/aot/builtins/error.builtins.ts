import * as b from "bun-builtins-table";

type ErrorOperands = Pick<b.SignatureOptions, "args" | "uses" | "implementation">;

const nativeErrorNames = ["EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"];

const message = b.string.loose.optional;
const keepsCause = [b.readByKey(b.keysNamed("cause"), b.nativeMemory), b.contentsStoredIn(b.nativeMemory)];
const header = b.named("name", "message");

// A constructor notes where it was called and calls nothing for that: Error.prepareStackTrace runs at the first read of `stack`, and a getter under Error.stackTraceLimit never.
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

// The realm creates each prototype with the name of the class and an empty message.
const textsOnPrototype = { name: b.property(b.string), message: b.property(b.string) };

const errorSubclass = (className: string, signature: b.FunctionField, namesDefinedOnNewInstances: readonly string[] = []) =>
  b.class(className, {
    extends: "Error",
    namesDefinedOnNewInstances,
    constructorOnlyReadsPrototypeOfNewTarget: true,
    types: [b.viewType(className)],
    call: signature,
    construct: signature,
    proto: textsOnPrototype,
  });

export default b.declarations(
  b.class("Error", {
    jsType: "ErrorInstanceType",
    // The last two are the host's, which maps positions back to sources.
    namesDefinedOnNewInstances: ["message", "cause", "stack", "line", "column", "sourceURL", "originalLine", "originalColumn"],
    // The constructor converts the message to a string, and the engine notes where the error was made. `cause` is whatever the options hold.
    typesOfNamesDefinedOnNewInstances: b.typesByName({
      message: b.string,
      stack: b.string,
      line: b.f64,
      column: b.f64,
      sourceURL: b.string,
      originalLine: b.f64,
      originalColumn: b.f64,
    }),
    // The error makes `stack` at the first read: a text, or what Error.prepareStackTrace answers for the error and its call sites.
    namesMadeByStaticMembers: { stack: "prepareStackTrace" },
    // The text begins with them, and so does what is printed for an error.
    namesReadToDescribeInstances: ["name", "message"],
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
          // A host that maps positions back to sources notes where the first frame was before.
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
    // The constructor defines the message, if it is given one, and then the cause. A program may store anything under either name.
    // `stack` is no slot: the error makes it at the first read.
    instance: {
      message: b.ownSlot(b.any, { structure: "errorStructure", after: [] }),
      cause: b.ownSlot(b.any, { structure: "errorStructure", after: ["message"] }),
    },
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

  ...nativeErrorNames.map(name => errorSubclass(name, messageSignature(name))),

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
);
