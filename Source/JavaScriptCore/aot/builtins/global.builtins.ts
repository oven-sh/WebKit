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

  // JSGlobalObject::init() asks an option before it puts each of these on the global object: useExplicitResourceManagement, useSharedArrayBuffer, useTemporal, useWasm.
  [
    "AsyncDisposableStack",
    "DisposableStack",
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
  // JSWebAssembly::finishCreation() adds these two only if the host's method table has the hook.
  ["WebAssembly.compileStreaming", "WebAssembly.instantiateStreaming"].flatMap(path =>
    b.extend(path, { marks: ["MayBeAbsent"] }),
  ),
);
