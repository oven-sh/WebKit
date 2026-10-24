/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConversions.h"
#include "AOTRepresentationWebs.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConversionWalk {
    ConversionInserter& inserter() const
    {
        if (!madeInserter)
            madeInserter.emplace(pass, builder);
        return *madeInserter;
    }

    const RepresentationInputs& inputs;
    Function& function;
    FunctionPassContext& pass;
    IRBuilder& builder;
    std::optional<ConversionInserter>& madeInserter;
};

} // anonymous namespace

static void uncoverEntryBlock(FunctionPassContext& pass, IRBuilder& builder)
{
    const Block& entry = pass.function.at(entryBlock);
    if (!entry.handler)
        return;

    builder.setOrigin(entry.terminator.origin());
    builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    pass.analyses.didChangeControlFlow();
}

AOT_THRESHOLD(unsigned, maxCopiesFollowed, 4);

static bool isMadeBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, unsigned copiesFollowed = 0)
{
    const Function& function = inputs.function;
    const Value& value = function.at(id);
    if (!picksOwnRepresentation(function, id))
        return resultRepresentationOf(inputs, value) == Representation::JSValue;
    if (copiesFollowed == maxCopiesFollowed || constantsInFormOfUses.contains(value.opcode) || value.opcode == Opcode::Unspecified)
        return false;

    if (value.opcode != Opcode::Parameter) {
        bool isBoxed = true;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (isForwardedOperand(value, operandIndex))
                isBoxed = isBoxed && isMadeBoxed(pass, inputs, operand, copiesFollowed + 1);
        });
        return isBoxed;
    }

    ValueLocation location = pass.analyses.useLists().locationOf(id);
    bool hasBoxedSource = false;
    for (Edge edge : pass.analyses.cfg().incomingEdges(location.block)) {
        if (edge.isHandlerEdge())
            return false;
        IncomingValue incoming = incomingValueOf(function, edge, location.index);
        ValueID source = incoming.argument;
        if (incoming.kind == IncomingValue::Kind::EdgeResult) {
            EdgeResult result = edgeResultAt(function, incoming.result);
            if (result.kind != EdgeResultKind::NarrowedOperand)
                return false;
            source = result.subject;
        }
        // A constant comes in whatever form is asked for: `value ?? 0`.
        if (constantsInFormOfUses.contains(function.at(source).opcode))
            continue;
        if (!isMadeBoxed(pass, inputs, source, copiesFollowed + 1))
            return false;
        hasBoxedSource = true;
    }
    return hasBoxedSource;
}

namespace {

struct UsesOfCopies {
    Vector<ValueID, 16> copies;
    uint64_t votesForBoxed { 0 };
    uint64_t votesForOthers { 0 };
};

} // anonymous namespace

static void countUsesOfCopies(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, UsesOfCopies& uses)
{
    AOT_THRESHOLD(size_t, maxCopies, 16);
    const Function& function = inputs.function;
    if (uses.copies.size() == maxCopies || uses.copies.contains(id))
        return;
    uses.copies.append(id);

    auto count = [&](Representation wanted, BlockID block) {
        if (hasRegister(wanted))
            (wanted == Representation::JSValue ? uses.votesForBoxed : uses.votesForOthers) += votesOfConversionIn(pass.analyses, block);
    };
    for (const Use& use : pass.analyses.useLists().usesOf(id)) {
        switch (use.kind) {
        case Use::Kind::Operand: {
            const Value& user = function.at(use.user);
            if (isForwardedOperand(user, use.operandIndex))
                countUsesOfCopies(pass, inputs, use.user, uses);
            else
                count(wantedRepresentationOf(inputs, user, use.operandIndex), use.block);
            break;
        }
        case Use::Kind::TerminatorOperand:
            count(wantedRepresentationOf(inputs, function.at(use.block).terminator, use.operandIndex), use.block);
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument: {
            ValueID parameter = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
            if (defaultRepresentationOf(inputs.typeOf(parameter)) == Representation::JSValue)
                count(Representation::JSValue, use.block);
            else
                countUsesOfCopies(pass, inputs, parameter, uses);
            break;
        }
        case Use::Kind::BlockSubject:
            break;
        }
    }
}

static bool isWantedBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id)
{
    UsesOfCopies uses;
    countUsesOfCopies(pass, inputs, id, uses);
    return uses.votesForBoxed > uses.votesForOthers;
}

template<typename Predicate>
static bool everyOperandBesideInt32(const RepresentationInputs& inputs, const Value& sum, const Predicate& holds)
{
    bool hasSuchOperand = false;
    bool holdsForAll = true;
    sum.forEachOperand([&](ValueID operand, uint32_t) {
        if (inputs.typeOf(operand).isInt32())
            return;
        hasSuchOperand = true;
        holdsForAll = holdsForAll && holds(operand);
    });
    return hasSuchOperand && holdsForAll;
}

static Vector<ValueID, 8> keepSumsOfBoxedNumbersBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Function& function = pass.function;
    Vector<ValueID, 8> kept;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!sumsOfNumbers.contains(value.opcode) || value.form() != OperatorForm::Numbers)
                continue;
            bool takesBoxedNumbers = everyOperandBesideInt32(inputs, value, [&](ValueID operand) {
                return inputs.typeOf(operand).primitives & Primitive::Integer && isMadeBoxed(pass, inputs, operand);
            });
            if (takesBoxedNumbers && isWantedBoxed(pass, inputs, id))
                kept.append(id);
        }
    }
    for (ValueID id : kept)
        function.edit(id, ValueEdit::Type).representation = Representation::JSValue;
    return kept;
}

static void settleSumsKeptBoxed(const RepresentationInputs& inputs, Function& function, std::span<const ValueID> sumsKeptBoxed)
{
    for (ValueID id : sumsKeptBoxed) {
        bool operandsStayedBoxed = everyOperandBesideInt32(inputs, function.at(id), [&](ValueID operand) {
            return function.at(operand).representation == Representation::JSValue;
        });
        if (!operandsStayedBoxed)
            function.edit(id, ValueEdit::Type).representation = Representation::Double;
    }
}

static void writeRepresentations(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Vector<ValueID, 8> sumsKeptBoxed = keepSumsOfBoxedNumbersBoxed(pass, inputs);
    RepresentationWebs webs(inputs, pass.analyses);
    webs.build();
    webs.choose();

    Function& function = pass.function;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters)
            function.edit(parameter, ValueEdit::Type).representation = webs.representationOf(parameter);
        for (ValueID value : block.values)
            function.edit(value, ValueEdit::Type).representation = webs.representationOf(value);
    }

    settleSumsKeptBoxed(inputs, function, sumsKeptBoxed.span());
}

namespace {

class FormsOfCopies {
public:
    FormsOfCopies(FunctionPassContext& pass, const RepresentationInputs& inputs)
        : m_pass(pass)
        , m_inputs(inputs)
        , m_function(pass.function)
        , m_numberOf(FillWith { }, pass.function.values.size(), 0u)
        , m_mayBeNarrow(pass.function.values.size())
    {
    }

    void run()
    {
        std::span<const BlockID> blocks = m_pass.analyses.cfg().reversePostOrder();
        for (BlockID block : blocks)
            writeFormsIn(block);
        if (m_copies.isEmpty())
            return;
        // The rules have no wish for an operand they take as it comes while its form is open.
        for (Copy& copy : m_copies)
            copy.formAtStart = std::exchange(m_function.edit(copy.value, ValueEdit::Type).representation, Representation::Unchosen);
        for (BlockID block : blocks)
            collectVotesIn(block);
        for (const Copy& copy : m_copies)
            m_function.edit(copy.value, ValueEdit::Type).representation = copy.formAtStart;
        sortLinks();
        chooseForCopies();
        chooseForConstants();
    }

private:
    using Votes = std::array<uint32_t, numberOfEnumerators<Representation>>;

    struct Copy {
        ValueID value;
        RepresentationSet legal;
        Representation formAtStart { Representation::Unchosen };
        Votes votes { };
        uint32_t firstLink { 0 };
        BlockID blockOfNarrowParameter;
        bool isConstant { false };
        bool staysAsItIs { false };
    };

    struct Link {
        uint32_t copy { 0 };
        ValueID other;
        uint32_t votes { 0 };
        bool isProducer { false };
    };

    static constexpr RepresentationSet formsOfCopies {
        Representation::Void, Representation::Boolean, Representation::Int32, Representation::Int64, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble,
        Representation::Cell, Representation::JSValue, Representation::LiteralString, Representation::RawWord,
    };

    Representation formOf(ValueID id) const { return m_function.at(id).representation; }
    Copy* copyOf(ValueID id) { return m_numberOf[id.index()] ? &m_copies[m_numberOf[id.index()] - 1] : nullptr; }
    static uint32_t votesAt(BlockFrequency frequency) { return frequency == BlockFrequency::Normal ? 64 : 1; }
    uint32_t votesOf(BlockID block) const { return votesAt(m_function.at(block).frequency); }

    RepresentationSet legalFormsOf(ValueID id)
    {
        TypeID type = m_function.at(id).type;
        LegalFormsOfType& entry = m_legalForms[type.index() % m_legalForms.size()];
        if (entry.type != type || !type)
            entry = { type, legalRepresentationsOf(m_inputs.typeOf(id)) };
        return entry.forms;
    }

    void add(ValueID id, RepresentationSet legal, bool isConstant)
    {
        m_copies.append(Copy { .value = id, .legal = legal, .isConstant = isConstant });
        m_numberOf[id.index()] = m_copies.size();
    }

    void writeFormsIn(BlockID id)
    {
        const Block& block = m_function.at(id);
        for (uint32_t index = 0; index < block.parameters.size(); ++index)
            writeFormOf(block.parameters[index], id, index);
        for (ValueID value : block.values)
            writeFormOf(value, id, 0);
    }

    // Anything that was not read out of a narrow slot may be an object in a frame, which lies outside the cage of cells.
    bool everythingThatArrivesMayBeNarrow(BlockID block, uint32_t index, ValueID parameter)
    {
        for (Edge edge : m_pass.analyses.cfg().incomingEdges(block)) {
            if (edge.isHandlerEdge())
                return false;
            IncomingValue incoming = incomingValueOf(m_function, edge, index);
            ValueID source = incoming.argument;
            if (incoming.kind == IncomingValue::Kind::EdgeResult) {
                EdgeResult result = edgeResultAt(m_function, incoming.result);
                if (result.kind != EdgeResultKind::NarrowedOperand)
                    return false;
                source = result.subject;
            }
            if (!source)
                return false;
            if (source == parameter || m_mayBeNarrow.quickGet(source.index()))
                continue;
            if (!constantsInFormOfUses.contains(m_function.at(source).opcode) || m_inputs.typeOf(source).hasCells())
                return false;
        }
        return true;
    }

    void writeFormOf(ValueID id, BlockID block, uint32_t indexOfParameter)
    {
        const Value& value = m_function.at(id);
        Representation form = resultRepresentationOf(m_inputs, value);
        if (form != Representation::Unchosen) {
            m_function.edit(id, ValueEdit::Type).representation = form;
            if (form == Representation::Cell32)
                m_mayBeNarrow.quickSet(id.index());
            return;
        }
        if (constantsInFormOfUses.contains(value.opcode)) {
            m_function.edit(id, ValueEdit::Type).representation = defaultRepresentationOf(m_inputs.typeOf(id));
            add(id, { }, true);
            return;
        }

        RepresentationSet legal = legalFormsOf(id);
        bool fitsNarrowReference = legal.contains(Representation::Cell32);
        legal &= formsOfCopies;
        ValueID handedOn;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!isForwardedOperand(value, operandIndex))
                return;
            handedOn = operand;
            RepresentationSet convertible = legalFormsOf(operand);
            convertible &= formsWithRegister;
            if (!hasRegister(formOf(operand)))
                convertible.add(formOf(operand));
            legal &= convertible;
        });
        form = defaultRepresentationOf(m_inputs.typeOf(id));
        if (!legal.contains(form) && handedOn)
            form = defaultRepresentationOf(m_inputs.typeOf(handedOn));
        if (!legal.contains(form))
            form = Representation::JSValue;
        RELEASE_ASSERT(legal.contains(form));
        m_function.edit(id, ValueEdit::Type).representation = form;

        if (fitsNarrowReference && handedOn && isCast(value.opcode) && m_mayBeNarrow.quickGet(handedOn.index())) {
            legal.add(Representation::Cell32);
            m_mayBeNarrow.quickSet(id.index());
        }
        bool isNarrowParameter = fitsNarrowReference && value.opcode == Opcode::Parameter && everythingThatArrivesMayBeNarrow(block, indexOfParameter, id);
        if (isNarrowParameter) {
            legal.add(Representation::Cell32);
            m_mayBeNarrow.quickSet(id.index());
        }

        for (Representation other : allMembersOf<Representation>) {
            if (legal.contains(other) && !isSameWord(other, form)) {
                add(id, legal, false);
                if (isNarrowParameter) {
                    m_copies.last().blockOfNarrowParameter = block;
                    m_hasNarrowParameters = true;
                }
                return;
            }
        }
    }

    void addUse(ValueID used, Representation wanted, uint32_t votes)
    {
        if (Copy* copy = copyOf(used); copy && hasRegister(wanted))
            copy->votes[std::to_underlying(wanted)] += votes;
    }

    void addCopying(ValueID from, ValueID to, uint32_t votes, bool isNarrowedOnEdge = false)
    {
        if (Copy* source = isNarrowedOnEdge ? nullptr : copyOf(from)) {
            if (copyOf(to))
                m_links.append(Link { m_numberOf[from.index()] - 1, to, votes, false });
            else if (hasRegister(formOf(to)))
                source->votes[std::to_underlying(formOf(to))] += votes;
            else if (source->isConstant)
                source->staysAsItIs = true;
        }
        Copy* destination = copyOf(to);
        if (!destination || constantsInFormOfUses.contains(m_function.at(from).opcode))
            return;
        if (copyOf(from))
            m_links.append(Link { m_numberOf[to.index()] - 1, from, votes, true });
        else
            destination->votes[std::to_underlying(formOf(from))] += votes;
    }

    // The collector does not see a narrow reference: every use has to come before it can run.
    void noteUseOfNarrowParameter(ValueID used, BlockID block, bool isReachedBeforeCollectorCanRun)
    {
        Copy* copy = copyOf(used);
        if (copy && copy->blockOfNarrowParameter && (copy->blockOfNarrowParameter != block || !isReachedBeforeCollectorCanRun)) {
            copy->blockOfNarrowParameter = { };
            copy->legal &= formsOfCopies;
        }
    }

    void collectVotesIn(BlockID id)
    {
        const Block& block = m_function.at(id);
        uint32_t votes = votesOf(id);
        uint32_t firstCollectingValue = block.values.size();
        if (m_hasNarrowParameters) {
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                if (mayCollect(m_function.at(block.values[index]).opcode)) {
                    firstCollectingValue = index;
                    break;
                }
            }
        }
        bool runsNoCollector = firstCollectingValue == block.values.size();
        uint32_t indexOfUser = 0;
        for (ValueID user : block.values) {
            const Value& value = m_function.at(user);
            bool isInFrontOfCollector = provesOf(value.opcode) != Proof::None ? runsNoCollector : indexOfUser <= firstCollectingValue;
            ++indexOfUser;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (m_hasNarrowParameters)
                    noteUseOfNarrowParameter(operand, id, isInFrontOfCollector);
                if (isForwardedOperand(value, operandIndex))
                    addCopying(operand, user, votes);
                else if (m_numberOf[operand.index()])
                    addUse(operand, wantedRepresentationOf(m_inputs, value, operandIndex), votes);
            });
        }

        const Terminator& terminator = block.terminator;
        bool ownsOperand = terminator.kind == TerminatorKind::TestAndBranch || terminator.kind == TerminatorKind::ReturnCall;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Copy* copy = copyOf(operand);
            if (!copy)
                return;
            noteUseOfNarrowParameter(operand, id, runsNoCollector);
            if (ownsOperand)
                copy->staysAsItIs |= copy->isConstant;
            else
                addUse(operand, wantedRepresentationOf(m_inputs, terminator, operandIndex), votes);
        });

        forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target& target) {
            const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
            uint32_t votesOnEdge = edge.isHandlerEdge() ? 1 : votesAt(rarerOf(block.frequency, m_function.at(target.block).frequency));
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                if (ValueID argument = target.arguments[index]) {
                    noteUseOfNarrowParameter(argument, id, runsNoCollector && !edge.isHandlerEdge());
                    addCopying(argument, parameters[index], votesOnEdge);
                }
            }
            if (edge.isHandlerEdge())
                return;
            uint32_t firstResult = firstEdgeResultParameter(m_function, edge.from, edge.targetIndex);
            uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
            for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
                ValueID parameter = parameters[firstResult + resultIndex];
                Copy* copy = copyOf(parameter);
                if (!copy)
                    continue;
                EdgeResultPosition position { edge.from, edge.targetIndex, resultIndex };
                EdgeResult result = edgeResultAt(m_function, position);
                if (result.kind == EdgeResultKind::NarrowedOperand)
                    addCopying(result.subject, parameter, votesOnEdge, true);
                else
                    copy->votes[std::to_underlying(edgeResultRepresentationOf(m_inputs, position))] += votesOnEdge;
            }
        });
    }

    void sortLinks()
    {
        AOT::stableSort(m_links, { }, &Link::copy);
        uint32_t next = 0;
        for (uint32_t index = 0; index < m_copies.size(); ++index) {
            m_copies[index].firstLink = next;
            while (next < m_links.size() && m_links[next].copy == index)
                ++next;
        }
    }

    std::span<const Link> linksOf(uint32_t index) const
    {
        uint32_t end = index + 1 < m_copies.size() ? m_copies[index + 1].firstLink : m_links.size();
        return m_links.span().subspan(m_copies[index].firstLink, end - m_copies[index].firstLink);
    }

    Votes votesWithLinks(uint32_t index) const
    {
        Votes votes = m_copies[index].votes;
        for (const Link& link : linksOf(index)) {
            Representation form = formOf(link.other);
            if (link.isProducer || hasRegister(form))
                votes[std::to_underlying(form)] += link.votes;
        }
        return votes;
    }

    uint64_t votesBetween(uint32_t index, ValueID other, bool isProducer) const
    {
        uint64_t votes = 0;
        for (const Link& link : linksOf(index)) {
            if (link.other == other && link.isProducer == isProducer)
                votes += link.votes;
        }
        return votes;
    }

    static uint64_t conversionsNeededBy(const Votes& votes, Representation form)
    {
        uint64_t all = 0;
        for (uint32_t votesForForm : votes)
            all += votesForForm;
        return all - votes[std::to_underlying(form)];
    }

    bool moveTogether(uint32_t source, uint32_t destination)
    {
        const Copy& from = m_copies[source];
        const Copy& to = m_copies[destination];
        Representation formOfSource = formOf(from.value);
        Representation formOfDestination = formOf(to.value);
        uint64_t votesAsProducer = votesBetween(destination, from.value, true);
        uint64_t votesAsUse = hasRegister(formOfDestination) ? votesBetween(source, to.value, false) : 0;
        Votes votesOfSource = votesWithLinks(source);
        Votes votesOfDestination = votesWithLinks(destination);
        uint64_t fewest = conversionsNeededBy(votesOfSource, formOfSource) + conversionsNeededBy(votesOfDestination, formOfDestination);
        if (formOfSource != formOfDestination)
            fewest -= votesAsUse;
        std::optional<Representation> best;
        for (Representation candidate : allMembersOf<Representation>) {
            if (!hasRegister(candidate) || !from.legal.contains(candidate) || !to.legal.contains(candidate) || candidate == formOfSource || candidate == formOfDestination)
                continue;
            uint64_t needed = conversionsNeededBy(votesOfSource, candidate) + conversionsNeededBy(votesOfDestination, candidate) - votesAsProducer - votesAsUse;
            if (needed < fewest) {
                best = candidate;
                fewest = needed;
            }
        }
        if (!best)
            return false;
        m_function.edit(from.value, ValueEdit::Type).representation = *best;
        m_function.edit(to.value, ValueEdit::Type).representation = *best;
        return true;
    }

    bool moveCopiesTogether()
    {
        bool changed = false;
        for (const Link& link : m_links) {
            if (!link.isProducer || m_copies[link.copy].isConstant)
                continue;
            uint32_t source = m_numberOf[link.other.index()] - 1;
            if (!m_copies[source].isConstant)
                changed |= moveTogether(source, link.copy);
        }
        return changed;
    }

    void chooseForCopies()
    {
        AOT_THRESHOLD(unsigned, maxRounds, 8);
        for (unsigned round = 0; round < maxRounds; ++round) {
            bool changed = false;
            for (uint32_t index = 0; index < m_copies.size(); ++index) {
                const Copy& copy = m_copies[index];
                if (copy.isConstant)
                    continue;
                Votes votes = votesWithLinks(index);
                Representation best = formOf(copy.value);
                for (Representation candidate : allMembersOf<Representation>) {
                    if (copy.legal.contains(candidate) && votes[std::to_underlying(candidate)] > votes[std::to_underlying(best)])
                        best = candidate;
                }
                if (best == formOf(copy.value))
                    continue;
                m_function.edit(copy.value, ValueEdit::Type).representation = best;
                changed = true;
            }
            if (!changed && !moveCopiesTogether())
                return;
        }
    }

    void chooseForConstants()
    {
        for (uint32_t index = 0; index < m_copies.size(); ++index) {
            const Copy& copy = m_copies[index];
            if (!copy.isConstant || copy.staysAsItIs)
                continue;
            bool goesWhereNoRegisterIs = std::ranges::any_of(linksOf(index), [&](const Link& link) {
                return !hasRegister(formOf(link.other)) && isCastLike(link.other);
            });
            if (goesWhereNoRegisterIs)
                continue;
            Votes votes = votesWithLinks(index);
            const Type& type = m_inputs.typeOf(copy.value);
            Representation best = formOf(copy.value);
            for (Representation candidate : allMembersOf<Representation>) {
                if (votes[std::to_underlying(candidate)] <= votes[std::to_underlying(best)] || !isLegalRepresentation(type, candidate))
                    continue;
                if (candidate == Representation::Cell32 && type.hasCells())
                    continue;
                best = candidate;
            }
            m_function.edit(copy.value, ValueEdit::Type).representation = best;
        }
    }

    bool isCastLike(ValueID id) const { return m_function.at(id).opcode != Opcode::Parameter; }

    struct LegalFormsOfType {
        TypeID type;
        RepresentationSet forms;
    };

    static constexpr RepresentationSet formsWithRegister = [] {
        RepresentationSet result;
        for (Representation representation : allMembersOf<Representation>) {
            if (hasRegister(representation))
                result.add(representation);
        }
        return result;
    }();

    FunctionPassContext& m_pass;
    const RepresentationInputs& m_inputs;
    Function& m_function;
    Vector<uint32_t> m_numberOf;
    BitVector m_mayBeNarrow;
    bool m_hasNarrowParameters { false };
    Vector<Copy> m_copies;
    Vector<Link> m_links;
    std::array<LegalFormsOfType, 16> m_legalForms { };
};

} // anonymous namespace

static void writeRepresentationsByTally(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Vector<ValueID, 8> sumsKeptBoxed = keepSumsOfBoxedNumbersBoxed(pass, inputs);
    FormsOfCopies(pass, inputs).run();
    settleSumsKeptBoxed(inputs, pass.function, sumsKeptBoxed.span());
}

static bool needsConversion(const Function& function, ValueID value, Representation wanted)
{
    return hasRegister(wanted) && function.at(value).representation != wanted;
}

static void convertTerminatorOperands(const ConversionWalk& walk, BlockID block)
{
    const Terminator& terminator = walk.function.at(block).terminator;
    for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
        ValueID operand = terminator.operand(operandIndex);
        if (!operand)
            continue;
        Representation wanted = wantedRepresentationOf(walk.inputs, terminator, operandIndex);
        if (needsConversion(walk.function, operand, wanted))
            walk.inserter().convertTerminatorOperand(block, operandIndex, wanted);
    }
}

// The code generator hands an operation each of its operands by the id of the value, in one form.
template<typename Operation>
static bool takesOneValueInTwoForms(const ConversionWalk& walk, const Operation& user)
{
    AOT_THRESHOLD(uint32_t, maxOperandsComparedInPairs, 6);
    uint32_t numberOfOperands = user.numberOfOperands();
    if (numberOfOperands < 2)
        return false;

    auto formTakenAt = [&](uint32_t operandIndex) {
        ValueID operand = user.operand(operandIndex);
        Representation wanted = wantedRepresentationOf(walk.inputs, user, operandIndex);
        return needsConversion(walk.function, operand, wanted) ? wanted : walk.function.at(operand).representation;
    };
    if (numberOfOperands <= maxOperandsComparedInPairs) {
        for (uint32_t later = 1; later < numberOfOperands; ++later) {
            ValueID operand = user.operand(later);
            for (uint32_t earlier = 0; operand && earlier < later; ++earlier) {
                if (user.operand(earlier) == operand && formTakenAt(earlier) != formTakenAt(later))
                    return true;
            }
        }
        return false;
    }

    Vector<std::pair<ValueID, uint32_t>, 16> operands;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (ValueID operand = user.operand(operandIndex))
            operands.append({ operand, operandIndex });
    }
    AOT::sort(operands);
    for (size_t index = 1; index < operands.size(); ++index) {
        if (operands[index - 1].first == operands[index].first && formTakenAt(operands[index - 1].second) != formTakenAt(operands[index].second))
            return true;
    }
    return false;
}

static void insertConversionsThatCannotWait(const ConversionWalk& walk, std::span<const BlockID> reversePostOrder)
{
    Function& function = walk.function;
    for (BlockID block : reversePostOrder) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID user = values[index];
            if (!takesOneValueInTwoForms(walk, function.at(user)))
                continue;
            uint32_t numberOfOperands = function.at(user).numberOfOperands();
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                ValueID operand = function.at(user).operand(operandIndex);
                if (!operand)
                    continue;
                Representation wanted = wantedRepresentationOf(walk.inputs, function.at(user), operandIndex);
                if (needsConversion(function, operand, wanted))
                    walk.inserter().convertOperand(user, operandIndex, wanted, { block, index });
            }
        }
        if (takesOneValueInTwoForms(walk, function.at(block).terminator))
            convertTerminatorOperands(walk, block);
    }
    if (walk.madeInserter)
        walk.madeInserter->finish();
}

static void countRepresentations(FunctionPassContext& pass, uint32_t numberOfConversions)
{
    const Function& function = pass.function;
    std::array<uint64_t, numberOfEnumerators<Representation>> numberOfValuesIn { };
    auto countValue = [&](ValueID id) {
        ++numberOfValuesIn[std::to_underlying(function.at(id).representation)];
    };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            countValue(parameter);
        for (ValueID value : block.values)
            countValue(value);
    }

    auto count = [&](StatisticID statistic, uint64_t amount) {
        if (amount)
            pass.build.statistics().count(statistic, amount);
    };
    count(StatisticID::ConversionsInserted, numberOfConversions);
    count(StatisticID::ValuesInInt32, numberOfValuesIn[std::to_underlying(Representation::Int32)]);
    count(StatisticID::ValuesInInt64, numberOfValuesIn[std::to_underlying(Representation::Int64)]);
    count(StatisticID::ValuesInDouble, numberOfValuesIn[std::to_underlying(Representation::Double)]);
    count(StatisticID::ValuesBoxed, numberOfValuesIn[std::to_underlying(Representation::JSValue)]);
    count(StatisticID::ReferencesCompressed, numberOfValuesIn[std::to_underlying(Representation::Cell32)]);
}

static bool copiesTakeFormsByTally(FunctionPassContext& pass)
{
    return isGeneratedWithoutOptimizing(pass.build, pass.inputs.heat, pass.program, pass.id);
}

PassOutcome chooseRepresentations(FunctionPassContext& pass)
{
    RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions && !pass.function.representationsAreChosen);
    Function& function = pass.function;
    size_t numberOfBlocksBefore = function.blocks.size();

    IRBuilder builder(pass.analyses.context(), function, pass.types);
    uncoverEntryBlock(pass, builder);

    RepresentationInputs inputs { pass.program, pass.id, function, *pass.inputs.placement, *pass.inputs.callingConventions };
    if (copiesTakeFormsByTally(pass))
        writeRepresentationsByTally(pass, inputs);
    else
        writeRepresentations(pass, inputs);

    std::optional<ConversionInserter> inserter;
    ConversionWalk walk { inputs, function, pass, builder, inserter };
    insertConversionsThatCannotWait(walk, pass.analyses.cfg().reversePostOrder());

    // No analysis reads a representation, and only a block that is added changes an edge.
    if (function.blocks.size() != numberOfBlocksBefore)
        pass.analyses.didChangeControlFlow();
    else if (inserter) {
        pass.analyses.didChangeValues();
        pass.keptAnalyses = controlFlowAnalyses;
    } else
        pass.keptAnalyses = allAnalyses;

    function.representationsAreChosen = true;
    countRepresentations(pass, inserter ? inserter->numberOfConversions() : 0);

    if (pass.build.options().verifiesProgram) {
        Vector<InternalError> errors;
        checkRepresentations(inputs, errors);
        if (!errors.isEmpty())
            pass.build.diagnostics().append(WTF::move(errors));
    }
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
