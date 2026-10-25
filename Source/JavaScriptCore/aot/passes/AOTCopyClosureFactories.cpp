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
    // The layouts of the environments of the copies that are being made, each with its original.
    Vector<std::pair<LayoutID, LayoutID>, 4> copiedEnvironments;
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
    bool definesWhatStandsAtOnePlace(FunctionID id)
    {
        const Function& function = program.at(id);
        if (function.constructedClass || isNamedByClosedType(id))
            return true;
        bool result = false;
        forEachValueOf(function, [&](ProgramPoint, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewClosure)
                result |= definesWhatStandsAtOnePlace(value.function());
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
            bool isClosureFactory = !makesWhatObjectCreateIsHanded && returnsClosureItCreates(function);
            if (!makesWhatObjectCreateIsHanded && !isClosureFactory)
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
