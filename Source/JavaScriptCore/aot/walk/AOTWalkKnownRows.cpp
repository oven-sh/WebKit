/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Known rows: what the table of built-ins says of a call that no one row was matched for: a callee that is one of several built-ins, a super call of a built-in class, a row whose code is a function of the library.
// At a suspension: no row suspends the frame of its caller.
// At a throw: a call whose operands are not proven to lie in the parameters of a row may be refused, so it throws by rule.
// Withheld: every door here stands behind BuiltinCalls. The direct call of a function of the library asks DirectCalls as well: callFunctionDirectly().

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnownCalls.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static constexpr OperandUses usesThatCallForKnownRows { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };
static constexpr OperandUses usesThatReachBeyondOperandForKnownRows { OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
// A store by key runs setters and traps.
static constexpr OperandUses usesThatStoreByKeyForKnownRows { OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::TakesDescriptor };
static constexpr OperandUses usesThatChangeShapeForKnownRows { OperandUse::DefinesName, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype };
static constexpr OperandUses usesThatChangeOperandForKnownRows {
    OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom,
    OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype,
};

// ---- A function of the library behind a row

// Link has not seen the call. What it says of a function whose callers it has counted does not hold of one more: callFunctionDirectly().
Effects effectsOfLibraryFunctionBehindRow(const Walker& walker, const BuiltinOverload& row)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, row.libraryFunction());
    if (!entry || !entry->function || entry->function.index() >= linked.functions.size() || !program.at(entry->function).hasBody())
        return Effects::unknownCode();
    const LinkedFunction& entered = linked.function(entry->function);
    if (!entered.isReached || entered.everyCallerIsKnown)
        return Effects::unknownCode();

    Effects effects = entered.effectsOnReturn;
    effects.merge(entered.effectsOnThrow);
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (program.builtins().has(BuiltinMark::MayTerminateProcess, row.builtin))
        effects.mayBeTerminated = true;
    // The error is allocated on the way to the handler.
    effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    addVariablesThatStandForFields(walker.plan(), effects);
    return effects;
}

// Only a string among the elements is compared by its characters, and only with a string.
std::optional<Effects> effectsOfSearchForWhatIsNoString(const TypeArena& types, InlineBuiltin builtin, TypeID searchedOrNotPassed)
{
    if (builtin != InlineBuiltin::ArrayIndexOf && builtin != InlineBuiltin::ArrayIncludes)
        return std::nullopt;
    if (!searchedOrNotPassed)
        return std::nullopt;
    Type searched = types.at(searchedOrNotPassed);
    if (!searched.isJavaScriptValue() || (searched.primitives & Primitive::AnyString))
        return std::nullopt;
    return declaredBoundOf(InlineBuiltin::ArrayAt);
}

// ---- What one row does on operands that nobody has matched to it

namespace {

enum class FitOfOperandsForKnownRows : uint8_t {
    // Each operand lies inside what its parameter accepts.
    IsProven,
    // Native code may turn an operand away.
    IsUnknown,
};

// As CallInWalkBuiltins::effectsOfRow(), of operands as they are written: native code converts them itself, by the protocols that the row lists.
class EffectsOfRowForKnownRows {
public:
    EffectsOfRowForKnownRows(const Walker& walker, const BuiltinOverload& row, const BuiltinOperandTypes& operands, FitOfOperandsForKnownRows fit)
        : m_walker(walker)
        , m_program(walker.program())
        , m_table(walker.program().builtins())
        , m_types(walker.types())
        , m_row(row)
        , m_operands(operands)
        , m_fit(fit)
    {
    }

    Effects effects() const;

private:
    static bool mayBeObject(const Type& type) { return type.hasObjects() || !type.isJavaScriptValue(); }

    bool mayRunCodeThatNoOperandNames() const;
    bool namesNoHeapForItsUses() const;
    bool takesObjectsByClassAlone(BuiltinTypeID accepted) const;
    bool mayRaiseErrorOn(const BuiltinParameter&, TypeID operandOrNotPassed, const Type&) const;
    bool callsBeforeReturn(const BuiltinParameter&) const;
    bool addEffectsOfFunctionsCalled(Effects&, const BuiltinParameter&, const Type&) const;
    void addEffectsOn(Effects&, const BuiltinParameter&, TypeID operand) const;

    const Walker& m_walker;
    const Program& m_program;
    const BuiltinTable& m_table;
    TypeArena& m_types;
    const BuiltinOverload& m_row;
    const BuiltinOperandTypes& m_operands;
    const FitOfOperandsForKnownRows m_fit;
};

bool EffectsOfRowForKnownRows::mayRunCodeThatNoOperandNames() const
{
    if (m_row.effects.capability == BuiltinCapability::JS && !m_row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        return true;
    if (m_table.at(m_row.builtin).flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }))
        return true;
    if (m_table.has(BuiltinMark::RunsWaitingJobs, m_row.builtin))
        return true;
    return std::ranges::any_of(m_row.flow.handlerCalls, [](const BuiltinHandlerCall& call) {
        return !isCalledAfterReturn(call.times);
    });
}

// The generator of the table derives heaps from uses, so a row that changes or reads an operand names a heap for it.
bool EffectsOfRowForKnownRows::namesNoHeapForItsUses() const
{
    OperandUses uses;
    if (m_row.receiver)
        uses.add(m_row.receiver->uses);
    for (const BuiltinParameter& parameter : m_row.parameters)
        uses.add(parameter.uses);
    if (uses.containsAny(usesThatChangeOperandForKnownRows) && m_row.effects.writes.isEmpty())
        return true;
    return uses.contains(OperandUse::ReadsContents) && m_row.effects.reads.isEmpty();
}

// The host refuses no instance of a class that the parameter takes. Of any other object that it takes it may refuse a member, and it describes what it refuses in the message of the error.
bool EffectsOfRowForKnownRows::takesObjectsByClassAlone(BuiltinTypeID acceptedID) const
{
    if (!acceptedID)
        return false;
    const BuiltinType& accepted = m_table.at(acceptedID);
    if (accepted.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        return false;
    return std::ranges::all_of(accepted.parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.contents.empty();
    });
}

// Of a row that throws nothing of its own: whether the call throws by a rule of the language lies with its operands.
bool EffectsOfRowForKnownRows::mayRaiseErrorOn(const BuiltinParameter& parameter, TypeID operandOrNotPassed, const Type& type) const
{
    constexpr Protocols protocolsThatNeedMembers { Protocol::Iterate, Protocol::CallsNamed, Protocol::Species, Protocol::HasInstance };
    constexpr uint16_t primitivesThatBecomeStringsAndNumbers = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString;

    if (parameter.admitted) {
        BuiltinTypeContext context { m_program, m_types, &m_walker.facts() };
        return mayBeObject(type) || fitOf(context, operandOrNotPassed, parameter.admitted).fit != TypeFit::Inside;
    }
    if (parameter.protocols.containsAny(protocolsThatNeedMembers))
        return true;
    if (parameter.protocols.contains(Protocol::ToPrimitive) && !type.hasOnlyPrimitives(primitivesThatBecomeStringsAndNumbers))
        return true;
    return mayBeObject(type) && parameter.uses.containsAny(usesThatChangeOperandForKnownRows);
}

bool EffectsOfRowForKnownRows::callsBeforeReturn(const BuiltinParameter& parameter) const
{
    if (!parameter.uses.containsAny(usesThatCallForKnownRows))
        return false;
    bool isListedAsCalledLater = false;
    for (const BuiltinCallback& callback : m_row.flow.callbacks) {
        if (callback.callee != parameter.operand)
            continue;
        if (!isCalledAfterReturn(callback.times))
            return true;
        isListedAsCalledLater = true;
    }
    return !isListedAsCalledLater;
}

// It is converted, settles a promise or is asked for a member: each may run code that an object among the answers names.
static bool rowLooksIntoAnswerForKnownRows(BuiltinCallbackResultUse use)
{
    switch (use) {
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
    case BuiltinCallbackResultUse::Returned:
        return false;
    case BuiltinCallbackResultUse::Escapes:
    case BuiltinCallbackResultUse::Stored:
    case BuiltinCallbackResultUse::Flattened:
    case BuiltinCallbackResultUse::AskedWhetherIterable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The row says how it calls the operand, and the operand is functions of the program alone: the call does what they do, whatever they are handed. False: nobody says what runs.
bool EffectsOfRowForKnownRows::addEffectsOfFunctionsCalled(Effects& effects, const BuiltinParameter& parameter, const Type& type) const
{
    if (!type.hasOnlyClosures() || type.namesMergedObjects())
        return false;

    bool rowListsCall = false;
    for (const BuiltinCallback& callback : m_row.flow.callbacks) {
        if (callback.callee != parameter.operand)
            continue;
        if (callback.constructs || callback.afterwards != BuiltinCallbackAftermath::TouchesOnlyWhatRowNames || rowLooksIntoAnswerForKnownRows(callback.resultUse))
            return false;
        rowListsCall = true;
    }
    if (!rowListsCall)
        return false;

    const Facts& facts = m_walker.facts();
    Effects ofFunctions = Effects::none();
    bool everyBodyIsEntered = true;
    forEachFunctionOf(m_types, type, scopedLambda<void(FunctionID)>([&](FunctionID function) {
        const Function& callee = m_program.at(function);
        everyBodyIsEntered &= callee.hasBody() && !isClassConstructorOf(callee.kind);
        ofFunctions.merge(facts.effectsOnReturn(function));
        ofFunctions.merge(facts.effectsOnThrow(function));
    }));
    if (!everyBodyIsEntered)
        return false;

    ofFunctions.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (type.primitives)
        ofFunctions.mayThrow = true;
    effects.merge(ofFunctions);
    return true;
}

void EffectsOfRowForKnownRows::addEffectsOn(Effects& effects, const BuiltinParameter& parameter, TypeID operand) const
{
    if (effects.mayRunOtherCode())
        return;
    if (!operand)
        operand = TypeTable::anything;
    Type type = m_types.at(operand);
    bool isObject = mayBeObject(type);

    if (readsCharactersOfString(parameter.conversion) && (type.primitives & Primitive::AnyString)) {
        Effects ofReadingCharacters = declaredBoundOf(NamedEffectSet::ResolvesRopes);
        ofReadingCharacters.mayThrow = ThrowEffect::onlyForLackOfMemory();
        effects.merge(ofReadingCharacters);
    }
    if (m_row.onlyPropagatesErrors() && mayRaiseErrorOn(parameter, operand, type))
        effects.mayThrow = true;
    if (!parameter.protocols.isEmpty()) {
        // Nobody here resolves the members that a protocol finds on what an operand holds.
        TypingContext typing { m_program, m_types, PositionTypes::knowingNothing() };
        bool reachesBeyondOperand = isObject && parameter.uses.containsAny(usesThatReachBeyondOperandForKnownRows);
        effects.merge(reachesBeyondOperand ? Effects::unknownCode() : effectsOfProtocolsOn(typing, parameter.protocols, operand));
    }
    if (effects.mayRunOtherCode() || !isObject)
        return;

    bool mayBeRefused = m_fit == FitOfOperandsForKnownRows::IsUnknown || !takesObjectsByClassAlone(parameter.accepted);
    bool mayBeDescribedInMessage = parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused) && !type.hasOnlyClosures() && mayBeRefused;
    if (parameter.uses.containsAny(usesThatStoreByKeyForKnownRows) || mayBeDescribedInMessage || (callsBeforeReturn(parameter) && !addEffectsOfFunctionsCalled(effects, parameter, type))) {
        effects.merge(Effects::unknownCode());
        return;
    }
    if (parameter.uses.containsAny(usesThatChangeShapeForKnownRows))
        effects.writes.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards, AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
}

// Never narrower than what the call can do. A capability says whether native code allocates and whether it can enter code of the program. What the call reads and writes is what the row names.
Effects EffectsOfRowForKnownRows::effects() const
{
    if (m_row.isUndeclared())
        return Effects::unknownCode();
    if (m_row.libraryFunction())
        return effectsOfLibraryFunctionBehindRow(m_walker, m_row);
    if (mayRunCodeThatNoOperandNames() || namesNoHeapForItsUses())
        return Effects::unknownCode();
    // A row names nothing only where it has no operand either, and what such a function answers comes out of some state.
    if (!m_row.receiver && m_row.parameters.empty() && m_row.effects.reads.isEmpty() && m_row.effects.writes.isEmpty())
        return Effects::unknownCode();

    Effects effects = Effects::none();
    effects.reads.add(m_row.effects.reads);
    effects.writes.add(m_row.effects.writes);
    switch (m_row.effects.capability) {
    case BuiltinCapability::None:
        break;
    case BuiltinCapability::Heap:
    case BuiltinCapability::JS: {
        Effects ofHeap = declaredBoundOf(NamedEffectSet::Allocates);
        ofHeap.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
        ofHeap.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
        ofHeap.mayThrow = ThrowEffect::onlyForLackOfMemory();
        effects.merge(ofHeap);
        break;
    }
    }
    if (m_row.throws())
        effects.mayThrow |= m_row.onlyPropagatesErrors() ? ThrowEffect::onlyForLackOfMemory() : ThrowEffect(true);
    if (m_table.has(BuiltinMark::MayTerminateProcess, m_row.builtin))
        effects.mayBeTerminated = true;

    if (m_row.receiver) {
        TypeID receiver = m_operands.receiver ? m_operands.receiver : TypeID(TypeTable::anything);
        if (m_types.at(receiver).mayBeUndefinedOrNull())
            effects.mayThrow = true;
        addEffectsOn(effects, *m_row.receiver, receiver);
    }
    std::span<const TypeID> arguments = m_operands.arguments.span();
    Type undefined = m_types.at(TypeTable::undefined);
    for (const BuiltinParameter& parameter : m_row.parameters) {
        size_t first = parameter.operand.argumentIndex();
        bool takesRest = parameter.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom;
        if (first >= arguments.size()) {
            if (!takesRest && m_row.onlyPropagatesErrors() && mayRaiseErrorOn(parameter, TypeID(), undefined))
                effects.mayThrow = true;
            continue;
        }
        size_t end = takesRest ? arguments.size() : first + 1;
        for (size_t index = first; index < end; ++index)
            addEffectsOn(effects, parameter, arguments[index]);
    }
    if (effects.mayRunOtherCode())
        return Effects::unknownCode();

    if (m_fit == FitOfOperandsForKnownRows::IsUnknown)
        effects.mayThrow = true;
    // The error is allocated on the way to the handler.
    if (effects.mayThrow)
        effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

struct CallOfBuiltinsForKnownRows {
    // Canonical.
    Vector<BuiltinID, 4> builtins;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    BuiltinOperandTypes operands;
};

} // anonymous namespace

// overloadTakesOperands() takes the word of whoever has tested that objects are pristine. Nobody has tested anything here.
static bool operandsAreProvenToLieInParametersForKnownRows(const BuiltinTypeContext& context, BuiltinOverloadID id, const BuiltinOperandTypes& operands)
{
    if (!overloadTakesOperands(context, id, BuiltinCallForm::Whole, operands))
        return false;
    const BuiltinTable& table = context.program.builtins();
    const BuiltinOverload& row = table.at(id);
    auto isProvenToLieIn = [&](TypeID operand, BuiltinTypeID taken) {
        OperandFit fit = fitOf(context, operand, taken);
        return fit.fit == TypeFit::Inside && !fit.unprovenPristine;
    };
    auto isTakenAt = [&](BuiltinOperand position, TypeID operand) {
        const BuiltinParameter* parameter = table.parameterFor(row, position);
        return parameter && (isProvenToLieIn(operand, parameter->accepted) || (parameter->converted && isProvenToLieIn(operand, parameter->converted)));
    };
    if (operands.receiver && !isTakenAt(BuiltinOperand::receiver(), operands.receiver))
        return false;
    for (unsigned index = 0; index < operands.arguments.size(); ++index) {
        if (!isTakenAt(BuiltinOperand::argument(index), operands.arguments[index]))
            return false;
    }
    return true;
}

// A feature that no listed use of a built-in asks for is left out of the executable, and a function that throws stands in the place of its entries.
static bool mayBelieveRowsForKnownRows(const Walker& walker, BuiltinID builtin)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!isCallableBuiltin(table, builtin) || isCallThatTheWalkMustLower(table, builtin))
        return false;
    return !hasFeatureGuard(table, builtin) || findFactRow(walker.facts().builtinUses(), builtin.index(), &BuiltinUseRecord::builtin);
}

// `class E extends Error { constructor(m) { super(m); } }`: the rows of a construction have the callee as new.target. They hold of another where the constructor only reads its `prototype`, which a closure holds as a plain value.
// `new Map(pairs)` calls the `set` that it finds on the new object, which a class below may have written.
static bool rowsSpeakOfSuperCallForKnownRows(Walker& walker, const BodyValue& superCall, const TypesOfInputs& typeOf, std::span<const BuiltinID> parents)
{
    if (parents.size() != 1)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    const Builtin& parent = table.at(parents[0]);
    if (parent.kind != BuiltinKind::Constructor || !parent.instanceClass)
        return false;
    const auto& made = table.at(parent.instanceClass);
    if (!made.constructorOnlyReadsPrototypeOfNewTarget || made.constructorAdder)
        return false;
    TypeID newTarget = superCall.newTargetOperand() ? typeOf(superCall.newTargetOperand()) : TypeID();
    if (!newTarget)
        return false;
    Type type = walker.types().at(newTarget);
    return type.isObject() && type.hasOnlyClosures();
}

static std::optional<CallOfBuiltinsForKnownRows> describeCallForKnownRows(Walker& walker, const BodyValue& call, const TypesOfInputs& typeOf)
{
    bool isSuperCall = call.opcode() == Opcode::SuperConstruct;
    if (!isSuperCall && (call.opcode() != Opcode::Call || call.isDoneInPlace()))
        return std::nullopt;
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    TypeID callee = typeOf(isSuperCall ? call.parentOperand() : call.calleeOperand());
    if (!callee)
        return std::nullopt;
    Type type = types.at(callee);
    if (!type.isObject() || !type.hasOnlyBuiltins())
        return std::nullopt;

    CallOfBuiltinsForKnownRows described;
    described.mode = isSuperCall ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    bool everyRowMayBeBelieved = true;
    forEachBuiltinOf(types, type, scopedLambda<void(BuiltinID)>([&](BuiltinID named) {
        BuiltinID builtin = table.canonicalID(named);
        everyRowMayBeBelieved = everyRowMayBeBelieved && mayBelieveRowsForKnownRows(walker, builtin);
        if (!described.builtins.contains(builtin))
            described.builtins.append(builtin);
    }));
    if (!everyRowMayBeBelieved || described.builtins.isEmpty())
        return std::nullopt;
    if (isSuperCall && !rowsSpeakOfSuperCallForKnownRows(walker, call, typeOf, described.builtins.span()))
        return std::nullopt;

    auto isValue = [](TypeID operand) {
        return operand && !isNeverType(operand);
    };
    if (ValueID thisValue = isSuperCall ? ValueID() : call.thisValueOperand()) {
        described.operands.receiver = typeOf(thisValue);
        if (!isValue(described.operands.receiver))
            return std::nullopt;
    }
    for (ValueID argument : call.argumentsOperands()) {
        TypeID passed = argument ? typeOf(argument) : TypeID();
        if (!isValue(passed))
            return std::nullopt;
        described.operands.arguments.append(passed);
    }
    if (described.operands.arguments.size() > BuiltinOperand::maxArgumentIndex + 1)
        return std::nullopt;
    return described;
}

// Wider operands reach more rows and a wider callee names more built-ins, so a wider look never counts on less.
std::optional<Effects> effectsOfCallOfBuiltinsByRows(Walker& walker, const BodyValue& call, const TypesOfInputs& typeOf)
{
    std::optional<CallOfBuiltinsForKnownRows> described = describeCallForKnownRows(walker, call, typeOf);
    if (!described)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), walker.types(), &walker.facts() };
    Effects effects = Effects::none();
    for (BuiltinID builtin : described->builtins) {
        if (described->mode == BuiltinCallMode::Call && !rowsSpeakOfCallOnReceiver(context, builtin, described->operands.receiver))
            return std::nullopt;
        Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(context, builtin, described->mode, described->operands);
        if (reached.isEmpty())
            return std::nullopt;
        for (BuiltinOverloadID id : reached) {
            const BuiltinOverload& row = table.at(id);
            // A row without a receiver never looks at `this`.
            BuiltinOperandTypes taken = described->operands;
            if (!row.receiver)
                taken.receiver = TypeID();
            FitOfOperandsForKnownRows fit = operandsAreProvenToLieInParametersForKnownRows(context, id, taken) ? FitOfOperandsForKnownRows::IsProven : FitOfOperandsForKnownRows::IsUnknown;
            effects.merge(EffectsOfRowForKnownRows(walker, row, described->operands, fit).effects());
            if (effects.mayRunOtherCode())
                return std::nullopt;
        }
    }
    // The way into native code by the value of the function.
    effects.reads.add(bookkeepingHeaps);
    effects.writes.add(bookkeepingHeaps);
    effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();
    effects.mayCollect = true;
    return effects;
}

TypeID resultOfCallOfBuiltinsByRows(Walker& walker, const BodyValue& call, const TypesOfInputs& typeOf)
{
    std::optional<CallOfBuiltinsForKnownRows> described = describeCallForKnownRows(walker, call, typeOf);
    // Of one built-in the rule of the call asks the rows itself, and a super call gives an object of the class that holds it.
    if (!described || described->mode != BuiltinCallMode::Call || described->builtins.size() < 2)
        return { };

    BuiltinTypeContext context { walker.program(), walker.types(), &walker.facts() };
    TypeID joined;
    for (BuiltinID builtin : described->builtins) {
        if (!rowsSpeakOfCallOnReceiver(context, builtin, described->operands.receiver))
            return { };
        TypeID given = resultTypeOfReachableOverloads(context, builtin, BuiltinCallMode::Call, described->operands);
        if (!given)
            return { };
        joined = joined ? joinOf(walker.types(), walker.facts().families(), joined, given) : given;
    }
    return joined;
}

// ---- Calls that become something cheaper

// As CallInWalkBuiltins::findCallee(). Canonical.
static BuiltinID findBuiltinCalledForKnownRows(Walker& walker, const BodyValue& call)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.valueBehindCasts(call.calleeOperand());
        bool isReadByName = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName();
        if (isReadByName)
            named = findConstantMemberReadOff(walker, walker.referenceOf(index.value(read).objectOperand()), index.value(read).key());
    }
    return named ? walker.program().builtins().canonicalID(named) : BuiltinID();
}

static bool usesDecimalRadixForKnownRows(Walker& walker, std::span<const ValueID> arguments)
{
    if (arguments.empty())
        return true;
    Reference radix = arguments[0] ? walker.referenceOf(arguments[0]) : Reference();
    TypeID proven = radix ? walker.typeOf(radix) : TypeID();
    if (!proven)
        return false;
    if (proven == TypeTable::undefined)
        return true;
    Type type = walker.types().at(proven);
    return type.isInt32() && type.integers.isSubsetOf({ 10, 10 });
}

// The operator folds over a constant, has a form for each kind of primitive and is a part of a concatenation.
static bool buildToStringOfPrimitiveForKnownRows(Walker& walker, const BodyValue& call, BuiltinID callee)
{
    const BuiltinTable& table = walker.program().builtins();
    uint16_t receivers = 0;
    bool takesRadix = true;
    if (callee == table.canonicalID(Builtins::NumberPrototypeToString))
        receivers = Primitive::Number;
    else if (callee == table.canonicalID(Builtins::BigIntPrototypeToString))
        receivers = Primitive::BigInt;
    else if (callee == table.canonicalID(Builtins::BooleanPrototypeToString)) {
        receivers = Primitive::Boolean;
        takesRadix = false;
    } else
        return false;

    Reference receiver = call.thisValueOperand() ? walker.referenceOf(call.thisValueOperand()) : Reference();
    TypeID proven = receiver ? walker.typeOf(receiver) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    if (type.isNever() || !type.hasOnlyPrimitives(receivers))
        return false;
    if (takesRadix && !usesDecimalRadixForKnownRows(walker, call.argumentsOperands()))
        return false;

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Draft conversion(Opcode::ToString, origin);
    conversion.setValueOperand(receiver);
    walker.chooseConservativeFields(conversion);
    conversion.setForm(formAllowedForTypedOperator(walker, conversion));
    conversion.inputValueGiven = call.id();
    if (Reference text = walker.build(WTF::move(conversion)))
        walker.bind(call.id(), text);
    return true;
}

bool buildToStringOfPrimitiveByRows(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    BuiltinID callee = findBuiltinCalledForKnownRows(walker, call);
    return callee && buildToStringOfPrimitiveForKnownRows(walker, call, callee);
}

// A look at a loop types the call by the rows of the built-in. The direct call is typed by what Link has proven of the function, which need not lie within that: widestTypeOfOperation().
static bool directCallLiesWithinLooksForKnownRows(Walker& walker, const BodyValue& call, FunctionID function)
{
    if (!walker.isInLoop())
        return true;
    Draft direct(Opcode::CallDirect);
    direct.setFunction(function);
    direct.setRole(walker.plan().conventions().directEntryOf(function).role);
    TypeID given = walker.resultTypeOf(direct);
    TypeID widest = widestTypeOfOperation(walker, call);
    return !widest || (given && walker.types().isSubset(given, widest));
}

// ---- A sort that calls no comparator

// `(a, b) => a - b`, `(a, b) => b - a`: the body is casts, one subtraction of one parameter from the other, and the return of that. Null: it is something else.
static BuiltinID sortForComparatorThatIsDifferenceForKnownRows(const Walker& walker, FunctionID comparator)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest };
    const Function& function = walker.program().at(comparator);
    const BodyIndex* index = walker.facts().bodies().find(comparator);
    if (!index || !function.hasBody() || function.numberOfPositionalParameters() != 2 || function.hasAny(otherForms))
        return { };
    BodyBlock entry = index->block(entryBlock);
    unsigned firstParameter = function.firstParameterIndex();
    if (entry.hasHandler() || entry.terminator().kind() != TerminatorKind::Return || entry.numberOfParameters() < firstParameter + 2)
        return { };

    ValueID difference;
    for (BodyValue value : entry.values()) {
        if (opcodesThatStateTypes.contains(value.opcode()))
            continue;
        if (value.opcode() != Opcode::Sub || difference)
            return { };
        difference = value.id();
    }
    ValueID returned = entry.terminator().operand(0);
    if (!difference || !returned || index->valueBehindCasts(returned) != difference)
        return { };
    ValueID left = index->valueBehindCasts(index->value(difference).operand(0));
    ValueID right = index->valueBehindCasts(index->value(difference).operand(1));
    if (left == entry.parameter(firstParameter) && right == entry.parameter(firstParameter + 1))
        return Builtins::CompilerHelpersSortByDifference;
    if (left == entry.parameter(firstParameter + 1) && right == entry.parameter(firstParameter))
        return Builtins::CompilerHelpersSortByReversedDifference;
    return { };
}

// `list.sort((a, b) => a - b)`. The library has a function that sorts a short list of numbers where it lies, without one call: on numbers nobody sees such a comparator run. It looks at the list itself, and hands every other list to sort().
// It takes the list and the comparator as its arguments. Null: the call is no such call, or the program does not hold the function.
static FunctionID findSortThatCallsNoComparatorForKnownRows(Walker& walker, const BodyValue& call, BuiltinID callee)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (callee != table.canonicalID(Builtins::ArrayPrototypeSort) || call.argumentsOperands().size() != 1 || !call.thisValueOperand())
        return { };
    Reference passed = walker.referenceOf(call.argumentsOperands()[0]);
    TypeID proven = passed ? walker.typeOf(passed) : TypeID();
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    FunctionID comparator = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    BuiltinID helper = comparator ? sortForComparatorThatIsDifferenceForKnownRows(walker, comparator) : BuiltinID();
    if (!helper || !table.contains(helper))
        return { };
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(helper))) {
        const RuntimeFunctionEntry* held = row.mode == BuiltinCallMode::Call ? findRuntimeFunction(program, row.libraryFunction()) : nullptr;
        if (held && held->function && program.at(held->function).hasBody())
            return held->function;
    }
    return { };
}

bool isLeafHandedToMethodOfLists(const Walker& walker, TypeID handedOver)
{
    if (!handedOver)
        return false;
    Type type = walker.types().at(handedOver);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return only && walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

bool mayBeLeafHandedToMethodOfLists(const Walker& walker, TypeID handedOver)
{
    if (!handedOver)
        return true;
    Type type = walker.types().at(handedOver);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return !only || walker.facts().linked().function(only).weightAsLeaf != LinkedFunction::notLeaf;
}

// The function object of such a built-in is the closure of that function, so the call enters the same code with the same operands, and spares the way through the value.
bool callLibraryFunctionBehindRows(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    FunctionID function = callee ? findLibraryFunctionBehindBuiltin(walker, walker.typeOf(callee)) : FunctionID();
    // `list.map(f)`: the rows name two texts, and what the function object runs takes whatever comes. Outside of loops it is called: copied at every such call, the code of a large application grew by a quarter.
    // In a loop the text repeats as the loop does. Where `f` calls nobody the rules of copies decide: `f` goes into the run over the vector, and no closure is made on the way through it.
    // `list.sort(f)`: the function object is native code, and the function that the rows name which these operands reach does what native code does on them.
    MayCopyFunctionCalledDirectly mayCopy = MayCopyFunctionCalledDirectly::Yes;
    bool isFoundByRowsReached = false;
    bool enteredByValue = false;
    if (!function && callee) {
        LibraryFunctionsBehindBuiltin behind = findLibraryFunctionsBehindBuiltin(walker, walker.typeOf(callee));
        if (behind.enteredByValue) {
            function = behind.enteredByValue;
            enteredByValue = true;
            Reference handedOver = call.argumentsOperands().empty() ? Reference() : walker.referenceOf(call.argumentsOperands()[0]);
            mayCopy = walker.isInLoop() && handedOver && isLeafHandedToMethodOfLists(walker, walker.typeOf(handedOver)) ? MayCopyFunctionCalledDirectly::Yes : MayCopyFunctionCalledDirectly::No;
        } else if (!behind.named.isEmpty())
            function = findLibraryFunctionOfRowsReachedBy(walker, call);
        isFoundByRowsReached = function && !behind.enteredByValue;
    }
    if (!function)
        return false;
    if (!directCallLiesWithinLooksForKnownRows(walker, call, function))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    // The rows that the call reaches are those of a list as its class makes it and a comparator that is a function.
    if (FunctionID withoutCalls = isFoundByRowsReached ? findSortThatCallsNoComparatorForKnownRows(walker, call, findBuiltinCalledForKnownRows(walker, call)) : FunctionID(); withoutCalls && directCallLiesWithinLooksForKnownRows(walker, call, withoutCalls)) {
        Reference listAndComparator[] { written.thisValueOperand(), written.argumentsOperands()[0] };
        if (callFunctionDirectly(walker, call, withoutCalls, Reference(), listAndComparator)) {
            walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
            walker.count(StatisticID::SortsThatCallNoComparator);
            return true;
        }
    }
    // The rows name the text on a list for the row of lists alone.
    FunctionID reached = enteredByValue && mayCopy == MayCopyFunctionCalledDirectly::Yes ? findLibraryFunctionOfRowsReachedBy(walker, call) : FunctionID();
    walker.noteThatCopyMakesNewList(reached && reached != function ? function : FunctionID(), written.thisValueOperand());
    bool isBuilt = callFunctionDirectly(walker, call, function, written.thisValueOperand(), written.argumentsOperands(), mayCopy);
    walker.noteThatCopyMakesNewList({ }, { });
    if (isBuilt)
        walker.count(StatisticID::LibraryFunctionsEnteredBehindRows);
    return isBuilt;
}

// ---- Reads

static bool mayCallFunctionOfAccessorForKnownRows(const BuiltinImplementation& implementation)
{
    switch (implementation.kind) {
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    case BuiltinImplementationKind::HostCall:
        return implementation.callee != BuiltinCallee::CustomGetter;
    case BuiltinImplementationKind::Inline:
        return implementation.otherwise.has_value();
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
    case BuiltinImplementationKind::OwnSlot:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// No location holds such a member that anybody could write. A closure is made with properties that a definition may replace, so of those the class says too little.
BuiltinID findOwnPropertyOfInstancesReadByRows(const Walker& walker, TypeID object, BuiltinClassID builtinClass, BuiltinName name)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!object || !builtinClass || !name || table.at(builtinClass).hasClosureInstances)
        return { };
    BuiltinID member = table.findOwnPropertyOfInstances(builtinClass, name);
    if (!member)
        return { };
    BuiltinID canonical = table.canonicalID(member);
    const Builtin& entry = table.at(canonical);
    if (entry.kind != BuiltinKind::DataProperty || !entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) || !table.dispatchOf(canonical, BuiltinCallMode::Get))
        return { };
    // A member that is a slot of the object itself, a number or a cell has no function to call: each row of the mode must do without one.
    if (!BuiltinCellKey::getter(canonical).hasCell(table)) {
        for (const BuiltinOverload& row : table.overloadsOf(canonical)) {
            if (row.mode == BuiltinCallMode::Get && mayCallFunctionOfAccessorForKnownRows(row.code.implementation))
                return { };
        }
    }
    return member;
}

bool isPrivateAliasOfBuiltinMember(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::EnginePrivateName)
        return false;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    return name && name.isPrivateAlias;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
