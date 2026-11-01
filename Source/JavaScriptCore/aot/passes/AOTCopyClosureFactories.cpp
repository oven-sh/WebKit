/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCopyClosureFactories.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCirclesOfModuleLoads.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPhaseTimes.h"
#include "AOTRegionCloner.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <ranges>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxValuesInCopiedFactory, 200);
AOT_THRESHOLD(unsigned, maxValuesInCopiedClassFactory, 2000);
// How many times the size of the program as read all copies of factories together may have.
AOT_THRESHOLD(unsigned, maxGrowthByCopiedFactories, 4);
AOT_THRESHOLD(unsigned, maxValuesInCopiedMaker, 400);
AOT_THRESHOLD(unsigned, maxCopiesOfMaker, 128);
AOT_THRESHOLD(unsigned, maxDepthOfCopiedCalls, 3);
AOT_THRESHOLD(unsigned, maxValuesInAllCopiesOfMakers, 1 << 18);
AOT_THRESHOLD(unsigned, maxStepsToExpectedValue, 12);
AOT_THRESHOLD(unsigned, maxWrittenNamesOfLoop, 16);

bool namesObjectCreate(const BuiltinTable& builtins, const Value& value)
{
    return value.opcode == Opcode::Builtin && builtins.canonicalID(value.builtin()) == Builtins::ObjectCreate;
}

bool createsObjectBelowItsArgument(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call || operation.expectedResult() || operation.variadicOperands().size() != 1)
        return false;
    return namesObjectCreate(builtins, function.at(definitionThroughUpcasts(function, operation.calleeOperand())));
}

static bool isCodeOfProgram(const Function& function)
{
    return function.module && function.role == FunctionRole::Source && function.hasBody();
}

// A copy of the function does what the function does unless the function can tell which closure was called.
static bool canBeCopiedForOneCall(const Function& function)
{
    static constexpr OptionSet<FunctionFlags> flagsOfOtherFunctions { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget };
    if (!isCodeOfProgram(function) || function.hasAny(flagsOfOtherFunctions) || function.values.size() > maxValuesInCopiedMaker)
        return false;
    return function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
}

static ValueID definitionBehindDeadZoneChecks(const Function& function, ValueID id)
{
    for (;;) {
        id = definitionThroughUpcasts(function, id);
        const Value& value = function.at(id);
        if (value.opcode != Opcode::CheckInitialized && value.opcode != Opcode::Convert)
            return id;
        id = value.valueOperand();
    }
}

static bool isCapture(const Function& function, ValueID id)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    return std::ranges::contains(parameters.first(std::min<size_t>(function.captures.size(), parameters.size())), id);
}

static void forEachValueOf(const Function& function, NOESCAPE const Invocable<void(ProgramPoint, ValueID, const Value&)> auto& functor)
{
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const auto& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index)
            functor({ BlockID(blockIndex), index }, values[index], function.at(values[index]));
    }
}

// Field initializers and static blocks are functions that are called directly, with their captures as arguments.
static bool referencesNestedFunction(const Program& program, const Value& value)
{
    return value.opcode == Opcode::NewClosure || (value.opcode == Opcode::CallDirect && isCodeOfProgram(program.at(value.function())));
}

namespace {

// A function and every function nested in it.
struct NestedFunctions {
    bool contains(FunctionID id) const { return indices.contains(id.index() + 1); }

    Vector<FunctionID, 8> functions;
    UncheckedKeyHashSet<uint32_t> indices;
    unsigned numberOfValues { 0 };
    bool isRecursive { false };
};

} // anonymous namespace

static void addNestedFunctions(const Program& program, FunctionID id, NestedFunctions& nested, UncheckedKeyHashSet<uint32_t>& ancestors)
{
    if (ancestors.contains(id.index() + 1)) {
        nested.isRecursive = true;
        return;
    }
    if (!nested.indices.add(id.index() + 1).isNewEntry)
        return;
    const Function& function = program.at(id);
    nested.functions.append(id);
    nested.numberOfValues += function.values.size();
    ancestors.add(id.index() + 1);
    forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
        if (referencesNestedFunction(program, value))
            addNestedFunctions(program, value.function(), nested, ancestors);
    });
    ancestors.remove(id.index() + 1);
}

static NestedFunctions nestedFunctionsOf(const Program& program, FunctionID id)
{
    NestedFunctions nested;
    UncheckedKeyHashSet<uint32_t> ancestors;
    addNestedFunctions(program, id, nested, ancestors);
    return nested;
}

// `Base => class extends Base { .. }`
static LayoutID classReturnedBy(const Function& function)
{
    LayoutID result;
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = block.terminator.valueOperand();
        const Value* definition = returned ? &function.at(definitionBehindDeadZoneChecks(function, returned)) : nullptr;
        if (!definition || definition->opcode != Opcode::FinishClass || (result && result != definition->layout()))
            return { };
        result = definition->layout();
    }
    return result;
}

static bool returnsClosureItCreates(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        if (block.terminator.kind != TerminatorKind::Return)
            return false;
        ValueID returned = block.terminator.valueOperand();
        return returned && function.at(definitionBehindDeadZoneChecks(function, returned)).opcode == Opcode::NewClosure;
    });
}

static std::optional<unsigned> positionalParameterAt(const Function& function, ValueID id)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    unsigned first = function.firstParameterIndex();
    for (unsigned index = 0; index < function.numberOfPositionalParameters() && first + index < parameters.size(); ++index) {
        if (parameters[first + index] == id)
            return index;
    }
    return std::nullopt;
}

// `object[name] = function () { .. }`   `object.__defineGetter__(name, function () { .. })`   `Object.defineProperty(object, name, { get() { .. } })`
// The functor is handed each value that names a closure the function creates.
static void forEachNameOfCreatedClosure(const Function& function, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    BitVector literalsThatHoldClosures;
    auto isCreatedClosure = [&](ValueID id) {
        return function.at(definitionBehindDeadZoneChecks(function, id)).opcode == Opcode::NewClosure;
    };
    auto holdsCreatedClosure = [&](ValueID id) {
        id = definitionBehindDeadZoneChecks(function, id);
        if (function.at(id).opcode == Opcode::FinishObject)
            id = definitionBehindDeadZoneChecks(function, function.at(id).operand(0));
        return function.at(id).opcode == Opcode::NewClosure || literalsThatHoldClosures.get(id.index());
    };
    forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
        if (value.opcode == Opcode::PropDefine) {
            if (isCreatedClosure(value.valueOperand()))
                literalsThatHoldClosures.set(definitionBehindDeadZoneChecks(function, value.operand(OperandIndex::PropDefine::object)).index());
            return;
        }
        if (value.opcode == Opcode::ElemSet) {
            if (!isCreatedClosure(value.valueOperand()))
                return;
            functor(definitionBehindDeadZoneChecks(function, value.operand(OperandIndex::ElemSet::property)));
            return;
        }
        if (value.opcode != Opcode::Call || !std::ranges::any_of(value.variadicOperands(), holdsCreatedClosure))
            return;
        for (ValueID argument : value.variadicOperands())
            functor(definitionBehindDeadZoneChecks(function, argument));
    });
}

static void forEachParameterThatNamesCreatedClosure(const Function& function, NOESCAPE const Invocable<void(unsigned)> auto& functor)
{
    forEachNameOfCreatedClosure(function, [&](ValueID name) {
        if (std::optional<unsigned> parameter = positionalParameterAt(function, name))
            functor(*parameter);
    });
}

static bool isThisOf(const Function& function, ValueID id)
{
    id = definitionBehindDeadZoneChecks(function, id);
    if (function.at(id).opcode == Opcode::ToThis)
        id = definitionBehindDeadZoneChecks(function, function.at(id).operand(0));
    std::optional<unsigned> index = function.thisParameterIndex();
    return index && function.at(entryBlock).parameters[*index] == id;
}

// The values of a program as read have no types yet, and a test asks for the type of what it tests.
static ValueID readWithItsType(Function& function, ValueID id)
{
    id = definitionBehindDeadZoneChecks(function, id);
    RELEASE_ASSERT(function.at(id).opcode == Opcode::PropGet);
    function.edit(id).type = TypeTable::anything;
    return id;
}

// What types the values later asks the first value of a function whether that is done.
static void removeTypesOfValues(Function& function)
{
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID id : function.blocks[blockIndex].values) {
            if (function.at(id).type)
                function.edit(id).type = TypeID();
        }
    }
}

static ProgramPoint pointOf(const Function& function, ValueID id)
{
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (size_t index = function.blocks[blockIndex].values.find(id); index != notFound)
            return { BlockID(blockIndex), static_cast<uint32_t>(index) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// With a copy of its own, the call defines members whose names the compiler can read.
static bool handsWrittenNameToDefineClosureUnder(const Function& caller, const Value& call, const Function& callee)
{
    bool result = false;
    std::span<const ValueID> arguments = call.variadicOperands();
    forEachParameterThatNamesCreatedClosure(callee, [&](unsigned parameter) {
        result |= parameter < arguments.size() && caller.at(definitionBehindDeadZoneChecks(caller, arguments[parameter])).opcode == Opcode::String;
    });
    return result;
}

namespace {

enum class ReasonToCopy : uint8_t { IsClosureFactory, MakesWhatObjectCreateIsHanded, ConstructsPrototypeObject, ConstructsPrototypeObjectByWhatItIsHanded };

// `Below.prototype = new Above()` in a function that takes Above.
struct ConstructionByParameter {
    ValueID construction;
    unsigned position { 0 };
};

struct CallToCopyFor {
    ProgramPoint point;
    ValueID call;
    FunctionID callee;
    // The closure that is called, where the caller creates it.
    ValueID closure;
    ReasonToCopy reason;
    // The field of an environment that the closure was read from: the call gets a copy if the program stores to the field at one place.
    std::optional<FieldReference> holder { };
    bool returnsClass { false };
    FunctionID copy { };
    // Where the callee constructs by one of its parameters: the construction, and the function that this call hands in there.
    ValueID constructionInCallee { };
    FunctionID constructorHandedIn { };
    // The callee was read from a `var` of another file, which answers undefined until that file has stored it: the call asks.
    bool testsCallee { false };
};

// A call that gets a copy, as it stood before: where the callee turns out not to be stored yet, this runs.
struct CallAsWritten {
    // A value that was put in front of everything the copy needs.
    ValueID standsInFront;
    Value call;
};

static uint64_t makeFieldKey(FieldReference field)
{
    return static_cast<uint64_t>(field.layout.index()) + 1 << 32 | field.field;
}

// What a value is if the program runs the way its text reads. Nothing rests on it: a copy that is made for it tests it where it uses it.
struct ExpectedValue {
    enum class Kind : uint8_t { Unknown, Closure, Instance, Text, Literal };
    Kind kind { Kind::Unknown };
    // The function of the closure, the place in the list of instances, the string, or the layout of the literal.
    uint32_t index { 0 };

    explicit operator bool() const { return kind != Kind::Unknown; }
    friend bool operator==(const ExpectedValue&, const ExpectedValue&) = default;
};

using ExpectedValueList = Vector<ExpectedValue, 4>;

// `new Helper(target, "name")`
struct ExpectedInstance {
    FunctionID constructor;
    ExpectedValueList arguments;

    friend bool operator==(const ExpectedInstance&, const ExpectedInstance&) = default;
};

struct Activation {
    FunctionID function;
    ExpectedValue thisValue { };
    ExpectedValueList arguments { };
    // Code that runs once, or a copy that one call has to itself: what is expected of its values is kept.
    bool isOnlyActivation { false };
};

struct ExpectedCall {
    ValueID call;
    Activation callee;
    FunctionID copy { };
    // The objects that the copy captures, each with the value of the caller that holds it.
    Vector<std::pair<LayoutID, ValueID>, 2> literals { };
};

struct ExpectedRead {
    ValueID read;
    ExpectedValue expected;
};

struct CopiedClass {
    LayoutID original;
    LayoutID copy;
};

using ClassReference = std::pair<LayoutID, FunctionID>;

struct Size {
    unsigned numberOfValuesWithClosures { 0 };
    bool definesWhatStandsAtOnePlace { false };
};

struct Copier {
    ProgramEditor& editor;
    BuildContext& build;
    const Program& program { editor.program() };
    TypeArena types { editor.types() };
    // The functions that create closures which hand Object.create what they captured.
    BitVector makers;
    // The constants of modules that somebody hands to Object.create.
    BitVector constantsHandedToObjectCreate;
    bool isAnyConstantPassedToObjectCreate { false };
    // The bodies of the modules of the program, and what they call at one place outside loops.
    Vector<FunctionID> callersThatRunOnce;
    BitVector isCallerThatRunsOnce;
    UncheckedKeyHashMap<uint32_t, unsigned> numberOfCopiesOfMaker;
    UncheckedKeyHashMap<uint32_t, unsigned> numberOfCallsOfFactory;
    // By constant of a module: the function whose closure it holds. The constants of a module are entered when the first of them is asked for.
    UncheckedKeyHashMap<uint32_t, uint32_t> functionsByConstant;
    UncheckedKeyHashMap<uint32_t, LayoutID> classesByConstant;
    BitVector modulesWithConstantsEntered;
    UncheckedKeyHashMap<uint32_t, Size> sizes;
    UncheckedKeyHashMap<uint32_t, std::optional<ConstructionByParameter>> constructionsByParameters;
    std::optional<BitVector> functionsNamedByClosedTypes;
    // Sorted. Each class with each function that has an operation on the class, on one of its private names or on one of its static fields.
    std::optional<Vector<ClassReference>> classReferences;
    BitVector copies;
    UncheckedKeyHashMap<uint32_t, FunctionID> originalsOfCopies;
    // The layouts of the environments of the copies that are being made, each with its original.
    Vector<std::pair<LayoutID, LayoutID>, 4> copiedEnvironments;
    Vector<std::pair<FunctionID, FunctionID>, 8> copiedNestedFunctions;
    // While a function that evaluates classes is copied: the classes, and what belongs to them.
    Vector<CopiedClass, 2> copiedClasses;
    Vector<std::pair<VariableID, VariableID>, 2> copiedStaticFields;
    Vector<std::pair<PropertyKeyID, PropertyKeyID>, 4> copiedPrivateNames;
    Vector<std::pair<FunctionID, FunctionID>, 8> copiedFunctionsOfClasses;
    UncheckedKeyHashMap<uint32_t, LayoutID> classesReturnedByCopies;
    // Each with the class that it is read as a class below from now on.
    Vector<std::pair<LayoutID, LayoutID>> classesPutBelowOthers;
    bool didCopyClasses { false };
    Vector<ExpectedInstance> expectedInstances;
    UncheckedKeyHashMap<uint64_t, ExpectedValue> expectedValuesInOnlyActivations;
    // By function and layout: the value that holds the object which the program writes out. In code that runs once that is the literal, in a copy a capture.
    UncheckedKeyHashMap<uint64_t, ValueID> holdersOfLiterals;
    // What the code that runs once stores: closures in variables of modules and in fields of environments, methods in `prototype` objects and in literals. No function where there are two.
    bool didNoteStoresOfCodeThatRunsOnce { false };
    UncheckedKeyHashMap<uint32_t, FunctionID> closuresInVariables;
    UncheckedKeyHashMap<uint64_t, FunctionID> closuresInEnvironments;
    UncheckedKeyHashMap<uint64_t, FunctionID> methodsOfConstructors;
    UncheckedKeyHashMap<uint64_t, FunctionID> methodsOfLiterals;
    // By constant of a module: the array that the program writes out and stores there.
    UncheckedKeyHashMap<uint32_t, DataID> arraysInConstants;
    std::optional<BitVector> modulesThatRequireMayLoad;
    bool requiresByNameNobodyKnows { false };
    std::optional<StronglyConnectedComponents> circlesOfLoads;
    UncheckedKeyHashMap<uint32_t, bool> variablesStoredOnEveryWayToEnd;
    unsigned numberOfValuesAsRead { 0 };
    unsigned numberOfValuesInCopiesOfFactories { 0 };
    unsigned numberOfValuesInCopiesOfMakers { 0 };
    uint64_t numberOfValuesReadForClassesBelowCopies { 0 };
    uint64_t numberOfMembersReadForTypesOfCopies { 0 };

    bool isConstantOfModule(VariableID variable) const { return program.at(variable).flags.contains(VariableFlags::IsImmutable) && program.at(variable).module; }

    void addCallerThatRunsOnce(FunctionID id)
    {
        if (isCodeOfProgram(program.at(id)) && isCallerThatRunsOnce.add(id.index()))
            callersThatRunOnce.append(id);
    }

    void findBodies()
    {
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            const Function& function = program.functions[index];
            numberOfValuesAsRead += function.values.size();
            if (isCodeOfProgram(function) && function.isModuleBody())
                addCallerThatRunsOnce(FunctionID(index));
        }
    }

    // `const make = options => { const members = { .. }; return function (value) { const self = Object.create(members); .. }; }`   `const members = makeMembers({ .. })   .. Object.create(members)`
    void findMakers(std::span<const FunctionID> functionsThatNameObjectCreate)
    {
        BitVector creators;
        BitVector modules;
        for (FunctionID id : functionsThatNameObjectCreate) {
            const Function& function = program.at(id);
            if (!isCodeOfProgram(function))
                continue;
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
                if (!createsObjectBelowItsArgument(program.builtins(), function, value))
                    return;
                ValueID handed = definitionBehindDeadZoneChecks(function, value.variadicOperands()[0]);
                if (isCapture(function, handed)) {
                    creators.set(id.index());
                    modules.set(function.module.index());
                    return;
                }
                const Value& definition = function.at(handed);
                if (definition.opcode == Opcode::GetModuleVariable && isConstantOfModule(definition.variable())) {
                    constantsHandedToObjectCreate.set(definition.variable().index());
                    isAnyConstantPassedToObjectCreate = true;
                }
            });
        }
        if (creators.isEmpty())
            return;

        // The function that creates closures of a function stands in the same module.
        for (uint32_t index = 0; index < program.numberOfFunctionsAsRead(); ++index) {
            const Function& function = program.functions[index];
            if (!isCodeOfProgram(function) || function.isModuleBody() || !modules.get(function.module.index()) || !function.captures.isEmpty() || !canBeCopiedForOneCall(function))
                continue;
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
                if (value.opcode == Opcode::NewClosure && creators.get(value.function().index()))
                    makers.set(index);
            });
        }
    }

    FunctionID functionHeldBy(VariableID variable)
    {
        if (!isConstantOfModule(variable))
            return { };
        // `function make() { .. }`
        if (program.at(variable).initialization == VariableInitialization::Function)
            return program.at(variable).function;
        auto module = program.at(variable).module;
        FunctionID body = program.at(module).body;
        if (modulesWithConstantsEntered.add(module.index()) && body && program.at(body).hasBody()) {
            const Function& function = program.at(body);
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
                if (value.opcode != Opcode::PutModuleVariable || !program.at(value.variable()).flags.contains(VariableFlags::IsImmutable))
                    return;
                const Value& stored = function.at(definitionBehindDeadZoneChecks(function, value.valueOperand()));
                if (stored.opcode == Opcode::NewClosure)
                    functionsByConstant.add(value.variable().index() + 1, stored.function().index());
                else if (stored.opcode == Opcode::FinishClass)
                    classesByConstant.add(value.variable().index() + 1, stored.layout());
            });
        }
        auto found = functionsByConstant.find(variable.index() + 1);
        return found == functionsByConstant.end() ? FunctionID() : FunctionID(found->value);
    }

    bool mayBeLoadedByRequire(ModuleID asked)
    {
        if (!modulesThatRequireMayLoad) {
            modulesThatRequireMayLoad.emplace(program.modules.size());
            for (const Module& module : program.modules) {
                for (const ModuleRequest& request : module.requests) {
                    if (request.kind == ModuleRequestKind::Require)
                        modulesThatRequireMayLoad->quickSet(request.target.index());
                }
            }
            std::atomic<bool> isFound { false };
            forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
                const Function& function = program.functions[index];
                if (!isCodeOfProgram(function) || isFound.load(std::memory_order_relaxed))
                    return;
                if (std::ranges::any_of(function.values, [](const Value& value) { return value.opcode == Opcode::RequireDynamic; }))
                    isFound.store(true, std::memory_order_relaxed);
            });
            requiresByNameNobodyKnows = isFound.load(std::memory_order_relaxed);
        }
        return requiresByNameNobodyKnows || modulesThatRequireMayLoad->quickGet(asked.index());
    }

    bool lieOnOneCircleOfLoads(ModuleID first, ModuleID second)
    {
        if (!circlesOfLoads)
            circlesOfLoads = computeCirclesOfModuleLoads(program, build.parallelLoops());
        return circlesOfLoads->componentOf[first.index()] == circlesOfLoads->componentOf[second.index()];
    }

    // The loader runs the body of a file when every file that it imports by a statement has run to its end, unless the two lie on a cycle.
    bool isStoredBeforeBodyStarts(VariableID variable, FunctionID callerID)
    {
        ModuleID exporter = program.at(variable).module;
        ModuleID importer = program.at(callerID).module;
        if (!importer || importer == exporter || program.at(importer).body != callerID)
            return false;
        FunctionID bodyID = program.at(exporter).body;
        if (!bodyID || !program.at(bodyID).hasBody() || program.at(bodyID).has(FunctionFlags::IsAsync))
            return false;
        bool isImportedByStatement = std::ranges::any_of(program.at(importer).requests, [&](const ModuleRequest& request) {
            return request.target == exporter && request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation && request.completes;
        });
        if (!isImportedByStatement || mayBeLoadedByRequire(importer) || lieOnOneCircleOfLoads(exporter, importer))
            return false;
        return variablesStoredOnEveryWayToEnd.ensure(variable.index() + 1, [&] {
            const Function& body = program.at(bodyID);
            ValueID store;
            forEachValueOf(body, [&](ProgramPoint, ValueID id, const Value& value) {
                if (value.opcode == Opcode::PutModuleVariable && value.variable() == variable)
                    store = id;
            });
            if (!store)
                return false;
            AnalysisContext context { build, program, nullptr, nullptr, bodyID };
            FunctionAnalyses analyses(context, body);
            for (uint32_t block = 0; block < body.blocks.size(); ++block) {
                if (body.blocks[block].terminator.kind == TerminatorKind::Return && !analyses.dominates(store, ProgramPoint { BlockID(block), static_cast<uint32_t>(body.blocks[block].values.size()) }))
                    return false;
            }
            return true;
        }).iterator->value;
    }

    // `class Base { .. }`, read in another function or module.
    LayoutID classHeldBy(VariableID variable)
    {
        functionHeldBy(variable);
        return classesByConstant.get(variable.index() + 1);
    }

    // A type that lists functions and admits no others holds no closure of a copy of one of them.
    bool isNamedByClosedType(FunctionID id)
    {
        // What the types say of a function as read, the types in a copy say of its copy.
        while (FunctionID original = originalsOfCopies.get(id.index() + 1))
            id = original;
        if (!functionsNamedByClosedTypes) {
            functionsNamedByClosedTypes.emplace(program.functions.size());
            for (uint32_t index = 0; index < program.types.size(); ++index) {
                const Type& type = program.types.at(TypeID(index));
                if (type.anyFunction || type.anyObject)
                    continue;
                for (FunctionID function : type.functions)
                    functionsNamedByClosedTypes->quickSet(function.index());
            }
        }
        return functionsNamedByClosedTypes->quickGet(id.index());
    }

    // A class, a function that constructs objects of a layout and is not copied with it, the array of a tagged template and the scope of an eval stand at one place of the program.
    // The closure of the copy that is called is held by nothing but the call, so no type speaks of it.
    bool definesWhatStandsAtOnePlace(FunctionID root, const NestedFunctions& nested, bool isHeldByCallAlone = true, LayoutID classThatIsCopied = { })
    {
        if (nested.isRecursive)
            return true;
        return std::ranges::any_of(nested.functions, [&](FunctionID id) {
            bool isHeldByOthers = id != root || !isHeldByCallAlone;
            const Function& function = program.at(id);
            bool constructsWhatStandsAtOnePlace = function.constructedClass && function.constructedClass != classThatIsCopied && (id == root || !isCopiedWithItsConstructor(function.constructedClass, nested));
            bool result = constructsWhatStandsAtOnePlace || (isHeldByOthers && isNamedByClosedType(id));
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
                if (value.opcode == Opcode::NewClass || value.opcode == Opcode::NewClassToken)
                    result |= isHeldByOthers || value.layout() != classThatIsCopied;
                else
                    result |= value.opcode == Opcode::NewTemplateObject || value.opcode == Opcode::DirectEval;
            });
            return result;
        });
    }

    struct ClassReferenceFinder {
        void operator()(const LayoutID& id) const
        {
            ClassReference reference { id, function };
            if (id && program.at(id).kind == LayoutKind::Instance && (found.isEmpty() || found.last() != reference))
                found.append(reference);
        }
        void operator()(const VariableID& id) const { (*this)(id ? program.at(id).owner : LayoutID()); }
        void operator()(const PropertyKeyID& id) const { (*this)(id ? program.at(id).owner : LayoutID()); }
        template<typename Tag> void operator()(const ID<Tag>&) const { }

        const Program& program;
        FunctionID function;
        Vector<ClassReference>& found;
    };

    void findClassReferences()
    {
        classReferences = collectInParallel<ClassReference>(build.parallelLoops(), program.numberOfFunctionsAsRead(), [&](unsigned index, Vector<ClassReference>& found) {
            ClassReferenceFinder finder { program, FunctionID(index), found };
            for (const Value& value : program.functions[index].values)
                forEachReference(value, finder);
        });
        AOT::sort(*classReferences);
        classReferences->shrink(std::ranges::unique(*classReferences).begin() - classReferences->begin());
    }

    // The entries of a WeakMap or WeakSet that holds private state are a hidden field of the class. Every function that sees the collection accesses the field, with a token that is created where the collection is.
    bool isReferencedOutside(LayoutID classLayout, const NestedFunctions& nested)
    {
        if (!classReferences)
            findClassReferences();
        return std::ranges::any_of(std::ranges::equal_range(*classReferences, classLayout, { }, &ClassReference::first), [&](const ClassReference& reference) {
            return !nested.contains(reference.second);
        });
    }

    bool isCopiedWithItsConstructor(LayoutID constructed, const NestedFunctions& nested)
    {
        return program.at(constructed).has(LayoutFlags::IsConstructorFunction) && !isReferencedOutside(constructed, nested);
    }

    // A copy of the function evaluates a copy of the class: a class of its own, with its own private names and static fields, as each evaluation of the class has anyway.
    bool isClassFactory(FunctionID id, const NestedFunctions& nested)
    {
        LayoutID returned = classReturnedBy(program.at(id));
        if (!returned || program.at(returned).kind != LayoutKind::Instance || program.at(returned).computedKeyCount)
            return false;
        return !definesWhatStandsAtOnePlace(id, nested, true, returned) && !isReferencedOutside(returned, nested);
    }

    // The copy gives the layout up, which goes on standing at one place.
    bool canBeCopiedWithoutItsLayout(FunctionID id)
    {
        const Function& function = program.at(id);
        if (function.kind != FunctionKind::Plain || !function.constructedClass)
            return false;
        NestedFunctions nested = nestedFunctionsOf(program, id);
        return nested.numberOfValues <= maxValuesInCopiedMaker && !definesWhatStandsAtOnePlace(id, nested, true, function.constructedClass);
    }

    bool isStoredAsPrototypeObject(const Function& function, FunctionAnalyses& analyses, ValueID made)
    {
        PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
        Vector<ValueID, 4> sameValues { made };
        for (size_t index = 0; index < sameValues.size(); ++index) {
            for (const Use& use : analyses.useLists().usesOf(sameValues[index])) {
                if (use.kind != Use::Kind::Operand)
                    continue;
                const Value& user = function.at(use.user);
                if (user.opcode == Opcode::Convert || user.opcode == Opcode::Upcast)
                    sameValues.append(use.user);
                else if (user.opcode == Opcode::PropSet && !user.isFreeName() && user.key() == prototype && user.valueOperand() == sameValues[index])
                    return true;
            }
        }
        return false;
    }

    std::optional<ConstructionByParameter> constructionByParameterIn(FunctionID id)
    {
        return constructionsByParameters.ensure(id.index() + 1, [&]() -> std::optional<ConstructionByParameter> {
            const Function& function = program.at(id);
            bool constructs = std::ranges::any_of(function.values, [](const Value& value) { return value.opcode == Opcode::Construct; });
            if (!constructs)
                return std::nullopt;
            AnalysisContext context { build, program, nullptr, nullptr, id };
            FunctionAnalyses analyses(context, function);
            std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
            std::optional<ConstructionByParameter> found;
            bool hasSeveral = false;
            forEachValueOf(function, [&](ProgramPoint point, ValueID made, const Value& value) {
                if (value.opcode != Opcode::Construct || value.newTargetOperand() || analyses.isInCycle(point.block))
                    return;
                size_t index = parameters.size();
                if (auto place = std::ranges::find(parameters, definitionBehindDeadZoneChecks(function, value.calleeOperand())); place != parameters.end())
                    index = place - parameters.begin();
                if (index == parameters.size() || index < function.firstParameterIndex() || !isStoredAsPrototypeObject(function, analyses, made))
                    return;
                hasSeveral |= !!found;
                found = ConstructionByParameter { made, static_cast<unsigned>(index - function.firstParameterIndex()) };
            });
            return hasSeveral ? std::nullopt : found;
        }).iterator->value;
    }

    Size sizeOf(FunctionID id)
    {
        return sizes.ensure(id.index() + 1, [&] {
            NestedFunctions nested = nestedFunctionsOf(program, id);
            return Size { nested.numberOfValues, definesWhatStandsAtOnePlace(id, nested) && !isClassFactory(id, nested) };
        }).iterator->value;
    }

    LayoutID copyOf(LayoutID layout) const
    {
        for (auto [original, copy] : copiedEnvironments) {
            if (original == layout)
                return copy;
        }
        return layout;
    }

    TypeID withCopiedEnvironment(TypeID type)
    {
        LayoutID environment = type ? program.at(type).environment : LayoutID();
        LayoutID copy = environment ? copyOf(environment) : environment;
        return copy == environment ? type : types.intern(Type::forEnvironment(copy));
    }

    // Flow keeps one answer per field of the layout of an environment, so the environments that a copy allocates have layouts of their own.
    void copyEnvironmentsOf(Function& copy)
    {
        forEachValueOf(copy, [&](ProgramPoint, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewObject && program.at(value.layout()).isEnvironment() && copyOf(value.layout()) == value.layout())
                copiedEnvironments.append({ value.layout(), editor.addLayout(Layout(program.at(value.layout()))) });
        });
        if (copiedEnvironments.isEmpty())
            return;
        for (DeclaredType& capture : copy.captures)
            capture.changeType(Proven::byPosition(withCopiedEnvironment(capture.written())));
        for (Value& value : copy.editAllValues()) {
            value.type = withCopiedEnvironment(value.type);
            if (value.opcode == Opcode::NewObject || value.opcode == Opcode::GetEnvironmentField || value.opcode == Opcode::PutEnvironmentField)
                value.setLayout(copyOf(value.layout()));
        }
    }

    template<typename IDType>
    static IDType copyIn(const auto& pairs, IDType id)
    {
        for (auto [original, copy] : pairs) {
            if (original == id)
                return copy;
        }
        return id;
    }

    LayoutID copyOfClass(LayoutID layout) const
    {
        for (const CopiedClass& copied : copiedClasses) {
            if (copied.original == layout)
                return copied.copy;
        }
        return layout;
    }

    // The closures that the copies create are closures of copies. Whoever names the function that was called, in its own body too, means what the program holds under that name.
    FunctionID copyOfFunctionOfClass(FunctionID id) const
    {
        return id == copiedFunctionsOfClasses.last().first ? id : copyIn(copiedFunctionsOfClasses, id);
    }

    LayoutSetID withCopiedClasses(LayoutSetID id)
    {
        if (!id)
            return id;
        Vector<LayoutID> members = program.at(id).written();
        numberOfMembersReadForTypesOfCopies += members.size();
        bool hasChanged = false;
        for (LayoutID& member : members) {
            LayoutID copy = copyOfClass(member);
            hasChanged |= std::exchange(member, copy) != copy;
        }
        if (!hasChanged)
            return id;
        AOT::sort(members);
        return types.intern(program.at(id).withMembers(WTF::move(members)));
    }

    // The layouts below the class stay in the type: what is true of fewer objects is true of these.
    TypeID withCopiedClasses(TypeID id)
    {
        if (!id)
            return id;
        Type type = program.at(id);
        numberOfMembersReadForTypesOfCopies += type.functions.size();
        LayoutSetID layouts = withCopiedClasses(type.layouts);
        bool hasChanged = std::exchange(type.layouts, layouts) != layouts;
        Vector<FunctionID, 8> functions(type.functions.size(), [&](size_t index) {
            return copyOfFunctionOfClass(type.functions[index]);
        });
        if (!std::ranges::equal(functions.span(), type.functions.span())) {
            type.functions = SortedSet<FunctionID>(functions.span());
            hasChanged = true;
        }
        for (ClassPart& part : type.classes) {
            for (TypeID& contents : part.writtenContents) {
                TypeID copy = withCopiedClasses(contents);
                hasChanged |= std::exchange(contents, copy) != copy;
            }
        }
        if (!hasChanged)
            return id;
        return types.intern(WTF::move(type));
    }

    struct TypesWithClassesBelow {
        TypeID inPlaceOf(TypeID id) const { return changes(id) ? types[id.index()] : id; }
        LayoutSetID inPlaceOf(LayoutSetID id) const { return changes(id) ? layoutSets[id.index()] : id; }
        bool changes(TypeID id) const { return id && id.index() < types.size() && types[id.index()]; }
        bool changes(LayoutSetID id) const { return id && id.index() < layoutSets.size() && layoutSets[id.index()]; }

        // By type and by set: what takes its place. Nothing where it stays.
        Vector<TypeID> types;
        Vector<LayoutSetID> layoutSets;
    };

    // In what was copied: the copies of the classes and of what belongs to them. With a table: in any record, the types and sets of the table.
    struct ReferencesToCopiedClasses {
        void operator()(TypeID& id) const { id = table ? table->inPlaceOf(id) : copier.withCopiedClasses(id); }
        void operator()(LayoutSetID& id) const { id = table ? table->inPlaceOf(id) : copier.withCopiedClasses(id); }
        void operator()(LayoutID& id) const { id = table ? id : copier.copyOfClass(id); }
        void operator()(VariableID& id) const { id = table ? id : copyIn(copier.copiedStaticFields, id); }
        void operator()(PropertyKeyID& id) const { id = table ? id : copyIn(copier.copiedPrivateNames, id); }
        void operator()(FunctionID& id) const { id = table ? id : copier.copyOfFunctionOfClass(id); }
        template<typename Tag> void operator()(ID<Tag>&) const { }

        Copier& copier;
        const TypesWithClassesBelow* table { nullptr };
    };

    LayoutID classCreatedBy(const Value& value) const
    {
        if (value.opcode == Opcode::NewClass)
            return value.layout();
        LayoutID constructed = value.opcode == Opcode::NewClosure ? program.at(value.function()).constructedClass : LayoutID();
        return constructed && program.at(constructed).has(LayoutFlags::IsConstructorFunction) ? constructed : LayoutID();
    }

    void copyClassesOf(const Function& copy)
    {
        forEachValueOf(copy, [&](ProgramPoint, ValueID, const Value& value) {
            LayoutID original = classCreatedBy(value);
            if (!original || copyOfClass(original) != original)
                return;
            LayoutID added = editor.addLayout(Layout(program.at(original)));
            copiedClasses.append({ original, added });
            // Whoever names the class as it is written means each of its evaluations.
            classesPutBelowOthers.append({ added, original });
            for (const Field& field : program.at(original).fields) {
                if (!field.key || program.at(field.key).owner != original)
                    continue;
                PropertyKey name = program.at(field.key);
                name.owner = added;
                copiedPrivateNames.append({ field.key, types.intern(name) });
            }
            for (const StaticField& field : program.at(original).staticFields) {
                Variable variable = program.at(field.variable);
                variable.owner = added;
                copiedStaticFields.append({ field.variable, editor.addVariable(WTF::move(variable)) });
            }
        });
    }

    // Everything that was copied names the copies of the classes and of what belongs to them.
    void finishCopiesOfClasses()
    {
        ReferencesToCopiedClasses rewriter { *this };
        for (auto [original, copy] : copiedFunctionsOfClasses) {
            Function& function = editor.at(copy);
            // These say which function the copy was made of.
            auto derivation = function.derivation;
            auto creator = function.creator;
            forEachReference(static_cast<FunctionRecord&>(function), rewriter);
            function.derivation = WTF::move(derivation);
            function.creator = WTF::move(creator);
            for (Value& value : function.editAllValues()) {
                value.type = withCopiedClasses(value.type);
                forEachReference(value, rewriter);
            }
            for (Block& block : function.editAllBlocks())
                forEachReference(block.terminator, rewriter);
        }
        for (const CopiedClass& copied : copiedClasses) {
            Layout& layout = editor.at(copied.copy);
            forEachReference(static_cast<LayoutRecord&>(layout), rewriter);
            layout.indexFields();
        }
        for (auto [original, copy] : copiedStaticFields)
            forEachReference(editor.at(copy), rewriter);
        copiedClasses.shrink(0);
        copiedStaticFields.shrink(0);
        copiedPrivateNames.shrink(0);
        copiedFunctionsOfClasses.shrink(0);
        didCopyClasses = true;
    }

    // The closures that the copy creates are closures of copies too, and so on below them.
    FunctionID copyWithItsClosures(FunctionID original, unsigned& numberOfValuesInCopies)
    {
        FunctionID result = copyNestedFunction(original, numberOfValuesInCopies);
        copiedNestedFunctions.shrink(0);
        return result;
    }

    FunctionID copyNestedFunction(FunctionID original, unsigned& numberOfValuesInCopies)
    {
        if (FunctionID existing = copyIn(copiedNestedFunctions, original); existing != original)
            return existing;
        Function copy = program.at(original);
        size_t numberOfEnvironmentsAround = copiedEnvironments.size();
        bool isOutermostWithClasses = copiedClasses.isEmpty();
        copyEnvironmentsOf(copy);
        copyClassesOf(copy);
        Vector<std::pair<ValueID, FunctionID>> closures;
        forEachValueOf(copy, [&](ProgramPoint, ValueID id, const Value& value) {
            if (referencesNestedFunction(program, value))
                closures.append({ id, value.function() });
        });
        for (auto [id, function] : closures)
            copy.edit(id).setFunction(copyNestedFunction(function, numberOfValuesInCopies));
        copiedEnvironments.shrink(numberOfEnvironmentsAround);
        numberOfValuesInCopies += copy.values.size();
        LayoutID returned = copiedClasses.isEmpty() ? LayoutID() : copyOfClass(classReturnedBy(copy));
        FunctionID result = editor.addFunctionLike(original, WTF::move(copy));
        copies.set(result.index());
        originalsOfCopies.add(result.index() + 1, original);
        copiedNestedFunctions.append({ original, result });
        if (copiedClasses.isEmpty())
            return result;
        copiedFunctionsOfClasses.append({ original, result });
        if (isOutermostWithClasses) {
            if (returned)
                classesReturnedByCopies.add(result.index() + 1, returned);
            finishCopiesOfClasses();
        }
        return result;
    }

    // The classes that a function evaluates below each of its values, in its own order. Giving calls their copies moves none of them.
    using ClassesBelowValues = UncheckedKeyHashMap<uint32_t, Vector<LayoutID, 1>>;

    ClassesBelowValues findClassesBelowValues(const Function& function)
    {
        ClassesBelowValues classes;
        forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
            ++numberOfValuesReadForClassesBelowCopies;
            if (value.opcode == Opcode::NewClass && value.heritageOperand())
                classes.add(definitionBehindDeadZoneChecks(function, value.heritageOperand()).index() + 1, Vector<LayoutID, 1>()).iterator->value.append(value.layout());
        });
        return classes;
    }

    // `class A extends Mixin(Base) { .. }`   The class below was read as a class below the one that every call of the function evaluates.
    void putClassesBelowCopy(const Function& caller, const CallToCopyFor& call, std::optional<ClassesBelowValues>& classesBelowValues)
    {
        LayoutID copy = classesReturnedByCopies.get(call.copy.index() + 1);
        LayoutID original = classReturnedBy(program.at(call.callee));
        if (!copy || !original)
            return;
        if (!classesBelowValues)
            classesBelowValues = findClassesBelowValues(caller);
        auto classesBelowCall = classesBelowValues->find(call.call.index() + 1);
        if (classesBelowCall == classesBelowValues->end())
            return;
        for (LayoutID id : classesBelowCall->value) {
            // Where the function has a declared result, the class below was read as a class below nothing that is known.
            LayoutID baseAsRead = program.at(id).base;
            if (baseAsRead && baseAsRead != original)
                continue;
            Layout& below = editor.at(id);
            below.base = copy;
            classesPutBelowOthers.append({ id, copy });
            for (LayoutID& supertype : below.supertypes) {
                if (supertype == original)
                    supertype = copy;
            }
            if (!below.supertypes.contains(copy))
                below.supertypes.append(copy);
            AOT::sort(below.supertypes);
        }
    }

    // `Mixin(Base)`   The copy is handed one class, which the caller names: what it evaluates is read as a class below that one, as if it were written so.
    void putCopyBelowClassItIsHanded(const Function& caller, const CallToCopyFor& call, UncheckedKeyHashMap<uint32_t, LayoutID>& classesMadeByCalls)
    {
        LayoutID copy = classesReturnedByCopies.get(call.copy.index() + 1);
        if (!copy)
            return;
        classesMadeByCalls.add(call.call.index() + 1, copy);
        if (program.at(copy).base)
            return;
        const Function& function = program.at(call.copy);
        ValueID heritage;
        forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewClass && value.layout() == copy)
                heritage = value.heritageOperand();
        });
        std::optional<unsigned> parameter = heritage ? positionalParameterAt(function, definitionBehindDeadZoneChecks(function, heritage)) : std::nullopt;
        std::span<const ValueID> arguments = caller.at(call.call).argumentsOperands();
        if (!parameter || *parameter >= arguments.size())
            return;
        ValueID handedID = definitionBehindDeadZoneChecks(caller, arguments[*parameter]);
        const Value& handed = caller.at(handedID);
        LayoutID base = classesMadeByCalls.get(handedID.index() + 1);
        if (handed.opcode == Opcode::FinishClass)
            base = handed.layout();
        else if (handed.opcode == Opcode::GetModuleVariable)
            base = classHeldBy(handed.variable());
        if (!base || program.at(base).kind != LayoutKind::Instance)
            return;
        Layout& layout = editor.at(copy);
        layout.base = base;
        if (!layout.supertypes.contains(base))
            layout.supertypes.append(base);
        AOT::sort(layout.supertypes);
        classesPutBelowOthers.append({ copy, base });
    }

    struct ClassesBelow {
        Vector<LayoutID> of(LayoutID id)
        {
            Vector<LayoutID> family;
            if (isVisited.set(id.index()))
                return family;
            for (LayoutID member : program.at(program.declaredMembersOf(id)).written()) {
                family.append(member);
                family.appendVector(addedBelow(member));
            }
            isVisited.clear(id.index());
            return family;
        }

        // What stands below the class and did not when the program was read.
        Vector<LayoutID> addedBelow(LayoutID id)
        {
            Vector<LayoutID> added;
            for (auto [below, above] : std::ranges::equal_range(ascendingByClassAbove, id, { }, &std::pair<LayoutID, LayoutID>::second))
                added.appendVector(of(below));
            return added;
        }

        const Program& program;
        std::span<const std::pair<LayoutID, LayoutID>> ascendingByClassAbove;
        BitVector isVisited { };
    };

    struct FinderOfTypesWithClassesBelow {
        void operator()(const TypeID& id) const { found |= table.changes(id); }
        void operator()(const LayoutSetID& id) const { found |= table.changes(id); }
        template<typename Tag> void operator()(const ID<Tag>&) const { }

        const TypesWithClassesBelow& table;
        bool& found;
    };

    // The reader lets a type that names a class name every class below it. The classes that were put below others are named as if the program were written so.
    void nameClassesWhereClassesAboveAreNamed()
    {
        if (classesPutBelowOthers.isEmpty())
            return;
        AOT::sort(classesPutBelowOthers, { }, [](const auto& entry) { return std::pair(entry.second, entry.first); });
        ClassesBelow classesBelow { program, classesPutBelowOthers.span() };
        UncheckedKeyHashMap<uint32_t, Vector<LayoutID>> added;
        BitVector hasClassesAdded;
        for (auto [below, above] : classesPutBelowOthers) {
            if (hasClassesAdded.set(above.index()))
                continue;
            added.add(above.index() + 1, classesBelow.addedBelow(above));
        }

        TypesWithClassesBelow table { Vector<TypeID>(program.types.size()), Vector<LayoutSetID>(program.types.numberOfLayoutSets()) };
        for (uint32_t index = 0, end = table.layoutSets.size(); index < end; ++index) {
            const LayoutSet& set = program.at(LayoutSetID(index));
            if (!set.listsEveryLayout())
                continue;
            Vector<LayoutID> members;
            for (LayoutID member : set.written()) {
                if (hasClassesAdded.get(member.index()))
                    members.appendVector(added.get(member.index() + 1));
            }
            if (members.isEmpty())
                continue;
            members.appendVector(set.written());
            AOT::sort(members);
            members.shrink(std::ranges::unique(members).begin() - members.begin());
            if (members.size() != set.written().size())
                table.layoutSets[index] = types.intern(set.withMembers(WTF::move(members)));
        }
        // A type stands behind the types of its contents only where the reader has met them in that order.
        for (bool didChange = true; std::exchange(didChange, false);) {
            for (uint32_t index = 0, end = table.types.size(); index < end; ++index) {
                const Type& current = program.at(table.types[index] ? table.types[index] : TypeID(index));
                bool namesWhatChanges = false;
                FinderOfTypesWithClassesBelow finder { table, namesWhatChanges };
                finder(current.layouts);
                for (const ClassPart& part : current.classes) {
                    for (TypeID contents : part.writtenContents)
                        finder(contents);
                }
                if (!namesWhatChanges)
                    continue;
                Type type = current;
                type.layouts = table.inPlaceOf(type.layouts);
                for (ClassPart& part : type.classes) {
                    for (TypeID& contents : part.writtenContents)
                        contents = table.inPlaceOf(contents);
                }
                table.types[index] = types.intern(WTF::move(type));
                didChange = true;
            }
        }

        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            if (LayoutSetID members = program.declaredMembersOf(LayoutID(index)); table.changes(members))
                editor.setDeclaredMembers(LayoutID(index), table.inPlaceOf(members));
        }

        ReferencesToCopiedClasses rewriter { *this, &table };
        auto names = [&](const auto& record) {
            bool found = false;
            forEachReference(record, FinderOfTypesWithClassesBelow { table, found });
            return found;
        };
        for (uint32_t index : findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
            const Function& function = program.functions[index];
            return names(static_cast<const FunctionRecord&>(function))
                || std::ranges::any_of(function.values, [&](const Value& value) { return table.changes(value.type) || names(value); })
                || std::ranges::any_of(function.blocks, [&](const Block& block) { return names(block.terminator); });
        })) {
            Function& function = editor.at(FunctionID(index));
            forEachReference(static_cast<FunctionRecord&>(function), rewriter);
            for (Value& value : function.editAllValues()) {
                rewriter(value.type);
                forEachReference(value, rewriter);
            }
            for (Block& block : function.editAllBlocks())
                forEachReference(block.terminator, rewriter);
        }
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            if (names(static_cast<const LayoutRecord&>(program.layouts[index])))
                forEachReference(static_cast<LayoutRecord&>(editor.at(LayoutID(index))), rewriter);
        }
        for (uint32_t index = 0; index < program.variables.size(); ++index) {
            if (names(program.variables[index]))
                forEachReference(editor.at(VariableID(index)), rewriter);
        }
    }

    // The caller runs once, or is a copy that one call has to itself. A call in a loop makes many objects at one place whatever is copied.
    Vector<CallToCopyFor> findCallsIn(FunctionID callerID, unsigned depth)
    {
        Vector<CallToCopyFor> calls;
        AnalysisContext context { build, program, nullptr, nullptr, callerID };
        const Function& caller = program.at(callerID);
        FunctionAnalyses analyses(context, caller);
        BitVector resultsHandedToObjectCreate;
        UncheckedKeyHashMap<uint32_t, ValueID> storesToConstants;
        // By field of an environment: the store in this function, or no value where there are several.
        UncheckedKeyHashMap<uint64_t, ValueID> storesToEnvironments;
        bool didFindStores = false;
        // Few callers have a call that asks.
        auto findStores = [&] {
            if (std::exchange(didFindStores, true))
                return;
            forEachValueOf(caller, [&](ProgramPoint, ValueID id, const Value& value) {
                if (value.opcode == Opcode::PutEnvironmentField) {
                    auto added = storesToEnvironments.add(makeFieldKey({ value.layout(), value.field() }), id);
                    if (!added.isNewEntry)
                        added.iterator->value = ValueID();
                    return;
                }
                if (value.opcode != Opcode::PutModuleVariable || !isConstantOfModule(value.variable()))
                    return;
                storesToConstants.add(value.variable().index() + 1, id);
                if (!depth && constantsHandedToObjectCreate.get(value.variable().index()))
                    resultsHandedToObjectCreate.set(definitionBehindDeadZoneChecks(caller, value.valueOperand()).index());
            });
        };
        if (!depth && isAnyConstantPassedToObjectCreate)
            findStores();
        // `class A extends Mixin(Other(B)) { .. }`   `const Stamped = Mixin(Plain);`   A class that is only handed on, `list.push(Mixin(B))`, is one of many wherever it arrives: a copy of its own buys it little.
        std::optional<BitVector> classesWithName;
        auto getsName = [&](ValueID made) {
            if (!classesWithName) {
                classesWithName.emplace();
                Vector<ValueID> values;
                forEachValueOf(caller, [&](ProgramPoint, ValueID id, const Value&) {
                    values.append(id);
                });
                auto give = [&](ValueID id) {
                    if (id)
                        classesWithName->set(definitionBehindDeadZoneChecks(caller, id).index());
                };
                // A value stands behind what it is made of.
                for (ValueID id : values | std::views::reverse) {
                    const Value& value = caller.at(id);
                    if (value.opcode == Opcode::PutModuleVariable || value.opcode == Opcode::PutEnvironmentField)
                        give(value.valueOperand());
                    else if (value.opcode == Opcode::NewClass) {
                        if (program.at(value.layout()).name || classesWithName->get(id.index()))
                            give(value.heritageOperand());
                    } else if (!classesWithName->get(id.index()))
                        continue;
                    else if (value.opcode == Opcode::FinishClass)
                        give(value.classObjectOperand());
                    else if (value.opcode == Opcode::Call) {
                        for (ValueID argument : value.argumentsOperands())
                            give(argument);
                    }
                }
            }
            return classesWithName->get(made.index());
        };
        // A read of a `let` or a `const` throws by itself until the store has run. A `var` answers undefined until then, and the call throws.
        auto isReadBehindItsStore = [&](VariableID variable, ProgramPoint read) {
            if (program.at(variable).initialization != VariableInitialization::Undefined)
                return true;
            if (!depth && program.at(variable).module != caller.module)
                return isStoredBeforeBodyStarts(variable, callerID);
            findStores();
            ValueID store = storesToConstants.get(variable.index() + 1);
            return store && analyses.dominates(store, read);
        };
        // The order in which the loader runs the files says that the other file has stored its `var`. A name that the build does not follow may begin this file before that: flow knows, and this pass runs before it.
        auto isCalleeReadFromOtherFile = [&](const Value& definition) {
            return !depth && definition.opcode == Opcode::GetModuleVariable && program.at(definition.variable()).initialization == VariableInitialization::Undefined && program.at(definition.variable()).module != caller.module;
        };
        // `var make = ..` in a function that has closures: the variable is a field of the environment of the function.
        auto closureStoredInto = [&](const Value& read, ProgramPoint point) -> ValueID {
            findStores();
            ValueID store = storesToEnvironments.get(makeFieldKey({ read.layout(), read.field() }));
            if (!store || caller.at(store).operand(OperandIndex::PutEnvironmentField::environment) != read.operand(OperandIndex::GetEnvironmentField::environment) || !analyses.dominates(store, point))
                return { };
            return definitionBehindDeadZoneChecks(caller, caller.at(store).valueOperand());
        };
        forEachValueOf(caller, [&](ProgramPoint point, ValueID id, const Value& value) {
            // The text of a CommonJS module is a function that the body of the module calls.
            if (value.opcode == Opcode::CallDirect && !depth && !analyses.isInCycle(point.block))
                addCallerThatRunsOnce(value.function());
            bool constructsPrototypeObject = value.opcode == Opcode::Construct && !value.newTargetOperand() && isStoredAsPrototypeObject(caller, analyses, id);
            if (value.opcode != Opcode::Call && !constructsPrototypeObject)
                return;
            ValueID definitionID = definitionBehindDeadZoneChecks(caller, value.calleeOperand());
            std::optional<FieldReference> holder;
            if (const Value& read = caller.at(definitionID); read.opcode == Opcode::GetEnvironmentField) {
                holder = FieldReference { read.layout(), read.field() };
                definitionID = closureStoredInto(read, point);
                if (!definitionID)
                    return;
            }
            const Value& definition = caller.at(definitionID);
            bool isCreatedByCaller = definition.opcode == Opcode::NewClosure;
            // `(function () { .. })()`
            if (isCreatedByCaller && !holder && !depth && !constructsPrototypeObject && analyses.useLists().usesOf(definitionID).size() == 1 && !analyses.isInCycle(point.block))
                addCallerThatRunsOnce(definition.function());
            FunctionID callee = isCreatedByCaller ? definition.function() : definition.opcode == Opcode::GetModuleVariable ? functionHeldBy(definition.variable()) : FunctionID();
            if (!callee || copies.get(callee.index()) || !canBeCopiedForOneCall(program.at(callee)))
                return;
            const Function& function = program.at(callee);
            if (!isCreatedByCaller && (!function.captures.isEmpty() || !isReadBehindItsStore(definition.variable(), point)))
                return;
            bool testsCallee = !isCreatedByCaller && isCalleeReadFromOtherFile(definition);
            if (constructsPrototypeObject) {
                if (!analyses.isInCycle(point.block) && canBeCopiedWithoutItsLayout(callee)) {
                    calls.append({ point, id, callee, isCreatedByCaller ? definitionID : ValueID(), ReasonToCopy::ConstructsPrototypeObject, holder });
                    calls.last().testsCallee = testsCallee;
                }
                return;
            }
            if (std::optional<ConstructionByParameter> construction = constructionByParameterIn(callee); construction && construction->position < value.argumentsOperands().size() && !analyses.isInCycle(point.block)) {
                const Value& handedIn = caller.at(definitionBehindDeadZoneChecks(caller, value.argumentsOperands()[construction->position]));
                FunctionID constructor = handedIn.opcode == Opcode::GetModuleVariable ? functionHeldBy(handedIn.variable()) : FunctionID();
                Size size = sizeOf(callee);
                if (constructor && !copies.get(constructor.index()) && canBeCopiedForOneCall(program.at(constructor)) && program.at(constructor).captures.isEmpty() && isReadBehindItsStore(handedIn.variable(), point)
                    && canBeCopiedWithoutItsLayout(constructor) && size.numberOfValuesWithClosures <= maxValuesInCopiedMaker && !size.definesWhatStandsAtOnePlace) {
                    calls.append({ point, id, callee, isCreatedByCaller ? definitionID : ValueID(), ReasonToCopy::ConstructsPrototypeObjectByWhatItIsHanded, holder, false, { }, construction->construction, constructor });
                    calls.last().testsCallee = testsCallee;
                    return;
                }
            }
            // What a copy calls to make the objects that it hands on is copied with it: `const members = makeMembers(options)`.
            bool makesWhatObjectCreateIsHanded = function.captures.isEmpty() && (depth || makers.get(callee.index()) || resultsHandedToObjectCreate.get(id.index()));
            bool definesClosureUnderWrittenName = !makesWhatObjectCreateIsHanded && handsWrittenNameToDefineClosureUnder(caller, value, function);
            bool returnsClass = !makesWhatObjectCreateIsHanded && !!classReturnedBy(function) && getsName(id);
            bool isClosureFactory = definesClosureUnderWrittenName || returnsClass || (!makesWhatObjectCreateIsHanded && returnsClosureItCreates(function));
            if (!makesWhatObjectCreateIsHanded && !isClosureFactory)
                return;
            if (!isCreatedByCaller && !returnsClass && program.at(definition.variable()).initialization == VariableInitialization::Function)
                return;
            // The closure of the copy is held by nothing but the call. What a type says of the function that is declared, it goes on saying of that function.
            if (!definesClosureUnderWrittenName && !returnsClass && isNamedByClosedType(callee))
                return;
            Size size = sizeOf(callee);
            // However often it runs, a call that hands on a written name defines closures of its copy under that name alone.
            if (size.numberOfValuesWithClosures > (returnsClass ? maxValuesInCopiedClassFactory : isClosureFactory ? maxValuesInCopiedFactory : maxValuesInCopiedMaker) || size.definesWhatStandsAtOnePlace || (!definesClosureUnderWrittenName && analyses.isInCycle(point.block)))
                return;
            calls.append({ point, id, callee, isCreatedByCaller ? definitionID : ValueID(), isClosureFactory ? ReasonToCopy::IsClosureFactory : ReasonToCopy::MakesWhatObjectCreateIsHanded, holder, returnsClass });
            calls.last().testsCallee = testsCallee;
        });
        return calls;
    }

    FunctionID copyWithCallsInside(FunctionID copy, unsigned depth)
    {
        if (depth < maxDepthOfCopiedCalls)
            copyForCalls(copy, findCallsIn(copy, depth + 1), depth + 1);
        return copy;
    }

    FunctionID copyFor(const CallToCopyFor& call, unsigned depth)
    {
        switch (call.reason) {
        case ReasonToCopy::IsClosureFactory:
            // The only call of a factory has the factory to itself. Somebody unseen may still call it with another class, so the class that is read as one below the class of this call is a copy.
            if ((numberOfCallsOfFactory.get(call.callee.index() + 1) < 2 && !call.returnsClass) || numberOfValuesInCopiesOfFactories / maxGrowthByCopiedFactories > numberOfValuesAsRead)
                return { };
            return copyWithCallsInside(copyWithItsClosures(call.callee, numberOfValuesInCopiesOfFactories), depth);
        case ReasonToCopy::MakesWhatObjectCreateIsHanded: {
            unsigned& numberOfCopies = numberOfCopiesOfMaker.add(call.callee.index() + 1, 0).iterator->value;
            if (numberOfCopies >= maxCopiesOfMaker || numberOfValuesInCopiesOfMakers > maxValuesInAllCopiesOfMakers)
                return { };
            ++numberOfCopies;
            return copyWithCallsInside(copyWithItsClosures(call.callee, numberOfValuesInCopiesOfMakers), depth);
        }
        case ReasonToCopy::ConstructsPrototypeObjectByWhatItIsHanded: {
            if (numberOfValuesInCopiesOfMakers > maxValuesInAllCopiesOfMakers)
                return { };
            // The closure of the copy is held by nothing but the call, so its parameter is what the call hands in.
            FunctionID copy = copyWithItsClosures(call.callee, numberOfValuesInCopiesOfMakers);
            Vector<CallToCopyFor> construction;
            construction.append({ pointOf(program.at(copy), call.constructionInCallee), call.constructionInCallee, call.constructorHandedIn, ValueID(), ReasonToCopy::ConstructsPrototypeObject });
            copyForCalls(copy, WTF::move(construction), depth + 1);
            return copy;
        }
        case ReasonToCopy::ConstructsPrototypeObject: {
            if (numberOfValuesInCopiesOfMakers > maxValuesInAllCopiesOfMakers)
                return { };
            FunctionID copy = copyWithItsClosures(call.callee, numberOfValuesInCopiesOfMakers);
            Function& constructor = editor.at(copy);
            constructor.constructedClass = { };
            constructor.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
            if (std::optional<unsigned> index = constructor.thisParameterIndex())
                constructor.edit(constructor.at(entryBlock).parameters[*index]).type = TypeTable::anything;
            return copy;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void copyForCalls(FunctionID callerID, Vector<CallToCopyFor>&& calls, unsigned depth)
    {
        bool hasCopies = false;
        for (CallToCopyFor& call : calls) {
            call.copy = copyFor(call, depth);
            hasCopies |= !!call.copy;
        }
        if (!hasCopies)
            return;

        AnalysisContext context { build, program, nullptr, nullptr, callerID };
        Function& caller = editor.at(callerID);
        Vector<CallAsWritten, 2> callsThatTestCallee;
        {
            FunctionAnalyses analyses(context, caller);
            IRBuilder builder(context, caller, types, &analyses);
            callCopiesInPlaceOfCallees(caller, builder, calls, callsThatTestCallee);
        }
        if (callsThatTestCallee.isEmpty())
            return;
        {
            FunctionAnalyses analyses(context, caller);
            IRBuilder builder(context, caller, types, &analyses);
            for (CallAsWritten& call : callsThatTestCallee)
                callAsWrittenWhereCalleeIsNotStored(builder, WTF::move(call));
        }
        removeTypesOfValues(caller);
        compactFunction(caller);
    }

    // `import { once } from "./helpers.js"; const a = once(load);` where helpers.js has `export var once = ..`: until helpers.js has stored the variable it answers undefined, and the call throws.
    // So in front of the call of the copy, the callee is asked for, and where it is undefined the call runs as the program writes it.
    void callAsWrittenWhereCalleeIsNotStored(IRBuilder& builder, CallAsWritten&& call)
    {
        Function& caller = builder.function();
        builder.setOrigin(call.call.origin());
        ValueID callee = definitionBehindDeadZoneChecks(caller, call.call.calleeOperand());
        // The values of a program as read have no types yet, and a test asks for the type of what it tests.
        caller.edit(callee).type = TypeTable::anything;
        IRBuilder::Expansion expansion = builder.beginExpansion(pointOf(caller, call.standsInFront));
        ValueID undefined = builder.appendUndefined();
        IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(Opcode::StrictEq, { callee, undefined }), BlockFrequency::Rare, BlockFrequency::Normal);
        builder.moveToEndOf(blocks.taken);
        // Flow knows what a call of undefined does, and sees no call of the function here.
        call.call.setOperand(call.call.opcode == Opcode::Call ? OperandIndex::Call::callee : OperandIndex::Construct::callee, undefined);
        call.call.type = TypeID();
        builder.append(WTF::move(call.call));
        builder.endExpansion(expansion, builder.appendUndefined());
        builder.moveToEndOf(blocks.notTaken);
        builder.endExpansion(expansion, builder.appendUndefined());
    }

    void callCopiesInPlaceOfCallees(Function& caller, IRBuilder& builder, const Vector<CallToCopyFor>& calls, Vector<CallAsWritten, 2>& callsThatTestCallee)
    {
        UncheckedKeyHashMap<uint32_t, LayoutID> classesMadeByCalls;
        std::optional<ClassesBelowValues> classesBelowValues;
        for (const CallToCopyFor& call : calls) {
            if (!call.copy)
                continue;
            builder.setOrigin(caller.at(call.call).origin());
            // What is put in front of the call comes behind the test.
            if (call.testsCallee)
                callsThatTestCallee.append({ builder.insertUntypedBefore(call.point, IRBuilder::make(Opcode::Undefined)), Value(caller.at(call.call)) });
            // A closure of the copy over what the closure of the original captured.
            Value closure = call.closure ? Value(caller.at(call.closure)) : IRBuilder::make(Opcode::NewClosure);
            closure.setFunction(call.copy);
            if (call.reason == ReasonToCopy::ConstructsPrototypeObject) {
                // What types the values later asks the first value of a function whether that is done, and a construction by a parameter may be the first.
                ValueID created = builder.insertUntypedBefore(call.point, WTF::move(closure));
                // `new` lays the object below what the function holds under `prototype` at that moment.
                PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
                Value read = IRBuilder::make(Opcode::PropGet, { caller.at(call.call).calleeOperand() });
                read.setKey(prototype);
                ValueID held = builder.insertUntypedBefore(call.point, WTF::move(read));
                Value store = IRBuilder::make(Opcode::PropSet, { created, held });
                store.setKey(prototype);
                store.setMode(ECMAMode::Strict);
                builder.insertUntypedBefore(call.point, WTF::move(store));
                caller.edit(call.call).setOperand(OperandIndex::Construct::callee, created);
                continue;
            }
            ValueID created = builder.insertBefore(call.point, WTF::move(closure));
            Value& edited = caller.edit(call.call);
            edited.setOperand(OperandIndex::Call::callee, created);
            // The copy has this one call, so its body moves here and is no second copy.
            edited.setIsAlwaysInlined(true);
            putClassesBelowCopy(caller, call, classesBelowValues);
            putCopyBelowClassItIsHanded(caller, call, classesMadeByCalls);
        }
        builder.applyInsertions();
    }

    // Every function that holds the environment can store to its fields, and so can the text of an eval that sees them.
    void dropCallsThroughFieldsStoredElsewhere(Vector<Vector<CallToCopyFor>>& callsByCaller)
    {
        UncheckedKeyHashMap<uint64_t, unsigned> numberOfStores;
        for (const Vector<CallToCopyFor>& calls : callsByCaller) {
            for (const CallToCopyFor& call : calls) {
                if (call.holder)
                    numberOfStores.add(makeFieldKey(*call.holder), 0);
            }
        }
        if (numberOfStores.isEmpty())
            return;
        Vector<uint64_t> stores = collectInParallel<uint64_t>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<uint64_t>& found) {
            for (const Value& value : program.functions[index].values) {
                if (value.opcode == Opcode::PutEnvironmentField && numberOfStores.contains(makeFieldKey({ value.layout(), value.field() })))
                    found.append(makeFieldKey({ value.layout(), value.field() }));
            }
        });
        for (uint64_t key : stores)
            ++numberOfStores.find(key)->value;
        for (const EvalScope& scope : program.evalScopes) {
            for (const EvalBinding& binding : scope.bindings) {
                if (!binding.layout || !binding.field)
                    continue;
                if (auto found = numberOfStores.find(makeFieldKey({ binding.layout, *binding.field })); found != numberOfStores.end())
                    ++found->value;
            }
        }
        for (Vector<CallToCopyFor>& calls : callsByCaller) {
            calls.removeAllMatching([&](const CallToCopyFor& call) {
                return call.holder && numberOfStores.get(makeFieldKey(*call.holder)) != 1;
            });
        }
    }

    static uint64_t keyOfMember(uint32_t holder, PropertyKeyID key) { return static_cast<uint64_t>(holder) + 1 << 32 | key.index(); }

    template<typename Map, typename Key>
    static void noteStore(Map& map, Key key, FunctionID function)
    {
        auto added = map.add(key, function);
        if (!added.isNewEntry && added.iterator->value != function)
            added.iterator->value = FunctionID();
    }

    void noteStoresOfCodeThatRunsOnce()
    {
        if (std::exchange(didNoteStoresOfCodeThatRunsOnce, true))
            return;
        struct MethodStore {
            FunctionID storedBy;
            ValueID constructor;
            PropertyKeyID key;
            FunctionID method;
        };
        Vector<MethodStore> methodStores;
        PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
        for (FunctionID id : callersThatRunOnce) {
            const Function& function = program.at(id);
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
                if (value.opcode != Opcode::PutModuleVariable && value.opcode != Opcode::PutEnvironmentField && value.opcode != Opcode::PropSet && value.opcode != Opcode::PropDefine)
                    return;
                const Value& stored = function.at(definitionBehindDeadZoneChecks(function, value.valueOperand()));
                if (stored.opcode != Opcode::NewClosure)
                    return;
                if (value.opcode == Opcode::PutModuleVariable)
                    return noteStore(closuresInVariables, value.variable().index() + 1, stored.function());
                if (value.opcode == Opcode::PutEnvironmentField)
                    return noteStore(closuresInEnvironments, makeFieldKey({ value.layout(), value.field() }), stored.function());
                const Value& holder = function.at(definitionBehindDeadZoneChecks(function, value.operand(0)));
                if (holder.opcode == Opcode::NewObject && !program.at(holder.layout()).isEnvironment())
                    return noteStore(methodsOfLiterals, keyOfMember(holder.layout().index(), value.key()), stored.function());
                if (holder.opcode == Opcode::PropGet && holder.key() == prototype)
                    methodStores.append({ id, holder.operand(0), value.key(), stored.function() });
            });
        }
        for (const MethodStore& store : methodStores) {
            if (ExpectedValue constructor = expectedValueOf({ store.storedBy, { }, { }, true }, store.constructor, 0); constructor.kind == ExpectedValue::Kind::Closure)
                noteStore(methodsOfConstructors, keyOfMember(constructor.index, store.key), store.method);
        }
    }

    static uint64_t keyOfHolder(FunctionID function, LayoutID layout) { return static_cast<uint64_t>(function.index()) + 1 << 32 | layout.index(); }

    void addLiteralsIn(ExpectedValue value, Vector<LayoutID, 2>& literals, unsigned steps = 0)
    {
        if (value.kind == ExpectedValue::Kind::Literal && !literals.contains(LayoutID(value.index)))
            literals.append(LayoutID(value.index));
        if (value.kind != ExpectedValue::Kind::Instance || steps > maxDepthOfCopiedCalls)
            return;
        for (ExpectedValue argument : ExpectedValueList(expectedInstances[value.index].arguments))
            addLiteralsIn(argument, literals, steps + 1);
    }

    // The copy captures the objects that the fields of the helper object are expected to hold.
    void captureLiterals(const Activation& caller, FunctionAnalyses& analysesOfCaller, ExpectedCall& call)
    {
        Vector<LayoutID, 2> literals;
        addLiteralsIn(call.callee.thisValue, literals);
        const Function& function = program.at(caller.function);
        ProgramPoint point = pointOf(function, call.call);
        for (LayoutID layout : literals) {
            ValueID holder = holdersOfLiterals.get(keyOfHolder(caller.function, layout));
            if (!holder)
                continue;
            if (function.at(holder).opcode != Opcode::Parameter && (!analysesOfCaller.dominates(holder, point) || analysesOfCaller.isInCycle(pointOf(function, holder).block)))
                continue;
            call.literals.append({ layout, holder });
        }
    }

    void addCapturesTo(FunctionID copyID, const ExpectedCall& call)
    {
        Function& copy = editor.at(copyID);
        IRBuilder builder(AnalysisContext { build, program, nullptr, nullptr, copyID }, copy, types);
        builder.setOrigin({ copy.position, 0 });
        for (auto [layout, holder] : call.literals) {
            auto& parameters = copy.edit(entryBlock, BlockEdit::Parameters).parameters;
            ValueID added = builder.addParameter(entryBlock, TypeTable::anything);
            RELEASE_ASSERT(parameters.last() == added);
            parameters.removeLast();
            parameters.insert(copy.captures.size(), added);
            copy.captures.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
            holdersOfLiterals.add(keyOfHolder(copyID, layout), added);
        }
    }

    ExpectedValue expectClosureOf(FunctionID function) const
    {
        return function ? ExpectedValue { ExpectedValue::Kind::Closure, function.index() } : ExpectedValue();
    }

    ExpectedValueList expectedValuesOf(const Activation& activation, std::span<const ValueID> values, unsigned steps)
    {
        return WTF::map<4>(values, [&](ValueID id) {
            return expectedValueOf(activation, id, steps);
        });
    }

    std::optional<Activation> expectedCalleeOf(const Activation& activation, const Value& call, unsigned steps)
    {
        ExpectedValue callee = expectedValueOf(activation, call.calleeOperand(), steps);
        if (callee.kind != ExpectedValue::Kind::Closure)
            return std::nullopt;
        ValueID thisValue = call.opcode == Opcode::Call ? call.operand(OperandIndex::Call::thisValue) : ValueID();
        return Activation { FunctionID(callee.index), thisValue ? expectedValueOf(activation, thisValue, steps) : ExpectedValue(), expectedValuesOf(activation, call.variadicOperands(), steps) };
    }

    // `return this`   `return new Helper(target, name)`
    ExpectedValue expectedResultOf(const Activation& activation, unsigned steps)
    {
        const Function& function = program.at(activation.function);
        if (!function.hasBody() || function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || function.values.size() > maxValuesInCopiedMaker)
            return { };
        ExpectedValue result;
        for (const Block& block : function.blocks) {
            ValueID returned = block.terminator.kind == TerminatorKind::Return ? block.terminator.valueOperand() : block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
            ExpectedValue expected = returned ? expectedValueOf(activation, returned, steps) : ExpectedValue();
            if (!expected)
                continue;
            if (result && result != expected)
                return { };
            result = expected;
        }
        return result;
    }

    // `this.name = name` in the constructor, or `Helper.prototype.method = function () { .. }`.
    ExpectedValue expectedMemberOf(ExpectedValue object, PropertyKeyID key, unsigned steps)
    {
        noteStoresOfCodeThatRunsOnce();
        if (object.kind == ExpectedValue::Kind::Literal)
            return expectClosureOf(methodsOfLiterals.get(keyOfMember(object.index, key)));
        if (object.kind != ExpectedValue::Kind::Instance)
            return { };
        Activation construction { expectedInstances[object.index].constructor, object, expectedInstances[object.index].arguments };
        const Function& constructor = program.at(construction.function);
        ValueID stored;
        forEachValueOf(constructor, [&](ProgramPoint, ValueID, const Value& value) {
            if (value.opcode == Opcode::PropSet && value.key() == key && isThisOf(constructor, value.operand(OperandIndex::PropSet::object)))
                stored = value.valueOperand();
        });
        if (stored)
            return expectedValueOf(construction, stored, steps);
        return expectClosureOf(methodsOfConstructors.get(keyOfMember(construction.function.index(), key)));
    }

    ExpectedValue expectedValueOf(const Activation& activation, ValueID id, unsigned steps)
    {
        id = definitionBehindDeadZoneChecks(program.at(activation.function), id);
        if (!activation.isOnlyActivation)
            return computeExpectedValueOf(activation, id, steps);
        // A chain of calls is as long as the program writes it: each link finds the link in front of it here.
        uint64_t key = static_cast<uint64_t>(activation.function.index()) + 1 << 32 | id.index();
        if (auto found = expectedValuesInOnlyActivations.find(key); found != expectedValuesInOnlyActivations.end())
            return found->value;
        ExpectedValue result = computeExpectedValueOf(activation, id, steps);
        expectedValuesInOnlyActivations.add(key, result);
        return result;
    }

    ExpectedValue computeExpectedValueOf(const Activation& activation, ValueID id, unsigned steps)
    {
        if (++steps > maxStepsToExpectedValue)
            return { };
        const Function& function = program.at(activation.function);
        const Value& value = function.at(id);
        if (value.opcode == Opcode::String)
            return { ExpectedValue::Kind::Text, value.string().index() };
        if (value.opcode == Opcode::NewClosure)
            return expectClosureOf(value.function());
        if (value.opcode == Opcode::NewObject)
            return program.at(value.layout()).isEnvironment() ? ExpectedValue() : ExpectedValue { ExpectedValue::Kind::Literal, value.layout().index() };
        if (value.opcode == Opcode::FinishObject) {
            ExpectedValue literal = expectedValueOf(activation, value.operand(0), steps);
            if (literal.kind == ExpectedValue::Kind::Literal && isCallerThatRunsOnce.get(activation.function.index()))
                holdersOfLiterals.add(keyOfHolder(activation.function, LayoutID(literal.index)), id);
            return literal;
        }
        if (value.opcode == Opcode::ToThis)
            return expectedValueOf(activation, value.operand(0), steps);
        if (value.opcode == Opcode::Parameter) {
            if (isThisOf(function, id))
                return activation.thisValue;
            if (std::optional<unsigned> parameter = positionalParameterAt(function, id); parameter && *parameter < activation.arguments.size())
                return activation.arguments[*parameter];
            return { };
        }
        if (value.opcode == Opcode::GetModuleVariable) {
            if (FunctionID held = functionHeldBy(value.variable()))
                return expectClosureOf(held);
            noteStoresOfCodeThatRunsOnce();
            return expectClosureOf(closuresInVariables.get(value.variable().index() + 1));
        }
        if (value.opcode == Opcode::GetEnvironmentField) {
            noteStoresOfCodeThatRunsOnce();
            return expectClosureOf(closuresInEnvironments.get(makeFieldKey({ value.layout(), value.field() })));
        }
        if (value.opcode == Opcode::PropGet) {
            if (ExpectedValue object = expectedValueOf(activation, value.operand(OperandIndex::PropGet::object), steps))
                return expectedMemberOf(object, value.key(), steps);
            return { };
        }
        if (value.opcode != Opcode::Call && value.opcode != Opcode::Construct)
            return { };
        std::optional<Activation> callee = expectedCalleeOf(activation, value, steps);
        if (!callee)
            return { };
        if (value.opcode == Opcode::Call)
            return expectedResultOf(*callee, steps);
        ExpectedInstance instance { callee->function, WTF::move(callee->arguments) };
        size_t index = expectedInstances.find(instance);
        if (index == notFound) {
            index = expectedInstances.size();
            expectedInstances.append(WTF::move(instance));
        }
        return { ExpectedValue::Kind::Instance, static_cast<uint32_t>(index) };
    }

    // `getter(name) { target.__defineGetter__(name, function () { .. }); }`   `access(name) { return this.getter(name).setter(name); }`
    bool definesClosureUnderWrittenName(const Activation& activation, unsigned depth)
    {
        const Function& function = program.at(activation.function);
        if (copies.get(activation.function.index()) || !canBeCopiedForOneCall(function))
            return false;
        Size size = sizeOf(activation.function);
        if (size.numberOfValuesWithClosures > maxValuesInCopiedFactory || size.definesWhatStandsAtOnePlace)
            return false;
        bool result = false;
        forEachParameterThatNamesCreatedClosure(function, [&](unsigned parameter) {
            result |= parameter < activation.arguments.size() && activation.arguments[parameter].kind == ExpectedValue::Kind::Text;
        });
        if (result || depth >= maxDepthOfCopiedCalls)
            return result;
        forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
            if (result || value.opcode != Opcode::Call)
                return;
            if (std::optional<Activation> callee = expectedCalleeOf(activation, value, 0))
                result = definesClosureUnderWrittenName(*callee, depth + 1);
        });
        return result;
    }

    // The calls of methods of a helper object: nothing proves what the member holds, so the caller tests it.
    Vector<ExpectedCall> findExpectedCallsIn(const Activation& activation, unsigned depth)
    {
        Vector<ExpectedCall> calls;
        const Function& caller = program.at(activation.function);
        forEachValueOf(caller, [&](ProgramPoint, ValueID id, const Value& value) {
            if (value.opcode != Opcode::Call || caller.at(definitionBehindDeadZoneChecks(caller, value.calleeOperand())).opcode != Opcode::PropGet)
                return;
            if (std::optional<Activation> callee = expectedCalleeOf(activation, value, 0); callee && definesClosureUnderWrittenName(*callee, depth))
                calls.append({ id, WTF::move(*callee) });
        });
        if (calls.isEmpty())
            return calls;
        AnalysisContext context { build, program, nullptr, nullptr, activation.function };
        FunctionAnalyses analyses(context, caller);
        calls.removeAllMatching([&](const ExpectedCall& call) {
            return analyses.isInCycle(pointOf(caller, call.call).block);
        });
        for (ExpectedCall& call : calls)
            captureLiterals(activation, analyses, call);
        return calls;
    }

    // `var target = this.target`: the fields of the helper object that are expected to hold a written string or an object that the program writes out.
    Vector<ExpectedRead> findExpectedReadsIn(const Activation& activation)
    {
        Vector<ExpectedRead> reads;
        forEachValueOf(program.at(activation.function), [&](ProgramPoint, ValueID id, const Value& value) {
            if (value.opcode != Opcode::PropGet)
                return;
            ExpectedValue expected = expectedValueOf(activation, id, 0);
            if (expected.kind == ExpectedValue::Kind::Text || (expected.kind == ExpectedValue::Kind::Literal && holdersOfLiterals.contains(keyOfHolder(activation.function, LayoutID(expected.index)))))
                reads.append({ id, expected });
        });
        return reads;
    }

    // In front of the first use of what was read, the copy tests that it read what is expected. From there on it has two versions: one where flow knows the values, and the text as written.
    void testExpectedReads(FunctionID functionID, IRBuilder& builder, FunctionAnalyses& analyses, Vector<ExpectedRead>&& reads, Vector<ExpectedCall>& calls)
    {
        Function& function = builder.function();
        if (reads.isEmpty() || std::ranges::any_of(function.blocks, [](const Block& block) { return !!block.handler; }))
            return;
        // The closures of the text as written are closures of the functions as written, which take environments of the layouts as written.
        if (std::ranges::any_of(function.values, [&](const Value& value) { return value.opcode == Opcode::NewObject && program.at(value.layout()).isEnvironment(); }))
            return;
        ProgramPoint first = pointOf(function, reads[0].read);
        if (analyses.isInCycle(first.block))
            return;
        UncheckedKeyHashMap<uint32_t, unsigned> readsByName;
        const auto& values = function.at(first.block).values;
        uint32_t firstUse = first.index;
        for (; firstUse < values.size(); ++firstUse) {
            ValueID id = values[firstUse];
            const Value& value = function.at(id);
            if (size_t index = reads.findIf([&](const ExpectedRead& read) { return read.read == id; }); index != notFound) {
                readsByName.add(id.index() + 1, index);
                continue;
            }
            bool isOtherName = value.opcode == Opcode::Convert || value.opcode == Opcode::Upcast || value.opcode == Opcode::CheckInitialized;
            if (isOtherName) {
                if (auto named = readsByName.find(value.operand(0).index() + 1); named != readsByName.end()) {
                    readsByName.add(id.index() + 1, unsigned { named->value });
                    continue;
                }
            }
            if (std::ranges::any_of(value.operands(), [&](ValueID operand) { return operand && readsByName.contains(operand.index() + 1); }))
                break;
        }

        builder.setOrigin(function.at(reads[0].read).origin());
        builder.insertBefore({ first.block, firstUse }, IRBuilder::make(Opcode::Undefined));
        builder.applyInsertions();
        IRBuilder::Expansion expansion = builder.beginExpansion({ first.block, firstUse });
        Vector<BlockID> rest { expansion.continuation };
        BitVector isInRest;
        isInRest.set(expansion.continuation.index());
        for (size_t index = 0; index < rest.size(); ++index) {
            for (const Target& target : function.at(rest[index]).terminator.targets) {
                if (!isInRest.set(target.block.index()))
                    rest.append(target.block);
            }
        }

        Vector<ValueID, 4> proven(reads.size());
        for (size_t index = 0; index < reads.size(); ++index) {
            auto [read, expected] = reads[index];
            if (!readsByName.contains(read.index() + 1))
                continue;
            // Flow keeps what it knew of a value that passed a test. It knows the written string and the captured object, so these stand for what was read.
            ValueID known;
            if (expected.kind == ExpectedValue::Kind::Text) {
                Value string = IRBuilder::make(Opcode::String);
                string.setString(StringID(expected.index));
                known = builder.append(WTF::move(string));
            } else
                known = holdersOfLiterals.get(keyOfHolder(functionID, LayoutID(expected.index)));
            IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(Opcode::StrictEq, { readWithItsType(function, read), known }), BlockFrequency::Normal, BlockFrequency::Rare);
            proven[index] = known;
            builder.moveToEndOf(blocks.notTaken);
            builder.endExpansion(expansion, builder.appendUndefined());
            builder.moveToEndOf(blocks.taken);
        }
        ValueID undefined = builder.appendUndefined();
        RegionCloner cloner(builder, function);
        for (auto [name, index] : readsByName)
            cloner.mapValue(ValueID(name - 1), proven[index]);
        cloner.cloneBlocks(rest.span(), [](ValueID, Value&) { });
        builder.emitJump(cloner.cloneOf(expansion.continuation), Arguments { undefined });
        for (BlockID block : rest) {
            for (ValueID id : function.at(block).values) {
                if (function.at(id).opcode == Opcode::NewClosure)
                    function.edit(id).setFunction(originalsOfCopies.get(function.at(id).function().index() + 1));
            }
        }
        for (ExpectedCall& call : calls) {
            if (ValueID clone = cloner.cloneOf(call.call))
                call.call = clone;
        }
    }

    void callCopyWhereCalleeIsExpected(IRBuilder& builder, const ExpectedCall& expected)
    {
        Function& caller = builder.function();
        // `return this.getter(name).setter(name)` in strict code: a call in tail position ends its block, and the test of the callee needs a block behind the call. The copy is pasted in, so no frame is left over.
        BlockID block = pointOf(caller, expected.call).block;
        if (const Terminator& terminator = caller.at(block).terminator; terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == expected.call) {
            // A cast that stands behind the call asks for the type of what it casts.
            caller.edit(expected.call).type = TypeTable::anything;
            builder.demoteReturnCall(block);
        }
        Value call = caller.at(expected.call);
        FunctionID callee = expected.callee.function;
        builder.setOrigin(call.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(pointOf(caller, expected.call));
        call.setOperand(OperandIndex::Call::callee, readWithItsType(caller, call.calleeOperand()));
        Value test = IRBuilder::make(Opcode::IsClosureOf, { call.calleeOperand() });
        test.setFunction(callee);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(WTF::move(test)), 0, BlockFrequency::Normal, BlockFrequency::Rare);
        builder.moveToEndOf(branch.notTaken);
        builder.endExpansion(expansion, builder.append(Value(call)));

        builder.moveToEndOf(branch.taken);
        Value closure = IRBuilder::make(Opcode::NewClosure);
        closure.setFunction(expected.copy);
        for (uint32_t index = 0; index < program.at(callee).captures.size(); ++index) {
            Value capture = IRBuilder::make(Opcode::GetCapture, { branch.provenOnTaken });
            capture.setFunction(callee);
            capture.setIndex(index);
            closure.appendVariadicOperand(builder.append(WTF::move(capture)));
        }
        for (auto [layout, holder] : expected.literals)
            closure.appendVariadicOperand(holder);
        call.setOperand(OperandIndex::Call::callee, builder.append(WTF::move(closure)));
        call.setIsAlwaysInlined(true);
        builder.endExpansion(expansion, builder.append(WTF::move(call)));
    }

    // The code that is entered with one of the names that the program writes out: a turn of a loop, or the body of a function that is called with each.
    struct CodeForWrittenNames {
        Edge entry;
        ValueID name;
        Vector<StringID, 4> writtenNames;
        // The first block first.
        Vector<BlockID> blocks;
        Vector<ValueID, 2> closures;
    };

    std::optional<Vector<StringID, 4>> namesWrittenIn(const Function& function, ValueID list)
    {
        const Value& array = function.at(definitionBehindDeadZoneChecks(function, list));
        DataID data = array.opcode == Opcode::Data ? array.data() : array.opcode == Opcode::GetModuleVariable ? arraysInConstants.get(array.variable().index() + 1) : DataID();
        const DataNode::Array* written = data ? program.at(data).root.tryAs<DataNode::Array>() : nullptr;
        if (!written || !written->elements.size() || written->elements.size() > maxWrittenNamesOfLoop)
            return std::nullopt;
        Vector<StringID, 4> names;
        for (const DataNode& element : written->elements) {
            const DataNode::String* name = element.tryAs<DataNode::String>();
            if (!name)
                return std::nullopt;
            if (!names.contains(name->value))
                names.append(name->value);
        }
        return names;
    }

    // The blocks from the target of the entry on that the functor takes in, if each written name can have a copy of them.
    std::optional<CodeForWrittenNames> findCodeForWrittenNames(const Function& function, FunctionAnalyses& analyses, Edge entry, ValueID name, Vector<StringID, 4>&& names, NOESCAPE const Invocable<bool(BlockID)> auto& isPartOfCode)
    {
        const Target& first = function.at(entry.from).terminator.targets[entry.targetIndex];
        if (!isPartOfCode(first.block) || !first.arguments.isEmpty() || !first.proofs.isEmpty() || analyses.cfg().incomingEdges(first.block).size() != 1)
            return std::nullopt;

        CodeForWrittenNames found { entry, name, WTF::move(names), { first.block }, { } };
        BitVector isInCode;
        isInCode.set(first.block.index());
        for (size_t index = 0; index < found.blocks.size(); ++index) {
            function.at(found.blocks[index]).forEachTarget([&](const Target& target) {
                if (isPartOfCode(target.block) && !isInCode.set(target.block.index()))
                    found.blocks.append(target.block);
            });
        }

        const UseLists& useLists = analyses.useLists();
        unsigned numberOfValues = 0;
        for (BlockID block : found.blocks) {
            auto isUsedElsewhere = [&](ValueID id) {
                return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) { return !isInCode.get(use.block.index()); });
            };
            if (std::ranges::any_of(function.at(block).parameters, isUsedElsewhere))
                return std::nullopt;
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                bool standsAtOnePlace = value.opcode == Opcode::NewClass || value.opcode == Opcode::NewClassToken || value.opcode == Opcode::NewTemplateObject || value.opcode == Opcode::DirectEval;
                if (standsAtOnePlace || isUsedElsewhere(id) || (value.opcode == Opcode::NewObject && program.at(value.layout()).isEnvironment()))
                    return std::nullopt;
                ++numberOfValues;
                if (value.opcode != Opcode::NewClosure)
                    continue;
                NestedFunctions nested = nestedFunctionsOf(program, value.function());
                if (definesWhatStandsAtOnePlace(value.function(), nested, false))
                    return std::nullopt;
                numberOfValues += nested.numberOfValues;
                found.closures.append(id);
            }
        }
        if (numberOfValues > maxValuesInCopiedFactory || RegionCloner::checkCanDuplicate(analyses, found.blocks.span(), singleElementSpan(first.block)) != CloneCheck::CanClone)
            return std::nullopt;
        return found;
    }

    std::optional<CodeForWrittenNames> findTurnOfLoopOverWrittenNames(const Function& function, FunctionAnalyses& analyses, ValueID name)
    {
        const Value& read = function.at(name);
        if (read.opcode != Opcode::ResultAt || read.index() != 1)
            return std::nullopt;
        const Value& step = function.at(read.operand(0));
        if (step.opcode != Opcode::IteratorResume || function.at(step.operand(0)).opcode != Opcode::IteratorOpen)
            return std::nullopt;
        std::optional<Vector<StringID, 4>> names = namesWrittenIn(function, function.at(step.operand(0)).iterableOperand());
        if (!names)
            return std::nullopt;

        NaturalLoops& loops = analyses.naturalLoops();
        BlockID header = analyses.useLists().blockOf(name);
        const NaturalLoop* loop = loops.headerOf(header);
        if (!loop || function.at(header).terminator.kind != TerminatorKind::Branch)
            return std::nullopt;
        return findCodeForWrittenNames(function, analyses, Edge { header, 1 }, name, WTF::move(*names), [&](BlockID block) {
            return block != header && (loops.belongsTo(block, *loop) || function.at(block).isHandlerBlock());
        });
    }

    // The code begins by asking which of the written names it was handed. It has a version for each, with copies of the functions it creates closures of, and one for any other value.
    void copyForEachWrittenName(FunctionID functionID, const CodeForWrittenNames& code)
    {
        Vector<Vector<FunctionID, 2>, 4> copiesByName;
        for (size_t index = 0; index < code.writtenNames.size(); ++index) {
            copiesByName.append(code.closures.map<Vector<FunctionID, 2>>([&](ValueID closure) {
                return copyWithItsClosures(program.at(functionID).at(closure).function(), numberOfValuesInCopiesOfFactories);
            }));
        }

        AnalysisContext context { build, program, nullptr, nullptr, functionID };
        Function& function = editor.at(functionID);
        FunctionAnalyses analyses(context, function);
        IRBuilder builder(context, function, types, &analyses);
        BlockID first = code.blocks.first();
        size_t numberOfValuesBefore = function.values.size();
        builder.setOrigin(function.at(code.name).origin());
        auto enterVersion = [&](ValueID name, const Vector<FunctionID, 2>* copiesOfClosures) {
            BlockID asked = builder.currentBlock();
            RegionCloner cloner(builder, function);
            cloner.mapValue(code.name, name);
            cloner.cloneBlocks(code.blocks.span(), [&](ValueID original, Value& clone) {
                if (size_t index = copiesOfClosures ? code.closures.find(original) : notFound; index != notFound)
                    clone.setFunction(copiesOfClosures->at(index));
            });
            builder.moveToEndOf(asked);
            builder.emitJump(cloner.cloneOf(first));
        };
        BlockID question = builder.addBlock(function.at(first).frequency);
        builder.moveToEndOf(question);
        TypeID typeOfName = std::exchange(function.edit(code.name).type, TypeTable::anything);
        ValueID unknownName = code.name;
        for (size_t index = 0; index < code.writtenNames.size(); ++index) {
            Value string = IRBuilder::make(Opcode::String);
            string.setString(code.writtenNames[index]);
            ValueID written = builder.append(WTF::move(string));
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(Opcode::StrictEq, { unknownName, written }));
            builder.moveToEndOf(branch.taken);
            enterVersion(written, &copiesByName[index]);
            builder.moveToEndOf(branch.notTaken);
            unknownName = branch.provenOnNotTaken;
        }
        enterVersion(unknownName, nullptr);
        builder.redirectEdge(code.entry, question, { });
        function.edit(code.name).type = typeOfName;
        numberOfValuesInCopiesOfFactories += function.values.size() - numberOfValuesBefore;
    }

    bool mayGrowByCopies() const { return numberOfValuesInCopiesOfFactories / maxGrowthByCopiedFactories <= numberOfValuesAsRead; }

    // `["a", "b"].forEach(name => { target[name] = function () { .. name .. }; });`
    void copyBodyForEachWrittenName(FunctionID functionID, Vector<StringID, 4>&& names)
    {
        std::optional<CodeForWrittenNames> code;
        {
            AnalysisContext context { build, program, nullptr, nullptr, functionID };
            Function& function = editor.at(functionID);
            FunctionAnalyses analyses(context, function);
            IRBuilder builder(context, function, types, &analyses);
            builder.splitBlock({ entryBlock, 0 });
            ValueID name = function.at(entryBlock).parameters[function.firstParameterIndex()];
            code = findCodeForWrittenNames(function, analyses, Edge { entryBlock, 0 }, name, WTF::move(names), [](BlockID block) { return block != entryBlock; });
        }
        if (code)
            copyForEachWrittenName(functionID, *code);
        Function& function = editor.at(functionID);
        removeTypesOfValues(function);
        compactFunction(function);
    }

    // `for (const name of ["a", "b"]) target[name] = function () { .. name .. };`
    // Flow keeps one answer per function, so the functions under "a" and under "b" would both hold either name.
    void copyCodeForWrittenNamesIn(FunctionID functionID)
    {
        Vector<CodeForWrittenNames, 1> turns;
        Vector<std::pair<FunctionID, Vector<StringID, 4>>, 1> callbacks;
        {
            const Function& function = program.at(functionID);
            if (!std::ranges::any_of(function.values, [](const Value& value) { return value.opcode == Opcode::Data; }))
                return;
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& store) {
                if (store.opcode != Opcode::PutModuleVariable || !isConstantOfModule(store.variable()))
                    return;
                if (const Value& stored = function.at(definitionBehindDeadZoneChecks(function, store.valueOperand())); stored.opcode == Opcode::Data)
                    arraysInConstants.add(store.variable().index() + 1, stored.data());
            });
            AnalysisContext context { build, program, nullptr, nullptr, functionID };
            FunctionAnalyses analyses(context, function);
            BitVector blocksOfTurns;
            auto addTurnHanded = [&](ValueID name) {
                if (turns.containsIf([&](const CodeForWrittenNames& turn) { return turn.name == name; }))
                    return;
                std::optional<CodeForWrittenNames> turn = findTurnOfLoopOverWrittenNames(function, analyses, name);
                if (!turn || std::ranges::any_of(turn->blocks, [&](BlockID block) { return blocksOfTurns.get(block.index()); }))
                    return;
                for (BlockID block : turn->blocks)
                    blocksOfTurns.set(block.index());
                turns.append(WTF::move(*turn));
            };
            forEachNameOfCreatedClosure(function, addTurnHanded);
            forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& call) {
                if (call.opcode != Opcode::Call)
                    return;
                // `for (const name of ["a", "b"]) define(name);`
                std::span<const ValueID> arguments = call.variadicOperands();
                if (std::ranges::any_of(arguments, [&](ValueID argument) { return function.at(definitionBehindDeadZoneChecks(function, argument)).opcode == Opcode::ResultAt; })) {
                    const Value& callee = function.at(definitionBehindDeadZoneChecks(function, call.calleeOperand()));
                    FunctionID called = callee.opcode == Opcode::NewClosure ? callee.function() : callee.opcode == Opcode::GetModuleVariable ? functionHeldBy(callee.variable()) : FunctionID();
                    if (called) {
                        forEachParameterThatNamesCreatedClosure(program.at(called), [&](unsigned parameter) {
                            if (parameter < arguments.size())
                                addTurnHanded(definitionBehindDeadZoneChecks(function, arguments[parameter]));
                        });
                    }
                }
                if (!call.thisValueOperand())
                    return;
                std::optional<Vector<StringID, 4>> names = namesWrittenIn(function, call.thisValueOperand());
                if (!names)
                    return;
                for (ValueID argument : call.variadicOperands()) {
                    const Value& closure = function.at(definitionBehindDeadZoneChecks(function, argument));
                    if (closure.opcode != Opcode::NewClosure || !canBeCopiedForOneCall(program.at(closure.function())))
                        continue;
                    bool firstParameterNamesClosure = false;
                    forEachParameterThatNamesCreatedClosure(program.at(closure.function()), [&](unsigned parameter) {
                        firstParameterNamesClosure |= !parameter;
                    });
                    if (firstParameterNamesClosure && !callbacks.containsIf([&](const auto& callback) { return callback.first == closure.function(); }))
                        callbacks.append({ closure.function(), *names });
                }
            });
        }

        for (const CodeForWrittenNames& turn : turns) {
            if (mayGrowByCopies())
                copyForEachWrittenName(functionID, turn);
        }
        if (!turns.isEmpty()) {
            Function& function = editor.at(functionID);
            removeTypesOfValues(function);
            compactFunction(function);
        }
        for (auto& [callback, names] : callbacks) {
            if (mayGrowByCopies())
                copyBodyForEachWrittenName(callback, WTF::move(names));
        }
    }

    // The function is code that runs once, or a copy that one call has to itself.
    void copyForExpectedCallsIn(const Activation& activation, unsigned depth)
    {
        Vector<ExpectedCall> calls = depth < maxDepthOfCopiedCalls ? findExpectedCallsIn(activation, depth) : Vector<ExpectedCall>();
        Vector<ExpectedRead> reads = depth ? findExpectedReadsIn(activation) : Vector<ExpectedRead>();
        if (calls.isEmpty() && reads.isEmpty())
            return;
        for (ExpectedCall& call : calls) {
            if (numberOfValuesInCopiesOfFactories / maxGrowthByCopiedFactories > numberOfValuesAsRead)
                break;
            call.copy = copyWithItsClosures(call.callee.function, numberOfValuesInCopiesOfFactories);
            addCapturesTo(call.copy, call);
            copyForExpectedCallsIn({ call.copy, call.callee.thisValue, call.callee.arguments, true }, depth + 1);
        }

        AnalysisContext context { build, program, nullptr, nullptr, activation.function };
        Function& function = editor.at(activation.function);
        {
            FunctionAnalyses analyses(context, function);
            IRBuilder builder(context, function, types, &analyses);
            testExpectedReads(activation.function, builder, analyses, WTF::move(reads), calls);
            for (const ExpectedCall& call : calls) {
                if (call.copy)
                    callCopyWhereCalleeIsExpected(builder, call);
            }
        }
        removeTypesOfValues(function);
        compactFunction(function);
    }

    BitVector run()
    {
        PhaseTimes times(build, PassID::CopyClosureFactories);
        // The reader notes which functions name Object.create only where nothing asks for the program as it was read. A build that verifies the program makes the same copies: there every function is looked at.
        Vector<FunctionID> found;
        const Program::BodiesAsRead* bodiesAsRead = program.bodiesAsRead();
        if (!bodiesAsRead) {
            found = collectInParallel<FunctionID>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& functions) {
                bool hasName = std::ranges::any_of(program.functions[index].values, [&](const Value& value) {
                    return namesObjectCreate(program.builtins(), value);
                });
                if (hasName)
                    functions.append(FunctionID(index));
            });
        }
        findBodies();
        findMakers(bodiesAsRead ? bodiesAsRead->functionsThatNameObjectCreate.span() : found.span());
        times.didFinish("findMakers"_s);

        Vector<Vector<CallToCopyFor>> callsByCaller;
        // The list grows while it is walked.
        for (size_t index = 0; index < callersThatRunOnce.size(); ++index) {
            copyCodeForWrittenNamesIn(callersThatRunOnce[index]);
            callsByCaller.append(findCallsIn(callersThatRunOnce[index], 0));
        }
        times.didFinish("findCalls"_s);
        dropCallsThroughFieldsStoredElsewhere(callsByCaller);
        for (const Vector<CallToCopyFor>& calls : callsByCaller) {
            for (const CallToCopyFor& call : calls) {
                if (call.reason == ReasonToCopy::IsClosureFactory)
                    ++numberOfCallsOfFactory.add(call.callee.index() + 1, 0).iterator->value;
            }
        }
        for (size_t index = 0; index < callersThatRunOnce.size(); ++index) {
            if (!callsByCaller[index].isEmpty())
                copyForCalls(callersThatRunOnce[index], WTF::move(callsByCaller[index]), 0);
        }
        times.didFinish("copyForCalls"_s);
        for (FunctionID caller : callersThatRunOnce)
            copyForExpectedCallsIn({ caller, { }, { }, true }, 0);
        times.didFinish("copyForExpectedCalls"_s);
        if (didCopyClasses) {
            nameClassesWhereClassesAboveAreNamed();
            editor.indexClassesAndNames(build.parallelLoops());
        }
        build.statistics().count(StatisticID::ValuesReadForClassesBelowCopies, numberOfValuesReadForClassesBelowCopies);
        build.statistics().count(StatisticID::MembersReadForTypesOfCopies, numberOfMembersReadForTypesOfCopies);
        return WTF::move(copies);
    }
};

} // anonymous namespace

BitVector copyClosureFactories(ProgramEditor& editor, BuildContext& build)
{
    RELEASE_ASSERT(editor.program().form() == IRForm::AsRead);
    return Copier { .editor = editor, .build = build }.run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
