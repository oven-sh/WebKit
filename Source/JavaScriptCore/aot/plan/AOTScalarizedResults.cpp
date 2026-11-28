/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTScalarizedResults.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentation.h"
#include "AOTSlotRepresentation.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <bit>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class OrderOfStores {
    WTF_MAKE_NONCOPYABLE(OrderOfStores);
public:
    OrderOfStores(const Program& program, const Facts& facts, LayoutID layout, std::span<const FieldIndex> fields)
        : m_program(program)
        , m_facts(facts)
        , m_layout(layout)
        , m_fields(fields)
    {
    }

    bool isCertain()
    {
        if (m_fields.size() > 64)
            return false;
        storedBy(constructorAtOrAbove(declaredLayoutOf(m_program, m_layout)));
        return m_isCertain;
    }

private:
    static constexpr size_t deepestCall = 8;
    struct Event {
        ValueID site;
        uint64_t places;
    };

    FunctionID constructorAtOrAbove(LayoutID layout) const
    {
        while (layout && !m_program.at(layout).constructor)
            layout = m_program.at(layout).base;
        return layout ? m_program.at(layout).constructor : FunctionID();
    }

    uint64_t placeOf(PropertyKeyID key) const
    {
        const Layout& layout = m_program.at(m_layout);
        for (size_t place = 0; place < m_fields.size(); ++place) {
            if (layout.fields[m_fields[place]].key == key)
                return uint64_t { 1 } << place;
        }
        return 0;
    }

    uint64_t storedBy(FunctionID);
    Vector<Event, 16> eventsOf(FunctionID, const ThinSummary&);

    const Program& m_program;
    const Facts& m_facts;
    LayoutID m_layout;
    std::span<const FieldIndex> m_fields;
    Vector<FunctionID, 8> m_functions;
    bool m_isCertain { true };
};

auto OrderOfStores::eventsOf(FunctionID function, const ThinSummary& summary) -> Vector<Event, 16>
{
    const LinkTables& linked = m_facts.linked();
    const Function& record = m_program.at(function);
    LayoutID constructed = isClassConstructorOf(record.kind) ? record.constructedClass : LayoutID();
    std::optional<unsigned> thisParameter = record.kind == FunctionKind::Initializer ? std::optional<unsigned>(0) : record.thisParameterIndex();
    auto isSuperCall = [&](const SummaryCall& call) {
        return constructed && call.isConstruction && call.newTarget != noSummaryNode;
    };
    auto isThis = [&](uint32_t node) {
        if (node == noSummaryNode)
            return false;
        const SummaryNode& origin = summary.nodes[node];
        switch (origin.kind) {
        case SummaryNode::Kind::Join:
            return true;
        case SummaryNode::Kind::Parameter:
            return !constructed && thisParameter && origin.first == *thisParameter;
        case SummaryNode::Kind::Allocation:
            return !!constructed;
        case SummaryNode::Kind::ResultOfCall:
            return isSuperCall(summary.calls[origin.first]);
        default:
            return false;
        }
    };
    Vector<Event, 16> events;
    for (const SummaryAccess& access : summary.accesses) {
        if ((access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define) || access.isToContents() || !isThis(access.subject))
            continue;
        PropertyKeyID key = access.key ? access.key : linked.keyAt(function, access.site);
        m_isCertain &= !!key;
        if (uint64_t place = key ? placeOf(key) : 0)
            events.append({ access.site, place });
    }
    for (const SummaryCall& call : summary.calls) {
        FunctionID entered;
        if (isSuperCall(call))
            entered = constructorAtOrAbove(m_program.at(constructed).base);
        else {
            bool takesThis = isThis(call.receiver);
            for (uint32_t argument = 0; argument < call.numberOfArguments; ++argument)
                takesThis |= isThis(summary.arguments[call.firstArgument + argument]);
            if (!takesThis || call.isConstruction)
                continue;
            entered = call.callee == SummaryCall::Callee::Binding ? call.function : linked.calleeAt(function, call.site);
        }
        if (uint64_t places = entered ? storedBy(entered) : 0)
            events.append({ call.site, places });
    }
    return events;
}

uint64_t OrderOfStores::storedBy(FunctionID function)
{
    const BodyIndex* body = function ? m_facts.bodies().find(function) : nullptr;
    if (!body || !m_isCertain)
        return 0;
    if (m_functions.contains(function) || m_functions.size() == deepestCall) {
        m_isCertain = false;
        return 0;
    }
    m_functions.append(function);
    Vector<Event, 16> events = eventsOf(function, m_facts.summaries().at(function));
    m_functions.removeLast();
    uint64_t stored = 0;
    for (const Event& event : events) {
        if (!event.site || body->isParameter(event.site) || body->block(body->blockOf(event.site)).hasHandler())
            m_isCertain = false;
        stored |= event.places;
    }
    for (size_t number = 0; number < events.size() && m_isCertain; ++number) {
        const Event& event = events[number];
        BlockID block = body->blockOf(event.site);
        uint64_t inFront = event.places;
        for (const Event& other : events) {
            BlockID blockOfOther = body->blockOf(other.site);
            if (blockOfOther == block ? other.site < event.site : body->dominates(blockOfOther, block))
                inFront |= other.places;
        }
        uint64_t last = std::bit_floor(event.places);
        m_isCertain = !(stored & (last - 1) & ~inFront);
    }
    return stored;
}

} // anonymous namespace

enum class KeysMayBeListed : bool { No, Yes };

static Vector<FieldIndex, 8> fieldsOfConstructions(const Program& program, const Facts& facts, LayoutID id, KeysMayBeListed keysMayBeListed)
{
    const LinkTables& linked = facts.linked();
    const Layout& layout = program.at(id);
    Vector<FieldIndex, 8> fields;
    uint32_t numberOfRuns = 0;
    bool isWhole = true;
    linked.forEachRunOfMakers(id, [&](std::span<const PropertyKeyID> keys) {
        if (numberOfRuns++)
            return;
        for (PropertyKeyID key : keys) {
            auto field = std::ranges::find(layout.fields, key, &Field::key);
            isWhole &= field != layout.fields.end();
            if (isWhole)
                fields.append(static_cast<FieldIndex>(field - layout.fields.begin()));
        }
    });
    if (!numberOfRuns) {
        for (FieldIndex field = 0; field < layout.fields.size(); ++field)
            fields.append(field);
    }
    if (numberOfRuns > 1 || !isWhole || fields.size() != layout.fields.size())
        return { };
    if (!numberOfRuns && keysMayBeListed == KeysMayBeListed::Yes && layout.kind == LayoutKind::Instance && !OrderOfStores(program, facts, id, fields.span()).isCertain())
        return { };
    return fields;
}

enum class FilledBy : bool { ListedLiterals, EveryMaker };

static bool canScalarizeLayout(const Program& program, const Facts& facts, const Plan& plan, LayoutID id, std::span<const FieldIndex> fields, FilledBy filledBy)
{
    const LinkTables& linked = facts.linked();
    LayoutID declared = declaredLayoutOf(program, id);
    if (!declared || program.at(declared).has(LayoutFlags::IsConstructorFunction))
        return false;
    LayoutKind kind = program.at(declared).kind;
    if (kind != LayoutKind::Record && (kind != LayoutKind::Instance || !facts.isEvaluatedAtMostOnce(declared)))
        return false;
    const PlannedLayout& planned = plan.layout(id);
    if (!planned.holdsCompiledObjects() || planned.environmentOfDeferredClosures || planned.cellsAreHandedBack)
        return false;
    for (FieldIndex field : fields) {
        bool isBrand = program.at(id).fields[field].flags.contains(FieldFlags::IsBrand);
        bool isUnlike = program.at(id).fields[field].flags.contains(FieldFlags::IsHidden) && !isBrand;
        const PlacedSlot& slot = planned.slots[field];
        const LinkedCell* cell = linked.findCellOfField({ id, field });
        bool isPlain = cell && ((kind == LayoutKind::Record && filledBy == FilledBy::ListedLiterals) || isBrand || (cell->isFilled && cell->isFilledAtEveryRead)) && !isUnlike && program.at(id).fields[field].form.isEmpty();
        if (!isPlain || slot.storage != SlotStorage::Inline || slot.holdsAccessor || slot.representation == SlotRepresentation::DeferredClosure)
            return false;
        if (!isBrand && !fieldStaysInSlot(program, slot))
            return false;
    }
    std::optional<PlacedAllocation> placed = plan.allocation(program, id, fields);
    return placed && !placed->isBornUnfinished;
}

static Representation formOfScalar(const Program& program, const PlacedSlot& slot)
{
    if (traitsOf(slot.representation).holdsCell)
        return Representation::JSValue;
    TypeID held = slot.provenType ? slot.provenType : slot.type;
    if (!held)
        return representationOf(slot.representation);
    Type type = program.at(held);
    type.primitives &= ~Primitive::Absent;
    return loadedRepresentationOf(slot.representation, type);
}

static void findCalleesWhoseResultIsRead(const Program& program, const LinkTables& linked, const BodyIndex& index, std::span<const LinkedCall> calls, const BitVector& candidates, BitVector& found)
{
    Vector<uint32_t> calleesOfNames;
    calleesOfNames.fill(UINT32_MAX, index.numberOfValues());
    for (const LinkedCall& call : calls) {
        if (call.site && candidates.quickGet(call.callee.index()))
            calleesOfNames[call.site.index()] = call.callee.index();
    }
    auto isInspected = [&](ValueID site, uint32_t position) {
        auto call = std::ranges::lower_bound(calls, site, { }, &LinkedCall::site);
        if (call == calls.end() || call->site != site)
            return false;
        auto inspected = std::ranges::equal_range(linked.inspectedParameters, call->callee, { }, &LinkedInspectedParameter::function);
        return std::ranges::contains(inspected, program.at(call->callee).firstParameterIndex() + position, &LinkedInspectedParameter::entryParameter);
    };
    auto calleeOf = [&](ValueID name) {
        return name ? calleesOfNames[name.index()] : UINT32_MAX;
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        for (BodyValue value : block.values()) {
            if (inputPassThroughOpcodes.contains(value.opcode()))
                calleesOfNames[value.id().index()] = calleeOf(value.operand(0));
            else if (value.opcode() == Opcode::PropGet && calleeOf(value.operand(OperandIndex::PropGet::object)) != UINT32_MAX)
                found.quickSet(calleeOf(value.operand(OperandIndex::PropGet::object)));
            else if (value.opcode() == Opcode::Call) {
                std::span<const ValueID> arguments = value.as<Opcode::Call>().argumentsOperands();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (calleeOf(arguments[position]) != UINT32_MAX && isInspected(value.id(), position))
                        found.quickSet(calleeOf(arguments[position]));
                }
            }
        }
        for (BodyTarget target : block.terminator().targets()) {
            BodyBlock to = index.block(target.block());
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t position = 0; position < arguments.size() && position < to.numberOfParameters(); ++position) {
                if (calleeOf(arguments[position]) != UINT32_MAX)
                    calleesOfNames[to.parameter(position).index()] = calleeOf(arguments[position]);
            }
        }
    }
}

ScalarizedResults planScalarizedResults(const Program& program, const Facts& facts, const Plan& plan)
{
    const LinkTables& linked = facts.linked();
    const CallingConventions& conventions = plan.conventions();
    ScalarizedResults results;
    if (linked.freshResults.isEmpty())
        return results;

    BitVector tailCallees(program.functions.size());
    BitVector calleesWhoseResultIsRead(program.functions.size());
    BitVector candidates(program.functions.size());
    for (const LinkedFreshResult& fresh : linked.freshResults)
        candidates.quickSet(fresh.function.index());
    for (size_t first = 0; first < linked.calls.size();) {
        size_t end = first;
        bool hasCandidate = false;
        for (; end < linked.calls.size() && linked.calls[end].caller == linked.calls[first].caller; ++end) {
            const LinkedCall& call = linked.calls[end];
            if (!candidates.quickGet(call.callee.index()))
                continue;
            hasCandidate = true;
            if (call.isInTailPosition)
                tailCallees.quickSet(call.callee.index());
        }
        if (const BodyIndex* index = hasCandidate ? facts.bodies().find(linked.calls[first].caller) : nullptr)
            findCalleesWhoseResultIsRead(program, linked, *index, linked.calls.span().subspan(first, end - first), candidates, calleesWhoseResultIsRead);
        first = end;
    }

    for (const LinkedFreshResult& fresh : linked.freshResults) {
        const FunctionConvention& convention = conventions.of(fresh.function);
        if (!convention.has(CodeRole::Body) || convention.numberOfResults != 1 || convention.takesArgumentsInBuffer || conventions.hasParameterGuard(fresh.function))
            continue;
        if (tailCallees.quickGet(fresh.function.index()) || !calleesWhoseResultIsRead.quickGet(fresh.function.index()) || plan.resultsPassNoReturn(fresh.function))
            continue;
        ScalarizedResult result { fresh.function, fresh.layout, { }, { } };
        for (uint32_t field : fresh.fields)
            result.fields.append(field);
        if (result.fields.isEmpty())
            result.fields = fieldsOfConstructions(program, facts, fresh.layout, KeysMayBeListed::Yes);
        if (result.fields.size() < 2 || !canScalarizeLayout(program, facts, plan, fresh.layout, result.fields.span(), FilledBy::ListedLiterals))
            continue;
        for (FieldIndex field : result.fields)
            result.forms.append(formOfScalar(program, plan.layout(fresh.layout).slots[field]));
        results.append(WTF::move(result));
    }
    return results;
}

static Vector<FieldIndex, 8> fieldsOfHeldObject(const Program& program, const Facts& facts, const Plan& plan, const PlacedSlot& slot, LayoutID held)
{
    TypeID type = slot.provenType ? slot.provenType : slot.type;
    if (!type || (program.at(type).primitives & ~Primitive::Absent) || singleLayoutOf(TypeReader(program), program.at(type)) != held)
        return { };
    Vector<FieldIndex, 8> fields = fieldsOfConstructions(program, facts, held, KeysMayBeListed::No);
    bool isWrittenInSight = std::ranges::all_of(fields, [&](FieldIndex field) {
        const LinkedCell* cell = facts.linked().findCellOfField({ held, field });
        return cell && cell->everyWriterIsKnown;
    });
    if (!isWrittenInSight || fields.isEmpty() || !canScalarizeLayout(program, facts, plan, held, fields.span(), FilledBy::EveryMaker))
        return { };
    return fields;
}

ScalarizedParameters planScalarizedParameters(const Program& program, const Facts& facts, const Plan& plan)
{
    static constexpr uint32_t maxScalars = 12;
    const LinkTables& linked = facts.linked();
    const CallingConventions& conventions = plan.conventions();
    ScalarizedParameters parameters;
    uint32_t numberOfScalars = 0;
    for (const LinkedInspectedParameter& inspected : linked.inspectedParameters) {
        const FunctionConvention& convention = conventions.of(inspected.function);
        const Function& function = program.at(inspected.function);
        if (!convention.has(CodeRole::Body) || convention.takesArgumentsInBuffer || conventions.hasParameterGuard(inspected.function) || !function.captures.isEmpty())
            continue;
        if (parameters.isEmpty() || parameters.last().function != inspected.function)
            numberOfScalars = 0;
        ScalarizedParameter parameter { inspected.function, inspected.entryParameter, convention.numberOfEntryParameters + numberOfScalars, inspected.layout, { }, { }, { } };
        parameter.fields = fieldsOfConstructions(program, facts, inspected.layout, KeysMayBeListed::No);
        if (parameter.fields.isEmpty() || !canScalarizeLayout(program, facts, plan, inspected.layout, parameter.fields.span(), FilledBy::EveryMaker))
            continue;
        for (FieldIndex field : parameter.fields) {
            const PlacedSlot& slot = plan.layout(inspected.layout).slots[field];
            auto held = std::ranges::find(inspected.inspectedFields, field, &LinkedInspectedField::field);
            Vector<FieldIndex, 8> fieldsOfHeld = held == inspected.inspectedFields.end() ? Vector<FieldIndex, 8>() : fieldsOfHeldObject(program, facts, plan, slot, held->held);
            for (FieldIndex fieldOfHeld : fieldsOfHeld) {
                parameter.places.append({ field, held->held, fieldOfHeld });
                parameter.forms.append(formOfScalar(program, plan.layout(held->held).slots[fieldOfHeld]));
            }
            if (!fieldsOfHeld.isEmpty())
                continue;
            parameter.places.append({ field, { }, { } });
            parameter.forms.append(formOfScalar(program, slot));
        }
        if (numberOfScalars + parameter.forms.size() > maxScalars)
            continue;
        numberOfScalars += parameter.forms.size();
        parameters.append(WTF::move(parameter));
    }
    return parameters;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
