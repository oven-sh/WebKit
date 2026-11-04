/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKeyOrders.h"

#if ENABLE(AOT)

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
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// For planProgram(). No header declares it.
KeyOrders computeKeyOrders(const Program&, const LinkTables&, const ThinSummaries&, const BodyIndexTable&, Mechanisms, const SlotPlan&, BuildContext&);

static size_t lengthSharedByKeyOrders(std::span<const FieldIndex> first, std::span<const FieldIndex> second)
{
    return static_cast<size_t>(std::ranges::mismatch(first, second).in1 - first.begin());
}

// The number of the first order that, as lists are sorted, does not come in front of the fields. The orders that begin with the fields are the first of those.
static uint32_t firstKeyOrderNotBefore(const KeyOrders& orders, LayoutID layout, std::span<const FieldIndex> fields)
{
    uint32_t low = 0;
    uint32_t high = orders.numberOfOrdersOf(layout);
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (std::ranges::lexicographical_compare(orders.order(layout, middle), fields))
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

// How many keys of the order lie in front of the fields, where the order lists the fields one behind the other. An order lists no field twice.
static std::optional<size_t> findPlaceInKeyOrder(std::span<const FieldIndex> order, std::span<const FieldIndex> fields)
{
    size_t place = static_cast<size_t>(std::ranges::find(order, fields.front()) - order.begin());
    if (order.size() - place < fields.size() || !std::ranges::equal(order.subspan(place, fields.size()), fields))
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
    // The run time takes a layout without orders to have one order without keys.
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
    // Among sorted lists, none shares a longer beginning with the fields than one of the two lists that the fields would stand between.
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
    std::optional<KeyOrderContinuation> best;
    if (createdFields.empty())
        return best;
    size_t fewest = std::numeric_limits<size_t>::max();
    uint32_t numberOfOrders = numberOfOrdersOf(layout);
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::optional<size_t> place = findPlaceInKeyOrder(order(layout, number), createdFields);
        if (!place || *place >= fewest || *place < fieldsInFront.size())
            continue;
        std::span<const FieldIndex> beginning = order(layout, number).first(*place);
        if (!std::ranges::all_of(fieldsInFront, [&](FieldIndex field) { return std::ranges::find(beginning, field) != beginning.end(); }))
            continue;
        fewest = *place;
        best = KeyOrderContinuation {
            wordOfBeginningOfKeyOrder(*this, layout, number, *place),
            wordOfBeginningOfKeyOrder(*this, layout, number, *place + createdFields.size()),
        };
    }
    return best;
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
                while (m_firstContinuationCreatingField.size() <= firstOfLayout + listed[place])
                    m_firstContinuationCreatingField.append(KeyOrderContinuation { });
                KeyOrderContinuation& first = m_firstContinuationCreatingField[firstOfLayout + listed[place]];
                if (first.orderWordAfter)
                    continue;
                first.orderWordBefore = place ? orderWordOf(firstOrderWithBeginning[place - 1], static_cast<uint32_t>(place)) : unorderedOrderWord;
                first.orderWordAfter = orderWordOf(firstOrderWithBeginning[place], static_cast<uint32_t>(place + 1));
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
    // From this one on a maker was seen to store them one behind the other.
    uint32_t numberOfFieldsByDeclaration { 0 };
};

class CollectorOfKeyOrders {
    WTF_MAKE_NONCOPYABLE(CollectorOfKeyOrders);
public:
    CollectorOfKeyOrders(const Program& program, const SlotPlan& slots, const ParallelLoops& loops)
        : m_program(program)
        , m_slots(slots)
        , m_loops(loops)
    {
        m_numbersOfCandidates.fill(0, program.layouts.size());
    }

    void addDeclaredOrders();
    void addOrdersOfData();
    void addOrdersOfMakers(const LinkTables&);
    void addOrdersOfMembersMadeByTag(const LinkTables&, const BodyIndexTable&);
    void addOrdersOfConstructorFunctions(const LinkTables&, const ThinSummaries&, const BodyIndexTable&);
    void addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables&, const ThinSummaries&);
    void addOrdersOfAllocations(const LinkTables&, const ThinSummaries&);
    void addOrdersOfCopies(const LinkTables&, const ThinSummaries&, const KeyOrders& ordersOfSources);
    KeyOrderParts sortIntoParts(BuildContext&);

private:
    std::span<const FieldIndex> fieldsOf(const CandidateForKeyOrders& candidate) const { return m_fields.span().subspan(candidate.firstField, candidate.numberOfFields); }
    bool hasOrders(LayoutID id) const
    {
        const PlannedLayout& planned = m_slots.layout(id);
        return planned.holdsCompiledObjects() && !planned.keysAreNeverEnumerated;
    }
    // What the compiler keeps for the closures of an object is no property, so no order lists it. Neither is what a type alone spells: it has no place in the record.
    static bool isListedInOrders(const PlannedLayout& planned, FieldIndex field) { return field < planned.numberOfFields() && planned.slots[field].hasStorage() && planned.slots[field].indexInLayoutRecord && planned.environmentOfDeferredClosures != field; }

    void finishCandidate(LayoutID layout, size_t firstField) { finishCandidate(layout, firstField, m_fields.size() - firstField); }
    void finishCandidate(LayoutID layout, size_t firstField, size_t numberOfFieldsByDeclaration)
    {
        if (m_fields.size() == firstField)
            return;
        m_candidates.append({ layout, static_cast<uint32_t>(firstField), static_cast<uint32_t>(m_fields.size() - firstField), static_cast<uint32_t>(numberOfFieldsByDeclaration) });
        ++m_numbersOfCandidates[layout.index()];
    }
    void listFieldsThatLayoutLists(LayoutID);
    void addDeclaredOrderOf(LayoutID);
    void addOrderOfDataObject(const DataNode::Object&);
    TypeID typeExpectedOfSourceOfCopy(const LinkTables&, FunctionID, const SummaryNode&) const;
    void addOrdersOfCopy(LayoutID target, LayoutID source, const KeyOrders& ordersOfSources);

    const Program& m_program;
    const SlotPlan& m_slots;
    const ParallelLoops& m_loops;
    Vector<CandidateForKeyOrders> m_candidates;
    // By layout.
    Vector<uint32_t> m_numbersOfCandidates;
    Vector<FieldIndex> m_fields;
    BitVector m_isListed;
    Vector<LayoutID, 8> m_declaringLayouts;
};

// The layout, the number of fields, the fields, list behind list.
static void forEachListOfFieldsForKeyOrders(std::span<const uint32_t> lists, const Invocable<void(LayoutID, std::span<const FieldIndex>)> auto& functor)
{
    for (size_t next = 0; next < lists.size();) {
        std::span<const FieldIndex> fields = lists.subspan(next + 2, lists[next + 1]);
        functor(LayoutID(lists[next]), fields);
        next += 2 + fields.size();
    }
}

// A constructor creates what the classes above declare in front of what its own class declares. The bases are those that are written: the order only has to be one that objects may have.
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
        // What it inherits stands behind what it declares: verifyLayout().
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
    finishCandidate(id, firstField);
}

// `function Signature(checker, flags) { .. }   const made = new Signature(checker, flags); made.declaration = ..; made.parameters = ..`
// The construction creates what the layout lists, and the maker goes on from there. An image has room for maxKeyOrdersPerLayout orders of a layout: the makers that come first in the program have theirs.
void CollectorOfKeyOrders::addOrdersOfMakers(const LinkTables& link)
{
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
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

// `const node = createBaseDeclaration(kind) as PropertyAccessExpression; node.expression = ..; node.name = ..`: no body names a member of a tagged family, and the constructor of its maker creates only what the member has from the maker.
// What the member's own types declare is created by whoever stores it first: a factory. Link says which makers the row of calls behind the run leads to, and the body says which members are written of the object.
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
        // `Object.defineProperty(made, "name", ..)` stores under a name too, and is a call.
        if (index.isParameter(run.firstStore) || (index.opcodeOf(run.firstStore) != Opcode::PropSet && index.opcodeOf(run.firstStore) != Opcode::PropDefine))
            continue;
        TypeID written;
        for (ValueID name = index.value(run.firstStore).objectOperand(); name && !written && !index.isParameter(name) && inputOperationsThatHandOn.contains(index.opcodeOf(name)); name = index.value(name).operand(0)) {
            if (index.opcodeOf(name) == Opcode::Convert)
                written = index.value(name).as<Opcode::Convert>().declared().written();
        }
        if (!written || !m_program.at(written).hasObjects() || !m_program.at(written).layouts)
            continue;
        std::span<const PropertyKeyID> keys = link.keysOfRunsBehindTaggedMakers.span().subspan(run.firstKey, run.numberOfKeys);
        std::span<const LayoutID> makers = link.makersOfRunsBehindTaggedMakers.span().subspan(run.firstMaker, run.numberOfMakers);
        // `createBaseDeclaration<T extends Declaration>()`: a type that names several members below one maker does not say which of them the object is.
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

// `function Type(checker, flags) { this.flags = flags; if (tracing) this.checker = checker; }`: the layout lists both names, and nearly every object is made without the second.
// So beside the orders that begin with all that the layout lists stand those that begin with what the constructor creates on every way through it, and the makers go on from there too.
void CollectorOfKeyOrders::addOrdersBehindWhatConstructorFunctionsAlwaysCreate(const LinkTables& link, const ThinSummaries& summaries)
{
    struct StoreOnThis {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<StoreOnThis, 32> stores;
    Vector<FieldIndex, 32> created;
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
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

// `function Request(socket) { this.aborted = false; this.url = ""; .. this._events = makeEvents(); }` below `Readable`: the body need not begin with what the classes above declare.
// It creates what it stores on `this`, each name once, in the order in which the stores are written: that is the order of the way through the body that stands first in the text.
// It ends at a store under a key that nobody knows, and at one under a name without a field.
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

void CollectorOfKeyOrders::addOrdersOfConstructorFunctions(const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies)
{
    struct StoreOnThis {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<StoreOnThis, 32> stores;
    Vector<FieldIndex, 32> firstPath;
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        LayoutID id(index);
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor || !hasOrders(id) || m_numbersOfCandidates[index] >= maxKeyOrdersPerLayout)
            continue;
        const Function& function = m_program.at(constructor);
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        if (function.constructedClass != id || !function.takesThisFromConstruction() || !thisParameter || !link.function(constructor).isReached)
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

// `{ flag: true, tail: t }` for `{ name?, depth?, flag, tail? }`: the allocation creates the two at once where their order is one of the image's. Where it is not, the walk defines by name whatever stands behind the longest beginning that is.
// The list that Plan::allocation() asks the word of. As with the makers, those that come first in the program have theirs.
void CollectorOfKeyOrders::addOrdersOfAllocations(const LinkTables& link, const ThinSummaries& summaries)
{
    // Reading the summaries is most of the work and asks nothing of the lists in front. Which lists a layout has room for is asked behind it, in the order of the program.
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
                // What has no slot is no field, and the word says nothing of an object that has such a property.
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

// `{ ...state, more }`: the object gets the keys of `state` in the order in which `state` got them. Where the walk knows the layout of the source it loads each field and makes the object with them
// (walk/AOTWalkTypedAccesses.cpp, buildCopyBehindTestsForTypedAccess()), which takes an order of the target that begins so. The summary says what is copied into what, and of a parameter, an allocation, a field
// and a variable somebody says what is expected there. Null: nobody does.
TypeID CollectorOfKeyOrders::typeExpectedOfSourceOfCopy(const LinkTables& link, FunctionID function, const SummaryNode& source) const
{
    switch (source.kind) {
    case SummaryNode::Kind::Parameter: {
        std::span<const LinkedParameter> parameters = link.parametersOf(function);
        return source.first < parameters.size() ? parameters[source.first].expected : TypeID();
    }
    case SummaryNode::Kind::Field: {
        const Layout& holder = m_program.at(LayoutID(source.first));
        return source.second < holder.fields.size() ? holder.fields[source.second].declared.written() : TypeID();
    }
    case SummaryNode::Kind::Variable:
        return m_program.at(VariableID(source.first)).declared.written();
    default:
        return { };
    }
}

// An order of the source that lists every field, as the target's fields under the same keys. A rest copy leaves keys out, and the target has no field for those.
void CollectorOfKeyOrders::addOrdersOfCopy(LayoutID target, LayoutID source, const KeyOrders& ordersOfSources)
{
    const Layout& declaredSource = m_program.at(source);
    if (source == target || declaredSource.kind != LayoutKind::Record || !hasOrders(source))
        return;
    const PlannedLayout& planned = m_slots.layout(target);
    for (uint32_t number = 0; number < ordersOfSources.numberOfOrdersOf(source); ++number) {
        std::span<const FieldIndex> order = ordersOfSources.order(source, number);
        if (order.size() != declaredSource.fields.size() || m_numbersOfCandidates[target.index()] >= maxKeyOrdersPerLayout)
            continue;
        // As many keys as the layout declares, and one of them appended in place of one that a type alone spells.
        if (std::ranges::any_of(order, [&](FieldIndex index) { return index >= declaredSource.fields.size(); }))
            continue;
        size_t firstField = m_fields.size();
        for (FieldIndex index : order) {
            std::optional<FieldIndex> field = m_slots.findField(m_program, target, declaredSource.fields[index].key);
            if (field && isListedInOrders(planned, *field))
                m_fields.append(*field);
        }
        finishCandidate(target, firstField);
    }
}

void CollectorOfKeyOrders::addOrdersOfCopies(const LinkTables& link, const ThinSummaries& summaries, const KeyOrders& ordersOfSources)
{
    Vector<std::pair<LayoutID, LayoutID>> copies;
    copies.appendVector(collectInParallel<std::pair<LayoutID, LayoutID>>(m_loops, static_cast<unsigned>(m_program.functions.size()), [&](unsigned number, Vector<std::pair<LayoutID, LayoutID>>& copies) {
        FunctionID function(number);
        if (!link.function(function).isReached)
            return;
        const ThinSummary& summary = summaries.at(function);
        for (const SummaryChange& change : summary.changes) {
            if (change.kind != SummaryChange::Kind::CopiesInto || change.other == noSummaryNode || summary.nodes[change.subject].kind != SummaryNode::Kind::Allocation)
                continue;
            LayoutID target(summary.nodes[change.subject].first);
            if (m_program.at(target).kind != LayoutKind::Record || !hasOrders(target))
                continue;
            const SummaryNode& source = summary.nodes[change.other];
            if (source.kind == SummaryNode::Kind::Allocation) {
                copies.append({ target, LayoutID(source.first) });
                continue;
            }
            TypeID expected = typeExpectedOfSourceOfCopy(link, function, source);
            if (LayoutSetID layouts = expected ? m_program.at(expected).layouts : LayoutSetID()) {
                m_program.at(layouts).forEach([&](LayoutID member) {
                    copies.append({ target, member });
                });
            }
        }
    }));
    AOT::sort(copies);
    copies.shrink(std::unique(copies.begin(), copies.end()) - copies.begin());
    for (auto [target, source] : copies)
        addOrdersOfCopy(target, source, ordersOfSources);
}

void CollectorOfKeyOrders::addDeclaredOrders()
{
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        if (hasOrders(LayoutID(index)))
            addDeclaredOrderOf(LayoutID(index));
    }
}

// The list that Plan::allocation() asks the word of for the row of the object: fieldOfDataMember() of image/AOTDataTable.cpp.
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

KeyOrderParts CollectorOfKeyOrders::sortIntoParts(BuildContext& build)
{
    parallelSort(build.parallelLoops(), m_candidates.mutableSpan(), [&](const CandidateForKeyOrders& a, const CandidateForKeyOrders& b) {
        if (a.layout != b.layout)
            return a.layout < b.layout;
        return std::ranges::lexicographical_compare(fieldsOf(a), fieldsOf(b));
    });

    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    KeyOrderParts parts;
    parts.firstOrderOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    size_t next = 0;
    // The lists in front of this one that begin it. What a maker was seen to store stays so in the longer list.
    Vector<CandidateForKeyOrders, 4> beginnings;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        uint32_t firstOrder = static_cast<uint32_t>(parts.firstFieldOfOrder.size());
        parts.firstOrderOfLayout.append(firstOrder);
        for (; next < m_candidates.size() && m_candidates[next].layout.index() == index; ++next) {
            std::span<const FieldIndex> fields = fieldsOf(m_candidates[next]);
            beginnings.append(m_candidates[next]);
            // Among sorted lists, a list that begins another begins the one behind it.
            if (next + 1 < m_candidates.size() && m_candidates[next + 1].layout.index() == index && lengthSharedByKeyOrders(fields, fieldsOf(m_candidates[next + 1])) == fields.size())
                continue;
            parts.firstFieldOfOrder.append(static_cast<uint32_t>(parts.fields.size()));
            for (FieldIndex field : fields)
                parts.fields.append(field);
            for (uint32_t place = 0; place < fields.size(); ++place) {
                bool isStoredInSequence = std::ranges::any_of(beginnings, [&](const CandidateForKeyOrders& beginning) {
                    return beginning.numberOfFieldsByDeclaration <= place && place < beginning.numberOfFields;
                });
                parts.sourceOfField.append(isStoredInSequence ? KeyOrderSource::StoresInSequence : KeyOrderSource::Declaration);
            }
            beginnings.shrink(0);
        }
        uint32_t numberOfOrders = static_cast<uint32_t>(parts.firstFieldOfOrder.size()) - firstOrder;
        if (numberOfOrders > maxKeyOrdersPerLayout)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index), .limit = LimitID::KeyOrdersPerLayout, .count = numberOfOrders });
    }
    RELEASE_ASSERT(next == m_candidates.size());
    parts.firstOrderOfLayout.append(static_cast<uint32_t>(parts.firstFieldOfOrder.size()));
    parts.firstFieldOfOrder.append(static_cast<uint32_t>(parts.fields.size()));
    return parts;
}

} // anonymous namespace

KeyOrders computeKeyOrders(const Program& program, const LinkTables& link, const ThinSummaries& summaries, const BodyIndexTable& bodies, Mechanisms mechanisms, const SlotPlan& slots, BuildContext& build)
{
    CollectorOfKeyOrders collector(program, slots, build.parallelLoops());
    collector.addDeclaredOrders();
    collector.addOrdersOfData();
    collector.addOrdersOfMakers(link);
    collector.addOrdersOfMembersMadeByTag(link, bodies);
    collector.addOrdersOfConstructorFunctions(link, summaries, bodies);
    collector.addOrdersBehindWhatConstructorFunctionsAlwaysCreate(link, summaries);
    if (mechanisms.contains(Mechanism::TypedSlots)) {
        collector.addOrdersOfAllocations(link, summaries);
        // A copy takes an order of its source, so those are put in order first.
        KeyOrders ordersOfSources(collector.sortIntoParts(build));
        collector.addOrdersOfCopies(link, summaries, ordersOfSources);
    }
    return KeyOrders(collector.sortIntoParts(build));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
