import * as b from "bun-builtins-table";

const serialized = b.runsTransitively("OwnKeys", "OwnValues", "ToJSON", "ToPrimitive");

export default b.declarations(
  b.namespace(
    "JSON",
    {
      parse: [
        {
          args: { text: b.string.loose, reviver: b.primitiveValue },
          returns: b.union(b.plainData, b.strictNull, b.bool, b.f64, b.string),
          capability: "heap",
          throws: true,
          uses: { text: [b.readAsCharacters] },
          typedResultForm: { form: "ParsedText", source: "text" },
          implementation: b.hostCall,
        },
        {
          args: { text: b.string.loose, reviver: b.any },
          returns: b.any,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: {
            text: [b.readAsCharacters],
            reviver: [
              // The walk goes into whatever the reviver has put into a holder in the meantime, and deletes and defines there.
              b.called({
                afterwards: "WritesIntoWhatItFinds",
                times: "Repeatedly",
                this: b.object,
                args: [b.string, b.any, b.object.fresh],
                result: "Escapes",
              }),
            ],
          },
          implementation: b.hostCall,
        },
      ],
      stringify: [
        {
          args: { value: b.any, replacer: b.null, space: b.any.optional },
          returns: b.union(b.string, b.undefined),
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { value: [serialized], space: [b.runs("ToPrimitive")] },
          boxedCallStub: "JSONStringify",
          implementation: b.hostCall,
        },
        {
          args: { value: b.any, replacer: b.callable, space: b.any.optional },
          returns: b.union(b.string, b.undefined),
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: {
            value: [serialized, b.retained, b.reachableValuesStoredIn(b.nativeMemory)],
            replacer: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Repeatedly", this: b.object, args: [b.string, b.any], result: "Escapes" })],
            space: [b.runs("ToPrimitive")],
          },
          implementation: b.hostCall,
        },
        {
          args: { value: b.any, replacer: b.any, space: b.any.optional },
          returns: b.union(b.string, b.undefined),
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: {
            value: [serialized, b.readByKey(b.keysIn("replacer"), b.nativeMemory)],
            replacer: [b.runsTransitively("OwnValues", "ToPrimitive", b.named("length"))],
            space: [b.runs("ToPrimitive")],
          },
          implementation: b.hostCall,
        },
      ],
      rawJSON: {
        args: { text: b.string.loose },
        returns: b.object.fresh,
        capability: "heap",
        throws: true,
        usesFeature: "JSONRawJSON",
        nativeSymbols: ["jsonProtoFuncRawJSON"],
        implementation: b.hostCall,
      },
      isRawJSON: {
        args: { value: b.any },
        returns: b.bool,
        capability: "heap",
        uses: { value: b.inspected },
        implementation: b.hostCall,
      },
    },
    { locksMembers: true, mutability: "AlwaysLocked", table: b.staticTable("runtime/JSONObject.cpp", "jsonTable") },
  ),
  // Tests spy on them, and libraries that revive dates or big integers wrap them on request (jju).
  b.replacedByPackages("JSON.parse", "JSON.stringify"),
);
