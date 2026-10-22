/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationsVerifier.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t unlisted = UINT32_MAX;
constexpr unsigned mostSubjects = 4096;

using Fates = OptionSet<ValueFate>;

struct RuleOfFate {
    ValueFate fate;
    RelationsRule rule;
};

constexpr RuleOfFate rulesOfFates[] = {
    { ValueFate::IsReturned, RelationsRule::IsReturned },
    { ValueFate::IsThrown, RelationsRule::IsThrown },
    { ValueFate::IsYielded, RelationsRule::IsYielded },
    { ValueFate::IsStoredInVariable, RelationsRule::IsStoredInVariable },
    { ValueFate::IsStoredInEnvironment, RelationsRule::IsStoredInEnvironment },
    { ValueFate::IsStoredInUnknownObject, RelationsRule::IsStoredInUnknownObject },
    { ValueFate::IsHandedToUnknownCode, RelationsRule::IsHandedToUnknownCode },
    { ValueFate::IsCalled, RelationsRule::IsCalled },
    { ValueFate::IsConstructed, RelationsRule::IsConstructed },
    { ValueFate::HasMembersRead, RelationsRule::HasMembersRead },
    { ValueFate::HasMembersWritten, RelationsRule::HasMembersWritten },
    { ValueFate::HasMembersDeletedOrDefined, RelationsRule::HasMembersDeletedOrDefined },
    { ValueFate::HasKeysEnumerated, RelationsRule::HasKeysEnumerated },
    { ValueFate::IsIterated, RelationsRule::IsIterated },
    { ValueFate::IsCompared, RelationsRule::IsCompared },
    { ValueFate::IsConvertedToPrimitive, RelationsRule::IsConvertedToPrimitive },
    { ValueFate::IsIncomplete, RelationsRule::IsIncomplete },
};

ASCIILiteral nameOf(RelationsRule rule)
{
    switch (rule) {
    case RelationsRule::IsReturned: return "IsReturned"_s;
    case RelationsRule::IsThrown: return "IsThrown"_s;
    case RelationsRule::IsYielded: return "IsYielded"_s;
    case RelationsRule::IsStoredInVariable: return "IsStoredInVariable"_s;
    case RelationsRule::IsStoredInEnvironment: return "IsStoredInEnvironment"_s;
    case RelationsRule::IsStoredInUnknownObject: return "IsStoredInUnknownObject"_s;
    case RelationsRule::IsHandedToUnknownCode: return "IsHandedToUnknownCode"_s;
    case RelationsRule::IsCalled: return "IsCalled"_s;
    case RelationsRule::IsConstructed: return "IsConstructed"_s;
    case RelationsRule::HasMembersRead: return "HasMembersRead"_s;
    case RelationsRule::HasMembersWritten: return "HasMembersWritten"_s;
    case RelationsRule::HasMembersDeletedOrDefined: return "HasMembersDeletedOrDefined"_s;
    case RelationsRule::HasKeysEnumerated: return "HasKeysEnumerated"_s;
    case RelationsRule::IsIterated: return "IsIterated"_s;
    case RelationsRule::IsCompared: return "IsCompared"_s;
    case RelationsRule::IsConvertedToPrimitive: return "IsConvertedToPrimitive"_s;
    case RelationsRule::IsIncomplete: return "IsIncomplete"_s;
    case RelationsRule::IsHandedToCode: return "IsHandedToCode"_s;
    case RelationsRule::IsStoredIntoOperand: return "IsStoredIntoOperand"_s;
    case RelationsRule::IsStoredIntoResult: return "IsStoredIntoResult"_s;
    case RelationsRule::ReadIsSubject: return "ReadIsSubject"_s;
    case RelationsRule::ContentsAreSubject: return "ContentsAreSubject"_s;
    case RelationsRule::StoreHasRow: return "StoreHasRow"_s;
    case RelationsRule::IsOperandOfUser: return "IsOperandOfUser"_s;
    case RelationsRule::KindFitsOperation: return "KindFitsOperation"_s;
    case RelationsRule::ResultIsListed: return "ResultIsListed"_s;
    case RelationsRule::StoreIsOnEveryPath: return "StoreIsOnEveryPath"_s;
    case RelationsRule::StoreIsBeforeTargetLeaves: return "StoreIsBeforeTargetLeaves"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

constexpr Fates fatesThatKeep {
    ValueFate::IsReturned,
    ValueFate::IsThrown,
    ValueFate::IsYielded,
    ValueFate::IsStoredInVariable,
    ValueFate::IsStoredInEnvironment,
    ValueFate::IsStoredInUnknownObject,
    ValueFate::IsHandedToUnknownCode,
};

constexpr Fates fatesOfWriting { ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined };

Fates fatesOfUses(const OperandInfo& info)
{
    constexpr OperandUses writes { OperandUse::WritesContents, OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::MovesElements };
    constexpr OperandUses defines { OperandUse::DeletesByKey, OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype };
    constexpr OperandUses calls { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };
    constexpr OperandUses reachesFurther { OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
    constexpr Protocols reads { Protocol::OwnValues, Protocol::PrototypeChain, Protocol::Named };
    constexpr Protocols runsCode { Protocol::Thenable, Protocol::Species, Protocol::ToJSON, Protocol::CallsNamed };

    Fates fates;
    auto add = [&](bool holds, Fates added) {
        if (holds)
            fates.add(added);
    };
    add(info.uses.contains(OperandUse::ReadsContents), ValueFate::HasMembersRead);
    add(info.uses.containsAny(writes), ValueFate::HasMembersWritten);
    add(info.uses.containsAny(defines), ValueFate::HasMembersDeletedOrDefined);
    add(info.uses.containsAny(calls), ValueFate::IsCalled);
    add(info.uses.contains(OperandUse::PassesToUserCode), ValueFate::IsHandedToUnknownCode);
    add(info.uses.containsAny(reachesFurther), ValueFate::IsIncomplete);
    add(info.protocols.contains(Protocol::OwnKeys), ValueFate::HasKeysEnumerated);
    add(info.protocols.contains(Protocol::Iterate), ValueFate::IsIterated);
    add(info.protocols.containsAny(reads), ValueFate::HasMembersRead);
    add(info.protocols.containsAny(runsCode), { ValueFate::IsHandedToUnknownCode, ValueFate::HasMembersRead });
    add(info.protocols.contains(Protocol::ToPrimitive), ValueFate::IsConvertedToPrimitive);
    add(info.protocols.contains(Protocol::HasInstance), ValueFate::IsIncomplete);
    return fates;
}

bool isCall(Opcode opcode)
{
    AccessClass accessClass = accessClassOf(opcode);
    return accessClass == AccessClass::IndirectCall || accessClass == AccessClass::DirectCall;
}

bool entersCode(Opcode opcode)
{
    return isCall(opcode) || opcode == Opcode::NewClosure;
}

bool isConstruction(Opcode opcode)
{
    return opcode == Opcode::Construct || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList;
}

bool hidesUsesOfParameters(Opcode opcode)
{
    switch (opcode) {
    case Opcode::CreateMappedArguments:
    case Opcode::CreateUnmappedArguments:
    case Opcode::GetMappedArgument:
    case Opcode::SetMappedArgument:
    case Opcode::DirectEval:
    case Opcode::ScopeHasBinding:
        return true;
    default:
        return false;
    }
}

std::optional<uint32_t> receiverOfRead(Opcode opcode)
{
    if (opcode == Opcode::Await || opcode == Opcode::IteratorResume || opcode == Opcode::GetContiguousIndex)
        return 0;
    if (accessOf(opcode) == Access::Read || interpretedOf(opcode) == Interpretation::Read)
        return receiverOf(opcode).value_or(0);
    return std::nullopt;
}

bool returnsOperand(const Value& value, uint32_t operandIndex)
{
    return operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns) || (value.opcode == Opcode::IteratorOpen && !operandIndex);
}

bool isNothingButItsOperand(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Convert:
    case Opcode::FinishObject:
    case Opcode::NewClass:
    case Opcode::FinishClass:
    case Opcode::PutInternalField:
    case Opcode::IteratorOpen:
        return true;
    default:
        return resultRuleOf(opcode) == ResultRule::Cast;
    }
}

bool anyDeclaredOperand(Opcode opcode, NOESCAPE const Invocable<bool(const OperandInfo&)> auto& functor)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (!info.isEngineOnly && functor(info))
            return true;
    }
    return false;
}

bool isConstant(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Absent:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::Builtin:
        return true;
    default:
        return false;
    }
}

enum class Origin : uint8_t { Nothing, Constant, Allocation, Call, Read, Computed, Other };

Origin originOf(Opcode opcode)
{
    if (opcode == Opcode::IteratorOpen || opcode == Opcode::ResultAt)
        return Origin::Nothing;
    if (opcode == Opcode::IteratorResume)
        return Origin::Read;
    ResultRule rule = resultRuleOf(opcode);
    if (rule == ResultRule::None)
        return Origin::Nothing;
    if (anyDeclaredOperand(opcode, [](const OperandInfo& info) { return info.uses.contains(OperandUse::Returns); }))
        return isNothingButItsOperand(opcode) ? Origin::Nothing : Origin::Other;
    if (isCall(opcode))
        return Origin::Call;
    if (isConstant(opcode))
        return Origin::Constant;
    if (boundToOf(opcode) == BoundTo::Site)
        return Origin::Other;
    if (rule == ResultRule::Allocation || flowOf(opcode) == ValueFlowEdgeClass::Allocates)
        return Origin::Allocation;
    if (anyDeclaredOperand(opcode, [](const OperandInfo& info) { return info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result; }))
        return Origin::Allocation;
    if (receiverOfRead(opcode))
        return Origin::Read;
    return rule == ResultRule::Natural ? Origin::Computed : Origin::Other;
}

bool retainsContentsOfAnOperand(Opcode opcode)
{
    return anyDeclaredOperand(opcode, [](const OperandInfo& info) { return info.uses.contains(OperandUse::RetainsContents); });
}

// A copy holds what its source holds, a literal of constants its constants: from birth, by no store that a path could miss.
bool isBornWithMoreThanItsOperands(Opcode opcode)
{
    if (retainsContentsOfAnOperand(opcode))
        return true;
    return resultRuleOf(opcode) == ResultRule::Constant && !anyDeclaredOperand(opcode, [](const OperandInfo&) { return true; });
}

bool asksAnOperandWhatAnInstanceIs(Opcode opcode)
{
    return anyDeclaredOperand(opcode, [](const OperandInfo& info) { return info.protocols.contains(Protocol::HasInstance); });
}

PropertyKeyID keyOf(const Value& value)
{
    return PayloadWord::key(value.opcode) ? value.key() : PropertyKeyID();
}

struct Place {
    uint32_t block { 0 };
    uint32_t position { 0 };
};

class RelationsOfFunction {
public:
    RelationsOfFunction(FunctionID id, const Function& function)
        : m_id(id)
        , m_function(function)
        , m_relations(function.relations)
    {
    }

    Vector<InternalError> run()
    {
        if (m_relations.subjects.isEmpty() || !isWellFormed())
            return { };
        sortRows();
        findWhatValuesHold();
        findValuesThatSomethingBecomesOf();
        checkKinds();
        checkRulesOfWholeFunction();
        checkUses();
        checkOrderOfStores();
        return WTF::move(m_errors);
    }

private:
    const RelationSubject& subject(uint32_t index) const { return m_relations.subjects[index]; }
    uint32_t numberOfSubjects() const { return m_relations.subjects.size(); }
    bool isIncomplete(uint32_t index) const { return subject(index).fate.contains(ValueFate::IsIncomplete); }
    const BitVector& held(ValueID value) const LIFETIME_BOUND { return m_held[value.index()]; }
    bool mayBeUnlisted(ValueID value) const { return m_mayBeUnlisted.get(value.index()); }
    bool mayBeOutsideSubjects(ValueID value) const { return held(value).isEmpty() || mayBeUnlisted(value); }
    bool isCoroutine() const { return m_function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }); }
    bool isThereForEverybody(uint32_t index) const { return subject(index).kind == RelationSubjectKind::Constant || subject(index).kind == RelationSubjectKind::Computed; }

    BitVector holdersIn(const BitVector& subjects) const
    {
        BitVector holders;
        forEachSubjectIn(subjects, [&](uint32_t index) {
            if (!isThereForEverybody(index))
                holders.set(index);
        });
        return holders;
    }

    static void forEachSubjectIn(const BitVector& subjects, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        for (size_t index : subjects)
            functor(static_cast<uint32_t>(index));
    }

    void fail(RelationsRule rule, BlockID block, ValueID value, String&& detail)
    {
        m_errors.append(InternalError { .kind = InternalErrorKind::UseWithoutRelation, .function = m_id, .block = block, .value = value, .count = static_cast<uint32_t>(rule), .detail = makeString(nameOf(rule), ": "_s, detail) });
    }

    void failUse(RelationsRule rule, uint32_t subjectIndex, ASCIILiteral user, uint32_t operandIndex)
    {
        fail(rule, m_block, m_user, makeString("subject "_s, subjectIndex, " is operand "_s, operandIndex, " of "_s, user));
    }

    void requireFates(Fates required, uint32_t subjectIndex, ASCIILiteral user, uint32_t operandIndex)
    {
        Fates missing = required - subject(subjectIndex).fate;
        if (missing.isEmpty())
            return;
        for (const RuleOfFate& entry : rulesOfFates) {
            if (missing.contains(entry.fate))
                failUse(entry.rule, subjectIndex, user, operandIndex);
        }
    }

    // A function may have thousands of rows, and each use asks for some.
    using Row = std::array<uint32_t, 4>;
    static Row rowOf(uint32_t source, uint32_t target, PropertyKeyID key, std::optional<uint32_t> creationOperand) { return { source, target, key ? key.index() : unlisted, creationOperand.value_or(unlisted) }; }

    void sortRows()
    {
        for (const StoredInto& store : m_relations.stores)
            m_stores.append(rowOf(store.source, store.target, store.key, store.creationOperand));
        for (const HandedTo& handed : m_relations.handed)
            m_handed.append(Row { handed.source, handed.call, handed.argument, 0 });
        for (const OperandOf& operand : m_relations.operands)
            m_operands.append(Row { operand.source, operand.user, operand.position, 0 });
        for (uint32_t index = 0; index < numberOfSubjects(); ++index) {
            if (subject(index).kind == RelationSubjectKind::ReadFrom)
                m_reads.append(Row { subject(index).index, subject(index).source, index, 0 });
        }
        for (Vector<Row>* rows : { &m_stores, &m_handed, &m_operands, &m_reads })
            std::ranges::sort(*rows);
    }

    bool hasStore(uint32_t source, uint32_t target, PropertyKeyID key, std::optional<uint32_t> creationOperand) const
    {
        return std::ranges::binary_search(m_stores, rowOf(source, target, key, creationOperand));
    }

    bool isHanded(uint32_t source, ValueID call, uint32_t operandIndex) const
    {
        return std::ranges::binary_search(m_handed, Row { source, static_cast<uint32_t>(call.index()), operandIndex, 0 });
    }

    bool isOperandOf(uint32_t source, uint32_t user, uint32_t position) const
    {
        return std::ranges::binary_search(m_operands, Row { source, user, position, 0 });
    }

    std::optional<uint32_t> findRead(uint32_t source, ValueID read) const
    {
        auto found = std::ranges::lower_bound(m_reads, Row { static_cast<uint32_t>(read.index()), source, 0, 0 });
        if (found == m_reads.end() || (*found)[0] != read.index() || (*found)[1] != source)
            return std::nullopt;
        return (*found)[2];
    }

    bool retainsAnOperandIn(const Value& user, uint32_t retaining) const
    {
        bool retains = false;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            const OperandInfo& info = operandInfoOf(user.opcode, operandIndex);
            retains |= operand && info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Operand && info.retainingOperand == retaining;
        });
        return retains;
    }

    bool isValueOfStep(const Value& resultAt) const
    {
        ValueID results = resultAt.operand(0);
        return results && m_function.at(results).opcode == Opcode::IteratorResume && resultAt.index() == static_cast<uint32_t>(IteratorResumeResult::Value);
    }

    bool isWellFormed() const;
    void findWhatValuesHold();
    bool saysSomethingOfOperand(const Value& user, uint32_t operandIndex) const;
    void findValuesThatSomethingBecomesOf();
    bool unite(ValueID into, const BitVector& subjects, bool mayBeUnlisted);
    bool followValue(ValueID);
    bool followEdges(BlockID);
    void checkKinds();
    void checkRulesOfWholeFunction();
    void checkUses();
    void checkMarkers(const Value& user, ValueID operand, uint32_t operandIndex);
    void checkWhatItIsBornWith(const Value&);
    void checkOperand(const Value& user, uint32_t operandIndex, uint32_t subjectIndex);
    Fates checkRetention(const OperandInfo&, const Value& user, uint32_t operandIndex, uint32_t subjectIndex);
    Fates checkAsHolder(const Value& user, uint32_t operandIndex, uint32_t subjectIndex);
    void checkTerminator(const Terminator&);

    void checkOrderOfStores();
    bool mayRunAgain(uint32_t block);
    bool isSurely(ValueID, uint32_t subjectIndex);
    bool makesStore(const Value&, const StoredInto&);
    bool letsLeave(const Value&, uint32_t subjectIndex) const;
    bool letsLeave(const Terminator&, uint32_t subjectIndex) const;
    enum class Goal : uint8_t { Return, Departure };
    bool reachesWithoutStore(Place start, const StoredInto&, Goal);

    FunctionID m_id;
    const Function& m_function;
    const FunctionRelations& m_relations;
    Vector<BitVector> m_held;
    Vector<Row> m_stores;
    Vector<Row> m_handed;
    Vector<Row> m_operands;
    Vector<Row> m_reads;
    BitVector m_mayBeUnlisted;
    BitVector m_somethingBecomesOf;
    Vector<uint32_t> m_ownSubject;
    Vector<Place> m_places;
    BlockID m_block;
    ValueID m_user;
    Vector<InternalError> m_errors;
};

bool RelationsOfFunction::isWellFormed() const
{
    if (numberOfSubjects() > mostSubjects)
        return false;
    uint32_t numberOfParameters = m_function.at(entryBlock).parameters.size();
    uint32_t numberOfValues = m_function.values.size();
    auto isSubjectOrUnlisted = [&](uint32_t index) {
        return index == unlisted || index < numberOfSubjects();
    };
    for (uint32_t index = 0; index < numberOfSubjects(); ++index) {
        const RelationSubject& listed = subject(index);
        if (listed.kind == RelationSubjectKind::Parameter) {
            if (listed.index >= numberOfParameters)
                return false;
            continue;
        }
        bool isUndefinedOfReturn = listed.kind == RelationSubjectKind::Constant && listed.index == unlisted;
        if (!isUndefinedOfReturn && listed.index >= numberOfValues)
            return false;
        if (listed.kind == RelationSubjectKind::ReadFrom && listed.source >= index)
            return false;
    }
    for (const StoredInto& store : m_relations.stores) {
        if (!isSubjectOrUnlisted(store.source) || store.target >= numberOfSubjects())
            return false;
    }
    for (const HandedTo& handed : m_relations.handed) {
        if (!isSubjectOrUnlisted(handed.source) || handed.call >= numberOfValues)
            return false;
    }
    for (const OperandOf& operand : m_relations.operands) {
        if (!isSubjectOrUnlisted(operand.source) || operand.user >= numberOfSubjects())
            return false;
    }
    return true;
}

bool RelationsOfFunction::unite(ValueID into, const BitVector& subjects, bool mayBeUnlisted)
{
    BitVector& known = m_held[into.index()];
    size_t numberKnown = known.bitCount();
    known.merge(subjects);
    bool hasChanged = known.bitCount() != numberKnown;
    if (mayBeUnlisted && !m_mayBeUnlisted.get(into.index())) {
        m_mayBeUnlisted.set(into.index());
        hasChanged = true;
    }
    return hasChanged;
}

bool RelationsOfFunction::followValue(ValueID id)
{
    const Value& value = m_function.at(id);
    BitVector subjects;
    bool isUnlisted = false;
    bool returnsAnOperand = false;
    auto take = [&](ValueID operand) {
        subjects.merge(held(operand));
        isUnlisted |= mayBeUnlisted(operand);
    };
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!operand || !returnsOperand(value, operandIndex))
            return;
        returnsAnOperand = true;
        take(operand);
    });

    if (value.opcode == Opcode::ResultAt) {
        if (isValueOfStep(value))
            take(value.operand(0));
        else
            isUnlisted = true;
    } else if (returnsAnOperand)
        isUnlisted |= !isNothingButItsOperand(value.opcode);
    else if (std::optional<uint32_t> receiver = receiverOfRead(value.opcode)) {
        ValueID base = value.operand(*receiver);
        isUnlisted |= !base || mayBeOutsideSubjects(base);
        if (base) {
            forEachSubjectIn(held(base), [&](uint32_t source) {
                isUnlisted |= !findRead(source, id);
            });
        }
    } else
        isUnlisted |= m_ownSubject[id.index()] == unlisted;
    return unite(id, subjects, isUnlisted);
}

bool RelationsOfFunction::followEdges(BlockID from)
{
    const Block& block = m_function.at(from);
    bool hasChanged = false;
    auto pass = [&](ValueID parameter, ValueID argument) {
        hasChanged |= argument ? unite(parameter, held(argument), mayBeUnlisted(argument)) : unite(parameter, { }, true);
    };
    auto passArguments = [&](const Target& target) {
        const Block& next = m_function.at(target.block);
        uint32_t position = 0;
        for (ValueID argument : target.arguments) {
            if (position < next.parameters.size())
                pass(next.parameters[position++], argument);
        }
        return position;
    };
    for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
        const Target& target = block.terminator.targets[targetIndex];
        const Block& next = m_function.at(target.block);
        uint32_t first = passArguments(target);
        for (uint32_t position = first; position < next.parameters.size(); ++position) {
            std::optional<EdgeResult> result = tryGetEdgeResult(m_function, { from, targetIndex, position - first });
            pass(next.parameters[position], result && result->kind == EdgeResultKind::NarrowedOperand ? result->subject : ValueID());
        }
    }
    if (block.handler) {
        const Block& next = m_function.at(block.handler->block);
        for (uint32_t position = passArguments(*block.handler); position < next.parameters.size(); ++position)
            pass(next.parameters[position], { });
    }
    return hasChanged;
}

void RelationsOfFunction::findWhatValuesHold()
{
    uint32_t numberOfValues = m_function.values.size();
    m_held.fill(BitVector(), numberOfValues);
    m_mayBeUnlisted.ensureSize(numberOfValues);
    m_ownSubject.fill(unlisted, numberOfValues);
    m_places.fill(Place { }, numberOfValues);

    const Block& entry = m_function.at(entryBlock);
    for (uint32_t index = 0; index < numberOfSubjects(); ++index) {
        const RelationSubject& listed = subject(index);
        if (listed.kind == RelationSubjectKind::Parameter) {
            m_held[entry.parameters[listed.index].index()].set(index);
            continue;
        }
        if (listed.index == unlisted)
            continue;
        if (listed.kind != RelationSubjectKind::ReadFrom)
            m_ownSubject[listed.index] = index;
        else if (!receiverOfRead(m_function.at(ValueID(listed.index)).opcode))
            continue;
        m_held[listed.index].set(index);
    }
    for (ValueID parameter : entry.parameters) {
        if (held(parameter).isEmpty())
            m_mayBeUnlisted.set(parameter.index());
    }
    for (uint32_t block = 0; block < m_function.blocks.size(); ++block) {
        uint32_t position = 0;
        for (ValueID id : m_function.blocks[block].values)
            m_places[id.index()] = { block, position++ };
    }
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t block = 0; block < m_function.blocks.size(); ++block) {
            for (ValueID id : m_function.blocks[block].values)
                hasChanged |= followValue(id);
            hasChanged |= followEdges(BlockID(block));
        }
    }
}

bool RelationsOfFunction::saysSomethingOfOperand(const Value& user, uint32_t operandIndex) const
{
    Opcode opcode = user.opcode;
    const OperandInfo& info = operandInfoOf(opcode, operandIndex);
    if (entersCode(opcode) || receiverOfRead(opcode) == operandIndex || provesOf(opcode) == Proof::Equality || asksAnOperandWhatAnInstanceIs(opcode))
        return true;
    if (info.uses.containsAny({ OperandUse::Retains, OperandUse::RetainsContents }) || !fatesOfUses(info).isEmpty())
        return true;
    return originOf(opcode) == Origin::Other && !info.uses.contains(OperandUse::Returns);
}

// The result of a call that nothing becomes of has no place in a record, and nothing is lost with it.
void RelationsOfFunction::findValuesThatSomethingBecomesOf()
{
    m_somethingBecomesOf.ensureSize(m_function.values.size());
    auto mark = [&](ValueID value) {
        return value && !m_somethingBecomesOf.set(value.index());
    };
    auto isMarked = [&](ValueID value) {
        return m_somethingBecomesOf.get(value.index());
    };
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t index = m_function.blocks.size(); index--;) {
            const Block& block = m_function.blocks[index];
            const Terminator& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::ReturnCall || terminator.kind == TerminatorKind::Throw || terminator.kind == TerminatorKind::Rethrow)
                hasChanged |= terminator.numberOfOperands() && mark(terminator.operand(0));
            auto followArguments = [&](const Target& target) {
                const Block& next = m_function.at(target.block);
                for (uint32_t position = 0; position < target.arguments.size() && position < next.parameters.size(); ++position) {
                    if (isMarked(next.parameters[position]))
                        hasChanged |= mark(target.arguments[position]);
                }
            };
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                const Target& target = terminator.targets[targetIndex];
                const Block& next = m_function.at(target.block);
                followArguments(target);
                uint32_t first = target.arguments.size();
                for (uint32_t position = first; position < next.parameters.size(); ++position) {
                    if (!isMarked(next.parameters[position]))
                        continue;
                    std::optional<EdgeResult> result = tryGetEdgeResult(m_function, { BlockID(index), targetIndex, position - first });
                    if (result && result->kind == EdgeResultKind::NarrowedOperand)
                        hasChanged |= mark(result->subject);
                }
            }
            if (block.handler)
                followArguments(*block.handler);
            for (uint32_t position = block.values.size(); position--;) {
                ValueID id = block.values[position];
                const Value& value = m_function.at(id);
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (!operand)
                        return;
                    bool passesOn = returnsOperand(value, operandIndex) || value.opcode == Opcode::ResultAt;
                    if (saysSomethingOfOperand(value, operandIndex) || (passesOn && isMarked(id)))
                        hasChanged |= mark(operand);
                });
            }
        }
    }
}

void RelationsOfFunction::checkKinds()
{
    for (uint32_t index = 0; index < numberOfSubjects(); ++index) {
        const RelationSubject& listed = subject(index);
        if (listed.kind == RelationSubjectKind::Parameter || listed.index == unlisted)
            continue;
        const Value& value = m_function.at(ValueID(listed.index));
        bool fits = false;
        switch (listed.kind) {
        case RelationSubjectKind::Parameter:
            break;
        case RelationSubjectKind::Allocation:
            fits = originOf(value.opcode) == Origin::Allocation;
            break;
        case RelationSubjectKind::CallResult:
            fits = originOf(value.opcode) == Origin::Call;
            break;
        case RelationSubjectKind::ReadFrom:
            fits = true;
            break;
        case RelationSubjectKind::Constant:
            fits = originOf(value.opcode) == Origin::Constant;
            break;
        case RelationSubjectKind::Computed:
            fits = originOf(value.opcode) == Origin::Computed;
            break;
        }
        if (!fits)
            fail(RelationsRule::KindFitsOperation, { }, ValueID(listed.index), makeString("subject "_s, index, " is "_s, Typed::nameOf(value.opcode)));
    }
}

void RelationsOfFunction::checkRulesOfWholeFunction()
{
    bool hidesUses = false;
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values)
            hidesUses |= hidesUsesOfParameters(m_function.at(id).opcode);
    }
    for (uint32_t index = 0; index < numberOfSubjects(); ++index) {
        if (hidesUses && !isIncomplete(index))
            fail(RelationsRule::IsIncomplete, { }, { }, makeString("subject "_s, index, " can be reached under names that no operand shows"_s));
        if (isCoroutine() && subject(index).kind == RelationSubjectKind::Parameter && !isIncomplete(index) && !subject(index).fate.contains(ValueFate::IsStoredInUnknownObject))
            fail(RelationsRule::IsStoredInUnknownObject, { }, { }, makeString("subject "_s, index, " is a parameter of a coroutine"_s));
    }
    if (hidesUses && m_relations.resultsFullyListed)
        fail(RelationsRule::ResultIsListed, { }, { }, "values can be reached under names that no operand shows"_str);
}

Fates RelationsOfFunction::checkRetention(const OperandInfo& info, const Value& user, uint32_t operandIndex, uint32_t subjectIndex)
{
    ASCIILiteral name = Typed::nameOf(user.opcode);
    BitVector targets;
    bool mayBeKeptElsewhere = true;
    bool isInResult = info.retainedIn == RetainedIn::Result;
    if (isInResult) {
        uint32_t made = m_ownSubject[m_user.index()];
        if (made != unlisted) {
            targets.set(made);
            mayBeKeptElsewhere = false;
        }
    } else if (info.retainedIn == RetainedIn::Operand && info.retainingOperand) {
        if (ValueID holder = user.operand(*info.retainingOperand)) {
            targets = holdersIn(held(holder));
            mayBeKeptElsewhere = targets.isEmpty() || targets != held(holder) || mayBeUnlisted(holder);
        }
    }
    forEachSubjectIn(targets, [&](uint32_t target) {
        if (isInResult) {
            if (!hasStore(subjectIndex, target, { }, operandIndex))
                failUse(RelationsRule::IsStoredIntoResult, subjectIndex, name, operandIndex);
        } else if (!hasStore(subjectIndex, target, keyOf(user), std::nullopt))
            failUse(RelationsRule::IsStoredIntoOperand, subjectIndex, name, operandIndex);
    });
    if (!mayBeKeptElsewhere)
        return { };
    switch (user.opcode) {
    case Opcode::Yield:
        return ValueFate::IsYielded;
    case Opcode::PutModuleVariable:
        return ValueFate::IsStoredInVariable;
    case Opcode::PutEnvironmentField:
        return ValueFate::IsStoredInEnvironment;
    default:
        return ValueFate::IsStoredInUnknownObject;
    }
}

Fates RelationsOfFunction::checkAsHolder(const Value& user, uint32_t operandIndex, uint32_t subjectIndex)
{
    Fates required;
    if (isThereForEverybody(subjectIndex))
        return required;
    user.forEachOperand([&](ValueID stored, uint32_t storedIndex) {
        const OperandInfo& info = operandInfoOf(user.opcode, storedIndex);
        if (!stored || !info.uses.contains(OperandUse::Retains) || info.retainedIn != RetainedIn::Operand || info.retainingOperand != operandIndex)
            return;
        if (user.opcode != Opcode::PutEnvironmentField)
            required.add(accessOf(user.opcode) == Access::Define ? ValueFate::HasMembersDeletedOrDefined : ValueFate::HasMembersWritten);
        if (mayBeOutsideSubjects(stored) && !hasStore(unlisted, subjectIndex, keyOf(user), std::nullopt))
            failUse(RelationsRule::StoreHasRow, subjectIndex, Typed::nameOf(user.opcode), operandIndex);
    });
    return required;
}

void RelationsOfFunction::checkOperand(const Value& user, uint32_t operandIndex, uint32_t subjectIndex)
{
    if (isIncomplete(subjectIndex))
        return;
    Opcode opcode = user.opcode;
    ASCIILiteral name = Typed::nameOf(opcode);
    const OperandInfo& info = operandInfoOf(opcode, operandIndex);
    Fates required = fatesOfUses(info);
    bool isReceiverOfRead = receiverOfRead(opcode) == operandIndex;
    if (isReceiverOfRead && opcode != Opcode::GetEnvironmentField)
        required.add(ValueFate::HasMembersRead);
    if (provesOf(opcode) == Proof::Equality)
        required.add(ValueFate::IsCompared);
    if (asksAnOperandWhatAnInstanceIs(opcode) && !info.protocols.contains(Protocol::HasInstance))
        required.add(ValueFate::IsHandedToUnknownCode);

    if (entersCode(opcode)) {
        if (m_ownSubject[m_user.index()] == unlisted && (opcode == Opcode::NewClosure || m_somethingBecomesOf.get(m_user.index())))
            required.add(ValueFate::IsIncomplete);
        if (required.contains(ValueFate::IsCalled) && isConstruction(opcode)) {
            required.remove(ValueFate::IsCalled);
            required.add(ValueFate::IsConstructed);
        }
        if (!isHanded(subjectIndex, m_user, operandIndex))
            failUse(RelationsRule::IsHandedToCode, subjectIndex, name, operandIndex);
    } else {
        if (info.uses.contains(OperandUse::Retains) && opcode != Opcode::IteratorOpen)
            required.add(checkRetention(info, user, operandIndex, subjectIndex));
        bool mayBeReadOut = originOf(opcode) == Origin::Other && !info.uses.contains(OperandUse::Returns);
        if (mayBeReadOut)
            required.add(ValueFate::HasMembersRead);
        if (mayBeReadOut || info.uses.contains(OperandUse::RetainsContents)) {
            // What is taken out goes where nobody follows it, or into the object that the operation makes: a copy holds from its birth what its source holds.
            std::optional<uint32_t> contents = findRead(subjectIndex, m_user);
            uint32_t made = m_ownSubject[m_user.index()];
            bool isHeldByCopy = contents && made != unlisted && hasStore(*contents, made, { }, std::nullopt);
            if (!contents || (!isIncomplete(*contents) && !isHeldByCopy))
                failUse(RelationsRule::ContentsAreSubject, subjectIndex, name, operandIndex);
        }
        if (required.containsAny(fatesOfWriting) && !retainsAnOperandIn(user, operandIndex) && !hasStore(unlisted, subjectIndex, keyOf(user), std::nullopt))
            failUse(RelationsRule::StoreHasRow, subjectIndex, name, operandIndex);
        required.add(checkAsHolder(user, operandIndex, subjectIndex));
    }
    if (isReceiverOfRead && !findRead(subjectIndex, m_user))
        failUse(RelationsRule::ReadIsSubject, subjectIndex, name, operandIndex);
    // `{ a: p, n: 2 }`: a data property defined under a name looks nothing up, finds no setter and puts no accessor anywhere. On an object that the function has made itself the walk may call that a store.
    if (opcode == Opcode::PropDefine && keyOf(user) && subject(subjectIndex).kind == RelationSubjectKind::Allocation && required.contains(ValueFate::HasMembersDeletedOrDefined)
        && !subject(subjectIndex).fate.contains(ValueFate::HasMembersDeletedOrDefined)) {
        required.remove(ValueFate::HasMembersDeletedOrDefined);
        required.add(ValueFate::HasMembersWritten);
    }
    requireFates(required, subjectIndex, name, operandIndex);
}

void RelationsOfFunction::checkMarkers(const Value& user, ValueID operand, uint32_t operandIndex)
{
    ASCIILiteral name = Typed::nameOf(user.opcode);
    bool needsMarker = !held(operand).isEmpty() && mayBeUnlisted(operand);
    if (entersCode(user.opcode)) {
        if (needsMarker && !isHanded(unlisted, m_user, operandIndex))
            fail(RelationsRule::IsHandedToCode, m_block, m_user, makeString("what is no subject may be operand "_s, operandIndex, " of "_s, name));
        return;
    }
    std::optional<uint32_t> receiver = receiverOfRead(user.opcode);
    if (receiver == operandIndex)
        return;
    BitVector users;
    uint32_t own = m_ownSubject[m_user.index()];
    if (own != unlisted && subject(own).kind == RelationSubjectKind::Computed)
        users.set(own);
    else if (receiver) {
        forEachSubjectIn(held(m_user), [&](uint32_t read) {
            if (subject(read).kind == RelationSubjectKind::ReadFrom && subject(read).index == m_user.index())
                users.set(read);
        });
    }
    forEachSubjectIn(users, [&](uint32_t listedUser) {
        forEachSubjectIn(held(operand), [&](uint32_t source) {
            if (!isOperandOf(source, listedUser, operandIndex))
                failUse(RelationsRule::IsOperandOfUser, source, name, operandIndex);
        });
        if (needsMarker && !isOperandOf(unlisted, listedUser, operandIndex))
            fail(RelationsRule::IsOperandOfUser, m_block, m_user, makeString("what is no subject may be operand "_s, operandIndex, " of "_s, name));
    });
}

void RelationsOfFunction::checkTerminator(const Terminator& terminator)
{
    m_user = { };
    ASCIILiteral name = Typed::nameOf(terminator.kind);
    bool isReturn = terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::ReturnCall;
    bool isThrow = terminator.kind == TerminatorKind::Throw || terminator.kind == TerminatorKind::Rethrow;
    if (!isReturn && !isThrow)
        return;
    ValueID operand = terminator.numberOfOperands() ? terminator.operand(0) : ValueID();
    if (operand) {
        forEachSubjectIn(held(operand), [&](uint32_t subjectIndex) {
            if (isIncomplete(subjectIndex))
                return;
            Fates required = ValueFate::IsThrown;
            if (isReturn) {
                required = ValueFate::IsReturned;
                if (isCoroutine())
                    required.add({ ValueFate::IsStoredInUnknownObject, ValueFate::HasMembersRead });
                if (subject(subjectIndex).fate.contains(ValueFate::IsReturned) && !(subject(subjectIndex).results & 1))
                    failUse(RelationsRule::IsReturned, subjectIndex, name, 0);
            }
            requireFates(required, subjectIndex, name, 0);
        });
    }
    if (!isReturn || !m_relations.resultsFullyListed)
        return;
    if (operand) {
        if (mayBeOutsideSubjects(operand))
            fail(RelationsRule::ResultIsListed, m_block, operand, makeString("returned by "_s, name));
        return;
    }
    bool listsUndefined = m_relations.subjects.containsIf([](const RelationSubject& listed) {
        return listed.kind == RelationSubjectKind::Constant && listed.index == unlisted && listed.fate.contains(ValueFate::IsReturned);
    });
    if (!listsUndefined)
        fail(RelationsRule::ResultIsListed, m_block, { }, "undefined is returned"_str);
}

// The rows into a listed Allocation are all that is written to it. So what it holds from birth besides its operands has rows too:
// a copy holds what is read out of each thing that its source may be, or something that is not listed; a literal of constants the latter.
void RelationsOfFunction::checkWhatItIsBornWith(const Value& made)
{
    uint32_t subjectIndex = m_ownSubject[m_user.index()];
    if (subjectIndex == unlisted || subject(subjectIndex).kind != RelationSubjectKind::Allocation || isIncomplete(subjectIndex))
        return;
    if (entersCode(made.opcode) || !isBornWithMoreThanItsOperands(made.opcode) || hasStore(unlisted, subjectIndex, { }, std::nullopt))
        return;
    bool isCovered = retainsContentsOfAnOperand(made.opcode);
    made.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!operand || !operandInfoOf(made.opcode, operandIndex).uses.contains(OperandUse::RetainsContents))
            return;
        isCovered &= !mayBeOutsideSubjects(operand);
        forEachSubjectIn(held(operand), [&](uint32_t source) {
            std::optional<uint32_t> contents = findRead(source, m_user);
            isCovered &= contents && hasStore(*contents, subjectIndex, { }, std::nullopt);
        });
    });
    if (!isCovered)
        fail(RelationsRule::StoreHasRow, m_block, m_user, makeString("subject "_s, subjectIndex, " is born with more than its rows say"_s));
}

void RelationsOfFunction::checkUses()
{
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        m_block = BlockID(index);
        const Block& block = m_function.blocks[index];
        for (ValueID id : block.values) {
            m_user = id;
            const Value& user = m_function.at(id);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!operand)
                    return;
                forEachSubjectIn(held(operand), [&](uint32_t subjectIndex) {
                    checkOperand(user, operandIndex, subjectIndex);
                });
                checkMarkers(user, operand, operandIndex);
            });
            checkWhatItIsBornWith(user);
        }
        checkTerminator(block.terminator);
    }
}

bool RelationsOfFunction::mayRunAgain(uint32_t start)
{
    BitVector seen;
    Vector<uint32_t, 16> pending { start };
    while (!pending.isEmpty()) {
        bool comesBack = false;
        m_function.blocks[pending.takeLast()].forEachTarget([&](const Target& target) {
            uint32_t next = target.block.index();
            comesBack |= next == start;
            if (!seen.set(next))
                pending.append(next);
        });
        if (comesBack)
            return true;
    }
    return false;
}

bool RelationsOfFunction::isSurely(ValueID value, uint32_t subjectIndex)
{
    if (!value || !held(value).get(subjectIndex) || held(value).bitCount() != 1 || mayBeUnlisted(value))
        return false;
    const RelationSubject& listed = subject(subjectIndex);
    if (listed.kind == RelationSubjectKind::Parameter)
        return true;
    if (!mayRunAgain(m_places[listed.index].block))
        return true;
    while (value.index() != listed.index) {
        const Value& copy = m_function.at(value);
        if (copy.opcode == Opcode::Parameter || !copy.numberOfOperands() || !isNothingButItsOperand(copy.opcode))
            return false;
        ValueID copied;
        copy.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operand && returnsOperand(copy, operandIndex))
                copied = operand;
        });
        if (!copied)
            return false;
        value = copied;
    }
    return true;
}

bool RelationsOfFunction::makesStore(const Value& value, const StoredInto& store)
{
    if (entersCode(value.opcode) || value.opcode == Opcode::IteratorOpen || keyOf(value) != store.key)
        return false;
    bool makes = false;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!operand || makes)
            return;
        const OperandInfo& info = operandInfoOf(value.opcode, operandIndex);
        if (info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Operand && info.retainingOperand) {
            bool isSource = store.source == unlisted ? mayBeOutsideSubjects(operand) : held(operand).get(store.source);
            makes = isSource && isSurely(value.operand(*info.retainingOperand), store.target);
            return;
        }
        if (store.source == unlisted && fatesOfUses(info).containsAny(fatesOfWriting) && !retainsAnOperandIn(value, operandIndex))
            makes = isSurely(operand, store.target);
    });
    return makes;
}

bool RelationsOfFunction::letsLeave(const Value& value, uint32_t subjectIndex) const
{
    bool leaves = false;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!operand || !held(operand).get(subjectIndex))
            return;
        const OperandInfo& info = operandInfoOf(value.opcode, operandIndex);
        leaves |= entersCode(value.opcode) || (info.uses.contains(OperandUse::Retains) && value.opcode != Opcode::IteratorOpen) || fatesOfUses(info).containsAny(fatesThatKeep);
    });
    return leaves;
}

bool RelationsOfFunction::letsLeave(const Terminator& terminator, uint32_t subjectIndex) const
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        break;
    default:
        return false;
    }
    ValueID operand = terminator.numberOfOperands() ? terminator.operand(0) : ValueID();
    return operand && held(operand).get(subjectIndex);
}

bool RelationsOfFunction::reachesWithoutStore(Place start, const StoredInto& store, Goal goal)
{
    BitVector seen;
    Vector<Place, 16> pending { start };
    while (!pending.isEmpty()) {
        Place place = pending.takeLast();
        const Block& block = m_function.blocks[place.block];
        auto enter = [&](const Target& target) {
            if (!seen.set(target.block.index()))
                pending.append(Place { static_cast<uint32_t>(target.block.index()), 0 });
        };
        if (block.handler)
            enter(*block.handler);
        bool isStored = false;
        for (uint32_t position = place.position; position < block.values.size() && !isStored; ++position) {
            const Value& value = m_function.at(block.values[position]);
            if (goal == Goal::Departure && letsLeave(value, store.target))
                return true;
            isStored = makesStore(value, store);
        }
        if (isStored)
            continue;
        const Terminator& terminator = block.terminator;
        if (goal == Goal::Departure ? letsLeave(terminator, store.target) : terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::ReturnCall)
            return true;
        for (const Target& target : terminator.targets)
            enter(target);
    }
    return false;
}

void RelationsOfFunction::checkOrderOfStores()
{
    for (uint32_t row = 0; row < m_relations.stores.size(); ++row) {
        const StoredInto& store = m_relations.stores[row];
        if (store.creationOperand || (!store.isOnEveryPath && !store.isBeforeTargetLeaves) || isIncomplete(store.target))
            continue;
        const RelationSubject& target = subject(store.target);
        if (target.kind != RelationSubjectKind::Parameter && target.index == unlisted)
            continue;
        if (target.kind == RelationSubjectKind::Allocation && !store.key && isBornWithMoreThanItsOperands(m_function.at(ValueID(target.index)).opcode))
            continue;
        Place start;
        if (target.kind != RelationSubjectKind::Parameter) {
            start = m_places[target.index];
            ++start.position;
        }
        if (store.isOnEveryPath && reachesWithoutStore(start, store, Goal::Return))
            fail(RelationsRule::StoreIsOnEveryPath, { }, { }, makeString("store "_s, row));
        if (store.isBeforeTargetLeaves && reachesWithoutStore(start, store, Goal::Departure))
            fail(RelationsRule::StoreIsBeforeTargetLeaves, { }, { }, makeString("store "_s, row));
    }
}

} // anonymous namespace

Vector<InternalError> findErrorsInRelations(FunctionID id, const Function& function)
{
    if (!function.hasBody())
        return { };
    return RelationsOfFunction(id, function).run();
}

bool verifyRelations(const Program& program, BuildContext& build)
{
    std::atomic<bool> isValid { true };
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        Vector<InternalError> errors = findErrorsInRelations(FunctionID(index), program.functions[index]);
        if (errors.isEmpty())
            return;
        isValid.store(false, std::memory_order_relaxed);
        build.diagnostics().append(WTF::move(errors));
    });
    return isValid.load(std::memory_order_relaxed);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
