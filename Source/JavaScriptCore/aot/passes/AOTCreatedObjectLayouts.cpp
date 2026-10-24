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

namespace {

struct LayoutCreation {
    FunctionID function;
    ValueID call;
    LayoutID layout;
};

struct CallOfMaker {
    FunctionID caller;
    ValueID call;
    FunctionID maker;
};

} // anonymous namespace

AOT_THRESHOLD(unsigned, maxValuesInCopiedMaker, 400);
AOT_THRESHOLD(unsigned, maxCopiesOfMaker, 128);
AOT_THRESHOLD(unsigned, maxDepthOfCopiedCalls, 3);
AOT_THRESHOLD(unsigned, maxValuesInAllCopies, 1 << 18);

static bool createsObjectBelowItsArgument(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call || operation.expectedResult() || operation.variadicOperands().size() != 1)
        return false;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    return callee.opcode == Opcode::Builtin && builtins.canonicalID(callee.builtin()) == Builtins::ObjectCreate;
}

static void becomeLiterals(const Program& program, BuildContext& build, FunctionID id, Function& function, TypeArena& types, std::span<const LayoutCreation> creations)
{
    AnalysisContext context { build, program, nullptr, nullptr, id };
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
            ValueID prototype = function.at(call).variadicOperands()[0];
            builder.setOrigin(function.at(call).origin());

            Value allocation = IRBuilder::make(Opcode::NewObject);
            allocation.setLayout(creation->layout);
            ValueID object = builder.insertBefore({ block, index }, WTF::move(allocation));

            Value link = IRBuilder::make(Opcode::SetLiteralPrototype, { object, prototype, function.at(call).calleeOperand(), function.at(call).thisValueOperand() });
            link.setRefusesOtherValues(true);
            builder.insertBefore({ block, index }, WTF::move(link));

            Value& finished = function.edit(call);
            Origin origin = finished.origin();
            finished = IRBuilder::make(Opcode::FinishObject, { object });
            finished.setOrigin(origin);
        }
    }
    builder.applyInsertions();
}

static bool isCodeOfProgram(const Function& function)
{
    return function.module && function.role == FunctionRole::Source && function.hasBody();
}

static bool isBodyOfModule(const Function& function)
{
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
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

static std::optional<unsigned> indexOfCapture(const Function& function, ValueID id)
{
    const auto& parameters = function.at(entryBlock).parameters;
    for (unsigned index = 0; index < function.captures.size() && index < parameters.size(); ++index) {
        if (parameters[index] == id)
            return index;
    }
    return std::nullopt;
}

static void forEachValueOf(const Function& function, NOESCAPE const Invocable<void(BlockID, ValueID, const Value&)> auto& functor)
{
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        for (ValueID id : function.blocks[index].values)
            functor(BlockID(index), id, function.at(id));
    }
}

namespace {

class MakerCopier {
public:
    MakerCopier(Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_types(program.types)
    {
    }

    // Answers which functions are copies.
    BitVector run();

private:
    void findMakers(std::span<const FunctionID> functionsThatNameObjectCreate);
    FunctionID functionCalledBy(const Function& caller, const Value& call);
    FunctionID functionHeldBy(VariableID);
    void copyForCallsIn(FunctionID caller, unsigned depth);
    FunctionID copyWithItsClosures(FunctionID original);

    Program& m_program;
    BuildContext& m_build;
    TypeArena m_types;
    // The functions that create closures which hand Object.create what they captured.
    BitVector m_makers;
    // The constants of modules that somebody hands to Object.create.
    BitVector m_constantsHandedToObjectCreate;
    // The bodies of the modules in which a call may get a copy.
    Vector<FunctionID> m_bodies;
    UncheckedKeyHashMap<uint32_t, unsigned> m_numberOfCopies;
    // By constant of a module: the function whose closure it holds. The constants of a module are entered when the first of them is asked for.
    UncheckedKeyHashMap<uint32_t, uint32_t> m_functionsByConstant;
    BitVector m_modulesWithConstantsEntered;
    BitVector m_copies;
    unsigned m_numberOfValuesInCopies { 0 };
};

} // anonymous namespace

// `const make = options => { const members = { .. }; return function (value) { const self = Object.create(members); .. }; }`   `const members = makeMembers({ .. })   .. Object.create(members)`
void MakerCopier::findMakers(std::span<const FunctionID> functionsThatNameObjectCreate)
{
    const BuiltinTable& builtins = m_program.builtins();
    BitVector creators;
    BitVector modules;
    for (FunctionID id : functionsThatNameObjectCreate) {
        const Function& function = m_program.at(id);
        if (!isCodeOfProgram(function))
            continue;
        forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
            if (!createsObjectBelowItsArgument(builtins, function, value))
                return;
            ValueID handed = definitionBehindDeadZoneChecks(function, value.variadicOperands()[0]);
            if (indexOfCapture(function, handed)) {
                creators.set(id.index());
                modules.set(function.module.index());
                return;
            }
            const Value& definition = function.at(handed);
            if (definition.opcode != Opcode::GetModuleVariable || !m_program.at(definition.variable()).flags.contains(VariableFlags::IsImmutable) || !m_program.at(definition.variable()).module)
                return;
            m_constantsHandedToObjectCreate.set(definition.variable().index());
            modules.set(m_program.at(definition.variable()).module.index());
        });
    }
    if (modules.isEmpty())
        return;

    // The function that creates closures of a function stands in the same module.
    bool hasMakers = false;
    for (uint32_t index = 0; index < m_program.numberOfFunctionsAsRead(); ++index) {
        const Function& function = m_program.functions[index];
        if (!isCodeOfProgram(function))
            continue;
        if (isBodyOfModule(function)) {
            // A maker is called by any module.
            m_bodies.append(FunctionID(index));
            continue;
        }
        if (creators.isEmpty() || !modules.get(function.module.index()) || !function.captures.isEmpty() || !canBeCopiedForOneCall(function))
            continue;
        forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
            if (value.opcode == Opcode::NewClosure && creators.get(value.function().index())) {
                m_makers.set(index);
                hasMakers = true;
            }
        });
    }
    if (!hasMakers) {
        m_bodies.removeAllMatching([&](FunctionID body) {
            return !modules.get(m_program.at(body).module.index());
        });
    }
}

FunctionID MakerCopier::functionHeldBy(VariableID variable)
{
    const Variable& constant = m_program.at(variable);
    if (!constant.flags.contains(VariableFlags::IsImmutable) || !constant.module)
        return { };
    if (!m_modulesWithConstantsEntered.get(constant.module.index())) {
        m_modulesWithConstantsEntered.set(constant.module.index());
        FunctionID body = m_program.at(constant.module).body;
        if (body && m_program.at(body).hasBody()) {
            const Function& function = m_program.at(body);
            forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
                if (value.opcode != Opcode::PutModuleVariable || !m_program.at(value.variable()).flags.contains(VariableFlags::IsImmutable))
                    return;
                const Value& stored = function.at(definitionBehindDeadZoneChecks(function, value.valueOperand()));
                if (stored.opcode == Opcode::NewClosure)
                    m_functionsByConstant.add(value.variable().index() + 1, stored.function().index());
            });
        }
    }
    auto found = m_functionsByConstant.find(variable.index() + 1);
    return found == m_functionsByConstant.end() ? FunctionID() : FunctionID(found->value);
}

FunctionID MakerCopier::functionCalledBy(const Function& caller, const Value& call)
{
    const Value& definition = caller.at(definitionBehindDeadZoneChecks(caller, call.calleeOperand()));
    if (definition.opcode == Opcode::GetModuleVariable)
        return functionHeldBy(definition.variable());
    if (definition.opcode == Opcode::NewClosure)
        return definition.function();
    return { };
}

// The closures that the copy creates are closures of copies too, and so on below them.
FunctionID MakerCopier::copyWithItsClosures(FunctionID original)
{
    Function copy = m_program.at(original);
    copy.derivation.source = m_program.at(original).derivation.source ? m_program.at(original).derivation.source : original;
    Vector<std::pair<ValueID, FunctionID>> closures;
    forEachValueOf(copy, [&](BlockID, ValueID id, const Value& value) {
        if (value.opcode == Opcode::NewClosure)
            closures.append({ id, value.function() });
    });
    for (auto [id, function] : closures)
        copy.edit(id).setFunction(copyWithItsClosures(function));
    m_numberOfValuesInCopies += copy.values.size();
    FunctionID result(m_program.functions.size());
    m_program.functions.append(WTF::move(copy));
    m_copies.set(result.index());
    return result;
}

// A class, and a function that constructs objects of a layout, stand at one place of the program. A type that the compiler gave a value names the function itself.
static bool definesWhatStandsAtOnePlace(const Program& program, const Function& function)
{
    if (function.constructedClass)
        return true;
    bool result = false;
    forEachValueOf(function, [&](BlockID, ValueID, const Value& value) {
        switch (value.opcode) {
        case Opcode::NewClosure:
            result |= definesWhatStandsAtOnePlace(program, program.at(value.function()));
            return;
        case Opcode::NewClass:
        case Opcode::NewClassToken:
            result = true;
            return;
        case Opcode::Convert:
            result |= value.declared().flags.contains(PositionFlags::IsFromCompiler) && !program.at(value.declared().type).functions.isEmpty();
            return;
        default:
            return;
        }
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

// The caller runs once, or is a copy that one call has to itself. A call in a loop makes many objects at one place whatever is copied.
void MakerCopier::copyForCallsIn(FunctionID callerID, unsigned depth)
{
    Vector<CallOfMaker> calls;
    {
        const Function& caller = m_program.at(callerID);
        AnalysisContext context { m_build, m_program, nullptr, nullptr, callerID };
        FunctionAnalyses analyses(context, caller);
        BitVector resultsHandedToObjectCreate;
        if (!depth) {
            forEachValueOf(caller, [&](BlockID, ValueID, const Value& value) {
                if (value.opcode == Opcode::PutModuleVariable && m_constantsHandedToObjectCreate.get(value.variable().index()))
                    resultsHandedToObjectCreate.set(definitionBehindDeadZoneChecks(caller, value.valueOperand()).index());
            });
        }
        forEachValueOf(caller, [&](BlockID block, ValueID id, const Value& value) {
            if (value.opcode != Opcode::Call)
                return;
            FunctionID callee = functionCalledBy(caller, value);
            if (!callee || m_copies.get(callee.index()) || !m_program.at(callee).captures.isEmpty() || !canBeCopiedForOneCall(m_program.at(callee)))
                return;
            // What a copy calls to make the objects that it hands on is copied with it: `const members = makeMembers(options)`.
            if (!depth && !m_makers.get(callee.index()) && !resultsHandedToObjectCreate.get(id.index()))
                return;
            const Function& function = m_program.at(callee);
            if (numberOfValuesWithClosures(m_program, function) > maxValuesInCopiedMaker || definesWhatStandsAtOnePlace(m_program, function) || analyses.isInCycle(block))
                return;
            calls.append({ callerID, id, callee });
        });
    }
    for (const CallOfMaker& call : calls) {
        unsigned& numberOfCopies = m_numberOfCopies.add(call.maker.index() + 1, 0).iterator->value;
        if (numberOfCopies >= maxCopiesOfMaker || m_numberOfValuesInCopies > maxValuesInAllCopies)
            continue;
        ++numberOfCopies;
        FunctionID copy = copyWithItsClosures(call.maker);
        if (depth < maxDepthOfCopiedCalls)
            copyForCallsIn(copy, depth + 1);

        Function& caller = m_program.functions[call.caller.index()];
        AnalysisContext context { m_build, m_program, nullptr, nullptr, call.caller };
        FunctionAnalyses analyses(context, caller);
        IRBuilder builder(context, caller, m_types, &analyses);
        builder.setOrigin(caller.at(call.call).origin());
        for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
            const auto& values = caller.blocks[blockIndex].values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (values[index] != call.call)
                    continue;
                Value closure = IRBuilder::make(Opcode::NewClosure);
                closure.setFunction(copy);
                ValueID created = builder.insertBefore({ BlockID(blockIndex), index }, WTF::move(closure));
                caller.edit(call.call).setOperand(OperandIndex::Call::callee, created);
            }
        }
        builder.applyInsertions();
    }
}

BitVector MakerCopier::run()
{
    // The reader notes which functions name Object.create only where nothing asks for the program as it was read. A build that verifies the program makes the same copies: there every function is looked at.
    Vector<FunctionID> found;
    const Program::BodiesAsRead* bodies = m_program.bodiesAsRead();
    if (!bodies) {
        const BuiltinTable& builtins = m_program.builtins();
        for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
            bool namesObjectCreate = std::ranges::any_of(m_program.functions[index].values, [&](const Value& value) {
                return value.opcode == Opcode::Builtin && builtins.canonicalID(value.builtin()) == Builtins::ObjectCreate;
            });
            if (namesObjectCreate)
                found.append(FunctionID(index));
        }
    }
    std::span<const FunctionID> functionsThatNameObjectCreate = bodies ? bodies->functionsThatNameObjectCreate.span() : found.span();
    if (functionsThatNameObjectCreate.empty())
        return { };
    findMakers(functionsThatNameObjectCreate);
    for (FunctionID body : m_bodies)
        copyForCallsIn(body, 0);
    return WTF::move(m_copies);
}

void giveLayoutsToCreatedObjects(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    BitVector copiesOfMakers = MakerCopier(program, build).run();

    // They come in the order of the functions, so a layout gets the same number on any number of threads.
    const BuiltinTable& builtins = program.builtins();
    // Where the reader has noted which functions name Object.create, no other function as read is looked at. The functions that were added since follow them.
    Vector<uint32_t> candidates;
    if (const Program::BodiesAsRead* bodies = program.bodiesAsRead()) {
        for (FunctionID id : bodies->functionsThatNameObjectCreate)
            candidates.append(id.index());
        for (uint32_t index = program.numberOfFunctionsAsRead(); index < program.functions.size(); ++index)
            candidates.append(index);
    } else {
        candidates = Vector<uint32_t>(program.functions.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
    }
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
        becomeLiterals(program, build, id, function, arenas.types, creations.subspan(ofFunction.begin() - creations.begin(), ofFunction.size()));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
