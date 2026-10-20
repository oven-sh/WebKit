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
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <span>
#include <wtf/Vector.h>

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
};

} // anonymous namespace

Value makeAssumeType(ValueID operand, TypeID castType)
{
    Value value = IRBuilder::make(Opcode::AssumeType, { operand });
    value.setCastType(castType);
    return value;
}

Value makePredict(ValueID operand, TypeID predicted)
{
    Value value = IRBuilder::make(Opcode::Predict, { operand });
    value.setPredicted(predicted);
    return value;
}

Value makeValidateType(ValueID operand, TypeID validatedType)
{
    Value value = IRBuilder::make(Opcode::ValidateType, { operand });
    value.setValidatedType(validatedType);
    return value;
}

FlowBodyCounts applyClaimUpdates(const FlowBodyApplication& body, const FlowBodyUpdate& update)
{
    FlowBodyCounts counts;
    for (const FlowClaimUpdate& entry : update.claims) {
        Value& claim = body.function.at(entry.claim);
        RELEASE_ASSERT(claim.opcode == Opcode::Claim);
        if (!entry.isRedundant) {
            if (entry.cause != CheckCause::Unknown)
                claim.setCause(entry.cause);
            ++counts.remainingClaims;
            continue;
        }

        ValueID operand = claim.valueOperand();
        TypeID castType = claim.castType();
        claim.become(Opcode::AssumeType);
        claim.setValueOperand(operand);
        claim.setCastType(castType);
        ++counts.removedClaims;
    }
    return counts;
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

static Value makeResultClaim(ValueID call, TypeID castType, TypeSiteID site)
{
    Value claim = IRBuilder::make(Opcode::Claim, { call });
    claim.setCastType(castType);
    claim.setSite(site);
    claim.setCause(CheckCause::MayFail);
    return claim;
}

void insertResultClaims(const FlowCastInsertion& insertion, const FlowBodyUpdate& update, FlowBodyCounts& counts)
{
    Vector<FlowDirectCallUpdate, 4> checkedCalls;
    for (const FlowDirectCallUpdate& entry : update.directCalls) {
        if (entry.resultCheck == ResultCheck::ByCallers)
            checkedCalls.append(entry);
    }
    if (checkedCalls.isEmpty())
        return;

    const FlowBodyApplication& body = insertion.body;
    Function& function = body.function;
    TypeArena& types = body.types;
    TypingContext written { body.program, function, types, TypeLevel::Written };
    RedirectionPlan plan = planRedirections(insertion, checkedCalls.span(), &FlowDirectCallUpdate::call);
    for (const PlannedRedirection& planned : plan.redirections) {
        const FlowDirectCallUpdate& entry = checkedCalls[planned.entryIndex];
        const DeclaredType& declared = body.program.at(entry.function).result();
        TypeID castType = types.intern(written.typeOf(declared).withoutProofs());

        Value cast;
        if (entry.resultIsInsideWrittenType) {
            // The callee's result type is now the join over all its calls, which may exceed what flow proved about this one.
            bool followsFromSignature = types.at(declared.type).isSubsetOf(types.at(castType), body.program);
            if (!planned.numberOfUses || followsFromSignature)
                continue;
            cast = makeAssumeType(entry.call, castType);
        } else {
            cast = makeResultClaim(entry.call, castType, declared.site);
            ++counts.remainingClaims;
        }

        insertion.builder.setOrigin(function.at(entry.call).origin());
        redirectUses(function, plan.usesOf(planned), insertion.builder.insertBefore(planned.point, WTF::move(cast)));
    }
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
        ValueID current = proof.value;
        if (proof.type)
            current = builder.insertBefore(planned.point, makeAssumeType(current, proof.type));
        if (proof.predicted)
            current = builder.insertBefore(planned.point, makePredict(current, proof.predicted));
        redirectUses(function, plan.usesOf(planned), current);
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

void startReadsAtPrototype(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    Function& function = insertion.body.function;
    for (ValueID id : update.readsFromPrototype) {
        ValueLocation location = insertion.analyses.useLists().locationOf(id);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && function.at(id).opcode == Opcode::PropGet);

        ValueID object = function.at(id).objectOperand();
        PropertyKeyID key = function.at(id).key();
        AccessSiteID accessSite = function.at(id).accessSite();

        Value prototype = IRBuilder::make(Opcode::GetSuperBase, { object });
        prototype.setHomeKind(HomeObjectKind::Object);
        insertion.builder.setOrigin(function.at(id).origin());
        ValueID base = insertion.builder.insertBefore({ location.block, location.index }, WTF::move(prototype));

        Value& read = function.at(id);
        read.become(Opcode::SuperGet);
        read.setBaseOperand(base);
        read.setReceiverOperand(object);
        read.setKey(key);
        read.setAccessSite(accessSite);
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
        if (program.at(program.at(entry.function).creator.as<CreatorRecord::BoundFunction>().target).has(FunctionFlags::HasThis)) {
            insertion.builder.setOrigin(function.at(entry.call).origin());
            captures.append(boundThis ? boundThis : insertion.builder.insertBefore({ location.block, location.index }, IRBuilder::make(Opcode::Undefined)));
        }
        captures.append(target);

        Value& creation = function.at(entry.call);
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
        function.at(entry.definition).setValueOperand(value);
    }
}

static void createByCopying(Function& function, const FlowAllocationStamp& stamp)
{
    const Value& filling = function.at(stamp.fillingCopy);
    RELEASE_ASSERT(function.at(stamp.value).opcode == Opcode::NewObject && filling.opcode == Opcode::CopyDataProperties);

    Value copy = IRBuilder::make(Opcode::CopyObject, { filling.sourceOperand() });
    copy.setExcludedKeys(filling.excludedKeys());
    copy.setExcludedOperands(filling.excludedOperands());
    copy.setOrigin(filling.origin());
    function.at(stamp.value) = WTF::move(copy);
    function.at(stamp.fillingCopy).become(Opcode::Undefined);
}

// `{}` is Object.create(%Object.prototype%), whose result is an ordinary object.
void createDictionaries(const FlowCastInsertion& insertion, const FlowBodyUpdate& update)
{
    Function& function = insertion.body.function;
    auto insertBuiltin = [&](ProgramPoint point, BuiltinID id) {
        Value builtin = IRBuilder::make(Opcode::Builtin);
        builtin.setBuiltin(id);
        return insertion.builder.insertBefore(point, WTF::move(builtin));
    };
    for (const FlowAllocationStamp& stamp : update.allocations) {
        if (!stamp.createsDictionary)
            continue;
        if (stamp.fillingCopy) {
            createByCopying(function, stamp);
            continue;
        }
        ValueLocation location = insertion.analyses.useLists().locationOf(stamp.value);
        Opcode literal = function.at(stamp.value).opcode;
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && (literal == Opcode::NewObject || literal == Opcode::Data));

        ProgramPoint point { location.block, location.index };
        insertion.builder.setOrigin(function.at(stamp.value).origin());
        ValueID callee = insertBuiltin(point, Builtins::ObjectCreate);
        ValueID prototype = insertBuiltin(point, Builtins::ObjectPrototype);
        Value call = IRBuilder::make(Opcode::Call);
        call.setCalleeOperand(callee);
        call.setArgumentsOperands(singleElementSpan(prototype));
        ValueID creation = insertion.builder.insertBefore(point, WTF::move(call));

        // The definitions of a literal take an object, and a call has that type only once it is resolved.
        Origin origin = function.at(stamp.value).origin();
        function.at(stamp.value) = makeAssumeType(creation, TypeTable::object);
        function.at(stamp.value).setOrigin(origin);
    }
}

static ValueID castToRequiredType(RequiredTypeCasts& casts, ProgramPoint point, Origin origin, ValueID operand, TypeID required)
{
    const TypingContext& typing = casts.typing;
    if (typing.typeOf(operand).isSubsetOf(typing.types.at(required), typing.program))
        return operand;

    IRBuilder& builder = casts.insertion.builder;
    builder.setOrigin(origin);
    ValueID cast = builder.insertBefore(point, makeAssumeType(operand, required));
    casts.inserted.append(cast);
    return cast;
}

static void castDeclaredOperands(RequiredTypeCasts& casts, ProgramPoint point)
{
    Function& function = casts.insertion.body.function;
    ValueID user = function.at(point.block).values[point.index];
    for (uint32_t operandIndex = 0; operandIndex < function.at(user).numberOfOperands(); ++operandIndex) {
        const Value& value = function.at(user);
        ValueID operand = value.operand(operandIndex);
        if (!operand)
            continue;
        TypeID required = requiredTypeOfOperand(casts.typing, value, operandIndex);
        if (!required)
            continue;

        // An insertion may move the values, so the user is looked up again.
        ValueID cast = castToRequiredType(casts, point, value.origin(), operand, required);
        function.at(user).setOperand(operandIndex, cast);
    }
}

static void castReturnedValue(RequiredTypeCasts& casts, BlockID block)
{
    Function& function = casts.insertion.body.function;
    Terminator& terminator = function.at(block).terminator;
    TypeID required = requiredTypeOfOperand(casts.typing, terminator, OperandIndex::Return::value);
    if (!required)
        return;
    terminator.setValueOperand(castToRequiredType(casts, pointBeforeTerminatorOf(function, block), terminator.origin(), terminator.valueOperand(), required));
}

static void assumeTailCallResult(RequiredTypeCasts& casts, BlockID block)
{
    const TypingContext& typing = casts.typing;
    TypeArena& types = typing.types;
    Terminator& terminator = casts.insertion.body.function.at(block).terminator;

    Type returned = typing.typeOf(terminator.callOperand());
    Vector<TailCast, 2> tailCasts;
    for (TailCast cast : terminator.casts()) {
        returned = returned.castTo(types.at(cast.declared.type), types);
        tailCasts.append(cast);
    }
    TypeID required = typing.function.result().type;
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

static ValueID calledOperandOf(const Value& call)
{
    ValueID called;
    call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandUsesOf(call.opcode, operandIndex).contains(OperandUse::Calls))
            called = operand;
    });
    return called;
}

// A stamp names the versions that run. A closure names the function it was created from.
static std::optional<Type> typeOfListedCallees(const FlowBodyApplication& body, const Value& call)
{
    if (!PayloadWord::callees(call.opcode) || !call.callees())
        return std::nullopt;
    const CalleeSet& callees = body.types.at(call.callees());
    if (!callees.isComplete || body.results.mayCallBoundFunction(call.accessSite()))
        return std::nullopt;

    Type listed;
    for (FunctionID version : callees.functions)
        listed = listed.unionWith(Type::forFunction(body.application.versions.bodyOf(version)), body.types);
    for (BuiltinID builtin : callees.builtins)
        listed = listed.unionWith(Type::forBuiltin(builtin), body.types);

    // A class that is called and an arrow function that is constructed throw before they are entered, so no stamp lists them.
    bool isConstruction = !!OperandPosition::newTarget(call.opcode);
    for (FunctionID function : body.types.at(body.function.at(calledOperandOf(call)).type).functions) {
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
    if (!receivers.isComplete)
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
// These operations read the value from before the cast and no pass removes them.
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
            if (std::optional<Type> listed = typeOfListedCallees(body, value)) {
                Value test = IRBuilder::make(Opcode::ValidateCallee, { calledOperandOf(value) });
                test.setValidatedType(body.types.intern(*listed));
                builder.insertBefore({ block, index }, WTF::move(test));
            }
        }
    }
    builder.applyInsertions();
}

static bool isNarrowerInLayouts(const Program& program, const Type& current, const Type& narrowed)
{
    if (!narrowed.layouts)
        return false;
    return !current.layouts || membersOf(program, current).size() > membersOf(program, narrowed).size();
}

static bool hasUnknownCallees(const Type& type)
{
    return type.anyFunction || type.anyObject;
}

static bool isNarrowerInCallees(const Type& current, const Type& narrowed)
{
    if (!narrowed.mayBeFunction() || hasUnknownCallees(narrowed))
        return false;
    return hasUnknownCallees(current) || current.functions.size() > narrowed.functions.size() || current.builtins.size() > narrowed.builtins.size();
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
        || isNarrowerInCallees(current, narrowed)
        || isNarrowerInClasses(current, narrowed);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
