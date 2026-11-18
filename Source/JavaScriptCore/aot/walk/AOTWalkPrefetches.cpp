/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPrefetchStep.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Callees = Vector<FunctionID, 3>;

Callees calleesOf(const Walker& walker, FunctionID function, const BodyIndex& index, const BodyValue& call)
{
    const Facts& facts = walker.facts();
    if (FunctionID callee = facts.linked().calleeAt(function, call.id()))
        return { callee };
    if (FunctionID callee = facts.held().calleeAt(function, call.id()))
        return { callee };
    ValueID read = index.stripCasts(call.as<Opcode::Call>().calleeOperand());
    if (!call.as<Opcode::Call>().thisValueOperand() || !read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    PropertyKeyID name = index.value(read).key();
    if (!name || liesInThread(name))
        return { };
    auto methods = facts.layoutsByNames().methodsUnder(name);
    static constexpr size_t maxMethodsOfName = 16;
    if (methods.empty() || methods.size() > maxMethodsOfName)
        return { };
    Callees callees;
    const Program& program = walker.program();
    for (const LayoutsByNames::MethodUnderName& method : methods) {
        bool overrides = false;
        unsigned steps = 0;
        for (LayoutID above = program.at(method.holder).base; above && !overrides && steps < maxMethodsOfName; above = program.at(above).base, ++steps)
            overrides = std::ranges::any_of(methods, [&](const LayoutsByNames::MethodUnderName& other) { return other.holder == above; });
        if (overrides || callees.contains(method.function))
            continue;
        if (callees.size() == callees.capacity())
            return { };
        callees.append(method.function);
    }
    return callees;
}

class PartsOpenedByCode {
    WTF_MAKE_NONCOPYABLE(PartsOpenedByCode);
public:
    static constexpr unsigned maxParts = 320;
    static constexpr unsigned maxOperations = 4000;
    static constexpr unsigned maxVisits = 20;
    static constexpr unsigned maxCallDepth = 5;
    static constexpr unsigned maxDepth = PrefetchStep::maxDepth;
    static constexpr unsigned mostOperationsOfLoop = 600;
    static constexpr unsigned fewestOpenedByCallees = 4;
    static constexpr unsigned operationsOfQuickLook = 400;
    static constexpr unsigned fewestOpenedInQuickLook = 6;
    static constexpr uint16_t none = UINT16_MAX;

    enum class Kind : uint8_t { Root, Field, Elements };

    struct Part {
        PropertyKeyID key;
        uint16_t parent { none };
        Kind kind { Kind::Root };
        uint8_t depth { 0 };
        uint8_t numberOfNames { 0 };
        bool isOpened { false };
        bool hasElementsRead { false };
        uint8_t endOfWrittenIndexes { 0 };
    };

    explicit PartsOpenedByCode(Walker& walker)
        : m_walker(walker)
    {
        m_parts.append(Part { });
        m_parts[0].isOpened = true;
    }

    void collect(FunctionID function, const BodyIndex& index, LoopID loop, ValueID value)
    {
        PartOfValue start { value.index(), 0 };
        visit(function, index, singleElementSpan(start), 0, loop);
        for (size_t next = 0; next < m_visits.size() && next < maxVisits; ++next) {
            Visit current = m_visits[next];
            if (current.callDepth > 1 && m_numberOfOpened < fewestOpenedByCallees)
                return;
            const BodyIndex* body = m_walker.facts().bodies().find(current.function);
            if (!body || (m_operations + body->numberOfValues() > operationsOfQuickLook && m_numberOfOpened < fewestOpenedInQuickLook))
                continue;
            BodyBlock entry = body->block(entryBlock);
            PartsOfValues starts;
            for (PartOfValue passed : current.passed) {
                if (passed.value < entry.numberOfParameters())
                    starts.append({ entry.parameter(passed.value).index(), passed.part });
            }
            visit(current.function, *body, starts.span(), current.callDepth);
        }
    }

    std::span<const Part> parts() const LIFETIME_BOUND { return m_parts.span(); }

private:
    struct PartOfValue {
        uint32_t value;
        uint16_t part;

        friend constexpr bool operator==(const PartOfValue&, const PartOfValue&) = default;
    };
    using PartsOfValues = Vector<PartOfValue, 4>;

    struct Visit {
        FunctionID function;
        PartsOfValues passed;
        uint8_t callDepth;
    };

    uint16_t partOf(uint16_t parent, Kind kind, PropertyKeyID key)
    {
        for (size_t place = parent + 1; place < m_parts.size(); ++place) {
            if (m_parts[place].parent == parent && m_parts[place].kind == kind && m_parts[place].key == key)
                return static_cast<uint16_t>(place);
        }
        unsigned depth = m_parts[parent].depth + (kind == Kind::Elements ? 2 : 1);
        if (m_parts.size() >= maxParts || depth > maxDepth)
            return none;
        m_parts.append(Part { key, parent, kind, static_cast<uint8_t>(depth), 0, false, false, 0 });
        if (kind == Kind::Field && m_parts[parent].numberOfNames < UINT8_MAX)
            ++m_parts[parent].numberOfNames;
        return static_cast<uint16_t>(m_parts.size() - 1);
    }

    void open(uint16_t part)
    {
        for (; part != none && !m_parts[part].isOpened; part = m_parts[part].parent) {
            m_parts[part].isOpened = true;
            ++m_numberOfOpened;
        }
    }

    void openElements(uint16_t part, const BodyIndex& index, ValueID property)
    {
        open(part);
        m_parts[part].hasElementsRead = true;
        ValueID written = property ? index.stripCasts(property) : ValueID();
        if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::Integer)
            return;
        int64_t position = index.value(written).as<Opcode::Integer>().integer();
        static constexpr int64_t maxIndex = 8 * PrefetchStep::maxLines;
        if (position >= 0 && position < maxIndex)
            m_parts[part].endOfWrittenIndexes = std::max<uint8_t>(m_parts[part].endOfWrittenIndexes, position + 1);
    }

    void pass(FunctionID callee, PartsOfValues&& passed, unsigned callDepth)
    {
        if (passed.isEmpty() || callDepth >= maxCallDepth || m_visits.size() >= maxVisits)
            return;
        for (const Visit& visit : m_visits) {
            if (visit.function == callee && visit.passed == passed)
                return;
        }
        m_visits.append({ callee, WTF::move(passed), static_cast<uint8_t>(callDepth + 1) });
    }

    void visit(FunctionID function, const BodyIndex& index, std::span<const PartOfValue> starts, unsigned callDepth, LoopID loop = { })
    {
        const Program& program = m_walker.program();
        if (!loop)
            m_operations += index.numberOfValues();
        if (m_operations > maxOperations)
            return;
        std::span<const BlockID> blocks = loop ? index.membersOf(loop) : index.blocksInDominatorOrder();
        uint32_t firstValue = loop ? index.block(blocks.front()).firstParameter().index() : 0;
        uint32_t endOfValues = loop && blocks.back().index() + 1 < index.numberOfBlocks() ? index.block(BlockID(blocks.back().index() + 1)).firstParameter().index() : index.numberOfValues();
        if (loop && endOfValues - firstValue > mostOperationsOfLoop)
            return;
        Vector<uint16_t, 512> partOfValue;
        partOfValue.fill(none, endOfValues - firstValue);
        auto partAt = [&](ValueID value) {
            return value && value.index() - firstValue < partOfValue.size() ? partOfValue[value.index() - firstValue] : none;
        };
        auto setPart = [&](ValueID value, uint16_t part) {
            if (partAt(value) == none && value.index() - firstValue < partOfValue.size())
                partOfValue[value.index() - firstValue] = part;
        };
        for (PartOfValue start : starts)
            setPart(ValueID(start.value), start.part);
        PropertyKeyID length = program.keyOf(BuiltinNames::length);
        for (BlockID block : blocks) {
            for (BodyValue operation : index.block(block).values()) {
                if (opcodesThatStateTypes.contains(operation.opcode())) {
                    if (uint16_t part = partAt(operation.operand(0)); part != none)
                        setPart(operation.id(), part);
                    continue;
                }
                switch (operation.opcode()) {
                case Opcode::PropGet: {
                    uint16_t holder = operation.isFreeName() ? none : partAt(operation.objectOperand());
                    if (holder == none || !operation.key() || liesInThread(operation.key()))
                        break;
                    open(holder);
                    if (operation.key() != length)
                        setPart(operation.id(), partOf(holder, Kind::Field, operation.key()));
                    break;
                }
                case Opcode::ElemGet: {
                    uint16_t holder = partAt(operation.objectOperand());
                    if (holder == none)
                        break;
                    openElements(holder, index, operation.propertyOperand());
                    setPart(operation.id(), partOf(holder, Kind::Elements, { }));
                    break;
                }
                case Opcode::PropSet:
                    if (uint16_t holder = operation.isFreeName() ? none : partAt(operation.objectOperand()); holder != none)
                        open(holder);
                    break;
                case Opcode::ElemSet:
                    if (uint16_t holder = partAt(operation.objectOperand()); holder != none)
                        openElements(holder, index, operation.propertyOperand());
                    break;
                case Opcode::Call: {
                    auto call = operation.as<Opcode::Call>();
                    uint16_t receiver = partAt(call.thisValueOperand());
                    bool passesParts = receiver != none || std::ranges::any_of(call.argumentsOperands(), [&](ValueID argument) { return partAt(argument) != none; });
                    if (!passesParts)
                        break;
                    for (FunctionID callee : calleesOf(m_walker, function, index, operation)) {
                        const Function& called = program.at(callee);
                        PartsOfValues passed;
                        if (receiver != none) {
                            if (std::optional<unsigned> parameter = called.thisParameterIndex())
                                passed.append({ *parameter, receiver });
                        }
                        unsigned first = called.firstParameterIndex();
                        unsigned numberOfPositionalParameters = called.numberOfPositionalParameters();
                        unsigned place = 0;
                        for (ValueID argument : call.argumentsOperands()) {
                            if (place >= numberOfPositionalParameters)
                                break;
                            if (uint16_t part = partAt(argument); part != none)
                                passed.append({ first + place, part });
                            ++place;
                        }
                        pass(callee, WTF::move(passed), callDepth);
                    }
                    break;
                }
                case Opcode::CallDirect: {
                    auto call = operation.as<Opcode::CallDirect>();
                    PartsOfValues passed;
                    unsigned parameter = 0;
                    for (ValueID argument : call.argumentsOperands()) {
                        if (uint16_t part = partAt(argument); part != none)
                            passed.append({ parameter, part });
                        ++parameter;
                    }
                    pass(call.function(), WTF::move(passed), callDepth);
                    break;
                }
                default:
                    break;
                }
            }
            BodyTerminator terminator = index.terminator(block);
            bool hasProofs = false;
            for (BodyTarget target : terminator.targets()) {
                hasProofs |= !target.proofs().empty();
                uint32_t place = 0;
                for (ValueID argument : target.arguments()) {
                    if (uint16_t part = partAt(argument); part != none)
                        setPart(index.block(target.block()).parameter(place), part);
                    ++place;
                }
            }
            if (!hasProofs)
                continue;
            index.forEachEdgeResult(block, [&](const BodyEdgeResult& result) {
                if (uint16_t part = partAt(result.subject); part != none)
                    setPart(result.parameter, part);
            });
        }
    }

    Walker& m_walker;
    Vector<Part, 64> m_parts;
    Vector<Visit, 32> m_visits;
    uint32_t m_operations { 0 };
    unsigned m_numberOfOpened { 0 };
};

class PrefetchSteps {
    WTF_MAKE_NONCOPYABLE(PrefetchSteps);
public:
    explicit PrefetchSteps(const PartsOpenedByCode& opened)
        : m_parts(opened.parts())
    {
        m_instances.append({ 0, 0, PrefetchStep::Kind::Element, 0, linesOf(0), true });
        for (size_t next = 0; next < m_instances.size(); ++next)
            addPartsOf(next);
    }

    size_t numberOfCells() const { return m_instances.size(); }
    unsigned numberOfLines() const
    {
        unsigned lines = 0;
        for (const Instance& instance : m_instances)
            lines += instance.lines;
        return lines;
    }
    unsigned maxDepth() const
    {
        unsigned depth = 0;
        for (const Instance& instance : m_instances)
            depth = std::max<unsigned>(depth, instance.depth);
        return depth;
    }

    Vector<uint32_t> words() const
    {
        Vector<uint32_t> words;
        unsigned deepest = maxDepth();
        auto append = [&](PrefetchStep::Kind kind, size_t parent, size_t slotOrCount, unsigned linesOrDistance, PropertyKeyID key = { }) {
            words.append(key ? key.index() : 0);
            words.append(PrefetchStep { kind, static_cast<uint8_t>(parent), static_cast<uint8_t>(slotOrCount), static_cast<uint8_t>(linesOrDistance) }.encoded());
        };
        append(PrefetchStep::Kind::Element, 0, deepest + 1, m_instances[0].lines);
        for (unsigned depth = 1; depth <= deepest; ++depth) {
            append(PrefetchStep::Kind::Row, 0, 0, deepest - depth + 1);
            for (size_t place = 1; place < m_instances.size(); ++place) {
                const Instance& instance = m_instances[place];
                if (instance.depth != depth)
                    continue;
                if (instance.kind != PrefetchStep::Kind::Field) {
                    append(instance.kind, instance.parent, place, instance.lines);
                    continue;
                }
                size_t end = place;
                while (end < m_instances.size() && m_instances[end].kind == PrefetchStep::Kind::Field && m_instances[end].parent == instance.parent)
                    ++end;
                append(PrefetchStep::Kind::Fields, instance.parent, end - place, 0);
                for (; place < end; ++place)
                    append(PrefetchStep::Kind::Field, 0, m_instances[place].isParent ? place : PrefetchStep::noSlot, m_instances[place].lines, m_parts[m_instances[place].part].key);
                --place;
            }
        }
        return words;
    }

private:
    struct Instance {
        uint16_t part;
        uint16_t parent;
        PrefetchStep::Kind kind;
        uint8_t depth;
        uint8_t lines;
        bool isParent;
    };

    uint8_t linesOfStorage(uint16_t part) const { return std::min<unsigned>((m_parts[part].endOfWrittenIndexes + elementsPerLine + elementsPerLine / 2) / elementsPerLine, PrefetchStep::maxLines); }
    uint8_t linesOf(uint16_t part) const { return std::min<unsigned>(1 + m_parts[part].numberOfNames / namesPerLine, PrefetchStep::maxLines); }

    void add(uint16_t part, uint16_t parent, PrefetchStep::Kind kind)
    {
        bool isStorage = kind == PrefetchStep::Kind::Storage;
        unsigned depth = m_parts[part].depth + isStorage;
        if (m_instances.size() >= PrefetchStep::maxSlots || depth > PrefetchStep::maxDepth)
            return;
        m_instances[parent].isParent = true;
        m_instances.append({ part, parent, kind, static_cast<uint8_t>(depth), isStorage ? linesOfStorage(part) : linesOf(part), false });
    }

    void addPartsOf(size_t place)
    {
        Instance instance = m_instances[place];
        auto parent = static_cast<uint16_t>(place);
        if (instance.kind == PrefetchStep::Kind::Storage)
            return;
        if (m_parts[instance.part].hasElementsRead)
            add(instance.part, parent, PrefetchStep::Kind::Storage);
        for (uint16_t part = instance.part + 1; part < m_parts.size(); ++part) {
            if (m_parts[part].parent != instance.part || !m_parts[part].isOpened)
                continue;
            if (m_parts[part].kind == PartsOpenedByCode::Kind::Field) {
                add(part, parent, PrefetchStep::Kind::Field);
                continue;
            }
            add(part, parent, PrefetchStep::Kind::FirstElement);
            add(part, parent, PrefetchStep::Kind::SecondElement);
        }
    }

    static constexpr unsigned namesPerLine = 6;
    static constexpr unsigned elementsPerLine = 8;

    std::span<const PartsOpenedByCode::Part> m_parts;
    Vector<Instance, PrefetchStep::maxSlots> m_instances;
};

} // anonymous namespace

static bool isSteppedByLoop(const BodyIndex& index, LoopID loop, ValueID position)
{
    BlockID head = index.blockOf(position);
    uint32_t place = position.index() - index.block(head).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != head)
                continue;
            ValueID next = place < target.arguments().size() ? index.stripCasts(target.arguments()[place]) : ValueID();
            if (!next || index.isParameter(next))
                return false;
            BodyValue sum = index.value(next);
            if (sum.opcode() != Opcode::Increment && sum.opcode() != Opcode::Add)
                return false;
            bool stepsCounter = false;
            sum.forEachOperand([&](ValueID operand, uint32_t) {
                stepsCounter |= operand && index.stripCasts(operand) == position;
            });
            if (!stepsCounter)
                return false;
        }
    }
    return true;
}

static constexpr unsigned linesBeyondCaches = 1u << 16;
static constexpr unsigned shortestPrefetchedArray = 1024;
static constexpr unsigned fewestPrefetchedCells = 12;
static constexpr unsigned leastPrefetchedDepth = 3;

static Walker::PrefetchOfRead prefetchOfElement(Walker& walker, const BodyIndex& index, ValueID element, LoopID loop)
{
    PartsOpenedByCode opened(walker);
    opened.collect(walker.currentFunction(), index, loop, element);
    PrefetchSteps steps(opened);
    if (steps.numberOfCells() < fewestPrefetchedCells || steps.maxDepth() < leastPrefetchedDepth)
        return { };
    return { steps.words(), std::max(shortestPrefetchedArray, linesBeyondCaches / steps.numberOfLines()) };
}

static void buildPrefetch(Walker& walker, ValueID element, Reference list, Reference position, Origin origin, NOESCAPE const Invocable<Walker::PrefetchOfRead()> auto& find)
{
    if (!list || !position || !list.isRecord() || !position.isRecord() || !walker.types().at(walker.typeOf(position)).isInteger() || !walker.types().at(walker.typeOf(list)).hasObjects())
        return;
    const Walker::PrefetchOfRead& prefetched = walker.prefetchesOfReads().ensure(static_cast<uint64_t>(walker.currentFunction().index()) << 32 | element.index(), find).iterator->value;
    if (prefetched.steps.isEmpty())
        return;
    origin.isTailCall = false;
    Draft prefetch(Opcode::PrefetchElements, origin);
    walker.chooseConservativeFields(prefetch);
    prefetch.setListOperand(list);
    prefetch.setPositionOperand(position);
    prefetch.setSteps(prefetched.steps.span());
    prefetch.setMinLength(prefetched.minLength);
    walker.build(WTF::move(prefetch));
    walker.count(StatisticID::ArraysPrefetchedByLoops);
}

static constexpr OpcodeSet callsThatPassParts { Opcode::Call, Opcode::CallDirect };

void prefetchElementsAhead(Walker& walker, const BodyValue& read)
{
    if (!walker.isInLoop())
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID position = index.stripCasts(read.propertyOperand());
    ValueID list = index.stripCasts(read.objectOperand());
    if (!position || !list || !index.isParameter(position))
        return;
    BlockID head = index.blockOf(position);
    if (!index.isLoopHead(head) || list.index() >= index.block(head).firstParameter().index())
        return;
    LoopID loop = index.loopOf(head);
    if (index.loopOf(index.blockOf(read.id())) != loop || !index.holdsAnyOf(callsThatPassParts))
        return;
    buildPrefetch(walker, read.id(), walker.referenceOf(read.objectOperand()), walker.referenceOf(read.propertyOperand()), walker.originOf(read), [&]() -> Walker::PrefetchOfRead {
        if (!isSteppedByLoop(index, loop, position))
            return { };
        return prefetchOfElement(walker, index, read.id(), loop);
    });
}

void prefetchIteratedElementsAhead(Walker& walker, Reference list, Reference position)
{
    if (!walker.isInLoop())
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID resume = walker.valueBeingWalked();
    if (!resume || index.isParameter(resume) || index.opcodeOf(resume) != Opcode::IteratorResume || !index.holdsAnyOf(callsThatPassParts))
        return;
    BlockID block = index.blockOf(resume);
    LoopID loop = index.loopOf(block);
    if (!loop)
        return;
    ValueID element;
    for (BodyValue operation : index.block(block).values()) {
        if (operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == resume && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
            element = operation.id();
    }
    if (!element)
        return;
    buildPrefetch(walker, element, list, position, walker.originOf(index.value(resume)), [&] {
        return prefetchOfElement(walker, index, element, loop);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
