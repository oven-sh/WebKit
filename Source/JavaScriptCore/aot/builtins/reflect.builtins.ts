import * as b from "bun-builtins-table";

const ownProperties = ["ObjectFields", "NamedProperties", "Elements", "ArrayStorage", "ShapeWords", "OrderWords"];
const inheritedProperties = [...ownProperties, "PrototypeLinks"];
const prototypeLink = ["ShapeWords", "NamedProperties", "PrototypeLinks"];
const memberGuards = ["MemberGuards"];
// An array under a prototype that may hold indexed accessors gets a storage that asks it at every store.
const prototypeChange = [...prototypeLink, ...memberGuards, "ArrayStorage"];

const operandCodeEffects = { capability: "js", runsOnlyOperandCode: true } as const;
const key = b.keyFrom("propertyKey");
const keyedArguments = { target: b.object, propertyKey: b.propertyKey.loose };
const argumentListProtocols = [b.runs("OwnValues", "ToPrimitive", b.named("length"))];
const listedArguments = b.ownValues("argumentsList");
const setterForReceiverCell = "Reflect.set.setter";

export default b.declarations(
  b.namespace(
    "Reflect",
    {
      apply: {
        ...operandCodeEffects,
        args: { target: b.callable, thisArgument: b.any, argumentsList: b.object },
        returns: b.callbackResult("target"),
        throws: true,
        uses: {
          target: [
            b.called({
              afterwards: "TouchesOnlyWhatRowNames",
              times: "Once",
              args: [],
              this: b.operand("thisArgument"),
              spreadArgs: listedArguments,
              result: "Returned",
            }),
          ],
          thisArgument: b.inspected,
          argumentsList: argumentListProtocols,
        },
        implementation: b.inline("ReflectApply", { otherwise: b.compiled("ReflectObject", "apply") }),
      },
      construct: [
        {
          ...operandCodeEffects,
          args: { target: b.any, argumentsList: b.any, newTarget: b.notPassed },
          returns: b.callbackResult("target"),
          throws: true,
          uses: {
            target: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", args: [], constructs: true, spreadArgs: listedArguments, result: "Returned" })],
            argumentsList: argumentListProtocols,
          },
          implementation: b.inline("ReflectConstruct", { otherwise: b.hostCall }),
        },
        {
          ...operandCodeEffects,
          args: { target: b.any, argumentsList: b.any, newTarget: b.any },
          returns: b.callbackResult("target"),
          throws: true,
          uses: {
            target: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", args: [], constructs: true, newTarget: b.operand("newTarget"), spreadArgs: listedArguments, result: "Returned" })],
            argumentsList: argumentListProtocols,
            newTarget: [b.runs(b.named("prototype"))],
          },
          implementation: b.inline("ReflectConstruct", { otherwise: b.hostCall }),
        },
      ],
      defineProperty: {
        ...operandCodeEffects,
        args: { ...keyedArguments, attributes: b.object },
        returns: b.bool,
        throws: true,
        uses: { target: [b.definedBy(key, "attributes"), ...b.ownPropertyTraps], attributes: b.inspected },
        reads: ownProperties,
        writes: memberGuards,
        implementation: b.hostCall,
      },
      deleteProperty: {
        ...operandCodeEffects,
        args: keyedArguments,
        returns: b.bool,
        throws: true,
        uses: { target: [b.deletedByKey(key), ...b.ownPropertyTraps] },
        reads: ownProperties,
        writes: memberGuards,
        implementation: b.hostCall,
      },
      get: [
        {
          ...operandCodeEffects,
          args: { ...keyedArguments, receiver: b.notPassed },
          returns: b.any,
          throws: true,
          uses: { target: [b.readByKey(key), b.runs("PrototypeChain")] },
          reads: inheritedProperties,
          implementation: b.compiled("ReflectObject", "get"),
        },
        {
          ...operandCodeEffects,
          args: { ...keyedArguments, receiver: b.any },
          returns: b.any,
          throws: true,
          // The getter that is found on the target runs with the receiver as `this`: it is called on what it does not hang above.
          uses: { target: [b.everything, b.readByKey(key), b.accessorReadByKey("Getter", key, b.nativeMemory)], receiver: [b.everything] },
          implementation: b.compiled("ReflectObject", "get"),
        },
      ],
      getOwnPropertyDescriptor: {
        ...operandCodeEffects,
        args: keyedArguments,
        returns: b.union(b.ordinaryObjectOf("PropertyDescriptor"), b.undefined).fresh,
        throws: true,
        uses: {
          target: [
            b.readByKey(key, b.resultFieldHoldingOnlyWhatIsStored("value")),
            b.accessorReadByKey("Getter", key, b.resultFieldHoldingOnlyWhatIsStored("get")),
            b.accessorReadByKey("Setter", key, b.resultFieldHoldingOnlyWhatIsStored("set")),
            ...b.ownPropertyTraps,
          ],
        },
        reads: ownProperties,
        fieldsForm: "OwnPropertyDescriptorFields",
        implementation: b.inline("ObjectGetOwnPropertyDescriptor", { otherwise: b.hostCall }),
      },
      getPrototypeOf: {
        ...operandCodeEffects,
        args: { target: b.object },
        returns: b.prototype("target"),
        uses: { target: [b.runs("PrototypeChain")] },
        reads: prototypeLink,
        implementation: b.inline("GetPrototypeOf", { parameters: ["ReflectGetPrototypeOf"], otherwise: b.hostCall }),
      },
      has: {
        ...operandCodeEffects,
        args: keyedArguments,
        returns: b.bool,
        uses: { target: [b.testedByKey(b.keyFrom("propertyKey")), ...b.ownPropertyTraps, b.runs("PrototypeChain")] },
        reads: inheritedProperties,
        implementation: b.compiled("ReflectObject", "has"),
      },
      isExtensible: {
        ...operandCodeEffects,
        args: { target: b.object },
        returns: b.bool,
        uses: { target: b.ownPropertyTraps },
        reads: ownProperties,
        implementation: b.hostCall,
      },
      ownKeys: {
        ...operandCodeEffects,
        args: { target: b.object },
        returns: b.instance("Array").holding({ element: b.union(b.string, b.symbol) }).fresh.dense,
        uses: { target: [b.runs("OwnKeys")] },
        reads: ownProperties,
        implementation: b.inline("OwnPropertyKeys", { parameters: ["All"], otherwise: b.hostCall }),
      },
      preventExtensions: {
        ...operandCodeEffects,
        args: { target: b.object },
        returns: b.bool,
        uses: { target: [b.integrityRaisedTo("NonExtensible"), ...b.ownPropertyTraps] },
        reads: ownProperties,
        writes: [...ownProperties, ...memberGuards],
        implementation: b.hostCall,
      },
      set: [
        {
          args: { ...keyedArguments, value: b.any, receiver: b.notPassed },
          returns: b.bool,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { target: [b.storedByKey(key, b.operand("value"))], value: b.inspected },
          reads: inheritedProperties,
          writes: memberGuards,
          implementation: b.hostCall,
        },
        {
          ...operandCodeEffects,
          args: { ...keyedArguments, value: b.any, receiver: b.any },
          returns: b.bool,
          throws: true,
          // As in `get`: the setter that is found on the target runs with the receiver as `this`. Where none is found the value is defined on the receiver.
          uses: {
            target: [
              b.accessorReadByKey("Setter", key, b.nativeCell(setterForReceiverCell)),
              b.keptCalled(setterForReceiverCell, {
                times: "Once",
                this: b.operand("receiver"),
                args: [b.operand("value")],
                result: "Ignored",
              }),
              // The receiver may be the target itself, or a proxy of it.
              b.definesName(key, b.operand("value")),
              b.storedByKey(key, b.operand("value")),
              ...b.ownPropertyTraps,
              b.runs("PrototypeChain"),
            ],
            value: [b.everything],
            receiver: [b.everything, b.definesName(key, b.operand("value"))],
          },
          implementation: b.hostCall,
        },
      ],
      setPrototypeOf: {
        ...operandCodeEffects,
        args: { target: b.object, proto: b.union(b.object, b.strictNull) },
        returns: b.bool,
        throws: true,
        uses: { target: [b.prototypeSetTo("proto"), b.runs("PrototypeChain")], proto: b.inspected },
        reads: prototypeLink,
        writes: prototypeChange,
        implementation: b.inline("SetPrototypeOf", { parameters: ["ReflectSetPrototypeOf"], otherwise: b.hostCall }),
      },
    },
    { locksMembers: true, mutability: "AlwaysLocked", table: b.staticTable("runtime/ReflectObject.cpp", "reflectObjectTable") },
  ),
);
