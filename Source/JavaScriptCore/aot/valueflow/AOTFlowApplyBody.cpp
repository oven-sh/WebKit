/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTAnalysisContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPredictedValues.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowCastInsertion {
    const FlowBodyApplication& body;
    FunctionAnalyses& analyses;
    IRBuilder& builder;
    // Of the body as it was when the checks that flow has answered were gone.
    const PredictedValues& predicted;

    // The type was cut out of the type of the value as read, which may be owed to any check in front of it.
    ProvenType provenOfValue(ValueID value, TypeID type) const { return Proven::byFlow(type, predicted.isBehindCheck(value) ? ProofScope::BehindChecks : ProofScope::EveryVersion); }
    // What flow says of a position leaves out the primitives that the checks in front of its stores turn away. Where the value was inside the type before it came to its checks, it is so whether they stand or go.
    ProvenType provenOfPosition(ValueID value, TypeID type) const
    {
        if (!predicted.isBehindCheck(value))
            return Proven::byFlow(type, ProofScope::EveryVersion);
        const Function& function = body.function;
        ValueID unchecked = value;
        while (predicted.isBehindCheck(unchecked) && isCast(function.at(unchecked).opcode))
            unchecked = function.at(unchecked).operand(0);
        bool holdsWithoutChecks = !predicted.isBehindCheck(unchecked) && body.types.at(function.at(unchecked).type).isSubsetOf(body.types.at(type), body.program);
        return Proven::byFlow(type, holdsWithoutChecks ? ProofScope::EveryVersion : ProofScope::BehindChecks);
    }
};

struct ListedMember {
    ValueID literal;
    PropertyKeyID key;
    ValueID value;
};

} // anonymous namespace

Value makeAssumeType(ValueID operand, ProvenType proven)
{
    Value value;
    value.becomeAssumeType(operand, proven);
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

// Which checks stand in front of it is known when every answered check is gone: applyScopesOfAnsweredChecks().
static void assumeInPlaceOfChecking(Value& check, TypeID held)
{
    RELEASE_ASSERT(check.opcode == Opcode::Check);
    check.becomeAssumeType(check.valueOperand(), Proven::byFlow(held, ProofScope::EveryVersion));
}

static uint64_t applyCheckUpdates(const FlowBodyApplication& body, const FlowBodyUpdate& update, Vector<ValueID, 8>& answeredChecks)
{
    uint64_t numberOfRemovedChecks = 0;
    if (body.provesEverything) {
        for (Value& value : body.function.editAllValues()) {
            if (value.opcode != Opcode::Check)
                continue;
            value.becomeAssumeType(value.valueOperand(), Proven::byFlow(value.expectedType().predicted(), ProofScope::EveryVersion));
            ++numberOfRemovedChecks;
        }
        return numberOfRemovedChecks;
    }

    Vector<ValueID, 4> refutedChecks;
    for (const FlowClaimUpdate& entry : update.claims) {
        Value& check = body.function.edit(entry.claim);
        if (entry.isRedundant) {
            assumeInPlaceOfChecking(check, check.expectedType().predicted());
            answeredChecks.append(entry.claim);
            ++numberOfRemovedChecks;
            continue;
        }
        if (entry.mayFail) {
            check.setCause(CheckCause::MayFail);
            continue;
        }
        // Nothing that arrives passes. What does arrive goes on, and the type that was declared says nothing of what is made from it.
        RELEASE_ASSERT(entry.arriving);
        assumeInPlaceOfChecking(check, entry.arriving);
        answeredChecks.append(entry.claim);
        refutedChecks.append(entry.claim);
        ++numberOfRemovedChecks;
    }
    if (!refutedChecks.isEmpty())
        forgetProofsBehind(body.function, CFG(body.function), body.types, refutedChecks.span(), { }, ExpectedTypes::Dropped);
    return numberOfRemovedChecks;
}

static void applyScopesOfAnsweredChecks(Function& function, const PredictedValues& predicted, std::span<const ValueID> answeredChecks)
{
    for (ValueID claim : answeredChecks) {
        if (predicted.isBehindCheck(function.at(claim).valueOperand()))
            function.edit(claim, ValueEdit::Definition).setIsBehindChecks(true);
    }
}

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

template<typename Entry>
static void insertBehindDefinitions(const FlowCastInsertion& insertion, const Vector<Entry>& entries, ValueID Entry::* definitionOf, NOESCAPE const Invocable<bool(const Entry&)> auto& isNeededWithoutUses, NOESCAPE const Invocable<Value(const Entry&)> auto& makeValue)
{
    struct Redirection {
        const Entry* entry;
        ProgramPoint point;
        uint32_t firstUse;
        uint32_t numberOfUses;
    };
    if (entries.isEmpty())
        return;

    Function& function = insertion.body.function;
    const UseLists& useLists = insertion.analyses.useLists();
    Vector<Redirection, 8> redirections;
    Vector<Use, 16> redirectedUses;
    for (const Entry& entry : entries) {
        ValueID definition = entry.*definitionOf;
        std::span<const Use> uses = useLists.usesOf(definition);
        ValuePlacement placement = useLists.locationOf(definition).placement;
        if (placement == ValuePlacement::Detached || isUsedByReturnCall(function, uses))
            continue;

        ProgramPoint point = useLists.pointAfter(definition);
        uint32_t firstUse = static_cast<uint32_t>(redirectedUses.size());
        for (const Use& use : uses) {
            if (acceptsCastOfValue(function, use) && (placement == ValuePlacement::InBlock || pointDominates(insertion, useLists, point, use)))
                redirectedUses.append(use);
        }
        redirections.append(Redirection { &entry, point, firstUse, static_cast<uint32_t>(redirectedUses.size()) - firstUse });
    }
    for (const Redirection& redirection : redirections) {
        if (!redirection.numberOfUses && !isNeededWithoutUses(*redirection.entry))
            continue;
        insertion.builder.setOrigin(function.at(redirection.entry->*definitionOf).origin());
        ValueID inserted = insertion.builder.insertBefore(redirection.point, makeValue(*redirection.entry));
        for (const Use& use : redirectedUses.subspan(redirection.firstUse, redirection.numberOfUses))
            replaceUse(function, use, inserted);
    }
}

static void replaceDefinedValues(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
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

static bool mayAssumeRequiredType(const FlowBodyApplication& body, const Value& user, uint32_t operandIndex)
{
    if (body.provesEverything)
        return true;

    switch (holderOfRequiredTypes(body.program, user)) {
    case RequiredTypeHolder::StoredSlot:
        return false;
    case RequiredTypeHolder::InitializedSlot:
        return hasProvenInitialValues(body, { user.layout(), user.fields()[*user.variadicOperandIndex(operandIndex)] });
    case RequiredTypeHolder::PositionThatFlowFollows:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void stopListingFields(Value& allocation, NOESCAPE const Invocable<bool(size_t)> auto& isDropped)
{
    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    Vector<uint32_t, 16> keptFields;
    Vector<ValueID, 16> keptValues;
    for (size_t index = 0; index < fields.size(); ++index) {
        if (isDropped(index))
            continue;
        keptFields.append(fields[index]);
        keptValues.append(values[index]);
    }
    if (keptFields.size() == fields.size())
        return;
    allocation.setFields(keptFields.span());
    allocation.setVariadicOperands(keptValues.span());
}

static void dropDeadZonesThatNothingReads(const TypingContext& typing, Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject || !typing.program.at(allocation.layout()).isEnvironment())
        return;

    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    if (fields.size() != values.size())
        return;
    stopListingFields(allocation, [&](size_t index) {
        const Type& value = typing.typeOf(values[index]);
        if (value.isNever() || !value.hasOnlyPrimitives(Primitive::Absent))
            return false;
        return !(typing.typeOf(typing.program.at(allocation.layout()).fields[fields[index]].declared).primitives & Primitive::Absent);
    });
}

static Vector<ValueID> insertCastsForRequiredTypes(const FlowCastInsertion& insertion)
{
    const FlowBodyApplication& body = insertion.body;
    Function& function = body.function;
    TypeArena& types = body.types;
    TypingContext typing { body.program, function, types, TypeLevel::Proven };
    const OpcodeSet& opcodesWithRequiredTypes = opcodesWithRequiredOperandTypes();
    Vector<ValueID> inserted;

    auto hasRequiredType = [&](ValueID operand, TypeID required) {
        return typing.typeOf(operand).isSubsetOf(types.at(required), body.program);
    };
    auto insert = [&](ProgramPoint point, Origin origin, Value&& value) {
        insertion.builder.setOrigin(origin);
        inserted.append(insertion.builder.insertBefore(point, WTF::move(value)));
        return inserted.last();
    };
    auto checkRequiredType = [&](ProgramPoint point, Origin origin, ValueID operand, TypeID required) {
        DeclaredType declared(Proven::byPosition(required));
        ConvertResolution resolution = resolveConvert(typing, typing.typeOf(operand), declared);
        RELEASE_ASSERT(resolution.cast == CastResolution::Check);

        Value check = IRBuilder::make(Opcode::Check, { operand });
        check.setExpectedType(ExpectedType(resolution.castType));
        check.setCause(CheckCause::AddedByPass);
        ValueID checked = insert(point, origin, WTF::move(check));

        Type passed = typing.typeOf(operand).castTo(types.at(resolution.castType), types);
        if (passed.isSubsetOf(types.at(required), body.program))
            return checked;
        return insert(point, origin, makeAssumeType(checked, Proven::byFlow(required, ProofScope::BehindChecks)));
    };
    auto castDeclaredOperands = [&](ProgramPoint point) {
        ValueID user = function.at(point.block).values[point.index];
        if (!opcodesWithRequiredTypes.contains(function.at(user).opcode))
            return;
        dropDeadZonesThatNothingReads(typing, function.edit(user));
        for (uint32_t operandIndex = 0; operandIndex < function.at(user).numberOfOperands(); ++operandIndex) {
            const Value& value = function.at(user);
            ValueID operand = value.operand(operandIndex);
            if (!operand)
                continue;
            TypeID required = requiredTypeOfOperand(typing, value, operandIndex);
            if (!required || hasRequiredType(operand, required))
                continue;
            bool mayAssume = mayAssumeRequiredType(body, value, operandIndex);

            Origin origin = value.origin();
            ValueID cast = mayAssume ? insert(point, origin, makeAssumeType(operand, insertion.provenOfPosition(operand, required))) : checkRequiredType(point, origin, operand, required);
            function.edit(user).setOperand(operandIndex, cast);
        }
    };
    auto castReturnedValue = [&](BlockID block) {
        Terminator& terminator = function.edit(block).terminator;
        TypeID required = requiredTypeOfOperand(typing, terminator, OperandIndex::Return::value);
        if (!required || hasRequiredType(terminator.valueOperand(), required))
            return;
        terminator.setValueOperand(insert(pointBeforeTerminatorOf(function, block), terminator.origin(), makeAssumeType(terminator.valueOperand(), insertion.provenOfPosition(terminator.valueOperand(), required))));
    };
    auto assumeTailCallResult = [&](BlockID block) {
        Terminator& terminator = function.edit(block).terminator;

        bool isCallByValue = function.at(terminator.callOperand()).opcode == Opcode::Call;
        Type returned = isCallByValue ? Type::anything() : typing.typeOf(terminator.callOperand());
        Vector<TailCast, 2> tailCasts;
        for (TailCast cast : terminator.casts()) {
            returned = returned.castTo(typing.typeOf(cast.declared), types);
            tailCasts.append(cast);
        }
        TypeID required = typing.typeIDOf(function.result());
        if (returned.isSubsetOf(types.at(required), body.program))
            return;

        TailCast assumed;
        assumed.declared.establish(Proven::byPosition(required));
        assumed.resolution = CastResolution::Assume;
        tailCasts.append(assumed);
        terminator.setCasts(tailCasts.span());
    };

    for (BlockID block : insertion.analyses.cfg().reversePostOrder()) {
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index)
            castDeclaredOperands({ block, index });
        switch (function.at(block).terminator.kind) {
        case TerminatorKind::Return:
            castReturnedValue(block);
            continue;
        case TerminatorKind::ReturnCall:
            assumeTailCallResult(block);
            continue;
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
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return inserted;
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

    bool isConstruction = !!OperandPosition::newTarget(call.opcode);
    for (FunctionID function : body.types.at(body.function.at(operandThatIsCalled(call)).type).functions) {
        const Function& callee = body.program.at(function);
        bool isClassConstructor = callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
        if (isConstruction ? callee.constructorForm() == ConstructorForm::NotConstructor : isClassConstructor)
            listed = listed.unionWith(Type::forFunction(function), body.types);
    }
    return listed;
}

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

static void insertRuntimeValidation(const FlowCastInsertion& insertion)
{
    const FlowBodyApplication& body = insertion.body;
    const Function& function = body.function;
    IRBuilder& builder = insertion.builder;
    auto insertFirstTimeTest = [&](ProgramPoint point, VariableID mark) {
        Value test = IRBuilder::make(Opcode::ValidateFirstRun);
        test.setVariable(mark);
        builder.insertBefore(point, WTF::move(test));
    };
    if (body.firstRunMark) {
        builder.setOrigin({ });
        insertFirstTimeTest({ insertion.analyses.cfg().root(), 0 }, body.firstRunMark);
    }
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            const Value& value = function.at(function.at(block).values[index]);
            builder.setOrigin(value.origin());
            if (value.opcode == Opcode::AssumeType) {
                builder.insertBefore({ block, index }, makeValidateType(value.valueOperand(), value.provenType()));
                continue;
            }
            if (VariableID mark = firstTimeMarkOf(body, value)) {
                insertFirstTimeTest({ block, index }, mark);
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

static void passOnInPlaceOfFinishing(Function& function, TypeID type, NOESCAPE const Invocable<bool(ValueID)> auto& isNoLongerFinished)
{
    for (Value& finished : function.editAllValues()) {
        if (finished.opcode != Opcode::FinishObject || !isNoLongerFinished(finished.objectOperand()))
            continue;
        Value same = IRBuilder::make(Opcode::Upcast, { finished.objectOperand() });
        same.setCastType(type);
        same.setOrigin(finished.origin());
        same.type = type;
        finished = WTF::move(same);
    }
}

static void restoreCallsOfObjectCreate(const FlowBodyApplication& body)
{
    Function& function = body.function;
    Vector<ValueID, 4> calls;
    for (Value& link : function.editAllValues()) {
        if (link.opcode != Opcode::SetLiteralPrototype || !link.refusesOtherValues())
            continue;
        ValueID callee = link.replacedCalleeOperand();
        ValueID thisValue = link.replacedThisOperand();
        link.setReplacedCalleeOperand({ });
        link.setReplacedThisOperand({ });
        Value& allocation = function.edit(link.objectOperand());
        if (!callee || allocation.opcode != Opcode::NewObject || !body.application.layouts[allocation.layout().index()].objectsAreDictionaries)
            continue;

        Value call = IRBuilder::make(Opcode::Call);
        call.setCalleeOperand(callee);
        call.setThisValueOperand(thisValue);
        ValueID prototype = link.prototypeOperand();
        call.setArgumentsOperands(singleElementSpan(prototype));
        call.setOrigin(allocation.origin());
        call.type = TypeTable::anything;
        allocation = WTF::move(call);
        calls.append(link.objectOperand());

        Value nothing = IRBuilder::make(Opcode::Undefined);
        nothing.setOrigin(link.origin());
        nothing.type = TypeTable::undefined;
        link = WTF::move(nothing);
    }
    if (!calls.isEmpty())
        passOnInPlaceOfFinishing(function, TypeTable::anything, [&](ValueID object) { return calls.contains(object); });
}

static void stopListingStartsThatNobodyReads(const FlowBodyApplication& body, Value& allocation)
{
    const Vector<FlowFieldUpdate>& updates = body.application.layouts[allocation.layout().index()].fields;
    auto isDropped = [&](uint32_t field) {
        return field < updates.size() && updates[field].undefinedAtStartIsNeverRead;
    };
    std::span<const uint32_t> fields = allocation.fields();
    if (std::ranges::any_of(fields, isDropped))
        stopListingFields(allocation, [&](size_t index) { return isDropped(fields[index]); });
}

static void applyStamps(const FlowBodyApplication& body, const FlowBodyUpdate& update)
{
    Function& function = body.function;
    const FlowAccessSiteNumbering& accessSites = body.application.accessSites;

    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        RELEASE_ASSERT(ordinal < accessSites.numberOfSitesOf(body.id));
        function.edit(value).setAccessSite(accessSites.siteOf(body.id, ordinal));
    });

    for (const FlowAccessStamp& stamp : update.accesses)
        function.edit(stamp.value).setReceivers(stamp.receivers);

    for (ValueID open : update.opensOfUndecidedStorage)
        function.edit(open).setStorageIsUndecided(true);

    for (const FlowCallStamp& stamp : update.calls)
        function.edit(stamp.value).setCallees(stamp.callees);

    for (const FlowAllocationStamp& stamp : update.allocations) {
        Value& allocation = function.edit(stamp.value);
        allocation.setAllocationSite(stamp.site);
        if (PayloadWord::allocationClass(allocation.opcode))
            allocation.setAllocationClass(stamp.allocationClass);
        if (stamp.elements)
            allocation.setContents(stamp.elements);
        if (allocation.opcode == Opcode::NewArray)
            allocation.setArrayStorage(stamp.arrayStorage);
        if (allocation.opcode == Opcode::NewObject && body.program.at(allocation.layout()).isEnvironment())
            stopListingStartsThatNobodyReads(body, allocation);
    }

    restoreCallsOfObjectCreate(body);
}

static Vector<ListedMember> makeOrdinaryObjects(const FlowBodyApplication& body)
{
    Function& function = body.function;
    Vector<ListedMember> listedMembers;
    if (!body.application.hasDictionaries)
        return listedMembers;
    DictionaryTypeRewriter rewriter { body.application };
    for (Value& value : function.editAllValues()) {
        TypeID passed = value.opcode == Opcode::Check ? rewriter.typeTestedBehind(value.type) : TypeID();
        TypeID tested = value.opcode == Opcode::Check ? rewriter.typeTestedBehind(value.expectedType().predicted()) : value.opcode == Opcode::TypeTest ? rewriter.typeTestedBehind(value.testedType()) : TypeID();
        rewriter(value.type);
        if (passed)
            value.type = passed;
        TypeID expected = rewriter.typeBehind(value.expected().predicted());
        value.setExpected(ExpectedType(expected == value.type ? TypeID() : expected));
        forEachReference(value, rewriter);
        if (value.opcode == Opcode::Check)
            value.setExpectedType(ExpectedType(tested));
        if (value.opcode == Opcode::TypeTest)
            value.setTestedType(tested);
    }
    for (Block& block : function.editAllBlocks())
        forEachReference(block.terminator, rewriter);
    BitVector dictionaries;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        Value& value = function.edit(ValueID(index));
        LayoutID layout = layoutOfPlainLiteral(body.program, value);
        if (!layout || !body.application.layouts[layout.index()].objectsAreDictionaries)
            continue;
        if (value.opcode == Opcode::NewObject) {
            dictionaries.set(index);
            std::span<const uint32_t> fields = value.fields();
            for (size_t position = 0; position < fields.size(); ++position)
                listedMembers.append({ ValueID(index), body.program.at(layout).fields[fields[position]].key, value.variadicOperands()[position] });
        }
        Value dictionary = IRBuilder::make(Opcode::NewDictionary);
        dictionary.setOrigin(value.origin());
        dictionary.type = TypeTable::object;
        value = WTF::move(dictionary);
    }
    if (!dictionaries.isEmpty())
        passOnInPlaceOfFinishing(function, TypeTable::object, [&](ValueID object) { return dictionaries.get(object.index()); });
    return listedMembers;
}

static void defineListedMembers(const Function& function, IRBuilder& builder, std::span<const ListedMember> ascendingByLiteral)
{
    if (ascendingByLiteral.empty())
        return;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            for (const ListedMember& member : std::ranges::equal_range(ascendingByLiteral, values[index], { }, &ListedMember::literal)) {
                Value definition = IRBuilder::make(Opcode::PropDefine, { member.literal, member.value });
                definition.setKey(member.key);
                builder.setOrigin(function.at(member.literal).origin());
                builder.insertBefore({ BlockID(blockIndex), index + 1 }, WTF::move(definition));
            }
        }
    }
}

static void demoteTailCall(const FlowBodyApplication& body, IRBuilder& builder, BlockID block)
{
    builder.demoteReturnCall(block);
    body.build.statistics().count(StatisticID::ReturnCallsDemoted);
}

static bool isCheck(const TailCast& cast)
{
    return cast.resolution == CastResolution::Check;
}

// The result of a tail call goes to the caller and nowhere else, so nothing in this function relies on a test of it: the type is expected, and whoever receives the value tests it.
static bool tryExpectInPlaceOfChecking(const Program& program, Function& function, TypeArena& types, BlockID block, bool resultIsProvenByFlow)
{
    Terminator& terminator = function.edit(block).terminator;
    TypingContext typing { program, function, types };
    Type returned = function.at(terminator.callOperand()).opcode == Opcode::Call ? Type::anything() : typing.typeOf(terminator.callOperand());
    Vector<TailCast, 2> casts;
    for (TailCast cast : terminator.casts()) {
        // A cast that does nothing may rest on a test in front of it.
        bool restsOnTest = cast.resolution == CastResolution::Nothing && !returned.isSubsetOf(typing.typeOf(cast.declared), program);
        if (isCheck(cast) || restsOnTest) {
            cast.resolution = CastResolution::Nothing;
            cast.declared.expect(cast.declared.expected());
        }
        returned = returned.castTo(typing.typeOf(cast.declared), types);
        casts.append(cast);
    }
    if (!resultIsProvenByFlow && !returned.isSubsetOf(typing.typeOf(function.result()), program))
        return false;
    terminator.setCasts(casts.span());
    return true;
}

static bool applyTailChecks(const FlowBodyApplication& body, IRBuilder& builder, BlockID block, const FlowTailCallUpdate& entry, FlowBodyCounts& counts)
{
    Terminator& terminator = body.function.edit(block).terminator;
    Vector<TailCast, 2> casts;
    for (TailCast cast : terminator.casts())
        casts.append(cast);
    uint64_t numberOfChecks = static_cast<uint64_t>(std::ranges::count_if(casts, isCheck));
    if (!numberOfChecks)
        return true;

    if (!entry.claimsAreRedundant && !body.provesEverything) {
        const Vector<FlowPositionUpdate, 1>& results = body.application.signatures[body.id.index()].results;
        if (tryExpectInPlaceOfChecking(body.program, body.function, body.types, block, results.size() == 1 && results[0].isProven))
            return true;
        demoteTailCall(body, builder, block);
        return false;
    }

    for (TailCast& cast : casts) {
        if (isCheck(cast))
            cast.resolution = CastResolution::Assume;
    }
    terminator.setCasts(casts.span());
    counts.removedChecks += numberOfChecks;
    return true;
}

static void applyTailCalls(const FlowBodyApplication& body, IRBuilder& builder, const FlowBodyUpdate& update, FlowBodyCounts& counts)
{
    Function& function = body.function;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = function.edit(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID call = terminator.callOperand();
        // A function that is constructed stays on the stack until it has seen what its last call returns.
        if (body.isOnlyConstructed) {
            demoteTailCall(body, builder, block);
            function.edit(call, ValueEdit::Definition).isTailCall = false;
            continue;
        }
        // What a built-in function calls returns to that function, and the frame of the caller is the one that stands for it.
        if (function.at(call).opcode == Opcode::Call && function.at(call).calledBuiltin()) {
            demoteTailCall(body, builder, block);
            continue;
        }
        const FlowTailCallUpdate* known = findByValue(update.tailCalls.span(), call, &FlowTailCallUpdate::call);
        FlowTailCallUpdate entry = known ? *known : FlowTailCallUpdate { };
        terminator.setRequirement(TailCallRequirement::Required);
        if (!applyTailChecks(body, builder, block, entry, counts))
            continue;

        if (findByValue(update.symbolConstants.span(), call, &FlowSymbolConstant::call))
            demoteTailCall(body, builder, block);
    }
}

static bool isTailCallWithCheck(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return false;
    for (TailCast cast : terminator.casts()) {
        if (isCheck(cast))
            return true;
    }
    return false;
}

void demoteTailCallsWithChecks(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    bool hasTailCallWithCheck = std::ranges::any_of(function.blocks, isTailCallWithCheck, &Block::terminator);
    if (!hasTailCallWithCheck)
        return;

    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        if (!isTailCallWithCheck(function.at(block).terminator) || tryExpectInPlaceOfChecking(program, function, types, block, false))
            continue;
        builder.demoteReturnCall(block);
        build.statistics().count(StatisticID::ReturnCallsDemoted);
    }
}

static uint64_t countChecks(const Function& function)
{
    uint64_t numberOfChecks = 0;
    for (const Block& block : function.blocks) {
        numberOfChecks += static_cast<uint64_t>(std::ranges::count_if(block.values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::Check;
        }));
    }
    return numberOfChecks;
}

// The front end writes the read of a locked member as a constant.
static void initializeMembersOfListedHolders(const FlowCastInsertion& insertion)
{
    const Function& function = insertion.body.function;
    const BuiltinTable& builtins = insertion.body.program.builtins();
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& constant = function.values[index];
        if (constant.opcode != Opcode::Builtin || !firstReadChangesListedKeys(builtins, insertion.body.results, constant.builtin()))
            continue;
        const UseLists& useLists = insertion.analyses.useLists();
        if (useLists.locationOf(ValueID(index)).placement != ValuePlacement::InBlock)
            continue;
        insertion.builder.setOrigin(constant.origin());
        insertion.builder.insertBefore(useLists.pointAfter(ValueID(index)), *tryMakeInitializeBuiltin(builtins, insertion.body.results, constant));
    }
}

// It writes one also where nothing takes the value.
static void eraseBuiltinConstantsWithoutUse(const FlowCastInsertion& insertion)
{
    const Function& function = insertion.body.function;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        BitVector indexes;
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            if (function.at(block.values[index]).opcode == Opcode::Builtin && insertion.analyses.useLists().usesOf(block.values[index]).empty())
                indexes.set(index);
        }
        if (!indexes.isEmpty())
            insertion.builder.eraseValues(BlockID(blockIndex), indexes);
    }
}

bool takesOnlyWhatFlowHolds(const Program& program, const Value& store, uint32_t operandIndex)
{
    switch (holderOfRequiredTypes(program, store)) {
    case RequiredTypeHolder::StoredSlot:
        return false;
    case RequiredTypeHolder::InitializedSlot: {
        std::optional<uint32_t> valueIndex = store.variadicOperandIndex(operandIndex);
        if (!valueIndex)
            return false;
        FieldReference field = declarationOf(program, { store.layout(), store.fields()[*valueIndex] });
        return fieldAt(program, field).flow.stability.contains(FieldStability::IsNeverStoredWithOtherType) || !canStoreInitialValueBehindAllocation(program, store, *valueIndex);
    }
    case RequiredTypeHolder::PositionThatFlowFollows:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct StoreBehindChecks {
    ProgramPoint point;
    // Null: the terminator of the block.
    ValueID user;
    uint32_t operandIndex;
    TypeID claimed;
};

} // anonymous namespace

enum class ValidatesClaims : bool { No, Yes };

static void assumeTypesOfPositionsAtStores(const Program& program, Function& function, TypeArena& types, FunctionAnalyses& analyses, IRBuilder& builder, ValidatesClaims validatesClaims)
{
    PredictedValues predicted(function, analyses.cfg());
    if (!predicted.hasChecks())
        return;
    // What is proven of a position that takes the very value that a check tests may rest on that check.
    Vector<ValueID, 16> checks;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::Check && mayTurnValueAway(program, function, function.at(id)))
                checks.append(id);
        }
    }
    BitVector isCheckedValueItself = valuesThatAre(function, analyses.cfg(), checks.span());
    TypingContext typing { program, function, types };
    Vector<StoreBehindChecks, 8> found;

    auto isTypedOnCredit = [&](ValueID value, TypeID claimed) {
        if (!value || !predicted.isBehindCheck(value))
            return false;
        const Value& definition = function.at(value);
        if (!types.at(definition.type).isJavaScriptValue() || !types.at(claimed).isJavaScriptValue())
            return false;
        // An allocation and a direct call are typed by their own words, whatever their operands are.
        const Value& producer = definition.opcode == Opcode::ResultAt ? function.at(definition.resultsOperand()) : definition;
        if (resultRuleOf(definition.opcode) == ResultRule::Allocation || definition.opcode == Opcode::NewClosure || producer.opcode == Opcode::CallDirect)
            return false;
        return definition.opcode != Opcode::AssumeType || definition.isBehindChecks() || !types.at(definition.provenType()).isSubsetOf(types.at(claimed), program);
    };
    // A literal is born with the storage that fits the types of its elements: one whose element is not known to be a number gets the storage that holds everything.
    // Its contents are what flow holds, cut by no type, so they hold of the very value that a check tests too.
    BitVector literalsSeen;
    auto visitElementsOfLiteral = [&](ValueID array, TypeID claimed) {
        const ClassPart* part = types.at(claimed).singleBuiltinClass();
        if (!part || !part->contentsAreProven || (part->arrayStorage != ArrayStorage::Int32 && part->arrayStorage != ArrayStorage::Double))
            return;
        ValueID literal = sameObjectThroughCasts(function, array);
        if (function.at(literal).opcode != Opcode::NewArray || literalsSeen.set(literal.index()))
            return;
        TypeID elements = part->contentsAt(program.builtins(), ContentsSlot::Element);
        for (uint32_t index = OperandIndex::NewArray::elements; index < function.at(literal).numberOfOperands(); ++index) {
            if (isTypedOnCredit(function.at(literal).operand(index), elements))
                found.append({ analyses.useLists().pointOf(literal), literal, index, elements });
        }
    };
    auto visit = [&](ProgramPoint point, ValueID user, ValueID stored, uint32_t operandIndex, TypeID required) {
        if (!required || required == TypeTable::anything || !isTypedOnCredit(stored, required) || isCheckedValueItself.get(stored.index()))
            return;
        found.append({ point, user, operandIndex, required });
        visitElementsOfLiteral(stored, required);
    };

    const OpcodeSet& opcodesWithRequiredTypes = opcodesWithRequiredOperandTypes();
    bool hasOneResult = function.results.size() == 1 && !function.isUnsplitCoroutine();
    TypeID requiredResult = hasOneResult ? typing.typeIDOf(function.result()) : TypeID();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Block& contents = function.at(block);
        uint32_t numberOfValues = contents.values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID user = contents.values[index];
            const Value& value = function.at(user);
            // It need not come by a position of this function: it may be stored by name.
            if (value.opcode == Opcode::AssumeType)
                visitElementsOfLiteral(value.valueOperand(), value.provenType());
            if (!opcodesWithRequiredTypes.contains(value.opcode))
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) != OperandKind::Declared)
                    return;
                TypeID required = requiredTypeOfOperand(typing, value, operandIndex);
                if (takesOnlyWhatFlowHolds(program, value, operandIndex))
                    return visit({ block, index }, user, operand, operandIndex, required);
                // Of the slot of an object that a store tests flow leaves out primitives, and no object. No test of a store tells what is proven of the instances of a class: that is flow's word.
                if (required && holderOfRequiredTypes(program, value) == RequiredTypeHolder::InitializedSlot && types.at(required) != types.at(required).withoutClassProofs())
                    visit({ block, index }, user, operand, operandIndex, types.withPrimitives(required, Primitive::All));
            });
        }
        const Terminator& terminator = contents.terminator;
        if (terminator.kind == TerminatorKind::Return)
            visit({ block, numberOfValues }, { }, terminator.valueOperand(), OperandIndex::Return::value, requiredResult);
        if (terminator.kind != TerminatorKind::ReturnCall || !requiredResult || requiredResult == TypeTable::anything || !predicted.isBehindCheck(terminator.callOperand()))
            continue;
        // The casts of a tail call are no operands. What a direct call answers is the word of its callee.
        bool isAnsweredByCallee = function.at(terminator.callOperand()).opcode == Opcode::CallDirect && typing.typeOf(terminator.callOperand()).isSubsetOf(types.at(requiredResult), program);
        if (isAnsweredByCallee || std::ranges::any_of(terminator.casts(), [&](TailCast cast) { return isClaimOfResult(function, cast); }))
            continue;
        Vector<TailCast, 2> casts;
        for (TailCast cast : terminator.casts())
            casts.append(cast);
        casts.append(TailCast { DeclaredType(Proven::byFlowOfPosition(requiredResult)), CastResolution::Assume });
        function.edit(block, BlockEdit::Terminator).terminator.setCasts(casts.span());
    }
    if (found.isEmpty())
        return;

    for (const StoreBehindChecks& store : found) {
        const Instruction& user = store.user ? static_cast<const Instruction&>(function.at(store.user)) : function.at(store.point.block).terminator;
        ValueID stored = user.operand(store.operandIndex);
        builder.setOrigin(user.origin());
        if (validatesClaims == ValidatesClaims::Yes)
            builder.insertBefore(store.point, makeValidateType(stored, store.claimed));
        ValueID assumed = builder.insertBefore(store.point, makeAssumeType(stored, Proven::byFlowOfPosition(store.claimed)));
        Instruction& edited = store.user ? static_cast<Instruction&>(function.edit(store.user, ValueEdit::Definition)) : function.edit(store.point.block, BlockEdit::Terminator).terminator;
        edited.setOperand(store.operandIndex, assumed);
    }
    builder.applyInsertions();
}

void assumeTypesOfPositionsAtStores(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    assumeTypesOfPositionsAtStores(program, function, types, analyses, builder, build.validates() ? ValidatesClaims::Yes : ValidatesClaims::No);
}

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication& body)
{
    const FlowBodyUpdate& update = body.application.bodies[body.id.index()];
    FlowBodyCounts counts;
    Vector<ValueID, 8> answeredChecks;
    counts.removedChecks = applyCheckUpdates(body, update, answeredChecks);
    PredictedValues predicted(body.function);
    applyScopesOfAnsweredChecks(body.function, predicted, answeredChecks.span());
    applyStamps(body, update);
    Vector<ListedMember> listedMembers = makeOrdinaryObjects(body);

    AnalysisContext context { body.build, body.program, nullptr, nullptr, body.id };
    FunctionAnalyses analyses(context, body.function);
    IRBuilder builder(context, body.function, body.types, &analyses);
    applyTailCalls(body, builder, update, counts);
    defineListedMembers(body.function, builder, listedMembers.span());

    FlowCastInsertion insertion { body, analyses, builder, predicted };
    insertBehindDefinitions(insertion, update.proofs, &FlowValueProof::value, [](const FlowValueProof& proof) {
        return proof.type == TypeTable::never;
    }, [&](const FlowValueProof& proof) {
        return makeAssumeType(proof.value, insertion.provenOfValue(proof.value, proof.type));
    });
    insertBehindDefinitions(insertion, update.symbolConstants, &FlowSymbolConstant::call, [](const FlowSymbolConstant&) {
        return false;
    }, [](const FlowSymbolConstant& entry) {
        Value constant = IRBuilder::make(Opcode::KeyConstant);
        constant.setKey(entry.key);
        return constant;
    });
    replaceDefinedValues(insertion, update);
    initializeMembersOfListedHolders(insertion);
    builder.applyInsertions();
    eraseBuiltinConstantsWithoutUse(insertion);

    TypePropagationInputs inputs { body.program, body.id, body.function, analyses, body.types, body.build };
    TypePropagationOutcome typing = typeValuesFromNever(inputs);

    Vector<ValueID> casts = insertCastsForRequiredTypes(insertion);
    if (!casts.isEmpty()) {
        builder.applyInsertions();
        typing += narrowValueTypes(inputs, casts.span());
    }
    if (!typing.waitsToCutCode)
        body.function.setValuesAreTypedAsDefined();
    // insertRuntimeValidation() validates every claim that it finds.
    assumeTypesOfPositionsAtStores(body.program, body.function, body.types, analyses, builder, ValidatesClaims::No);
    if (body.build.validates())
        insertRuntimeValidation(insertion);

    counts.remainingChecks = countChecks(body.function);
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
