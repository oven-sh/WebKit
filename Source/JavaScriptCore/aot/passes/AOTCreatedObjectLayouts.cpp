/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreatedObjectLayouts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxValuesInCopiedMaker, 400);
AOT_THRESHOLD(unsigned, maxCopiesOfMaker, 128);
AOT_THRESHOLD(unsigned, maxDepthOfCopiedCalls, 3);
AOT_THRESHOLD(unsigned, maxValuesInAllCopies, 1 << 18);

static bool namesObjectCreate(const BuiltinTable& builtins, const Value& value)
{
    return value.opcode == Opcode::Builtin && builtins.canonicalID(value.builtin()) == Builtins::ObjectCreate;
}

static bool createsObjectBelowItsArgument(const BuiltinTable& builtins, const Function& function, const Value& operation)
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

static void forEachValueOf(const Function& function, NOESCAPE const Invocable<void(BlockID, ValueID, const Value&)> auto& functor)
{
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        for (ValueID id : function.blocks[index].values)
            functor(BlockID(index), id, function.at(id));
    }
}

// A class, and a function that constructs objects of a layout, stand at one place of the program. A type that the compiler gave a value names the function itself.
static bool definesWhatStandsAtOnePlace(const Program& program, const Function& function)
{
    if (function.constructedClass)
        return true;
    bool result = false;
    forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
        if (value.opcode == Opcode::NewClosure)
            result |= definesWhatStandsAtOnePlace(program, program.at(value.function()));
        else if (value.opcode == Opcode::Convert)
            result |= value.declared().flags.contains(PositionFlags::IsFromCompiler) && !program.at(value.declared().type).functions.isEmpty();
        else
            result |= value.opcode == Opcode::NewClass || value.opcode == Opcode::NewClassToken;
    });
    return result;
}

static unsigned numberOfValuesWithClosures(const Program& program, const Function& function)
{
    unsigned result = function.values.size();
    forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
        if (value.opcode == Opcode::NewClosure)
            result += numberOfValuesWithClosures(program, program.at(value.function()));
    });
    return result;
}

namespace {

struct LayoutCreation {
    FunctionID function;
    ValueID call;
    LayoutID layout;
};

struct MakerCopier {
    Program& program;
    BuildContext& build;
    TypeArena types { program.types };
    // The functions that create closures which hand Object.create what they captured.
    BitVector makers;
    // The constants of modules that somebody hands to Object.create.
    BitVector constantsHandedToObjectCreate;
    // The bodies of the modules in which a call may get a copy.
    Vector<FunctionID> bodies;
    UncheckedKeyHashMap<uint32_t, unsigned> numberOfCopies;
    // By constant of a module: the function whose closure it holds. The constants of a module are entered when the first of them is asked for.
    UncheckedKeyHashMap<uint32_t, uint32_t> functionsByConstant;
    BitVector modulesWithConstantsEntered;
    BitVector copies;
    unsigned numberOfValuesInCopies { 0 };

    bool isConstantOfModule(VariableID variable) const { return program.at(variable).flags.contains(VariableFlags::IsImmutable) && program.at(variable).module; }

    // `const make = options => { const members = { .. }; return function (value) { const self = Object.create(members); .. }; }`   `const members = makeMembers({ .. })   .. Object.create(members)`
    void findMakers(std::span<const FunctionID> functionsThatNameObjectCreate)
    {
        BitVector creators;
        BitVector modules;
        for (FunctionID id : functionsThatNameObjectCreate) {
            const Function& function = program.at(id);
            if (!isCodeOfProgram(function))
                continue;
            forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
                if (!createsObjectBelowItsArgument(program.builtins(), function, value))
                    return;
                ValueID handed = definitionBehindDeadZoneChecks(function, value.variadicOperands()[0]);
                if (isCapture(function, handed)) {
                    creators.set(id.index());
                    modules.set(function.module.index());
                    return;
                }
                const Value& definition = function.at(handed);
                if (definition.opcode != Opcode::GetModuleVariable || !isConstantOfModule(definition.variable()))
                    return;
                constantsHandedToObjectCreate.set(definition.variable().index());
                modules.set(program.at(definition.variable()).module.index());
            });
        }
        if (modules.isEmpty())
            return;

        // The function that creates closures of a function stands in the same module.
        for (uint32_t index = 0; index < program.numberOfFunctionsAsRead(); ++index) {
            const Function& function = program.functions[index];
            if (!isCodeOfProgram(function))
                continue;
            if (function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule) {
                // A maker is called by any module.
                bodies.append(FunctionID(index));
                continue;
            }
            if (creators.isEmpty() || !modules.get(function.module.index()) || !function.captures.isEmpty() || !canBeCopiedForOneCall(function))
                continue;
            forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
                if (value.opcode == Opcode::NewClosure && creators.get(value.function().index()))
                    makers.set(index);
            });
        }
        if (makers.isEmpty()) {
            bodies.removeAllMatching([&](FunctionID body) {
                return !modules.get(program.at(body).module.index());
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
            forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
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

    // The closures that the copy creates are closures of copies too, and so on below them.
    FunctionID copyWithItsClosures(FunctionID original)
    {
        Function copy = program.at(original);
        if (!copy.derivation.source)
            copy.derivation.source = original;
        Vector<std::pair<ValueID, FunctionID>> closures;
        forEachValueOf(copy, [&](BlockID, ValueID id, const Value& value) {
            if (value.opcode == Opcode::NewClosure)
                closures.append({ id, value.function() });
        });
        for (auto [id, function] : closures)
            copy.edit(id).setFunction(copyWithItsClosures(function));
        numberOfValuesInCopies += copy.values.size();
        FunctionID result(program.functions.size());
        program.functions.append(WTF::move(copy));
        copies.set(result.index());
        return result;
    }

    // The caller runs once, or is a copy that one call has to itself. A call in a loop makes many objects at one place whatever is copied.
    void copyForCallsIn(FunctionID callerID, unsigned depth)
    {
        AnalysisContext context { build, program, nullptr, nullptr, callerID };
        Vector<std::pair<ValueID, FunctionID>> callsOfMakers;
        {
            const Function& caller = program.at(callerID);
            FunctionAnalyses analyses(context, caller);
            BitVector resultsHandedToObjectCreate;
            if (!depth) {
                forEachValueOf(caller, [&](BlockID, ValueID, const Value& value) {
                    if (value.opcode == Opcode::PutModuleVariable && constantsHandedToObjectCreate.get(value.variable().index()))
                        resultsHandedToObjectCreate.set(definitionBehindDeadZoneChecks(caller, value.valueOperand()).index());
                });
            }
            forEachValueOf(caller, [&](BlockID block, ValueID id, const Value& value) {
                if (value.opcode != Opcode::Call)
                    return;
                const Value& definition = caller.at(definitionBehindDeadZoneChecks(caller, value.calleeOperand()));
                FunctionID callee = definition.opcode == Opcode::GetModuleVariable ? functionHeldBy(definition.variable()) : definition.opcode == Opcode::NewClosure ? definition.function() : FunctionID();
                if (!callee || copies.get(callee.index()) || !program.at(callee).captures.isEmpty() || !canBeCopiedForOneCall(program.at(callee)))
                    return;
                // What a copy calls to make the objects that it hands on is copied with it: `const members = makeMembers(options)`.
                if (!depth && !makers.get(callee.index()) && !resultsHandedToObjectCreate.get(id.index()))
                    return;
                const Function& function = program.at(callee);
                if (numberOfValuesWithClosures(program, function) > maxValuesInCopiedMaker || definesWhatStandsAtOnePlace(program, function) || analyses.isInCycle(block))
                    return;
                callsOfMakers.append({ id, callee });
            });
        }
        for (auto [call, maker] : callsOfMakers) {
            unsigned& numberOfCopiesOfMaker = numberOfCopies.add(maker.index() + 1, 0).iterator->value;
            if (numberOfCopiesOfMaker >= maxCopiesOfMaker || numberOfValuesInCopies > maxValuesInAllCopies)
                continue;
            ++numberOfCopiesOfMaker;
            FunctionID copy = copyWithItsClosures(maker);
            if (depth < maxDepthOfCopiedCalls)
                copyForCallsIn(copy, depth + 1);

            Function& caller = program.functions[callerID.index()];
            FunctionAnalyses analyses(context, caller);
            IRBuilder builder(context, caller, types, &analyses);
            builder.setOrigin(caller.at(call).origin());
            for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
                const auto& values = caller.blocks[blockIndex].values;
                for (uint32_t index = 0; index < values.size(); ++index) {
                    if (values[index] != call)
                        continue;
                    Value closure = IRBuilder::make(Opcode::NewClosure);
                    closure.setFunction(copy);
                    ValueID created = builder.insertBefore({ BlockID(blockIndex), index }, WTF::move(closure));
                    caller.edit(call).setOperand(OperandIndex::Call::callee, created);
                }
            }
            builder.applyInsertions();
        }
    }

    // Answers which functions are copies.
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
        std::span<const FunctionID> functionsThatNameObjectCreate = bodiesAsRead ? bodiesAsRead->functionsThatNameObjectCreate.span() : found.span();
        if (functionsThatNameObjectCreate.empty())
            return { };
        findMakers(functionsThatNameObjectCreate);
        for (FunctionID body : bodies)
            copyForCallsIn(body, 0);
        return WTF::move(copies);
    }
};

} // anonymous namespace

static void becomeLiterals(const AnalysisContext& context, Function& function, TypeArena& types, std::span<const LayoutCreation> creations)
{
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID call = function.at(block).values[index];
            auto creation = std::ranges::find(creations, call, &LayoutCreation::call);
            if (creation == creations.end())
                continue;
            Origin origin = function.at(call).origin();
            builder.setOrigin(origin);

            Value allocation = IRBuilder::make(Opcode::NewObject);
            allocation.setLayout(creation->layout);
            ValueID object = builder.insertBefore({ block, index }, WTF::move(allocation));

            Value link = IRBuilder::make(Opcode::SetLiteralPrototype, { object, function.at(call).variadicOperands()[0], function.at(call).calleeOperand(), function.at(call).thisValueOperand() });
            link.setRefusesOtherValues(true);
            builder.insertBefore({ block, index }, WTF::move(link));

            Value& finished = function.edit(call);
            finished = IRBuilder::make(Opcode::FinishObject, { object });
            finished.setOrigin(origin);
        }
    }
    builder.applyInsertions();
}

void giveLayoutsToCreatedObjects(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    BitVector copiesOfMakers = MakerCopier { .program = program, .build = build }.run();

    const BuiltinTable& builtins = program.builtins();
    const Program::BodiesAsRead* bodies = program.bodiesAsRead();
    // Where the reader has noted which functions name Object.create, no other function as read is looked at. The functions that were added since follow them.
    Vector<uint32_t> candidates;
    if (bodies) {
        for (FunctionID id : bodies->functionsThatNameObjectCreate)
            candidates.append(id.index());
    }
    for (uint32_t index = bodies ? program.numberOfFunctionsAsRead() : 0; index < program.functions.size(); ++index)
        candidates.append(index);
    // They come in the order of the functions, so a layout gets the same number on any number of threads.
    Vector<LayoutCreation> creations = collectInParallel<LayoutCreation>(build.parallelLoops(), candidates.size(), [&](unsigned position, Vector<LayoutCreation>& found) {
        uint32_t index = candidates[position];
        const Function& function = program.functions[index];
        // The model of a built-in says what the built-in does and is never run: what it answers is made by other code, with no layout of the model.
        if (!function.module || function.role == FunctionRole::Model)
            return;
        for (const Block& block : function.blocks) {
            // A tail call stays a call.
            ValueID returnedCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.operand(0) : ValueID();
            for (ValueID id : block.values) {
                if (id != returnedCall && createsObjectBelowItsArgument(builtins, function, function.at(id)))
                    found.append({ FunctionID(index), id, { } });
            }
        }
    });
    // From here on the bodies are no longer as they were read.
    program.forgetBodiesAsRead();
    if (creations.isEmpty())
        return;

    ProgramEditor editor(program);
    Vector<FunctionID> functions;
    // The copies of a function make the same objects.
    UncheckedKeyHashMap<uint64_t, LayoutID> layoutsByPlace;
    for (LayoutCreation& creation : creations) {
        const Function& function = program.at(creation.function);
        if (functions.isEmpty() || functions.last() != creation.function)
            functions.append(creation.function);
        uint64_t place = (static_cast<uint64_t>(function.module.index()) + 1) << 32 | function.at(creation.call).position;
        // What each copy of a maker makes has a prototype object of its own.
        bool hasPlace = function.at(creation.call).hasPosition() && !copiesOfMakers.get(creation.function.index());
        if (LayoutID known = hasPlace ? layoutsByPlace.get(place) : LayoutID()) {
            creation.layout = known;
            continue;
        }
        Layout layout;
        layout.kind = LayoutKind::Instance;
        layout.module = function.module;
        layout.position = function.at(creation.call).position;
        layout.flags = { LayoutFlags::IsOfCreatedObject };
        creation.layout = editor.addLayout(WTF::move(layout));
        if (hasPlace)
            layoutsByPlace.add(place, creation.layout);
    }
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        auto ofFunction = std::ranges::equal_range(creations, id, { }, &LayoutCreation::function);
        becomeLiterals({ build, program, nullptr, nullptr, id }, function, arenas.types, creations.subspan(ofFunction.begin() - creations.begin(), ofFunction.size()));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
