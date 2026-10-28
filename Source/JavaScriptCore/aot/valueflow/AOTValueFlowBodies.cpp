/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowOrder.h"
#include "AOTKnownClosures.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueFlowTypedResults.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Body = ValueFlowBody;
using NodeName = ValueFlowNodeName;
using ObjectName = ValueFlowObjectName;
using OperandForm = Body::Operand;
using Forms = Vector<OperandForm, 1>;

enum class Role : uint8_t {
    Nothing,
    Constant,
    Node,
    Result,
    Passing,
};

struct BodySource {
    ValueID value;
    uint32_t mask { 0 };

    friend bool operator==(const BodySource&, const BodySource&) = default;
};

struct Description {
    Role role { Role::Nothing };
    bool isLive { false };
    bool isLiveForComparisonsOnly { false };
    NodeName name;
    uint32_t constant { 0 };
    uint32_t firstSource { 0 };
    uint32_t sourceCount { 0 };
    uint32_t operandReaders { 0 };
    uint32_t otherReaders { 0 };
    uint32_t mask { 0 };
    ValueID head;
};

struct EdgeToNode {
    ValueID value;
    NodeName target;
};

struct PendingStatement {
    ValueID site;
    BlockID block;
    uint32_t firstOperand { 0 };
    uint32_t operandCount { 0 };
    bool leavesBody { true };
};

struct Guard {
    ValueID value;
    uint32_t mask { 0 };
};

struct GuardsOfBlock {
    Vector<Guard, 4> values;
    bool areAskedFor { false };
    bool areWritten { false };
    uint8_t count { 0 };
    uint32_t first { 0 };
};

constexpr OperandUses inspectsOrReturns { OperandUse::Inspects, OperandUse::Returns };

bool carriesProgramValues(OpaqueKind kind)
{
    switch (kind) {
    case OpaqueKind::IteratorState:
    case OpaqueKind::Exception:
        return true;
    case OpaqueKind::Enumerator:
    case OpaqueKind::ClassToken:
    case OpaqueKind::StringBuilder:
    case OpaqueKind::Cursor:
    case OpaqueKind::KeyOrder:
    case OpaqueKind::Guard:
    case OpaqueKind::Fact:
    case OpaqueKind::FlatString:
    case OpaqueKind::CodeAddress:
    case OpaqueKind::JSONHolders:
    case OpaqueKind::BuilderCursor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint16_t bodyKindsOf(uint16_t primitives)
{
    uint16_t kinds = primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol);
    if (primitives & Primitive::LiteralString)
        kinds |= Primitive::String;
    if (primitives & Primitive::LiteralSymbol)
        kinds |= Primitive::Symbol;
    return kinds;
}

template<typename Record>
class InternedBodyRecords {
public:
    explicit InternedBodyRecords(Vector<Record>& records)
        : m_records(records)
    {
        m_records.append(Record { });
        m_hashes.append(0);
    }

    uint32_t intern(Record&& record, unsigned hash)
    {
        AOT_THRESHOLD(size_t, mostSearchedInOrder, 24);
        if (m_records.size() <= mostSearchedInOrder) {
            for (uint32_t candidate = 1; candidate < m_hashes.size(); ++candidate) {
                if (m_hashes[candidate] == hash && m_records[candidate] == record)
                    return candidate;
            }
            m_hashes.append(hash);
            m_records.append(WTF::move(record));
            return m_records.size() - 1;
        }
        if (m_byHash.isEmpty()) {
            for (uint32_t candidate = 1; candidate < m_hashes.size(); ++candidate)
                m_byHash.add(m_hashes[candidate], Vector<uint32_t, 1> { }).iterator->value.append(candidate);
        }
        Vector<uint32_t, 1>& candidates = m_byHash.add(hash, Vector<uint32_t, 1> { }).iterator->value;
        for (uint32_t candidate : candidates) {
            if (m_records[candidate] == record)
                return candidate;
        }
        candidates.append(m_records.size());
        m_records.append(WTF::move(record));
        return m_records.size() - 1;
    }

private:
    Vector<Record>& m_records;
    Vector<unsigned, 25> m_hashes;
    HashMap<unsigned, Vector<uint32_t, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_byHash;
};

class BodyReducer {
    WTF_MAKE_NONCOPYABLE(BodyReducer);
public:
    BodyReducer(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id, const UnobservedFieldDefinitions* unobservedDefinitions)
        : m_program(program)
        , m_tables(tables)
        , m_unobservedDefinitions(unobservedDefinitions)
        , m_id(id)
        , m_function(program.at(id))
        , m_cfg(m_function)
        , m_constants(m_body.constants)
        , m_masks(m_body.masks)
        , m_descriptions(m_function.values.size())
        , m_caughtNodes(m_function.blocks.size())
        , m_targetsWithHostError(0)
    {
    }

    Body reduce()
    {
        addInterface();
        m_isBehindCheck = valuesTypedBehindChecks(m_program, m_function, m_cfg);
        for (BlockID id : m_cfg.reversePostOrder()) {
            const Block& block = m_function.at(id);
            for (uint32_t index = 0; id != entryBlock && index < block.parameters.size(); ++index) {
                if (!carriesValue(block.parameters[index]))
                    continue;
                at(block.parameters[index]).role = Role::Passing;
                m_blockParameters.append({ id, index, block.parameters[index] });
            }
            findGuardsOf(id);
            for (ValueID value : block.values)
                describeValue(id, value);
            describeTerminator(id, block.terminator);
        }
        markLiveValues();
        countReaders();
        addResultNodes();
        resolveValues();
        emitEdges();
        emitStatements();
        noteClassesOfInstances();
        noteUsesOfModules();
        appendSitesOfTypedResults(m_program, m_tables, m_function, m_cfg, m_body.sites);
        sortSites();
        sortAndRemoveRepeats(m_body.edges);
        sortAndRemoveRepeats(m_body.edgesOnceReached);
        sortAndRemoveRepeats(m_body.seedsOnceReached);
        sortAndRemoveRepeats(m_body.functionsEnteredOnceReached);
        m_body.knowsValuesBehindChecks = true;
        if (!m_isBehindCheck.isEmpty())
            m_body.valuesBehindChecks = WTF::move(m_isBehindCheck);
        return WTF::move(m_body);
    }

private:
    static void sortAndRemoveRepeats(auto& list)
    {
        AOT::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    }

    void noteClassesOfInstances()
    {
        const BuiltinTable& builtins = m_program.builtins();
        BitVector isNoted;
        auto note = [&](BuiltinClassID builtinClass) {
            for (BuiltinClassID current = builtinClass; current && !isNoted.set(current.index()); current = builtins.at(current).parent) {
                for (BuiltinID object : { builtins.at(current).constructor, builtins.at(current).prototype }) {
                    if (!object)
                        continue;
                    object = builtins.canonicalID(object);
                    if (builtins.calledFunctionsOf(object).empty() && builtins.moduleLoadsOf(object).empty())
                        continue;
                    m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::UsedBuiltins), constantOf({ ValueFlowObjectKind::Builtin, object.index(), { } }), 0 });
                }
            }
        };
        for (const ValueFlowSite& site : m_body.sites) {
            if (site.builtinClass)
                note(site.builtinClass);
            else if (site.layout)
                note(builtinBaseClassOf(m_program, site.layout));
        }
        if (!m_function.isUnsplitCoroutine())
            return;
        if (m_function.has(FunctionFlags::IsGenerator))
            note(m_function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator);
        else
            note(BuiltinClasses::Promise);
    }

    void noteUsesOfModules()
    {
        auto use = [&](ModuleID module) {
            if (module && awaitsUse(m_program, module) && m_program.at(module).body != m_id)
                m_body.functionsEnteredOnceReached.append(m_program.at(module).body);
        };
        auto useVariableOf = [&](ModuleID module) {
            if (!module || module == m_function.module)
                return;
            FunctionID body = m_program.at(module).body;
            if (body && m_program.at(body).hasBody())
                m_body.functionsEnteredOnceReached.append(body);
        };
        use(m_function.module);
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::PutModuleVariable)
                    useVariableOf(m_program.at(value.variable()).module);
            }
        }
    }

    void sortSites()
    {
        Vector<uint32_t> order(m_body.sites.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        AOT::sort(order, { }, [&](uint32_t index) {
            return std::pair { m_body.sites[index].value, m_body.sites[index].ordinal };
        });
        Vector<uint32_t> placeOf(order.size());
        Vector<ValueFlowSite> sorted;
        sorted.reserveInitialCapacity(order.size());
        for (uint32_t place = 0; place < order.size(); ++place) {
            placeOf[order[place]] = place;
            sorted.append(m_body.sites[order[place]]);
        }
        m_body.sites = WTF::move(sorted);
        for (Body::Constant& constant : m_body.constants) {
            for (ObjectName& object : constant.objects) {
                if (object.kind == ValueFlowObjectKind::Site)
                    object.index = placeOf[object.index];
            }
            AOT::sort(constant.objects);
        }
        for (Body::Statement& statement : m_body.statements) {
            auto first = std::ranges::lower_bound(m_body.sites, statement.site, { }, &ValueFlowSite::value);
            if (statement.site && first != m_body.sites.end() && first->value == statement.site)
                statement.firstSite = static_cast<uint32_t>(first - m_body.sites.begin());
        }
    }

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    Description& at(ValueID value) LIFETIME_BOUND { return m_descriptions[value.index()]; }

    uint32_t constantOf(uint16_t primitives, std::span<const ObjectName> objects)
    {
        if (!primitives && objects.empty())
            return 0;
        Body::Constant constant { primitives, Vector<ObjectName, 1>(objects) };
        Hasher hasher;
        add(hasher, primitives);
        for (const ObjectName& object : constant.objects)
            add(hasher, static_cast<uint8_t>(object.kind), object.index, object.module);
        return m_constants.intern(WTF::move(constant), hasher.hash());
    }

    uint32_t constantOf(uint16_t primitives) { return constantOf(primitives, { }); }
    uint32_t constantOf(const ObjectName& object) { return constantOf(0, singleElementSpan(object)); }

    uint32_t constantOfType(const Type& type)
    {
        uint16_t primitives = bodyKindsOf(type.primitives);
        if (!type.hasObjects())
            return constantOf(primitives);
        ObjectName unknownObject { ValueFlowObjectKind::UnknownObject, 0, { } };
        return constantOf(primitives, singleElementSpan(unknownObject));
    }

    uint32_t uniteConstants(uint32_t first, uint32_t second)
    {
        if (!first || first == second)
            return second;
        if (!second)
            return first;
        Vector<ObjectName, 4> objects;
        objects.appendVector(m_body.constants[first].objects);
        objects.appendVector(m_body.constants[second].objects);
        sortAndRemoveRepeats(objects);
        return constantOf(m_body.constants[first].primitives | m_body.constants[second].primitives, objects.span());
    }

    uint32_t internMask(Body::Mask&& mask)
    {
        if (mask.admitsEverything())
            return 0;
        Hasher hasher;
        add(hasher, static_cast<uint8_t>(mask.kind));
        for (TypeID type : mask.inside)
            add(hasher, type);
        add(hasher, 0x5eed);
        for (TypeID type : mask.outside)
            add(hasher, type);
        for (auto [builtin, module] : mask.outsideBuiltinsOfModules)
            add(hasher, builtin.index(), module.index());
        for (BuiltinClassID builtinClass : mask.insideOneOfClasses)
            add(hasher, builtinClass.index());
        add(hasher, 0x5eed);
        for (BuiltinClassID builtinClass : mask.outsideClasses)
            add(hasher, builtinClass.index());
        return m_masks.intern(WTF::move(mask), hasher.hash());
    }

    uint32_t maskInside(TypeID type, ValueFlowMaskKind kind)
    {
        const Type& record = m_program.at(type);
        if (record.isAnything() || !record.isJavaScriptValue())
            return 0;
        return internMask({ { type }, { }, kind });
    }

    uint32_t maskOutside(TypeID type) { return internMask({ { }, { type }, ValueFlowMaskKind::Checked }); }
    uint32_t maskOutside(BuiltinID builtin, ModuleID module) { return internMask({ { }, { }, ValueFlowMaskKind::Checked, { { builtin, module } } }); }

    uint32_t maskOfTruthinessTest(EdgeSide side)
    {
        bool isTaken = side == EdgeSide::Taken;
        Vector<TypeID, 1> outside { isTaken ? TypeTable::nullish : TypeTable::object };
        std::optional<std::optional<TypeID>>& known = m_typeOfBoolean[isTaken];
        if (!known)
            known = m_program.types.find(Type::fromPrimitives(isTaken ? Primitive::False : Primitive::True));
        if (std::optional<TypeID> boolean = *known)
            outside.append(*boolean);
        return internMask({ { }, WTF::move(outside), ValueFlowMaskKind::Checked });
    }

    uint32_t maskOfCellTypeTest(const Value& test, EdgeSide side)
    {
        const BuiltinTable& builtins = m_program.builtins();
        JSType first = toJSType(test.first());
        JSType last = toJSType(test.last());
        Vector<BuiltinClassID, 0> classes;
        BitVector typesWithClass;
        for (uint32_t index = 0; index < builtins.classCount(); ++index) {
            const BuiltinClass& candidate = builtins.at(BuiltinClassID(index));
            if (candidate.instanceTest != BuiltinInstanceTest::JSTypeEquals || candidate.type < first || candidate.type > last)
                continue;
            classes.append(candidate.id);
            typesWithClass.set(candidate.type - first);
        }
        if (side == EdgeSide::NotTaken)
            return internMask({ { }, { }, ValueFlowMaskKind::Checked, { }, { }, WTF::move(classes) });
        if (first > last || typesWithClass.bitCount() != static_cast<size_t>(last - first) + 1)
            return 0;
        return internMask({ { }, { }, ValueFlowMaskKind::Checked, { }, WTF::move(classes), { } });
    }

    bool areOfOneKind(uint32_t first, uint32_t second) const { return !first || !second || m_body.masks[first].kind == m_body.masks[second].kind; }

    uint32_t intersectMasks(uint32_t first, uint32_t second)
    {
        if (!first || first == second)
            return second;
        if (!second)
            return first;
        RELEASE_ASSERT(areOfOneKind(first, second));
        auto unite = [](const Vector<TypeID, 1>& a, const Vector<TypeID, 1>& b) {
            Vector<TypeID, 1> all;
            all.appendVector(a);
            all.appendVector(b);
            sortAndRemoveRepeats(all);
            return all;
        };
        auto outsideBuiltinsOfModules = m_body.masks[first].outsideBuiltinsOfModules;
        for (auto object : m_body.masks[second].outsideBuiltinsOfModules) {
            if (!outsideBuiltinsOfModules.contains(object))
                outsideBuiltinsOfModules.append(object);
        }
        // Of two lists of which one member has to hold the value, one list is kept: the mask lets more through.
        auto insideOneOfClasses = m_body.masks[first].insideOneOfClasses.isEmpty() ? m_body.masks[second].insideOneOfClasses : m_body.masks[first].insideOneOfClasses;
        auto outsideClasses = m_body.masks[first].outsideClasses;
        for (BuiltinClassID builtinClass : m_body.masks[second].outsideClasses) {
            if (!outsideClasses.contains(builtinClass))
                outsideClasses.append(builtinClass);
        }
        return internMask({ unite(m_body.masks[first].inside, m_body.masks[second].inside), unite(m_body.masks[first].outside, m_body.masks[second].outside), m_body.masks[first].kind, WTF::move(outsideBuiltinsOfModules), WTF::move(insideOneOfClasses), WTF::move(outsideClasses) });
    }

    uint32_t maskFor(TypeID from, TypeID to)
    {
        if (from == to || m_program.at(from).isSubsetOf(m_program.at(to), m_program))
            return 0;
        const Type& target = m_program.at(to);
        if (target.isAnything() && !(target.primitives & Primitive::Absent))
            return maskOutside(TypeTable::absent);
        return maskInside(to, ValueFlowMaskKind::Checked);
    }

    static ValueFlowMaskKind kindOfResultMask(Opcode opcode)
    {
        switch (flowOf(opcode)) {
        case ValueFlowEdgeClass::Allocates:
        case ValueFlowEdgeClass::TableProves:
        case ValueFlowEdgeClass::Checks:
            return ValueFlowMaskKind::Checked;
        case ValueFlowEdgeClass::Copies:
        case ValueFlowEdgeClass::Unsound:
            return ValueFlowMaskKind::Declared;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    NodeName addNode(ValueFlowBodyNodeKind kind, ValueID value, uint32_t mask = 0)
    {
        m_body.nodes.append({ kind, value, mask });
        return NodeName::ofBody(m_body.nodes.size() - 1);
    }

    ObjectName addSite(const ValueFlowSite& site)
    {
        m_body.sites.append(site);
        return { ValueFlowObjectKind::Site, static_cast<uint32_t>(m_body.sites.size() - 1), { } };
    }

    ObjectName addSiteFor(ValueID id)
    {
        const Type& type = typeOf(id);
        const ClassPart* classPart = type.singleBuiltinClass();
        return addSite({ ValueFlowSiteOrigin::Operation, id, 0, type.singleLayout(m_program), classPart ? classPart->builtinClass : BuiltinClassID() });
    }

    void addInterface()
    {
        const Block& entry = m_function.at(entryBlock);
        m_body.function = m_id;
        m_body.numberOfEntryParameters = m_function.numberOfEntryParameters();
        m_body.numberOfResults = m_function.results.size();
        m_body.hasYieldedAndSent = m_function.isUnsplitCoroutine() && m_function.has(FunctionFlags::IsGenerator);
        RELEASE_ASSERT(entry.parameters.size() == m_body.numberOfEntryParameters);

        for (ValueID parameter : entry.parameters) {
            NodeName node = addNode(ValueFlowBodyNodeKind::EntryParameter, parameter, carriesValue(parameter) ? maskInside(m_function.at(parameter).type, ValueFlowMaskKind::Declared) : 0);
            if (carriesValue(parameter))
                at(parameter) = { .role = Role::Node, .name = node };
        }
        for (uint32_t index = 0; index < m_body.numberOfResults; ++index)
            addNode(ValueFlowBodyNodeKind::Result, ValueID());
        addNode(ValueFlowBodyNodeKind::Thrown, ValueID());
        if (m_body.hasYieldedAndSent) {
            addNode(ValueFlowBodyNodeKind::Yielded, ValueID());
            addNode(ValueFlowBodyNodeKind::Sent, ValueID());
        }

        std::optional<unsigned> restParameter = m_function.hiddenRestParameterIndex();
        if (!restParameter && m_function.has(FunctionFlags::HasRestParameter))
            restParameter = m_function.firstParameterIndex() + m_function.numberOfPositionalParameters();
        if (restParameter)
            m_body.seedsOnceReached.append({ NodeName::ofBody(*restParameter), constantOf({ ValueFlowObjectKind::RestArray, m_id.index(), { } }), 0 });
    }

    bool carriesValue(ValueID id) const
    {
        const Value& value = m_function.at(id);
        if (!value.type)
            return false;
        const Type& type = m_program.at(value.type);
        if (type.environment)
            return false;
        return !type.opaque || carriesProgramValues(*type.opaque);
    }

    NodeName thrownTargetOf(BlockID block)
    {
        const Block& record = m_function.at(block);
        return record.handler ? caughtNodeOf(record.handler->block) : NodeName::ofBody(m_body.thrown());
    }

    NodeName caughtNodeOf(BlockID handler)
    {
        NodeName& node = m_caughtNodes[handler.index()];
        if (!node)
            node = addNode(ValueFlowBodyNodeKind::Caught, ValueID());
        return node;
    }

    static bool mayThrowValue(Opcode opcode) { return mayThrow(opcode) || mayRunOperandCode(opcode); }

    void noteHostError(BlockID block)
    {
        NodeName target = thrownTargetOf(block);
        if (!m_targetsWithHostError.set(target.owner))
            m_body.seedsOnceReached.append({ target, constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }), 0 });
    }

    void beginSources(ValueID id)
    {
        at(id).role = Role::Passing;
        at(id).firstSource = m_sources.size();
    }

    void addSource(ValueID id, ValueID value, uint32_t mask)
    {
        if (!value || at(value).role == Role::Nothing)
            return;
        Description& description = at(id);
        BodySource source { value, mask };
        std::span<const BodySource> sources = m_sources.subspan(description.firstSource);
        if (sources.size() < minSourcesFoundByHash) {
            if (std::ranges::contains(sources, source))
                return;
        } else {
            auto keyOf = [](const BodySource& source) {
                return (static_cast<uint64_t>(source.value.index()) << 32 | source.mask) + 1;
            };
            if (std::exchange(m_valueWithHashedSources, id) != id) {
                m_hashedSources.clear();
                for (const BodySource& earlier : sources)
                    m_hashedSources.add(keyOf(earlier));
            }
            if (!m_hashedSources.add(keyOf(source)).isNewEntry)
                return;
        }
        m_sources.append(source);
        ++description.sourceCount;
    }

    std::span<const BodySource> sourcesOf(ValueID id) LIFETIME_BOUND { return m_sources.subspan(at(id).firstSource, at(id).sourceCount); }

    void setConstant(ValueID id, uint32_t constant) { at(id) = { .role = Role::Constant, .constant = constant }; }
    void setNode(ValueID id, NodeName name) { at(id) = { .role = Role::Node, .name = name }; }

    void connect(ValueID value, NodeName target)
    {
        if (value && at(value).role != Role::Nothing)
            m_edges.append({ value, target });
    }

    bool isBehindCheck(ValueID value) const { return value && m_isBehindCheck.get(value.index()); }

    static bool takesTypeFromOperands(Opcode opcode)
    {
        switch (resultRuleOf(opcode)) {
        case ResultRule::Unknown:
        case ResultRule::Cast:
            return true;
        case ResultRule::Natural:
            return answersByKindsOfOperands(opcode);
        case ResultRule::None:
        case ResultRule::Constant:
        case ResultRule::Allocation:
        case ResultRule::Declared:
        case ResultRule::Results:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isTypedByChecks(ValueID id) const { return isBehindCheck(id) && takesTypeFromOperands(m_function.at(id).opcode); }

    // A block that one edge leads to is entered only behind what that edge is behind. Out of a test the edge brings the tested value as the test lets it through.
    void findGuardsOf(BlockID id)
    {
        if (m_guardsOfBlock.isEmpty())
            m_guardsOfBlock.grow(m_function.blocks.size());
        std::span<const Edge> incoming = m_cfg.incomingEdges(id);
        if (id == entryBlock || incoming.size() != 1)
            return;
        GuardsOfBlock& guards = m_guardsOfBlock[id.index()];
        guards.values = m_guardsOfBlock[incoming[0].from.index()].values;
        const Block& block = m_function.at(id);
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            if (!carriesValue(parameter) || !isBehindCheck(parameter))
                continue;
            IncomingValue value = incomingValueOf(m_function, incoming[0], index);
            if (value.kind != IncomingValue::Kind::EdgeResult)
                continue;
            EdgeResult result = edgeResultAt(m_function, value.result);
            if (result.kind != EdgeResultKind::NarrowedOperand || (!result.test && !result.caseIndex))
                continue;
            addGuard(guards, { parameter, 0 });
        }
        const Terminator& terminator = m_function.at(incoming[0].from).terminator;
        if (terminator.isConditionalBranch() && !incoming[0].isHandlerEdge())
            addGuardOfCondition(guards, terminator.conditionOperand(), incoming[0].targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken);
    }

    void addGuard(GuardsOfBlock& guards, Guard guard)
    {
        AOT_THRESHOLD(size_t, mostGuardsOfBlock, 4);
        if (guards.values.size() == mostGuardsOfBlock)
            guards.values.removeAt(0);
        guards.values.append(guard);
    }

    // What the edge brings of the operand of the last conversion is a guard already.
    void addGuardOfCondition(GuardsOfBlock& guards, ValueID condition, EdgeSide side)
    {
        bool isNegated = false;
        ValueID tested = condition;
        for (;;) {
            const Value& value = m_function.at(tested);
            if (value.opcode == Opcode::LogicalNot) {
                isNegated = true;
                side = side == EdgeSide::Taken ? EdgeSide::NotTaken : EdgeSide::Taken;
            } else if (value.opcode != Opcode::ToBoolean && !isCast(value.opcode))
                break;
            tested = value.operand(0);
        }
        const Value& test = m_function.at(tested);
        if (test.opcode == Opcode::HasJSType) {
            if (uint32_t mask = maskOfCellTypeTest(test, side); mask && carriesValue(test.valueOperand()))
                addGuard(guards, { test.valueOperand(), mask });
            return;
        }
        if (isNegated && carriesValue(tested))
            addGuard(guards, { tested, maskOfTruthinessTest(side) });
    }

    void addSourcesOfBlockParameters()
    {
        for (auto [block, index, parameter] : m_blockParameters) {
            beginSources(parameter);
            for (Edge edge : m_cfg.incomingEdges(block)) {
                IncomingValue incoming = incomingValueOf(m_function, edge, index);
                switch (incoming.kind) {
                case IncomingValue::Kind::Argument:
                    addSource(parameter, incoming.argument, 0);
                    break;
                case IncomingValue::Kind::EdgeResult:
                    addEdgeResult(parameter, edgeResultAt(m_function, incoming.result), m_function.at(incoming.result.from).terminator);
                    break;
                }
            }
        }
    }

    static bool isStrictlyEqualOnlyToItself(const Type& type) { return (type.isSingleValue() || !type.hasObjects()) && !(type.primitives & Primitive::Number); }

    bool isNumberInEveryVersion(ValueID value) const
    {
        if (!isBehindCheck(value))
            return typeOf(value).isNumber();
        std::optional<Type> widest = tryWidestNaturalTypeOf(m_function.at(value));
        return widest && widest->isNumber();
    }

    uint32_t maskOfCase(const Terminator& terminator, uint32_t caseIndex)
    {
        if (terminator.kind == TerminatorKind::Switch)
            return maskInside(TypeTable::number, ValueFlowMaskKind::Checked);
        if (terminator.kind != TerminatorKind::SwitchString)
            return 0;
        return maskInside(m_program.types.find(Type::stringLiteral(terminator.strings()[caseIndex])).value_or(TypeTable::string), ValueFlowMaskKind::Checked);
    }

    void addEdgeResult(ValueID parameter, const EdgeResult& result, const Terminator& terminator)
    {
        TypeID type = m_function.at(parameter).type;
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand: {
            bool parameterIsBehindCheck = isBehindCheck(parameter);
            uint32_t mask = parameterIsBehindCheck ? 0 : maskFor(m_function.at(result.subject).type, type);
            if (parameterIsBehindCheck && result.test && result.side == EdgeSide::Taken) {
                const Value& test = m_function.at(result.test);
                if (test.opcode == Opcode::TypeTest)
                    mask = maskInside(test.testedType(), ValueFlowMaskKind::Checked);
                else if (test.opcode == Opcode::StrictEq) {
                    ValueID other = test.operand(result.operandIndex ? 0 : 1);
                    if (!isBehindCheck(other) && isStrictlyEqualOnlyToItself(typeOf(other)))
                        mask = maskInside(m_function.at(other).type, ValueFlowMaskKind::Checked);
                    else if (isNumberInEveryVersion(other))
                        mask = maskInside(TypeTable::number, ValueFlowMaskKind::Checked);
                } else if (test.opcode == Opcode::ToBoolean)
                    mask = maskOfTruthinessTest(result.side);
            }
            if (parameterIsBehindCheck && result.caseIndex)
                mask = maskOfCase(terminator, *result.caseIndex);
            if (result.test && result.side == EdgeSide::NotTaken) {
                const Value& test = m_function.at(result.test);
                if (test.opcode == Opcode::TypeTest)
                    mask = intersectMasks(mask, maskOutside(test.testedType()));
                else if (test.opcode == Opcode::StrictEq) {
                    ValueID other = test.operand(result.operandIndex ? 0 : 1);
                    if (!isBehindCheck(other) && typeOf(other).isSingleValue())
                        mask = intersectMasks(mask, maskOutside(m_function.at(other).type));
                    else if (const Value& compared = m_function.at(other); compared.opcode == Opcode::ModuleBuiltin && isObjectKind(m_program.builtins().at(m_program.builtins().canonicalID(compared.builtin())).kind))
                        mask = intersectMasks(mask, maskOutside(m_program.builtins().canonicalID(compared.builtin()), compared.module()));
                } else if (parameterIsBehindCheck && test.opcode == Opcode::ToBoolean)
                    mask = maskOfTruthinessTest(result.side);
            }
            return addSource(parameter, result.subject, mask);
        }
        case EdgeResultKind::DeclaredResult:
            at(parameter).constant = uniteConstants(at(parameter).constant, constantOfType(m_program.at(type)));
            return;
        case EdgeResultKind::Guard:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void describeValue(BlockID block, ValueID id)
    {
        const Value& value = m_function.at(id);
        if (flowsWithoutRuleOf(value.opcode)) {
            if (mayThrow(value.opcode))
                noteHostError(block);
            return describeOperationWithoutRule(block, id, value);
        }

        describeResult(id, value);
        if (value.opcode == Opcode::NewObject && m_program.at(value.layout()).isEnvironment())
            seedDeadZonesOf(value);
        if (value.opcode == Opcode::SetLiteralPrototype)
            noteLiteralNamingPrototype(definitionThroughSameValues(m_function, value.operand(OperandIndex::SetLiteralPrototype::object)));
        if (!needsRule(id, value)) {
            if (mayThrow(value.opcode))
                noteHostError(block);
            return;
        }
        if (hasRuleOfItsOwn(value.opcode))
            return addStatement(block, id, value);

        switch (flowOf(value.opcode)) {
        case ValueFlowEdgeClass::Allocates:
        case ValueFlowEdgeClass::Copies:
        case ValueFlowEdgeClass::TableProves:
        case ValueFlowEdgeClass::Checks:
            return describeBySchema(block, id, value);
        case ValueFlowEdgeClass::Unsound:
            return describeUnsoundOperation(block, id, value);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void noteLiteralNamingPrototype(ValueID literal)
    {
        for (ValueFlowSite& site : m_body.sites | std::views::reverse) {
            if (site.value != literal)
                continue;
            site.namesPrototype = site.origin == ValueFlowSiteOrigin::Operation && m_function.at(literal).opcode == Opcode::NewObject;
            return;
        }
    }

    bool isWrittenInProgram() const { return !isLibraryCode(m_program, m_function) && m_function.role != FunctionRole::Model; }

    void describeUnsoundOperation(BlockID block, ValueID id, const Value& value)
    {
        size_t numberOfEdges = m_edges.size();
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index)
            connect(value.operand(index), NodeName::escaped());
        if (m_edges.size() != numberOfEdges)
            m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::UnknownCodeWrites), constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }), 0 });
        describeUnknownOutcome(block, id, value);
    }

    void describeBySchema(BlockID block, ValueID id, const Value& value)
    {
        constexpr OperandUses usesOfProtocols { OperandUse::Inspects, OperandUse::Returns, OperandUse::AppliesTransitively };
        constexpr OperandUses usesThatKeep { OperandUse::Inspects, OperandUse::Returns, OperandUse::Retains, OperandUse::ReadsContents, OperandUse::RetainsContents };

        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            const OperandInfo& info = operandInfoOf(value.opcode, index);
            if (!value.operand(index) || (info.uses.containsOnly(inspectsOrReturns) && info.protocols.isEmpty()))
                continue;
            if (info.uses.containsOnly(usesOfProtocols) && !info.protocols.contains(Protocol::Species)) {
                if (ValueFlowBehaviorID behavior = m_tables.behaviorOf(info))
                    connect(value.operand(index), NodeName::hub(behavior));
                continue;
            }
            connect(value.operand(index), NodeName::escaped());
            if (!info.uses.containsOnly(usesThatKeep))
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::UnknownCodeWrites), constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }), 0 });
        }
        describeUnknownOutcome(block, id, value);
    }

    void describeUnknownOutcome(BlockID block, ValueID id, const Value& value)
    {
        if (at(id).role == Role::Result)
            m_resultsOfUnknownValue.append(id);
        if (!mayThrowValue(value.opcode))
            return;
        NodeName target = thrownTargetOf(block);
        if (!m_targetsWithUnknownValue.set(target.owner))
            m_body.seedsOnceReached.append({ target, unknownValue(), 0 });
    }

    uint32_t unknownValue()
    {
        ObjectName unknownObject { ValueFlowObjectKind::UnknownObject, 0, { } };
        return constantOf(Primitive::All, singleElementSpan(unknownObject));
    }

    void describeOperationWithoutRule(BlockID block, ValueID id, const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Absent:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::BigInt:
            return setConstant(id, constantOf(bodyKindsOf(typeOf(id).primitives)));
        case Opcode::String: {
            std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = value.string() });
            return setConstant(id, key ? constantOf({ ValueFlowObjectKind::Key, key->index(), { } }) : constantOf(Primitive::String));
        }
        case Opcode::KeyConstant:
            return setConstant(id, constantOf({ ValueFlowObjectKind::Key, value.key().index(), { } }));
        case Opcode::Parameter:
        case Opcode::LoopHint:
        case Opcode::InitialYield:
        case Opcode::ReadScope:
        case Opcode::ReportMismatch:
        case Opcode::RecordRequire:
            return;
        case Opcode::GetModuleVariable:
            if (carriesValue(id))
                setNode(id, NodeName::variable(value.variable()));
            return;
        case Opcode::PutModuleVariable:
            return connect(value.valueOperand(), NodeName::variable(value.variable()));
        case Opcode::GetEnvironmentField:
            if (carriesValue(id))
                setNode(id, NodeName::field(value.layout(), value.field()));
            return;
        case Opcode::PutEnvironmentField:
            return connect(value.valueOperand(), NodeName::field(value.layout(), value.field()));
        case Opcode::NewClosure: {
            setConstant(id, constantOf({ ValueFlowObjectKind::Closure, value.function().index(), { } }));
            std::span<const ValueID> captures = value.variadicOperands();
            for (uint32_t index = 0; index < captures.size(); ++index)
                connect(captures[index], NodeName::entryParameter(value.function(), index));
            return;
        }
        case Opcode::CallDirect:
            return describeCallDirect(block, id, value);
        case Opcode::Results:
            return;
        case Opcode::ResultAt:
            return describeResultAt(id, value);
        case Opcode::CatchValue:
        case Opcode::CatchException:
            return setNode(id, caughtNodeOf(block));
        case Opcode::Yield:
            connect(value.valueOperand(), NodeName::ofBody(m_body.yielded()));
            return setNode(id, NodeName::ofBody(m_body.sent()));
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void describeCallDirect(BlockID block, ValueID id, const Value& value)
    {
        FunctionID calleeID = value.function();
        const Function& callee = m_program.at(calleeID);
        std::span<const ValueID> arguments = value.variadicOperands();
        RELEASE_ASSERT(arguments.size() == callee.numberOfEntryParameters());
        for (uint32_t index = 0; index < arguments.size(); ++index)
            connect(arguments[index], NodeName::entryParameter(calleeID, index));
        m_body.functionsEnteredOnceReached.append(calleeID);

        if (callee.isUnsplitCoroutine())
            return setConstant(id, constantOf({ ValueFlowObjectKind::CoroutineResult, calleeID.index(), { } }));
        m_body.edges.append({ NodeName::thrown(calleeID), thrownTargetOf(block) });
        if (callee.results.size() == 1 && carriesValue(id))
            setNode(id, NodeName::result(calleeID, 0));
    }

    void describeResultAt(ValueID id, const Value& value)
    {
        if (!carriesValue(id))
            return;
        ValueID producerID = value.resultsOperand();
        const Value& producer = m_function.at(producerID);
        if (producer.opcode == Opcode::Results) {
            beginSources(id);
            return addSource(id, producer.operand(value.index()), 0);
        }
        if (producer.opcode == Opcode::CallDirect && !m_program.at(producer.function()).isUnsplitCoroutine())
            return setNode(id, NodeName::result(producer.function(), value.index()));
        bool isListedName = value.index() == static_cast<uint32_t>(ForInStepResult::Key) && walksNamesThatAreReExported(m_function, producer);
        if (!isListedName && !typeOf(id).hasObjects() && typeOf(id).isJavaScriptValue())
            return setConstant(id, constantOfType(typeOf(id)));
        beginSources(id);
        addSource(id, producerID, maskInside(value.type, kindOfResultMask(producer.opcode)));
    }

    // What it makes of a value that is no object hangs on which values arrive, so a rule adds that.
    void describeConversionToObject(ValueID id, const Value& value)
    {
        const Type& type = typeOf(value.valueOperand());
        bool mayMakeObject = value.opcode == Opcode::ToThis && type.mayBeUndefinedOrNull();
        for (uint32_t ordinal = 0; ordinal < std::size(wrappersOfPrimitives); ++ordinal) {
            if (!(type.primitives & wrappersOfPrimitives[ordinal].first))
                continue;
            addSite({ ValueFlowSiteOrigin::Operation, id, ordinal, { }, wrappersOfPrimitives[ordinal].second });
            mayMakeObject = true;
        }
        if (mayMakeObject) {
            at(id).role = Role::Result;
            return;
        }
        beginSources(id);
        addSource(id, value.valueOperand(), maskInside(TypeTable::object, ValueFlowMaskKind::Checked));
    }

    uint32_t maskInEveryVersion(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::AssumeType:
            return value.isWordOfFrontEnd() ? maskInside(value.provenType(), ValueFlowMaskKind::Checked) : 0;
        case Opcode::CheckInitialized:
            return maskOutside(TypeTable::absent);
        case Opcode::RequireObjectCoercible:
            return maskOutside(TypeTable::nullish);
        case Opcode::RequireObject:
            return maskInside(TypeTable::object, ValueFlowMaskKind::Checked);
        default:
            return 0;
        }
    }

    std::optional<uint32_t> returnedOperandOf(const Value& value) const
    {
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            if (value.operand(index) && operandUsesOf(value.opcode, index).contains(OperandUse::Returns))
                return index;
        }
        return std::nullopt;
    }

    void describeResult(ValueID id, const Value& value)
    {
        if (provesOf(value.opcode) == Proof::Equality && m_program.typeOf(m_function, value.operand(0)).mayBeFunction() && m_program.typeOf(m_function, value.operand(1)).mayBeFunction())
            m_comparedValues.append({ value.operand(0), value.operand(1) });
        if (value.opcode == Opcode::IteratorResume || value.opcode == Opcode::AppendSpread)
            addSite({ ValueFlowSiteOrigin::Operation, id, 0, { }, BuiltinClasses::Array });
        if (value.opcode == Opcode::NewDictionary || value.opcode == Opcode::CopyObject)
            addSite({ ValueFlowSiteOrigin::Operation, id, 0, { }, BuiltinClasses::Object });
        if (value.opcode == Opcode::RequireDynamic || value.opcode == Opcode::ImportDynamic) {
            addSite({ ValueFlowSiteOrigin::Operation, id, 1, { }, BuiltinClasses::Object });
            addSite({ ValueFlowSiteOrigin::Operation, id, 2, { }, BuiltinClasses::Array });
        }

        if (value.opcode == Opcode::PropGet) {
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID named = entryNamedByOperations(m_program, m_function, id);
            if (const Value& holder = m_function.at(definitionThroughSameValues(m_function, value.objectOperand())); !named && holder.opcode == Opcode::ModuleBuiltin)
                named = findOwnMember(m_program, builtins.canonicalID(holder.builtin()), value.key());
            if (named && builtins.letsInUnseenCode(builtins.canonicalID(named)) && isWrittenInProgram())
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), constantOf({ ValueFlowObjectKind::Builtin, builtins.canonicalID(named).index(), { } }), 0 });
            if (named && builtins.at(named).origin == BuiltinOrigin::Host && isCallableBuiltin(builtins, named))
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders), constantOf({ ValueFlowObjectKind::Builtin, builtins.canonicalID(named).index(), { } }), 0 });
        }

        ResultRule rule = resultRuleOf(value.opcode);
        if (rule == ResultRule::Results) {
            at(id).role = Role::Result;
            return;
        }
        if (flowRuleOf(value.opcode) == FlowRule::Call) {
            bool namesLayout = value.opcode == Opcode::SuperConstruct || value.opcode == Opcode::SuperConstructList;
            addSite({ ValueFlowSiteOrigin::BuiltinResult, id, 0, namesLayout ? value.layout() : LayoutID(), { } });
            addSite({ ValueFlowSiteOrigin::BuiltinResult, id, 1, { }, { } });
        }
        if (!carriesValue(id))
            return;

        if (value.opcode == Opcode::Convert && value.illTypedCause() != IllTypedCause::None) {
            at(id).role = Role::Result;
            return;
        }
        if (conversionsToObject.contains(value.opcode))
            return describeConversionToObject(id, value);
        if (value.opcode == Opcode::Check) {
            beginSources(id);
            addSource(id, value.valueOperand(), maskInside(value.expectedType().predicted(), ValueFlowMaskKind::Expected));
            m_checks.append(id);
            return;
        }
        if (std::optional<uint32_t> returned = returnedOperandOf(value)) {
            ValueID operand = value.operand(*returned);
            beginSources(id);
            return addSource(id, operand, isBehindCheck(operand) ? maskInEveryVersion(value) : maskFor(m_function.at(operand).type, value.type));
        }

        std::optional<Type> typeInGenericVersion;
        if (rule == ResultRule::Natural && isBehindCheck(id) && typeOf(id).isNever())
            typeInGenericVersion = tryWidestNaturalTypeOf(value);
        const Type& type = typeInGenericVersion ? *typeInGenericVersion : typeOf(id);
        switch (value.opcode) {
        case Opcode::Builtin: {
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID builtin = builtins.canonicalID(value.builtin());
            if (compiledModuleWithObject(m_program, builtin)) {
                at(id).role = Role::Result;
                return;
            }
            BuiltinName symbol = builtins.nameOfSymbol(builtin);
            if (PropertyKeyID key = symbol ? m_program.keyOf(symbol) : PropertyKeyID())
                return setConstant(id, constantOf({ ValueFlowObjectKind::Key, key.index(), { } }));
            if (!isObjectKind(builtins.at(builtin).kind))
                return setConstant(id, constantOfType(type));
            // An import of the whole of "bun" is followed as what require("bun") answers is.
            if (builtin == Builtins::Bun)
                at(id).role = Role::Result;
            else
                setConstant(id, constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }));
            m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::BuiltinsThatStatementsName), constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }), 0 });
            if (builtins.letsInUnseenCode(builtin) && isWrittenInProgram())
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }), 0 });
            return;
        }
        case Opcode::ModuleBuiltin: {
            if (!isObjectKind(m_program.builtins().at(m_program.builtins().canonicalID(value.builtin())).kind))
                return setConstant(id, constantOfType(type));
            uint32_t object = constantOf({ ValueFlowObjectKind::ModuleBuiltin, m_program.builtins().canonicalID(value.builtin()).index(), value.module() });
            setConstant(id, object);
            if (m_program.builtins().letsInUnseenCode(m_program.builtins().canonicalID(value.builtin())) && isWrittenInProgram())
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), constantOf({ ValueFlowObjectKind::Builtin, m_program.builtins().canonicalID(value.builtin()).index(), { } }), 0 });
            if (m_program.builtins().canonicalID(value.builtin()) == Builtins::ModuleObject)
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::ModuleObjectsHandedOut), object, 0 });
            return;
        }
        case Opcode::Data:
            return describeData(id, value);
        case Opcode::NewObject: {
            ObjectName site = addSite({ ValueFlowSiteOrigin::Operation, id, 0, value.layout(), { } });
            if (mayAllocateOtherLayout(m_program, value))
                at(id).role = Role::Result;
            else
                setConstant(id, constantOf(site));
            return;
        }
        case Opcode::NewRuntimeError:
        case Opcode::NewFunctionThatThrows:
        case Opcode::NewSuppressedError:
            return setConstant(id, constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }));
        case Opcode::InstanceOf:
            at(id).role = Role::Result;
            return;
        default:
            break;
        }

        if (rule == ResultRule::Allocation)
            return setConstant(id, constantOf(addSiteFor(id)));
        if (rule == ResultRule::Natural && isBehindCheck(id) && !type.hasObjects() && type.isJavaScriptValue() && !answersByKindsOfOperands(value.opcode)) {
            uint16_t kinds = bodyKindsOf(type.primitives);
            for (uint16_t group : { Primitive::Boolean, Primitive::Number }) {
                if (kinds & group)
                    kinds |= group;
            }
            return setConstant(id, constantOf(kinds));
        }
        bool mayTurnOutWithoutFraction = type.primitives & Primitive::FractionalNumber && makesNoFraction(value.opcode);
        bool isOfOneKind = (type.hasOnlyPrimitives(Primitive::Number) && !mayTurnOutWithoutFraction) || type.hasOnlyPrimitives(Primitive::AnyString) || type.hasOnlyPrimitives(Primitive::BigInt);
        if (!type.hasObjects() && type.isJavaScriptValue() && !isTypedByChecks(id) && (isOfOneKind || !answersByKindsOfOperands(value.opcode)))
            return setConstant(id, constantOfType(type));
        at(id).role = Role::Result;
    }

    void describeData(ValueID id, const Value& value)
    {
        Vector<const DataNode*, 32> stack { &m_program.at(value.data()).root };
        uint32_t ordinal = 0;
        while (!stack.isEmpty()) {
            const DataNode& node = *stack.takeLast();
            std::optional<ObjectName> site;
            switch (node.kind()) {
            case DataNode::Kind::Array: {
                site = addSite({ ValueFlowSiteOrigin::DataNode, id, ordinal, { }, BuiltinClasses::Array });
                const Vector<DataNode>& elements = node.as<DataNode::Array>().elements;
                for (size_t index = elements.size(); index--;)
                    stack.append(&elements[index]);
                break;
            }
            case DataNode::Kind::Object: {
                site = addSite({ ValueFlowSiteOrigin::DataNode, id, ordinal, node.as<DataNode::Object>().layout, { } });
                const Vector<DataMember>& members = node.as<DataNode::Object>().members;
                for (size_t index = members.size(); index--;)
                    stack.append(&members[index].value);
                break;
            }
            case DataNode::Kind::Undefined:
            case DataNode::Kind::Null:
            case DataNode::Kind::Hole:
            case DataNode::Kind::Boolean:
            case DataNode::Kind::Number:
            case DataNode::Kind::String:
            case DataNode::Kind::BigInt:
                break;
            }
            if (!ordinal)
                setConstant(id, site ? constantOf(*site) : constantOfType(typeOf(id)));
            ++ordinal;
        }
    }

    bool needsRule(ValueID id, const Value& value)
    {
        if (at(id).role == Role::Result)
            return true;
        if (value.opcode == Opcode::Data)
            return !m_body.constants[at(id).constant].objects.isEmpty();
        if (value.opcode == Opcode::NewTemplateObject)
            return true;

        bool hasOperand = false;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            if (!value.operand(index))
                continue;
            hasOperand = true;
            const OperandInfo& info = operandInfoOf(value.opcode, index);
            if (!info.uses.containsOnly(inspectsOrReturns) || !info.protocols.isEmpty())
                return true;
        }
        Effects bound = declaredBoundOf(value.opcode);
        if (bound.mayRunOtherCode())
            return true;
        if (!hasOperand && resultRuleOf(value.opcode) == ResultRule::Allocation)
            return false;
        return !bound.writes.containsOnly(heapsWhoseWritesNobodySees);
    }

    static bool onlyLooksAtOperands(const Value& value)
    {
        if (resultRuleOf(value.opcode) == ResultRule::Allocation)
            return false;
        bool hasOperand = false;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            if (!value.operand(index))
                continue;
            hasOperand = true;
            const OperandInfo& info = operandInfoOf(value.opcode, index);
            if (!info.uses.containsOnly(OperandUse::Inspects) || !info.protocols.isEmpty())
                return false;
        }
        return hasOperand && declaredBoundOf(value.opcode).isPure();
    }

    void seedDeadZonesOf(const Value& allocation)
    {
        const Vector<Field>& fields = m_program.at(allocation.layout()).fields;
        BitVector isListed(fields.size());
        for (uint32_t field : allocation.fields())
            isListed.set(field);
        for (uint32_t field = 0; field < fields.size(); ++field) {
            bool hasDeadZone = m_program.at(fields[field].declared.written()).primitives & Primitive::Absent;
            if (hasDeadZone && !isListed.get(field))
                m_body.seedsOnceReached.append({ NodeName::field(allocation.layout(), field), constantOf(Primitive::Absent), 0 });
        }
    }

    void addStatement(BlockID block, ValueID id, const Value& value)
    {
        if (value.opcode == Opcode::NewObject && m_program.at(value.layout()).isEnvironment()) {
            std::span<const ValueID> values = value.variadicOperands();
            std::span<const uint32_t> fields = value.fields();
            RELEASE_ASSERT(values.size() == fields.size());
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (!listsStartOfVar(m_function, value, index))
                    connect(values[index], NodeName::field(value.layout(), fields[index]));
            }
            return;
        }

        PendingStatement statement { id, block, static_cast<uint32_t>(m_statementOperands.size()), value.numberOfOperands(), !onlyLooksAtOperands(value) };
        for (uint32_t index = 0; index < statement.operandCount; ++index)
            m_statementOperands.append(value.operand(index));
        m_guardsOfBlock[block.index()].areAskedFor = true;
        m_statements.append(statement);
    }

    void describeTerminator(BlockID block, const Terminator& terminator)
    {
        switch (terminator.kind) {
        case TerminatorKind::Return:
            return describeReturn(terminator);
        case TerminatorKind::ReturnCall:
            connect(terminator.callOperand(), NodeName::ofBody(m_body.result(0)));
            m_returnsCall = true;
            return;
        case TerminatorKind::Throw:
            return connect(terminator.valueOperand(), thrownTargetOf(block));
        case TerminatorKind::Rethrow:
            return connect(terminator.exceptionOperand(), thrownTargetOf(block));
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            return noteTestedValue(terminator);
        case TerminatorKind::Jump:
        case TerminatorKind::Unreachable:
            return;
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void noteTestedValue(const Terminator& terminator)
    {
        ValueID tested = terminator.operand(0);
        if (terminator.kind != TerminatorKind::Branch)
            m_observedValues.append({ ValueID(), tested });
        else if (const Value& condition = m_function.at(tested); condition.opcode == Opcode::TypeTest || condition.opcode == Opcode::ToBoolean)
            m_observedValues.append({ tested, condition.operand(0) });
    }

    bool isOwnNewTarget(ValueID id) const
    {
        std::optional<unsigned> index = m_function.newTargetParameterIndex();
        return id && index && definitionThroughSameValues(m_function, id) == m_function.at(entryBlock).parameters[*index];
    }

    bool isObjectUnderConstruction(ValueID id) const
    {
        const Value& value = m_function.at(id);
        if (std::optional<unsigned> index = m_function.thisParameterIndex())
            return id == m_function.at(entryBlock).parameters[*index];
        if (value.opcode == Opcode::SuperConstruct || value.opcode == Opcode::SuperConstructList)
            return isOwnNewTarget(value.operand(1));
        if (value.opcode != Opcode::NewObject || m_function.kind != FunctionKind::BaseConstructor || m_program.at(value.layout()).kind != LayoutKind::Instance)
            return false;
        if (!value.prototypeOperand())
            return true;
        const Value& prototype = m_function.at(definitionThroughSameValues(m_function, value.prototypeOperand()));
        return prototype.opcode == Opcode::GetConstructPrototype && isOwnNewTarget(prototype.operand(0));
    }

    bool answersOnlyWhatItConstructs()
    {
        if (m_function.constructorForm() == ConstructorForm::NotConstructor || m_function.isUnsplitCoroutine() || m_returnsCall)
            return false;
        BitVector isVisited(m_function.values.size());
        Vector<ValueID, 8> worklist = m_returnedValues;
        while (!worklist.isEmpty()) {
            ValueID value = worklist.takeLast();
            if (isVisited.set(value.index()) || at(value).role == Role::Nothing || (!isBehindCheck(value) && !typeOf(value).hasObjects()) || isObjectUnderConstruction(value))
                continue;
            if (at(value).role != Role::Passing)
                return false;
            for (const BodySource& source : sourcesOf(value))
                worklist.append(source.value);
        }
        return true;
    }

    void describeReturn(const Terminator& terminator)
    {
        ValueID returned = terminator.valueOperand();
        if (!returned) {
            m_body.seedsOnceReached.append({ NodeName::ofBody(m_body.result(0)), constantOf(Primitive::Undefined), 0 });
            return;
        }
        m_returnedValues.append(returned);
        const Value& producer = m_function.at(returned);
        if (producer.opcode == Opcode::Results) {
            RELEASE_ASSERT(producer.numberOfOperands() == m_body.numberOfResults);
            for (uint32_t index = 0; index < m_body.numberOfResults; ++index)
                connect(producer.operand(index), NodeName::ofBody(m_body.result(index)));
            return;
        }
        if (m_body.numberOfResults == 1)
            return connect(returned, NodeName::ofBody(m_body.result(0)));
        if (producer.opcode == Opcode::CallDirect && !m_program.at(producer.function()).isUnsplitCoroutine()) {
            for (uint32_t index = 0; index < m_body.numberOfResults; ++index)
                m_body.edges.append({ NodeName::result(producer.function(), index), NodeName::ofBody(m_body.result(index)) });
            return;
        }
        for (uint32_t index = 0; index < m_body.numberOfResults; ++index) {
            connect(returned, NodeName::ofBody(m_body.result(index)));
            m_body.seedsOnceReached.append({ NodeName::ofBody(m_body.result(index)), constantOfType(m_program.at(m_program.provenTypeOf(m_function.results[index]))), 0 });
        }
    }

    void markLiveValues()
    {
        addSourcesOfBlockParameters();
        // A test that is not read lets everything through.
        for (GuardsOfBlock& guards : m_guardsOfBlock) {
            guards.values.removeAllMatching([&](const Guard& guard) {
                return !guard.mask && !sourcesOf(guard.value)[0].mask;
            });
        }
        m_body.answersOnlyWhatItConstructs = answersOnlyWhatItConstructs();

        Vector<ValueID, 64> worklist;
        bool marksForComparisons = false;
        auto markLive = [&](ValueID value) {
            if (!value || at(value).role == Role::Nothing || at(value).isLive)
                return;
            at(value).isLive = true;
            at(value).isLiveForComparisonsOnly = marksForComparisons;
            worklist.append(value);
        };
        auto markSources = [&] {
            while (!worklist.isEmpty()) {
                ValueID value = worklist.takeLast();
                if (at(value).role != Role::Passing)
                    continue;
                for (const BodySource& source : sourcesOf(value))
                    markLive(source.value);
            }
        };
        for (ValueID operand : m_statementOperands)
            markLive(operand);
        for (const EdgeToNode& edge : m_edges)
            markLive(edge.value);
        for (auto [test, value] : m_observedValues)
            markLive(value);
        markSources();
        marksForComparisons = true;
        for (auto [left, right] : m_comparedValues) {
            markLive(left);
            markLive(right);
        }
        forEachGuardAskedFor(markLive);
        markSources();

        for (ValueID check : m_checks) {
            ValueID tested = m_function.at(check).valueOperand();
            if (at(tested).isLive && !at(tested).isLiveForComparisonsOnly)
                m_observedValues.append({ check, tested });
        }
    }

    void forEachGuardAskedFor(NOESCAPE const Invocable<void(ValueID)> auto& functor)
    {
        for (const GuardsOfBlock& guards : m_guardsOfBlock) {
            if (guards.areAskedFor)
                std::ranges::for_each(guards.values, functor, &Guard::value);
        }
    }

    void countReaders()
    {
        for (ValueID operand : m_statementOperands) {
            if (operand)
                ++at(operand).operandReaders;
        }
        for (auto [test, value] : m_observedValues)
            ++at(value).operandReaders;
        for (auto [left, right] : m_comparedValues) {
            ++at(left).operandReaders;
            ++at(right).operandReaders;
        }
        forEachGuardAskedFor([&](ValueID guard) {
            ++at(guard).operandReaders;
        });
        for (const EdgeToNode& edge : m_edges)
            ++at(edge.value).otherReaders;
        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            ValueID value(index);
            if (!at(value).isLive || at(value).role != Role::Passing)
                continue;
            for (const BodySource& source : sourcesOf(value))
                ++at(source.value).otherReaders;
        }
    }

    bool readsValueOfStepThatFindsEnd(BlockID block, ValueID step)
    {
        const Value& operation = m_function.at(step);
        if (operation.readsValueWhenDone() || operation.stepIndex())
            return false;
        auto reads = [&](ValueID id, IteratorResumeResult result) {
            const Value& value = m_function.at(id);
            return value.opcode == Opcode::ResultAt && value.resultsOperand() == step && value.index() == static_cast<uint32_t>(result);
        };
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !reads(terminator.operand(0), IteratorResumeResult::Done))
            return true;
        BlockID behind = terminator.notTaken().block;
        if (behind == terminator.taken().block || m_cfg.incomingEdges(behind).size() != 1)
            return true;
        size_t numberOfReads = 0;
        for (uint32_t index = step.index() + 1; index < m_function.values.size(); ++index)
            numberOfReads += reads(ValueID(index), IteratorResumeResult::Value);
        size_t numberOfReadsBehind = std::ranges::count_if(m_function.at(behind).values, [&](ValueID id) {
            return reads(id, IteratorResumeResult::Value);
        });
        return numberOfReads != numberOfReadsBehind;
    }

    void addResultNodes()
    {
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                Description& description = at(id);
                if (description.role != Role::Result || !description.isLive)
                    continue;
                TypeID type = m_function.at(id).type;
                Opcode opcode = m_function.at(id).opcode;
                bool isInsideType = type && resultRuleOf(opcode) != ResultRule::Results && opcode != Opcode::Builtin;
                ValueFlowMaskKind kind = isTypedByChecks(id) ? ValueFlowMaskKind::Expected : kindOfResultMask(opcode);
                description.name = addNode(ValueFlowBodyNodeKind::OperationResult, id, isInsideType ? maskInside(type, kind) : 0);
                if (opcode == Opcode::IteratorResume && readsValueOfStepThatFindsEnd(block, id))
                    m_body.seedsOnceReached.append({ description.name, constantOf(Primitive::Undefined), 0 });
            }
        }
        for (ValueID id : m_resultsOfUnknownValue) {
            if (at(id).isLive)
                m_body.seedsOnceReached.append({ at(id).name, unknownValue(), 0 });
        }
    }

    static void addForm(Forms& forms, const OperandForm& form)
    {
        if (!forms.contains(form))
            forms.append(form);
    }

    AOT_CLASS_THRESHOLD(size_t, maxFormsComparedInPairs, 32);

    // Adds like addForm() while the list is short. A longer list takes every form, and removeRepeatedForms() leaves what addForm() would have.
    static void addFormToLongList(Forms& forms, const OperandForm& form)
    {
        if (forms.size() < maxFormsComparedInPairs)
            addForm(forms, form);
        else
            forms.append(form);
    }

    static void keepFormsAt(Forms& forms, NOESCAPE const Invocable<bool(uint32_t)> auto& isKept)
    {
        uint32_t numberKept = 0;
        for (uint32_t position = 0; position < forms.size(); ++position) {
            if (isKept(position))
                forms[numberKept++] = forms[position];
        }
        forms.shrink(numberKept);
    }

    static Vector<uint32_t> sortPositionsOfForms(const Forms& forms)
    {
        Vector<uint32_t> positions;
        for (uint32_t position = 0; position < forms.size(); ++position)
            positions.append(position);
        AOT::sort(positions, [&](uint32_t a, uint32_t b) {
            return std::tuple(forms[a].mask, forms[a].node, forms[a].constant, a) < std::tuple(forms[b].mask, forms[b].node, forms[b].constant, b);
        });
        return positions;
    }

    static void removeRepeatedForms(Forms& forms)
    {
        if (forms.size() <= maxFormsComparedInPairs)
            return;
        Vector<uint32_t> positions = sortPositionsOfForms(forms);
        BitVector isRepeated(forms.size());
        for (size_t index = 1; index < positions.size(); ++index) {
            if (forms[positions[index]] == forms[positions[index - 1]])
                isRepeated.quickSet(positions[index]);
        }
        keepFormsAt(forms, [&](uint32_t position) {
            return !isRepeated.quickGet(position);
        });
    }

    // Does what the loop over pairs does, in its order. Among the forms of one mask, the first takes every form without a node. If it has no node
    // itself it takes the node of the first form that has one. Every other form joins the first one with its node.
    void uniteManyFormsWithSameMask(Forms& forms)
    {
        Vector<uint32_t> positions = sortPositionsOfForms(forms);
        Vector<uint32_t> joinedForm;
        joinedForm.grow(forms.size());
        for (size_t begin = 0; begin < positions.size();) {
            uint32_t mask = forms[positions[begin]].mask;
            size_t end = begin;
            uint32_t first = positions[begin];
            std::optional<uint32_t> firstWithNode;
            for (; end < positions.size() && forms[positions[end]].mask == mask; ++end) {
                first = std::min(first, positions[end]);
                if (forms[positions[end]].node)
                    firstWithNode = std::min(firstWithNode.value_or(positions[end]), positions[end]);
            }
            NodeName nodeOfFirst = forms[first].node;
            if (!nodeOfFirst && firstWithNode)
                nodeOfFirst = forms[*firstWithNode].node;
            while (begin < end) {
                NodeName node = forms[positions[begin]].node;
                size_t endOfNode = begin;
                uint32_t firstOfNode = positions[begin];
                for (; endOfNode < end && forms[positions[endOfNode]].node == node; ++endOfNode)
                    firstOfNode = std::min(firstOfNode, positions[endOfNode]);
                for (; begin < endOfNode; ++begin)
                    joinedForm[positions[begin]] = !node || node == nodeOfFirst ? first : firstOfNode;
            }
        }

        Vector<std::pair<uint32_t, uint32_t>> joins;
        for (uint32_t position = 0; position < forms.size(); ++position) {
            if (joinedForm[position] != position)
                joins.append({ joinedForm[position], position });
        }
        AOT::sort(joins);
        for (auto [index, other] : joins) {
            if (!forms[index].node)
                forms[index].node = forms[other].node;
            forms[index].constant = uniteConstants(forms[index].constant, forms[other].constant);
        }
        keepFormsAt(forms, [&](uint32_t position) {
            return joinedForm[position] == position;
        });
    }

    void uniteFormsWithSameMask(Forms& forms)
    {
        if (forms.size() > maxFormsComparedInPairs)
            return uniteManyFormsWithSameMask(forms);
        for (size_t index = 0; index < forms.size(); ++index) {
            for (size_t other = index + 1; other < forms.size();) {
                bool canUnite = forms[index].mask == forms[other].mask && (!forms[index].node || !forms[other].node || forms[index].node == forms[other].node);
                if (!canUnite) {
                    ++other;
                    continue;
                }
                if (!forms[index].node)
                    forms[index].node = forms[other].node;
                forms[index].constant = uniteConstants(forms[index].constant, forms[other].constant);
                forms.removeAt(other);
            }
        }
    }

    void connectForm(const OperandForm& form, NodeName target, uint32_t maskOfTarget)
    {
        bool leavesBody = target.kind != NodeName::Kind::OfBody;
        if (form.constant)
            m_body.seedsOnceReached.append({ target, form.constant, form.mask == maskOfTarget ? 0 : form.mask });
        if (!form.node)
            return;
        NodeName from = form.mask && form.mask != maskOfTarget ? filterNodeOf({ form.node, 0, form.mask }) : form.node;
        (leavesBody ? m_body.edgesOnceReached : m_body.edges).append({ from, target });
    }

    NodeName filterNodeOf(const OperandForm& form)
    {
        for (auto& [known, node] : m_filterNodes) {
            if (known == form)
                return node;
        }
        NodeName node = addNode(ValueFlowBodyNodeKind::Filter, ValueID(), form.mask);
        m_body.edges.append({ form.node, node });
        m_filterNodes.append({ form, node });
        return node;
    }

    OperandForm throughMask(const OperandForm& form, uint32_t mask)
    {
        if (areOfOneKind(form.mask, mask))
            return { form.node, form.constant, intersectMasks(form.mask, mask) };
        return { form.node, form.constant, m_body.masks[mask].kind == ValueFlowMaskKind::Checked ? mask : form.mask };
    }

    OperandForm makeNode(ValueID value, const Forms& forms)
    {
        bool isFilter = forms.size() == 1;
        uint32_t mask = isFilter ? forms[0].mask : 0;
        NodeName node = addNode(isFilter ? ValueFlowBodyNodeKind::Filter : ValueFlowBodyNodeKind::Merge, value, mask);
        for (const OperandForm& form : forms)
            connectForm(form, node, mask);
        if (isFilter)
            m_sharedFilters.append({ node, forms[0] });
        return { node, 0, 0 };
    }

    OperandForm throughSharedFilters(OperandForm form)
    {
        while (form.node && !form.constant) {
            size_t index = m_sharedFilters.findIf([&](const auto& filter) {
                return filter.first == form.node;
            });
            if (index == notFound)
                break;
            form = throughMask(m_sharedFilters[index].second, form.mask);
        }
        return form;
    }

    const Forms& formsOf(ValueID value) LIFETIME_BOUND
    {
        ValueID head = at(value).head ? at(value).head : value;
        return m_forms[head.index()];
    }

    void resolveValues()
    {
        m_forms.grow(m_descriptions.size());
        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            const Description& description = m_descriptions[index];
            if (!description.isLive)
                continue;
            switch (description.role) {
            case Role::Constant:
                m_forms[index].append({ { }, description.constant, 0 });
                break;
            case Role::Node:
            case Role::Result:
                m_forms[index].append({ description.name, 0, description.mask });
                break;
            case Role::Passing:
            case Role::Nothing:
                break;
            }
        }
        resolveCyclesOfPassingValues();
    }

    void resolveCyclesOfPassingValues()
    {
        constexpr uint32_t unvisited = UINT32_MAX;
        Vector<uint32_t> order;
        order.fill(unvisited, m_descriptions.size());
        Vector<uint32_t> lowest;
        lowest.fill(0, m_descriptions.size());
        BitVector isOnPath(m_descriptions.size());
        Vector<ValueID, 64> path;
        struct Frame {
            ValueID value;
            uint32_t nextSource;
        };
        Vector<Frame, 64> frames;
        uint32_t next = 0;

        auto isPassing = [&](ValueID value) {
            return at(value).isLive && at(value).role == Role::Passing;
        };
        auto enter = [&](ValueID value) {
            order[value.index()] = lowest[value.index()] = next++;
            path.append(value);
            isOnPath.quickSet(value.index());
            frames.append({ value, 0 });
        };

        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            if (!isPassing(ValueID(index)) || order[index] != unvisited)
                continue;
            enter(ValueID(index));
            while (!frames.isEmpty()) {
                Frame& frame = frames.last();
                ValueID value = frame.value;
                std::span<const BodySource> sources = sourcesOf(value);
                if (frame.nextSource < sources.size()) {
                    ValueID source = sources[frame.nextSource++].value;
                    if (!isPassing(source))
                        continue;
                    if (order[source.index()] == unvisited)
                        enter(source);
                    else if (isOnPath.quickGet(source.index()))
                        lowest[value.index()] = std::min(lowest[value.index()], order[source.index()]);
                    continue;
                }
                frames.removeLast();
                if (!frames.isEmpty())
                    lowest[frames.last().value.index()] = std::min(lowest[frames.last().value.index()], lowest[value.index()]);
                if (lowest[value.index()] != order[value.index()])
                    continue;
                size_t first = path.reverseFind(value);
                for (ValueID member : path.subspan(first))
                    isOnPath.quickClear(member.index());
                resolveCycle(path.subspan(first));
                path.shrink(first);
            }
        }
    }

    void resolveCycle(std::span<const ValueID> members)
    {
        auto isMember = [&](ValueID value) {
            return members.size() > 1 ? std::ranges::contains(members, value) : value == members[0];
        };
        bool hasMaskInside = false;
        for (ValueID member : members) {
            for (const BodySource& source : sourcesOf(member))
                hasMaskInside |= source.mask && isMember(source.value);
        }
        if (hasMaskInside)
            return resolveCycleThroughMasks(members);

        ValueID head = members[0];
        Forms forms;
        uint32_t operandReaders = 0;
        uint32_t otherReaders = 0;
        for (ValueID member : members) {
            at(member).head = head;
            operandReaders += at(member).operandReaders;
            otherReaders += at(member).otherReaders;
            if (at(member).constant)
                addFormToLongList(forms, { { }, at(member).constant, 0 });
            for (const BodySource& source : sourcesOf(member)) {
                if (isMember(source.value)) {
                    --otherReaders;
                    continue;
                }
                for (const OperandForm& form : formsOf(source.value))
                    addFormToLongList(forms, throughMask(form, source.mask));
            }
        }
        removeRepeatedForms(forms);
        uniteFormsWithSameMask(forms);

        uint32_t readers = operandReaders + otherReaders;
        bool staysAsNode = false;
        if (forms.size() == 1)
            staysAsNode = forms[0].mask && forms[0].node && readers > 1;
        else if (forms.size() > 1)
            staysAsNode = operandReaders || forms.size() * readers > forms.size() + readers;
        if (staysAsNode)
            forms = { makeNode(head, forms) };
        m_forms[head.index()] = WTF::move(forms);
    }

    void resolveCycleThroughMasks(std::span<const ValueID> members)
    {
        for (ValueID member : members)
            m_forms[member.index()] = { OperandForm { addNode(ValueFlowBodyNodeKind::Merge, member), 0, 0 } };
        for (ValueID member : members) {
            NodeName node = m_forms[member.index()][0].node;
            if (at(member).constant)
                connectForm({ { }, at(member).constant, 0 }, node, 0);
            for (const BodySource& source : sourcesOf(member)) {
                for (const OperandForm& form : formsOf(source.value))
                    connectForm(throughMask(form, source.mask), node, 0);
            }
        }
    }

    std::optional<uint32_t> operandOfCallerAt(NodeName node) const
    {
        if (node.kind != NodeName::Kind::OfBody || node.owner >= m_body.numberOfEntryParameters)
            return std::nullopt;
        EntryParameter kind = m_function.entryParameterKind(node.owner);
        if (kind == EntryParameter::This)
            return node.owner;
        if (kind == EntryParameter::Parameter && node.owner - m_function.firstParameterIndex() < m_function.numberOfPositionalParameters())
            return node.owner;
        return std::nullopt;
    }

    void emitEdges()
    {
        NodeName result = NodeName::ofBody(m_body.result(0));
        bool mayHaveSummary = m_body.numberOfResults == 1 && !m_function.isUnsplitCoroutine();
        bool returnsParameter = false;
        for (const EdgeToNode& edge : m_edges) {
            if (!mayHaveSummary || edge.target != result)
                continue;
            for (const OperandForm& form : formsOf(edge.value))
                returnsParameter |= !!operandOfCallerAt(throughSharedFilters(form).node);
        }
        NodeName ownResult;
        if (returnsParameter) {
            ownResult = addNode(ValueFlowBodyNodeKind::Merge, ValueID());
            m_body.ownResult = ownResult.owner;
            for (size_t index = 0, end = m_body.seedsOnceReached.size(); index < end; ++index) {
                if (m_body.seedsOnceReached[index].node == result) {
                    auto seed = m_body.seedsOnceReached[index];
                    seed.node = ownResult;
                    m_body.seedsOnceReached.append(seed);
                }
            }
        }
        for (const EdgeToNode& edge : m_edges) {
            for (const OperandForm& form : formsOf(edge.value)) {
                connectForm(form, edge.target, 0);
                if (!returnsParameter || edge.target != result)
                    continue;
                OperandForm source = throughSharedFilters(form);
                std::optional<uint32_t> parameter = operandOfCallerAt(source.node);
                if (!parameter) {
                    connectForm(form, ownResult, 0);
                    continue;
                }
                if (source.constant)
                    connectForm({ { }, source.constant, source.mask }, ownResult, 0);
                ValueFlowBody::PassedParameter passed { *parameter, source.mask };
                if (!m_body.passedParameters.containsIf([&](const auto& known) { return known.entryParameter == passed.entryParameter && known.mask == passed.mask; }))
                    m_body.passedParameters.append(passed);
            }
        }
    }

    OperandForm operandFormOf(ValueID value)
    {
        if (!value || !at(value).isLive)
            return { };
        const Forms& forms = formsOf(value);
        RELEASE_ASSERT(forms.size() <= 1);
        return forms.isEmpty() ? OperandForm { } : forms[0];
    }

    OperandForm operandFormOf(const Value& operation, uint32_t index, ValueID value)
    {
        OperandForm form = operandFormOf(value);
        bool isLookedThrough = receiverOf(operation.opcode) == index || (!index && flowRuleOf(operation.opcode) == FlowRule::Call);
        if (!isLookedThrough || !form.node || form.mask)
            return form;
        const Type& type = typeOf(value);
        if (!type.anyObject && !type.anyFunction && type.classes.isEmpty() && (type.layouts || !type.functions.isEmpty()))
            form.mask = maskInside(m_function.at(value).type, isBehindCheck(value) ? ValueFlowMaskKind::Expected : ValueFlowMaskKind::Declared);
        return form;
    }

    const UnobservedFieldDefinition* findUnobservedDefinition(ValueID operation, uint32_t operandIndex) const
    {
        Opcode opcode = m_function.at(operation).opcode;
        bool isDefinedValue = (opcode == Opcode::PropDefine && operandIndex == OperandIndex::PropDefine::value) || (opcode == Opcode::PrivateDefine && operandIndex == OperandIndex::PrivateDefine::value);
        return m_unobservedDefinitions && isDefinedValue ? m_unobservedDefinitions->find(m_id, operation) : nullptr;
    }

    OperandForm valueOf(const UnobservedFieldDefinition& definition)
    {
        uint32_t& value = m_body.valueOfUnobservedDefinitions[definition.isObservedWhereObjectRefusesDefinition];
        if (value == Body::none)
            value = addNode(ValueFlowBodyNodeKind::Merge, ValueID()).owner;
        return { NodeName::ofBody(value), 0, 0 };
    }

    // The reads by name whose value goes anywhere but into tests and into the callee of a call on the object it was read from.
    void findReadsHandedOn()
    {
        m_readsHandedOn = BitVector(m_function.values.size());
        Vector<ValueID> narrowedFrom(m_function.values.size());
        auto definitionOf = [&](ValueID id) {
            for (unsigned steps = 0; steps < 8; ++steps) {
                id = definitionThroughSameValues(m_function, id);
                if (m_function.at(id).opcode != Opcode::Parameter || !narrowedFrom[id.index()])
                    break;
                id = narrowedFrom[id.index()];
            }
            return id;
        };
        auto handOn = [&](ValueID used) {
            if (used)
                m_readsHandedOn.quickSet(definitionOf(used).index());
        };
        Vector<ValueID, 8> narrowedTwice;
        for (uint32_t from = 0; from < m_function.blocks.size(); ++from) {
            const auto& targets = m_function.blocks[from].terminator.targets;
            for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
                uint32_t first = firstEdgeResultParameter(m_function, BlockID(from), targetIndex);
                for (uint32_t result = 0; result < numberOfEdgeResults(m_function, BlockID(from), targetIndex); ++result) {
                    ValueID narrowed = operandNarrowedOnEdge(m_function, { BlockID(from), targetIndex, result });
                    ValueID& origin = narrowedFrom[m_function.at(targets[targetIndex].block).parameters[first + result].index()];
                    if (origin && origin != narrowed)
                        narrowedTwice.appendList({ origin, narrowed });
                    origin = narrowed;
                }
            }
        }
        for (ValueID narrowed : narrowedTwice)
            handOn(narrowed);
        for (const Block& block : m_function.blocks) {
            for (ValueID id : block.values) {
                const Value& user = m_function.at(id);
                for (uint32_t index = 0; index < user.numberOfOperands(); ++index) {
                    ValueID operand = user.operand(index);
                    if (!operand || sameValueOperandOf(user) == operand)
                        continue;
                    const OperandInfo& info = operandInfoOf(user.opcode, index);
                    if (info.uses.contains(OperandUse::Calls) && info.uses.containsOnly({ OperandUse::Inspects, OperandUse::Calls }) && (user.opcode == Opcode::Call || user.opcode == Opcode::CallList) && user.thisValueOperand()) {
                        const Value& read = m_function.at(definitionOf(operand));
                        if (read.opcode != Opcode::PropGet || definitionOf(read.objectOperand()) == definitionOf(user.thisValueOperand()))
                            continue;
                    }
                    if (info.uses.containsOnly(OperandUse::Inspects) && info.protocols.isEmpty() && receiverOf(user.opcode) != index)
                        continue;
                    handOn(operand);
                }
            }
            const Terminator& terminator = block.terminator;
            std::span<const ValueID> operandsOfTerminator = terminator.operands();
            for (uint32_t index = 0; index < operandsOfTerminator.size(); ++index) {
                if (!operandInfoOf(terminator.kind, index).uses.containsOnly(OperandUse::Inspects))
                    handOn(operandsOfTerminator[index]);
            }
            for (const Target& target : terminator.targets) {
                for (ValueID argument : target.arguments)
                    handOn(argument);
            }
            if (block.handler) {
                for (ValueID argument : block.handler->arguments)
                    handOn(argument);
            }
            handOn(block.subject);
        }
    }

    // The pass that replaces such a call reads the type of its answer, wherever the call stands.
    bool callsHelperOfCompiler(const Value& value) const
    {
        if (flowRuleOf(value.opcode) != FlowRule::Call || !value.operand(0))
            return false;
        const Value& callee = m_function.at(definitionThroughSameValues(m_function, value.operand(0)));
        if (callee.opcode != Opcode::Builtin)
            return false;
        const Builtin& entry = m_program.builtins().at(callee.builtin());
        return entry.flags.contains(BuiltinFlag::IsCompilerHelper) || entry.holder == Builtins::BunBundle;
    }

    void emitStatement(const PendingStatement& pending)
    {
        const Value& value = m_function.at(pending.site);
        Body::Statement statement { value.opcode, pending.site, static_cast<uint32_t>(m_body.operands.size()), pending.operandCount, { }, { } };
        if (value.opcode == Opcode::PropGet) {
            if (!std::exchange(m_hasFoundReadsHandedOn, true))
                findReadsHandedOn();
            statement.isOnlyCalledOnItsHolder = !m_readsHandedOn.quickGet(pending.site.index());
        }
        for (uint32_t index = 0; index < pending.operandCount; ++index) {
            const UnobservedFieldDefinition* unobserved = findUnobservedDefinition(pending.site, index);
            m_body.operands.append(unobserved ? valueOf(*unobserved) : operandFormOf(value, index, m_statementOperands[pending.firstOperand + index]));
        }
        GuardsOfBlock& guards = m_guardsOfBlock[pending.block.index()];
        if (!guards.areWritten) {
            guards.areWritten = true;
            guards.first = m_body.operands.size();
            for (const Guard& guard : guards.values) {
                // Of a value that has no node flow says nothing.
                if (OperandForm form = operandFormOf(guard.value); form.node) {
                    m_body.operands.append(guard.mask ? throughMask(form, guard.mask) : form);
                    ++guards.count;
                }
            }
        }
        if (!callsHelperOfCompiler(value)) {
            statement.firstGuard = guards.first;
            statement.guardCount = guards.count;
        }
        if (at(pending.site).role == Role::Result && at(pending.site).isLive)
            statement.result = at(pending.site).name;
        if (mayThrowValue(value.opcode))
            statement.thrown = thrownTargetOf(pending.block);
        m_body.statements.append(statement);
    }

    void emitStatements()
    {
        m_body.statements.reserveInitialCapacity(m_statements.size() + 3);
        m_body.operands.reserveInitialCapacity(m_statementOperands.size());
        for (const PendingStatement& pending : m_statements) {
            if (!pending.leavesBody)
                emitStatement(pending);
        }
        m_body.firstStatementLeavingBody = m_body.statements.size();
        for (const PendingStatement& pending : m_statements) {
            if (pending.leavesBody)
                emitStatement(pending);
        }
        if (m_function.module && m_program.at(m_function.module).body == m_id)
            m_body.statements.append({ { }, { }, static_cast<uint32_t>(m_body.operands.size()), 0, { }, { }, ValueFlowStatementKind::ModuleEvaluation });
        if (m_function.isUnsplitCoroutine())
            m_body.statements.append({ { }, { }, static_cast<uint32_t>(m_body.operands.size()), 0, { }, { }, ValueFlowStatementKind::Coroutine });

        for (auto [test, value] : m_observedValues)
            m_body.observedValues.append({ test, operandFormOf(value) });
        for (auto [left, right] : m_comparedValues)
            m_body.comparedValues.append({ operandFormOf(left), operandFormOf(right) });
        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            if (m_descriptions[index].isLiveForComparisonsOnly)
                m_body.valuesAliveForComparisonsOnly.append(ValueID(index));
        }
        m_body.nodesOfValuesAliveForComparisonsOnly.grow(m_body.valuesAliveForComparisonsOnly.size());
        for (uint32_t index = 0; index < m_body.nodes.size() && !m_body.valuesAliveForComparisonsOnly.isEmpty(); ++index) {
            ValueID value = m_body.nodes[index].value;
            if (!value || !at(value).isLiveForComparisonsOnly)
                continue;
            size_t position = std::ranges::lower_bound(m_body.valuesAliveForComparisonsOnly, value) - m_body.valuesAliveForComparisonsOnly.begin();
            m_body.nodesOfValuesAliveForComparisonsOnly[position] = NodeName::ofBody(index);
        }
    }

    struct BlockParameter {
        BlockID block;
        uint32_t index;
        ValueID parameter;
    };

    struct ObservedValue {
        ValueID test;
        ValueID value;
    };

    const Program& m_program;
    const ValueFlowBuiltinTables& m_tables;
    const UnobservedFieldDefinitions* m_unobservedDefinitions;
    BitVector m_readsHandedOn;
    bool m_hasFoundReadsHandedOn { false };
    FunctionID m_id;
    const Function& m_function;
    CFG m_cfg;
    Body m_body;
    InternedBodyRecords<Body::Constant> m_constants;
    InternedBodyRecords<Body::Mask> m_masks;
    Vector<Description, 48> m_descriptions;
    Vector<BodySource, 48> m_sources;
    AOT_CLASS_THRESHOLD(size_t, minSourcesFoundByHash, 32);
    ValueID m_valueWithHashedSources;
    UncheckedKeyHashSet<uint64_t> m_hashedSources;
    Vector<BlockParameter, 16> m_blockParameters;
    Vector<GuardsOfBlock> m_guardsOfBlock;
    Vector<ValueID, 8> m_returnedValues;
    bool m_returnsCall { false };
    Vector<NodeName, 16> m_caughtNodes;
    BitVector m_targetsWithHostError;
    BitVector m_targetsWithUnknownValue;
    Vector<ValueID, 8> m_resultsOfUnknownValue;
    Vector<EdgeToNode, 32> m_edges;
    Vector<PendingStatement, 32> m_statements;
    Vector<ValueID, 96> m_statementOperands;
    Vector<ObservedValue, 16> m_observedValues;
    Vector<std::pair<ValueID, ValueID>, 4> m_comparedValues;
    Vector<ValueID, 8> m_checks;
    BitVector m_isBehindCheck;
    Vector<Forms, 48> m_forms;
    Vector<std::pair<OperandForm, NodeName>, 4> m_filterNodes;
    Vector<std::pair<NodeName, OperandForm>, 4> m_sharedFilters;
    std::array<std::optional<std::optional<TypeID>>, 2> m_typeOfBoolean;
};

} // namespace

static ValueFlowBody makeBodyWithoutOperations(FunctionID id, uint32_t numberOfEntryParameters, uint32_t numberOfResults, bool hasYieldedAndSent)
{
    ValueFlowBody body;
    body.function = id;
    body.numberOfEntryParameters = numberOfEntryParameters;
    body.numberOfResults = numberOfResults;
    body.hasYieldedAndSent = hasYieldedAndSent;
    body.constants.append({ });
    body.masks.append({ });
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index)
        body.nodes.append({ ValueFlowBodyNodeKind::EntryParameter, { }, 0 });
    for (uint32_t index = 0; index < numberOfResults; ++index)
        body.nodes.append({ ValueFlowBodyNodeKind::Result, { }, 0 });
    body.nodes.append({ ValueFlowBodyNodeKind::Thrown, { }, 0 });
    if (hasYieldedAndSent) {
        body.nodes.append({ ValueFlowBodyNodeKind::Yielded, { }, 0 });
        body.nodes.append({ ValueFlowBodyNodeKind::Sent, { }, 0 });
    }
    return body;
}

bool isUsedOnlyAsInstanceOfOwnFunction(const Program& program, FunctionID functionID, ValueID thisValue)
{
    const Function& function = program.at(functionID);
    auto isThis = [&](ValueID id) {
        return id && definitionThroughSameValues(function, id) == thisValue;
    };
    BlockID testing;
    ValueID test;
    for (uint32_t index = 0; index < function.blocks.size() && !test; ++index) {
        for (ValueID id : function.blocks[index].values) {
            const Value& candidate = function.at(id);
            if (candidate.opcode != Opcode::InstanceOf || !isThis(candidate.operand(0)))
                continue;
            const Type& constructor = program.typeOf(function, candidate.operand(1));
            if (constructor.primitives || constructor.anyObject || constructor.anyFunction || constructor.functions.size() != 1 || constructor.functions[0] != functionID)
                return false;
            testing = BlockID(index);
            test = id;
            break;
        }
    }
    if (!test)
        return false;
    const Terminator& terminator = function.at(testing).terminator;
    if (terminator.kind != TerminatorKind::Branch)
        return false;
    ValueID condition = terminator.operand(0);
    if (function.at(condition).opcode == Opcode::ToBoolean)
        condition = function.at(condition).operand(0);
    BlockID passed = terminator.taken().block;
    if (condition != test || passed == terminator.notTaken().block || passed == entryBlock)
        return false;

    CFG cfg(function);
    BitVector isReachedOtherwise(function.blocks.size());
    Vector<BlockID, 16> worklist { entryBlock };
    isReachedOtherwise.quickSet(entryBlock.index());
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        for (BlockID successor : cfg.successors(block)) {
            if (successor == passed) {
                if (block != testing)
                    return false;
                continue;
            }
            if (!isReachedOtherwise.quickSet(successor.index()))
                worklist.append(successor);
        }
    }
    for (size_t index : isReachedOtherwise) {
        const Block& block = function.blocks[index];
        for (ValueID id : block.values) {
            const Value& user = function.at(id);
            if (id == test || sameValueOperandOf(user))
                continue;
            for (uint32_t operand = 0; operand < user.numberOfOperands(); ++operand) {
                if (isThis(user.operand(operand)))
                    return false;
            }
        }
        for (uint32_t operand = 0; operand < block.terminator.numberOfOperands(); ++operand) {
            if (isThis(block.terminator.operand(operand)))
                return false;
        }
        bool handsOnThis = false;
        block.forEachTarget([&](const Target& target) {
            handsOnThis |= std::ranges::any_of(target.arguments, isThis);
        });
        if (handsOnThis)
            return false;
    }
    return true;
}

ValueFlowBody reduceBody(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id, const UnobservedFieldDefinitions* unobservedDefinitions)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return makeBodyWithoutOperations(id, function.numberOfEntryParameters(), function.results.size(), function.isUnsplitCoroutine() && function.has(FunctionFlags::IsGenerator));
    return BodyReducer(program, tables, id, unobservedDefinitions).reduce();
}

bool startsWithoutValueInSolve(const Program& program, const Variable& variable)
{
    return variable.initialization == VariableInitialization::Undefined && variable.flags.contains(VariableFlags::IsImmutable) && variable.module && program.at(variable.module).kind == ModuleKind::RuntimeSource;
}

ValueFlowBody makeHostBody(const Program& program, const ValueFlowBuiltinTables& tables, std::span<const uint64_t> operationsOfFunctions)
{
    ValueFlowBody body = makeBodyWithoutOperations({ }, 0, 0, false);

    uint32_t undefinedValue = body.constants.size();
    body.constants.append({ Primitive::Undefined, { } });
    uint32_t absentValue = body.constants.size();
    body.constants.append({ Primitive::Absent, { } });
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        ValueFlowNodeName node = ValueFlowNodeName::variable(VariableID(index));
        switch (variable.initialization) {
        case VariableInitialization::Uninitialized:
            body.seedsOnceReached.append({ node, absentValue, 0 });
            break;
        case VariableInitialization::Undefined:
            if (!startsWithoutValueInSolve(program, variable))
                body.seedsOnceReached.append({ node, undefinedValue, 0 });
            break;
        case VariableInitialization::Function:
            if (variable.function) {
                body.seedsOnceReached.append({ node, static_cast<uint32_t>(body.constants.size()), 0 });
                body.constants.append({ 0, { ValueFlowObjectName { ValueFlowObjectKind::Closure, variable.function.index(), { } } } });
            }
            break;
        }
    }

    for (uint32_t functionIndex = 0; functionIndex < program.functions.size(); ++functionIndex) {
        if (!operationsOfFunctions.empty() && !(operationsOfFunctions[functionIndex] & notedOperations({ ValueFlowNotedOperation::NewObject })))
            continue;
        const Function& function = program.functions[functionIndex];
        for (const Value& value : function.values) {
            if (value.opcode != Opcode::NewObject || !program.at(value.layout()).isEnvironment())
                continue;
            std::span<const uint32_t> fields = value.fields();
            for (uint32_t index = 0; index < fields.size(); ++index) {
                if (listsStartOfVar(function, value, index))
                    body.seedsOnceReached.append({ ValueFlowNodeName::field(value.layout(), fields[index]), undefinedValue, 0 });
            }
        }
    }

    describeHost(program, body);

    RELEASE_ASSERT(body.nodes.size() == body.thrown() + 1 + numberOfValueFlowHostNodes);
    for (uint32_t index = 0; index < tables.numberOfBehaviors(); ++index) {
        body.nodes.append({ ValueFlowBodyNodeKind::Merge, { }, 0 });
        body.statements.append({ { }, { }, 0, 0, ValueFlowNodeName::hub(ValueFlowBehaviorID(index)), { }, ValueFlowStatementKind::Behavior });
    }
    return body;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
