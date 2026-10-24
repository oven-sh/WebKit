/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliningNormalForm.h"

#if ENABLE(AOT)

#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static bool dependsOnModule(Opcode opcode)
{
    return std::ranges::any_of(payloadFieldsOf(opcode), [](const PayloadFieldInfo& field) {
        return field.kind == PayloadKind::Origin;
    });
}

static uint64_t findWordsOfAccessSites(Opcode opcode)
{
    uint64_t words = 0;
    for (const PayloadFieldInfo& field : payloadFieldsOf(opcode)) {
        if (field.kind != PayloadKind::AccessSite)
            continue;
        RELEASE_ASSERT(field.firstWord < 64);
        words |= uint64_t { 1 } << field.firstWord;
    }
    return words;
}

template<typename Functor>
static void forEachPayloadWordWithoutAccessSite(const Value& value, const Functor& functor)
{
    uint64_t wordsOfAccessSites = findWordsOfAccessSites(value.opcode);
    unsigned position = 0;
    auto visit = [&](uint32_t word) {
        bool isAccessSite = position < 64 && (wordsOfAccessSites >> position & 1);
        ++position;
        functor(isAccessSite ? absentPayloadWord : word);
    };
    for (uint32_t word : value.inlinePayload())
        visit(word);
    functor(static_cast<uint32_t>(value.outOfLinePayload().size()));
    for (uint32_t word : value.outOfLinePayload())
        visit(word);
}

static void addPayloadWithoutAccessSite(Hasher& hasher, const Value& value)
{
    add(hasher, static_cast<uint16_t>(value.flags));
    forEachPayloadWordWithoutAccessSite(value, [&](uint32_t word) {
        add(hasher, word);
    });
}

static bool haveSamePayloadWithoutAccessSite(const Value& first, const Value& second)
{
    if (first.flags != second.flags || first.outOfLinePayload().size() != second.outOfLinePayload().size())
        return false;
    Vector<uint32_t, 16> wordsOfFirst;
    forEachPayloadWordWithoutAccessSite(first, [&](uint32_t word) {
        wordsOfFirst.append(word);
    });
    size_t position = 0;
    bool areSame = true;
    forEachPayloadWordWithoutAccessSite(second, [&](uint32_t word) {
        areSame &= wordsOfFirst[position++] == word;
    });
    return areSame;
}

static bool readsConditionOperands(const Terminator& terminator)
{
    if (!terminator.isConditionalBranch())
        return false;
    return terminator.kind == TerminatorKind::TestAndBranch || std::ranges::any_of(terminator.targets, [](const Target& target) {
        return !target.proofs.isEmpty();
    });
}

OutliningNormalForm::OutliningNormalForm(const Function& function, const OutliningSite& site, uint64_t maxLiveIns)
    : m_function(function)
    , m_site(site)
    , m_maxLiveIns(maxLiveIns)
{
    for (ValueID id : entryValuesOf(function, site))
        appendValue(id);
    if (site.isRegion()) {
        appendTerminator(function.at(site.entryBlock).terminator);
        for (BlockID blockID : site.interiorBlocks) {
            const Block& block = function.at(blockID);
            for (ValueID parameter : block.parameters)
                appendParameter(parameter);
            for (ValueID id : block.values)
                appendValue(id);
            appendTerminator(block.terminator);
        }
    }
    appendMark(Mark::Result);
    appendWord(site.rootIsUsed);
    // A constant leaves nothing in the form where it stands, and it may be what the run answers.
    if (site.rootIsUsed && isRematerializable(function.at(site.root)))
        appendReference(site.root);
    appendWord(site.resultType.index());
}

void OutliningNormalForm::appendValue(ValueID id)
{
    const Value& value = m_function.at(id);
    if (isRematerializable(value))
        return;

    appendMark(Mark::Value);
    appendWord(static_cast<uint32_t>(value.opcode));
    appendWord(value.type.index());
    if (dependsOnModule(value.opcode))
        appendWord(m_function.module.index());
    m_payloads.append(&value);

    uint32_t numberOfOperands = value.numberOfOperands();
    appendWord(numberOfOperands);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        Reference reference = appendReference(value.operand(operandIndex));
        bool isOutside = reference == Reference::LiveIn || reference == Reference::Constant;
        if (isOutside && mustBeDefinition(value.operandKind(operandIndex)))
            m_hasRequiredDefinitionOutside = true;
    }
    m_definitions.append(id);
}

void OutliningNormalForm::appendParameter(ValueID id)
{
    appendMark(Mark::Parameter);
    appendWord(m_function.at(id).type.index());
    m_definitions.append(id);
}

void OutliningNormalForm::appendTerminator(const Terminator& terminator)
{
    appendMark(Mark::Terminator);
    appendWord(static_cast<uint32_t>(terminator.kind));

    bool needsConditionInSite = readsConditionOperands(terminator);
    uint32_t numberOfOperands = terminator.numberOfOperands();
    appendWord(numberOfOperands);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        Reference reference = appendReference(terminator.operand(operandIndex));
        bool isOutside = reference == Reference::LiveIn || reference == Reference::Constant;
        if (isOutside && (needsConditionInSite || mustBeDefinition(terminator.operandKind(operandIndex))))
            m_hasRequiredDefinitionOutside = true;
    }

    appendWord(terminator.flags);
    std::array<uint32_t, numberOfInlinePayloadWords> inlinePayload = terminator.inlinePayload();
    appendCount(inlinePayload.size());
    m_words.append(std::span<const uint32_t> { inlinePayload });
    appendCount(terminator.outOfLinePayload().size());
    m_words.append(terminator.outOfLinePayload());

    appendCount(terminator.targets.size());
    for (const Target& target : terminator.targets)
        appendTarget(target);
}

void OutliningNormalForm::appendTarget(const Target& target)
{
    if (target.block == m_site.exitBlock) {
        appendMark(Mark::ExitTarget);
        if (m_site.exitArgument)
            appendReference(target.arguments[*m_site.exitArgument]);
        return;
    }

    size_t interiorIndex = m_site.interiorBlocks.find(target.block);
    RELEASE_ASSERT(interiorIndex != notFound);
    appendMark(Mark::InteriorTarget);
    appendCount(interiorIndex);
    appendCount(target.arguments.size());
    for (ValueID argument : target.arguments)
        appendReference(argument);

    appendMark(Mark::Proof);
    appendCount(target.proofs.size());
    m_words.append(target.proofs.span());
}

OutliningNormalForm::Reference OutliningNormalForm::appendReference(ValueID id)
{
    if (!id) {
        appendMark(Mark::Absent);
        return Reference::Absent;
    }

    const Value& value = m_function.at(id);
    if (isRematerializable(value)) {
        appendMark(Mark::Constant);
        appendWord(static_cast<uint32_t>(value.opcode));
        appendWord(value.type.index());
        m_payloads.append(&value);
        return Reference::Constant;
    }

    size_t ordinal = m_definitions.find(id);
    if (ordinal != notFound) {
        appendMark(Mark::InSite);
        appendCount(ordinal);
        return Reference::InSite;
    }

    appendMark(Mark::LiveIn);
    appendCount(liveInIndex(id));
    appendWord(value.type.index());
    return Reference::LiveIn;
}

size_t OutliningNormalForm::liveInIndex(ValueID id)
{
    size_t index = m_liveIns.find(id);
    if (index != notFound)
        return index;
    index = m_liveIns.size();
    if (index <= m_maxLiveIns)
        m_liveIns.append(id);
    return index;
}

unsigned OutliningNormalForm::hash() const
{
    Hasher hasher;
    for (uint32_t word : m_words)
        add(hasher, word);
    for (const Value* payload : m_payloads)
        addPayloadWithoutAccessSite(hasher, *payload);
    return hasher.hash();
}

bool OutliningNormalForm::isEqualTo(const OutliningNormalForm& other) const
{
    return m_words == other.m_words && std::ranges::equal(m_payloads, other.m_payloads, [](const Value* payload, const Value* otherPayload) {
        return haveSamePayloadWithoutAccessSite(*payload, *otherPayload);
    });
}

bool areEqualSites(const Program& program, const OutliningSite& first, const OutliningSite& second)
{
    if (first.hash != second.hash)
        return false;
    OutliningNormalForm firstForm(program.at(first.function), first, first.liveIns.size());
    OutliningNormalForm secondForm(program.at(second.function), second, second.liveIns.size());
    ASSERT(firstForm.liveIns() == first.liveIns && secondForm.liveIns() == second.liveIns);
    return firstForm.isEqualTo(secondForm);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
