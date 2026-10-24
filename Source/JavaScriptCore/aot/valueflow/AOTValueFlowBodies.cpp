/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"

#if ENABLE(AOT)
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"
#endif

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowOrder.h"
#include "AOTKnownClosures.h"
#include "AOTResultRules.h"
#include "AOTTypeRelations.h"
#include "AOTValueFlowTypedResults.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

// Where a check fails, the generic version of the function goes on with what the check has turned away, through the same operations.
BitVector findValuesBehindChecks(const Program& program, const Function& function, const CFG& cfg, const BitVector* checksThatTestNothing)
{
    BitVector isBehindCheck;
    isBehindCheck.ensureSize(function.values.size());
    bool hasCheck = std::ranges::any_of(function.values, [](const Value& value) {
        return value.opcode == Opcode::Check;
    });
    if (!hasCheck)
        return isBehindCheck;
    bool hasChanged = false;
    bool takesFromLaterBlock = false;
    auto isNoted = [&](ValueID value) {
        return value && isBehindCheck.get(value.index());
    };
    auto note = [&](ValueID value) {
        hasChanged |= !isBehindCheck.set(value.index());
    };
    auto visitParameter = [&](BlockID block, uint32_t index, ValueID parameter) {
        for (Edge edge : cfg.incomingEdges(block)) {
            takesFromLaterBlock |= cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(block);
            IncomingValue incoming = incomingValueOf(function, edge, index);
            if (incoming.kind == IncomingValue::Kind::Argument) {
                if (isNoted(incoming.argument))
                    note(parameter);
                continue;
            }
            // What a test lets by is typed by all that the test reads: behind `x === k` by the type of k too.
            const EdgeResult& result = edgeResultAt(function, incoming.result);
            bool takesTypeFromTest = isNoted(result.test);
            if (takesTypeFromTest && !isNoted(result.subject)) {
                const Value& test = function.at(result.test);
                if (test.opcode == Opcode::Less || test.opcode == Opcode::LessEq || test.opcode == Opcode::Greater || test.opcode == Opcode::GreaterEq)
                    takesTypeFromTest = program.typeOf(function, test.operand(result.operandIndex ? 0 : 1)).integers != Range::full();
            }
            if (isNoted(result.subject) || takesTypeFromTest)
                note(parameter);
        }
    };
    auto visitValue = [&](ValueID id) {
        const Value& value = function.at(id);
        if (value.opcode == Opcode::Check && !(checksThatTestNothing && checksThatTestNothing->get(id.index())))
            return note(id);
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            if (isNoted(value.operand(index)))
                return note(id);
        }
    };
    do {
        hasChanged = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t index = 0; id != entryBlock && index < block.parameters.size(); ++index) {
                if (!isNoted(block.parameters[index]))
                    visitParameter(id, index, block.parameters[index]);
            }
            for (ValueID value : block.values) {
                if (!isNoted(value))
                    visitValue(value);
            }
        }
    } while (hasChanged && takesFromLaterBlock);
    return isBehindCheck;
}

namespace {

using Body = ValueFlowBody;
using NodeName = ValueFlowNodeName;
using ObjectName = ValueFlowObjectName;
using OperandForm = Body::Operand;
using Forms = Vector<OperandForm, 1>;

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

struct BodySource {
    ValueID value;
    uint32_t mask { 0 };

    friend bool operator==(const BodySource&, const BodySource&) = default;
};

struct Description {
    Role role { Role::Nothing };
    bool isLive { false };
    // Nothing but `==` or `===` looks at it. Its set says which closures can be told apart, and nobody is told anything else about it.
    bool isLiveForComparisonsOnly { false };
    NodeName name;
    uint32_t constant { 0 };
    uint32_t firstSource { 0 };
    uint32_t sourceCount { 0 };
    uint32_t operandReaders { 0 };
    uint32_t otherReaders { 0 };
    // Of a read in a typed body: what goes on in typed code. The node holds all that was found.
    uint32_t mask { 0 };
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
    Opcode::LoopHint, Opcode::InitialYield, Opcode::ReportMismatch, Opcode::RecordRequire,
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
    case OpaqueKind::SourcePlan:
    case OpaqueKind::BuilderCursor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A literal is a string or a symbol like any other unless it is a key, which is a member of its own.
uint16_t bodyKindsOf(uint16_t primitives)
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
    return function.isUnsplitCoroutine();
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
        findValuesBehindChecks();
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
    template<typename T>
    static void sortAndRemoveRepeats(Vector<T>& list)
    {
        AOT::sort(list);
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
        if (!isCoroutine(m_function))
            return;
        if (m_function.has(FunctionFlags::IsGenerator))
            note(m_function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator);
        else
            note(BuiltinClasses::Promise);
    }

    // A function of a module that waits for a use needs what the body of the module stores, and so does whoever names a variable of it.
    void noteUsesOfModules()
    {
        auto use = [&](ModuleID module) {
            if (module && awaitsUse(m_program, module) && m_program.at(module).body != m_id)
                m_body.functionsEnteredOnceReached.append(m_program.at(module).body);
        };
        // A module with side effects may be imported by modules alone that nothing uses.
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

    // The universe finds a site by its operation.
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

    // What is known of a value by its type alone.
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
        AOT::sort(objects);
        objects.shrink(std::ranges::unique(objects).begin() - objects.begin());
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
        return m_masks.intern(WTF::move(mask), hasher.hash());
    }

    uint32_t maskInside(TypeID type, ValueFlowMaskKind kind)
    {
        const Type& record = m_program.at(type);
        if (record.isAnything() || !record.isJavaScriptValue())
            return 0;
        return internMask({ { type }, { }, kind });
    }

    // Only a test turns something away.
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
            AOT::sort(all);
            all.shrink(std::ranges::unique(all).begin() - all.begin());
            return all;
        };
        auto outsideBuiltinsOfModules = m_body.masks[first].outsideBuiltinsOfModules;
        for (auto object : m_body.masks[second].outsideBuiltinsOfModules) {
            if (!outsideBuiltinsOfModules.contains(object))
                outsideBuiltinsOfModules.append(object);
        }
        return internMask({ unite(m_body.masks[first].inside, m_body.masks[second].inside), unite(m_body.masks[first].outside, m_body.masks[second].outside), m_body.masks[first].kind, WTF::move(outsideBuiltinsOfModules) });
    }

    // The mask of a value that an operation or a branch lets through from one type to another: none where the type it comes from says as much.
    uint32_t maskFor(TypeID from, TypeID to)
    {
        if (from == to || m_program.at(from).isSubsetOf(m_program.at(to), m_program))
            return 0;
        // Every value of the language passes. The mark of a location that nothing has been stored into is none of them.
        const Type& target = m_program.at(to);
        if (target.isAnything() && !(target.primitives & Primitive::Absent))
            return maskOutside(TypeTable::absent);
        return maskInside(to, ValueFlowMaskKind::Checked);
    }

    // The type of the result is a fact where the operation makes the value itself or tests it. Where the operation hands on what lies in a position, the type is that of the position, which nothing has tested here.
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
        m_body.hasYieldedAndSent = isCoroutine(m_function) && m_function.has(FunctionFlags::IsGenerator);
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

    // An operation throws what its own code throws, and what comes out of the code that it runs for an operand: a trap of a proxy, a getter, a conversion.
    static bool mayThrowValue(Opcode opcode) { return mayThrow(opcode) || mayRunOperandCode(opcode); }

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
        BodySource source { value, mask };
        if (std::ranges::contains(m_sources.subspan(description.firstSource), source))
            return;
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

    void findValuesBehindChecks()
    {
        m_isBehindCheck = Typed::findValuesBehindChecks(m_program, m_function, m_cfg);
    }

    // The type of the result is read from the type of an operand: of the receiver of a read, of what a cast narrows, of what an operator converts.
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
                    addEdgeResult(parameter, edgeResultAt(m_function, incoming.result), m_function.at(incoming.result.from).terminator);
                    break;
                }
            }
        }
    }

    static bool isStrictlyEqualOnlyToItself(const Type& type) { return (type.isSingleValue() || !type.hasObjects()) && !(type.primitives & Primitive::Number); }

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
            // Behind a check only the test itself says what comes by.
            bool restsOnCheck = isBehindCheck(parameter);
            uint32_t mask = restsOnCheck ? 0 : maskFor(m_function.at(result.subject).type, type);
            if (restsOnCheck && result.test && result.side == EdgeSide::Taken) {
                const Value& test = m_function.at(result.test);
                if (test.opcode == Opcode::TypeTest)
                    mask = maskInside(test.testedType(), ValueFlowMaskKind::Checked);
                else if (test.opcode == Opcode::StrictEq) {
                    ValueID other = test.operand(result.operandIndex ? 0 : 1);
                    if (!isBehindCheck(other) && isStrictlyEqualOnlyToItself(typeOf(other)))
                        mask = maskInside(m_function.at(other).type, ValueFlowMaskKind::Checked);
                    else if (!isBehindCheck(other) && typeOf(other).isNumber())
                        mask = maskInside(TypeTable::number, ValueFlowMaskKind::Checked);
                } else if (test.opcode == Opcode::ToBoolean)
                    mask = maskOfTruthinessTest(result.side);
            }
            if (restsOnCheck && result.caseIndex)
                mask = maskOfCase(terminator, *result.caseIndex);
            if (result.test && result.side == EdgeSide::NotTaken) {
                const Value& test = m_function.at(result.test);
                if (test.opcode == Opcode::TypeTest)
                    mask = intersectMasks(mask, maskOutside(test.testedType()));
                else if (test.opcode == Opcode::StrictEq) {
                    ValueID other = test.operand(result.operandIndex ? 0 : 1);
                    if (!isBehindCheck(other) && typeOf(other).isSingleValue())
                        mask = intersectMasks(mask, maskOutside(m_function.at(other).type));
                    // A module has one object of each of these.
                    else if (const Value& compared = m_function.at(other); compared.opcode == Opcode::ModuleBuiltin && isObjectKind(m_program.builtins().at(m_program.builtins().canonicalID(compared.builtin())).kind))
                        mask = intersectMasks(mask, maskOutside(m_program.builtins().canonicalID(compared.builtin()), compared.module()));
                } else if (restsOnCheck && test.opcode == Opcode::ToBoolean)
                    mask = maskOfTruthinessTest(result.side);
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
        if (operationsWithoutRule.contains(value.opcode)) {
            if (mayThrow(value.opcode))
                noteHostError(block);
            describeOperationWithoutRule(block, id, value);
            return;
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
        if (hasRuleOfItsOwn(value.opcode)) {
            addStatement(block, id, value);
            return;
        }

        // What holds for an operation without a rule of its own depends on no set, so it needs no statement. The schema says of each operation how its result comes about, and it takes no operation that leaves that out.
        switch (flowOf(value.opcode)) {
        case ValueFlowEdgeClass::Allocates:
        case ValueFlowEdgeClass::Copies:
        case ValueFlowEdgeClass::TableProves:
        case ValueFlowEdgeClass::Checks:
            describeBySchema(block, id, value);
            return;
        case ValueFlowEdgeClass::Unsound:
            describeUnsoundOperation(block, id, value);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The allocation dominates the operation, so its site is there.
    void noteLiteralNamingPrototype(ValueID literal)
    {
        for (ValueFlowSite& site : m_body.sites | std::views::reverse) {
            if (site.value != literal)
                continue;
            site.namesPrototype = site.origin == ValueFlowSiteOrigin::Operation && m_function.at(literal).opcode == Opcode::NewObject;
            return;
        }
    }

    // Nothing says what the operation does with its operands and where its result comes from: it is code that nobody compiles.
    void describeUnsoundOperation(BlockID block, ValueID id, const Value& value)
    {
        size_t numberOfEdges = m_edges.size();
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index)
            connect(value.operand(index), NodeName::escaped());
        if (m_edges.size() != numberOfEdges)
            m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::UnknownCodeWrites), constantOf({ ValueFlowObjectKind::UnknownObject, 0, { } }), 0 });
        describeUnknownOutcome(block, id, value);
    }

    // An operand that only protocols can get hold of goes into the hub of its behavior, any other that is more than looked at escapes, the result is whatever its type admits, and what is thrown is unknown.
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
            setConstant(id, constantOf(bodyKindsOf(typeOf(id).primitives)));
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
        // It notes for the host who has required whom first. What `children` and `parent` may answer the requests say.
        case Opcode::RecordRequire:
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
        addSource(id, producerID, maskInside(value.type, kindOfResultMask(producer.opcode)));
    }

    // `function F(..) { if (!(this instanceof F)) return new F(..); .. this[key] = value; .. }`: a constructor function that may be called without `new`.
    bool isUsedOnlyAsInstanceOfOwnFunction(ValueID thisValue)
    {
        auto isThis = [&](ValueID id) {
            return id && definitionThroughSameValues(m_function, id) == thisValue;
        };
        BlockID testing;
        ValueID test;
        for (uint32_t index = 0; index < m_function.blocks.size() && !test; ++index) {
            for (ValueID id : m_function.blocks[index].values) {
                const Value& candidate = m_function.at(id);
                if (candidate.opcode != Opcode::InstanceOf || !isThis(candidate.operand(0)))
                    continue;
                const Type& constructor = typeOf(candidate.operand(1));
                if (constructor.primitives || constructor.anyObject || constructor.anyFunction || constructor.functions.size() != 1 || constructor.functions[0] != m_id)
                    return false;
                testing = BlockID(index);
                test = id;
                break;
            }
        }
        if (!test)
            return false;
        const Terminator& terminator = m_function.at(testing).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            return false;
        ValueID condition = terminator.operand(0);
        if (m_function.at(condition).opcode == Opcode::ToBoolean)
            condition = m_function.at(condition).operand(0);
        BlockID passed = terminator.taken().block;
        if (condition != test || passed == terminator.notTaken().block || passed == entryBlock)
            return false;

        // The blocks that control reaches without coming by the block behind the test. That block is entered from among them by the passing edge alone.
        BitVector isReachedOtherwise(m_function.blocks.size());
        Vector<BlockID, 16> worklist { entryBlock };
        isReachedOtherwise.quickSet(entryBlock.index());
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (BlockID successor : m_cfg.successors(block)) {
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
            const Block& block = m_function.blocks[index];
            for (ValueID id : block.values) {
                const Value& user = m_function.at(id);
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

    void describeToObject(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        beginSources(id);
        addSource(id, operand, maskInside(TypeTable::object, ValueFlowMaskKind::Checked));

        // One wrapper for each class of wrappers that the operand may need. The ordinal tells them apart.
        static constexpr std::pair<uint16_t, NamedBuiltinClass> wrappers[] {
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
        if (value.opcode == Opcode::ToThis && type.mayBeUndefinedOrNull() && !isUsedOnlyAsInstanceOfOwnFunction(id))
            made.append({ ValueFlowObjectKind::Builtin, m_program.builtins().canonicalID(Builtins::GlobalThis).index(), { } });
        at(id).constant = constantOf(0, made.span());
    }

    uint32_t maskInEveryVersion(Opcode opcode)
    {
        switch (opcode) {
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
        // Two closures of one function are two objects, and a comparison is what tells.
        if (provesOf(value.opcode) == Proof::Equality && m_program.typeOf(m_function, value.operand(0)).mayBeFunction() && m_program.typeOf(m_function, value.operand(1)).mayBeFunction())
            m_comparedValues.append({ value.operand(0), value.operand(1) });
        // A step over a map hands out a pair that it allocates.
        if (value.opcode == Opcode::IteratorResume || value.opcode == Opcode::AppendSpread)
            addSite({ ValueFlowSiteOrigin::Operation, id, 0, { }, BuiltinClasses::Array });
        if (value.opcode == Opcode::NewDictionary || value.opcode == Opcode::CopyObject)
            addSite({ ValueFlowSiteOrigin::Operation, id, 0, { }, BuiltinClasses::Object });
        // What a load under a name nobody knows finds outside the image is data: the objects and the arrays that the loader makes of a file. See dataFromOutsideImage().
        if (value.opcode == Opcode::RequireDynamic || value.opcode == Opcode::ImportDynamic) {
            addSite({ ValueFlowSiteOrigin::Operation, id, 1, { }, BuiltinClasses::Object });
            addSite({ ValueFlowSiteOrigin::Operation, id, 2, { }, BuiltinClasses::Array });
        }

        if (value.opcode == Opcode::PropGet) {
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID named = entryNamedByOperations(m_program, m_function, id);
            if (named && builtins.at(named).origin == BuiltinOrigin::Host && isCallableBuiltin(builtins, named))
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders), constantOf({ ValueFlowObjectKind::Builtin, builtins.canonicalID(named).index(), { } }), 0 });
        }

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
        if (!carriesValue(id))
            return;

        // Nothing covers the conversion. A rule takes the value for what it may not be.
        if (value.opcode == Opcode::Convert && value.illTypedCause() != IllTypedCause::None) {
            at(id).role = Role::Result;
            return;
        }
        // What the test turns away goes on through the same operations in the generic version of the function, so the result holds it too.
        if (value.opcode == Opcode::Check) {
            beginSources(id);
            addSource(id, value.valueOperand(), maskInside(value.castType(), ValueFlowMaskKind::Expected));
            m_checks.append(id);
            return;
        }
        if (std::optional<uint32_t> returned = returnedOperandOf(value)) {
            ValueID operand = value.operand(*returned);
            beginSources(id);
            addSource(id, operand, isBehindCheck(operand) ? maskInEveryVersion(value.opcode) : maskFor(m_function.at(operand).type, value.type));
            return;
        }

        // While the check passes nothing may reach an operation behind it, which then has no type at all. The generic version of the function runs it on what the check has turned away.
        std::optional<Type> typeInGenericVersion;
        if (rule == ResultRule::Natural && isBehindCheck(id) && typeOf(id).isNever())
            typeInGenericVersion = tryWidestNaturalTypeOf(value);
        const Type& type = typeInGenericVersion ? *typeInGenericVersion : typeOf(id);
        switch (value.opcode) {
        case Opcode::Builtin: {
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID builtin = builtins.canonicalID(value.builtin());
            // The operation names what the body of a module makes: a rule evaluates the module.
            if (compiledModuleWithObject(m_program, builtin)) {
                at(id).role = Role::Result;
                return;
            }
            BuiltinName symbol = builtins.nameOfSymbol(builtin);
            if (PropertyKeyID key = symbol ? m_program.keyOf(symbol) : PropertyKeyID()) {
                setConstant(id, constantOf({ ValueFlowObjectKind::Key, key.index(), { } }));
                return;
            }
            if (!isObjectKind(builtins.at(builtin).kind)) {
                setConstant(id, constantOfType(type));
                return;
            }
            setConstant(id, constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }));
            m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::BuiltinsThatStatementsName), constantOf({ ValueFlowObjectKind::Builtin, builtin.index(), { } }), 0 });
            return;
        }
        case Opcode::ModuleBuiltin: {
            // The path and the folder of a module are strings.
            if (!isObjectKind(m_program.builtins().at(m_program.builtins().canonicalID(value.builtin())).kind)) {
                setConstant(id, constantOfType(type));
                return;
            }
            uint32_t object = constantOf({ ValueFlowObjectKind::ModuleBuiltin, m_program.builtins().canonicalID(value.builtin()).index(), value.module() });
            setConstant(id, object);
            if (m_program.builtins().canonicalID(value.builtin()) == Builtins::ModuleObject)
                m_body.seedsOnceReached.append({ NodeName::ofHost(ValueFlowHostNode::ModuleObjectsHandedOut), object, 0 });
            return;
        }
        case Opcode::Data:
            describeData(id, value);
            return;
        case Opcode::NewObject: {
            ObjectName site = addSite({ ValueFlowSiteOrigin::Operation, id, 0, value.layout(), { } });
            // The prototype picks the class, and each class has an object of its own for its instances: the rule says which of them the operation answers.
            if (mayAllocateOtherLayout(m_program, value))
                at(id).role = Role::Result;
            else
                setConstant(id, constantOf(site));
            return;
        }
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
        // The types of the operands say which boolean a test answers, which text `typeof` and which numbers a conversion.
        if (rule == ResultRule::Natural && isBehindCheck(id) && !type.hasObjects() && type.isJavaScriptValue() && !answersByKindsOfOperands(value.opcode)) {
            uint16_t kinds = bodyKindsOf(type.primitives);
            for (uint16_t group : { Primitive::Boolean, Primitive::Number }) {
                if (kinds & group)
                    kinds |= group;
            }
            setConstant(id, constantOf(kinds));
            return;
        }
        // Where the type leaves open whether an operator answers a number, a string or a bigint, the kinds of its operands decide.
        bool mayTurnOutWithoutFraction = type.primitives & Primitive::FractionalNumber && makesNoFraction(value.opcode);
        bool isOfOneKind = (type.hasOnlyPrimitives(Primitive::Number) && !mayTurnOutWithoutFraction) || type.hasOnlyPrimitives(Primitive::AnyString) || type.hasOnlyPrimitives(Primitive::BigInt);
        if (!type.hasObjects() && type.isJavaScriptValue() && !isTypedByChecks(id) && (isOfOneKind || !answersByKindsOfOperands(value.opcode))) {
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
        // It has no operand, and it is born with its elements.
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
            // The host's body holds what a `var` starts as.
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (!listsStartOfVar(m_function, value, index))
                    connect(values[index], NodeName::field(value.layout(), fields[index]));
            }
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
            m_returnsCall = true;
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

    bool isOwnNewTarget(ValueID id) const
    {
        std::optional<unsigned> index = m_function.newTargetParameterIndex();
        return id && index && definitionThroughSameValues(m_function, id) == m_function.at(entryBlock).parameters[*index];
    }

    // What the function gets as `this`, what the constructor of a base class allocates for its new.target, and what the constructor above answers to that new.target.
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

    // A constructor answers its completion value if that is an object, and else the object that it constructs.
    bool answersOnlyWhatItConstructs()
    {
        if (m_function.constructorForm() == ConstructorForm::NotConstructor || m_function.isUnsplitCoroutine() || m_returnsCall)
            return false;
        BitVector isVisited(m_function.values.size());
        Vector<ValueID, 8> worklist = m_returnedValues;
        while (!worklist.isEmpty()) {
            ValueID value = worklist.takeLast();
            if (isVisited.set(value.index()) || at(value).role == Role::Nothing || !typeOf(value).hasObjects() || isObjectUnderConstruction(value))
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
        if (returned)
            m_returnedValues.append(returned);
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
        markSources();

        // Whether a check can go is asked of what it looks at. A check alone keeps no value alive.
        for (ValueID check : m_checks) {
            ValueID tested = m_function.at(check).valueOperand();
            if (at(tested).isLive && !at(tested).isLiveForComparisonsOnly)
                m_observedValues.append({ check, tested });
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

    // The step that finds the end answers undefined as its value.
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
                // The type of a Builtin names the built-in alone. One with a rule also holds what the body of a compiled module makes.
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

    // What the mask lets through of the form. Masks of one kind are one mask. A type that is taken on trust keeps nothing out: it only says what an untracked object may be, and a test says that better.
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
            for (const BodySource& source : sourcesOf(member))
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
            for (const BodySource& source : sourcesOf(member)) {
                if (isMember(source.value)) {
                    --otherReaders;
                    continue;
                }
                for (const OperandForm& form : formsOf(source.value))
                    addForm(forms, throughMask(form, source.mask));
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
            OperandForm node = makeNode(head, forms);
            forms = { node };
        }
        m_forms[head.index()] = WTF::move(forms);
    }

    // A mask inside a cycle takes something out on each way round, so the members do not hold the same: each is a node.
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

    // The index of the operand of a call that the node stands for: `this` or a parameter with a position.
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
        bool mayHaveSummary = m_body.numberOfResults == 1 && !isCoroutine(m_function);
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
            // A return without a value has put its constant into the result already.
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

    // Through a receiver and through a callee a rule looks things up member by member.
    OperandForm operandFormOf(const Value& operation, uint32_t index, ValueID value)
    {
        OperandForm form = operandFormOf(value);
        bool isLookedThrough = receiverOf(operation.opcode) == index || (!index && operationsCallingValues.contains(operation.opcode));
        if (!isLookedThrough || !form.node || form.mask)
            return form;
        const Type& type = typeOf(value);
        if (!type.anyObject && !type.anyFunction && type.classes.isEmpty() && (type.layouts || !type.functions.isEmpty()))
            form.mask = maskInside(m_function.at(value).type, isBehindCheck(value) ? ValueFlowMaskKind::Expected : ValueFlowMaskKind::Declared);
        return form;
    }

    bool isNeverObserved(ValueID operation, uint32_t operandIndex) const
    {
        Opcode opcode = m_function.at(operation).opcode;
        bool isDefinedValue = (opcode == Opcode::PropDefine && operandIndex == OperandIndex::PropDefine::value) || (opcode == Opcode::PrivateDefine && operandIndex == OperandIndex::PrivateDefine::value);
        return m_unobservedDefinitions && isDefinedValue && m_unobservedDefinitions->contains(m_id, operation);
    }

    void emitStatement(const PendingStatement& pending)
    {
        const Value& value = m_function.at(pending.site);
        Body::Statement statement { value.opcode, pending.site, static_cast<uint32_t>(m_body.operands.size()), pending.operandCount, { }, { } };
        for (uint32_t index = 0; index < pending.operandCount; ++index)
            m_body.operands.append(isNeverObserved(pending.site, index) ? OperandForm { } : operandFormOf(value, index, m_statementOperands[pending.firstOperand + index]));
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
        if (isCoroutine(m_function))
            m_body.statements.append({ { }, { }, static_cast<uint32_t>(m_body.operands.size()), 0, { }, { }, ValueFlowStatementKind::Coroutine });

        for (auto [test, value] : m_observedValues)
            m_body.observedValues.append({ test, operandFormOf(value) });
        for (auto [left, right] : m_comparedValues)
            m_body.comparedValues.append({ operandFormOf(left), operandFormOf(right) });
        for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
            if (m_descriptions[index].isLiveForComparisonsOnly)
                m_body.valuesAliveForComparisonsOnly.append(ValueID(index));
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
    FunctionID m_id;
    const Function& m_function;
    CFG m_cfg;
    Body m_body;
    InternedBodyRecords<Body::Constant> m_constants;
    InternedBodyRecords<Body::Mask> m_masks;
    Vector<Description, 48> m_descriptions;
    Vector<BodySource, 48> m_sources;
    Vector<BlockParameter, 16> m_blockParameters;
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
    // The type of `true`, then that of `false`, once somebody has asked.
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

ValueFlowBody reduceBody(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id, const UnobservedFieldDefinitions* unobservedDefinitions)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return makeBodyWithoutOperations(id, function.numberOfEntryParameters(), function.results.size(), isCoroutine(function) && function.has(FunctionFlags::IsGenerator));
    return BodyReducer(program, tables, id, unobservedDefinitions).reduce();
}

bool startsWithoutValueInSolve(const Program& program, const Variable& variable)
{
    return variable.initialization == VariableInitialization::Undefined && variable.flags.contains(VariableFlags::IsImmutable) && variable.module && program.at(variable.module).kind == ModuleKind::RuntimeSource;
}

ValueFlowBody makeHostBody(const Program& program, const ValueFlowBuiltinTables& tables, std::span<const uint64_t> operationsOfFunctions)
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

    // So does a `var` that lives in an environment. Its `undefined` stands here, apart from what the code of the program stores: where no read sees the start, the type of the field is that of the stores.
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
