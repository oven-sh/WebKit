/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKeyOrders.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTOpcodeSets.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

KeyOrders computeKeyOrders(const Program&, const LinkTables&, const ThinSummaries&, const BodyIndexTable&, Mechanisms, std::span<const PlannedLayout>, BuildContext&);

static size_t lengthSharedByKeyOrders(std::span<const FieldIndex> first, std::span<const FieldIndex> second)
{
    return static_cast<size_t>(std::ranges::mismatch(first, second).in1 - first.begin());
}

static uint32_t firstKeyOrderNotBefore(const KeyOrders& orders, LayoutID layout, std::span<const FieldIndex> fields)
{
    uint32_t low = 0;
    uint32_t high = orders.numberOfOrdersOf(layout);
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        std::span<const FieldIndex> listed = orders.order(layout, middle);
        size_t shared = lengthSharedByKeyOrders(listed, fields);
        bool comesInFront = shared < fields.size() && (shared == listed.size() || listed[shared] < fields[shared]);
        if (comesInFront)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static std::optional<size_t> findPlaceInKeyOrder(std::span<const FieldIndex> order, std::span<const FieldIndex> fields)
{
    size_t place = static_cast<size_t>(std::ranges::find(order, fields.front()) - order.begin());
    if (order.size() - place < fields.size() || !std::ranges::equal(order.subspan(place, fields.size()), fields))
        return std::nullopt;
    return place;
}

static std::optional<size_t> findPlaceInKeyOrderInFrontOf(size_t limit, std::span<const FieldIndex> order, std::span<const FieldIndex> fields)
{
    std::span<const FieldIndex> beginning = order.first(std::min(order.size(), limit));
    size_t place = static_cast<size_t>(std::ranges::find(beginning, fields.front()) - beginning.begin());
    if (place == beginning.size() || order.size() - place < fields.size() || !std::ranges::equal(order.subspan(place, fields.size()), fields))
        return std::nullopt;
    return place;
}

static uint32_t wordOfBeginningOfKeyOrder(const KeyOrders& orders, LayoutID layout, uint32_t number, size_t count)
{
    std::optional<uint32_t> word = orders.findOrderWord(layout, orders.order(layout, number).first(count));
    RELEASE_ASSERT(word);
    return *word;
}

std::optional<uint32_t> KeyOrders::findOrderWord(LayoutID layout, std::span<const FieldIndex> fields) const
{
    if (fields.empty())
        return unorderedOrderWord;
    uint32_t number = firstKeyOrderNotBefore(*this, layout, fields);
    if (number == numberOfOrdersOf(layout) || lengthSharedByKeyOrders(order(layout, number), fields) < fields.size())
        return std::nullopt;
    RELEASE_ASSERT(number < maxKeyOrdersPerLayout && fields.size() <= maxFieldsInOrder);
    return orderWordOf(number, static_cast<uint32_t>(fields.size()));
}

size_t KeyOrders::lengthOfLongestBeginningWithOrderWord(LayoutID layout, std::span<const FieldIndex> fields) const
{
    uint32_t next = firstKeyOrderNotBefore(*this, layout, fields);
    size_t length = 0;
    if (next < numberOfOrdersOf(layout))
        length = lengthSharedByKeyOrders(order(layout, next), fields);
    if (next)
        length = std::max(length, lengthSharedByKeyOrders(order(layout, next - 1), fields));
    return length;
}

std::optional<KeyOrderContinuation> KeyOrders::continuationCreatingBehindFewestKeys(LayoutID layout, std::span<const FieldIndex> createdFields) const
{
    return continuationCreatingBehindFewestKeys(layout, createdFields, { });
}

std::optional<KeyOrderContinuation> KeyOrders::continuationCreatingBehindFewestKeys(LayoutID layout, std::span<const FieldIndex> createdFields, std::span<const FieldIndex> fieldsInFront) const
{
    uint32_t numberOfOrders = numberOfOrdersOf(layout);
    if (createdFields.empty() || !numberOfOrders)
        return std::nullopt;
    std::span<const uint32_t> firstFieldOfOrder = m_parts.firstFieldOfOrder.span().subspan(m_parts.firstOrderOfLayout[layout.index()], numberOfOrders + 1);
    std::span<const FieldIndex> fields = m_parts.fields.span();
    BitVector isInFront;
    size_t numberOfFieldsInFront = 0;
    for (FieldIndex field : fieldsInFront)
        numberOfFieldsInFront += !isInFront.set(field);
    constexpr size_t noPlace = std::numeric_limits<size_t>::max();
    size_t fewest = noPlace;
    uint32_t best = 0;
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> listed = fields.subspan(firstFieldOfOrder[number], firstFieldOfOrder[number + 1] - firstFieldOfOrder[number]);
        std::optional<size_t> place = findPlaceInKeyOrderInFrontOf(fewest, listed, createdFields);
        if (!place || *place < fieldsInFront.size())
            continue;
        std::span<const FieldIndex> beginning = listed.first(*place);
        if (numberOfFieldsInFront && static_cast<size_t>(std::ranges::count_if(beginning, [&](FieldIndex field) { return isInFront.get(field); })) != numberOfFieldsInFront)
            continue;
        fewest = *place;
        best = number;
        if (fewest == fieldsInFront.size())
            break;
    }
    if (fewest == noPlace)
        return std::nullopt;
    uint32_t wordInFront = wordOfBeginningOfKeyOrder(*this, layout, best, fewest);
    size_t numberOfFieldsBehind = fewest + createdFields.size();
    ASSERT(firstKeyOrderNotBefore(*this, layout, order(layout, best).first(numberOfFieldsBehind)) == best);
    RELEASE_ASSERT(best < maxKeyOrdersPerLayout && numberOfFieldsBehind <= maxFieldsInOrder);
    return KeyOrderContinuation { wordInFront, orderWordOf(best, static_cast<uint32_t>(numberOfFieldsBehind)) };
}

std::optional<uint32_t> KeyOrders::findOrderWordOfShortestBeginningThatLists(LayoutID layout, std::span<const FieldIndex> fields) const
{
    std::optional<uint32_t> best;
    size_t fewest = std::numeric_limits<size_t>::max();
    uint32_t numberOfOrders = fields.empty() ? 0 : numberOfOrdersOf(layout);
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> listed = order(layout, number);
        size_t count = 0;
        for (FieldIndex field : fields) {
            size_t place = static_cast<size_t>(std::ranges::find(listed, field) - listed.begin());
            count = std::max(count, place + 1);
        }
        if (count > listed.size() || count >= fewest)
            continue;
        fewest = count;
        best = wordOfBeginningOfKeyOrder(*this, layout, number, count);
    }
    return best;
}

Vector<BeginningsOfKeyOrder, 2> KeyOrders::beginningsThatList(LayoutID layout, std::span<const FieldIndex> fields) const
{
    Vector<BeginningsOfKeyOrder, 2> beginnings;
    BitVector isAskedFor;
    size_t numberAskedFor = 0;
    for (FieldIndex field : fields)
        numberAskedFor += !isAskedFor.set(field);
    uint32_t numberOfOrders = numberAskedFor ? numberOfOrdersOf(layout) : 0;
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> listed = order(layout, number);
        size_t numberFound = 0;
        size_t count = 0;
        while (count < listed.size() && numberFound < numberAskedFor)
            numberFound += isAskedFor.get(listed[count++]);
        if (numberFound < numberAskedFor)
            continue;
        size_t sharedWithOrderInFront = number ? lengthSharedByKeyOrders(order(layout, number - 1), listed) : 0;
        size_t fewest = std::max(count, sharedWithOrderInFront + 1);
        if (fewest > listed.size())
            continue;
        RELEASE_ASSERT(number < maxKeyOrdersPerLayout && listed.size() <= maxFieldsInOrder);
        beginnings.append({ orderWordOf(number, static_cast<uint32_t>(fewest)), static_cast<uint32_t>(listed.size() - fewest) });
    }
    return beginnings;
}

std::optional<KeyOrderContinuation> KeyOrders::firstContinuationCreating(LayoutID layout, std::span<const FieldIndex> createdFields) const
{
    uint32_t numberOfOrders = numberOfOrdersOf(layout);
    if (createdFields.empty() || !numberOfOrders)
        return std::nullopt;

    if (createdFields.size() == 1) {
        uint32_t first = m_firstContinuationOfLayout[layout.index()];
        FieldIndex field = createdFields.front();
        if (field >= m_firstContinuationOfLayout[layout.index() + 1] - first || !m_firstContinuationCreatingField[first + field].orderWordAfter)
            return std::nullopt;
        return m_firstContinuationCreatingField[first + field];
    }

    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        if (std::optional<size_t> place = findPlaceInKeyOrder(order(layout, number), createdFields)) {
            return KeyOrderContinuation {
                wordOfBeginningOfKeyOrder(*this, layout, number, *place),
                wordOfBeginningOfKeyOrder(*this, layout, number, *place + createdFields.size()),
            };
        }
    }
    return std::nullopt;
}

std::optional<KeyOrderContinuation> KeyOrders::secondContinuationCreating(LayoutID layout, FieldIndex createdField) const
{
    if (!numberOfOrdersOf(layout))
        return std::nullopt;
    uint32_t first = m_firstContinuationOfLayout[layout.index()];
    if (createdField >= m_firstContinuationOfLayout[layout.index() + 1] - first || !m_secondContinuationCreatingField[first + createdField].orderWordAfter)
        return std::nullopt;
    return m_secondContinuationCreatingField[first + createdField];
}

void KeyOrders::indexFirstContinuations()
{
    size_t numberOfLayouts = m_parts.firstOrderOfLayout.isEmpty() ? 0 : m_parts.firstOrderOfLayout.size() - 1;
    m_firstContinuationOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    Vector<uint32_t, 64> firstOrderWithBeginning;
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID layout(static_cast<uint32_t>(index));
        size_t firstOfLayout = m_firstContinuationCreatingField.size();
        m_firstContinuationOfLayout.append(static_cast<uint32_t>(firstOfLayout));
        uint32_t numberOfOrders = numberOfOrdersOf(layout);
        for (uint32_t number = 0; number < numberOfOrders; ++number) {
            std::span<const FieldIndex> listed = order(layout, number);
            size_t shared = number ? lengthSharedByKeyOrders(order(layout, number - 1), listed) : 0;
            firstOrderWithBeginning.shrink(shared);
            while (firstOrderWithBeginning.size() < listed.size())
                firstOrderWithBeginning.append(number);
            for (size_t place = shared; place < listed.size(); ++place) {
                while (m_firstContinuationCreatingField.size() <= firstOfLayout + listed[place]) {
                    m_firstContinuationCreatingField.append(KeyOrderContinuation { });
                    m_secondContinuationCreatingField.append(KeyOrderContinuation { });
                }
                KeyOrderContinuation& first = m_firstContinuationCreatingField[firstOfLayout + listed[place]];
                KeyOrderContinuation& continuation = first.orderWordAfter ? m_secondContinuationCreatingField[firstOfLayout + listed[place]] : first;
                if (continuation.orderWordAfter)
                    continue;
                continuation.orderWordBefore = place ? orderWordOf(firstOrderWithBeginning[place - 1], static_cast<uint32_t>(place)) : unorderedOrderWord;
                continuation.orderWordAfter = orderWordOf(firstOrderWithBeginning[place], static_cast<uint32_t>(place + 1));
            }
        }
    }
    m_firstContinuationOfLayout.append(static_cast<uint32_t>(m_firstContinuationCreatingField.size()));
}

namespace {

struct CandidateForKeyOrders {
    LayoutID layout;
    uint32_t firstField { 0 };
    uint32_t numberOfFields { 0 };
    uint32_t numberOfFieldsByDeclaration { 0 };
    bool isDeclaredOrder { false };
};

struct CompleteOrdersOfSources {
    struct Order {
        uint32_t firstKey { 0 };
        uint32_t numberOfKeys { 0 };
    };

    std::span<const PropertyKeyID> keysOf(const Order& order) const { return keys.span().subspan(order.firstKey, order.numberOfKeys); }

    Vector<PropertyKeyID> keys;
    Vector<Order> orders;
    Vector<std::pair<PropertyKeyID, uint32_t>> ordersByRarestKey;
};

class CollectorOfKeyOrders {
    WTF_MAKE_NONCOPYABLE(CollectorOfKeyOrders);
public:
    CollectorOfKeyOrders(const Program& program, PlannedLayouts slots, const ParallelLoops& loops, std::span<uint32_t> numbersOfCandidates)
        : m_program(program)
        , m_slots(slots)
        , m_loops(loops)
        , m_numbersOfCandidates(numbersOfCandidates)
    {
    }

    void addDeclaredOrders();
    void addOrdersOfData();
    void addOrdersOfMakers(const LinkTables&);
    void addOrdersOfMembersMadeByTag(const LinkTables&, const BodyIndexTable&);
    void addOrdersOfConstructorFunctions(const LinkTables&, const ThinSummaries&, const BodyIndexTable&);
    void addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables&, const ThinSummaries&);
    void addOrdersOfAllocations(const LinkTables&, const ThinSummaries&);
    void addOrdersOfComputedLiterals(const LinkTables&);
    void addOrdersOfCopies(const LinkTables&, const ThinSummaries&, const BodyIndexTable&, BuildContext&);
    void addOrdersOfLibraryBases();
    KeyOrderParts sortIntoParts(BuildContext&);

private:
    void addOrdersOfLayoutsOnAllCores(const Invocable<void(CollectorOfKeyOrders&, uint32_t firstLayout, uint32_t endOfLayouts)> auto& addOrders)
    {
        static constexpr uint32_t layoutsInRange = 256;
        struct FoundInRange {
            Vector<CandidateForKeyOrders> candidates;
            Vector<FieldIndex> fields;
            Vector<FunctionID> methodsOfConstructions;
        };
        uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
        Vector<FoundInRange> found((numberOfLayouts + layoutsInRange - 1) / layoutsInRange);
        forEachIndexInParallel(m_loops, static_cast<unsigned>(found.size()), [&](unsigned range) {
            CollectorOfKeyOrders collectorOfRange(m_program, m_slots, m_loops, m_numbersOfCandidates);
            addOrders(collectorOfRange, range * layoutsInRange, std::min(numberOfLayouts, (range + 1) * layoutsInRange));
            found[range].candidates = WTF::move(collectorOfRange.m_candidates);
            found[range].fields = WTF::move(collectorOfRange.m_fields);
            found[range].methodsOfConstructions = WTF::move(collectorOfRange.m_methodsOfConstructions);
        });
        size_t numberOfCandidates = m_candidates.size();
        size_t numberOfFields = m_fields.size();
        for (const FoundInRange& inRange : found) {
            numberOfCandidates += inRange.candidates.size();
            numberOfFields += inRange.fields.size();
        }
        m_candidates.reserveCapacity(numberOfCandidates);
        m_fields.reserveCapacity(numberOfFields);
        for (const FoundInRange& inRange : found) {
            uint32_t fieldsInFront = static_cast<uint32_t>(m_fields.size());
            for (CandidateForKeyOrders candidate : inRange.candidates) {
                candidate.firstField += fieldsInFront;
                m_candidates.append(candidate);
            }
            m_fields.appendVector(inRange.fields);
            m_methodsOfConstructions.appendVector(inRange.methodsOfConstructions);
        }
    }

    void addDeclaredOrders(uint32_t firstLayout, uint32_t endOfLayouts);
    void addOrdersOfMakers(const LinkTables&, uint32_t firstLayout, uint32_t endOfLayouts);
    void addOrdersOfConstructorFunctions(const LinkTables&, const ThinSummaries&, const BodyIndexTable&, uint32_t firstLayout, uint32_t endOfLayouts);
    void addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables&, const ThinSummaries&, uint32_t firstLayout, uint32_t endOfLayouts);
    void sortCandidates();

    std::span<const FieldIndex> fieldsOf(const CandidateForKeyOrders& candidate) const { return m_fields.span().subspan(candidate.firstField, candidate.numberOfFields); }
    bool hasOrders(LayoutID id) const { return m_slots.layout(id).holdsCompiledObjects(); }
    static bool isListedInOrders(const PlannedLayout& planned, FieldIndex field) { return field < planned.numberOfFields() && planned.slots[field].indexInLayoutRecord && planned.environmentOfDeferredClosures != field; }

    void finishCandidate(LayoutID layout, size_t firstField) { finishCandidate(layout, firstField, m_fields.size() - firstField); }
    void finishCandidate(LayoutID layout, size_t firstField, size_t numberOfFieldsByDeclaration)
    {
        if (m_fields.size() == firstField)
            return;
        m_candidates.append({ layout, static_cast<uint32_t>(firstField), static_cast<uint32_t>(m_fields.size() - firstField), static_cast<uint32_t>(numberOfFieldsByDeclaration) });
        ++m_numbersOfCandidates[layout.index()];
    }
    void listFieldsThatLayoutLists(LayoutID);
    bool collectThisKeys(const LinkTables&, const ThinSummaries&, LayoutID, FunctionID, Vector<PropertyKeyID, 32>& keys, Vector<FunctionID, 8>& entered) const;
    void addOrderOfClassConstructor(const LinkTables&, const ThinSummaries&, LayoutID);
    void addDeclaredOrderOf(LayoutID);
    void addOrderOfDataObject(const DataNode::Object&);
    TypeID typeExpectedOfSourceOfCopy(const LinkTables&, FunctionID, const ThinSummary&, const SummaryNode&) const;
    class SourcesOfCopies;
    void collectCompleteOrdersOf(LayoutID source, std::span<const uint32_t> firstCandidateOfLayout, CompleteOrdersOfSources&) const;
    static void fileOrdersByRarestKey(CompleteOrdersOfSources&);
    void addOrdersOfCopy(LayoutID target, const CompleteOrdersOfSources&, std::span<const uint32_t> writtenBeforeCopy, std::span<const uint32_t> writtenBesideCopy, std::span<const uint32_t> keysLeftOut);
    void addOrderOfCopy(LayoutID target, std::span<const PropertyKeyID> keysOfSource, std::span<const uint32_t> writtenBeforeCopy, std::span<const uint32_t> writtenBesideCopy, std::span<const uint32_t> keysLeftOut);

    const Program& m_program;
    PlannedLayouts m_slots;
    const ParallelLoops& m_loops;
    Vector<CandidateForKeyOrders> m_candidates;
    std::span<uint32_t> m_numbersOfCandidates;
    Vector<FieldIndex> m_fields;
    Vector<FunctionID> m_methodsOfConstructions;
    BitVector m_isListed;
    Vector<LayoutID, 8> m_declaringLayouts;
    struct OrderOfSourceInTarget {
        LayoutID target;
        const PropertyKeyID* keysOfSource { nullptr };
        Vector<uint32_t> keysLeftOut;
        Vector<FieldIndex> fields;
        BitVector isListed;
        bool everyKeyIsListed { false };
        bool isBroughtWithoutOtherNames { false };
    };
    OrderOfSourceInTarget m_orderOfSourceFoundLast;
};

static void forEachListOfFieldsForKeyOrders(std::span<const uint32_t> lists, const Invocable<void(LayoutID, std::span<const FieldIndex>)> auto& functor)
{
    for (size_t next = 0; next < lists.size();) {
        std::span<const FieldIndex> fields = lists.subspan(next + 2, lists[next + 1]);
        functor(LayoutID(lists[next]), fields);
        next += 2 + fields.size();
    }
}

void CollectorOfKeyOrders::listFieldsThatLayoutLists(LayoutID id)
{
    const PlannedLayout& planned = m_slots.layout(id);
    const Layout& layout = m_program.at(id);
    m_isListed.clearAll();
    auto list = [&](FieldIndex field) {
        if (isListedInOrders(planned, field) && !m_isListed.set(field))
            m_fields.append(field);
    };

    m_declaringLayouts.shrink(0);
    for (LayoutID current = id; current && m_declaringLayouts.size() < m_program.layouts.size(); current = writtenBaseOf(m_program.at(current)))
        m_declaringLayouts.append(current);

    for (size_t index = m_declaringLayouts.size(); index--;) {
        LayoutID declaringLayout = m_declaringLayouts[index];
        const Layout& declaring = m_program.at(declaringLayout);
        if (declaring.has(LayoutFlags::HasHeritage) && !writtenBaseOf(declaring)) {
            for (const Field& field : declaring.fields) {
                if (std::optional<uint32_t> found = field.flags.contains(FieldFlags::IsInherited) ? layout.findField(field.key) : std::nullopt)
                    list(*found);
            }
        }
        auto listDeclaration = [&](uint32_t field) {
            if (field >= declaring.fields.size() || declaring.fields[field].flags.contains(FieldFlags::IsInherited))
                return;
            if (declaringLayout == id) {
                list(field);
                return;
            }
            if (std::optional<uint32_t> found = layout.findField(declaring.fields[field].key))
                list(*found);
        };
        for (uint32_t field : declaring.declaredOrder)
            listDeclaration(field);
        for (uint32_t field = 0; field < declaring.ownFieldCount; ++field)
            listDeclaration(field);
    }
    for (FieldIndex field = 0; field < layout.fields.size(); ++field)
        list(field);
}

void CollectorOfKeyOrders::addDeclaredOrderOf(LayoutID id)
{
    const PlannedLayout& planned = m_slots.layout(id);
    size_t firstField = m_fields.size();
    listFieldsThatLayoutLists(id);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (isListedInOrders(planned, field) && !m_isListed.set(field))
            m_fields.append(field);
    }
    size_t numberOfCandidatesBefore = m_candidates.size();
    finishCandidate(id, firstField);
    if (m_candidates.size() != numberOfCandidatesBefore)
        m_candidates.last().isDeclaredOrder = true;
}

void CollectorOfKeyOrders::addOrdersOfMakers(const LinkTables& link)
{
    addOrdersOfLayoutsOnAllCores([&](CollectorOfKeyOrders& collectorOfRange, uint32_t firstLayout, uint32_t endOfLayouts) {
        collectorOfRange.addOrdersOfMakers(link, firstLayout, endOfLayouts);
    });
}

void CollectorOfKeyOrders::addOrdersOfMakers(const LinkTables& link, uint32_t firstLayout, uint32_t endOfLayouts)
{
    for (uint32_t index = firstLayout; index < endOfLayouts; ++index) {
        LayoutID id(index);
        if (!hasOrders(id))
            continue;
        const PlannedLayout& planned = m_slots.layout(id);
        link.forEachRunOfMakers(id, [&](std::span<const PropertyKeyID> keys) {
            if (m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
                return;
            size_t firstField = m_fields.size();
            listFieldsThatLayoutLists(id);
            size_t numberOfFieldsByDeclaration = m_fields.size() - firstField;
            for (PropertyKeyID key : keys) {
                std::optional<FieldIndex> field = m_slots.findField(m_program, id, key);
                if (!field || !isListedInOrders(planned, *field) || m_isListed.set(*field))
                    break;
                m_fields.append(*field);
            }
            finishCandidate(id, firstField, numberOfFieldsByDeclaration);
        });
    }
}

void CollectorOfKeyOrders::addOrdersOfMembersMadeByTag(const LinkTables& link, const BodyIndexTable& bodies)
{
    if (link.runsBehindTaggedMakers.isEmpty())
        return;
    BitVector isMemberMadeByTag(m_program.layouts.size());
    for (const TaggedFamily& family : m_program.taggedFamilies) {
        for (const TaggedLayout& member : family.members)
            isMemberMadeByTag.quickSet(member.layout.index());
    }
    Vector<LayoutID, 4> membersWritten;
    Vector<bool, 4> severalAreWritten;
    for (const LinkTables::RunBehindTaggedMakers& run : link.runsBehindTaggedMakers) {
        const BodyIndex& index = bodies.at(run.body);
        if (index.isParameter(run.firstStore) || (index.opcodeOf(run.firstStore) != Opcode::PropSet && index.opcodeOf(run.firstStore) != Opcode::PropDefine))
            continue;
        TypeID written;
        for (ValueID name = index.value(run.firstStore).objectOperand(); name && !written && !index.isParameter(name) && inputPassThroughOpcodes.contains(index.opcodeOf(name)); name = index.value(name).operand(0)) {
            if (index.opcodeOf(name) == Opcode::Convert)
                written = index.value(name).as<Opcode::Convert>().declared().written();
        }
        if (!written || !m_program.at(written).hasObjects() || !m_program.at(written).layouts)
            continue;
        std::span<const PropertyKeyID> keys = link.keysOfRunsBehindTaggedMakers.span().subspan(run.firstKey, run.numberOfKeys);
        std::span<const LayoutID> makers = link.makersOfRunsBehindTaggedMakers.span().subspan(run.firstMaker, run.numberOfMakers);
        membersWritten.fill(LayoutID(), makers.size());
        severalAreWritten.fill(false, makers.size());
        m_program.at(m_program.at(written).layouts).forEach([&](LayoutID id) {
            if (!isMemberMadeByTag.quickGet(id.index()))
                return;
            size_t place = std::ranges::find(makers, writtenBaseOf(m_program.at(id))) - makers.begin();
            if (place == makers.size())
                return;
            severalAreWritten[place] = !!membersWritten[place];
            membersWritten[place] = id;
        });
        for (size_t place = 0; place < makers.size(); ++place) {
            LayoutID id = membersWritten[place];
            if (!id || severalAreWritten[place] || !hasOrders(id) || m_numbersOfCandidates[id.index()] >= maxKeyOrdersPerLayout)
                continue;
            const Layout& layout = m_program.at(id);
            const PlannedLayout& planned = m_slots.layout(id);
            size_t firstField = m_fields.size();
            listFieldsThatLayoutLists(id);
            size_t numberKept = firstField;
            for (size_t listed = firstField; listed < m_fields.size(); ++listed) {
                FieldIndex field = m_fields[listed];
                if (field < layout.fields.size() && layout.fields[field].flags.contains(FieldFlags::IsInherited))
                    m_fields[numberKept++] = field;
                else
                    m_isListed.clear(field);
            }
            m_fields.shrink(numberKept);
            size_t numberOfFieldsByDeclaration = m_fields.size() - firstField;
            for (PropertyKeyID key : keys) {
                std::optional<FieldIndex> field = m_slots.findField(m_program, id, key);
                if (!field || !isListedInOrders(planned, *field))
                    break;
                if (!m_isListed.set(*field))
                    m_fields.append(*field);
            }
            if (m_fields.size() - firstField == numberOfFieldsByDeclaration) {
                m_fields.shrink(firstField);
                continue;
            }
            finishCandidate(id, firstField, numberOfFieldsByDeclaration);
        }
    }
}

void CollectorOfKeyOrders::addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables& link, const ThinSummaries& summaries)
{
    addOrdersOfLayoutsOnAllCores([&](CollectorOfKeyOrders& collectorOfRange, uint32_t firstLayout, uint32_t endOfLayouts) {
        collectorOfRange.addOrdersBehindWhatConstructorFunctionsAlwaysCreate(link, summaries, firstLayout, endOfLayouts);
    });
}

void CollectorOfKeyOrders::addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables& link, const ThinSummaries& summaries, uint32_t firstLayout, uint32_t endOfLayouts)
{
    struct StoreOnThis {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<StoreOnThis, 32> stores;
    Vector<FieldIndex, 32> created;
    for (uint32_t index = firstLayout; index < endOfLayouts; ++index) {
        LayoutID id(index);
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor || !hasOrders(id))
            continue;
        const Function& function = m_program.at(constructor);
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        if (function.constructedClass != id || !function.takesThisFromConstruction() || !thisParameter || !link.function(constructor).isReached)
            continue;
        const ThinSummary& summary = summaries.at(constructor);
        const SummaryConstruction* construction = nullptr;
        for (const SummaryConstruction& candidate : summary.constructions) {
            if (candidate.object == SummaryConstruction::Object::This)
                construction = &candidate;
        }
        if (!construction)
            continue;
        std::span<const SummaryConstruction::Key> keys = summary.keysOfConstructions.span().subspan(construction->firstKey, construction->numberOfKeys);
        stores.shrink(0);
        bool leavesSomeOut = false;
        for (const SummaryAccess& access : summary.accesses) {
            if ((access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define) || access.isToContents())
                continue;
            const SummaryNode& subject = summary.nodes[access.subject];
            if (subject.kind != SummaryNode::Kind::Parameter || subject.first != *thisParameter)
                continue;
            PropertyKeyID key = access.key ? access.key : link.keyAt(constructor, access.site);
            auto found = std::ranges::find(keys, key, &SummaryConstruction::Key::key);
            if (key && found != keys.end() && found->isFilled)
                stores.append({ access.site, key });
            else
                leavesSomeOut = true;
        }
        if (!leavesSomeOut)
            continue;
        std::ranges::sort(stores, { }, &StoreOnThis::site);
        const PlannedLayout& planned = m_slots.layout(id);
        created.shrink(0);
        for (const StoreOnThis& store : stores) {
            std::optional<FieldIndex> field = m_slots.findField(m_program, id, store.key);
            if (!field || !isListedInOrders(planned, *field))
                break;
            if (!created.contains(*field))
                created.append(*field);
        }
        if (created.isEmpty())
            continue;
        auto beginWithWhatIsCreated = [&] {
            size_t firstField = m_fields.size();
            m_isListed.clearAll();
            for (FieldIndex field : created) {
                m_isListed.set(field);
                m_fields.append(field);
            }
            return firstField;
        };
        finishCandidate(id, beginWithWhatIsCreated());
        link.forEachRunOfMakers(id, [&](std::span<const PropertyKeyID> run) {
            if (m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
                return;
            size_t firstField = beginWithWhatIsCreated();
            for (PropertyKeyID key : run) {
                std::optional<FieldIndex> field = m_slots.findField(m_program, id, key);
                if (!field || !isListedInOrders(planned, *field) || m_isListed.set(*field))
                    break;
                m_fields.append(*field);
            }
            finishCandidate(id, firstField, created.size());
        });
    }
}

static bool isReachableInBody(const BodyIndex& body, BlockID from, BlockID to)
{
    BitVector visited(body.numberOfBlocks());
    Vector<BlockID, 16> worklist { from };
    visited.quickSet(from.index());
    while (!worklist.isEmpty()) {
        for (BodyTarget target : body.block(worklist.takeLast()).terminator().targets()) {
            if (target.block() == to)
                return true;
            if (!visited.quickSet(target.block().index()))
                worklist.append(target.block());
        }
    }
    return false;
}

bool CollectorOfKeyOrders::collectThisKeys(const LinkTables& link, const ThinSummaries& summaries, LayoutID layoutOfThis, FunctionID function, Vector<PropertyKeyID, 32>& keys, Vector<FunctionID, 8>& entered) const
{
    const Function& record = m_program.at(function);
    LayoutID constructed = isClassConstructorOf(record.kind) ? record.constructedClass : LayoutID();
    std::optional<unsigned> thisParameter = record.kind == FunctionKind::Initializer ? std::optional<unsigned>(0) : record.thisParameterIndex();
    if ((!thisParameter && !constructed) || entered.contains(function))
        return false;
    entered.append(function);
    struct Event {
        ValueID site;
        PropertyKeyID key;
        FunctionID method;
    };
    const ThinSummary& summary = summaries.at(function);
    auto isConstructedLayout = [&](LayoutID layout) {
        size_t stepsLeft = m_program.layouts.size();
        while (layout && layout != constructed && stepsLeft--)
            layout = m_program.at(layout).base;
        return layout == constructed;
    };
    auto isSuperCall = [&](const SummaryCall& call) {
        return call.isConstruction && call.newTarget != noSummaryNode;
    };
    auto isParameter = [&](uint32_t node) {
        const SummaryNode& origin = summary.nodes[node];
        if (!constructed)
            return origin.kind == SummaryNode::Kind::Parameter && origin.first == *thisParameter;
        if (origin.kind == SummaryNode::Kind::Allocation)
            return isConstructedLayout(LayoutID(origin.first));
        return origin.kind == SummaryNode::Kind::ResultOfCall && isSuperCall(summary.calls[origin.first]);
    };
    auto isThis = [&](uint32_t node) {
        if (node == noSummaryNode || isParameter(node))
            return node != noSummaryNode;
        bool isConverted = false;
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != node)
                continue;
            if (!isParameter(edge.from))
                return false;
            isConverted = true;
        }
        return isConverted && summary.nodes[node].kind == SummaryNode::Kind::Join;
    };
    Vector<Event, 32> events;
    for (const SummaryAccess& access : summary.accesses) {
        if ((access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define) && !access.isToContents() && isThis(access.subject))
            events.append({ access.site, access.key ? access.key : link.keyAt(function, access.site), { } });
    }
    for (const SummaryCall& call : summary.calls) {
        if (constructed && isSuperCall(call)) {
            LayoutID parent = m_program.at(constructed).base;
            if (FunctionID above = parent ? m_program.at(parent).constructor : FunctionID())
                events.append({ call.site, { }, above });
            continue;
        }
        if (call.callee == SummaryCall::Callee::Binding && call.function && m_program.at(call.function).kind == FunctionKind::Initializer && call.numberOfArguments && isThis(summary.arguments[call.firstArgument])) {
            events.append({ call.site, { }, call.function });
            continue;
        }
        if (call.isConstruction || !isThis(call.receiver))
            continue;
        FunctionID method = call.callee == SummaryCall::Callee::Binding ? call.function : link.calleeAt(function, call.site);
        if (!method && call.callee == SummaryCall::Callee::Name)
            method = link.findWrittenMethod(m_program, layoutOfThis, call.key);
        if (method)
            events.append({ call.site, { }, method });
    }
    std::ranges::sort(events, { }, &Event::site);
    bool callsMethods = false;
    for (const Event& event : events) {
        if (event.method) {
            callsMethods = true;
            collectThisKeys(link, summaries, layoutOfThis, event.method, keys, entered);
        } else
            keys.append(event.key);
    }
    return callsMethods;
}

void CollectorOfKeyOrders::addOrderOfClassConstructor(const LinkTables& link, const ThinSummaries& summaries, LayoutID id)
{
    Vector<PropertyKeyID, 32> keys;
    Vector<FunctionID, 8> entered;
    collectThisKeys(link, summaries, id, m_program.at(id).constructor, keys, entered);
    for (FunctionID function : entered) {
        FunctionKind kind = m_program.at(function).kind;
        if (kind != FunctionKind::Initializer && !isClassConstructorOf(kind))
            m_methodsOfConstructions.append(function);
    }
    const PlannedLayout& planned = m_slots.layout(id);
    size_t firstField = m_fields.size();
    m_isListed.clearAll();
    for (PropertyKeyID key : keys) {
        std::optional<FieldIndex> field = key ? m_slots.findField(m_program, id, key) : std::nullopt;
        if (!field || !isListedInOrders(planned, *field))
            break;
        if (!m_isListed.set(*field))
            m_fields.append(*field);
    }
    size_t endOfStored = m_fields.size();
    if (endOfStored == firstField)
        return;
    listFieldsThatLayoutLists(id);
    size_t endOfDeclared = m_fields.size();
    bool isDeclaredOrder = endOfDeclared - endOfStored >= endOfStored - firstField && std::ranges::equal(m_fields.span().subspan(firstField, endOfStored - firstField), m_fields.span().subspan(endOfStored, endOfStored - firstField));
    if (isDeclaredOrder) {
        m_fields.shrink(firstField);
        return;
    }
    for (size_t place = endOfStored; place < endOfDeclared; ++place) {
        FieldIndex field = m_fields[place];
        if (!std::ranges::contains(m_fields.span().subspan(firstField, endOfStored - firstField), field))
            m_fields[endOfStored++] = field;
    }
    m_fields.shrink(endOfStored);
    finishCandidate(id, firstField);
}

void CollectorOfKeyOrders::addOrdersOfConstructorFunctions(const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies)
{
    addOrdersOfLayoutsOnAllCores([&](CollectorOfKeyOrders& collectorOfRange, uint32_t firstLayout, uint32_t endOfLayouts) {
        collectorOfRange.addOrdersOfConstructorFunctions(link, summaries, bodies, firstLayout, endOfLayouts);
    });
}

void CollectorOfKeyOrders::addOrdersOfConstructorFunctions(const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies, uint32_t firstLayout, uint32_t endOfLayouts)
{
    struct StoreOnThis {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<StoreOnThis, 32> stores;
    Vector<FieldIndex, 32> firstPath;
    for (uint32_t index = firstLayout; index < endOfLayouts; ++index) {
        LayoutID id(index);
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor || !hasOrders(id) || m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
            continue;
        const Function& function = m_program.at(constructor);
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        if (function.constructedClass != id || !link.function(constructor).isReached)
            continue;
        if (isClassConstructorOf(function.kind))
            addOrderOfClassConstructor(link, summaries, id);
        if (!function.takesThisFromConstruction() || !thisParameter)
            continue;

        const ThinSummary& summary = summaries.at(constructor);
        stores.shrink(0);
        for (const SummaryAccess& access : summary.accesses) {
            if ((access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define) || access.isToContents())
                continue;
            const SummaryNode& subject = summary.nodes[access.subject];
            if (subject.kind == SummaryNode::Kind::Parameter && subject.first == *thisParameter)
                stores.append({ access.site, access.key ? access.key : link.keyAt(constructor, access.site) });
        }
        std::ranges::sort(stores, { }, &StoreOnThis::site);

        const PlannedLayout& planned = m_slots.layout(id);
        size_t firstField = m_fields.size();
        m_isListed.clearAll();
        for (const StoreOnThis& store : stores) {
            std::optional<FieldIndex> field = store.key ? m_slots.findField(m_program, id, store.key) : std::nullopt;
            if (!field || !isListedInOrders(planned, *field))
                break;
            if (!m_isListed.set(*field))
                m_fields.append(*field);
        }
        finishCandidate(id, firstField);

        Vector<PropertyKeyID, 32> keys;
        Vector<FunctionID, 8> entered;
        if (collectThisKeys(link, summaries, id, constructor, keys, entered) && m_numbersOfCandidates[index] < maxKeyOrdersPerLayout) {
            size_t firstFieldOfMethods = m_fields.size();
            m_isListed.clearAll();
            for (PropertyKeyID key : keys) {
                std::optional<FieldIndex> field = key ? m_slots.findField(m_program, id, key) : std::nullopt;
                if (!field || !isListedInOrders(planned, *field))
                    break;
                if (!m_isListed.set(*field))
                    m_fields.append(*field);
            }
            finishCandidate(id, firstFieldOfMethods);
        }

        const BodyIndex& body = bodies.at(constructor);
        firstPath.shrink(0);
        BlockID lastBlock;
        for (const StoreOnThis& store : stores) {
            std::optional<FieldIndex> field = store.key ? m_slots.findField(m_program, id, store.key) : std::nullopt;
            if (!field || !isListedInOrders(planned, *field))
                break;
            BlockID block = body.blockOf(store.site);
            if (lastBlock && block != lastBlock && !isReachableInBody(body, lastBlock, block))
                continue;
            lastBlock = block;
            if (!firstPath.contains(*field))
                firstPath.append(*field);
        }
        link.forEachRunOfMakers(id, [&](std::span<const PropertyKeyID> run) {
            if (m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
                return;
            size_t firstFieldOfRun = m_fields.size();
            m_isListed.clearAll();
            for (FieldIndex field : firstPath) {
                m_isListed.set(field);
                m_fields.append(field);
            }
            for (PropertyKeyID key : run) {
                std::optional<FieldIndex> field = m_slots.findField(m_program, id, key);
                if (!field || !isListedInOrders(planned, *field) || m_isListed.set(*field))
                    break;
                m_fields.append(*field);
            }
            finishCandidate(id, firstFieldOfRun, firstPath.size());
        });
    }
}

void CollectorOfKeyOrders::addOrdersOfAllocations(const LinkTables& link, const ThinSummaries& summaries)
{
    Vector<Vector<uint32_t>> listsOfChunks = collectChunksInParallel<uint32_t>(m_loops, static_cast<unsigned>(m_program.functions.size()), [&](unsigned number, Vector<uint32_t>& lists) {
        if (!link.function(FunctionID(number)).isReached)
            return;
        BitVector isListed;
        summaries.at(FunctionID(number)).forEachListOfAllocation([&](LayoutID id, std::span<const uint32_t> listed) {
            if (!hasOrders(id))
                return;
            const PlannedLayout& planned = m_slots.layout(id);
            size_t start = lists.size();
            lists.append(id.index());
            lists.append(0);
            isListed.clearAll();
            for (uint32_t field : listed) {
                if (field & ThinSummary::keyInListOfAllocation) {
                    std::optional<FieldIndex> found = m_slots.findField(m_program, id, PropertyKeyID(field & ~ThinSummary::keyInListOfAllocation));
                    if (!found)
                        break;
                    field = *found;
                }
                if (!isListedInOrders(planned, field))
                    continue;
                if (isListed.set(field)) {
                    lists.shrink(start);
                    return;
                }
                lists.append(field);
            }
            lists[start + 1] = static_cast<uint32_t>(lists.size() - start - 2);
        });
    });
    for (const Vector<uint32_t>& lists : listsOfChunks) {
        forEachListOfFieldsForKeyOrders(lists.span(), [&](LayoutID id, std::span<const FieldIndex> fields) {
            if (!hasOrders(id) || m_numbersOfCandidates[id.index()] >= maxKeyOrdersPerLayout)
                return;
            size_t firstField = m_fields.size();
            m_fields.append(fields);
            finishCandidate(id, firstField);
        });
    }
}

void CollectorOfKeyOrders::addOrdersOfComputedLiterals(const LinkTables& link)
{
    for (size_t index : link.computedLiterals) {
        LayoutID id(static_cast<uint32_t>(index));
        if (!hasOrders(id) || m_slots.layout(id).numberOfFields() > mostNamesOfComputedLiteral)
            continue;
        const PlannedLayout& planned = m_slots.layout(id);
        size_t firstField = m_fields.size();
        listFieldsThatLayoutLists(id);
        Vector<FieldIndex, mostNamesOfComputedLiteral> written(m_fields.span().subspan(firstField));
        m_fields.shrink(firstField);
        for (FieldIndex name = 0; name < planned.numberOfFields(); ++name) {
            if (!isListedInOrders(planned, name))
                continue;
            Vector<FieldIndex, mostNamesOfComputedLiteral> others(written);
            others.removeFirst(name);
            for (size_t place = 0; place <= others.size() && m_numbersOfCandidates[index] < maxKeyOrdersPerLayout; ++place) {
                firstField = m_fields.size();
                m_fields.append(others.span().first(place));
                m_fields.append(name);
                m_fields.append(others.span().subspan(place));
                finishCandidate(id, firstField);
            }
        }
    }
}

TypeID CollectorOfKeyOrders::typeExpectedOfSourceOfCopy(const LinkTables& link, FunctionID function, const ThinSummary& summary, const SummaryNode& source) const
{
    switch (source.kind) {
    case SummaryNode::Kind::Parameter: {
        std::span<const LinkedParameter> parameters = link.parametersOf(function);
        return source.first < parameters.size() ? parameters[source.first].writtenOrSeenToArrive() : TypeID();
    }
    case SummaryNode::Kind::Field: {
        const Layout& holder = m_program.at(LayoutID(source.first));
        return source.second < holder.fields.size() ? holder.fields[source.second].declared.written() : TypeID();
    }
    case SummaryNode::Kind::Variable:
        return m_program.at(VariableID(source.first)).declared.written();
    case SummaryNode::Kind::ResultOfCall: {
        const SummaryCall& call = summary.calls[source.first];
        FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : link.calleeAt(function, call.site);
        return callee && !call.isConstruction && !m_program.at(callee).isCoroutine() ? link.function(callee).result : TypeID();
    }
    default:
        return { };
    }
}

class CollectorOfKeyOrders::SourcesOfCopies {
public:
    SourcesOfCopies(const CollectorOfKeyOrders& collector, const LinkTables& link, FunctionID function, const ThinSummary& summary)
        : m_collector(collector)
        , m_link(link)
        , m_function(function)
        , m_summary(summary)
    {
    }

    void forEach(uint32_t node, NOESCAPE const Invocable<void(LayoutID)> auto& forAllocation, NOESCAPE const Invocable<void(LayoutSetID)> auto& forLayouts)
    {
        Vector<uint32_t, 8> waiting { node };
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> joinsSeen;
        while (!waiting.isEmpty()) {
            uint32_t next = waiting.takeLast();
            const SummaryNode& source = m_summary.nodes[next];
            if (source.kind == SummaryNode::Kind::Allocation) {
                forAllocation(LayoutID(source.first));
                continue;
            }
            if (source.kind == SummaryNode::Kind::Join) {
                if (joinsSeen.add(next).isNewEntry)
                    appendNodesThatArriveAt(next, waiting);
                continue;
            }
            bool isNamedAtCopy = next == node && source.kind != SummaryNode::Kind::ResultOfAccess;
            forEachLayoutSet(source, [&](LayoutSetID layouts) {
                if (isNamedAtCopy || m_collector.m_program.at(layouts).size() <= mostLayoutsThatCopyIsBuiltFor)
                    forLayouts(layouts);
            });
        }
    }

private:
    void appendNodesThatArriveAt(uint32_t join, Vector<uint32_t, 8>& nodes)
    {
        if (m_numberOfJoinsAsked++ < maxJoinsSearchedByEdges) {
            for (const SummaryEdge& edge : m_summary.edges) {
                if (edge.to == join)
                    nodes.append(edge.from);
            }
            return;
        }
        if (m_firstArrivalAtNode.isEmpty()) {
            m_firstArrivalAtNode.fill(0, m_summary.nodes.size() + 2);
            for (const SummaryEdge& edge : m_summary.edges)
                ++m_firstArrivalAtNode[edge.to + 2];
            for (size_t node = 2; node < m_firstArrivalAtNode.size(); ++node)
                m_firstArrivalAtNode[node] += m_firstArrivalAtNode[node - 1];
            m_arrivals.fill(0, m_summary.edges.size());
            for (const SummaryEdge& edge : m_summary.edges)
                m_arrivals[m_firstArrivalAtNode[edge.to + 1]++] = edge.from;
        }
        uint32_t first = m_firstArrivalAtNode[join];
        nodes.append(m_arrivals.span().subspan(first, m_firstArrivalAtNode[join + 1] - first));
    }

    void forEachLayoutSet(const SummaryNode& source, NOESCAPE const Invocable<void(LayoutSetID)> auto& functor) const
    {
        const Program& program = m_collector.m_program;
        auto tell = [&](TypeID type) {
            if (LayoutSetID layouts = type ? program.at(type).layouts : LayoutSetID())
                functor(layouts);
        };
        if (source.kind != SummaryNode::Kind::ResultOfAccess)
            return tell(m_collector.typeExpectedOfSourceOfCopy(m_link, m_function, m_summary, source));
        const SummaryAccess& read = m_summary.accesses[source.first];
        TypeID holder = read.kind == SummaryAccess::Kind::Read && read.key ? m_collector.typeExpectedOfSourceOfCopy(m_link, m_function, m_summary, m_summary.nodes[read.subject]) : TypeID();
        if (!holder)
            return;
        for (LayoutID layout : membersOf(TypeReader(program), program.at(holder))) {
            if (std::optional<FieldReference> field = findField(program, layout, read.key))
                tell(fieldAt(program, declarationOf(program, *field)).declared.written());
        }
    }

    const CollectorOfKeyOrders& m_collector;
    const LinkTables& m_link;
    FunctionID m_function;
    const ThinSummary& m_summary;
    static constexpr size_t mostLayoutsThatCopyIsBuiltFor = 2;
    static constexpr unsigned maxJoinsSearchedByEdges = 4;
    unsigned m_numberOfJoinsAsked { 0 };
    Vector<uint32_t> m_firstArrivalAtNode;
    Vector<uint32_t> m_arrivals;
};

void CollectorOfKeyOrders::collectCompleteOrdersOf(LayoutID source, std::span<const uint32_t> firstCandidateOfLayout, CompleteOrdersOfSources& found) const
{
    const Layout& declaredSource = m_program.at(source);
    if (declaredSource.kind != LayoutKind::Record || !hasOrders(source))
        return;
    uint32_t endOfCandidates = firstCandidateOfLayout[source.index() + 1];
    for (uint32_t position = firstCandidateOfLayout[source.index()]; position < endOfCandidates; ++position) {
        const CandidateForKeyOrders& order = m_candidates[position];
        if (position + 1 < endOfCandidates && lengthSharedByKeyOrders(fieldsOf(order), fieldsOf(m_candidates[position + 1])) == order.numberOfFields)
            continue;
        if (order.numberOfFields != declaredSource.fields.size())
            continue;
        if (std::ranges::any_of(fieldsOf(order), [&](FieldIndex index) { return index >= declaredSource.fields.size(); }))
            continue;
        found.orders.append({ static_cast<uint32_t>(found.keys.size()), order.numberOfFields });
        for (FieldIndex index : fieldsOf(order))
            found.keys.append(declaredSource.fields[index].key);
    }
}

void CollectorOfKeyOrders::fileOrdersByRarestKey(CompleteOrdersOfSources& found)
{
    std::ranges::sort(found.orders, [&](const auto& first, const auto& second) {
        return std::ranges::lexicographical_compare(found.keysOf(first), found.keysOf(second));
    });
    found.orders.shrink(std::unique(found.orders.begin(), found.orders.end(), [&](const auto& first, const auto& second) {
        return std::ranges::equal(found.keysOf(first), found.keysOf(second));
    }) - found.orders.begin());

    Vector<PropertyKeyID> keysOfAllOrders;
    for (const CompleteOrdersOfSources::Order& order : found.orders)
        keysOfAllOrders.append(found.keysOf(order));
    AOT::sort(keysOfAllOrders);
    auto numberOfOrdersWith = [&](PropertyKeyID key) {
        return std::ranges::equal_range(keysOfAllOrders, key).size();
    };
    found.ordersByRarestKey.reserveInitialCapacity(found.orders.size());
    for (uint32_t order = 0; order < found.orders.size(); ++order) {
        std::span<const PropertyKeyID> keys = found.keysOf(found.orders[order]);
        if (keys.empty())
            continue;
        found.ordersByRarestKey.append({ *std::ranges::min_element(keys, { }, [&](PropertyKeyID key) { return std::pair(numberOfOrdersWith(key), key); }), order });
    }
    AOT::sort(found.ordersByRarestKey);
}

void CollectorOfKeyOrders::addOrderOfCopy(LayoutID target, std::span<const PropertyKeyID> keysOfSource, std::span<const uint32_t> writtenBeforeCopy, std::span<const uint32_t> writtenBesideCopy, std::span<const uint32_t> keysLeftOut)
{
    if (m_numbersOfCandidates[target.index()] >= maxKeyOrdersPerLayout)
        return;
    OrderOfSourceInTarget& found = m_orderOfSourceFoundLast;
    if (found.target != target || found.keysOfSource != keysOfSource.data() || !std::ranges::equal(found.keysLeftOut, keysLeftOut)) {
        const PlannedLayout& planned = m_slots.layout(target);
        found.target = target;
        found.keysOfSource = keysOfSource.data();
        found.keysLeftOut.shrink(0);
        found.keysLeftOut.append(keysLeftOut);
        found.fields.shrink(0);
        found.isListed.clearAll();
        found.isBroughtWithoutOtherNames = false;
        found.everyKeyIsListed = std::ranges::all_of(keysOfSource, [&](PropertyKeyID key) {
            if (std::ranges::binary_search(keysLeftOut, key.bits()))
                return true;
            std::optional<FieldIndex> field = m_slots.findField(m_program, target, key);
            if (!field || !isListedInOrders(planned, *field))
                return false;
            if (!found.isListed.set(*field))
                found.fields.append(*field);
            return true;
        });
    }
    if (!found.everyKeyIsListed)
        return;
    if (!writtenBeforeCopy.empty()) {
        size_t firstField = m_fields.size();
        m_isListed.clearAll();
        for (std::span<const uint32_t> fields : { writtenBeforeCopy, std::span<const uint32_t>(found.fields.span()), writtenBesideCopy }) {
            for (FieldIndex field : fields) {
                if (!m_isListed.set(field))
                    m_fields.append(field);
            }
        }
        finishCandidate(target, firstField);
        return;
    }
    bool writesOtherNames = std::ranges::any_of(writtenBesideCopy, [&](FieldIndex field) { return !found.isListed.get(field); });
    if (!writesOtherNames && std::exchange(found.isBroughtWithoutOtherNames, true))
        return;
    size_t firstField = m_fields.size();
    m_fields.appendVector(found.fields);
    if (writesOtherNames) {
        m_isListed = found.isListed;
        for (FieldIndex field : writtenBesideCopy) {
            if (!m_isListed.set(field))
                m_fields.append(field);
        }
    }
    finishCandidate(target, firstField);
}

void CollectorOfKeyOrders::addOrdersOfCopy(LayoutID target, const CompleteOrdersOfSources& sources, std::span<const uint32_t> writtenBeforeCopy, std::span<const uint32_t> writtenBesideCopy, std::span<const uint32_t> keysLeftOut)
{
    const Layout& layout = m_program.at(target);
    if (sources.ordersByRarestKey.size() < layout.fields.size()) {
        const auto& appendedKeys = m_slots.layout(target).appendedKeys;
        Vector<std::pair<size_t, uint32_t>, 4> ordersByPlaceOfRarestKey;
        for (auto [rarestKey, order] : sources.ordersByRarestKey) {
            auto leftOut = std::ranges::lower_bound(keysLeftOut, rarestKey.bits());
            if (leftOut != keysLeftOut.end() && *leftOut == rarestKey.bits())
                ordersByPlaceOfRarestKey.append({ layout.fields.size() + appendedKeys.size() + (leftOut - keysLeftOut.begin()), order });
            else if (std::optional<uint32_t> field = layout.findField(rarestKey))
                ordersByPlaceOfRarestKey.append({ *field, order });
            else if (size_t place = appendedKeys.find(rarestKey); place != notFound)
                ordersByPlaceOfRarestKey.append({ layout.fields.size() + place, order });
        }
        std::ranges::stable_sort(ordersByPlaceOfRarestKey, { }, &std::pair<size_t, uint32_t>::first);
        for (auto [place, order] : ordersByPlaceOfRarestKey)
            addOrderOfCopy(target, sources.keysOf(sources.orders[order]), writtenBeforeCopy, writtenBesideCopy, keysLeftOut);
        return;
    }
    auto addOrdersFiledUnder = [&](PropertyKeyID key) {
        for (auto [rarestKey, order] : std::ranges::equal_range(sources.ordersByRarestKey, key, { }, &std::pair<PropertyKeyID, uint32_t>::first))
            addOrderOfCopy(target, sources.keysOf(sources.orders[order]), writtenBeforeCopy, writtenBesideCopy, keysLeftOut);
    };
    for (const Field& field : m_program.at(target).fields) {
        if (!std::ranges::binary_search(keysLeftOut, field.key.bits()))
            addOrdersFiledUnder(field.key);
    }
    for (PropertyKeyID key : m_slots.layout(target).appendedKeys) {
        if (!std::ranges::binary_search(keysLeftOut, key.bits()))
            addOrdersFiledUnder(key);
    }
    for (uint32_t key : keysLeftOut)
        addOrdersFiledUnder(PropertyKeyID::fromBits(key));
}

void CollectorOfKeyOrders::addOrdersOfCopies(const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies, BuildContext& build)
{
    static constexpr uint64_t sourceIsSetOfLayouts = 1ull << 32;
    struct Copy {
        LayoutID target;
        uint64_t source { 0 };
        uint32_t firstInFront { 0 };
        uint32_t numberInFront { 0 };
        uint32_t numberWritten { 0 };
        uint32_t numberLeftOut { 0 };
    };
    struct CopyInFunction {
        auto operator<=>(const CopyInFunction&) const = default;

        LayoutID target;
        uint64_t source { 0 };
        ValueID site;
    };
    Vector<uint32_t> words = collectInParallel<uint32_t>(m_loops, static_cast<unsigned>(m_program.functions.size()), [&](unsigned number, Vector<uint32_t>& words) {
        FunctionID function(number);
        if (!link.function(function).isReached)
            return;
        const ThinSummary& summary = summaries.at(function);
        Vector<CopyInFunction, 4> copies;
        SourcesOfCopies sourcesOfCopies(*this, link, function, summary);
        for (const SummaryChange& change : summary.changes) {
            if (change.kind != SummaryChange::Kind::CopiesInto || change.other == noSummaryNode || summary.nodes[change.subject].kind != SummaryNode::Kind::Allocation)
                continue;
            LayoutID target(summary.nodes[change.subject].first);
            if (m_program.at(target).kind != LayoutKind::Record || !hasOrders(target))
                continue;
            sourcesOfCopies.forEach(change.other, [&](LayoutID allocated) {
                copies.append({ target, allocated.index(), change.site });
            }, [&](LayoutSetID layouts) {
                copies.append({ target, layouts.bits() | sourceIsSetOfLayouts, change.site });
            });
        }
        if (copies.isEmpty())
            return;
        AOT::sort(copies);
        copies.shrink(std::unique(copies.begin(), copies.end()) - copies.begin());
        struct ListOfTarget {
            std::span<const uint32_t> listed;
            bool isWritten { false };
            bool severalAreWritten { false };
        };
        Vector<ListOfTarget, 4> lists(copies.size());
        summary.forEachListOfAllocation([&](LayoutID id, std::span<const uint32_t> listed) {
            auto first = std::ranges::lower_bound(copies, id, { }, &CopyInFunction::target);
            if (first == copies.end() || first->target != id)
                return;
            ListOfTarget& list = lists[first - copies.begin()];
            list.severalAreWritten |= list.isWritten && !std::ranges::equal(list.listed, listed);
            list.isWritten = true;
            list.listed = listed;
        });
        const BodyIndex& body = bodies.at(function);
        auto literalFilledBy = [&](ValueID site) {
            BodyValue copy = body.value(site);
            ValueID made = copy.opcode() == Opcode::CopyDataProperties ? body.stripCasts(copy.as<Opcode::CopyDataProperties>().targetOperand()) : ValueID();
            return made && !body.isParameter(made) && body.opcodeOf(made) == Opcode::NewObject ? made : ValueID();
        };
        HashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstCopyIntoLiteral;
        for (const CopyInFunction& copy : copies) {
            ValueID literal = literalFilledBy(copy.site);
            if (!literal || body.value(literal).as<Opcode::NewObject>().fields().empty())
                continue;
            ValueID& first = firstCopyIntoLiteral.add(literal.index(), copy.site).iterator->value;
            first = std::min(first, copy.site);
        }
        size_t placeOfTarget = 0;
        for (size_t place = 0; place < copies.size(); ++place) {
            auto [target, source, site] = copies[place];
            if (copies[placeOfTarget].target != target)
                placeOfTarget = place;
            const PlannedLayout& planned = m_slots.layout(target);
            size_t start = words.size();
            words.appendList({ target.index(), static_cast<uint32_t>(source >> 32), static_cast<uint32_t>(source), 0u, 0u, 0u });
            BodyValue operation = body.value(site);
            if (ValueID literal = firstCopyIntoLiteral.isEmpty() ? ValueID() : literalFilledBy(site); literal && firstCopyIntoLiteral.get(literal.index()) == site) {
                for (uint32_t field : body.value(literal).as<Opcode::NewObject>().fields()) {
                    if (isListedInOrders(planned, field))
                        words.append(field);
                }
            }
            words[start + 3] = static_cast<uint32_t>(words.size() - start - 6);
            size_t firstWritten = words.size();
            const ListOfTarget& list = lists[placeOfTarget];
            std::span<const uint32_t> written = list.isWritten ? list.listed : m_program.at(target).declaredOrder.span();
            for (uint32_t field : list.severalAreWritten ? std::span<const uint32_t>() : written) {
                if (field & ThinSummary::keyInListOfAllocation) {
                    std::optional<FieldIndex> found = m_slots.findField(m_program, target, PropertyKeyID(field & ~ThinSummary::keyInListOfAllocation));
                    if (!found)
                        break;
                    field = *found;
                }
                if (isListedInOrders(planned, field))
                    words.append(field);
            }
            words[start + 4] = static_cast<uint32_t>(words.size() - firstWritten);
            if (operation.opcode() != Opcode::CopyDataProperties)
                continue;
            size_t firstLeftOut = words.size();
            for (PropertyKeyID key : operation.as<Opcode::CopyDataProperties>().excludedKeys())
                words.append(key.bits());
            std::ranges::sort(words.mutableSpan().subspan(firstLeftOut));
            words[start + 5] = static_cast<uint32_t>(words.size() - firstLeftOut);
        }
    });
    Vector<Copy> copies;
    for (uint32_t next = 0; next < words.size(); next += 6 + words[next + 3] + words[next + 4] + words[next + 5])
        copies.append({ LayoutID(words[next]), static_cast<uint64_t>(words[next + 1]) << 32 | words[next + 2], next + 6, words[next + 3], words[next + 4], words[next + 5] });
    auto writtenInFront = [&](const Copy& copy) {
        return words.span().subspan(copy.firstInFront, copy.numberInFront);
    };
    auto writtenBeside = [&](const Copy& copy) {
        return words.span().subspan(copy.firstInFront + copy.numberInFront, copy.numberWritten);
    };
    auto leftOut = [&](const Copy& copy) {
        return words.span().subspan(copy.firstInFront + copy.numberInFront + copy.numberWritten, copy.numberLeftOut);
    };
    std::ranges::sort(copies, [&](const Copy& first, const Copy& second) {
        if (first.target != second.target)
            return first.target < second.target;
        if (first.source != second.source)
            return first.source < second.source;
        if (!std::ranges::equal(writtenInFront(first), writtenInFront(second)))
            return std::ranges::lexicographical_compare(writtenInFront(first), writtenInFront(second));
        if (!std::ranges::equal(writtenBeside(first), writtenBeside(second)))
            return std::ranges::lexicographical_compare(writtenBeside(first), writtenBeside(second));
        return std::ranges::lexicographical_compare(leftOut(first), leftOut(second));
    });
    copies.shrink(std::unique(copies.begin(), copies.end(), [&](const Copy& first, const Copy& second) {
        return first.target == second.target && first.source == second.source && std::ranges::equal(writtenInFront(first), writtenInFront(second)) && std::ranges::equal(writtenBeside(first), writtenBeside(second)) && std::ranges::equal(leftOut(first), leftOut(second));
    }) - copies.begin());

    bool someLayoutMayHaveTooManyOrders = std::ranges::any_of(m_numbersOfCandidates, [](uint32_t numberOfCandidates) {
        return numberOfCandidates > maxKeyOrdersPerLayout;
    });
    if (someLayoutMayHaveTooManyOrders)
        sortIntoParts(build);
    if (copies.isEmpty())
        return;
    if (!someLayoutMayHaveTooManyOrders)
        sortCandidates();

    Vector<uint32_t> firstCandidateOfLayout;
    firstCandidateOfLayout.reserveInitialCapacity(m_numbersOfCandidates.size() + 1);
    uint32_t numberOfCandidatesInFront = 0;
    for (uint32_t numberOfCandidates : m_numbersOfCandidates) {
        firstCandidateOfLayout.append(numberOfCandidatesInFront);
        numberOfCandidatesInFront += numberOfCandidates;
    }
    firstCandidateOfLayout.append(numberOfCandidatesInFront);
    RELEASE_ASSERT(numberOfCandidatesInFront == m_candidates.size());

    Vector<uint64_t> sources;
    sources.appendContainerWithMapping(copies, [](const Copy& copy) { return copy.source; });
    AOT::sort(sources);
    sources.shrink(std::unique(sources.begin(), sources.end()) - sources.begin());
    Vector<CompleteOrdersOfSources> ordersOfSources(sources.size());
    for (size_t place = 0; place < sources.size(); ++place) {
        CompleteOrdersOfSources& found = ordersOfSources[place];
        if (sources[place] & sourceIsSetOfLayouts) {
            m_program.at(LayoutSetID::fromBits(static_cast<uint32_t>(sources[place]))).forEach([&](LayoutID member) {
                collectCompleteOrdersOf(member, firstCandidateOfLayout.span(), found);
            });
        } else
            collectCompleteOrdersOf(LayoutID(static_cast<uint32_t>(sources[place])), firstCandidateOfLayout.span(), found);
        fileOrdersByRarestKey(found);
    }
    m_orderOfSourceFoundLast = { };
    for (const Copy& copy : copies)
        addOrdersOfCopy(copy.target, ordersOfSources[std::ranges::lower_bound(sources, copy.source) - sources.begin()], writtenInFront(copy), writtenBeside(copy), leftOut(copy));
}

void CollectorOfKeyOrders::addDeclaredOrders()
{
    addOrdersOfLayoutsOnAllCores([&](CollectorOfKeyOrders& collectorOfRange, uint32_t firstLayout, uint32_t endOfLayouts) {
        collectorOfRange.addDeclaredOrders(firstLayout, endOfLayouts);
    });
}

void CollectorOfKeyOrders::addDeclaredOrders(uint32_t firstLayout, uint32_t endOfLayouts)
{
    for (uint32_t index = firstLayout; index < endOfLayouts; ++index) {
        if (hasOrders(LayoutID(index)))
            addDeclaredOrderOf(LayoutID(index));
    }
}

void CollectorOfKeyOrders::addOrderOfDataObject(const DataNode::Object& object)
{
    if (!object.layout || !hasOrders(object.layout))
        return;
    const PlannedLayout& planned = m_slots.layout(object.layout);
    size_t firstField = m_fields.size();
    for (const DataMember& member : object.members) {
        std::optional<FieldIndex> field = m_slots.findField(m_program, object.layout, member.key);
        if (field && isListedInOrders(planned, *field))
            m_fields.append(*field);
    }
    finishCandidate(object.layout, firstField);
}

void CollectorOfKeyOrders::addOrdersOfData()
{
    Vector<const DataNode*, 32> nodesToVisit;
    for (const Data& data : m_program.data) {
        nodesToVisit.append(&data.root);
        while (!nodesToVisit.isEmpty()) {
            const DataNode& node = *nodesToVisit.takeLast();
            switch (node.kind()) {
            case DataNode::Kind::Undefined:
            case DataNode::Kind::Null:
            case DataNode::Kind::Hole:
            case DataNode::Kind::Boolean:
            case DataNode::Kind::Number:
            case DataNode::Kind::String:
            case DataNode::Kind::BigInt:
                break;
            case DataNode::Kind::Array:
                for (const DataNode& element : node.as<DataNode::Array>().elements)
                    nodesToVisit.append(&element);
                break;
            case DataNode::Kind::Object: {
                auto& object = node.as<DataNode::Object>();
                addOrderOfDataObject(object);
                for (const DataMember& member : object.members)
                    nodesToVisit.append(&member.value);
                break;
            }
            }
        }
    }
}

void CollectorOfKeyOrders::addOrdersOfLibraryBases()
{
    auto isClassOfLibrary = [&](LayoutID id) {
        FunctionID constructor = m_program.at(id).constructor;
        return constructor && isLibraryCode(m_program, m_program.at(constructor));
    };
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    auto baseOfLibrary = [&](LayoutID id) {
        const Layout& layout = m_program.at(id);
        LayoutID base = layout.kind == LayoutKind::Instance && layout.constructor && hasOrders(id) && !isClassOfLibrary(id) ? writtenBaseOf(layout) : LayoutID();
        return base && hasOrders(base) && isClassOfLibrary(base) ? base : LayoutID();
    };
    HashMap<uint32_t, Vector<uint32_t, 8>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidatesOfBases;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (LayoutID base = baseOfLibrary(LayoutID(index)))
            candidatesOfBases.add(base.index(), Vector<uint32_t, 8>());
    }
    if (candidatesOfBases.isEmpty())
        return;
    for (uint32_t place = 0; place < m_candidates.size(); ++place) {
        auto found = candidatesOfBases.find(m_candidates[place].layout.index());
        if (found != candidatesOfBases.end() && !m_candidates[place].isDeclaredOrder)
            found->value.append(place);
    }
    Vector<FieldIndex, 32> fieldsOfBase;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        LayoutID base = baseOfLibrary(id);
        if (!base)
            continue;
        const PlannedLayout& planned = m_slots.layout(id);
        const PlannedLayout& plannedOfBase = m_slots.layout(base);
        const Layout& layoutOfBase = m_program.at(base);
        for (uint32_t place : candidatesOfBases.get(base.index())) {
            if (m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
                break;
            fieldsOfBase.shrink(0);
            fieldsOfBase.append(fieldsOf(m_candidates[place]));
            size_t firstField = m_fields.size();
            for (FieldIndex field : fieldsOfBase) {
                PropertyKeyID key = field < layoutOfBase.fields.size() ? layoutOfBase.fields[field].key : plannedOfBase.appendedKeys[field - layoutOfBase.fields.size()];
                std::optional<FieldIndex> found = key ? m_slots.findField(m_program, id, key) : std::nullopt;
                if (!found || !isListedInOrders(planned, *found))
                    break;
                m_fields.append(*found);
            }
            finishCandidate(id, firstField);
        }
    }
}

void CollectorOfKeyOrders::sortCandidates()
{
    parallelSort(m_loops, m_candidates.mutableSpan(), [&](const CandidateForKeyOrders& a, const CandidateForKeyOrders& b) {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        return std::ranges::lexicographical_compare(fieldsOf(a), fieldsOf(b));
    });
}

KeyOrderParts CollectorOfKeyOrders::sortIntoParts(BuildContext& build)
{
    sortCandidates();

    size_t numberOfCandidates = m_candidates.size();
    Vector<uint8_t> beginsNextCandidate;
    beginsNextCandidate.fill(0, numberOfCandidates);
    forEachRangeOfIndicesInParallel(m_loops, numberOfCandidates, [&](size_t first, size_t end) {
        for (size_t next = first; next < end && next + 1 < numberOfCandidates; ++next) {
            const CandidateForKeyOrders& candidate = m_candidates[next];
            beginsNextCandidate[next] = m_candidates[next + 1].layout == candidate.layout && lengthSharedByKeyOrders(fieldsOf(candidate), fieldsOf(m_candidates[next + 1])) == candidate.numberOfFields;
        }
    });

    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    KeyOrderParts parts;
    parts.firstOrderOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    Vector<uint32_t> lastCandidateOfOrder;
    size_t next = 0;
    size_t numberOfFields = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        uint32_t firstOrder = static_cast<uint32_t>(parts.firstFieldOfOrder.size());
        parts.firstOrderOfLayout.append(firstOrder);
        for (; next < numberOfCandidates && m_candidates[next].layout.index() == index; ++next) {
            if (beginsNextCandidate[next])
                continue;
            parts.firstFieldOfOrder.append(static_cast<uint32_t>(numberOfFields));
            lastCandidateOfOrder.append(static_cast<uint32_t>(next));
            numberOfFields += m_candidates[next].numberOfFields;
        }
        uint32_t numberOfOrders = static_cast<uint32_t>(parts.firstFieldOfOrder.size()) - firstOrder;
        if (numberOfOrders > maxKeyOrdersPerLayout)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index), .limit = LimitID::KeyOrdersPerLayout, .count = numberOfOrders });
    }
    RELEASE_ASSERT(next == numberOfCandidates);
    parts.firstOrderOfLayout.append(static_cast<uint32_t>(parts.firstFieldOfOrder.size()));
    parts.firstFieldOfOrder.append(static_cast<uint32_t>(numberOfFields));

    parts.fields.fill(0, numberOfFields);
    parts.sourceOfField.fill(KeyOrderSource::Declaration, numberOfFields);
    parts.someMakerStopsBehindField.fill(0, numberOfFields);
    forEachRangeOfIndicesInParallel(m_loops, lastCandidateOfOrder.size(), [&](size_t firstOrder, size_t endOfOrders) {
        for (size_t order = firstOrder; order < endOfOrders; ++order) {
            size_t firstBeginning = order ? lastCandidateOfOrder[order - 1] + 1 : 0;
            std::span<const CandidateForKeyOrders> beginnings = m_candidates.span().subspan(firstBeginning, lastCandidateOfOrder[order] + 1 - firstBeginning);
            std::span<const FieldIndex> fields = fieldsOf(beginnings.back());
            size_t firstField = parts.firstFieldOfOrder[order];
            for (const CandidateForKeyOrders& beginning : beginnings) {
                if (!beginning.isDeclaredOrder)
                    parts.someMakerStopsBehindField[firstField + beginning.numberOfFields - 1] = 1;
            }
            for (uint32_t place = 0; place < fields.size(); ++place) {
                parts.fields[firstField + place] = fields[place];
                bool isStoredInSequence = std::ranges::any_of(beginnings, [&](const CandidateForKeyOrders& beginning) {
                    return beginning.numberOfFieldsByDeclaration <= place && place < beginning.numberOfFields;
                });
                if (isStoredInSequence)
                    parts.sourceOfField[firstField + place] = KeyOrderSource::StoresInSequence;
            }
        }
    });
    for (FunctionID method : m_methodsOfConstructions)
        parts.methodsOfConstructions.set(method.index());
    return parts;
}

} // anonymous namespace

KeyOrders computeKeyOrders(const Program& program, const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies, Mechanisms mechanisms, std::span<const PlannedLayout> layouts, BuildContext& build)
{
    RELEASE_ASSERT(layouts.size() == program.layouts.size());
    Vector<uint32_t> numbersOfCandidates;
    numbersOfCandidates.fill(0, program.layouts.size());
    CollectorOfKeyOrders collector(program, PlannedLayouts(layouts), build.parallelLoops(), numbersOfCandidates.mutableSpan());
    collector.addDeclaredOrders();
    collector.addOrdersOfData();
    collector.addOrdersOfMakers(link);
    collector.addOrdersOfMembersMadeByTag(link, bodies);
    collector.addOrdersOfConstructorFunctions(link, summaries, bodies);
    collector.addOrdersBehindWhatConstructorFunctionsAlwaysCreate(link, summaries);
    collector.addOrdersOfLibraryBases();
    if (mechanisms.contains(Mechanism::TypedSlots)) {
        collector.addOrdersOfAllocations(link, summaries);
        collector.addOrdersOfComputedLiterals(link);
        collector.addOrdersOfCopies(link, summaries, bodies, build);
    }
    return KeyOrders(collector.sortIntoParts(build));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
