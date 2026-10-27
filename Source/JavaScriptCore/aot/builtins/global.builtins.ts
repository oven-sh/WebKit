import * as b from "bun-builtins-table";

const numberTest = (inlineBuiltin: string) =>
  ({
    args: { number: b.f64.loose },
    returns: b.bool,
    capability: "none",
    deterministic: true,
    implementation: b.inline(inlineBuiltin),
  }) satisfies b.FunctionField;

const stringCoding = (parameter: string, usesFeature: string, throws: boolean) =>
  ({
    args: { [parameter]: b.string.loose },
    returns: b.string,
    capability: "heap",
    throws,
    uses: { [parameter]: [b.readAsCharacters] },
    deterministic: true,
    usesFeature,
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const realmFeatures = ["ShadowRealm", "Interpreter", "BuiltinSourceText"];
const wrappedValue = b.union(b.primitiveValue, b.callable);

export default b.declarations(
  b.namespace(
    "globalThis",
    {},
    { isGlobalObject: true, table: b.staticTable("runtime/JSGlobalObject.cpp", "globalObjectTable") },
  ),

  b.static("undefined", b.constant(undefined)),
  b.static("NaN", b.constant(b.checked(NaN, "PNaN"))),
  b.static("Infinity", b.constant(b.checked(Infinity, "std::numeric_limits<double>::infinity()"))),

  b.name("default", "module.exports", "require", "children", "parent", "__esModule", "callee"),
  b.name("configurable", "done", "enumerable", "errors", "writable"),
  b.name("app", "fetch", "routes", "stop"),

  b.fn("isNaN", { ...numberTest("IsNaN"), boxedCallStub: "IsNaN" }),
  b.fn("isFinite", numberTest("IsFinite")),
  b.fn("parseFloat", {
    args: { string: b.string.loose },
    returns: b.f64,
    capability: "heap",
    uses: { string: [b.readAsCharacters] },
    deterministic: true,
    implementation: b.hostCall,
  }),
  b.fn("parseInt", {
    args: { string: b.string.loose, radix: b.i32.wrap.optional },
    returns: b.f64,
    capability: "heap",
    throws: true,
    uses: { string: [b.readAsCharacters] },
    deterministic: true,
    implementation: b.inline("ParseInt", { otherwise: b.hostCall }),
  }),

  b.fn("encodeURI", stringCoding("uri", "URICoding", true)),
  b.fn("encodeURIComponent", stringCoding("uriComponent", "URICoding", true)),
  b.fn("decodeURI", stringCoding("encodedURI", "URICoding", true)),
  b.fn("decodeURIComponent", stringCoding("encodedURIComponent", "URICoding", true)),
  b.fn("escape", stringCoding("string", "EscapeUnescape", false)),
  b.fn("unescape", stringCoding("string", "EscapeUnescape", false)),

  b.fn("eval", [
    {
      args: { x: b.string },
      returns: b.any,
      capability: "js",
      throws: true,
      uses: { x: [b.evaluatedAsText] },
      overloadUsesFeature: "Interpreter",
      implementation: b.generic,
    },
    {
      args: { x: b.any.optional },
      returns: b.union(b.operand("x"), b.undefined),
      capability: "none",
      uses: { x: b.inspected },
      implementation: b.generic,
    },
  ]),

  b.class("ShadowRealm", {
    jsType: "ShadowRealmType",
    usesFeature: realmFeatures,
    nativeSymbols: ["callShadowRealm", "constructWithShadowRealmConstructor"],
    prototypeTable: b.staticTable("runtime/ShadowRealmPrototype.cpp", "shadowRealmPrototypeTable"),
    // ShadowRealmObject::finishCreation() puts the tag on each realm, besides the one on the prototype.
    namesDefinedOnNewInstances: ["@@toStringTag"],
    call: { returns: b.never, capability: "heap", throws: true, implementation: b.hostCall },
    construct: {
      returns: b.instance("ShadowRealm").fresh,
      capability: "js",
      throws: true,
      implementation: b.hostCall,
    },
    proto: {
      evaluate: {
        args: { sourceText: b.string },
        returns: wrappedValue,
        capability: "js",
        throws: true,
        uses: { this: b.inspected, sourceText: [b.evaluatedAsText] },
        usesFeature: realmFeatures,
        nativeSymbols: ["evalInRealm"],
        marks: ["EvaluatesTextInGlobalScope"],
        implementation: b.compiled("ShadowRealmPrototype", "evaluate"),
      },
      importValue: {
        args: { specifier: b.string.loose, exportName: b.string },
        returns: b.instance("Promise"),
        capability: "js",
        throws: true,
        uses: { this: b.inspected },
        usesFeature: realmFeatures,
        nativeSymbols: ["importInRealm"],
        implementation: b.compiled("ShadowRealmPrototype", "importValue"),
      },
    },
  }),

  // JSGlobalObject::init() asks an option before it puts each of these on the global object: useExplicitResourceManagement, useShadowRealm, useSharedArrayBuffer, useTemporal, useWasm.
  [
    "AsyncDisposableStack",
    "DisposableStack",
    "ShadowRealm",
    "SharedArrayBuffer",
    "SuppressedError",
    "Temporal",
    "WebAssembly",
  ].flatMap(path => b.extend(path, { marks: ["MayBeAbsent"] })),
  // And each of these on its holder: useIteratorSequencing, useJointIteration, useIteratorChunking, useJSPI.
  [
    "Iterator.concat",
    "Iterator.zip",
    "Iterator.zipKeyed",
    "Iterator.prototype.chunks",
    "Iterator.prototype.windows",
    "WebAssembly.promising",
    "WebAssembly.Suspending",
    "WebAssembly.SuspendError",
  ].flatMap(path => b.extend(path, { marks: ["MayBeAbsent"] })),
);
