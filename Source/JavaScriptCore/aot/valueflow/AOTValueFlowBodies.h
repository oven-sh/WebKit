/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlow.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowUnobservedDefinitions.h"
#include "AOTVector.h"
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <atomic>
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CFG;
class ValueSetMaker;
class ValueSets;

enum class ValueFlowObjectKind : uint8_t {
    Primitive,
    // The unknown object, and the untracked object, which no body names.
    UnknownObject,
    Builtin,
    // import.meta, module, require: one object per module that an operation names it for, and `module` for each CommonJS module that holds what it exports in one variable.
    ModuleBuiltin,
    Namespace,
    Closure,
    // The promise of an async function, the object of a generator: one per function.
    CoroutineResult,
    // The array that a function's rest parameter holds.
    RestArray,
    // Of a plain function without a layout: the prototype object that it has from the start, and what a construction with it as new.target allocates. Both are ordinary objects.
    FunctionPrototype,
    ConstructedObject,
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
    // What the compiled parsers and validators allocate that take the place of a call: see ValueFlowTypedResult for the ordinals.
    TypedResult,
};

struct ValueFlowSite {
    ValueFlowSiteOrigin origin { ValueFlowSiteOrigin::Operation };
    ValueID value;
    uint32_t ordinal { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    // A literal with `__proto__: value`. Nobody sees it before the value is above it.
    bool namesPrototype { false };

    friend bool operator==(const ValueFlowSite&, const ValueFlowSite&) = default;
};

// Who is who among the members of a set.
class ValueFlowUniverse {
public:
    static constexpr ValueFlowObjectID primitive(PrimitiveSet kind) { return ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(static_cast<uint16_t>(kind)))); }
    static constexpr uint32_t numberOfPrimitiveKinds = 16;

    // Anything: a value from code that nobody compiles or from an unsound edge, and any object that was handed to such code. It stands for all that ValueFlowProgramNodes::escaped() holds.
    static constexpr ValueFlowObjectID unknownObject() { return ValueFlowObjectID(numberOfPrimitiveKinds); }
    // An object or a function that compiled code has allocated and that the analysis has stopped following. It stands for all that ValueFlowProgramNodes::lost() holds.
    static constexpr ValueFlowObjectID untrackedObject() { return ValueFlowObjectID(numberOfPrimitiveKinds + 1); }
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
    // The object that stands for all that native code makes for one record of BuiltinTable::hostObjects().
    ValueFlowObjectID hostObject(uint32_t indexOfRecord) const;
    ValueFlowObjectID closureOf(FunctionID function) const { return ValueFlowObjectID(m_firstClosure + function.index()); }
    ValueFlowObjectID coroutineResultOf(FunctionID function) const { return ValueFlowObjectID(m_firstCoroutineResult + function.index()); }
    ValueFlowObjectID restArrayOf(FunctionID function) const { return ValueFlowObjectID(m_firstRestArray + function.index()); }
    ValueFlowObjectID prototypeObjectOf(FunctionID function) const { return ValueFlowObjectID(m_firstFunctionPrototype + function.index()); }
    ValueFlowObjectID constructedObjectOf(FunctionID newTarget) const { return ValueFlowObjectID(m_firstConstructedObject + newTarget.index()); }
    // None where the operation makes no object.
    ValueFlowObjectID siteObject(const ValueFlowStatement&, uint32_t ordinal = 0) const;
    ValueFlowObjectID siteObject(FunctionID, ValueID, uint32_t ordinal = 0) const;
    // Sites may share an object: siteOf() and functionOf() answer for the first of them.
    bool isSiteObjectOf(const ValueFlowStatement&, ValueFlowObjectID) const;
    ValueFlowObjectID keyConstant(PropertyKeyID key) const { return ValueFlowObjectID(m_firstKey + key.index()); }
    ValueFlowObjectID objectNamed(FunctionID body, const ValueFlowObjectName&) const;

    uint32_t numberOfObjects() const { return m_numberOfObjects; }
    ValueFlowObjectKind kindOf(ValueFlowObjectID) const;
    bool isObject(ValueFlowObjectID object) const { return object.index() >= numberOfPrimitiveKinds && object.index() < m_firstKey; }

    // Each answers none for a member of another kind.
    FunctionID closureFunction(ValueFlowObjectID) const;
    // Of a function that only tests its arguments and of the typed body that it calls: the generic version, which runs in their place where a test or a check fails. None for any other function.
    FunctionID genericVersionOf(FunctionID function) const { return function.index() < m_genericVersionOf.size() ? m_genericVersionOf[function.index()] : FunctionID(); }
    BuiltinID builtinOf(ValueFlowObjectID) const;
    ModuleID moduleOf(ValueFlowObjectID) const;
    // The first module of the program that names the module of the run time. None where no module does.
    ModuleID moduleOf(RuntimeModuleID runtimeModule) const { return runtimeModule.index() < m_moduleOfRuntimeModule.size() ? m_moduleOfRuntimeModule[runtimeModule.index()] : ModuleID(); }
    // For runHostRoots(): one more than the set of the closures of the functions that the function calls directly, zero until somebody has found it.
    std::atomic<uint32_t>& rememberedClosuresCalledDirectlyBy(FunctionID function) const { return m_closuresCalledDirectlyBy[function.index()]; }
    // The module whose body the function is. A file may hold the bodies of several modules, and every function that is written in the file names the module of the file.
    ModuleID moduleWithBody(FunctionID) const;
    PropertyKeyID keyOf(ValueFlowObjectID) const;
    // The function that a site lies in, and that of a closure, a coroutine's result, a rest array, a prototype object of a function and what a function constructs.
    FunctionID functionOf(ValueFlowObjectID) const;
    const ValueFlowSite* siteOf(ValueFlowObjectID) const LIFETIME_BOUND;
    // Of a site with a layout, an instance and a namespace.
    LayoutID layoutOf(ValueFlowObjectID) const;
    // Of a prototype object: the class it belongs to.
    LayoutID classLayoutOf(ValueFlowObjectID) const;
    // Of a site that allocates an instance of a built-in class, of a coroutine's result and a rest array, Function for a closure, and Object for what a plain function has and makes.
    BuiltinClassID classOf(ValueFlowObjectID) const;

    // Masks. Each mask that is made for "may be" admits both placeholders as soon as it admits an object. What a placeholder becomes behind the mask of a type is up to the kind of the mask.
    ValueSetID primitives() const { return m_primitives; }
    // As a value: a primitive of which nothing is known. It lists the kinds and no key: a string of which only the kind is known may be any key.
    ValueSetID anyPrimitive() const { return m_anyPrimitive; }
    ValueSetID undefinedValue() const { return m_undefinedValue; }
    ValueSetID objects() const { return m_objects; }
    // Without the placeholders.
    ValueSetID knownObjects() const { return m_knownObjects; }
    ValueSetID builtinObjects() const { return m_builtinObjects; }
    // The built-in objects of which the realm has one. The run time locks the members of these that compiled code relies on.
    ValueSetID objectsOfRealm() const { return m_objectsOfRealm; }
    // Built-in objects that start-up makes immutable: nobody stores into them, defines a property on them or takes one from them.
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    // Built-in objects that keep the object above them. The locked objects are among them.
    ValueSetID objectsWithLockedPrototypeLink() const { return m_objectsWithLockedPrototypeLink; }
    // The objects on which the exports of a module of the library hang: what the body of the module allocates and returns, and the namespace object of the module.
    ValueSetID holdersOfLibraryExports() const { return m_holdersOfLibraryExports; }
    ValueSetID callables() const { return m_callables; }
    // The three below share nothing, hold no placeholder, and make up the callables with the placeholders.
    ValueSetID closures() const { return m_closures; }
    ValueSetID builtinFunctions() const { return m_builtinFunctions; }
    // The built-in functions that the table itself has under a name by which a protocol of the language finds what it calls: they open and step iterators and settle promises, which the rules follow.
    ValueSetID builtinFunctionsOfProtocols() const { return m_builtinFunctionsOfProtocols; }
    // A bound function, and every object of which nothing says what it is.
    ValueSetID otherCallables() const { return m_otherCallables; }
    // For boundFunctionsBindingNoArgumentAmong(): two sets: above, the callees of which it was asked; below, those among them that bind none. Both are nothing at first, which is zero.
    std::atomic<uint64_t>& rememberedBoundFunctionsBindingNoArgument() const { return m_boundFunctionsBindingNoArgument[0]; }
    ValueSetID arrays() const { return m_arrays; }
    ValueSetID keys() const { return m_keys; }
    // The kind, and the keys of that kind.
    ValueSetID strings() const { return m_strings; }
    ValueSetID symbols() const { return m_symbols; }
    // What may pass a test for the class.
    ValueSetID instancesOf(BuiltinClassID builtinClass) const { return m_objectsOfClass[builtinClass.index()]; }
    // What is allocated with the layout; for a layout of the table also whatever a built-in makes. A placeholder, and an object from an operation that says nothing of it, may have it too.
    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }
    // What passes the test for the class whatever happens: the objects that are allocated as instances of it or of a class below it.
    ValueSetID sureInstancesOf(BuiltinClassID builtinClass) const { return m_sureInstancesOfClass[builtinClass.index()]; }
    // The placeholders, and every object of which nothing says what it is: each mask for a class or for functions admits these.
    ValueSetID objectsThatMayBeAnything() const { return m_objectsThatMayBeAnything; }
    ValueSetID unknownObjectAlone() const { return m_unknownObjectAlone; }
    ValueSetID untrackedObjectAlone() const { return m_untrackedObjectAlone; }
    ValueSetID placeholdersAlone() const { return m_placeholdersAlone; }
    // What the constructors of base classes allocate.
    ValueSetID objectsThatPickTheirLayout() const { return m_objectsThatPickTheirLayout; }
    // instanceOf() of the classes and constructor functions without a heritage. Such an instance has the layout of its class unless an operand passes the class as new.target.
    ValueSetID instancesOfRootClasses() const { return m_instancesOfRootClasses; }
    // instanceOf() of every class.
    ValueSetID instancesOfClasses() const { return m_instancesOfClasses; }
    // The records that the type of a parameter of a checked function lists, and those that the types of their fields and of what their arrays hold lead to: data that typed code takes. Each requires a field.
    std::span<const LayoutID> recordsThatTypedCodeTakes() const LIFETIME_BOUND { return m_recordsThatTypedCodeTakes.span(); }
    // What stands for what the allocation of the site makes under the prototype.
    ValueFlowObjectID objectAllocatedUnder(const Program&, ValueFlowObjectID site, ValueFlowObjectID prototype) const;
    // The closures of the constructors that answer nothing but the object that they construct.
    ValueSetID constructorsAnsweringOnlyWhatTheyConstruct() const { return m_constructorsAnsweringOnlyWhatTheyConstruct; }
    ValueFlowPlaceholders placeholders() const { return { untrackedObject(), unknownObject(), m_objectsWithLayoutOfProgram, m_closures }; }
    // The four below are safe from every thread.
    ValueSetID maskOf(ValueSetMaker&, const Program&, const Type&) const;
    // What is sure to be inside the type: the arm of a test that fails may drop this much. Of objectsThatPickTheirLayout() only the solved sets say what they pick: ValueFlowPositions::isSurelyInside().
    ValueSetID membersSurelyInside(ValueSetMaker&, const Program&, const Type&) const;
    ValueSetID membersSurelyInside(ValueSetMaker&, const Program&, TypeID) const;
    ValueSetID membersInsideUnlessNewTargetDiffers(ValueSetMaker&, const Program&, TypeID) const;
    // The objects that the type lists one by one: what is allocated with its layouts, the closures of its functions, its built-in objects.
    ValueSetID familyObjectsOf(ValueSetMaker&, const Program&, const Type&) const;
    // Only the program makes an object that is inside the type, so a placeholder inside the type is one of familyObjectsOf().
    bool namesProgramObjectsAlone(const Program&, const Type&) const;

    const ValueFlowBuiltinTables& builtinTables() const LIFETIME_BOUND { return *m_builtinTables; }

private:
    friend class ValueFlowLinker;

    struct ModuleBuiltin {
        BuiltinID builtin;
        ModuleID module;

        friend constexpr auto operator<=>(const ModuleBuiltin&, const ModuleBuiltin&) = default;
    };

    std::span<const ValueFlowSite> sitesOf(FunctionID) const LIFETIME_BOUND;
    void appendFunctionObjectsOf(const Program&, const Type&, Vector<ValueFlowObjectID, 32>& members, Vector<ValueSetID, 8>& parts) const;
    ValueSetID objectsSurelyWithLayoutIn(ValueSetMaker&, const Program&, LayoutSetID) const;
    ValueSetID objectsWithLayoutIn(ValueSetMaker&, const Program&, LayoutSetID) const;

    const BuiltinTable* m_builtins { nullptr };
    uint32_t m_firstBuiltin { 0 };
    uint32_t m_firstModuleBuiltin { 0 };
    uint32_t m_firstNamespace { 0 };
    uint32_t m_firstLayoutObject { 0 };
    uint32_t m_firstResultOfBuiltin { 0 };
    uint32_t m_firstClosure { 0 };
    uint32_t m_firstCoroutineResult { 0 };
    uint32_t m_firstRestArray { 0 };
    uint32_t m_firstFunctionPrototype { 0 };
    uint32_t m_firstConstructedObject { 0 };
    uint32_t m_firstSite { 0 };
    uint32_t m_firstBuiltinResultSite { 0 };
    uint32_t m_firstKey { 0 };
    uint32_t m_numberOfObjects { 0 };
    Vector<ModuleBuiltin> m_moduleBuiltins;
    // In the order of the functions.
    Vector<std::pair<FunctionID, ModuleID>> m_modulesWithBodies;
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
    ValueSetID m_objectsThatPickTheirLayout;
    ValueSetID m_instancesOfRootClasses;
    ValueSetID m_instancesOfClasses;
    ValueSetID m_constructorsAnsweringOnlyWhatTheyConstruct;
    BitVector m_isLayoutOfTable;
    // A layout of the program with an allocation site, a namespace or a constructor.
    BitVector m_isAllocatedByProgram;
    Vector<LayoutID> m_recordsThatTypedCodeTakes;
    ValueSetID m_closures;
    ValueSetID m_builtinFunctions;
    ValueSetID m_builtinFunctionsOfProtocols;
    ValueSetID m_builtinsThatMayBeAnyLibraryClosure;
    uint32_t m_numberOfRememberedLayoutSets { 0 };
    std::unique_ptr<std::atomic<uint32_t>[]> m_objectsSurelyWithLayoutIn;
    std::unique_ptr<std::atomic<uint32_t>[]> m_objectsWithLayoutIn;
    std::unique_ptr<std::atomic<uint32_t>[]> m_closuresCalledDirectlyBy;
    Vector<ModuleID> m_moduleOfRuntimeModule;
    std::unique_ptr<std::atomic<uint64_t>[]> m_boundFunctionsBindingNoArgument;
    uint32_t m_numberOfRememberedTypes { 0 };
    std::unique_ptr<std::atomic<uint32_t>[]> m_membersSurelyInsideType;
    BitVector m_isCanonicalEntryOfCallableBuiltin;
    HashMap<uint32_t, Vector<ValueFlowObjectID, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_builtinsThatMayBeClosureOf;
    ValueSetID m_otherCallables;
    ValueSetID m_strings;
    ValueSetID m_symbols;
    ValueSetID m_textKeys;
    ValueSetID m_symbolKeys;
    ValueSetID m_knownObjects;
    ValueSetID m_builtinObjects;
    ValueSetID m_objectsOfRealm;
    Vector<FunctionID> m_genericVersionOf;
    ValueSetID m_lockedObjects;
    ValueSetID m_objectsWithLockedPrototypeLink;
    ValueSetID m_holdersOfLibraryExports;
    ValueSetID m_primitives;
    ValueSetID m_anyPrimitive;
    ValueSetID m_undefinedValue;
    ValueSetID m_objects;
    ValueSetID m_callables;
    ValueSetID m_arrays;
    ValueSetID m_keys;
    ValueSetID m_unknownObjectAlone;
    ValueSetID m_untrackedObjectAlone;
    ValueSetID m_placeholdersAlone;
    std::unique_ptr<ValueFlowBuiltinTables> m_builtinTables;
};

// Only a function of the program is split into the tests of its arguments, its typed body and its generic version in front of the analysis. The typed body keeps the role.
inline bool isWrittenInProgram(const Function& function)
{
    return function.role == FunctionRole::Source || function.role == FunctionRole::EntryChecks || function.role == FunctionRole::GenericVersion;
}

// The nodes of the host's body behind `thrown`.
enum class ValueFlowHostNode : uint8_t {
    // The built-ins that are used in any way but tested, those that are written, and those whose code runs: they are called or constructed. One that has escaped is held, and runs once a call may reach it.
    UsedBuiltins,
    WrittenBuiltins,
    RunBuiltins,
    // The namespace objects of the modules of the library that seen code asks for. The body of such a module is seen code.
    ModulesOfSeenCode,
    // The closures of the functions that seen code calls: a function of the library among them is seen code too. And the built-ins whose application by seen code has raised an event.
    FunctionsOfSeenCode,
    // Each of these five holds the unknown object once it is so.
    UnseenCodeRuns,
    // Code that nobody compiles stores into the objects that it was handed. Until then it only reads them.
    UnknownCodeWrites,
    HostChoosesOperands,
    ModulesAreRequiredByName,
    ModulesAreImportedByName,
    // What require() and import() answer for a name that the analysis does not know.
    RequiredValuesLoadedByName,
    NamespacesLoadedByName,
    // The module objects that somebody has got hold of: an operation names one, or `require.main`, `process.mainModule`, `children` or `parent` has answered it.
    ModuleObjectsHandedOut,
    // The namespace objects that somebody has asked for.
    CreatedNamespaces,
    // What native code holds only through the exports of a module of the library: it keeps these and calls none of them.
    HeldByNativeCode,
    // Holds the unknown object once a loader that answers modules of the library and nothing else is given a name that the analysis does not know, and what it answers then.
    LibraryModulesAreRequiredByName,
    LibraryValuesLoadedByName,
    // Holds the unknown object once seen code stores or defines a property of an object that nobody knows, or hands an object of the host that keeps strings what is no string, number or boolean.
    ObjectNobodyKnowsIsWritten,
    // What an operand hands a construction as new.target. In every other construction the callee is new.target.
    NewTargetsFromOperands,
    // Holds the unknown object once unseen code runs that is more than text of the global scope: see UnseenCodeReach.
    UnseenCodeChangesRealm,
    // The closures that the prototype objects above escaped instances name as their constructors. Whoever holds an instance finds its class there, and the class has not escaped with the instance.
    ConstructorsAboveEscapedInstances,
    // The objects of the program above escaped instances. Whoever holds an instance and asks what lies above it gets one of them, and none of them has escaped with the instance.
    ObjectsAboveEscapedInstances,
    HoldersOfExportsInEscapedObjects,
    // Holds the unknown object once a call is reached whose callee nobody knows and that names no member of its receiver: `f(..)`, `o[key](..)`, a callback, an accessor, the target of a bound function.
    ValueNobodyKnowsIsCalled,
    // The built-ins that some statement has read, written, called or constructed as themselves.
    BuiltinsThatStatementsName,
    // The prototype objects of the classes of the table of which an instance may exist: an overload that was applied names the class in what it answers or in what it hands to a callback.
    PrototypeObjectsOfInstances,
    // The pairs that Object.entries has made.
    PairsOfEntries,
    // The Maps whose constructor has taken keys and values as the records say: out of all that the entries lead to.
    MapsFilledAsRecordsSay,
    // The built-ins that let in unseen code on a holder at which a read under a key nobody knows starts, and the objects that they hold.
    BuiltinsHeldWithoutBeingListed,
    // Holds the unknown object once unseen code runs that has a loader in its hands: see UnseenCodeReach.
    UnseenCodeHasLoader,
    // What has been stored, on whatever object, under a name under which the host finds a method that it calls with a function of a start-up module (BuiltinTable::namesOfHostMethodsCallingStartupLoads).
    StoredUnderNamesOfMethodsCallingStartupLoads,
    // instanceOf() of each record that a parser of the run time may make in the place of an ordinary object.
    RecordsMadeByParsers,
    // The built-in objects that a statement stores to, defines on or deletes from as themselves, and the objects of the host that the receiver of a store lists. The run time locks no member that such a statement may hit.
    BuiltinObjectsThatStatementsWrite,
    // The functions of the host that the program has taken from a holder that the analysis follows.
    FunctionsOfHostTakenFromKnownHolders,
    // The namespace objects that a statement of a body has tied to the exports of their modules, and goes on tying: see namespaceOf().
    NamespacesTiedByStatementsOfBodies,
    // The module objects of what the modules require whose module objects are handed out, and of the modules that require those: what `children` and `parent` answer.
    ChildrenOfModuleObjects,
    ParentsOfModuleObjects,
    // The pairs that the steps of built-in iterators over entries have made.
    PairsOfSteps,
};

inline constexpr unsigned numberOfValueFlowHostNodes = 38;

// The nodes that more than one body names. Those of a function lie in front of the other nodes of its body.
class ValueFlowProgramNodes {
public:
    // Whatever was handed to code that nobody compiles.
    static constexpr ValueFlowNodeID escaped() { return ValueFlowNodeID(0u); }
    // Whatever the analysis has stopped following. Nothing follows from being here: no function is entered and no object is opened.
    static constexpr ValueFlowNodeID lost() { return ValueFlowNodeID(1u); }
    ValueFlowNodeID variable(VariableID variable) const { return ValueFlowNodeID(m_firstVariable + variable.index()); }
    ValueFlowNodeID field(LayoutID layout, unsigned index) const { return ValueFlowNodeID(m_firstFieldOfLayout[layout.index()] + index); }
    ValueFlowNodeID environmentField(LayoutID layout, unsigned index) const { return field(layout, index); }
    ValueFlowNodeID privateField(LayoutID layout, unsigned index) const { return field(layout, index); }

    // In the order of the parameters of the entry block: captures first.
    ValueFlowNodeID entryParameter(FunctionID function, unsigned index) const { return ValueFlowNodeID(m_firstNodeOfFunction[function.index()] + index); }
    ValueFlowNodeID capture(FunctionID function, unsigned index) const { return entryParameter(function, index); }
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

    // Of a function that returns one of its parameters as it came, `identity` or `checkDefined`.
    struct PassedParameter {
        unsigned entryParameter { 0 };
        // What lies between the parameter and the return lets this through.
        ValueSetID mask;
        ValueFlowMaskKind maskKind { ValueFlowMaskKind::Plain };
    };
    struct ResultSummary {
        ValueFlowNodeID ownResult;
        Vector<PassedParameter, 2> passedParameters;
    };
    // Null for a function that returns none of its parameters.
    const ResultSummary* resultSummaryOf(FunctionID function) const
    {
        const ResultSummary& summary = m_resultSummaries[function.index()];
        return summary.ownResult ? &summary : nullptr;
    }

    uint32_t numberOfNodes() const { return m_numberOfNodes; }
    uint32_t firstNodeOf(FunctionID function) const { return m_firstNodeOfFunction[function.index()]; }
    uint32_t endOfNodesOf(FunctionID function) const { return m_firstNodeOfFunction[function.index() + 1]; }
    // None for a node of no body.
    FunctionID functionOf(ValueFlowNodeID) const;

private:
    friend class ValueFlowLinker;

    uint32_t m_firstVariable { 2 };
    uint32_t m_numberOfNodes { 0 };
    Vector<uint32_t> m_firstFieldOfLayout;
    // One entry more than there are functions.
    Vector<uint32_t> m_firstNodeOfFunction;
    Vector<uint32_t> m_firstResultOfFunction;
    Vector<uint32_t> m_thrownOfFunction;
    Vector<ResultSummary> m_resultSummaries;
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
        ValueFlowMaskKind kind { ValueFlowMaskKind::Checked };
        // `outside` also drops instancesOfRootClasses(). The rule that reads the mask asks mayHaveOtherLayoutThanItsClass().
        bool trustsInstancesOfRootClasses { false };

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

    // The node holds what the mask lets through of the constant.
    struct Seed {
        ValueFlowNodeName node;
        uint32_t constant { 0 };
        uint32_t mask { 0 };

        friend constexpr bool operator==(const Seed&, const Seed&) = default;
        friend constexpr auto operator<=>(const Seed&, const Seed&) = default;
    };

    // What the mask lets through of (the node's set | constant).
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
        // The first of the operation's sites in `sites`.
        uint32_t firstSite { none };
    };

    // What a branch or a check looks at. No rule runs for it: whether the test can go is asked of the operand once the values stand. `test` is none for the subject of a switch.
    struct ObservedValue {
        ValueID test;
        Operand operand;
    };

    // See ValueFlowProgramNodes::ResultSummary.
    struct PassedParameter {
        uint32_t entryParameter { 0 };
        uint32_t mask { 0 };
    };

    FunctionID function;
    // See ValueFlowUniverse::genericVersionOf().
    FunctionID genericVersion;
    uint32_t numberOfEntryParameters { 0 };
    uint32_t numberOfResults { 0 };
    bool hasYieldedAndSent { false };
    // The node's index in the body. `none` where the function returns none of its parameters.
    uint32_t ownResult { none };
    Vector<PassedParameter> passedParameters;
    // Of a constructor: whatever it answers is the object that it constructs.
    bool answersOnlyWhatItConstructs { false };
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
    Vector<Statement> statements;
    uint32_t firstStatementLeavingBody { 0 };
    Vector<Operand> operands;
    Vector<ObservedValue> observedValues;
    // The two sides of `==` and `===` where both may be functions. No rule runs for them: which closures can be told apart is asked of the operands once the values stand.
    struct ComparedValues {
        Operand left;
        Operand right;
    };
    Vector<ComparedValues> comparedValues;
    // Ascending. Nothing else in the body looks at them: no step behind the solve says anything about them.
    Vector<ValueID> valuesAliveForComparisonsOnly;

    uint32_t result(uint32_t index) const { return numberOfEntryParameters + index; }
    uint32_t thrown() const { return numberOfEntryParameters + numberOfResults; }
    uint32_t yielded() const { return thrown() + 1; }
    uint32_t sent() const { return thrown() + 2; }
    std::span<const Operand> operandsOf(const Statement& statement) const LIFETIME_BOUND { return operands.subspan(statement.firstOperand, statement.operandCount); }
};

// Reads nothing but the program, so every body can be reduced on a thread of its own.
ValueFlowBody reduceBody(const Program&, const ValueFlowBuiltinTables&, FunctionID, const UnobservedFieldDefinitions* = nullptr, FunctionID genericVersion = { });
// What the host does of its own accord: it enters the entry points and the functions it calls, with the unknown object as every operand. It has the number of no function.
ValueFlowBody makeHostBody(const Program&, const ValueFlowBuiltinTables&);
bool startsWithoutValueInSolve(const Program&, const Variable&);

struct ValueFlowObservedValue {
    FunctionID function;
    // None for the subject of a switch.
    ValueID test;
    ValueFlowOperand operand;
};

struct ValueFlowComparedValues {
    FunctionID function;
    ValueFlowOperand left;
    ValueFlowOperand right;
};

struct ValueFlowValueOfFunction {
    FunctionID function;
    ValueID value;
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
    std::span<const ValueFlowNodeMask> masks() const LIFETIME_BOUND { return m_masks.span(); }
    std::span<const ValueFlowStatement> statements() const LIFETIME_BOUND { return m_statements.span(); }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_operands.span().subspan(statement.firstOperand, statement.operandCount); }
    std::span<const ValueFlowOperand> operands() const LIFETIME_BOUND { return m_operands.span(); }
    std::span<const ValueFlowEdge> edgesOnceReached() const LIFETIME_BOUND { return m_edgesOnceReached.span(); }
    std::span<const ValueFlowNodeSet> seedsOnceReached() const LIFETIME_BOUND { return m_seedsOnceReached.span(); }
    std::span<const FunctionID> functionsEnteredOnceReached() const LIFETIME_BOUND { return m_functionsEnteredOnceReached.span(); }
    std::span<const ValueFlowBodyRange> ranges() const LIFETIME_BOUND { return m_ranges.span(); }
    std::span<const ValueFlowObservedValue> observedValues() const LIFETIME_BOUND { return m_observedValues.span(); }
    std::span<const ValueFlowComparedValues> comparedValues() const LIFETIME_BOUND { return m_comparedValues.span(); }
    // Ascending by function and value.
    std::span<const ValueFlowValueOfFunction> valuesAliveForComparisonsOnly() const LIFETIME_BOUND { return m_valuesAliveForComparisonsOnly.span(); }
    const UnobservedFieldDefinitions& unobservedDefinitions() const LIFETIME_BOUND { return m_unobservedDefinitions; }
    const ValueFlowBodiesTimes& times() const { return m_times; }

    // The host's body comes after the last function.
    uint32_t numberOfBodies() const { return m_ranges.size() - 1; }
    uint32_t hostBody() const { return numberOfBodies() - 1; }
    std::span<const ValueFlowStatement> statementsOf(uint32_t body) const LIFETIME_BOUND { return m_statements.span().subspan(m_ranges[body].statement, m_ranges[body + 1].statement - m_ranges[body].statement); }
    std::span<const ValueFlowStatement> statementsStayingInside(uint32_t body) const LIFETIME_BOUND { return m_statements.span().subspan(m_ranges[body].statement, m_ranges[body].statementLeavingBody - m_ranges[body].statement); }
    std::span<const ValueFlowEdge> edgesOnceReached(uint32_t body) const LIFETIME_BOUND { return m_edgesOnceReached.subspan(m_ranges[body].edgeOnceReached, m_ranges[body + 1].edgeOnceReached - m_ranges[body].edgeOnceReached); }
    std::span<const ValueFlowNodeSet> seedsOnceReached(uint32_t body) const LIFETIME_BOUND { return m_seedsOnceReached.subspan(m_ranges[body].seedOnceReached, m_ranges[body + 1].seedOnceReached - m_ranges[body].seedOnceReached); }
    std::span<const FunctionID> functionsEnteredOnceReached(uint32_t body) const LIFETIME_BOUND { return m_functionsEnteredOnceReached.subspan(m_ranges[body].functionEntered, m_ranges[body + 1].functionEntered - m_ranges[body].functionEntered); }

private:
    friend class ValueFlowLinker;

    std::unique_ptr<ValueSets> m_sets;
    ValueFlowUniverse m_universe;
    ValueFlowProgramNodes m_nodes;
    Vector<ValueFlowEdge> m_edges;
    Vector<ValueFlowNodeMask> m_masks;
    // Each body writes its own stretch of the two, and nobody writes them before that.
    MallocSpan<ValueFlowStatement> m_statements;
    MallocSpan<ValueFlowOperand> m_operands;
    Vector<ValueFlowEdge> m_edgesOnceReached;
    Vector<ValueFlowNodeSet> m_seedsOnceReached;
    Vector<FunctionID> m_functionsEnteredOnceReached;
    Vector<ValueFlowObservedValue> m_observedValues;
    Vector<ValueFlowComparedValues> m_comparedValues;
    Vector<ValueFlowValueOfFunction> m_valuesAliveForComparisonsOnly;
    UnobservedFieldDefinitions m_unobservedDefinitions;
    ValueFlowBodiesTimes m_times;
    // One entry more than there are bodies.
    Vector<ValueFlowBodyRange> m_ranges;
};

// The values whose type rests on what a Check lets through.
BitVector findValuesBehindChecks(const Program&, const Function&, const CFG&, const BitVector* checksThatTestNothing = nullptr);

// A function whose calls flow tells apart.
struct ValueFlowInstancedFunction {
    FunctionID function;
    // Among the positional parameters.
    unsigned positionOfKey { 0 };
    // The edges and the seeds of the bodies that end at a node of the function.
    Vector<ValueFlowEdge> edges;
    Vector<ValueFlowNodeSet> seeds;
};

class ValueFlowInstancedFunctions {
    WTF_MAKE_NONCOPYABLE(ValueFlowInstancedFunctions);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowInstancedFunctions);
public:
    // For each function the positional parameter that is a key; noKey for most.
    static constexpr uint32_t noKey = UINT32_MAX;
    static Vector<uint32_t> findParametersThatAreKeys(const Program&, const ParallelLoops&);

    ValueFlowInstancedFunctions(const Program&, const ValueFlowBodies&, std::span<const uint32_t> positionOfKey, ValueSetMaker&, const ParallelLoops&);

    bool isEmpty() const { return m_functions.isEmpty(); }
    // Null for most functions.
    const ValueFlowInstancedFunction* find(FunctionID function) const { return function && function.index() < m_indexOfFunction.size() && m_indexOfFunction[function.index()] ? &m_functions[m_indexOfFunction[function.index()] - 1] : nullptr; }
    bool holdsNode(ValueFlowNodeID node) const { return node.index() < m_isNodeOfFunction.size() && m_isNodeOfFunction.quickGet(node.index()); }
    ValueSetID closures() const { return m_closures; }

    // It is no key, so it stands for nothing else among the markers of a function.
    static constexpr ValueFlowObjectID markerOfOtherCalls() { return ValueFlowUniverse::primitive(PrimitiveSet::Undefined); }
    // A call that may pass more constants than this is one of the other calls, and so is a call with a new constant once the function has this many instances. A larger body has no instances.
    static constexpr unsigned mostMarkersOfCall = 8;
    static constexpr unsigned mostInstances = 64;
    static constexpr uint32_t mostNodesOfBody = 1024;

private:
    Vector<ValueFlowInstancedFunction> m_functions;
    // One more than the index in the list; zero for none.
    Vector<uint32_t> m_indexOfFunction;
    BitVector m_isNodeOfFunction;
    ValueSetID m_closures;
};

// Reduces every body on the threads of the build and numbers objects and nodes by prefix sums over the bodies: but for the numbers of sets the result is the same on any number of threads.
ValueFlowBodies reduceBodies(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
