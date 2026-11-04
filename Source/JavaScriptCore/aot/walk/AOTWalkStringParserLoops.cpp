/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The two loops of a parser unit: the elements of a list, members in any order. No body of the input holds them, so nobody has looked at them ahead: the types of the head's parameters are said.
// At a suspension: a parser never suspends. At a throw: nothing in a parser unit catches. Withheld with BuiltinCalls or TypedOperations: no call names a parser, so no parser unit is walked.

#include "config.h"
#include "AOTWalkStringParsers.h"

#if ENABLE(AOT)

#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"

namespace JSC { namespace AOT { namespace Typed {

Reference inFormForParsers(Walker& walker, Reference reference, Representation wanted, Origin origin)
{
    Representation held = walker.representationOf(reference);
    if (!reference || !hasRegister(wanted) || held == wanted)
        return reference;

    TypeArena& types = walker.types();
    if (reference.isConstant()) {
        // A constant has no form of its own: each use gets a copy in the form that it wants.
        const Operation& record = walker.code().at(reference.id());
        bool hasCopyInForm = constantsInFormOfUses.contains(record.opcode) && isLegalRepresentation(types.at(record.type), wanted)
            && wanted != Representation::Cell32 && wanted != Representation::LiteralString;
        if (hasCopyInForm) {
            Draft copy(record.opcode, record.origin());
            copy.flags = static_cast<uint8_t>(record.flags);
            copy.setPayloadListFrom(0, record.payload());
            copy.representation = wanted;
            return walker.constant(WTF::move(copy));
        }
    }

    std::optional<ConversionPath> path;
    if (held != Representation::Unchosen)
        path = findConversion(types, walker.typeOf(reference), held, wanted);
    if (!path) {
        walker.fail("a value of a parser has no conversion into the form in which it is wanted"_s);
        return { };
    }
    origin.isTailCall = false;
    for (ConversionStep step : *path) {
        Draft conversion(step.opcode, origin);
        conversion.setOperand(0, reference);
        conversion.representation = step.to;
        reference = walker.build(WTF::move(conversion));
        if (!reference)
            return { };
    }
    return reference;
}

BuiltLoopOfParsers::BuiltLoopOfParsers(Walker& walker, std::span<const Reference> entering, std::span<const TypeID> typesOfParameters)
    : m_walker(walker)
{
    RELEASE_ASSERT(entering.size() == typesOfParameters.size());
    if (walker.isUnreachable())
        return;

    Code& code = walker.code();
    Origin origin = walker.originOfOperation();
    BlockFrequency frequency = code.at(walker.currentBlock()).frequency;
    m_standsInLoopAround = code.at(walker.currentBlock()).isInLoop;
    m_head = code.addBlock(BlockRole::Ordinary, frequency);
    code.setLoopMarks(m_head, true, true);
    Vector<ValueID, 8> parameters;
    for (TypeID type : typesOfParameters) {
        parameters.append(code.appendParameter(m_head, type, walker.formOfParameter(type, { }), origin));
        m_parameters.append(Reference::value(parameters.last()));
    }
    if (!parameters.isEmpty())
        code.setParameters(m_head, parameters.span());

    if (!leadToHead(entering)) {
        m_head = { };
        return;
    }
    walker.continueIn(m_head);
    Draft poll(Opcode::LoopHint, origin);
    walker.chooseConservativeFields(poll);
    if (Reference built = walker.build(WTF::move(poll)))
        m_poll = built.id();
}

bool BuiltLoopOfParsers::leadToHead(std::span<const Reference> values)
{
    RELEASE_ASSERT(values.size() == m_parameters.size());
    Code& code = m_walker.code();
    Origin origin = m_walker.originOfOperation();

    Vector<ValueID, 8> arguments;
    for (size_t index = 0; index < values.size(); ++index) {
        TypeID typeOfParameter = code.at(m_parameters[index].id()).type;
        Representation form = code.at(m_parameters[index].id()).representation;
        Reference value = values[index];
        if (!value || !value.isRecord() || !m_walker.types().isSubset(code.at(value.id()).type, typeOfParameter)) {
            m_walker.fail("what is handed to the head of a loop of a parser lies outside the type of its parameter"_s);
            return false;
        }
        // An edge hands over a word, so it takes a cell for its boxed form and the other way round.
        if (!isSameWord(m_walker.representationOf(value), form))
            value = inFormForParsers(m_walker, value, form, origin);
        if (!value || m_walker.isUnreachable())
            return false;
        arguments.append(value.id());
    }

    BlockID from = m_walker.currentBlock();
    code.end(from, TerminatorDraft(TerminatorKind::Jump, origin), 1);
    code.closeEdge(code.at(from).terminator.next(), m_head, arguments.span());
    return true;
}

void BuiltLoopOfParsers::goRound(std::span<const Reference> values)
{
    if (!m_head || m_walker.isUnreachable())
        return;
    if (leadToHead(values))
        m_hasBackEdge = true;
}

void BuiltLoopOfParsers::close()
{
    if (!m_head || m_walker.hasFailed())
        return;
    if (!m_walker.isUnreachable()) {
        m_walker.fail("a loop of a parser is closed and the block that the walk stands in has no end"_s);
        return;
    }
    if (m_hasBackEdge)
        return;
    // No edge has come back. The head lies in no loop of its own, its parameters keep the types that were said, and its poll polls for nothing.
    Code& code = m_walker.code();
    code.setLoopMarks(m_head, m_standsInLoopAround, true);
    if (m_poll)
        code.addFateOfPoll(m_poll, FateOfPoll::IsDropped);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
