/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRawWordCheck.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTRawWordWebs.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/ForbidHeapAllocation.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class RawWordChecker {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(RawWordChecker);
public:
    RawWordChecker(FunctionID id, const Function& function, FunctionAnalyses& analyses, const RawWordWebs& webs, Vector<InternalError>& errors)
        : m_id(id)
        , m_function(function)
        , m_analyses(analyses)
        , m_webs(webs)
        , m_errors(errors)
    {
    }

    void run();

private:
    using Bases = Vector<ValueID, 2>;

    void checkWord(ValueID);
    void checkReader(const Use&, ValueID pointer, const Bases&);
    bool isCompanionWord(ValueID, const Bases&) const;
    void report(InternalErrorKind, BlockID, ValueID);

    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    const RawWordWebs& m_webs;
    Vector<InternalError>& m_errors;
};

void RawWordChecker::run()
{
    for (size_t index : m_webs.rawWords())
        checkWord(ValueID(index));
    for (const EndedRawPointer& ended : m_webs.findEndedPointers())
        report(InternalErrorKind::RawPointerAcrossSuspension, ended.end.block, ended.pointer);
}

void RawWordChecker::checkWord(ValueID word)
{
    const UseLists& useLists = m_analyses.useLists();
    bool isPointer = isInteriorPointer(*m_webs.rawKindOf(word));
    Bases bases;
    if (isPointer) {
        Vector<RawWordProduction, 2> productions;
        if (m_webs.collectProductions(word, productions) == ProductionReach::Unknown)
            report(InternalErrorKind::RawPointerWithoutBase, useLists.blockOf(word), word);
        for (const RawWordProduction& production : productions)
            bases.append(m_webs.baseOf(production));
    }

    for (const Use& use : useLists.usesOf(word)) {
        switch (use.kind) {
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
            break;
        case Use::Kind::TerminatorOperand:
            if (operandUsesOf(m_function.at(use.block).terminator.kind, use.operandIndex) != OperandUses { OperandUse::Inspects })
                report(InternalErrorKind::RawWordEscapes, use.block, word);
            break;
        case Use::Kind::BlockSubject:
            report(InternalErrorKind::RawWordEscapes, use.block, word);
            break;
        case Use::Kind::Operand:
            if (operandUsesOf(m_function.at(use.user).opcode, use.operandIndex) != OperandUses { OperandUse::Inspects })
                report(InternalErrorKind::RawWordEscapes, use.block, use.user);
            else if (isPointer)
                checkReader(use, word, bases);
            break;
        }
    }
}

void RawWordChecker::checkReader(const Use& use, ValueID pointer, const Bases& bases)
{
    if (bases.isEmpty())
        return;
    Effects effects = m_analyses.effectsOf(use.user).onAnyEdge();
    if (effects.reads.isEmpty() && effects.writes.isEmpty())
        return;

    const Value& reader = m_function.at(use.user);
    Vector<ValueID, 8> namedBases;
    bool hasStaleCompanion = false;
    reader.forEachOperand([&](ValueID operand, uint32_t) {
        namedBases.append(m_webs.canonicalBase(operand));
        if (operand != pointer && isCompanionWord(operand, bases))
            hasStaleCompanion |= m_webs.compareProductions(pointer, operand) != ProductionIdentity::SameProduction;
    });

    bool namesEveryBase = std::ranges::all_of(bases, [&](ValueID base) {
        return namedBases.contains(base);
    });
    bool namesBaseOnEveryPath = namesEveryBase || std::ranges::any_of(namedBases, [&](ValueID named) {
        return named != pointer && m_webs.isBaseOnEveryPath(named, pointer);
    });
    if (!namesBaseOnEveryPath)
        report(InternalErrorKind::RawPointerWithoutBase, use.block, use.user);
    if (hasStaleCompanion)
        report(InternalErrorKind::RawPointerOutlivesStorage, use.block, use.user);
}

bool RawWordChecker::isCompanionWord(ValueID word, const Bases& bases) const
{
    Opcode opcode = m_function.at(word).opcode;
    if (opcode != Opcode::ResultAt && opcode != Opcode::Parameter)
        return false;

    Vector<RawWordProduction, 2> productions;
    if (m_webs.collectProductions(word, productions) == ProductionReach::Unknown)
        return false;
    return std::ranges::any_of(productions, [&](const RawWordProduction& production) {
        return bases.contains(m_webs.baseOf(production));
    });
}

void RawWordChecker::report(InternalErrorKind kind, BlockID block, ValueID value)
{
    m_errors.append(InternalError { .kind = kind, .function = m_id, .block = block, .value = value });
}

} // anonymous namespace

void checkRawWords(const Program& program, FunctionID id, const Function& function, FunctionAnalyses& analyses, Vector<InternalError>& errors)
{
    RawWordWebs webs(program, function, analyses);
    if (webs.isEmpty())
        return;
    RawWordChecker(id, function, analyses, webs, errors).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
