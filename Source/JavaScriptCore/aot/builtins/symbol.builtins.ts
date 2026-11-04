import * as b from "bun-builtins-table";

const symbolObject = b.instance("Symbol");
const symbolOrObject = b.symbol.or(symbolObject);
const description = b.union(b.string, b.undefined);

const symbolValue = [
  {
    this: b.symbol,
    returns: b.operand("this"),
    capability: "none",
    implementation: b.inline("ReturnThis"),
  },
  {
    this: symbolObject,
    returns: b.symbol,
    capability: "heap",
    uses: { this: b.inspected },
    implementation: b.hostCall,
  },
] satisfies b.FunctionField[];

export default b.declarations(
  b.class("Symbol", {
    isPrimitiveWrapper: true,
    types: [b.viewType("Symbol")],
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    constructorTable: b.staticTable("runtime/SymbolConstructor.cpp", "symbolConstructorTable"),
    prototypeTable: b.staticTable("runtime/SymbolPrototype.cpp", "symbolPrototypeTable"),
    call: {
      args: { description: b.string.loose.optional },
      returns: b.symbol.fresh,
      capability: "heap",
      implementation: b.hostCall,
    },
    construct: {
      returns: b.never,
      capability: "none",
      throws: true,
      implementation: b.hostCall,
    },
    static: {
      for: {
        args: { key: b.string.loose },
        returns: b.symbol,
        capability: "heap",
        implementation: b.hostCall,
      },
      keyFor: {
        args: { symbol: b.symbol },
        returns: description,
        capability: "heap",
        implementation: b.hostCall,
      },
      asyncDispose: b.wellKnownSymbol,
      asyncIterator: b.wellKnownSymbol,
      dispose: b.wellKnownSymbol,
      hasInstance: b.wellKnownSymbol,
      isConcatSpreadable: b.wellKnownSymbol,
      iterator: b.wellKnownSymbol,
      match: b.wellKnownSymbol,
      matchAll: b.wellKnownSymbol,
      replace: b.wellKnownSymbol,
      search: b.wellKnownSymbol,
      species: b.wellKnownSymbol,
      split: b.wellKnownSymbol,
      toPrimitive: b.wellKnownSymbol,
      toStringTag: b.wellKnownSymbol,
      unscopables: b.wellKnownSymbol,
    },
    proto: {
      description: {
        this: symbolOrObject,
        getter: description,
        capability: "heap",
        uses: { this: b.inspected },
        implementation: b.generic,
      },
      toString: {
        this: symbolOrObject,
        returns: b.string,
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      valueOf: symbolValue,
      "@@toPrimitive": symbolValue,
    },
  }),
);
