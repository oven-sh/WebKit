import * as b from "bun-builtins-table";

// The host of the programs that WebKit's own build links: jsc, testaot, testFFI. It has no natives and no front end.
export default b.declarations(
  b.namespace("%UnfilledHostRole%", {}),
  b.class("%UnfilledHostRoleClass%", { hasConstructor: false }),
  b.unfilledHostRoles("%UnfilledHostRole%", "UnfilledHostRoleClass"),
  b.namespace("console", {}),
  b.namespace("%UndeclaredNativeCode%", {}),
  // models/Objects.ts has a model of it.
  b.namespace("%PrivateGlobals%", {
    toClass: {
      args: { target: b.any, name: b.any.optional, base: b.any.optional },
      returns: b.undefined,
      capability: "js",
      throws: true,
      uses: {
        target: [b.everything, b.prototypeSetTo("base"), b.definesName(["prototype", "name"], b.any)],
        name: [b.everything],
        base: [b.everything],
      },
      model: b.functionIn("models/Objects", "toClass"),
      implementation: b.hostCall,
    },
  }),
  b.frontEndFunctions("prelude/array-search", "includes", "indexOf"),
  b.frontEndFunctions("prelude/array-sort", "sort", "toSorted"),
);
