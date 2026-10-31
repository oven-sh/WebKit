import * as b from "bun-builtins-table";

const ownProperties = ["ObjectFields", "NamedProperties", "Elements", "ArrayStorage", "ShapeWords", "OrderWords"];
const inheritedProperties = [...ownProperties, "PrototypeLinks"];
const prototypeLink = ["ShapeWords", "NamedProperties", "PrototypeLinks"];
const memberGuards = ["MemberGuards"];
// An array under a prototype that may hold indexed accessors gets a storage that asks it at every store.
const prototypeChange = [...prototypeLink, ...memberGuards, "ArrayStorage"];

const objectOrNull = b.union(b.object, b.strictNull);
const nonNullishPrimitive = b.union(b.bool, b.f64, b.string, b.symbol, b.bigint);

const wrapperClasses = [
  [b.bool, "Boolean"],
  [b.f64, "Number"],
  [b.string, "String"],
  [b.symbol, "Symbol"],
  [b.bigint, "BigInt"],
] as const;

const wrapperOverloads = (overloadOf: (primitive: b.TableType, wrapper: b.ClassName) => b.FunctionField) =>
  wrapperClasses.map(([primitive, wrapper]) => overloadOf(primitive, wrapper));

const objectConversion = (forObject: b.ImplementationSpec) =>
  [
    {
      args: { value: b.object },
      returns: b.operand("value"),
      capability: "none",
      uses: { value: b.inspected },
      implementation: forObject,
    },
    {
      args: { value: b.null },
      returns: b.instance("Object").fresh,
      capability: "heap",
      implementation: b.hostCall,
    },
    ...wrapperOverloads((primitive, wrapper) => ({
      args: { value: primitive },
      returns: b.instance(wrapper).fresh,
      capability: "heap",
      implementation: b.hostCall,
    })),
  ] satisfies b.FunctionField[];

const ownKeyList = (kind: "Keys" | "Names" | "Symbols", key: b.TableType) =>
  ({
    args: { object: b.object.loose },
    returns: b.instance("Array").holding({ element: key }).fresh.dense,
    capability: "js",
    throws: b.propagated,
    runsOnlyOperandCode: true,
    uses: { object: [b.runs("OwnKeys")] },
    reads: ownProperties,
    privateAlias: true,
    implementation: b.inline("OwnPropertyKeys", { parameters: [kind], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const integrityChange = (level: b.IntegrityLevel, implementation: b.ImplementationSpec) =>
  ({
    args: { object: b.any },
    returns: b.operand("object"),
    capability: "js",
    throws: b.propagated,
    runsOnlyOperandCode: true,
    uses: { object: [b.integrityRaisedTo(level), ...b.ownPropertyTraps] },
    reads: ownProperties,
    writes: [...ownProperties, ...memberGuards],
    implementation,
  }) satisfies b.FunctionField;

// A revoked Proxy throws, and so does one whose trap says what its target rules out.
const integrityTest = {
  args: { object: b.any },
  returns: b.bool,
  capability: "js",
  throws: true,
  runsOnlyOperandCode: true,
  uses: { object: b.ownPropertyTraps },
  reads: ownProperties,
  implementation: b.hostCall,
} satisfies b.FunctionField;

// An object without properties that cannot be extended is sealed and frozen.
// The properties are looked at one by one, and an export of a module namespace object that is still in its dead zone throws.
const integrityTestOfProperties = {
  ...integrityTest,
  throws: true,
  uses: { object: [b.testedByKey(b.anyKey), ...b.ownPropertyTraps] },
} satisfies b.FunctionField;

const ownPropertyTest = (inlineBuiltin: string) =>
  ({
    this: b.any,
    args: { key: b.propertyKey.loose },
    returns: b.bool,
    capability: "js",
    throws: b.propagated,
    runsOnlyOperandCode: true,
    uses: { this: [b.testedByKey(b.keyFrom("key")), ...b.ownPropertyTraps] },
    reads: ownProperties,
    implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

// What a descriptor holds comes from the object: the value of the property, or the two functions of an accessor.
// A descriptor holds the value or the two functions of the property that it describes.
const describedPart = b.resultFieldHoldingOnlyWhatIsStored;

const describedBy = (key: b.KeySpec, value: b.PlaceSpec, getter: b.PlaceSpec, setter: b.PlaceSpec) => [
  b.readByKey(key, value),
  b.accessorReadByKey("Getter", key, getter),
  b.accessorReadByKey("Setter", key, setter),
];

const accessorDefinition = (accessor: "Getter" | "Setter", nativeSymbol: string) =>
  ({
    this: b.object.loose.refusing(b.null),
    args: { key: b.propertyKey.loose, accessor: b.callable },
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: {
      this: [b.definesAccessor(accessor, b.keyFrom("key"), b.operand("accessor")), ...b.ownPropertyTraps],
      accessor: b.inspected,
    },
    reads: ownProperties,
    writes: memberGuards,
    usesFeature: "LegacyAccessors",
    nativeSymbols: [nativeSymbol],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

// It answers the function that it finds and no other: the cell stands for the way from the property to the answer.
const accessorLookup = (accessor: "Getter" | "Setter", nativeSymbol: string) =>
  ({
    this: b.object.loose.refusing(b.null),
    args: { key: b.propertyKey.loose },
    returns: b.union(b.callable, b.undefined),
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: {
      this: [
        b.testedByKey(b.keyFrom("key")),
        b.accessorReadByKey(accessor, b.keyFrom("key"), b.nativeCell(`Object.lookedUp${accessor}s`)),
        b.returnedFrom(`Object.lookedUp${accessor}s`),
        ...b.ownPropertyTraps,
        b.runs("PrototypeChain"),
      ],
    },
    reads: inheritedProperties,
    usesFeature: "LegacyAccessors",
    nativeSymbols: [nativeSymbol],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

export default b.declarations(
  b.class("Object", {
    isNeverPristine: true,
    lockedObjects: ["prototype"],
    lockedMembers: ["constructor"],
    mutability: "AlwaysLocked",
    constructorTable: b.staticTable("runtime/ObjectConstructor.cpp", "objectConstructorTable"),
    call: objectConversion(b.inline("CallObjectConstructor", { otherwise: b.hostCall })),
    // What `new Object(value)` does. Where new.target is another function (`super(value)` in a class below Object, Reflect.construct(Object, [value], Other)) the language
    // ignores the value and makes an ordinary object below new.target.prototype: the row for `null`, whatever is passed. Rows have no word for new.target:
    // the resolver of calls takes a construction only where new.target is the callee, and flow asks overloadForAnotherNewTarget() (valueflow/AOTValueFlowBuiltins.h).
    construct: objectConversion(b.hostCall),
    static: {
      assign: [
        {
          args: { target: b.object, sources: b.any.rest },
          returns: b.operand("target"),
          capability: "js",
          throws: b.propagated,
          runsOnlyOperandCode: true,
          uses: { target: [b.assignedFrom("sources")], sources: b.inspected },
          reads: inheritedProperties,
          writes: memberGuards,
          implementation: b.inline("ObjectAssign", { otherwise: b.hostCall }),
        },
        ...wrapperOverloads((primitive, wrapper) => ({
          args: { target: primitive, sources: b.any.rest },
          returns: b.instance(wrapper).unaliased,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: {
            target: [b.storedByKey(b.anyKey, b.ownValues("sources"))],
            sources: [b.runs("OwnKeys", "OwnValues"), b.contentsStoredIn(b.nativeMemory)],
          },
          implementation: b.hostCall,
        })),
      ],
      create: [
        {
          args: { prototype: objectOrNull, properties: b.undefined.optional },
          returns: b.instance("Object").fresh,
          capability: "heap",
          uses: { prototype: [b.prototypeOf(b.result)] },
          privateAlias: true,
          implementation: b.inline("ObjectCreate", { otherwise: b.hostCall }),
        },
        {
          args: { prototype: objectOrNull, properties: b.object.loose },
          returns: b.instance("Object").unaliased,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { prototype: [b.prototypeOf(b.result)], properties: [b.describesResult] },
          implementation: b.hostCall,
        },
      ],
      defineProperties: {
        args: { object: b.object, properties: b.object.loose },
        returns: b.operand("object"),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { object: [b.definedByMap("properties"), ...b.ownPropertyTraps], properties: b.inspected },
        reads: ownProperties,
        writes: memberGuards,
        implementation: b.hostCall,
      },
      defineProperty: {
        args: { object: b.object, key: b.propertyKey.loose, descriptor: b.object },
        returns: b.operand("object"),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          object: [b.definedBy(b.keyFrom("key"), "descriptor"), ...b.ownPropertyTraps],
          descriptor: b.inspected,
        },
        reads: ownProperties,
        writes: memberGuards,
        privateAlias: true,
        implementation: b.hostCall,
      },
      entries: {
        args: { object: b.object.loose.refusing(b.null) },
        returns: b.instance("Array").holding({ element: b.layoutOf("EntriesPair") }).fresh.dense,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { object: [b.runs("OwnKeys", "OwnValues"), b.contentsStoredIn(b.nativeMemory)] },
        model: b.functionIn("models/Objects", "entries"),
        implementation: b.inline("ObjectEntries"),
      },
      freeze: integrityChange("Frozen", b.inline("ObjectFreeze", { otherwise: b.hostCall })),
      fromEntries: {
        args: { iterable: b.any },
        returns: b.instance("Object").unaliased,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          iterable: [
            b.runsTransitively("Iterate", "OwnValues", "PrototypeChain", "ToPrimitive"),
            b.reachableValuesStoredIn(b.nativeMemory),
          ],
        },
        model: b.functionIn("models/Objects", "fromEntries"),
        implementation: b.compiled("ObjectConstructor", "fromEntries"),
      },
      getOwnPropertyDescriptor: {
        args: { object: b.object.loose, key: b.propertyKey.loose },
        returns: b.union(b.ordinaryObjectOf("PropertyDescriptor"), b.undefined).fresh,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: {
          object: [
            ...describedBy(b.keyFrom("key"), describedPart("value"), describedPart("get"), describedPart("set")),
            ...b.ownPropertyTraps,
          ],
        },
        reads: ownProperties,
        privateAlias: true,
        fieldsForm: "OwnPropertyDescriptorFields",
        implementation: b.inline("ObjectGetOwnPropertyDescriptor", { otherwise: b.hostCall }),
      },
      getOwnPropertyDescriptors: {
        args: { object: b.object.loose },
        returns: b.instance("Object").unaliased,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: {
          object: [
            ...describedBy(b.anyKey, b.nativeMemory, b.nativeMemory, b.nativeMemory),
            b.contentsStoredIn(b.nativeMemory),
            b.describedByResult,
            b.runs("OwnKeys"),
          ],
        },
        reads: ownProperties,
        model: b.functionIn("models/Objects", "getOwnPropertyDescriptors"),
        implementation: b.hostCall,
      },
      getOwnPropertyNames: ownKeyList("Names", b.string),
      getOwnPropertySymbols: ownKeyList("Symbols", b.symbol),
      getPrototypeOf: [
        {
          args: { object: b.object },
          returns: b.prototype("object"),
          capability: "js",
          runsOnlyOperandCode: true,
          uses: { object: [b.runs("PrototypeChain")] },
          reads: prototypeLink,
          privateAlias: true,
          boxedCallStub: "ObjectGetPrototypeOf",
          implementation: b.inline("GetPrototypeOf", { parameters: ["ObjectGetPrototypeOf"], otherwise: b.hostCall }),
        },
        ...wrapperOverloads((primitive, wrapper) => ({
          args: { object: primitive },
          returns: b.builtin(`${wrapper}.prototype`),
          capability: "heap",
          implementation: b.hostCall,
        })),
      ],
      hasOwn: {
        args: { object: b.object.loose, key: b.propertyKey.loose },
        returns: b.bool,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { object: [b.testedByKey(b.keyFrom("key")), ...b.ownPropertyTraps] },
        reads: ownProperties,
        privateAlias: true,
        implementation: b.inline("ObjectHasOwn", { otherwise: b.hostCall }),
      },
      is: {
        args: { value1: b.any, value2: b.any },
        returns: b.bool,
        capability: "heap",
        uses: { value1: b.inspected, value2: b.inspected },
        boxedCallStub: "ObjectIs",
        implementation: b.inline("ObjectIs", { otherwise: b.hostCall }),
      },
      isExtensible: integrityTest,
      isFrozen: integrityTestOfProperties,
      isSealed: integrityTestOfProperties,
      keys: ownKeyList("Keys", b.string),
      preventExtensions: integrityChange("NonExtensible", b.hostCall),
      seal: integrityChange("Sealed", b.hostCall),
      setPrototypeOf: {
        args: { object: b.any.refusing(b.null), prototype: objectOrNull },
        returns: b.operand("object"),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { object: [b.prototypeSetTo("prototype"), b.runs("PrototypeChain")], prototype: b.inspected },
        reads: prototypeLink,
        writes: prototypeChange,
        boxedCallStub: "ObjectSetPrototypeOf",
        implementation: b.inline("SetPrototypeOf", { parameters: ["ObjectSetPrototypeOf"], otherwise: b.hostCall }),
      },
      values: [
        {
          args: { object: b.object },
          returns: b.instance("Array").fresh.dense,
          capability: "js",
          throws: b.propagated,
          runsOnlyOperandCode: true,
          uses: { object: [b.runs("OwnKeys", "OwnValues"), b.contentsStoredIn(b.resultSlot("element"))] },
          privateAlias: true,
          implementation: b.inline("ObjectValues"),
        },
        {
          args: { object: nonNullishPrimitive },
          returns: b.instance("Array").holding({ element: b.string }).fresh.dense,
          capability: "heap",
          implementation: b.hostCall,
        },
      ],
    },
    proto: {
      __defineGetter__: accessorDefinition("Getter", "objectProtoFuncDefineGetter"),
      __defineSetter__: accessorDefinition("Setter", "objectProtoFuncDefineSetter"),
      __lookupGetter__: accessorLookup("Getter", "objectProtoFuncLookupGetter"),
      __lookupSetter__: accessorLookup("Setter", "objectProtoFuncLookupSetter"),
      hasOwnProperty: ownPropertyTest("ObjectHasOwn"),
      isPrototypeOf: {
        this: b.any,
        args: { value: b.any },
        returns: b.bool,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { this: b.inspected, value: [b.runs("PrototypeChain")] },
        reads: prototypeLink,
        implementation: b.hostCall,
      },
      propertyIsEnumerable: ownPropertyTest("ObjectPropertyIsEnumerable"),
      toLocaleString: {
        this: b.any.refusing(b.null),
        returns: b.any,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: [b.runs(b.calls("toString"))] },
        reads: inheritedProperties,
        model: b.functionIn("models/Objects", "toLocaleString"),
        implementation: b.hostCall,
      },
      toString: [
        {
          this: b.object,
          returns: b.string,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [b.runs(b.named("@@toStringTag")), b.membersOnlyInspected] },
          reads: inheritedProperties,
          implementation: b.inline("ObjectPrototypeToString", { otherwise: b.hostCall }),
        },
        {
          this: b.null,
          returns: b.string,
          capability: "heap",
          implementation: b.hostCall,
        },
        {
          this: nonNullishPrimitive,
          returns: b.string,
          capability: "heap",
          implementation: b.hostCall,
        },
      ],
      valueOf: [
        {
          this: b.object,
          returns: b.operand("this"),
          capability: "none",
          uses: { this: b.inspected },
          implementation: b.inline("ReturnThis"),
        },
        ...wrapperOverloads((primitive, wrapper) => ({
          this: primitive,
          returns: b.instance(wrapper).fresh,
          capability: "heap",
          implementation: b.hostCall,
        })),
      ],
    },
  }),

  b.method("Object.prototype.__proto__", {
    this: b.object.loose.refusing(b.null),
    getter: b.prototype("this"),
    capability: "js",
    runsOnlyOperandCode: true,
    uses: { this: [b.runs("PrototypeChain")] },
    reads: prototypeLink,
    implementation: b.generic,
    setter: {
      this: b.any.refusing(b.null),
      value: b.any,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { this: [b.prototypeSetTo("value"), b.runs("PrototypeChain")], value: b.inspected },
      reads: prototypeLink,
      writes: prototypeChange,
      implementation: b.generic,
    },
  }),
);
