/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkListsOfFields.h"

#if ENABLE(AOT)

#include "AOTBuiltinsInReach.h"
#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include <algorithm>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ListsOfFields {
public:
    ListsOfFields(const LinkFlow& flow, const LinkTables& tables)
        : m_flow(flow)
        , m_tables(tables)
        , m_program(flow.program)
        , m_bodies(bodiesLookedAt(flow.looks))
        , m_recordOfFamily(FillWith { }, flow.numberOfSites(), none)
        , m_familiesNamedElsewhere(flow.numberOfSites())
    {
        for (ASCIILiteral name : { "push"_s, "pop"_s, "shift"_s, "unshift"_s, "splice"_s, "slice"_s, "concat"_s, "join"_s, "indexOf"_s, "lastIndexOf"_s, "includes"_s, "at"_s, "forEach"_s, "map"_s, "filter"_s, "some"_s, "every"_s, "find"_s, "findIndex"_s, "findLast"_s, "findLastIndex"_s, "reduce"_s, "reduceRight"_s })
            addMethod(name, m_methods);
        for (ASCIILiteral name : { "sort"_s, "reverse"_s, "fill"_s, "copyWithin"_s })
            addMethod(name, m_methodsReturningReceiver);
    }

    Vector<LinkedReceiverAsMade> find()
    {
        findFields();
        if (!m_records.isEmpty())
            findMakers();
        if (!m_records.isEmpty())
            lookAtSummaries();
        if (!m_records.isEmpty())
            lookAtBodies();
        if (!m_records.isEmpty())
            lookAtStores();
        if (!m_records.isEmpty())
            lookAtNodes();
        Vector<LinkedReceiverAsMade> stores;
        for (const Record& record : m_records)
            stores.appendVector(record.stores);
        std::ranges::sort(stores);
        return stores;
    }

private:
    static constexpr uint32_t none = UINT32_MAX;

    struct Read {
        FunctionID function;
        ValueID site;
        ValueID lastUse;
        bool isUsedInOtherBlocks { false };
    };
    struct Record {
        uint32_t family { none };
        PropertyKeyID key;
        ModuleID module;
        Vector<LinkedReceiverAsMade, 2> makers;
        Vector<LinkedReceiverAsMade, 2> stores;
        Vector<Read, 8> reads;
    };
    struct ValueOfRecord {
        FunctionID function;
        ValueID value;
        uint32_t record;
        bool isRead;
        uint32_t place;
    };

    void addMethod(ASCIILiteral name, Vector<PropertyKeyID, 24>& methods)
    {
        BuiltinName builtin = m_program.builtins().nameOf(byteCast<uint8_t>(name.span()));
        if (PropertyKeyID key = builtin ? m_program.keyOf(builtin) : PropertyKeyID())
            methods.append(key);
    }

    void nameElsewhere(uint32_t family) { m_familiesNamedElsewhere.quickSet(family); }
    void dropRecordsNamedElsewhere()
    {
        m_records.removeAllMatching([&](const Record& record) {
            return m_familiesNamedElsewhere.quickGet(record.family);
        });
        std::ranges::fill(m_recordOfFamily, none);
        for (uint32_t index = 0; index < m_records.size(); ++index)
            m_recordOfFamily[m_records[index].family] = index;
    }
    uint32_t recordAt(FlowNodeID node) const
    {
        if (!m_flow.holdsInstancesAt(node))
            return none;
        SiteID site = m_flow.typeAt(node).site;
        return site ? m_recordOfFamily[m_flow.familyOf(site).index()] : none;
    }

    void findFields()
    {
        for (uint32_t number = 0; number < m_program.layouts.size(); ++number) {
            LayoutID id(number);
            const Layout& layout = m_program.at(id);
            const LinkedLayout& linked = m_tables.layout(id);
            bool staysInSight = !layout.isEnvironment() && !linked.reachesUnseenCode && linked.possibleChanges.isEmpty() && !linked.isFilledUnderComputedKeys;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                FlowNodeID node = m_flow.field({ id, field });
                if (!m_flow.holdsInstancesAt(node))
                    continue;
                LinkType type = m_flow.typeAt(node);
                if (!type.site)
                    continue;
                uint32_t family = m_flow.familyOf(type.site).index();
                PropertyKeyID key = layout.fields[field].key;
                bool holdsListsAlone = staysInSight && key && !liesInThread(key) && m_program.at(key).kind == PropertyKeyKind::Text && !m_program.at(key).owner && m_tables.field({ id, field }).everyWriterIsKnown
                    && type.classes == LinkType::Classes::Site && m_flow.classOf(type.site) == BuiltinClasses::Array && m_flow.isOfClassItself(type.site) && !m_flow.hasEscaped(type.site)
                    && type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && !type.builtin && !type.hasUnknownObjects && !type.holdsLostBuiltins
                    && !(LinkType::inNamesOfPrimitive(type.primitives) & ~(Primitive::UndefinedOrNull | Primitive::Absent));
                uint32_t& record = m_recordOfFamily[family];
                if (!holdsListsAlone || (record != none && m_records[record].key != key)) {
                    nameElsewhere(family);
                    continue;
                }
                m_possibleHolderNodes.add(node.index());
                if (record == none) {
                    record = static_cast<uint32_t>(m_records.size());
                    m_records.append(Record { .family = family, .key = key, .module = layout.module });
                } else if (m_records[record].module != layout.module)
                    m_records[record].module = { };
            }
        }
        dropRecordsNamedElsewhere();
    }

    LinkedReceiverAsMade storeOfMadeList(const Record& record, FunctionID function, ValueID made, bool& isKnown) const
    {
        isKnown = false;
        const BodyIndex* index = function ? m_bodies.find(function) : nullptr;
        if (!index || !made || index->isParameter(made))
            return { };
        static constexpr OpcodeSet makers { Opcode::Data, Opcode::NewArray, Opcode::Call };
        if (!makers.contains(index->opcodeOf(made)))
            return { };
        ValueID user = index->onlyUsingValue(made);
        ValueID used = made;
        while (user && inputIdentityOpcodes.contains(index->opcodeOf(user))) {
            used = user;
            user = index->onlyUsingValue(user);
        }
        if (!user || index->blockOf(user) != index->blockOf(made))
            return { };
        BodyValue store = index->value(user);
        if (store.opcode() == Opcode::PropSet) {
            auto set = store.as<Opcode::PropSet>();
            isKnown = !set.isFreeName() && set.key() == record.key && set.valueOperand() == used && set.objectOperand() != used;
            return isKnown ? LinkedReceiverAsMade { function, user } : LinkedReceiverAsMade();
        }
        if (store.opcode() == Opcode::PropDefine) {
            auto definition = store.as<Opcode::PropDefine>();
            isKnown = definition.key() == record.key && definition.valueOperand() == used && definition.objectOperand() != used;
            return { };
        }
        isKnown = store.opcode() == Opcode::NewObject;
        return { };
    }

    void findMakers()
    {
        Vector<SiteID> sites;
        for (uint32_t site = 0; site < m_flow.numberOfSites(); ++site) {
            if (m_recordOfFamily[m_flow.familyOf(SiteID(site)).index()] != none)
                sites.append(SiteID(site));
        }
        Vector<LinkFlow::SiteMaker> makers = m_flow.findMakersOf(sites.span());
        for (size_t place = 0; place < sites.size(); ++place) {
            uint32_t family = m_flow.familyOf(sites[place]).index();
            Record& record = m_records[m_recordOfFamily[family]];
            bool isKnown = false;
            LinkedReceiverAsMade store = storeOfMadeList(record, makers[place].function, makers[place].value, isKnown);
            if (!isKnown) {
                nameElsewhere(family);
                continue;
            }
            record.makers.append({ makers[place].function, makers[place].value });
            if (store.function)
                record.stores.append(store);
        }
        for (const Record& record : m_records) {
            if (record.makers.isEmpty() || record.stores.isEmpty())
                nameElsewhere(record.family);
        }
        dropRecordsNamedElsewhere();
    }

    void lookAtSummaries()
    {
        BitVector keys;
        BitVector functionsOfMakers(m_program.functions.size());
        for (const Record& record : m_records) {
            keys.set(record.key.index());
            for (const LinkedReceiverAsMade& maker : record.makers)
                functionsOfMakers.quickSet(maker.function.index());
        }
        auto readsSuchField = [&](const SummaryAccess& access) {
            return access.kind == SummaryAccess::Kind::Read && access.key && !liesInThread(access.key) && keys.get(access.key.index());
        };
        for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
            if (!m_flow.reached.get(number))
                continue;
            FunctionID function(number);
            const ThinSummary& summary = m_flow.summaries.at(function);
            if (!functionsOfMakers.quickGet(number) && std::ranges::none_of(summary.accesses, readsSuchField))
                continue;
            std::span<const uint32_t> nodes = m_flow.nodesOfSummaryOf(function);
            for (uint32_t place = 0; place < nodes.size(); ++place) {
                FlowNodeID node(nodes[place]);
                uint32_t recordNumber = recordAt(node);
                if (recordNumber != none)
                    inspectSummaryNode(m_records[recordNumber], function, summary, summary.nodes[place], node);
            }
        }
        dropRecordsNamedElsewhere();
    }

    void lookAtNodes()
    {
        m_flow.forEachContents(scopedLambda<void(SiteID, SlotOfContents, FlowNodeID)>([&](SiteID site, SlotOfContents slot, FlowNodeID node) {
            if (slot.stateSlot() && m_flow.classOf(site) == BuiltinClasses::ArrayIterator && !m_flow.hasEscaped(site))
                m_possibleHolderNodes.add(node.index());
        }));
        for (uint32_t number = 0; number < m_flow.numberOfNodes(); ++number) {
            FlowNodeID node(number);
            uint32_t recordNumber = recordAt(node);
            if (recordNumber == none || m_familiesNamedElsewhere.quickGet(m_records[recordNumber].family) || m_possibleHolderNodes.contains(number) || m_flow.isAccessedThroughFamilyUnder(node, m_records[recordNumber].key))
                continue;
            nameElsewhere(m_records[recordNumber].family);
        }
        dropRecordsNamedElsewhere();
    }

    void inspectSummaryNode(Record& record, FunctionID function, const ThinSummary& summary, const SummaryNode& node, FlowNodeID inFlow)
    {
        switch (node.kind) {
        case SummaryNode::Kind::Field:
            return;
        case SummaryNode::Kind::ClassInstance:
            m_possibleHolderNodes.add(inFlow.index());
            return;
        case SummaryNode::Kind::ResultOfAccess: {
            const SummaryAccess& access = summary.accesses[node.first];
            if (access.kind != SummaryAccess::Kind::Read || access.isToContents() || access.key != record.key || access.receiver != noSummaryNode || !access.site)
                break;
            m_possibleHolderNodes.add(inFlow.index());
            if (std::ranges::none_of(record.reads, [&](const Read& read) { return read.function == function && read.site == access.site; }))
                record.reads.append(Read { function, access.site, access.site });
            return;
        }
        case SummaryNode::Kind::ResultOfCall: {
            ValueID site = summary.calls[node.first].site;
            const BodyIndex* index = m_bodies.find(function);
            bool isMaker = std::ranges::contains(record.makers, LinkedReceiverAsMade { function, site });
            if (!site || !index || index->isParameter(site) || (!isMaker && !index->isUnused(site)))
                break;
            m_possibleHolderNodes.add(inFlow.index());
            return;
        }
        default:
            break;
        }
        nameElsewhere(record.family);
    }

    void lookAtBodies()
    {
        Vector<ValueOfRecord> values;
        for (uint32_t number = 0; number < m_records.size(); ++number) {
            Record& record = m_records[number];
            for (uint32_t place = 0; place < record.reads.size(); ++place)
                values.append({ record.reads[place].function, record.reads[place].site, number, true, place });
        }
        std::ranges::sort(values, { }, [](const ValueOfRecord& value) {
            return static_cast<uint64_t>(value.function.index()) << 32 | value.value.index();
        });
        for (size_t first = 0; first < values.size();) {
            size_t end = first + 1;
            while (end < values.size() && values[end].function == values[first].function)
                ++end;
            lookAtBody(values[first].function, values.span().subspan(first, end - first));
            first = end;
        }
        dropRecordsNamedElsewhere();
    }

    void lookAtBody(FunctionID function, std::span<const ValueOfRecord> reads)
    {
        const BodyIndex* index = m_bodies.find(function);
        auto nameAll = [&] {
            for (const ValueOfRecord& read : reads)
                nameElsewhere(m_records[read.record].family);
        };
        if (!index || m_program.at(function).isCoroutine())
            return nameAll();
        Uses uses { *index, reads, Vector<uint32_t>(FillWith { }, index->numberOfValues(), 0u), Vector<uint32_t>(FillWith { }, index->numberOfValues(), 0u) };
        for (size_t place = 0; place < reads.size(); ++place) {
            ValueID site = reads[place].value;
            if (index->isParameter(site) || index->opcodeOf(site) != Opcode::PropGet || index->value(site).isFreeName())
                return nameAll();
            uses.readsOfValues[site.index()] = static_cast<uint32_t>(place) + 1;
        }
        auto lose = [&](ValueID operand, auto...) { escape(uses, operand); };
        for (BlockID id : index->blocksInDominatorOrder()) {
            BodyBlock block = index->block(id);
            for (BodyValue value : block.values())
                lookAtUser(uses, value);
            block.terminator().forEachOperand(lose);
            auto loseAtEdge = [&](BodyTarget target, bool) {
                for (ValueID argument : target.arguments())
                    escape(uses, argument);
            };
            block.forEachOutgoingEdge(loseAtEdge);
            index->forEachEdgeResult(id, [&](const BodyEdgeResult& handed) {
                escape(uses, handed.subject);
            });
        }
    }

    struct Uses {
        uint32_t readOf(ValueID value) const { return value ? readsOfValues[value.index()] : 0; }
        uint32_t iterationOf(ValueID value) const { return value ? iterationsOfValues[value.index()] : 0; }

        const BodyIndex& index;
        std::span<const ValueOfRecord> reads;
        Vector<uint32_t> readsOfValues;
        Vector<uint32_t> iterationsOfValues;
    };

    void escape(const Uses& uses, ValueID value)
    {
        for (uint32_t read : { uses.readOf(value), uses.iterationOf(value) }) {
            if (read)
                nameElsewhere(m_records[uses.reads[read - 1].record].family);
        }
    }

    void noteUse(const Uses& uses, uint32_t place, ValueID user)
    {
        Read& read = m_records[uses.reads[place - 1].record].reads[uses.reads[place - 1].place];
        if (uses.index.blockOf(read.site) != uses.index.blockOf(user))
            read.isUsedInOtherBlocks = true;
        else if (read.lastUse < user)
            read.lastUse = user;
    }

    void lookAtUser(Uses& uses, const BodyValue& user)
    {
        static constexpr OpcodeSet questions { Opcode::ToBoolean, Opcode::TypeTest };
        static constexpr OpcodeSet stepsOfIteration { Opcode::IteratorResume, Opcode::IteratorClose };
        const Opcode opcode = user.opcode();
        bool usesList = false;
        user.forEachOperand([&](ValueID operand, auto...) {
            if (uint32_t read = uses.readOf(operand)) {
                usesList = true;
                noteUse(uses, read, user.id());
            }
            if (uint32_t read = uses.iterationOf(operand)) {
                noteUse(uses, read, user.id());
                if (!stepsOfIteration.contains(opcode))
                    escape(uses, operand);
            }
        });
        if (!usesList)
            return;
        auto escapeAllBut = [&](ValueID list) {
            bool isSeen = false;
            user.forEachOperand([&](ValueID operand, auto...) {
                if (operand != list || std::exchange(isSeen, true))
                    escape(uses, operand);
            });
        };
        if (inputPassThroughOpcodes.contains(opcode)) {
            uint32_t read = uses.readOf(user.operand(0));
            escapeAllBut(user.operand(0));
            if (read && !uses.readsOfValues[user.id().index()])
                uses.readsOfValues[user.id().index()] = read;
            return;
        }
        if (questions.contains(opcode))
            return;
        switch (opcode) {
        case Opcode::PropGet: {
            auto read = user.as<Opcode::PropGet>();
            return escapeAllBut(read.isFreeName() ? ValueID() : read.objectOperand());
        }
        case Opcode::PropSet: {
            auto store = user.as<Opcode::PropSet>();
            bool storesLength = !store.isFreeName() && m_program.nameClassOf(store.key()) == NameClass::Length;
            return escapeAllBut(storesLength ? store.objectOperand() : ValueID());
        }
        case Opcode::ElemGet:
            return escapeAllBut(user.as<Opcode::ElemGet>().objectOperand());
        case Opcode::ElemSet:
            return escapeAllBut(user.as<Opcode::ElemSet>().objectOperand());
        case Opcode::Call:
            return escapeAllBut(isArrayMethodCall(uses, user) ? user.as<Opcode::Call>().thisValueOperand() : ValueID());
        case Opcode::IteratorOpen: {
            auto open = user.as<Opcode::IteratorOpen>();
            if (open.iteratorKind() != IteratorKind::Sync || open.isDelegating())
                return escapeAllBut({ });
            uses.iterationsOfValues[user.id().index()] = uses.readOf(open.iterableOperand());
            return escapeAllBut(open.iterableOperand());
        }
        default:
            return escapeAllBut({ });
        }
    }

    bool isArrayMethodCall(const Uses& uses, const BodyValue& user) const
    {
        auto call = user.as<Opcode::Call>();
        uint32_t list = uses.readOf(call.thisValueOperand());
        ValueID callee = call.calleeOperand();
        while (callee && !uses.index.isParameter(callee) && inputPassThroughOpcodes.contains(uses.index.opcodeOf(callee)))
            callee = uses.index.value(callee).operand(0);
        if (!list || !callee || uses.index.isParameter(callee) || uses.index.opcodeOf(callee) != Opcode::PropGet)
            return false;
        auto method = uses.index.value(callee).as<Opcode::PropGet>();
        if (method.isFreeName() || uses.readOf(method.objectOperand()) != list)
            return false;
        return m_methods.contains(method.key()) || (m_methodsReturningReceiver.contains(method.key()) && uses.index.isUnused(user.id()));
    }

    void lookAtStores()
    {
        for (const Record& record : m_records) {
            if (!hasNoStoresDuringListUse(record))
                nameElsewhere(record.family);
        }
        dropRecordsNamedElsewhere();
    }

    bool hasNoStoresDuringListUse(const Record& record)
    {
        BitVector leadToStores(m_program.functions.size());
        Vector<FunctionID, 16> toVisit;
        for (const LinkedReceiverAsMade& store : record.stores) {
            if (!leadToStores.quickSet(store.function.index()))
                toVisit.append(store.function);
        }
        while (!toVisit.isEmpty()) {
            FunctionID function = toVisit.takeLast();
            const Function& body = m_program.at(function);
            if (body.isModuleBody()) {
                if (body.module != record.module || body.isCoroutine() || m_program.at(body.module).flags.contains(ModuleFlags::MayBeReevaluated))
                    return false;
                continue;
            }
            const LinkedFunction& linked = m_tables.function(function);
            if (!linked.everyCallerIsKnown || !linked.everyCallHasOneCallee || body.isCoroutine())
                return false;
            for (FunctionID caller : callersOf(function)) {
                if (!leadToStores.quickSet(caller.index()))
                    toVisit.append(caller);
            }
        }
        return std::ranges::all_of(record.reads, [&](const Read& read) {
            if (!leadToStores.quickGet(read.function.index()))
                return true;
            if (read.isUsedInOtherBlocks)
                return false;
            auto isBetween = [&](ValueID site) {
                return site > read.site && site <= read.lastUse;
            };
            for (const LinkedReceiverAsMade& store : record.stores) {
                if (store.function == read.function && isBetween(store.site))
                    return false;
            }
            return std::ranges::none_of(m_tables.callsOf(read.function), [&](const LinkedCall& call) {
                return isBetween(call.site) && leadToStores.quickGet(call.callee.index());
            });
        });
    }

    std::span<const FunctionID> callersOf(FunctionID callee)
    {
        if (m_endOfCallers.isEmpty())
            listCallers();
        uint32_t first = callee.index() ? m_endOfCallers[callee.index() - 1] : 0;
        return m_callers.span().subspan(first, m_endOfCallers[callee.index()] - first);
    }

    void listCallers()
    {
        m_endOfCallers.fill(0, m_program.functions.size());
        for (const LinkedCall& call : m_tables.calls) {
            if (call.callee)
                ++m_endOfCallers[call.callee.index()];
        }
        uint32_t numberOfCalls = 0;
        for (uint32_t& end : m_endOfCallers)
            end = numberOfCalls += end;
        m_callers.grow(numberOfCalls);
        Vector<uint32_t> next = m_endOfCallers;
        for (const LinkedCall& call : m_tables.calls) {
            if (call.callee)
                m_callers[--next[call.callee.index()]] = call.caller;
        }
    }

    const LinkFlow& m_flow;
    const LinkTables& m_tables;
    const Program& m_program;
    const BodyIndexTable& m_bodies;
    Vector<uint32_t> m_recordOfFamily;
    BitVector m_familiesNamedElsewhere;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_possibleHolderNodes;
    Vector<Record> m_records;
    Vector<PropertyKeyID, 24> m_methods;
    Vector<PropertyKeyID, 24> m_methodsReturningReceiver;
    Vector<FunctionID> m_callers;
    Vector<uint32_t> m_endOfCallers;
};

} // anonymous namespace

Vector<LinkedReceiverAsMade> storesFreeingFieldLists(const LinkFlow& flow, const LinkTables& tables)
{
    if (flow.leavesOutDirectEvals || !flow.program.openedNames.isEmpty() || flow.program.admitsInspector)
        return { };
    return ListsOfFields(flow, tables).find();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
