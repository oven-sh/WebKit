/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateEntrypointsInternal.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCreateBoundFunctions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEntrypointsNothingEnters.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTInterningPhases.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRepresentation.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSlotPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AOTWrittenFunction.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

Function createDerivedHeader(const Function& target, FunctionID targetID, FunctionRole role, uint32_t keyParameter)
{
    Function header;
    header.name = target.name;
    header.module = target.module;
    header.position = target.position;
    header.text = target.text;
    header.kind = target.kind;
    header.role = role;
    header.flags = target.flags & FunctionFlags::IsChecked;
    header.length = target.length;
    header.enclosingRuntimeFunction = target.enclosingRuntimeFunction;
    header.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    header.capabilities = capabilitiesOf(role);
    header.derivation = Derivation { .source = targetID, .parameter = keyParameter };
    return header;
}

void emitReturnOfEntrypointCall(IRBuilder& builder, ValueID call)
{
    const Function& function = builder.function();
    const Block& block = function.at(builder.currentBlock());
    RELEASE_ASSERT(!block.handler && block.values.last() == call && function.at(call).opcode == Opcode::CallDirect);
    if (builder.context().program.typeOf(function, call).isNever()) {
        builder.emitUnreachable();
        return;
    }

    // The frame stays only where the callee returns its result in another form than the entrypoint.
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call });
    terminator.setRequirement(TailCallRequirement::Allowed);
    builder.emitTerminator(WTF::move(terminator));
}

static bool mayBeReadWithItsFunction(const Variable& variable)
{
    switch (variable.flow.reads) {
    case ReadState::Unknown:
    case ReadState::OnlyAccessed:
        return true;
    case ReadState::OnlyCalled:
        // A call cannot name its callee while another value may be stored, so it reads the variable.
        return variable.flow.stores != StoreCount::Never;
    case ReadState::NeverRead:
    case ReadState::OnlyWhereDefined:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A function under a key in brackets takes its name from the key where its object is made. One key names the function as it is written.
static BitVector findWrittenFunctionsNamedBySeveralKeys(const Program& program, const ParallelLoops& loops)
{
    struct NameTaken {
        FunctionID written;
        // None for an async function: the tables of names leave those out.
        ConstantKey key;
        MethodKind kind;
    };
    Vector<NameTaken> names = collectInParallel<NameTaken>(loops, program.functions.size(), [&](unsigned index, Vector<NameTaken>& found) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& operation = function.at(id);
                if (operation.opcode != Opcode::SetFunctionName)
                    continue;
                ConstantKey key = constantKeyOf(program, program.typeOf(function, operation.operand(OperandIndex::SetFunctionName::computedKey)));
                for (FunctionID named : program.typeOf(function, operation.operand(OperandIndex::SetFunctionName::function)).functions)
                    found.append({ writtenFunctionOf(program, named), program.at(named).has(FunctionFlags::IsAsync) ? ConstantKey { } : key, operation.methodKind() });
            }
        }
    });
    BitVector functions(program.functions.size());
    UncheckedKeyHashMap<uint32_t, const NameTaken*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstNames;
    for (const NameTaken& name : names) {
        const NameTaken& first = *firstNames.add(name.written.index(), &name).iterator->value;
        if (!name.key || first.key.kind != name.key.kind || first.key.text != name.key.text || first.key.symbol != name.key.symbol || first.kind != name.kind)
            functions.quickSet(name.written.index());
    }
    return functions;
}

// A stack frame shows the name that the function object has when the stack is read. One function object stands for every frame of the function as it is written.
static void markFunctionsThatRecordTheirCallee(ProgramEditor& editor, const FlowResults& flow, const ParallelLoops& loops, Statistics& statistics)
{
    const Program& program = editor.program();
    auto writtenFunctionOf = [&](FunctionID id) {
        return Typed::writtenFunctionOf(program, id);
    };
    BitVector namedBySeveralKeys = findWrittenFunctionsNamedBySeveralKeys(program, loops);
    auto hasClosures = [&](const Function& function) {
        return function.hasBody() && canBeCalledByValueOf(function.role);
    };
    Vector<PropertyKeyID, 2> keysOfNames { program.keyOf(BuiltinNames::name) };
    if (StringID text = program.strings.find(byteCast<uint8_t>("displayName"_span))) {
        if (auto key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }))
            keysOfNames.append(*key);
    }
    // What stands behind the tests at the head of a function runs under the function object of the tests.
    BitVector standsBehindEntryChecks(program.functions.size());
    for (const Function& function : program.functions) {
        if (const auto* record = function.creator.tryAs<CreatorRecord::EntryChecks>()) {
            standsBehindEntryChecks.quickSet(record->body.index());
            standsBehindEntryChecks.quickSet(record->fallback.index());
        }
    }
    BitVector hasBody(program.functions.size());
    BitVector hasForm(program.functions.size());
    BitVector hasSeveralForms(program.functions.size());
    BitVector recordsFromTheStart(program.functions.size());
    Vector<FunctionID> marked;
    Vector<FunctionID> writtenWithClosuresOutOfSight;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        uint32_t written = writtenFunctionOf(FunctionID(index)).index();
        hasBody.quickSet(written);
        // No function object is made of a form that is only called directly.
        if (hasClosures(program.functions[index]) && !program.functions[index].hasOnlyDirectCallers() && !standsBehindEntryChecks.quickGet(index) && hasForm.set(written))
            hasSeveralForms.quickSet(written);
    }
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        FunctionID id(index);
        // Nothing enters a function that has no body in any form, so no frame shows a name of its function objects and the run time keeps none.
        if (FunctionID written = writtenFunctionOf(id); !hasBody.quickGet(written.index())) {
            if (canBeCalledByValueOf(function.role) && flow.closuresMayBeRenamed(id))
                editor.at(written).capabilities.add(FunctionCapabilities::RecordsCallee);
            continue;
        }
        // The body of a generator, and of an async function that may wait, runs under a function object of its own.
        bool mayWait = function.role == FunctionRole::CoroutineRamp && function.creator.as<CreatorRecord::Coroutine>().worker != function.creator.as<CreatorRecord::Coroutine>().body;
        if (!hasClosures(function) || mayWait || function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::IsHiddenInStackTraces }))
            continue;
        FunctionID written = writtenFunctionOf(id);
        if (!flow.closuresMayBeRenamed(id) && !namedBySeveralKeys.quickGet(written.index())) {
            if (flow.closuresGoOutOfSightUnnamed(id) && (!flow.closureIsMadeOnce(id) || hasSeveralForms.quickGet(written.index()))) {
                writtenWithClosuresOutOfSight.append(written);
                statistics.count(StatisticID::RenamableFunctionsOutOfSightUnnamed, 1);
            }
            continue;
        }
        // A static field is stored without a word to the run time.
        bool isNamedByStaticField = function.constructedClass && std::ranges::any_of(program.at(function.constructedClass).staticFields, [&](PropertyKeyID key) { return keysOfNames.contains(key); }, &StaticField::key);
        if (flow.closureIsMadeOnce(id) && !hasSeveralForms.quickGet(written.index()) && !isNamedByStaticField) {
            statistics.count(StatisticID::RenamableFunctionsWithOneClosure, 1);
            continue;
        }
        editor.at(id).capabilities.add(FunctionCapabilities::RecordsCallee);
        editor.at(written).capabilities.add(FunctionCapabilities::RecordsCallee);
        marked.append(id);
        bool isRenamedInSight = !flow.closuresMayBeRenamedOnlyWhereNobodySees(id) || namedBySeveralKeys.quickGet(written.index());
        statistics.count(isRenamedInSight ? StatisticID::RenamableFunctionsRenamedInSight : StatisticID::RenamableFunctionsRenamedOutOfSight, 1);
        if (isRenamedInSight)
            recordsFromTheStart.quickSet(written.index());
    }
    // The run time sees the write that renames such a function object, and has the calls by value record their callees from then on.
    for (FunctionID id : marked) {
        FunctionID written = writtenFunctionOf(id);
        if (recordsFromTheStart.quickGet(written.index()))
            continue;
        editor.at(id).capabilities.add(FunctionCapabilities::RecordsCalleeOnceRenamed);
        editor.at(written).capabilities.add(FunctionCapabilities::RecordsCalleeOnceRenamed);
    }
    // Without RecordsCallee: the calls by value do the same, and nothing in compiled code asks.
    for (FunctionID written : writtenWithClosuresOutOfSight) {
        if (!program.at(written).has(FunctionCapabilities::RecordsCallee))
            editor.at(written).capabilities.add(FunctionCapabilities::RecordsCalleeOnceRenamed);
    }
}

void createEntrypoints(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    markFunctionsThatRecordTheirCallee(editor, inputs.flow, loops, build.statistics());
    AnalysisContext context { build, program, &inputs.flow, nullptr };
    size_t numberOfFunctionsBefore = program.functions.size();

    BitVector calledByHost = functionsCalledByHost(program, loops);
    // The functions whose closure exists without a NewClosure: a read of the variable that starts with the function creates it.
    BitVector described = findVariablesDescribedToRunTime(program, inputs.flow);
    BitVector readFromVariables(program.functions.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.function && (described.quickGet(index) || mayBeReadWithItsFunction(variable)))
            readFromVariables.quickSet(variable.function.index());
    }
    Vector<FunctionID> targets = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& entered) {
        const Function& function = program.functions[index];
        // A function that is read and never called loses its body. The read still creates the closure, whose identity is the entrypoint.
        bool isEntered = readFromVariables.quickGet(index) || (calledByHost.quickGet(index) && function.hasBody());
        if (isEntered && canBeCalledByValueOf(function.role))
            entered.append(FunctionID(index));
    });

    // The entrypoints get the ids, and their types the ids, that they would get one after the other.
    if (unsigned count = static_cast<unsigned>(targets.size())) {
        Vector<uint32_t> firstAdded;
        firstAdded.reserveInitialCapacity(count);
        unsigned numberAdded = 0;
        for (FunctionID target : targets) {
            firstAdded.append(numberAdded);
            numberAdded += entrypointVariantsOf(program, program.at(target)) == EntrypointVariants::Both ? 2 : 1;
        }
        uint32_t firstID = editor.reserveFunctions(numberAdded).index();

        InterningPhases phases(editor, count);
        forEachIndexInParallel(loops, count, [&](unsigned position) {
            TaskArenas arenas(phases, position);
            uint32_t nextID = firstID + firstAdded[position];
            auto add = [&](const GenericEntrypointRequest& variant) {
                FunctionID id(nextID++);
                editor.replaceFunctionLike(id, variant.target, createGenericEntrypoint(context, arenas.types, arenas.strings, variant));
                return id;
            };
            GenericEntrypointRequest request { .target = targets[position], .isEnteredByHost = true };
            const Function& target = program.at(request.target);
            EntrypointVariants variants = entrypointVariantsOf(program, target);
            if (variants == EntrypointVariants::Both) {
                GenericEntrypointRequest constructRequest = request;
                constructRequest.takesNewTarget = true;
                constructRequest.allocatesThis = hasAllocatingConstructEntrypoint(program, target);
                request.constructEntrypoint = add(constructRequest);
            }
            // The entrypoint that closures name.
            request.takesNewTarget = variants == EntrypointVariants::WithNewTarget;
            add(request);
        });

        BodyRemaps remaps = phases.finish();
        if (!remaps.isIdentity()) {
            forEachIndexInParallel(loops, numberAdded, [&](unsigned index) {
                applyRemaps(remaps, editor.at(FunctionID(firstID + index)));
            });
        }
    }

    uint64_t numberOfAddedFunctions = program.functions.size() - numberOfFunctionsBefore;
    build.statistics().count(StatisticID::GenericEntrypoints, numberOfAddedFunctions);
    build.statistics().countChanges(PassID::CreateEntrypoints, numberOfAddedFunctions);
}

static constexpr OptionSet<FunctionFlags> flagsOfUnboundTargets { FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount, FunctionFlags::TakesCallee };

bool canCreateBoundFunction(const Program& program, FunctionID targetID)
{
    const Function& target = program.at(targetID);
    return canBeBoundOf(target.kind) && !target.hasAny(flagsOfUnboundTargets) && mayBeCalledDirectlyOrThroughEntrypoint(program, targetID);
}

// instanceof asks the target of a bound function.
static bool answersInstanceOfWithAnyClosure(const Program& program, const FlowResults& flow, FunctionID targetID)
{
    // Each closure of a function that constructs has a prototype object of its own, which `new` and instanceof on the bound function go by.
    if (!program.at(targetID).captures.isEmpty() || program.at(targetID).constructorForm() != ConstructorForm::NotConstructor)
        return false;
    std::array<PropertyKeyID, 2> keysOfInstanceOf { program.keyOf(BuiltinNames::prototype), program.keyOf(program.builtins().nameOf(WellKnownSymbol::HasInstance)) };
    return flow.areClosuresAsCreated(targetID, keysOfInstanceOf);
}

Function buildBoundFunction(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    FunctionID targetID = key.target;
    uint32_t numberOfBoundArguments = key.parameter;
    const Function& target = program.at(targetID);
    RELEASE_ASSERT(!target.hasAny(flagsOfUnboundTargets));
    bool callsTargetByValue = !mayBeCalledDirectly(program, targetID);

    CreatorRecord::BoundFunction record {
        .target = targetID,
        .numberOfBoundArguments = numberOfBoundArguments,
        .numberOfCapturedArguments = std::min<uint32_t>(numberOfBoundArguments, target.numberOfPositionalParameters()),
        .capturesTargetClosure = callsTargetByValue || !answersInstanceOfWithAnyClosure(program, inputs.flow, targetID),
    };
    Function bound = createDerivedHeader(target, targetID, FunctionRole::BoundFunction, numberOfBoundArguments);
    bound.length = target.length > numberOfBoundArguments ? target.length - numberOfBoundArguments : 0;
    bound.flags = target.flags & OptionSet<FunctionFlags> { FunctionFlags::IsAsync, FunctionFlags::IsChecked };
    if (callsTargetByValue)
        bound.results.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
    else
        bound.results = target.results;
    bound.creator = record;

    IRBuilder builder({ inputs.build, program, &inputs.flow, nullptr }, bound, inputs.types);
    builder.setOrigin({ target.position, 0 });
    BlockID block = builder.addBlock();
    auto addEntryParameter = [&](const DeclaredType& declared) {
        TypeID proven = TypingContext { program, bound, inputs.types }.typeIDOf(declared);
        ValueID parameter = builder.addParameter(block, proven);
        if (declared.expected().predicted() != proven)
            bound.edit(parameter, ValueEdit::Type).setExpected(declared.expected());
        return parameter;
    };
    auto addCapture = [&](const DeclaredType& capture) {
        bound.captures.append(capture);
        return addEntryParameter(capture);
    };

    // A bound function that holds the closure of its target reads the captures out of it.
    Vector<ValueID, 8> targetCaptures;
    if (!record.capturesTargetClosure) {
        for (const DeclaredType& capture : target.captures)
            targetCaptures.append(addCapture(capture));
    }
    ValueID boundThis;
    if (target.has(FunctionFlags::HasThis))
        boundThis = addCapture(target.thisType);
    // `new` on the bound function is answered by the run time, which hands the bound arguments on as values: there the closure holds each as a value. Whoever binds has seen that it is of the parameter's type.
    bool isConstructedByRunTime = target.constructorForm() != ConstructorForm::NotConstructor;
    Vector<ValueID, 8> arguments;
    Vector<uint32_t, 4> argumentsHeldAsValues;
    for (uint32_t i = 0; i < record.numberOfCapturedArguments; ++i) {
        if (isConstructedByRunTime && !holdsCells(declaredRepresentationOf(program, target.parameters[i]))) {
            argumentsHeldAsValues.append(i);
            arguments.append(addCapture(DeclaredType(Proven::byPosition(TypeTable::anything))));
            continue;
        }
        arguments.append(addCapture(target.parameters[i]));
    }
    // The run time reads the target closure for instanceof.
    ValueID targetClosure;
    if (record.capturesTargetClosure)
        targetClosure = addCapture(DeclaredType(Proven::byPosition(inputs.types.intern(Type::forFunction(targetID)))));
    for (size_t i = record.numberOfCapturedArguments; i < target.parameters.size(); ++i) {
        bound.parameters.append(target.parameters[i]);
        arguments.append(addEntryParameter(target.parameters[i]));
    }

    builder.moveToEndOf(block);
    if (callsTargetByValue) {
        Value call = IRBuilder::make(Opcode::Call);
        call.setCalleeOperand(targetClosure);
        call.setThisValueOperand(boundThis ? boundThis : builder.appendUndefined());
        call.setArgumentsOperands(arguments.span());
        // The entrypoint takes the place of this frame, so what it records stands for the call of the bound function.
        Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { builder.append(WTF::move(call)) });
        terminator.setRequirement(TailCallRequirement::Required);
        builder.emitTerminator(WTF::move(terminator));
        return bound;
    }
    if (targetClosure)
        targetCaptures = appendCaptures(builder, targetID, targetClosure);
    for (uint32_t i : argumentsHeldAsValues)
        arguments[i] = builder.append(makeAssumeType(arguments[i], Proven::byPosition(TypingContext { program, bound, inputs.types }.typeIDOf(target.parameters[i]))));
    ValueID result = appendDirectCall(builder, {
        .callee = targetID,
        .captures = targetCaptures.span(),
        .thisValue = boundThis,
        .arguments = arguments.span(),
    });
    emitReturnOfEntrypointCall(builder, result);
    return bound;
}

static constexpr uint32_t typedFormBit = 1u << 31;

static uint32_t encodeCountAndForm(MethodSelector selector)
{
    return selector.numberOfArguments | (selector.form == ArgumentForm::Typed ? typedFormBit : 0);
}

static MethodSelector decodeSelector(uint32_t key, uint32_t countAndForm)
{
    return { PropertyKeyID(key), countAndForm & ~typedFormBit, countAndForm & typedFormBit ? ArgumentForm::Typed : ArgumentForm::Boxed };
}

DerivedFunctionKey tableEntrypointKey(FunctionID method, MethodSelector selector)
{
    return { .role = FunctionRole::TableEntrypoint, .target = method, .parameter = encodeCountAndForm(selector), .node = selector.key.index() };
}

DerivedFunctionKey tableLookupKey(MethodSelector selector)
{
    return { .role = FunctionRole::TableLookup, .target = FunctionID(), .parameter = encodeCountAndForm(selector), .node = selector.key.index() };
}

MethodSelector selectorOfTableFunction(const Function& function)
{
    RELEASE_ASSERT(function.role == FunctionRole::TableLookup || function.role == FunctionRole::TableEntrypoint);
    return decodeSelector(function.derivation.node, function.derivation.parameter);
}

MethodSelector selectorOf(const Program& program, const Value& tableCall)
{
    RELEASE_ASSERT(tableCall.opcode == Opcode::CallTable);
    return selectorOfTableFunction(program.at(tableCall.function()));
}

// Hands the functor each member that the class defines on its prototype under the name, until the functor answers true.
static void visitPrototypeMembersNamed(const Program& program, LayoutID declarer, PropertyKeyID key, const Invocable<bool(const ClassMember&)> auto& functor)
{
    const ClassFlow& flow = program.at(declarer).classFlow;
    for (const Vector<ClassMember>* members : { &flow.members, &flow.namedMembers }) {
        for (const ClassMember& member : *members) {
            if (member.key == key && member.side == ClassSide::Prototype && functor(member))
                return;
        }
    }
}

LayoutList receiversOfTableEntrypoint(const Program& program, PropertyKeyID key, FunctionID method)
{
    LayoutList receivers;
    std::span<const LayoutID> layouts = program.layoutsWithName(key);
    std::span<const LayoutID> declarers = program.declarersOfName(key);
    for (size_t index = 0; index < layouts.size(); ++index) {
        visitPrototypeMembersNamed(program, declarers[index], key, [&](const ClassMember& first) {
            if (first.kind == MethodKind::Method && first.function == method)
                receivers.add(layouts[index]);
            return true;
        });
    }
    return receivers;
}

Vector<TypeID, 4> parameterTypesOf(const Program& program, TypeArena& types, MethodSelector selector)
{
    Vector<TypeID, 4> result(FillWith { }, selector.numberOfArguments, TypeTable::anything);
    if (selector.form == ArgumentForm::Boxed)
        return result;

    Vector<Type, 4> declared(selector.numberOfArguments);
    LayoutID last;
    for (LayoutID declarer : program.declarersOfName(selector.key)) {
        if (declarer == std::exchange(last, declarer))
            continue;
        visitPrototypeMembersNamed(program, declarer, selector.key, [&](const ClassMember& member) {
            if (member.kind != MethodKind::Method || !member.function)
                return false;
            const Function& method = program.at(member.function);
            size_t numberOfPositionalParameters = method.numberOfPositionalParameters();
            for (uint32_t index = 0; index < selector.numberOfArguments; ++index) {
                // An argument that goes into a list, or that the method may find among `arguments`, is any value.
                bool isDeclared = index < numberOfPositionalParameters && !method.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest });
                declared[index] = declared[index].unionWith(isDeclared ? program.at(method.parameters[index].expected().predicted()) : Type::anything(), types);
            }
            return false;
        });
    }
    for (uint32_t index = 0; index < selector.numberOfArguments; ++index) {
        if (!declared[index].isNever())
            result[index] = types.intern(declared[index]);
    }
    return result;
}

// An entrypoint passes on what it was given, undefined for what it was not given. The method tests a parameter whose type it only expects. Any other has to hold whatever the selector lets through.
bool canHaveTableEntrypoint(const Program& program, TypeArena& types, FunctionID method, MethodSelector selector)
{
    if (!canCallWithoutClosure(program, method))
        return false;
    return canHaveTableEntrypoint(program, types, method, parameterTypesOf(program, types, selector).span());
}

bool canHaveTableEntrypoint(const Program& program, TypeArena& types, FunctionID method, std::span<const TypeID> given)
{
    if (!canCallWithoutClosure(program, method))
        return false;
    const Vector<DeclaredType>& parameters = program.at(method).parameters;
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index].isExpected())
            continue;
        Type passed = index < given.size() ? types.at(given[index]) : Type::fromPrimitives(Primitive::Undefined);
        if (!passed.isSubsetOf(types.at(*parameters[index].established()), program))
            return false;
    }
    return true;
}

namespace {

struct TableParameters {
    ValueID thisValue;
    Vector<ValueID, 4> arguments;
};

// Function: a call by value may find it. Set: it may find the members of the callee set. Anything: nobody can say what it finds.
enum class FoundKind : uint8_t { Function, Set, Anything };

struct FoundEntrypoints {
    FoundKind kind;
    uint32_t index;
};

} // anonymous namespace

static void declareTableConvention(const Program& program, Function& function, TypeArena& types, MethodSelector selector)
{
    function.flags.add(FunctionFlags::HasThis);
    function.thisType = DeclaredType(Proven::byPosition(types.intern(Type::object())));
    function.length = selector.numberOfArguments;
    for (TypeID parameter : parameterTypesOf(program, types, selector))
        function.parameters.append(DeclaredType(Proven::byPosition(parameter)));
    function.results.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
}

static TableParameters addEntryBlock(IRBuilder& builder)
{
    const Function& function = builder.function();
    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);

    TableParameters parameters;
    parameters.thisValue = builder.addParameter(block, *function.thisType.established());
    for (const DeclaredType& parameter : function.parameters)
        parameters.arguments.append(builder.addParameter(block, *parameter.established()));
    return parameters;
}

Function buildTableEntrypoint(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& method = program.at(key.target);
    MethodSelector selector = decodeSelector(key.node, key.parameter);
    RELEASE_ASSERT(canHaveTableEntrypoint(program, inputs.types, key.target, selector));

    Function entry = createDerivedHeader(method, key.target, FunctionRole::TableEntrypoint, key.parameter);
    declareTableConvention(program, entry, inputs.types, selector);
    entry.creator = CreatorRecord::Entrypoint { .target = key.target, .arguments = ArgumentPassing::Registers };

    IRBuilder builder({ inputs.build, program, &inputs.flow, nullptr }, entry, inputs.types);
    builder.setOrigin({ method.position, 0 });
    TableParameters parameters = addEntryBlock(builder);

    // What the method expects of `this` says nothing about who calls it. The rows that may hold the entrypoint do.
    ValueID receiver = parameters.thisValue;
    LayoutList receivers = receiversOfTableEntrypoint(program, selector.key, key.target);
    if (method.has(FunctionFlags::HasThis) && !receivers.isEmpty()) {
        TypeID instances = inputs.types.intern(Type::forLayoutSet(inputs.types.completeLayoutSet(receivers.span())));
        if (inputs.build.validates())
            builder.append(makeValidateType(receiver, instances));
        receiver = builder.append(makeAssumeType(receiver, Proven::byPosition(instances)));
    }

    ValueID result = appendDirectCall(builder, {
        .callee = key.target,
        .thisValue = method.has(FunctionFlags::HasThis) ? receiver : ValueID(),
        .arguments = parameters.arguments.span(),
    });
    emitReturnOfEntrypointCall(builder, result);
    return entry;
}

Function buildTableLookup(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    MethodSelector selector = decodeSelector(key.node, key.parameter);

    Function lookup;
    lookup.kind = FunctionKind::Plain;
    lookup.role = FunctionRole::TableLookup;
    lookup.capabilities = capabilitiesOf(FunctionRole::TableLookup);
    lookup.creator = CreatorRecord::TableLookup { .key = selector.key };
    lookup.derivation = Derivation { .source = FunctionID(), .parameter = key.parameter, .node = key.node };
    declareTableConvention(inputs.program, lookup, inputs.types, selector);

    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, lookup, inputs.types);
    TableParameters parameters = addEntryBlock(builder);

    Value read = IRBuilder::make(Opcode::PropGet, { parameters.thisValue });
    read.setKey(selector.key);
    ValueID callee = builder.append(WTF::move(read));

    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(callee);
    call.setThisValueOperand(parameters.thisValue);
    call.setArgumentsOperands(parameters.arguments.span());
    call.setCalleeKey(selector.key);
    if (const PropertyKey& key = inputs.program.at(selector.key); key.kind == PropertyKeyKind::Text)
        call.setCalleeName(key.text);
    call.setIsNeverInlined(true);
    builder.emitReturn(builder.append(WTF::move(call)));
    return lookup;
}

static ValueID calledOperandOf(const Value& call)
{
    ValueID called;
    call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (call.operandUses(operandIndex).contains(OperandUse::Calls))
            called = operand;
    });
    return called;
}

CalleesOfSites collectCalleesOfSites(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    CalleesOfSites result;

    struct Chunk {
        Vector<AccessSiteID> sites;
        Vector<CalleeSetOfSite> calleeSets;
        Vector<CalleeOfSite> typedCallees;
    };
    Vector<Vector<Chunk>> chunks = collectChunksInParallel<Chunk>(loops, program.functions.size(), { }, [&](unsigned index, Vector<Chunk>& found) {
        const Function& function = program.functions[index];
        forEachListedValue(program, FunctionID(index), Value::opcodesWithCallees, [&](BlockID, uint32_t, ValueID, const Value& call) {
            AccessSiteID site = call.accessSite();
            if (!site)
                return;
            if (found.isEmpty())
                found.append(Chunk { });
            found[0].sites.append(site);
            if (call.callees())
                found[0].calleeSets.append({ site, call.callees() });
            ValueID callee = calledOperandOf(call);
            if (!callee)
                return;
            CalleeCandidates candidates = calleeCandidates(program, function, callee);
            for (FunctionID listed : program.typeOf(function, candidates.value).functions)
                found[0].typedCallees.append({ site, listed });
            for (FunctionID listed : candidates.expected().predicted())
                found[0].typedCallees.append({ site, listed });
        });
    });
    Vector<Chunk*> filled;
    for (Vector<Chunk>& chunk : chunks) {
        if (!chunk.isEmpty())
            filled.append(&chunk[0]);
    }

    auto join = [&]<typename Entry>(Vector<Entry> Chunk::* list, Vector<Entry>& joined) {
        forEachIndexInParallel(loops, filled.size(), [&](unsigned index) {
            AOT::sort(filled[index]->*list);
            removeRepeatedElements(filled[index]->*list);
        });
        size_t size = 0;
        bool isAscending = true;
        const Entry* last = nullptr;
        for (Chunk* chunk : filled) {
            if ((chunk->*list).isEmpty())
                continue;
            isAscending &= !last || *last < (chunk->*list).first();
            last = &(chunk->*list).last();
            size += (chunk->*list).size();
        }
        joined = createVectorWithCapacityInParallel<Entry>(loops, size);
        for (Chunk* chunk : filled)
            joined.appendVector(chunk->*list);
        if (!isAscending)
            sortAndRemoveDuplicates(loops, joined);
    };
    runTasksInParallel(loops, {
        [&] {
            uint32_t numberOfSites = 0;
            for (Chunk* chunk : filled) {
                for (AccessSiteID site : chunk->sites)
                    numberOfSites = std::max(numberOfSites, site.index() + 1);
            }
            result.hadCallByValue.ensureSize(numberOfSites);
            for (Chunk* chunk : filled) {
                for (AccessSiteID site : chunk->sites)
                    result.hadCallByValue.quickSet(site.index());
            }
        },
        [&] {
            join(&Chunk::calleeSets, result.calleeSets);
        },
        [&] {
            join(&Chunk::typedCallees, result.typedCallees);
        },
    });
    build.destroyLater(WTF::move(chunks));
    result.wasTaken = true;
    return result;
}

void closeEntrypointsThatNothingEnters(ProgramEditor& editor, const FlowResults& flow, const CalleesOfSites& sites, BuildContext& build)
{
    if (!sites.wasTaken || !flow.hasResults())
        return;

    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    // The type of the callee lists functions that the call may find.
    Vector<FoundEntrypoints> found = collectInParallel<FoundEntrypoints>(loops, count, [&](unsigned index, Vector<FoundEntrypoints>& foundHere) {
        const Function& function = program.functions[index];
        forEachListedValue(program, FunctionID(index), Value::opcodesWithCallees, [&](BlockID, uint32_t, ValueID, const Value& call) {
            if (ValueID callee = calledOperandOf(call)) {
                CalleeCandidates candidates = calleeCandidates(program, function, callee);
                const Type& type = program.typeOf(function, candidates.value);
                for (FunctionID listed : type.functions)
                    foundHere.append({ FoundKind::Function, listed.index() });
                if (!admitsUnlistedFunctions(type) && type.classes.isEmpty())
                    return;
                for (FunctionID listed : candidates.expected().predicted())
                    foundHere.append({ FoundKind::Function, listed.index() });
            }
            if (CalleeSetID stamp = call.callees())
                foundHere.append({ FoundKind::Set, stamp.index() });

            AccessSiteID site = call.accessSite();
            if (!site) {
                // A pass makes a call of its own for a protocol, which flow lists among the indirect calls, and in the lookup of a method table, whose callers stand at the sites of the calls that they replace.
                bool namesItsCallee = (PayloadWord::calleeKey(call.opcode) && call.calleeKey()) || (PayloadWord::calleeName(call.opcode) && call.calleeName());
                if (function.role != FunctionRole::TableLookup && namesItsCallee)
                    foundHere.append({ FoundKind::Anything, 0 });
                return;
            }
            if (!sites.hadCallByValue.get(site.index())) {
                foundHere.append({ FoundKind::Anything, 0 });
                return;
            }
            for (const CalleeSetOfSite& entry : std::ranges::equal_range(sites.calleeSets, site, { }, &CalleeSetOfSite::site))
                foundHere.append({ FoundKind::Set, entry.callees.index() });
            for (const CalleeOfSite& entry : std::ranges::equal_range(sites.typedCallees, site, { }, &CalleeOfSite::site))
                foundHere.append({ FoundKind::Function, entry.callee.index() });
        });
    });

    // THE CONTRACT OF FLOW THAT THIS FILE RESTS ON, as computeFunctionResults (AOTValueFlowResultsFunctions.cpp) and stampCall (AOTValueFlowResultsCalls.cpp) state it.
    BitVector mayBeEntered = functionsCalledByHost(program, loops);
    for (uint32_t index : findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        bool isFollowedByFlow = index < flow.functionCountAsRead() && flow.function(FunctionID(index)).callers == CallerState::AllKnown;
        // The second entrypoint of a constructor function that reads no new.target is entered by the constructions that would enter the first.
        bool readsNewTarget = entrypointVariantsOf(program, function) != EntrypointVariants::WithoutNewTarget && !hasAllocatingConstructEntrypoint(program, function);
        return !isFollowedByFlow || function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor || readsNewTarget;
    }))
        mayBeEntered.quickSet(index);
    for (const FlowIndirectCall& call : flow.indirectCalls())
        mayBeEntered.set(call.callee.index());
    BitVector isVisitedSet(program.types.numberOfCalleeSets());
    for (FoundEntrypoints entry : found) {
        switch (entry.kind) {
        case FoundKind::Anything:
            return;
        case FoundKind::Function:
            mayBeEntered.set(entry.index);
            break;
        case FoundKind::Set:
            if (isVisitedSet.quickSet(entry.index))
                break;
            for (FunctionID function : program.at(CalleeSetID(entry.index)).followed().value())
                mayBeEntered.set(function.index());
            break;
        }
    }

    Vector<FunctionID> entrypoints = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& closed) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        if (facts.role != FunctionRole::GenericEntrypoint || !facts.hasBody || facts.mayBeCalledByHost())
            return;
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (record && function.role == FunctionRole::GenericEntrypoint && function.hasBody() && !function.mayBeCalledByHost()
            && !mayBeEntered.get(record->target.index()) && !(function.blocks.size() == 1 && function.at(entryBlock).terminator.kind != TerminatorKind::ReturnCall && function.at(entryBlock).terminator.kind != TerminatorKind::Return)
            && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            closed.append(FunctionID(index));
    });
    AnalysisContext context { build, program, &flow, nullptr };
    editFunctionsInParallel(editor, build, entrypoints.span(), [&](unsigned, FunctionID, Function& entrypoint, TaskArenas& arenas) {
        Vector<TypeID, 8> parameterTypes;
        for (ValueID parameter : entrypoint.at(entryBlock).parameters)
            parameterTypes.append(entrypoint.at(parameter).type);
        entrypoint.editAllBlocks().clear();
        entrypoint.editAllValues().clear();
        entrypoint.writtenPositions.clear();
        entrypoint.inlineFrames.clear();

        IRBuilder builder(context, entrypoint, arenas.types);
        builder.setOrigin({ entrypoint.position, 0 });
        BlockID block = builder.addBlock(BlockFrequency::Rare);
        builder.moveToEndOf(block);
        for (TypeID type : parameterTypes)
            builder.addParameter(block, type);
        Value error = IRBuilder::make(Opcode::NewRuntimeError);
        error.setRuntimeError(RuntimeErrorKind::LeftOutOfExecutable);
        builder.emitThrow(builder.append(WTF::move(error)));
    });
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, entrypoints.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
