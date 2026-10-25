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
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTRegionCloner.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxValuesInCopiedFactory, 200);
// How many times the size of the program as read all copies of factories together may have.
AOT_THRESHOLD(unsigned, maxGrowthByCopiedFactories, 4);
AOT_THRESHOLD(unsigned, maxValuesInCopiedMaker, 400);
AOT_THRESHOLD(unsigned, maxCopiesOfMaker, 128);
AOT_THRESHOLD(unsigned, maxDepthOfCopiedCalls, 3);
AOT_THRESHOLD(unsigned, maxValuesInAllCopiesOfMakers, 1 << 18);
AOT_THRESHOLD(unsigned, maxStepsToExpectedValue, 12);

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

static unsigned numberOfValuesWithClosures(const Program& program, const Function& function)
{
    unsigned result = function.values.size();
    forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
        if (value.opcode == Opcode::NewClosure)
            result += numberOfValuesWithClosures(program, program.at(value.function()));
    });
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
// The functor is handed each positional parameter that names a closure the function creates.
static void forEachParameterThatNamesCreatedClosure(const Function& function, NOESCAPE const Invocable<void(unsigned)> auto& functor)
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
            if (std::optional<unsigned> parameter = positionalParameterAt(function, definitionBehindDeadZoneChecks(function, value.operand(OperandIndex::ElemSet::property))))
                functor(*parameter);
            return;
        }
        if (value.opcode != Opcode::Call || !std::ranges::any_of(value.variadicOperands(), holdsCreatedClosure))
            return;
        for (ValueID argument : value.variadicOperands()) {
            if (std::optional<unsigned> parameter = positionalParameterAt(function, definitionBehindDeadZoneChecks(function, argument)))
                functor(*parameter);
        }
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

enum class ReasonToCopy : uint8_t { IsClosureFactory, MakesWhatObjectCreateIsHanded };

struct CallToCopyFor {
    ProgramPoint point;
    ValueID call;
    FunctionID callee;
    // The closure that is called, where the caller creates it.
    ValueID closure;
    ReasonToCopy reason;
    // The field of an environment that the closure was read from: the call gets a copy if the program stores to the field at one place.
    std::optional<FieldReference> holder { };
    FunctionID copy { };
};

static uint64_t keyOf(FieldReference field)
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

using ExpectedValues = Vector<ExpectedValue, 4>;

// `new Helper(target, "name")`
struct ExpectedInstance {
    FunctionID constructor;
    ExpectedValues arguments;

    friend bool operator==(const ExpectedInstance&, const ExpectedInstance&) = default;
};

struct Activation {
    FunctionID function;
    ExpectedValue thisValue { };
    ExpectedValues arguments { };
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
    // The bodies of the modules of the program, and what they call at one place outside loops.
    Vector<FunctionID> callersThatRunOnce;
    BitVector isCallerThatRunsOnce;
    UncheckedKeyHashMap<uint32_t, unsigned> numberOfCopiesOfMaker;
    UncheckedKeyHashMap<uint32_t, unsigned> numberOfCallsOfFactory;
    // By constant of a module: the function whose closure it holds. The constants of a module are entered when the first of them is asked for.
    UncheckedKeyHashMap<uint32_t, uint32_t> functionsByConstant;
    BitVector modulesWithConstantsEntered;
    UncheckedKeyHashMap<uint32_t, Size> sizes;
    std::optional<BitVector> functionsNamedByClosedTypes;
    BitVector copies;
    UncheckedKeyHashMap<uint32_t, FunctionID> originalsOfCopies;
    // The layouts of the environments of the copies that are being made, each with its original.
    Vector<std::pair<LayoutID, LayoutID>, 4> copiedEnvironments;
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
    unsigned numberOfValuesAsRead { 0 };
    unsigned numberOfValuesInCopiesOfFactories { 0 };
    unsigned numberOfValuesInCopiesOfMakers { 0 };

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
                if (definition.opcode == Opcode::GetModuleVariable && isConstantOfModule(definition.variable()))
                    constantsHandedToObjectCreate.set(definition.variable().index());
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
            });
        }
        auto found = functionsByConstant.find(variable.index() + 1);
        return found == functionsByConstant.end() ? FunctionID() : FunctionID(found->value);
    }

    // A type that lists functions and admits no others holds no closure of a copy of one of them.
    bool isNamedByClosedType(FunctionID id)
    {
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

    // A class, a function that constructs objects of a layout, the array of a tagged template and the scope of an eval stand at one place of the program.
    // The closure of the copy that is called is held by nothing but the call, so no type speaks of it.
    bool definesWhatStandsAtOnePlace(FunctionID id, bool isHeldByCallAlone = true)
    {
        const Function& function = program.at(id);
        if (function.constructedClass || (!isHeldByCallAlone && isNamedByClosedType(id)))
            return true;
        bool result = false;
        forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewClosure)
                result |= definesWhatStandsAtOnePlace(value.function(), false);
            else
                result |= value.opcode == Opcode::NewClass || value.opcode == Opcode::NewClassToken || value.opcode == Opcode::NewTemplateObject || value.opcode == Opcode::DirectEval;
        });
        return result;
    }

    Size sizeOf(FunctionID id)
    {
        return sizes.ensure(id.index() + 1, [&] {
            return Size { numberOfValuesWithClosures(program, program.at(id)), definesWhatStandsAtOnePlace(id) };
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
            capture.type = withCopiedEnvironment(capture.type);
        for (Value& value : copy.editAllValues()) {
            value.type = withCopiedEnvironment(value.type);
            if (value.opcode == Opcode::NewObject || value.opcode == Opcode::GetEnvironmentField || value.opcode == Opcode::PutEnvironmentField)
                value.setLayout(copyOf(value.layout()));
        }
    }

    // The closures that the copy creates are closures of copies too, and so on below them.
    FunctionID copyWithItsClosures(FunctionID original, unsigned& numberOfValuesInCopies)
    {
        Function copy = program.at(original);
        size_t numberOfEnvironmentsAround = copiedEnvironments.size();
        copyEnvironmentsOf(copy);
        Vector<std::pair<ValueID, FunctionID>> closures;
        forEachValueOf(copy, [&](ProgramPoint, ValueID id, const Value& value) {
            if (value.opcode == Opcode::NewClosure)
                closures.append({ id, value.function() });
        });
        for (auto [id, function] : closures)
            copy.edit(id).setFunction(copyWithItsClosures(function, numberOfValuesInCopies));
        copiedEnvironments.shrink(numberOfEnvironmentsAround);
        numberOfValuesInCopies += copy.values.size();
        FunctionID result = editor.addFunctionLike(original, WTF::move(copy));
        copies.set(result.index());
        originalsOfCopies.add(result.index() + 1, original);
        return result;
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
                    auto added = storesToEnvironments.add(keyOf({ value.layout(), value.field() }), id);
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
        if (!depth && !constantsHandedToObjectCreate.isEmpty())
            findStores();
        // A read of a `let` or a `const` throws by itself until the store has run. A `var` answers undefined until then, and the call throws.
        auto isReadBehindItsStore = [&](VariableID variable, ProgramPoint read) {
            if (program.at(variable).initialization != VariableInitialization::Undefined)
                return true;
            findStores();
            ValueID store = storesToConstants.get(variable.index() + 1);
            return store && analyses.dominates(store, read);
        };
        // `var make = ..` in a function that has closures: the variable is a field of the environment of the function.
        auto closureStoredInto = [&](const Value& read, ProgramPoint point) -> ValueID {
            findStores();
            ValueID store = storesToEnvironments.get(keyOf({ read.layout(), read.field() }));
            if (!store || caller.at(store).operand(OperandIndex::PutEnvironmentField::environment) != read.operand(OperandIndex::GetEnvironmentField::environment) || !analyses.dominates(store, point))
                return { };
            return definitionBehindDeadZoneChecks(caller, caller.at(store).valueOperand());
        };
        forEachValueOf(caller, [&](ProgramPoint point, ValueID id, const Value& value) {
            // The text of a CommonJS module is a function that the body of the module calls.
            if (value.opcode == Opcode::CallDirect && !depth && !analyses.isInCycle(point.block))
                addCallerThatRunsOnce(value.function());
            if (value.opcode != Opcode::Call)
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
            if (isCreatedByCaller && !holder && !depth && analyses.useLists().usesOf(definitionID).size() == 1 && !analyses.isInCycle(point.block))
                addCallerThatRunsOnce(definition.function());
            FunctionID callee = isCreatedByCaller ? definition.function() : definition.opcode == Opcode::GetModuleVariable ? functionHeldBy(definition.variable()) : FunctionID();
            if (!callee || copies.get(callee.index()) || !canBeCopiedForOneCall(program.at(callee)))
                return;
            const Function& function = program.at(callee);
            if (!isCreatedByCaller && (!function.captures.isEmpty() || !isReadBehindItsStore(definition.variable(), point)))
                return;
            // What a copy calls to make the objects that it hands on is copied with it: `const members = makeMembers(options)`.
            bool makesWhatObjectCreateIsHanded = function.captures.isEmpty() && (depth || makers.get(callee.index()) || resultsHandedToObjectCreate.get(id.index()));
            bool definesClosureUnderWrittenName = !makesWhatObjectCreateIsHanded && handsWrittenNameToDefineClosureUnder(caller, value, function);
            bool isClosureFactory = definesClosureUnderWrittenName || (!makesWhatObjectCreateIsHanded && returnsClosureItCreates(function));
            if (!makesWhatObjectCreateIsHanded && !isClosureFactory)
                return;
            if (!definesClosureUnderWrittenName && isNamedByClosedType(callee))
                return;
            Size size = sizeOf(callee);
            if (size.numberOfValuesWithClosures > (isClosureFactory ? maxValuesInCopiedFactory : maxValuesInCopiedMaker) || size.definesWhatStandsAtOnePlace || analyses.isInCycle(point.block))
                return;
            calls.append({ point, id, callee, isCreatedByCaller ? definitionID : ValueID(), isClosureFactory ? ReasonToCopy::IsClosureFactory : ReasonToCopy::MakesWhatObjectCreateIsHanded, holder });
        });
        return calls;
    }

    FunctionID copyFor(const CallToCopyFor& call, unsigned depth)
    {
        switch (call.reason) {
        case ReasonToCopy::IsClosureFactory:
            // The only call of a factory has the factory to itself.
            if (numberOfCallsOfFactory.get(call.callee.index() + 1) < 2 || numberOfValuesInCopiesOfFactories / maxGrowthByCopiedFactories > numberOfValuesAsRead)
                return { };
            return copyWithItsClosures(call.callee, numberOfValuesInCopiesOfFactories);
        case ReasonToCopy::MakesWhatObjectCreateIsHanded: {
            unsigned& numberOfCopies = numberOfCopiesOfMaker.add(call.callee.index() + 1, 0).iterator->value;
            if (numberOfCopies >= maxCopiesOfMaker || numberOfValuesInCopiesOfMakers > maxValuesInAllCopiesOfMakers)
                return { };
            ++numberOfCopies;
            FunctionID copy = copyWithItsClosures(call.callee, numberOfValuesInCopiesOfMakers);
            if (depth < maxDepthOfCopiedCalls)
                copyForCalls(copy, findCallsIn(copy, depth + 1), depth + 1);
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
        FunctionAnalyses analyses(context, caller);
        IRBuilder builder(context, caller, types, &analyses);
        for (const CallToCopyFor& call : calls) {
            if (!call.copy)
                continue;
            builder.setOrigin(caller.at(call.call).origin());
            // A closure of the copy over what the closure of the original captured.
            Value closure = call.closure ? Value(caller.at(call.closure)) : IRBuilder::make(Opcode::NewClosure);
            closure.setFunction(call.copy);
            ValueID created = builder.insertBefore(call.point, WTF::move(closure));
            Value& edited = caller.edit(call.call);
            edited.setOperand(OperandIndex::Call::callee, created);
            // The copy has this one call, so its body moves here and is no second copy.
            edited.setIsAlwaysInlined(true);
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
                    numberOfStores.add(keyOf(*call.holder), 0);
            }
        }
        if (numberOfStores.isEmpty())
            return;
        Vector<uint64_t> stores = collectInParallel<uint64_t>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<uint64_t>& found) {
            for (const Value& value : program.functions[index].values) {
                if (value.opcode == Opcode::PutEnvironmentField && numberOfStores.contains(keyOf({ value.layout(), value.field() })))
                    found.append(keyOf({ value.layout(), value.field() }));
            }
        });
        for (uint64_t key : stores)
            ++numberOfStores.find(key)->value;
        for (const EvalScope& scope : program.evalScopes) {
            for (const EvalBinding& binding : scope.bindings) {
                if (!binding.layout || !binding.field)
                    continue;
                if (auto found = numberOfStores.find(keyOf({ binding.layout, *binding.field })); found != numberOfStores.end())
                    ++found->value;
            }
        }
        for (Vector<CallToCopyFor>& calls : callsByCaller) {
            calls.removeAllMatching([&](const CallToCopyFor& call) {
                return call.holder && numberOfStores.get(keyOf(*call.holder)) != 1;
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
                    return noteStore(closuresInEnvironments, keyOf({ value.layout(), value.field() }), stored.function());
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
        for (ExpectedValue argument : ExpectedValues(expectedInstances[value.index].arguments))
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
            copy.captures.append(DeclaredType { .type = TypeTable::anything });
            holdersOfLiterals.add(keyOfHolder(copyID, layout), added);
        }
    }

    ExpectedValue expectClosureOf(FunctionID function) const
    {
        return function ? ExpectedValue { ExpectedValue::Kind::Closure, function.index() } : ExpectedValue();
    }

    ExpectedValues expectedValuesOf(const Activation& activation, std::span<const ValueID> values, unsigned steps)
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
            ValueID returned = block.terminator.kind == TerminatorKind::Return ? block.terminator.valueOperand() : ValueID();
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
            noteStoresOfCodeThatRunsOnce();
            return expectClosureOf(closuresInVariables.get(value.variable().index() + 1));
        }
        if (value.opcode == Opcode::GetEnvironmentField) {
            noteStoresOfCodeThatRunsOnce();
            return expectClosureOf(closuresInEnvironments.get(keyOf({ value.layout(), value.field() })));
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
            if (auto named = readsByName.find(value.numberOfOperands() ? value.operand(0).index() + 1 : 0); isOtherName && named != readsByName.end()) {
                readsByName.add(id.index() + 1, named->value);
                continue;
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

    // The function is code that runs once, or a copy that one call has to itself.
    void copyForExpectedCallsIn(const Activation& activation, unsigned depth)
    {
        Vector<ExpectedCall> calls = findExpectedCallsIn(activation, depth);
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
        FunctionAnalyses analyses(context, function);
        IRBuilder builder(context, function, types, &analyses);
        testExpectedReads(activation.function, builder, analyses, WTF::move(reads), calls);
        for (const ExpectedCall& call : calls) {
            if (call.copy)
                callCopyWhereCalleeIsExpected(builder, call);
        }
    }

    BitVector run()
    {
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

        Vector<Vector<CallToCopyFor>> callsByCaller;
        // The list grows while it is walked.
        for (size_t index = 0; index < callersThatRunOnce.size(); ++index)
            callsByCaller.append(findCallsIn(callersThatRunOnce[index], 0));
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
        for (FunctionID caller : callersThatRunOnce)
            copyForExpectedCallsIn({ caller, { }, { }, true }, 0);
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
