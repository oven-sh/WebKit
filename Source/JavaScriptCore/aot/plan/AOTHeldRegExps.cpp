/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeldRegExps.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t holdsNothingYet = 0;
constexpr uint32_t holdsAnything = 1;
constexpr uint32_t firstLiteral = 2;

struct CapturePassedOn {
    uint32_t to;
    uint32_t from;
};

struct FunctionsWithClosuresOfOperation {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id && ((role == FunctionReferenceKind::Closure) || ...))
                functions.append(id);
        }
    }

    Vector<FunctionID, 8>& functions;
};

} // anonymous namespace

HeldRegExps::HeldRegExps(const Program& program, const Facts& facts)
{
    const ThinSummaries& summaries = facts.summaries();
    Vector<uint32_t> held;
    BitVector functionsAsked(program.functions.size());
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placesOfLiterals;
    auto calleeOf = [](const BodyValue& site) -> FunctionID {
        if (site.opcode() == Opcode::NewClosure)
            return site.as<Opcode::NewClosure>().function();
        return site.opcode() == Opcode::CallDirect ? site.as<Opcode::CallDirect>().function() : FunctionID();
    };
    auto operandsOf = [](const BodyValue& site) {
        return site.opcode() == Opcode::NewClosure ? site.as<Opcode::NewClosure>().capturesOperands() : site.as<Opcode::CallDirect>().argumentsOperands();
    };
    auto madeBy = [](const BodyIndex& index, ValueID operand) {
        while (operand && !index.isParameter(operand) && inputPassThroughOpcodes.contains(index.opcodeOf(operand)))
            operand = index.value(operand).operand(0);
        return operand;
    };
    auto ownCapture = [&](const BodyIndex& index, FunctionID id, ValueID operand) -> std::optional<uint32_t> {
        std::optional<uint32_t> own = operand && index.isParameter(operand) ? index.block(entryBlock).indexOfParameter(operand) : std::nullopt;
        return own && *own < program.at(id).captures.size() ? own : std::nullopt;
    };
    auto forEachSite = [&](FunctionID id, const BodyIndex& index, const Invocable<void(const BodyValue&)> auto& functor) {
        for (ValueID site : summaries.at(id).sitesThatHandOverCaptures)
            functor(index.value(site));
    };

    Vector<FunctionID, 64> toSpread;
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex* index = facts.bodies().find(id);
        if (!index)
            return;
        if (!index->opcodes().contains(Opcode::NewRegExp))
            continue;
        forEachSite(id, *index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee || functionsAsked.get(callee.index()))
                return;
            for (ValueID operand : operandsOf(site)) {
                ValueID made = madeBy(*index, operand);
                if (made && !index->isParameter(made) && index->opcodeOf(made) == Opcode::NewRegExp) {
                    functionsAsked.set(callee.index());
                    toSpread.append(callee);
                    return;
                }
            }
        });
    }
    if (toSpread.isEmpty())
        return;
    while (!toSpread.isEmpty()) {
        FunctionID id = toSpread.takeLast();
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex& index = *facts.bodies().find(id);
        forEachSite(id, index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee || functionsAsked.get(callee.index()))
                return;
            bool handsOwnOver = std::ranges::any_of(operandsOf(site), [&](ValueID operand) {
                return !!ownCapture(index, id, madeBy(index, operand));
            });
            if (handsOwnOver) {
                functionsAsked.set(callee.index());
                toSpread.append(callee);
            }
        });
    }

    for (size_t number : functionsAsked) {
        m_firstCaptures.add(static_cast<uint32_t>(number), static_cast<uint32_t>(held.size()));
        held.insertFill(held.size(), holdsNothingYet, program.at(FunctionID(static_cast<uint32_t>(number))).captures.size());
    }
    auto captureOf = [&](FunctionID function, uint32_t capture) {
        return m_firstCaptures.get(function.index()) + capture;
    };
    Vector<uint32_t> changed;
    Vector<CapturePassedOn> passedOn;
    auto note = [&](uint32_t capture, uint32_t literal) {
        if (held[capture] == literal || held[capture] == holdsAnything)
            return;
        held[capture] = held[capture] == holdsNothingYet ? literal : holdsAnything;
        changed.append(capture);
    };
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex& index = *facts.bodies().find(id);
        forEachSite(id, index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee) {
                Vector<FunctionID, 8> functions;
                FunctionsWithClosuresOfOperation made { functions };
                site.forEachReference(made);
                for (FunctionID function : functions) {
                    for (uint32_t capture = 0; functionsAsked.get(function.index()) && capture < program.at(function).captures.size(); ++capture)
                        note(captureOf(function, capture), holdsAnything);
                }
                return;
            }
            if (!functionsAsked.get(callee.index()))
                return;
            std::span<const ValueID> operands = operandsOf(site);
            for (uint32_t capture = 0; capture < program.at(callee).captures.size(); ++capture) {
                uint32_t to = captureOf(callee, capture);
                ValueID operand = capture < operands.size() ? madeBy(index, operands[capture]) : ValueID();
                if (operand && !index.isParameter(operand) && index.opcodeOf(operand) == Opcode::NewRegExp) {
                    auto made = index.value(operand).as<Opcode::NewRegExp>();
                    auto added = placesOfLiterals.add(static_cast<uint64_t>(made.pattern().index()) << 16 | made.regExpFlags().toRaw(), m_literals.size());
                    if (added.isNewEntry)
                        m_literals.append(HeldRegExp { made.pattern(), made.regExpFlags() });
                    note(to, firstLiteral + added.iterator->value);
                    continue;
                }
                std::optional<uint32_t> own = functionsAsked.get(number) ? ownCapture(index, id, operand) : std::nullopt;
                if (own)
                    passedOn.append({ to, captureOf(id, *own) });
                else
                    note(to, holdsAnything);
            }
        });
    }
    std::ranges::sort(passedOn, { }, &CapturePassedOn::from);
    while (!changed.isEmpty()) {
        uint32_t capture = changed.takeLast();
        for (auto [to, from] : std::ranges::equal_range(passedOn, capture, { }, &CapturePassedOn::from))
            note(to, held[from]);
    }
    m_literalsOfCaptures = WTF::move(held);
}

const HeldRegExp* HeldRegExps::find(FunctionID function, uint32_t capture) const
{
    auto first = m_firstCaptures.find(function.index());
    if (first == m_firstCaptures.end())
        return nullptr;
    uint32_t place = first->value + capture;
    if (m_literalsOfCaptures[place] < firstLiteral)
        return nullptr;
    return &m_literals[m_literalsOfCaptures[place] - firstLiteral];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
