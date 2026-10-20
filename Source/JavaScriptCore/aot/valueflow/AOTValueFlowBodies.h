/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlow.h"
#include "AOTValueFlowBuiltinTables.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ValueSetMaker;
class ValueSets;

enum class ValueFlowObjectKind : uint8_t {
    Primitive,
    UnknownObject,
    Builtin,
    // import.meta, module, exports, require: one object per module.
    ModuleBuiltin,
    Namespace,
    Closure,
    // The promise of an async function, the object of a generator: one per function.
    CoroutineResult,
    // The array that a function's rest parameter holds.
    RestArray,
    Site,
    // What the construction of a plain function allocates.
    Instance,
    Prototype,
    // What a built-in allocates where no call operation stands for the call: native code calls it, or it is called together with others. Two for each built-in, as for a call.
    ResultOfBuiltin,
    // A string or a symbol that names a property.
    Key,
};

// A member of the universe as a body names it, before the members have numbers.
struct ValueFlowObjectName {
    ValueFlowObjectKind kind { ValueFlowObjectKind::UnknownObject };
    // The site's index in the body that names it, or the index of a built-in, module, function, layout or key.
    uint32_t index { 0 };
    // Of a ModuleBuiltin.
    ModuleID module;

    friend constexpr bool operator==(const ValueFlowObjectName&, const ValueFlowObjectName&) = default;
    friend constexpr auto operator<=>(const ValueFlowObjectName&, const ValueFlowObjectName&) = default;
};

enum class ValueFlowSiteOrigin : uint8_t {
    Operation,
    // An object or an array in the tree of a Data operation. `ordinal` is its number in preorder, the primitives counted.
    DataNode,
    // What a built-in allocates that a call finds as its callee: with ordinal 0 the object it returns, with ordinal 1 all the objects it makes to put into that one.
    BuiltinResult,
    // What a parser makes of a data file that is loaded by a name nobody knows: with ordinal 0 all its objects, with ordinal 1 all its arrays.
    ParsedData,
};

struct ValueFlowSite {
    ValueFlowSiteOrigin origin { ValueFlowSiteOrigin::Operation };
    ValueID value;
    uint32_t ordinal { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    // Of a site in a copy of a body: the function that the operation lies in.
    FunctionID function;

    friend bool operator==(const ValueFlowSite&, const ValueFlowSite&) = default;
};

// Who is who among the members of a set. Kinds of primitives come first, in the order of the bits of PrimitiveSet, then the unknown object, the built-ins, the objects of
// modules and layouts, then function by function its closure, the result of a coroutine, the array of a rest parameter and its sites, then the keys.
class ValueFlowUniverse {
public:
    static constexpr ValueFlowObjectID primitive(PrimitiveSet kind) { return ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(static_cast<uint16_t>(kind)))); }
    static constexpr uint32_t numberOfPrimitiveKinds = 16;

    static constexpr ValueFlowObjectID unknownObject() { return ValueFlowObjectID(numberOfPrimitiveKinds); }
    // The built-in's canonical entry stands for it.
    ValueFlowObjectID objectOf(BuiltinID) const;
    ValueFlowObjectID objectOf(BuiltinID, ModuleID) const;
    // The object of the realm, and those of the modules that have one of their own.
    void appendObjectsOf(BuiltinID, Vector<ValueFlowObjectID, 32>&) const;
    ValueFlowObjectID namespaceOf(ModuleID module) const { return ValueFlowObjectID(m_firstNamespace + module.index()); }
    ValueFlowObjectID instanceOf(LayoutID layout) const { return ValueFlowObjectID(m_firstLayoutObject + 2 * layout.index()); }
    ValueFlowObjectID prototypeOf(LayoutID layout) const { return ValueFlowObjectID(m_firstLayoutObject + 2 * layout.index() + 1); }
    // With ordinal 0 the object that the built-in returns, with ordinal 1 all the objects it makes to put into that one.
    ValueFlowObjectID resultOf(BuiltinID, uint32_t ordinal) const;
    ValueFlowObjectID closureOf(FunctionID function) const { return ValueFlowObjectID(m_firstClosure + function.index()); }
    ValueFlowObjectID coroutineResultOf(FunctionID function) const { return ValueFlowObjectID(m_firstCoroutineResult + function.index()); }
    ValueFlowObjectID restArrayOf(FunctionID function) const { return ValueFlowObjectID(m_firstRestArray + function.index()); }
    // None where the operation makes no object. A statement in a copy of a body has objects of its own.
    ValueFlowObjectID siteObject(const ValueFlowStatement&, uint32_t ordinal = 0) const;
    ValueFlowObjectID siteObject(FunctionID, ValueID, uint32_t ordinal = 0) const;
    // Of an object that a site in a copy of a body makes: what the site makes in the body itself. Any other object is its own original.
    // What holds for an allocation holds for the original and all its copies together.
    ValueFlowObjectID originalOf(ValueFlowObjectID) const;
    ValueFlowObjectID keyConstant(PropertyKeyID key) const { return ValueFlowObjectID(m_firstKey + key.index()); }
    ValueFlowObjectID objectNamed(FunctionID body, const ValueFlowObjectName&) const;

    uint32_t numberOfObjects() const { return m_numberOfObjects; }
    ValueFlowObjectKind kindOf(ValueFlowObjectID) const;
    bool isObject(ValueFlowObjectID object) const { return object.index() >= numberOfPrimitiveKinds && object.index() < m_firstKey; }

    // Each answers none for a member of another kind.
    FunctionID closureFunction(ValueFlowObjectID) const;
    BuiltinID builtinOf(ValueFlowObjectID) const;
    ModuleID moduleOf(ValueFlowObjectID) const;
    PropertyKeyID keyOf(ValueFlowObjectID) const;
    // The function that a site lies in, and that of a closure, a coroutine's result and a rest array.
    FunctionID functionOf(ValueFlowObjectID) const;
    const ValueFlowSite* siteOf(ValueFlowObjectID) const LIFETIME_BOUND;
    // Of a site with a layout, an instance and a namespace.
    LayoutID layoutOf(ValueFlowObjectID) const;
    // Of a prototype object: the class it belongs to.
    LayoutID classLayoutOf(ValueFlowObjectID) const;
    // Of a site that allocates an instance of a built-in class, of a coroutine's result and a rest array, and Function for a closure.
    BuiltinClassID classOf(ValueFlowObjectID) const;

    // Masks. Each mask that admits an object admits the unknown object, but for maskOf() of a type that lists every object inside it.
    ValueSetID primitives() const { return m_primitives; }
    // As a value: a primitive of which nothing is known. It lists the kinds and no key: a string of which only the kind is known may be any key.
    ValueSetID anyPrimitive() const { return m_anyPrimitive; }
    ValueSetID undefinedValue() const { return m_undefinedValue; }
    ValueSetID objects() const { return m_objects; }
    // Without the unknown object.
    ValueSetID knownObjects() const { return m_knownObjects; }
    ValueSetID builtinObjects() const { return m_builtinObjects; }
    // Built-in objects that start-up makes immutable: nobody stores into them, defines a property on them or takes one from them.
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    // Built-in objects that keep the object above them. The locked objects are among them.
    ValueSetID objectsWithLockedPrototypeLink() const { return m_objectsWithLockedPrototypeLink; }
    ValueSetID callables() const { return m_callables; }
    // The three below share nothing, hold no unknown object, and make up the callables with it.
    ValueSetID closures() const { return m_closures; }
    ValueSetID builtinFunctions() const { return m_builtinFunctions; }
    // A bound function, and every object of which nothing says what it is.
    ValueSetID otherCallables() const { return m_otherCallables; }
    ValueSetID arrays() const { return m_arrays; }
    ValueSetID keys() const { return m_keys; }
    // The kind, and the keys of that kind.
    ValueSetID strings() const { return m_strings; }
    ValueSetID symbols() const { return m_symbols; }
    // What may pass a test for the class.
    ValueSetID instancesOf(BuiltinClassID builtinClass) const { return m_objectsOfClass[builtinClass.index()]; }
    // What is allocated with the layout; for a layout of the table also whatever a built-in makes. The unknown object, and an object from an operation that says nothing of it, may have it too.
    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }
    // What passes the test for the class whatever happens: the objects that are allocated as instances of it or of a class below it.
    ValueSetID sureInstancesOf(BuiltinClassID builtinClass) const { return m_sureInstancesOfClass[builtinClass.index()]; }
    // The unknown object, and every object of which nothing says what it is: each mask for a class or for functions admits these.
    ValueSetID objectsThatMayBeAnything() const { return m_objectsThatMayBeAnything; }
    ValueSetID unknownObjectAlone() const { return m_unknownObjectAlone; }
    ValueFlowStandIn standIn() const { return { unknownObject(), m_objectsWithLayoutOfProgram }; }
    // What a value of the type may be. Safe from every thread.
    ValueSetID maskOf(ValueSetMaker&, const Program&, const Type&) const;
    // What is sure to be inside the type: the arm of a test that fails may drop this much.
    ValueSetID membersSurelyInside(ValueSetMaker&, const Program&, const Type&) const;

    const ValueFlowBuiltinTables& builtinTables() const LIFETIME_BOUND { return *m_builtinTables; }

private:
    friend class ValueFlowLinker;

    struct ModuleBuiltin {
        BuiltinID builtin;
        ModuleID module;

        friend constexpr auto operator<=>(const ModuleBuiltin&, const ModuleBuiltin&) = default;
    };

    std::span<const ValueFlowSite> sitesOf(FunctionID) const LIFETIME_BOUND;
    bool listsEveryObjectInside(const Program&, const Type&) const;

    const BuiltinTable* m_builtins { nullptr };
    uint32_t m_firstBuiltin { 0 };
    uint32_t m_firstModuleBuiltin { 0 };
    uint32_t m_firstNamespace { 0 };
    uint32_t m_firstLayoutObject { 0 };
    uint32_t m_firstResultOfBuiltin { 0 };
    uint32_t m_firstClosure { 0 };
    uint32_t m_firstCoroutineResult { 0 };
    uint32_t m_firstRestArray { 0 };
    uint32_t m_firstSite { 0 };
    uint32_t m_firstBuiltinResultSite { 0 };
    uint32_t m_firstKey { 0 };
    uint32_t m_numberOfObjects { 0 };
    Vector<ModuleBuiltin> m_moduleBuiltins;
    // One entry more than there are functions.
    Vector<uint32_t> m_firstSiteOfFunction;
    // In the order of the functions.
    Vector<ValueFlowSite> m_sites;
    // The members of one kind lie side by side, so that a set of closures or of allocated objects takes few blocks. What built-ins allocate at calls comes behind all other sites.
    Vector<ValueFlowObjectID> m_objectOfSite;
    Vector<uint32_t> m_siteOfObject;
    Vector<LayoutID> m_layoutOfNamespace;
    Vector<BuiltinClassID> m_classOfCoroutineResult;
    Vector<ValueSetID> m_objectsOfLayout;
    Vector<ValueSetID> m_objectsOfClass;
    Vector<ValueSetID> m_sureInstancesOfClass;
    ValueSetID m_objectsThatMayBeAnything;
    ValueSetID m_objectsWithAnyLayout;
    ValueSetID m_objectsWithLayoutOfProgram;
    BitVector m_isLayoutOfTable;
    // A layout of the program with an allocation site, a namespace or a constructor.
    BitVector m_isAllocatedByProgram;
    ValueSetID m_closures;
    ValueSetID m_builtinFunctions;
    ValueSetID m_otherCallables;
    ValueSetID m_strings;
    ValueSetID m_symbols;
    ValueSetID m_textKeys;
    ValueSetID m_symbolKeys;
    ValueSetID m_knownObjects;
    ValueSetID m_builtinObjects;
    ValueSetID m_lockedObjects;
    ValueSetID m_objectsWithLockedPrototypeLink;
    ValueSetID m_primitives;
    ValueSetID m_anyPrimitive;
    ValueSetID m_undefinedValue;
    ValueSetID m_objects;
    ValueSetID m_callables;
    ValueSetID m_arrays;
    ValueSetID m_keys;
    ValueSetID m_unknownObjectAlone;
    std::unique_ptr<ValueFlowBuiltinTables> m_builtinTables;
};

// The nodes of the host's body behind `thrown`.
enum class ValueFlowHostNode : uint8_t {
    // The built-ins that are used in any way but tested, those that are written, and those whose code runs: they are called, constructed or have escaped.
    UsedBuiltins,
    WrittenBuiltins,
    RunBuiltins,
    // The namespace objects of the modules of the library that seen code asks for. The functions of such a module are seen code.
    ModulesOfSeenCode,
    // The closures of the functions that seen code calls: a function of the library among them is seen code too.
    FunctionsOfSeenCode,
    // Each of these five holds the unknown object once it is so.
    UnseenCodeRuns,
    EscapedObjectsAreOpen,
    HostChoosesOperands,
    ModulesAreRequiredByName,
    ModulesAreImportedByName,
    // What require() and import() answer for a name that the analysis does not know.
    RequiredValuesLoadedByName,
    NamespacesLoadedByName,
    // What require() of a known module answers in place of that module once the program has put something of its own into the loader.
    AnswersOfReplacedLoader,
    // The namespace objects that somebody has asked for.
    CreatedNamespaces,
};

inline constexpr unsigned numberOfValueFlowHostNodes = 14;

// The nodes that more than one body names. Those of a function lie in front of the other nodes of its body.
class ValueFlowProgramNodes {
public:
    // Whatever has reached native code.
    static constexpr ValueFlowNodeID escaped() { return ValueFlowNodeID(0u); }
    ValueFlowNodeID variable(VariableID variable) const { return ValueFlowNodeID(m_firstVariable + variable.index()); }
    ValueFlowNodeID field(LayoutID layout, unsigned index) const { return ValueFlowNodeID(m_firstFieldOfLayout[layout.index()] + index); }
    ValueFlowNodeID environmentField(LayoutID layout, unsigned index) const { return field(layout, index); }
    ValueFlowNodeID privateField(LayoutID layout, unsigned index) const { return field(layout, index); }

    // In the order of the parameters of the entry block: captures first.
    ValueFlowNodeID entryParameter(FunctionID function, unsigned index) const { return ValueFlowNodeID(m_firstNodeOfFunction[function.index()] + index); }
    // Of a coroutine: what its body returns, not the promise or the generator.
    ValueFlowNodeID result(FunctionID function, unsigned index) const { return ValueFlowNodeID(m_firstResultOfFunction[function.index()] + index); }
    ValueFlowNodeID thrown(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()]); }
    // Of a generator: what its Yield operations hand out and what resuming it hands in. They follow `thrown`.
    ValueFlowNodeID yielded(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()] + 1); }
    ValueFlowNodeID sent(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()] + 2); }
    ValueFlowNodeID ofHost(ValueFlowHostNode node) const { return ValueFlowNodeID(m_thrownOfFunction.last() + 1 + static_cast<uint32_t>(node)); }
    // Whatever reaches an operand with the behavior, whichever operation or call it came through. The hubs follow the host's nodes.
    ValueFlowNodeID hub(ValueFlowBehaviorID behavior) const { return ValueFlowNodeID(m_thrownOfFunction.last() + 1 + numberOfValueFlowHostNodes + behavior.index()); }
    ValueFlowBehaviorID behaviorOf(ValueFlowNodeID hub) const { return ValueFlowBehaviorID(hub.index() - m_thrownOfFunction.last() - 1 - numberOfValueFlowHostNodes); }

    uint32_t numberOfNodes() const { return m_numberOfNodes; }
    uint32_t firstNodeOf(FunctionID function) const { return m_firstNodeOfFunction[function.index()]; }
    uint32_t endOfNodesOf(FunctionID function) const { return m_firstNodeOfFunction[function.index() + 1]; }
    // None for a node of no body.
    FunctionID functionOf(ValueFlowNodeID) const;

private:
    friend class ValueFlowLinker;

    uint32_t m_firstVariable { 1 };
    uint32_t m_numberOfNodes { 0 };
    Vector<uint32_t> m_firstFieldOfLayout;
    // One entry more than there are functions.
    Vector<uint32_t> m_firstNodeOfFunction;
    Vector<uint32_t> m_firstResultOfFunction;
    Vector<uint32_t> m_thrownOfFunction;
};

// A node as a body names it, before the nodes have numbers.
struct ValueFlowNodeName {
    enum class Kind : uint8_t { None, OfBody, Escaped, Variable, Field, EntryParameter, Result, Thrown, OfHost };

    Kind kind { Kind::None };
    // The node's index in the body, or the index of a variable, layout, function or node of the host.
    uint32_t owner { 0 };
    // Which field, parameter or result.
    uint32_t index { 0 };

    static constexpr ValueFlowNodeName ofBody(uint32_t index) { return { Kind::OfBody, index, 0 }; }
    static constexpr ValueFlowNodeName variable(VariableID variable) { return { Kind::Variable, variable.index(), 0 }; }
    static constexpr ValueFlowNodeName field(LayoutID layout, uint32_t index) { return { Kind::Field, layout.index(), index }; }
    static constexpr ValueFlowNodeName entryParameter(FunctionID function, uint32_t index) { return { Kind::EntryParameter, function.index(), index }; }
    static constexpr ValueFlowNodeName result(FunctionID function, uint32_t index) { return { Kind::Result, function.index(), index }; }
    static constexpr ValueFlowNodeName thrown(FunctionID function) { return { Kind::Thrown, function.index(), 0 }; }
    static constexpr ValueFlowNodeName ofHost(ValueFlowHostNode node) { return { Kind::OfHost, static_cast<uint32_t>(node), 0 }; }
    static constexpr ValueFlowNodeName hub(ValueFlowBehaviorID behavior) { return { Kind::OfHost, numberOfValueFlowHostNodes + behavior.index(), 0 }; }
    static constexpr ValueFlowNodeName escaped() { return { Kind::Escaped, 0, 0 }; }

    constexpr explicit operator bool() const { return kind != Kind::None; }
    friend constexpr bool operator==(const ValueFlowNodeName&, const ValueFlowNodeName&) = default;
    friend constexpr auto operator<=>(const ValueFlowNodeName&, const ValueFlowNodeName&) = default;
};

enum class ValueFlowBodyNodeKind : uint8_t {
    EntryParameter,
    Result,
    Thrown,
    Yielded,
    Sent,
    // What the handler block of `value`'s block catches.
    Caught,
    // The result of an operation, filled by its rule.
    OperationResult,
    Filter,
    Merge,
};

// A body in its own numbers: what is left of a function when only the nodes stay that a round needs.
struct ValueFlowBody {
    static constexpr uint32_t none = UINT32_MAX;

    // Kinds of primitives and members with a name. Entry 0 is the empty set.
    struct Constant {
        uint16_t primitives { 0 };
        Vector<ValueFlowObjectName, 1> objects;

        bool isEmpty() const { return !primitives && objects.isEmpty(); }
        friend bool operator==(const Constant&, const Constant&) = default;
    };

    // The value is inside every type of `inside` and is none of what is sure to be inside a type of `outside`. Entry 0 admits everything.
    struct Mask {
        Vector<TypeID, 1> inside;
        Vector<TypeID, 1> outside;

        bool admitsEverything() const { return inside.isEmpty() && outside.isEmpty(); }
        friend bool operator==(const Mask&, const Mask&) = default;
    };

    struct Node {
        ValueFlowBodyNodeKind kind { ValueFlowBodyNodeKind::Merge };
        ValueID value;
        uint32_t mask { 0 };
    };

    struct Edge {
        ValueFlowNodeName from;
        ValueFlowNodeName to;

        friend constexpr bool operator==(const Edge&, const Edge&) = default;
        friend constexpr auto operator<=>(const Edge&, const Edge&) = default;
    };

    // The node holds mask & constant.
    struct Seed {
        ValueFlowNodeName node;
        uint32_t constant { 0 };
        uint32_t mask { 0 };

        friend constexpr bool operator==(const Seed&, const Seed&) = default;
        friend constexpr auto operator<=>(const Seed&, const Seed&) = default;
    };

    // mask & (the node's set | constant)
    struct Operand {
        ValueFlowNodeName node;
        uint32_t constant { 0 };
        uint32_t mask { 0 };

        friend constexpr bool operator==(const Operand&, const Operand&) = default;
    };

    struct Statement {
        Opcode opcode { };
        ValueID site;
        uint32_t firstOperand { 0 };
        uint32_t operandCount { 0 };
        ValueFlowNodeName result;
        ValueFlowNodeName thrown;
        ValueFlowStatementKind kind { ValueFlowStatementKind::Operation };
        // In a copy of a body: the function that the operation lies in.
        FunctionID function;
        FunctionID copiedCallee;
        // The first of the operation's sites in `sites`.
        uint32_t firstSite { none };
    };

    // The nodes of a function's body as they stand in a copy of it: in the same order, from `firstNode` on.
    struct Copy {
        FunctionID function;
        uint32_t firstNode { 0 };
        uint32_t numberOfNodes { 0 };
    };

    // What a branch or a claim looks at. No rule runs for it: whether the test can go is asked of the operand once the values stand. `test` is none for the subject of a switch.
    struct ObservedValue {
        ValueID test;
        Operand operand;
    };

    FunctionID function;
    uint32_t numberOfEntryParameters { 0 };
    uint32_t numberOfResults { 0 };
    bool hasYieldedAndSent { false };
    // Entry parameters, results, thrown, yielded and sent where there are any, then the others.
    Vector<Node> nodes;
    Vector<ValueFlowSite> sites;
    Vector<Constant> constants;
    Vector<Mask> masks;
    // They hold whether or not the function is reached: nothing leaves the body along them.
    Vector<Edge> edges;
    // What a body does to the rest of the program once it is reached: stores into variables and fields of environments, operands of functions it names, constants it makes.
    Vector<Edge> edgesOnceReached;
    Vector<Seed> seedsOnceReached;
    Vector<FunctionID> functionsEnteredOnceReached;
    // First those that only look at their operands, which may run before the function is reached, then from `firstStatementLeavingBody` on the others.
    // The statements of copied bodies come last.
    Vector<Statement> statements;
    uint32_t firstStatementLeavingBody { 0 };
    uint32_t numberOfCopiedStatements { 0 };
    Vector<Copy> copies;
    Vector<Operand> operands;
    Vector<ObservedValue> observedValues;

    uint32_t result(uint32_t index) const { return numberOfEntryParameters + index; }
    uint32_t thrown() const { return numberOfEntryParameters + numberOfResults; }
    uint32_t yielded() const { return thrown() + 1; }
    uint32_t sent() const { return thrown() + 2; }
    std::span<const Operand> operandsOf(const Statement& statement) const LIFETIME_BOUND { return operands.subspan(statement.firstOperand, statement.operandCount); }
};

// Reads nothing but the program, so every body can be reduced on a thread of its own. Of a function without a body only the nodes stay that others name.
ValueFlowBody reduceBody(const Program&, const ValueFlowBuiltinTables&, FunctionID);
// What the host does of its own accord: it enters the entry points and the functions it calls, with the unknown object as every operand. It has the number of no function.
ValueFlowBody makeHostBody(const Program&, const ValueFlowBuiltinTables&);

struct ValueFlowObservedValue {
    FunctionID function;
    // None for the subject of a switch.
    ValueID test;
    ValueFlowOperand operand;
};

// The nodes of a function's body as they stand in one copy of it: in the same order, from `firstNode` on.
struct ValueFlowBodyCopy {
    FunctionID function;
    ValueFlowNodeID firstNode;
    uint32_t numberOfNodes { 0 };
};

struct ValueFlowBodiesTimes {
    Seconds reduce;
    Seconds number;
    Seconds makeMasks;
    Seconds link;
};

// All bodies in the numbers of the program.
class ValueFlowBodies {
    WTF_MAKE_NONCOPYABLE(ValueFlowBodies);
public:
    ValueFlowBodies();
    ValueFlowBodies(ValueFlowBodies&&);
    ValueFlowBodies& operator=(ValueFlowBodies&&);
    ~ValueFlowBodies();

    // Made for the universe of these bodies. Constants and masks are interned in it.
    ValueSets& sets() const { return *m_sets; }
    const ValueFlowUniverse& universe() const LIFETIME_BOUND { return m_universe; }
    const ValueFlowProgramNodes& nodes() const LIFETIME_BOUND { return m_nodes; }

    std::span<const ValueFlowEdge> edges() const LIFETIME_BOUND { return m_edges.span(); }
    std::span<const ValueFlowNodeSet> masks() const LIFETIME_BOUND { return m_masks.span(); }
    std::span<const ValueFlowStatement> statements() const LIFETIME_BOUND { return m_statements.span(); }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_operands.subspan(statement.firstOperand, statement.operandCount); }
    std::span<const ValueFlowOperand> operands() const LIFETIME_BOUND { return m_operands.span(); }
    std::span<const ValueFlowEdge> edgesOnceReached() const LIFETIME_BOUND { return m_edgesOnceReached.span(); }
    std::span<const ValueFlowNodeSet> seedsOnceReached() const LIFETIME_BOUND { return m_seedsOnceReached.span(); }
    std::span<const FunctionID> functionsEnteredOnceReached() const LIFETIME_BOUND { return m_functionsEnteredOnceReached.span(); }
    std::span<const ValueFlowBodyRange> ranges() const LIFETIME_BOUND { return m_ranges.span(); }
    std::span<const ValueFlowObservedValue> observedValues() const LIFETIME_BOUND { return m_observedValues.span(); }
    std::span<const ValueFlowBodyCopy> copies() const LIFETIME_BOUND { return m_copies.span(); }
    const ValueFlowBodiesTimes& times() const { return m_times; }

    // The host's body comes after the last function.
    uint32_t numberOfBodies() const { return m_ranges.size() - 1; }
    uint32_t hostBody() const { return numberOfBodies() - 1; }
    // Without the statements of bodies that were copied into the body.
    std::span<const ValueFlowStatement> statementsOf(uint32_t body) const LIFETIME_BOUND { return m_statements.subspan(m_ranges[body].statement, m_ranges[body].copiedStatement - m_ranges[body].statement); }
    std::span<const ValueFlowStatement> statementsStayingInside(uint32_t body) const LIFETIME_BOUND { return m_statements.subspan(m_ranges[body].statement, m_ranges[body].statementLeavingBody - m_ranges[body].statement); }
    std::span<const ValueFlowEdge> edgesOnceReached(uint32_t body) const LIFETIME_BOUND { return m_edgesOnceReached.subspan(m_ranges[body].edgeOnceReached, m_ranges[body + 1].edgeOnceReached - m_ranges[body].edgeOnceReached); }
    std::span<const ValueFlowNodeSet> seedsOnceReached(uint32_t body) const LIFETIME_BOUND { return m_seedsOnceReached.subspan(m_ranges[body].seedOnceReached, m_ranges[body + 1].seedOnceReached - m_ranges[body].seedOnceReached); }
    std::span<const FunctionID> functionsEnteredOnceReached(uint32_t body) const LIFETIME_BOUND { return m_functionsEnteredOnceReached.subspan(m_ranges[body].functionEntered, m_ranges[body + 1].functionEntered - m_ranges[body].functionEntered); }

private:
    friend class ValueFlowLinker;

    std::unique_ptr<ValueSets> m_sets;
    ValueFlowUniverse m_universe;
    ValueFlowProgramNodes m_nodes;
    Vector<ValueFlowEdge> m_edges;
    Vector<ValueFlowNodeSet> m_masks;
    Vector<ValueFlowStatement> m_statements;
    Vector<ValueFlowOperand> m_operands;
    Vector<ValueFlowEdge> m_edgesOnceReached;
    Vector<ValueFlowNodeSet> m_seedsOnceReached;
    Vector<FunctionID> m_functionsEnteredOnceReached;
    Vector<ValueFlowObservedValue> m_observedValues;
    Vector<ValueFlowBodyCopy> m_copies;
    ValueFlowBodiesTimes m_times;
    // One entry more than there are bodies.
    Vector<ValueFlowBodyRange> m_ranges;
};

// Reduces every body on the threads of the build and numbers objects and nodes by prefix sums over the bodies: but for the numbers of sets the result is the same on any number of threads.
ValueFlowBodies reduceBodies(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
