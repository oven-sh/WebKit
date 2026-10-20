/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"

#if ENABLE(AOT)
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesModules.h"
#endif

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTEffectsDeclared.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Body = ValueFlowBody;
using NodeName = ValueFlowNodeName;
using ObjectName = ValueFlowObjectName;
using Form = Body::Operand;
using Forms = Vector<Form, 1>;

enum class Role : uint8_t {
    // It carries no value of the program, or no block that is reached holds it.
    Nothing,
    Constant,
    // It is a node with a name of its own: an entry parameter, a node of the program, what a handler catches.
    Node,
    // The rule of its operation fills it. It is a node if somebody reads it.
    Result,
    // It holds what its sources hold, each through a mask, and a constant beside them.
    Passing,
};

struct Source {
    ValueID value;
    uint32_t mask { 0 };

    friend bool operator==(const Source&, const Source&) = default;
};

struct Description {
    Role role { Role::Nothing };
    bool isLive { false };
    NodeName name;
    uint32_t constant { 0 };
    uint32_t firstSource { 0 };
    uint32_t sourceCount { 0 };
    uint32_t operandReaders { 0 };
    uint32_t otherReaders { 0 };
    // Members of one cycle of plain copies have the same head and hold the same.
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

constexpr OperandUses inspectsOrReturns { OperandUse::Inspects, OperandUse::Returns };

// The body says all there is to say about them: with edges, constants and masks.
constexpr OpcodeSet operationsWithoutRule {
    Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt, Opcode::KeyConstant,
    Opcode::Parameter, Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::NewClosure,
    Opcode::CallDirect, Opcode::Results, Opcode::ResultAt, Opcode::CatchValue, Opcode::CatchException, Opcode::Yield, Opcode::ToObject, Opcode::ToThis,
    Opcode::LoopHint, Opcode::InitialYield, Opcode::ReportMismatch,
};

// The callee may be a built-in that allocates.
constexpr OpcodeSet operationsCallingValues { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };

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
    case OpaqueKind::FlatString:
    case OpaqueKind::CodeAddress:
    case OpaqueKind::BuilderCursor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A literal is a string or a symbol like any other unless it is a key, which is a member of its own.
uint16_t kindsOf(uint16_t primitives)
{
    uint16_t kinds = primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol);
    if (primitives & Primitive::LiteralString)
        kinds |= Primitive::String;
    if (primitives & Primitive::LiteralSymbol)
        kinds |= Primitive::Symbol;
    return kinds;
}

bool isCoroutine(const Function& function)
{
    return function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
}

template<typename Record>
class InternedRecords {
public:
    explicit InternedRecords(Vector<Record>& records)
        : m_records(records)
    {
        m_records.append(Record { });
    }

    uint32_t intern(Record&& record, unsigned hash)
    {
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
    HashMap<unsigned, Vector<uint32_t, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_byHash;
};

class BodyReducer {
    WTF_MAKE_NONCOPYABLE(BodyReducer);
public:
    BodyReducer(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id)
        : m_program(program)
        , m_tables(tables)
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
        for (BlockID block : m_cfg.reversePostOrder())
            describeBlock(block);
        markLiveValues();
        countReaders();
        addResultNodes();
        resolveValues();
        emitEdges();
        emitStatements();
        noteClassesOfInstances();
        noteUsesOfModules();
        sortSites();
        sortAndRemoveRepeats(m_body.edges);
        sortAndRemoveRepeats(m_body.edgesOnceReached);
        sortAndRemoveRepeats(m_body.seedsOnceReached);
        sortAndRemoveRepeats(m_body.functionsEnteredOnceReached);
        return WTF::move(m_body);
    }

private:
    template<typename T>
    static void sortAndRemoveRepeats(Vector<T>& list)
    {
        std::ranges::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    }

    // Native code does with an instance what the table hangs on the constructor and on the prototype object of its class, so both count as used where the function that makes the instance is reached.
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
        if (m_function.has(FunctionFlags::IsGenerator))
            note(m_function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator);
        else if (m_function.has(FunctionFlags::IsAsync))
            note(BuiltinClasses::Promise);
    }

    // A function of a module that waits for a use needs what the body of the module stores, and so does whoever names a variable of it.
    void noteUsesOfModules()
    {
        auto use = [&](ModuleID module) {
            if (module && awaitsUse(m_program, module) && m_program.at(module).body != m_id)
                m_body.functionsEnteredOnceReached.append(m_program.at(module).body);
        };
        use(m_function.module);
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::PutModuleVariable)
                    use(m_program.at(value.variable()).module);
            }
        }
    }

    // The universe finds a site by its operation.
    void sortSites()
    {
        Vector<uint32_t> order(m_body.sites.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(order, { }, [&](uint32_t index) {
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
            std::ranges::sort(constant.objects);
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

    // What is known of a value by its type alone.
    uint32_t constantOfType(const Type& type)
    {
        uint16_t primitives = kindsOf(type.primitives);
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
        std::ranges::sort(objects);
        objects.shrink(std::ranges::unique(objects).begin() - objects.begin());
        return constantOf(m_body.constants[first].primitives | m_body.constants[second].primitives, objects.span());
    }

    uint32_t internMask(Body::Mask&& mask)
    {
        if (mask.admitsEverything())
            return 0;
        Hasher hasher;
        for (TypeID type : mask.inside)
            add(hasher, type);
        add(hasher, 0x5eed);
        for (TypeID type : mask.outside)
            add(hasher, type);
        return m_masks.intern(WTF::move(mask), hasher.hash());
    }

    uint32_t maskInside(TypeID type)
    {
        const Type& record = m_program.at(type);
        if (record.isAnything() || !record.isJavaScriptValue())
            return 0;
        return internMask({ { type }, { } });
    }

    uint32_t maskOutside(TypeID type) { return internMask({ { }, { type } }); }

    uint32_t intersectMasks(uint32_t first, uint32_t second)
    {
        if (!first || first == second)
            return second;
        if (!second)
            return first;
        auto unite = [](const Vector<TypeID, 1>& a, const Vector<TypeID, 1>& b) {
            Vector<TypeID, 1> all;
            all.appendVector(a);
            all.appendVector(b);
            std::ranges::sort(all);
            all.shrink(std::ranges::unique(all).begin() - all.begin());
            return all;
        };
        return internMask({ unite(m_body.masks[first].inside, m_body.masks[second].inside), unite(m_body.masks[first].outside, m_body.masks[second].outside) });
    }

    // The mask of a value that passes from one type to another: none where the type it comes from says as much.
    uint32_t maskFor(TypeID from, TypeID to)
    {
        if (from == to || m_program.at(from).isSubsetOf(m_program.at(to), m_program))
            return 0;
        return maskInside(to);
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
        m_body.hasYieldedAndSent = m_function.has(FunctionFlags::IsGenerator);
        RELEASE_ASSERT(entry.parameters.size() == m_body.numberOfEntryParameters);

        for (ValueID parameter : entry.parameters) {
            NodeName node = addNode(ValueFlowBodyNodeKind::EntryParameter, parameter, carriesValue(parameter) ? maskInside(m_function.at(parameter).type) : 0);
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

    // An error that the run time makes is an object that native code makes.
    void noteHostError(BlockID block)
    {
        NodeName target = thrownTargetOf(block);
        if (m_targetsWithHostError.get(target.owner))
            return;
        m_targetsWithHostError.set(target.owner);
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
        Source source { value, mask };
        if (std::ranges::contains(m_sources.subspan(description.firstSource), source))
            return;
        m_sources.append(source);
        ++description.sourceCount;
    }

    std::span<const Source> sourcesOf(ValueID id) LIFETIME_BOUND { return m_sources.subspan(at(id).firstSource, at(id).sourceCount); }

    void setConstant(ValueID id, uint32_t constant) { at(id) = { .role = Role::Constant, .constant = constant }; }
    void setNode(ValueID id, NodeName name) { at(id) = { .role = Role::Node, .name = name }; }

    void connect(ValueID value, NodeName target)
    {
        if (value && at(value).role != Role::Nothing)
            m_edges.append({ value, target });
    }

    void describeBlock(BlockID id)
    {
        const Block& block = m_function.at(id);
        if (id != entryBlock) {
            for (uint32_t index = 0; index < block.parameters.size(); ++index)
                describeBlockParameter(id, index, block.parameters[index]);
        }
        for (ValueID value : block.values)
            describeValue(id, value);
        describeTerminator(id, block.terminator);
    }

    // A loop passes a value on that is described later, so the sources of block parameters are looked at again once every value has its role.
    void describeBlockParameter(BlockID block, uint32_t index, ValueID parameter)
    {
        if (!carriesValue(parameter))
            return;
        at(parameter).role = Role::Passing;
        m_blockParameters.append({ block, index, parameter });
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
                    addEdgeResult(parameter, edgeResultAt(m_function, incoming.result));
                    break;
                }
            }
        }
    }

    void addEdgeResult(ValueID parameter, const EdgeResult& result)
    {
        TypeID type = m_function.at(parameter).type;
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand: {
            uint32_t mask = maskFor(m_function.at(result.subject).type, type);
            if (result.test && result.side == EdgeSide::NotTaken) {
                const Value& test = m_function.at(result.test);
                if (test.opcode == Opcode::TypeTest)
                    mask = intersectMasks(mask, maskOutside(test.testedType()));
                else if (test.opcode == Opcode::StrictEq) {
                    ValueID other = test.operand(result.operandIndex ? 0 : 1);
                    if (typeOf(other).isSingleValue())
                        mask = intersectMasks(mask, maskOutside(m_function.at(other).type));
                }
            }
            addSource(parameter, result.subject, mask);
            return;
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
        if (value.opcode == Opcode::Claim)
            m_claims.append(id);
        if (operationsWithoutRule.contains(value.opcode)) {
            if (mayThrow(value.opcode))
                noteHostError(block);
            describeOperationWithoutRule(block, id, value);
            return;
        }

        describeResult(id, value);
        if (value.opcode == Opcode::NewObject && m_program.at(value.layout()).isEnvironment())
            seedDeadZonesOf(value);
        if (!needsRule(id, value)) {
            if (mayThrow(value.opcode))
                noteHostError(block);
            return;
        }
        if (hasRuleOfItsOwn(value.opcode))
            addStatement(block, id, value);
        else
            describeBySchema(block, id, value);
    }

    // What holds for an operation without a rule of its own depends on no set, so it needs no statement: an operand that only protocols can get hold of goes into the hub of its behavior,
    // any other that is more than looked at escapes, the result is whatever its type admits, and what is thrown is unknown.
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
            // Nothing follows what the operation does to the operand beyond keeping it.
            if (!info.uses.containsOnly(usesThatKeep))
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::EscapedObjectsAreOpen), constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }), 0 });
        }

        if (at(id).role == Role::Result)
            m_resultsOfUnknownValue.append(id);
        if (!mayThrow(value.opcode))
            return;
        NodeName target = thrownTargetOf(block);
        if (m_targetsWithUnknownValue.get(target.owner))
            return;
        m_targetsWithUnknownValue.set(target.owner);
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
            setConstant(id, constantOf(kindsOf(typeOf(id).primitives)));
            return;
        case Opcode::String: {
            std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = value.string() });
            setConstant(id, key ? constantOf({ ValueFlowObjectKind::Key, key->index(), { } }) : constantOf(Primitive::String));
            return;
        }
        case Opcode::KeyConstant:
            setConstant(id, constantOf({ ValueFlowObjectKind::Key, value.key().index(), { } }));
            return;
        case Opcode::Parameter:
        case Opcode::LoopHint:
        case Opcode::InitialYield:
        case Opcode::ReportMismatch:
            return;
        case Opcode::GetModuleVariable:
            if (carriesValue(id))
                setNode(id, NodeName::variable(value.variable()));
            return;
        case Opcode::PutModuleVariable:
            connect(value.valueOperand(), NodeName::variable(value.variable()));
            return;
        case Opcode::GetEnvironmentField:
            if (carriesValue(id))
                setNode(id, NodeName::field(value.layout(), value.field()));
            return;
        case Opcode::PutEnvironmentField:
            connect(value.valueOperand(), NodeName::field(value.layout(), value.field()));
            return;
        case Opcode::NewClosure: {
            setConstant(id, constantOf({ ValueFlowObjectKind::Closure, value.function().index(), { } }));
            std::span<const ValueID> captures = value.variadicOperands();
            for (uint32_t index = 0; index < captures.size(); ++index)
                connect(captures[index], NodeName::entryParameter(value.function(), index));
            return;
        }
        case Opcode::CallDirect:
            describeCallDirect(block, id, value);
            return;
        case Opcode::Results:
            return;
        case Opcode::ResultAt:
            describeResultAt(id, value);
            return;
        case Opcode::CatchValue:
        case Opcode::CatchException:
            setNode(id, caughtNodeOf(block));
            return;
        case Opcode::Yield:
            connect(value.valueOperand(), NodeName::ofBody(m_body.yielded()));
            setNode(id, NodeName::ofBody(m_body.sent()));
            return;
        case Opcode::ToObject:
        case Opcode::ToThis:
            describeToObject(id, value);
            return;
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

        // The caller gets the promise or the generator. What the body returns and throws goes into that object.
        if (isCoroutine(callee)) {
            setConstant(id, constantOf({ ValueFlowObjectKind::CoroutineResult, calleeID.index(), { } }));
            return;
        }
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
            addSource(id, producer.operand(value.index()), 0);
            return;
        }
        if (producer.opcode == Opcode::CallDirect && !isCoroutine(m_program.at(producer.function()))) {
            setNode(id, NodeName::result(producer.function(), value.index()));
            return;
        }
        if (!typeOf(id).hasObjects() && typeOf(id).isJavaScriptValue()) {
            setConstant(id, constantOfType(typeOf(id)));
            return;
        }
        // One node holds whatever any result of the operation holds, and each result is the part of it that its type admits.
        beginSources(id);
        addSource(id, producerID, maskInside(value.type));
    }

    void describeToObject(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        beginSources(id);
        addSource(id, operand, maskInside(TypeTable::object));

        // One wrapper for each class of wrappers that the operand may need. The ordinal tells them apart.
        static constexpr std::pair<uint16_t, BuiltinClassID> wrappers[] {
            { Primitive::AnyString, BuiltinClasses::String },
            { Primitive::Number, BuiltinClasses::Number },
            { Primitive::Boolean, BuiltinClasses::Boolean },
            { Primitive::AnySymbol, BuiltinClasses::Symbol },
            { Primitive::BigInt, BuiltinClasses::BigInt },
        };
        const Type& type = typeOf(operand);
        Vector<ObjectName, 6> made;
        for (uint32_t ordinal = 0; ordinal < std::size(wrappers); ++ordinal) {
            if (type.primitives & wrappers[ordinal].first)
                made.append(addSite({ ValueFlowSiteOrigin::Operation, id, ordinal, { }, wrappers[ordinal].second }));
        }
        if (value.opcode == Opcode::ToThis && type.mayBeUndefinedOrNull())
            made.append({ ValueFlowObjectKind::Builtin, m_program.builtins().canonicalID(Builtins::GlobalThis).index(), { } });
        at(id).constant = constantOf(0, made.span());
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
        // A step over a map hands out a pair that it allocates.
        if (value.opcode == Opcode::IteratorResume || value.opcode == Opcode::AppendSpread)
            addSite({ ValueFlowSiteOrigin::Operation, id, 0, { }, BuiltinClasses::Array });

        ResultRule rule = resultRuleOf(value.opcode);
        if (rule == ResultRule::Results) {
            at(id).role = Role::Result;
            return;
        }
        if (operationsCallingValues.contains(value.opcode)) {
            // The instance that a built-in base class makes for a class of the program has the layout of that class.
            bool namesLayout = value.opcode == Opcode::SuperConstruct || value.opcode == Opcode::SuperConstructList;
            addSite({ ValueFlowSiteOrigin::BuiltinResult, id, 0, namesLayout ? value.layout() : LayoutID(), { } });
            addSite({ ValueFlowSiteOrigin::BuiltinResult, id, 1, { }, { } });
        }
        if (value.opcode == Opcode::RequireDynamic || value.opcode == Opcode::ImportDynamic) {
            addSite({ ValueFlowSiteOrigin::ParsedData, id, 0, { }, BuiltinClasses::Object });
            addSite({ ValueFlowSiteOrigin::ParsedData, id, 1, { }, BuiltinClasses::Array });
        }
        if (!carriesValue(id))
            return;

        if (std::optional<uint32_t> returned = returnedOperandOf(value)) {
            ValueID operand = value.operand(*returned);
            beginSources(id);
            addSource(id, operand, maskFor(m_function.at(operand).type, value.type));
            return;
        }

        const Type& type = typeOf(id);
        switch (value.opcode) {
        case Opcode::Builtin: {
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID builtin = builtins.canonicalID(value.builtin());
            // The operation names what the body of a module makes: a rule evaluates the module.
            if (compiledModuleWithObject(m_program, builtin)) {
                at(id).role = Role::Result;
                return;
            }
            setConstant(id, isObjectKind(builtins.at(builtin).kind) ? constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }) : constantOfType(type));
            return;
        }
        case Opcode::ModuleBuiltin:
            setConstant(id, constantOf({ ValueFlowObjectKind::ModuleBuiltin, m_program.builtins().canonicalID(value.builtin()).index(), value.module() }));
            return;
        case Opcode::Data:
            describeData(id, value);
            return;
        case Opcode::NewObject:
            setConstant(id, constantOf(addSite({ ValueFlowSiteOrigin::Operation, id, 0, value.layout(), { } })));
            return;
        case Opcode::NewRuntimeError:
        case Opcode::NewSuppressedError:
            // Native code makes the error, with a class, a message and a stack that no operation spells out.
            setConstant(id, constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }));
            return;
        default:
            break;
        }

        if (rule == ResultRule::Allocation) {
            setConstant(id, constantOf(addSiteFor(id)));
            return;
        }
        if (!type.hasObjects() && type.isJavaScriptValue()) {
            setConstant(id, constantOfType(type));
            return;
        }
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

    // An operation needs no rule if it only looks at its operands or hands one on, does nothing that outlives it, and the body knows its result.
    bool needsRule(ValueID id, const Value& value)
    {
        if (at(id).role == Role::Result)
            return true;
        if (value.opcode == Opcode::Data)
            return !m_body.constants[at(id).constant].objects.isEmpty();

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
        return !bound.writes.containsOnly(bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage });
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

    // An environment leaves a field with a dead zone out of its allocation: the field holds nothing until its first store.
    void seedDeadZonesOf(const Value& allocation)
    {
        const Vector<Field>& fields = m_program.at(allocation.layout()).fields;
        BitVector isListed(fields.size());
        for (uint32_t field : allocation.fields())
            isListed.set(field);
        for (uint32_t field = 0; field < fields.size(); ++field) {
            bool hasDeadZone = m_program.at(fields[field].declared.type).primitives & Primitive::Absent;
            if (hasDeadZone && !isListed.get(field))
                m_body.seedsOnceReached.append({ NodeName::field(allocation.layout(), field), constantOf(Primitive::Absent), 0 });
        }
    }

    void addStatement(BlockID block, ValueID id, const Value& value)
    {
        // The fields of an environment are nodes of the program, whichever object holds them.
        if (value.opcode == Opcode::NewObject && m_program.at(value.layout()).isEnvironment()) {
            std::span<const ValueID> values = value.variadicOperands();
            std::span<const uint32_t> fields = value.fields();
            RELEASE_ASSERT(values.size() == fields.size());
            for (uint32_t index = 0; index < values.size(); ++index)
                connect(values[index], NodeName::field(value.layout(), fields[index]));
            return;
        }

        PendingStatement statement { id, block, static_cast<uint32_t>(m_statementOperands.size()), value.numberOfOperands(), !onlyLooksAtOperands(value) };
        for (uint32_t index = 0; index < statement.operandCount; ++index)
            m_statementOperands.append(value.operand(index));
        m_statements.append(statement);
    }

    void describeTerminator(BlockID block, const Terminator& terminator)
    {
        switch (terminator.kind) {
        case TerminatorKind::Return:
            describeReturn(terminator);
            return;
        case TerminatorKind::ReturnCall:
            connect(terminator.callOperand(), NodeName::ofBody(m_body.result(0)));
            return;
        case TerminatorKind::Throw:
            connect(terminator.valueOperand(), thrownTargetOf(block));
            return;
        case TerminatorKind::Rethrow:
            connect(terminator.exceptionOperand(), thrownTargetOf(block));
            return;
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            // Whether the test can be removed is asked of what it looks at.
            noteTestedValue(terminator);
            return;
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
        if (terminator.kind == TerminatorKind::Branch) {
            const Value& condition = m_function.at(tested);
            if (condition.opcode != Opcode::TypeTest && condition.opcode != Opcode::ToBoolean)
                return;
            m_observedValues.append({ tested, condition.operand(0) });
            return;
        }
        m_observedValues.append({ ValueID(), tested });
    }

    void describeReturn(const Terminator& terminator)
    {
        ValueID returned = terminator.valueOperand();
        if (!returned) {
            m_body.seedsOnceReached.append({ NodeName::ofBody(m_body.result(0)), constantOf(Primitive::Undefined), 0 });
            return;
        }
        const Value& producer = m_function.at(returned);
        if (producer.opcode == Opcode::Results) {
            RELEASE_ASSERT(producer.numberOfOperands() == m_body.numberOfResults);
            for (uint32_t index = 0; index < m_body.numberOfResults; ++index)
                connect(producer.operand(index), NodeName::ofBody(m_body.result(index)));
            return;
        }
        if (m_body.numberOfResults == 1) {
            connect(returned, NodeName::ofBody(m_body.result(0)));
            return;
        }
        if (producer.opcode == Opcode::CallDirect && !isCoroutine(m_program.at(producer.function()))) {
            for (uint32_t index = 0; index < m_body.numberOfResults; ++index)
                m_body.edges.append({ NodeName::result(producer.function(), index), NodeName::ofBody(m_body.result(index)) });
            return;
        }
        for (uint32_t index = 0; index < m_body.numberOfResults; ++index) {
            connect(returned, NodeName::ofBody(m_body.result(index)));
            m_body.seedsOnceReached.append({ NodeName::ofBody(m_body.result(index)), constantOfType(m_program.at(m_function.results[index].type)), 0 });
        }
    }

    void markLiveValues()
    {
        addSourcesOfBlockParameters();

        Vector<ValueID, 64> worklist;
        auto markLive = [&](ValueID value) {
            if (!value || at(value).role == Role::Nothing || at(value).isLive)
                return;
            at(value).isLive = true;
            worklist.append(value);
        };
        for (ValueID operand : m_statementOperands)
            markLive(operand);
        for (const EdgeToNode& edge : m_edges)
            markLive(edge.value);
        for (auto [test, value] : m_observedValues)
            markLive(value);
        while (!worklist.isEmpty()) {
            ValueID value = worklist.takeLast();
            if (at(value).role != Role::Passing)
                continue;
            for (const Source& source : sourcesOf(value))
                markLive(source.value);
        }

        // Whether a claim can go is asked of what it looks at. A claim alone keeps no value alive.
        for (ValueID claim : m_claims) {
            ValueID claimed = m_function.at(claim).operand(0);
            if (at(claimed).isLive)
                m_observedValues.append({ claim, claimed });
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
        for (const EdgeToNode& edge : m_edges)
            ++at(edge.value).otherReaders;
        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            ValueID value(index);
            if (!at(value).isLive || at(value).role != Role::Passing)
                continue;
            for (const Source& source : sourcesOf(value))
                ++at(source.value).otherReaders;
        }
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
                // The type of a Builtin names the built-in alone. One with a rule also holds what the body of a compiled module makes.
                bool isInsideType = type && resultRuleOf(opcode) != ResultRule::Results && opcode != Opcode::Builtin;
                description.name = addNode(ValueFlowBodyNodeKind::OperationResult, id, isInsideType ? maskInside(type) : 0);
            }
        }
        for (ValueID id : m_resultsOfUnknownValue) {
            if (at(id).isLive)
                m_body.seedsOnceReached.append({ at(id).name, unknownValue(), 0 });
        }
    }

    static void addForm(Forms& forms, const Form& form)
    {
        if (!forms.contains(form))
            forms.append(form);
    }

    // Constants under one mask are one constant, and they go along with the node under that mask.
    void uniteFormsWithSameMask(Forms& forms)
    {
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

    void connectForm(const Form& form, NodeName target, uint32_t maskOfTarget)
    {
        bool leavesBody = target.kind != NodeName::Kind::OfBody;
        if (form.constant)
            m_body.seedsOnceReached.append({ target, form.constant, form.mask == maskOfTarget ? 0 : form.mask });
        if (!form.node)
            return;
        NodeName from = form.mask && form.mask != maskOfTarget ? filterNodeOf({ form.node, 0, form.mask }) : form.node;
        (leavesBody ? m_body.edgesOnceReached : m_body.edges).append({ from, target });
    }

    NodeName filterNodeOf(const Form& form)
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

    Form makeNode(ValueID value, const Forms& forms)
    {
        bool isFilter = forms.size() == 1;
        uint32_t mask = isFilter ? forms[0].mask : 0;
        NodeName node = addNode(isFilter ? ValueFlowBodyNodeKind::Filter : ValueFlowBodyNodeKind::Merge, value, mask);
        for (const Form& form : forms)
            connectForm(form, node, mask);
        return { node, 0, 0 };
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
                m_forms[index].append({ description.name, 0, 0 });
                break;
            case Role::Passing:
            case Role::Nothing:
                break;
            }
        }
        forEachCycleOfPassingValues([&](std::span<const ValueID> members) {
            resolveCycle(members);
        });
    }

    // Tarjan's walk over the values that pass others on, along the edges to their sources: a cycle comes out after everything it draws on.
    void forEachCycleOfPassingValues(NOESCAPE const Invocable<void(std::span<const ValueID>)> auto& functor)
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
                std::span<const Source> sources = sourcesOf(value);
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
                functor(path.subspan(first));
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
            for (const Source& source : sourcesOf(member))
                hasMaskInside |= source.mask && isMember(source.value);
        }
        if (hasMaskInside) {
            resolveCycleThroughMasks(members);
            return;
        }

        ValueID head = members[0];
        Forms forms;
        uint32_t operandReaders = 0;
        uint32_t otherReaders = 0;
        for (ValueID member : members) {
            at(member).head = head;
            operandReaders += at(member).operandReaders;
            otherReaders += at(member).otherReaders;
            if (at(member).constant)
                addForm(forms, { { }, at(member).constant, 0 });
            for (const Source& source : sourcesOf(member)) {
                if (isMember(source.value)) {
                    --otherReaders;
                    continue;
                }
                for (const Form& form : formsOf(source.value))
                    addForm(forms, { form.node, form.constant, intersectMasks(form.mask, source.mask) });
            }
        }
        uniteFormsWithSameMask(forms);

        uint32_t readers = operandReaders + otherReaders;
        bool staysAsNode = false;
        if (forms.size() == 1)
            staysAsNode = forms[0].mask && forms[0].node && readers > 1;
        else if (forms.size() > 1)
            staysAsNode = operandReaders || forms.size() * readers > forms.size() + readers;
        if (staysAsNode) {
            Form node = makeNode(head, forms);
            forms = { node };
        }
        m_forms[head.index()] = WTF::move(forms);
    }

    // A mask inside a cycle takes something out on each way round, so the members do not hold the same: each is a node.
    void resolveCycleThroughMasks(std::span<const ValueID> members)
    {
        for (ValueID member : members)
            m_forms[member.index()] = { Form { addNode(ValueFlowBodyNodeKind::Merge, member), 0, 0 } };
        for (ValueID member : members) {
            NodeName node = m_forms[member.index()][0].node;
            if (at(member).constant)
                connectForm({ { }, at(member).constant, 0 }, node, 0);
            for (const Source& source : sourcesOf(member)) {
                for (const Form& form : formsOf(source.value))
                    connectForm({ form.node, form.constant, intersectMasks(form.mask, source.mask) }, node, 0);
            }
        }
    }

    void emitEdges()
    {
        for (const EdgeToNode& edge : m_edges) {
            for (const Form& form : formsOf(edge.value))
                connectForm(form, edge.target, 0);
        }
    }

    Form operandFormOf(ValueID value)
    {
        if (!value || !at(value).isLive)
            return { };
        const Forms& forms = formsOf(value);
        RELEASE_ASSERT(forms.size() <= 1);
        return forms.isEmpty() ? Form { } : forms[0];
    }

    void emitStatement(const PendingStatement& pending)
    {
        const Value& value = m_function.at(pending.site);
        Body::Statement statement { value.opcode, pending.site, static_cast<uint32_t>(m_body.operands.size()), pending.operandCount, { }, { } };
        for (ValueID operand : m_statementOperands.subspan(pending.firstOperand, pending.operandCount))
            m_body.operands.append(operandFormOf(operand));
        if (at(pending.site).role == Role::Result && at(pending.site).isLive)
            statement.result = at(pending.site).name;
        if (mayThrow(value.opcode))
            statement.thrown = thrownTargetOf(pending.block);
        m_body.statements.append(statement);
    }

    void emitStatements()
    {
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
        if (isCoroutine(m_function))
            m_body.statements.append({ { }, { }, static_cast<uint32_t>(m_body.operands.size()), 0, { }, { }, ValueFlowStatementKind::Coroutine });
        for (auto [test, value] : m_observedValues)
            m_body.observedValues.append({ test, operandFormOf(value) });
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
    FunctionID m_id;
    const Function& m_function;
    CFG m_cfg;
    Body m_body;
    InternedRecords<Body::Constant> m_constants;
    InternedRecords<Body::Mask> m_masks;
    Vector<Description> m_descriptions;
    Vector<Source> m_sources;
    Vector<BlockParameter> m_blockParameters;
    Vector<NodeName> m_caughtNodes;
    BitVector m_targetsWithHostError;
    BitVector m_targetsWithUnknownValue;
    Vector<ValueID> m_resultsOfUnknownValue;
    Vector<EdgeToNode> m_edges;
    Vector<PendingStatement> m_statements;
    Vector<ValueID> m_statementOperands;
    Vector<ObservedValue> m_observedValues;
    Vector<ValueID> m_claims;
    Vector<Forms> m_forms;
    Vector<std::pair<Form, NodeName>, 4> m_filterNodes;
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

ValueFlowBody reduceBody(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return makeBodyWithoutOperations(id, function.numberOfEntryParameters(), function.results.size(), function.has(FunctionFlags::IsGenerator));
    return BodyReducer(program, tables, id).reduce();
}

ValueFlowBody makeHostBody(const Program& program, const ValueFlowBuiltinTables& tables)
{
    ValueFlowBody body = makeBodyWithoutOperations({ }, 0, 0, false);

    // What a variable holds before any code has run. The host's body is reached from the start.
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
