/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedRedirection {
    uint32_t entryIndex { 0 };
    ProgramPoint point;
    uint32_t firstUse { 0 };
    uint32_t numberOfUses { 0 };
};

struct RedirectionPlan {
    std::span<const Use> usesOf(const PlannedRedirection& redirection) const LIFETIME_BOUND { return uses.span().subspan(redirection.firstUse, redirection.numberOfUses); }

    Vector<PlannedRedirection, 8> redirections;
    Vector<Use, 16> uses;
};

struct RequiredTypeCasts {
    const FlowCastInsertion& insertion;
    TypingContext typing;
    Vector<ValueID> inserted;
    const OpcodeSet& opcodesWithRequiredTypes { opcodesWithRequiredOperandTypes() };
};

} // anonymous namespace

Value makeAssumeType(ValueID operand, TypeID castType)
{
    Value value = IRBuilder::make(Opcode::AssumeType, { operand });
    value.setCastType(castType);
    return value;
}

Value makeValidateType(ValueID operand, TypeID validatedType)
{
    Value value = IRBuilder::make(Opcode::ValidateType, { operand });
    value.setValidatedType(validatedType);
    return value;
}

Value makeValidateCallee(ValueID callee, TypeID validatedType)
{
    Value value = IRBuilder::make(Opcode::ValidateCallee, { callee });
    value.setValidatedType(validatedType);
    return value;
}

static void assumeCheckedType(Value& check)
{
    RELEASE_ASSERT(check.opcode == Opcode::Check);
    ValueID operand = check.valueOperand();
    TypeID castType = check.castType();
    check.become(Opcode::AssumeType);
    check.setValueOperand(operand);
    check.setCastType(castType);
}

// An Upcast tests nothing: what goes in comes out.
static void admitWhatArrivesBehind(const FlowBodyApplication& body, std::span<const ValueID> replacedChecks)
{
    Function& function = body.function;
    BitVector handsOn(function.values.size());
    for (ValueID check : replacedChecks)
        handsOn.quickSet(check.index());
    // A value that a pass has put in has a higher number than its users.
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            Value& value = function.edit(ValueID(index));
            if (familyOf(value.opcode) != OperationFamily::Cast || handsOn.quickGet(index) || !value.numberOfOperands() || !value.operand(0) || !handsOn.quickGet(value.operand(0).index()))
                continue;
            handsOn.quickSet(index);
            hasChanged = true;
        }
    }
    Type arriving = Type::never();
    for (ValueID check : replacedChecks)
        arriving = arriving.unionWith(body.types.at(function.at(check).castType()), body.types);
    for (size_t index : handsOn) {
        Value& value = function.edit(ValueID(index));
        if (value.opcode == Opcode::Upcast)
            value.setCastType(body.types.intern(body.types.at(value.castType()).unionWith(arriving, body.types)));
    }
}

uint64_t applyCheckUpdates(const FlowBodyApplication& body, const FlowBodyUpdate& update)
{
    uint64_t numberOfRemovedChecks = 0;
    if (provesEverythingIn(body)) {
        for (Value& value : body.function.editAllValues()) {
            if (value.opcode != Opcode::Check)
                continue;
            assumeCheckedType(value);
            ++numberOfRemovedChecks;
        }
        return numberOfRemovedChecks;
    }

    Vector<ValueID, 4> replacedChecks;
    for (const FlowClaimUpdate& entry : update.claims) {
        Value& check = body.function.edit(entry.claim);
        if (entry.isRedundant) {
            assumeCheckedType(check);
            ++numberOfRemovedChecks;
            continue;
        }
        // Flow lists a check only where it has one of the two answers.
        RELEASE_ASSERT(entry.arriving);
        assumeCheckedType(check);
        check.setCastType(entry.arriving);
        replacedChecks.append(entry.claim);
        ++numberOfRemovedChecks;
    }
    if (!replacedChecks.isEmpty())
        admitWhatArrivesBehind(body, replacedChecks.span());
    return numberOfRemovedChecks;
}

// A parameter is available at the head of its block and in the handler of that block. A value inserted into the block is not.
static bool pointDominates(const FlowCastInsertion& insertion, const UseLists& useLists, ProgramPoint point, const Use& use)
{
    if (use.block != point.block) {
        if (!insertion.body.function.at(point.block).handler)
            return true;
        return insertion.analyses.dominators().terminatorDominates(point.block, use.block);
    }

    switch (use.kind) {
    case Use::Kind::Operand:
        return useLists.locationOf(use.user).index >= point.index;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return true;
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isUsedByReturnCall(const Function& function, std::span<const Use> uses)
{
    return std::ranges::any_of(uses, [&](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::ReturnCall;
    });
}

static void planRedirection(const FlowCastInsertion& insertion, const UseLists& useLists, ValueID definition, uint32_t entryIndex, RedirectionPlan& plan)
{
    const Function& function = insertion.body.function;
    std::span<const Use> uses = useLists.usesOf(definition);
    ValuePlacement placement = useLists.locationOf(definition).placement;
    if (placement == ValuePlacement::Detached || isUsedByReturnCall(function, uses))
        return;

    ProgramPoint point = useLists.pointAfter(definition);

    // Every use of a value defined in a block comes after the value, so only a parameter needs the dominators.
    uint32_t firstUse = static_cast<uint32_t>(plan.uses.size());
    for (const Use& use : uses) {
        if (acceptsCastOfValue(function, use) && (placement == ValuePlacement::InBlock || pointDominates(insertion, useLists, point, use)))
            plan.uses.append(use);
    }
    plan.redirections.append(PlannedRedirection { entryIndex, point, firstUse, static_cast<uint32_t>(plan.uses.size()) - firstUse });
}

// IRBuilder::insertBefore() invalidates the use lists, so everything read from them is copied before the first insertion.
template<typename Entry>
static RedirectionPlan planRedirections(const FlowCastInsertion& insertion, std::span<const Entry> entries, ValueID Entry::* definition)
{
    const UseLists& useLists = insertion.analyses.useLists();
    RedirectionPlan plan;
    for (uint32_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex)
        planRedirection(insertion, useLists, entries[entryIndex].*definition, entryIndex, plan);
    return plan;
}

static void redirectUses(Function& function, std::span<const Use> uses, ValueID replacement)
{
    for (const Use& use : uses)
        replaceUse(function, use, replacement);
}

void insertProofCasts(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    if (update.proofs.isEmpty())
        return;

    Function& function = insertion.body.function;
    IRBuilder& builder = insertion.builder;
    RedirectionPlan plan = planRedirections(insertion, update.proofs.span(), &FlowValueProof::value);
    for (const PlannedRedirection& planned : plan.redirections) {
        const FlowValueProof& proof = update.proofs[planned.entryIndex];
        bool endsBlock = proof.type == TypeTable::never;
        if (!planned.numberOfUses && !endsBlock)
            continue;

        builder.setOrigin(function.at(proof.value).origin());
        redirectUses(function, plan.usesOf(planned), builder.insertBefore(planned.point, makeAssumeType(proof.value, proof.type)));
    }
}

void insertSymbolConstants(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    if (update.symbolConstants.isEmpty())
        return;

    Function& function = insertion.body.function;
    RedirectionPlan plan = planRedirections(insertion, update.symbolConstants.span(), &FlowSymbolConstant::call);
    for (const PlannedRedirection& planned : plan.redirections) {
        if (!planned.numberOfUses)
            continue;

        const FlowSymbolConstant& entry = update.symbolConstants[planned.entryIndex];
        Value constant = IRBuilder::make(Opcode::KeyConstant);
        constant.setKey(entry.key);

        insertion.builder.setOrigin(function.at(entry.call).origin());
        redirectUses(function, plan.usesOf(planned), insertion.builder.insertBefore(planned.point, WTF::move(constant)));
    }
}

void createBoundClosures(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    Function& function = insertion.body.function;
    const Program& program = insertion.body.program;
    for (const FlowBoundClosureCreation& entry : update.boundClosures) {
        ValueLocation location = insertion.analyses.useLists().locationOf(entry.call);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && function.at(entry.call).opcode == Opcode::Call);

        ValueID target = function.at(entry.call).thisValueOperand();
        ValueID boundThis = function.at(entry.call).argumentsOperands().empty() ? ValueID() : function.at(entry.call).argumentsOperands()[0];
        Vector<ValueID, 2> captures;
        const auto& bound = program.at(entry.function).creator.as<CreatorRecord::BoundFunction>();
        if (program.at(bound.target).has(FunctionFlags::HasThis)) {
            insertion.builder.setOrigin(function.at(entry.call).origin());
            captures.append(boundThis ? boundThis : insertion.builder.insertBefore({ location.block, location.index }, IRBuilder::make(Opcode::Undefined)));
        }
        if (bound.capturesTargetClosure)
            captures.append(target);

        Value& creation = function.edit(entry.call);
        creation.become(Opcode::NewClosure);
        creation.setFunction(entry.function);
        creation.setCapturesOperands(captures.span());
    }
}

void replaceDefinedValues(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    Function& function = insertion.body.function;
    for (const FlowDefinedValue& entry : update.definedValues) {
        ValueLocation location = insertion.analyses.useLists().locationOf(entry.definition);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && accessOf(function.at(entry.definition).opcode) == Access::Define);

        insertion.builder.setOrigin(function.at(entry.definition).origin());
        ValueID value = insertion.builder.insertBefore({ location.block, location.index }, Value(entry.value));
        function.edit(entry.definition).setValueOperand(value);
    }
}

static bool hasRequiredType(const TypingContext& typing, ValueID operand, TypeID required)
{
    return typing.typeOf(operand).isSubsetOf(typing.types.at(required), typing.program);
}

static ValueID assumeRequiredType(RequiredTypeCasts& casts, ProgramPoint point, Origin origin, ValueID operand, TypeID required)
{
    IRBuilder& builder = casts.insertion.builder;
    builder.setOrigin(origin);
    ValueID cast = builder.insertBefore(point, makeAssumeType(operand, required));
    casts.inserted.append(cast);
    return cast;
}

// Where nobody answers for the value, it is tested.
static ValueID checkRequiredType(RequiredTypeCasts& casts, ProgramPoint point, Origin origin, ValueID operand, TypeID required)
{
    const TypingContext& typing = casts.typing;
    DeclaredType declared;
    declared.type = required;
    ConvertResolution resolution = resolveConvert(typing, typing.typeOf(operand), declared);
    RELEASE_ASSERT(resolution.cast == CastResolution::Check);

    IRBuilder& builder = casts.insertion.builder;
    builder.setOrigin(origin);
    Value check = IRBuilder::make(Opcode::Check, { operand });
    check.setCastType(resolution.castType);
    check.setCause(CheckCause::AddedByPass);
    ValueID checked = builder.insertBefore(point, WTF::move(check));
    casts.inserted.append(checked);

    Type passed = typing.typeOf(operand).castTo(typing.types.at(resolution.castType), typing.types);
    if (passed.isSubsetOf(typing.types.at(required), typing.program))
        return checked;
    return assumeRequiredType(casts, point, origin, checked, required);
}

// Whether whatever an allocation writes into the slot of the field lies inside the type that the field has now.
static bool hasProvenInitialValues(const FlowBodyApplication& body, FieldReference reference)
{
    FieldReference declaration = declarationOf(body.program, reference);
    if (fieldAt(body.program, declaration).flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
        return true;
    if (body.fieldsExpectedAsRead[declaration.layout.index()].get(declaration.field))
        return false;
    const Vector<FlowFieldUpdate>& updates = body.application.layouts[declaration.layout.index()].fields;
    return declaration.field < updates.size() && updates[declaration.field].position.isProven;
}

// What enters a position was inside the type of the position as read.
static bool mayAssumeRequiredType(const FlowBodyApplication& body, const Value& user, uint32_t operandIndex)
{
    if (provesEverythingIn(body))
        return true;

    switch (user.opcode) {
    case Opcode::NewObject:
    case Opcode::CloneAs:
        if (!isObjectLayout(body.program.at(user.layout()).kind))
            return true;
        return hasProvenInitialValues(body, { user.layout(), user.fields()[*user.variadicOperandIndex(operandIndex)] });
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
    case Opcode::PutSlots:
        return false;
    default:
        return true;
    }
}

// Where a store comes before every read of a binding, the type of its field has lost `absent`, and nothing sees what the allocation of the environment puts there.
static void dropDeadZonesThatNothingReads(const TypingContext& typing, Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject || !typing.program.at(allocation.layout()).isEnvironment())
        return;

    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    if (fields.size() != values.size())
        return;
    auto isUnreadDeadZone = [&](size_t index) {
        const Type& value = typing.typeOf(values[index]);
        if (value.isNever() || !value.hasOnlyPrimitives(Primitive::Absent))
            return false;
        return !(typing.typeOf(typing.program.at(allocation.layout()).fields[fields[index]].declared).primitives & Primitive::Absent);
    };

    Vector<uint32_t, 8> keptFields;
    Vector<ValueID, 8> keptValues;
    for (size_t index = 0; index < fields.size(); ++index) {
        if (isUnreadDeadZone(index))
            continue;
        keptFields.append(fields[index]);
        keptValues.append(values[index]);
    }
    if (keptFields.size() == fields.size())
        return;
    allocation.setFields(keptFields.span());
    allocation.setVariadicOperands(keptValues.span());
}

static void castDeclaredOperands(RequiredTypeCasts& casts, ProgramPoint point)
{
    Function& function = casts.insertion.body.function;
    ValueID user = function.at(point.block).values[point.index];
    if (!casts.opcodesWithRequiredTypes.contains(function.at(user).opcode))
        return;
    dropDeadZonesThatNothingReads(casts.typing, function.edit(user));
    for (uint32_t operandIndex = 0; operandIndex < function.at(user).numberOfOperands(); ++operandIndex) {
        const Value& value = function.at(user);
        ValueID operand = value.operand(operandIndex);
        if (!operand)
            continue;
        TypeID required = requiredTypeOfOperand(casts.typing, value, operandIndex);
        if (!required || hasRequiredType(casts.typing, operand, required))
            continue;
        bool mayAssume = mayAssumeRequiredType(casts.insertion.body, value, operandIndex);

        // An insertion may move the values, so the user is looked up again.
        Origin origin = value.origin();
        ValueID cast = mayAssume ? assumeRequiredType(casts, point, origin, operand, required) : checkRequiredType(casts, point, origin, operand, required);
        function.edit(user).setOperand(operandIndex, cast);
    }
}

static void castReturnedValue(RequiredTypeCasts& casts, BlockID block)
{
    Function& function = casts.insertion.body.function;
    Terminator& terminator = function.edit(block).terminator;
    TypeID required = requiredTypeOfOperand(casts.typing, terminator, OperandIndex::Return::value);
    if (!required || hasRequiredType(casts.typing, terminator.valueOperand(), required))
        return;
    terminator.setValueOperand(assumeRequiredType(casts, pointBeforeTerminatorOf(function, block), terminator.origin(), terminator.valueOperand(), required));
}

// The type of a position that is only expected to hold it bounds nothing.
static void assumeTailCallResult(RequiredTypeCasts& casts, BlockID block)
{
    const TypingContext& typing = casts.typing;
    TypeArena& types = typing.types;
    Terminator& terminator = casts.insertion.body.function.edit(block).terminator;

    // A call by value of one known function has the type of that function's result, which holds only as long as the result is proven: the run of GuardTaintedPositions behind flow may take that away.
    bool isCallByValue = typing.function.at(terminator.callOperand()).opcode == Opcode::Call;
    Type returned = isCallByValue ? Type::anything() : typing.typeOf(terminator.callOperand());
    Vector<TailCast, 2> tailCasts;
    for (TailCast cast : terminator.casts()) {
        returned = returned.castTo(typing.typeOf(cast.declared), types);
        tailCasts.append(cast);
    }
    TypeID required = typing.typeIDOf(typing.function.result());
    if (returned.isSubsetOf(types.at(required), typing.program))
        return;

    TailCast assumed;
    assumed.declared.type = required;
    assumed.resolution = CastResolution::Assume;
    tailCasts.append(assumed);
    terminator.setCasts(tailCasts.span());
}

static void castResult(RequiredTypeCasts& casts, BlockID block)
{
    switch (casts.typing.function.at(block).terminator.kind) {
    case TerminatorKind::Return:
        castReturnedValue(casts, block);
        return;
    case TerminatorKind::ReturnCall:
        assumeTailCallResult(casts, block);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ValueID> insertCastsForRequiredTypes(const FlowCastInsertion& insertion)
{
    const FlowBodyApplication& body = insertion.body;
    const Function& function = body.function;
    RequiredTypeCasts casts { insertion, TypingContext { body.program, function, body.types, TypeLevel::Proven }, { } };
    for (BlockID block : insertion.analyses.cfg().reversePostOrder()) {
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index)
            castDeclaredOperands(casts, ProgramPoint { block, index });
        castResult(casts, block);
    }
    return WTF::move(casts.inserted);
}

static ValueID operandThatIsCalled(const Value& call)
{
    ValueID called;
    call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandUsesOf(call.opcode, operandIndex).contains(OperandUse::Calls))
            called = operand;
    });
    return called;
}

// A stamp lists the functions that the call enters, and a closure names the function it was created from.
static std::optional<Type> typeOfListedCallees(const FlowBodyApplication& body, const Value& call)
{
    if (!PayloadWord::callees(call.opcode) || !call.callees())
        return std::nullopt;
    const CalleeSet& callees = body.types.at(call.callees());
    if (!callees.isComplete || body.results.mayCallBoundFunction(call.accessSite()))
        return std::nullopt;

    Type listed;
    for (FunctionID function : callees.functions)
        listed = listed.unionWith(Type::forFunction(function), body.types);
    for (BuiltinID builtin : callees.builtins)
        listed = listed.unionWith(Type::forBuiltin(builtin), body.types);

    // A class that is called and an arrow function that is constructed throw before they are entered, so no stamp lists them.
    bool isConstruction = !!OperandPosition::newTarget(call.opcode);
    for (FunctionID function : body.types.at(body.function.at(operandThatIsCalled(call)).type).functions) {
        const Function& callee = body.program.at(function);
        bool isClassConstructor = callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
        if (isConstruction ? callee.constructorForm() == ConstructorForm::NotConstructor : isClassConstructor)
            listed = listed.unionWith(Type::forFunction(function), body.types);
    }
    return listed;
}

// A complete stamp lists the layout of every object that reaches the access. It says nothing about a receiver that is no object.
static std::optional<Type> typeOfListedReceivers(const FlowBodyApplication& body, const Value& access)
{
    if (!Value::opcodesWithReceivers.contains(access.opcode) || !receiverOf(access.opcode) || !access.receivers())
        return std::nullopt;
    const LayoutSet& receivers = body.types.at(access.receivers());
    if (!receivers.isComplete || receivers.hasMembersWithoutLayout)
        return std::nullopt;
    Type listed = receivers.members.isEmpty() ? Type() : Type::forLayoutSet(access.receivers());
    return listed.withPrimitives(Primitive::All);
}

static VariableID firstTimeMarkOf(const FlowBodyApplication& body, const Value& value)
{
    if (value.opcode == Opcode::PutModuleVariable && value.variable().index() < body.firstTimeMarks->storesByVariable.size())
        return body.firstTimeMarks->storesByVariable[value.variable().index()];
    if (value.opcode == Opcode::NewClass)
        return body.firstTimeMarks->evaluationsByLayout[value.layout().index()];
    return { };
}

static void insertFirstRunTest(const FlowCastInsertion& insertion)
{
    Value test = IRBuilder::make(Opcode::ValidateFirstRun);
    test.setVariable(insertion.body.firstRunMark);
    insertion.builder.setOrigin({ });
    insertion.builder.insertBefore({ insertion.analyses.cfg().root(), 0 }, WTF::move(test));
}

// A pass may use a proof up: it folds a test, resolves a callee or removes a read, and the cast or the stamp that stated the proof goes.
void insertRuntimeValidation(const FlowCastInsertion& insertion)
{
    const FlowBodyApplication& body = insertion.body;
    const Function& function = body.function;
    IRBuilder& builder = insertion.builder;
    if (body.firstRunMark)
        insertFirstRunTest(insertion);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            const Value& value = function.at(function.at(block).values[index]);
            builder.setOrigin(value.origin());
            if (value.opcode == Opcode::AssumeType) {
                builder.insertBefore({ block, index }, makeValidateType(value.valueOperand(), value.castType()));
                continue;
            }
            if (VariableID mark = firstTimeMarkOf(body, value)) {
                Value test = IRBuilder::make(Opcode::ValidateFirstRun);
                test.setVariable(mark);
                builder.insertBefore({ block, index }, WTF::move(test));
                continue;
            }
            if (std::optional<Type> listed = typeOfListedReceivers(body, value)) {
                builder.insertBefore({ block, index }, makeValidateType(value.operand(*receiverOf(value.opcode)), body.types.intern(*listed)));
                continue;
            }
            if (std::optional<Type> listed = typeOfListedCallees(body, value))
                builder.insertBefore({ block, index }, makeValidateCallee(operandThatIsCalled(value), body.types.intern(*listed)));
        }
    }
    builder.applyInsertions();
}

// A family may hold objects without a layout. Until they are ruled out, no access through it finds a slot without a test.
static bool isNarrowerInLayouts(const Program& program, const Type& current, const Type& narrowed)
{
    if (!narrowed.layouts)
        return false;
    if (!current.layouts || membersOf(program, current).size() > membersOf(program, narrowed).size())
        return true;
    return hasMembersWithoutLayout(program, current) && !hasMembersWithoutLayout(program, narrowed);
}

static bool hasUnknownCallees(const Program& program, const Type& type)
{
    return type.anyFunction || type.anyObject || hasMembersWithoutLayout(program, type);
}

static bool isNarrowerInCallees(const Program& program, const Type& current, const Type& narrowed)
{
    if (!narrowed.mayBeFunction() || hasUnknownCallees(program, narrowed))
        return false;
    return hasUnknownCallees(program, current) || current.functions.size() > narrowed.functions.size() || current.builtins.size() > narrowed.builtins.size();
}

static bool isNarrowerInClasses(const Type& current, const Type& narrowed)
{
    return std::ranges::any_of(narrowed.classes, [&](const ClassPart& part) {
        const ClassPart* currentPart = current.findClassPart(part.builtinClass);
        if (!currentPart)
            return true;
        return (part.contentsAreProven && !currentPart->contentsAreProven) || !currentPart->provenPristine.containsAll(part.provenPristine);
    });
}

bool isWorthNarrowing(const Program& program, const Type& current, const Type& narrowed)
{
    return (narrowed.isNever() && !current.isNever())
        || defaultSlotRepresentationOf(current) != defaultSlotRepresentationOf(narrowed)
        || isNarrowerInLayouts(program, current, narrowed)
        || isNarrowerInCallees(program, current, narrowed)
        || isNarrowerInClasses(current, narrowed);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
