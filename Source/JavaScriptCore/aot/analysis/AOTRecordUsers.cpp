/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRecordUsers.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTIR.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include <atomic>
#include <numeric>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What can be seen through a record.
using Mark = uint64_t;

constexpr unsigned numberOfKeyBits = 61;
constexpr Mark seenByAll = ~static_cast<Mark>(0);
constexpr Mark seenWithoutKey = static_cast<Mark>(1) << 63;
// A field of an environment, which an operation names by its position.
constexpr Mark seenInEnvironment = static_cast<Mark>(1) << 62;
// A field under a name that a protocol or a built-in looks for in what it is handed.
constexpr Mark seenByCodeOfOperands = static_cast<Mark>(1) << 61;

constexpr Mark bitOf(PropertyKeyID key) { return static_cast<Mark>(1) << (key.index() % numberOfKeyBits); }

class Marks {
public:
    explicit Marks(size_t size)
        : m_marks(FillWith { }, size, static_cast<Mark>(0))
    {
    }

    size_t size() const { return m_marks.size(); }
    Mark at(size_t index) const { return WTF::atomicLoad(const_cast<Mark*>(&m_marks[index]), std::memory_order_relaxed); }
    // One thread at a time for an index.
    void add(size_t index, Mark mark) { WTF::atomicStore(&m_marks[index], at(index) | mark, std::memory_order_relaxed); }

private:
    Vector<Mark> m_marks;
};

class RecordUsers {
public:
    RecordUsers(const Program& program, const ParallelLoops& loops)
        : m_program(program)
        , m_loops(loops)
        , m_layouts(program.layouts.size())
        , m_variables(program.variables.size())
        , m_layoutSets(program.types.numberOfLayoutSets())
        , m_types(program.types.size())
    {
    }

    void add(LayoutID id) { m_layouts.add(id.index(), seenByAll); }
    void add(VariableID id) { m_variables.add(id.index(), seenByAll); }
    void add(std::span<const FieldReference>);

    void addRecordsNamingMarkedRecords();
    void addRecordsLeadingToMarkedRecords();

    bool seesMarkedRecord(FunctionID) const;
    bool namesMarkedRecord(FunctionID) const;
    bool namesMarkedVariable(FunctionID) const;
    Vector<FunctionID> findFunctionsWhere(const Invocable<bool(unsigned)> auto& isFound) const;

private:
    struct Search {
        void operator()(const LayoutID& id) { found |= users.m_layouts.at(id.index()); }
        void operator()(const VariableID& id) { found |= users.m_variables.at(id.index()); }
        void operator()(const LayoutSetID& id) { found |= users.m_layoutSets.at(id.index()); }
        void operator()(const TypeID& id) { found |= users.m_types.at(id.index()); }
        template<typename Tag> void operator()(const ID<Tag>&) { }

        const RecordUsers& users;
        Mark found { 0 };
    };

    struct SourcesInRecord {
        void operator()(const LayoutID& id) { functor(users.nodeOf(id)); }
        void operator()(const LayoutSetID& id) { functor(users.nodeOf(id)); }
        void operator()(const TypeID& id) { functor(users.nodeOf(id)); }
        template<typename Tag> void operator()(const ID<Tag>&) { }

        const RecordUsers& users;
        const ScopedLambda<void(uint32_t)>& functor;
    };

    struct SearchOutsideTypes : Search {
        using Search::operator();
        void operator()(const TypeID&) { }
    };

    struct KeySearch {
        void operator()(const PropertyKeyID& key)
        {
            hasKey = true;
            if (users.m_isKeyOfMarkedField[key.index()])
                found |= bitOf(key);
        }
        template<typename Tag> void operator()(const ID<Tag>&) { }

        const RecordUsers& users;
        bool hasKey { false };
        Mark found { 0 };
    };

    // Layouts, sets of layouts and types in one row of numbers.
    using Node = uint32_t;
    Node nodeOf(LayoutID id) const { return id.index(); }
    Node nodeOf(LayoutSetID id) const { return m_layouts.size() + id.index(); }
    Node nodeOf(TypeID id) const { return m_layouts.size() + m_layoutSets.size() + id.index(); }
    unsigned numberOfNodes() const { return m_layouts.size() + m_layoutSets.size() + m_types.size(); }
    Mark markAt(Node) const;
    void addAt(Node, Mark);

    void forEachSourceOf(const LayoutSet&, const Invocable<void(Node)> auto&) const;
    void forEachSourceOf(const Type&, const Invocable<void(Node)> auto&) const;
    void forEachInheritedLayout(LayoutID, const Invocable<void(Node)> auto&) const;
    void forEachLayoutStoodFor(LayoutID, const Invocable<void(Node)> auto&) const;
    void addMarksOfSources(Node firstTarget, Node endOfTargets, const Invocable<void(Node, const ScopedLambda<void(Node)>&)> auto& forEachSourceOf);
    Mark markOfTypesOf(const Value&) const;
    Mark markOfAllNamedBy(FunctionID) const;
    Mark markOfOperandsWhoseCodeRuns(const Function&, const auto& instruction, const Invocable<bool(uint32_t)> auto& runsCodeOfOperand) const;
    Mark markOfOperandsWhoseCodeRuns(const Function&, const Value&) const;
    Mark markOfOperandsWhoseCodeRuns(const Function&, const Terminator&) const;
    bool looksForMarkedField(const Function&, const Value&, bool looksAtCodeOfOperands) const;
    bool looksForMarkedField(FunctionID, Mark markOfAllNamed) const;

    const Program& m_program;
    const ParallelLoops& m_loops;
    Marks m_layouts;
    Marks m_variables;
    Marks m_layoutSets;
    Marks m_types;
    Vector<uint8_t> m_isKeyOfMarkedField;
    Vector<FieldReference> m_fieldsOfEnvironments;
};

} // anonymous namespace

// A protocol and a built-in look for names of their own in the objects they are handed. An environment is handed to neither, and nothing looks for a key in it.
void RecordUsers::add(std::span<const FieldReference> fields)
{
    if (fields.empty())
        return;

    unsigned numberOfKeys = m_program.types.numberOfPropertyKeys();
    m_isKeyOfMarkedField.fill(0, numberOfKeys);
    Vector<uint8_t> isReadFromOperands(FillWith { }, numberOfKeys, static_cast<uint8_t>(0));
    for (BuiltinName name : namesReadFromOperands(m_program.builtins()))
        isReadFromOperands[m_program.keyOf(name).index()] = 1;

    for (FieldReference reference : fields) {
        if (m_program.at(reference.layout).kind == LayoutKind::Environment) {
            m_fieldsOfEnvironments.append(reference);
            m_layouts.add(reference.layout.index(), seenInEnvironment);
            continue;
        }
        PropertyKeyID key = fieldAt(m_program, reference).key;
        if (!key) {
            add(reference.layout);
            continue;
        }
        m_isKeyOfMarkedField[key.index()] = 1;
        m_layouts.add(reference.layout.index(), isReadFromOperands[key.index()] ? bitOf(key) | seenByCodeOfOperands : bitOf(key));
    }
    std::ranges::sort(m_fieldsOfEnvironments);
}

Mark RecordUsers::markAt(Node node) const
{
    if (node < m_layouts.size())
        return m_layouts.at(node);
    node -= m_layouts.size();
    return node < m_layoutSets.size() ? m_layoutSets.at(node) : m_types.at(node - m_layoutSets.size());
}

void RecordUsers::addAt(Node node, Mark mark)
{
    if (node < m_layouts.size()) {
        m_layouts.add(node, mark);
        return;
    }
    node -= m_layouts.size();
    if (node < m_layoutSets.size())
        m_layoutSets.add(node, mark);
    else
        m_types.add(node - m_layoutSets.size(), mark);
}

void RecordUsers::forEachSourceOf(const LayoutSet& set, const Invocable<void(Node)> auto& functor) const
{
    for (LayoutID member : set.members)
        functor(nodeOf(member));
}

void RecordUsers::forEachSourceOf(const Type& type, const Invocable<void(Node)> auto& functor) const
{
    if (type.environment)
        functor(nodeOf(type.environment));
    if (type.layouts)
        functor(nodeOf(type.layouts));
    for (const ClassPart& part : type.classes) {
        for (TypeID contents : part.contents) {
            if (contents)
                functor(nodeOf(contents));
        }
    }
}

// Whoever looks for a member of an object of this layout goes on in the layouts it inherits from, and asks the layout that declares a field what flow says of it.
void RecordUsers::forEachInheritedLayout(LayoutID id, const Invocable<void(Node)> auto& functor) const
{
    const Layout& layout = m_program.at(id);
    auto visitIfSet = [&](LayoutID other) {
        if (other)
            functor(nodeOf(other));
    };
    visitIfSet(layout.base);
    visitIfSet(layout.flow.base);
    visitIfSet(layout.classFlow.prototypeBase);
    for (LayoutID supertype : layout.supertypes)
        functor(nodeOf(supertype));
    for (const Field& field : layout.fields)
        visitIfSet(field.originLayout);
}

// An object that is known by this layout may have any of the layouts below it.
void RecordUsers::forEachLayoutStoodFor(LayoutID id, const Invocable<void(Node)> auto& functor) const
{
    for (LayoutID member : m_program.at(id).flow.members)
        functor(nodeOf(member));
    if (LayoutSetID members = m_program.declaredMembersOf(id))
        functor(nodeOf(members));
    for (LayoutID below : m_program.layoutsBelow(id))
        functor(nodeOf(below));
}

Mark RecordUsers::markOfTypesOf(const Value& value) const
{
    Mark mark = 0;
    if (value.type)
        mark |= m_types.at(value.type.index());
    if (TypeID expected = value.expected())
        mark |= m_types.at(expected.index());
    return mark;
}

// Where something that whoever names it sees is found, the rest of the marks may be missing.
Mark RecordUsers::markOfAllNamedBy(FunctionID id) const
{
    BodyIndex* index = m_program.bodyIndex();
    if (std::optional<NamesInBody> names = index ? index->namesIn(m_program, id) : std::nullopt) {
        Mark mark = 0;
        for (TypeID type : names->types)
            mark |= m_types.at(type.index());
        for (LayoutID layout : names->layouts)
            mark |= m_layouts.at(layout.index());
        for (LayoutSetID set : names->layoutSets)
            mark |= m_layoutSets.at(set.index());
        for (VariableID variable : names->variables)
            mark |= m_variables.at(variable.index());
        return mark;
    }

    const Function& function = m_program.functions[id.index()];
    Search search { *this };
    forEachReference(static_cast<const FunctionRecord&>(function), search);
    for (const Value& value : function.values) {
        if (search.found & seenWithoutKey)
            return search.found;
        search.found |= markOfTypesOf(value);
        forEachReference(value, search);
    }
    for (const Block& block : function.blocks)
        forEachReference(block.terminator, search);
    return search.found;
}

bool RecordUsers::namesMarkedRecord(FunctionID id) const
{
    return !!markOfAllNamedBy(id);
}

// What seesMarkedRecord() answers where only variables are marked: no layout, no set of layouts and no type gets a mark from a variable.
bool RecordUsers::namesMarkedVariable(FunctionID id) const
{
    BodyIndex* index = m_program.bodyIndex();
    if (std::optional<NamesInBody> names = index ? index->namesIn(m_program, id) : std::nullopt) {
        return std::ranges::any_of(names->variables, [&](VariableID variable) {
            return !!m_variables.at(variable.index());
        });
    }

    const Function& function = m_program.functions[id.index()];
    bool isFound = false;
    auto search = [&](VariableID id) {
        isFound |= !!m_variables.at(id.index());
    };
    forEachVariable(static_cast<const FunctionRecord&>(function), search);
    for (const Value& value : function.values) {
        if (isFound)
            return true;
        forEachVariable(value, search);
    }
    for (const Block& block : function.blocks)
        forEachVariable(block.terminator, search);
    return isFound;
}

Mark RecordUsers::markOfOperandsWhoseCodeRuns(const Function& function, const auto& instruction, const Invocable<bool(uint32_t)> auto& runsCodeOfOperand) const
{
    Mark mark = 0;
    instruction.forEachOperand([&](ValueID operand, uint32_t index) {
        if (operand && runsCodeOfOperand(index))
            mark |= markOfTypesOf(function.at(operand));
    });
    return mark;
}

// As describeEffects() goes about it: an operation says which protocols run on each operand.
Mark RecordUsers::markOfOperandsWhoseCodeRuns(const Function& function, const Value& value) const
{
    switch (effectRuleOf(value.opcode)) {
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        return markOfOperandsWhoseCodeRuns(function, value, [&](uint32_t index) {
            return !protocolsOf(value.opcode, index).isEmpty();
        });
    case EffectRule::Call:
        if (!OperandPosition::newTarget(value.opcode) && calleesOfCall(m_program, value).builtins.isEmpty())
            return 0;
        [[fallthrough]];
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return markOfOperandsWhoseCodeRuns(function, value, [](uint32_t) {
            return true;
        });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Mark RecordUsers::markOfOperandsWhoseCodeRuns(const Function& function, const Terminator& terminator) const
{
    bool isInline = effectRuleOf(terminator.kind) == EffectRule::Inline;
    return markOfOperandsWhoseCodeRuns(function, terminator, [&](uint32_t index) {
        return isInline || !protocolsOf(terminator.kind, index).isEmpty();
    });
}

// What is said of an operation comes from the record of a field in three ways.
bool RecordUsers::looksForMarkedField(const Function& function, const Value& value, bool looksAtCodeOfOperands) const
{
    constexpr OpcodeSet accessesToOneFieldOfEnvironment { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
    if (accessesToOneFieldOfEnvironment.contains(value.opcode))
        return std::ranges::binary_search(m_fieldsOfEnvironments, FieldReference { value.layout(), value.field() });

    KeySearch keys { *this };
    forEachReference(value, keys);
    bool goesByAnyField = Value::opcodesWithLayout.contains(value.opcode) || Value::opcodesWithFields.contains(value.opcode)
        || (effectRuleOf(value.opcode) == EffectRule::Access && !keys.hasKey);
    if (looksAtCodeOfOperands && (markOfOperandsWhoseCodeRuns(function, value) & seenByCodeOfOperands))
        return true;
    Mark wanted = goesByAnyField ? seenByAll : keys.found;
    if (!wanted)
        return false;

    Search search { *this, markOfTypesOf(value) };
    forEachReference(value, search);
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand)
            search.found |= markOfTypesOf(function.at(operand));
    });
    return !!(search.found & wanted);
}

bool RecordUsers::looksForMarkedField(FunctionID id, Mark markOfAllNamed) const
{
    const Function& function = m_program.functions[id.index()];
    SearchOutsideTypes searchOfRecord { { *this } };
    forEachReference(static_cast<const FunctionRecord&>(function), searchOfRecord);
    if (searchOfRecord.found)
        return true;

    bool looksAtCodeOfOperands = !!(markOfAllNamed & seenByCodeOfOperands);
    if (looksAtCodeOfOperands) {
        for (const Value& value : function.values) {
            if (looksForMarkedField(function, value, true))
                return true;
        }
    } else {
        static const OpcodeSet opcodesThatMayLook = [] {
            OpcodeSet opcodes = findOpcodesNaming<PropertyKeyID>() | Value::opcodesWithLayout | Value::opcodesWithFields;
            for (Opcode opcode : allMembersOf<Opcode>) {
                if (effectRuleOf(opcode) == EffectRule::Access)
                    opcodes.add(opcode);
            }
            return opcodes;
        }();
        bool looks = anyValueListedOrNot(m_program, id, opcodesThatMayLook, [&](ValueID, const Value& value) {
            return looksForMarkedField(function, value, false);
        });
        if (looks)
            return true;
        static const TerminatorKindSet kindsThatMayName = findTerminatorKindsNaming<LayoutID, VariableID, LayoutSetID>();
        if (!mayEndBlockWith(m_program, id, kindsThatMayName))
            return false;
    }
    for (const Block& block : function.blocks) {
        SearchOutsideTypes search { { *this } };
        forEachReference(block.terminator, search);
        if (search.found || (looksAtCodeOfOperands && (markOfOperandsWhoseCodeRuns(function, block.terminator) & seenByCodeOfOperands)))
            return true;
    }
    return false;
}

bool RecordUsers::seesMarkedRecord(FunctionID id) const
{
    Mark mark = markOfAllNamedBy(id);
    if (!mark)
        return false;
    return (mark & seenWithoutKey) || looksForMarkedField(id, mark);
}

// Gives every node the marks of its sources, and of their sources, until nothing is added: the least marks that hold.
void RecordUsers::addMarksOfSources(Node firstOfTargets, Node endOfTargets, const Invocable<void(Node, const ScopedLambda<void(Node)>&)> auto& forEachSourceOf)
{
    unsigned count = endOfTargets;
    constexpr unsigned nodesInShare = 512;
    auto forEachEdge = [&](const Invocable<void(Node source, Node target)> auto& functor) {
        forEachIndexInParallel(m_loops, (endOfTargets - firstOfTargets + nodesInShare - 1) / nodesInShare, [&](unsigned share) {
            for (Node target = firstOfTargets + share * nodesInShare; target < std::min(endOfTargets, firstOfTargets + (share + 1) * nodesInShare); ++target) {
                if (markAt(target) == seenByAll)
                    continue;
                auto visitSource = [&](Node source) {
                    functor(source, target);
                };
                forEachSourceOf(target, ScopedLambda<void(Node)>(visitSource));
            }
        });
    };

    Vector<uint32_t> firstTarget(FillWith { }, count + 1, 0u);
    forEachEdge([&](Node source, Node) {
        WTF::atomicExchangeAdd(&firstTarget[source + 1], 1u, std::memory_order_relaxed);
    });
    std::partial_sum(firstTarget.begin(), firstTarget.end(), firstTarget.begin());
    Vector<Node> targets;
    targets.grow(firstTarget[count]);
    Vector<uint32_t> nextTarget(firstTarget.span().first(count));
    forEachEdge([&](Node source, Node target) {
        targets[WTF::atomicExchangeAdd(&nextTarget[source], 1u, std::memory_order_relaxed)] = target;
    });

    Vector<Node> worklist = findIndicesInParallel(m_loops, count, [&](unsigned node) {
        return markAt(node) && firstTarget[node] != firstTarget[node + 1];
    });
    BitVector isInWorklist(count);
    for (Node node : worklist)
        isInWorklist.quickSet(node);
    while (!worklist.isEmpty()) {
        Node source = worklist.takeLast();
        isInWorklist.quickClear(source);
        Mark mark = markAt(source);
        for (Node target : targets.span().subspan(firstTarget[source], firstTarget[source + 1] - firstTarget[source])) {
            if (!(mark & ~markAt(target)))
                continue;
            addAt(target, mark);
            if (firstTarget[target] != firstTarget[target + 1] && !isInWorklist.quickSet(target))
                worklist.append(target);
        }
    }
}

// What a field of a layout holds has a type of its own wherever a body reads it, so no layout is marked for the types of its fields.
void RecordUsers::addRecordsNamingMarkedRecords()
{
    Node firstLayoutSet = nodeOf(LayoutSetID(0u));
    Node firstType = nodeOf(TypeID(0u));
    addMarksOfSources(0, firstLayoutSet, [&](Node node, const ScopedLambda<void(Node)>& functor) {
        forEachInheritedLayout(LayoutID(node), functor);
    });
    addMarksOfSources(0, firstType, [&](Node node, const ScopedLambda<void(Node)>& functor) {
        if (node < firstLayoutSet)
            forEachLayoutStoodFor(LayoutID(node), functor);
        else
            forEachSourceOf(m_program.at(LayoutSetID(node - firstLayoutSet)), functor);
    });

    // The marks of the layouts and the sets stand now.
    auto addMarksOfSourcesTo = [&](unsigned index, bool& hasContents) {
        Mark known = m_types.at(index);
        Mark mark = known;
        forEachSourceOf(m_program.at(TypeID(index)), [&](Node source) {
            mark |= markAt(source);
            hasContents |= source >= firstType;
        });
        if (mark == known)
            return false;
        m_types.add(index, mark);
        return true;
    };
    Vector<uint32_t> typesWithContents = collectInParallel<uint32_t>(m_loops, m_types.size(), [&](unsigned index, Vector<uint32_t>& found) {
        bool hasContents = false;
        addMarksOfSourcesTo(index, hasContents);
        if (hasContents)
            found.append(index);
    });
    for (std::atomic<bool> didAdd = true; didAdd.exchange(false, std::memory_order_relaxed);) {
        forEachIndexInParallel(m_loops, typesWithContents.size(), [&](unsigned position) {
            bool hasContents = false;
            if (addMarksOfSourcesTo(typesWithContents[position], hasContents))
                didAdd.store(true, std::memory_order_relaxed);
        });
    }
}

// Whoever goes from an object to all that it reaches reads the record of each layout on the way, and goes on wherever that record says.
void RecordUsers::addRecordsLeadingToMarkedRecords()
{
    // No variable gets a mark from anything, so what the variables in a record give the layout is there from the start.
    forEachIndexInParallel(m_loops, m_layouts.size(), [&](unsigned index) {
        Mark mark = 0;
        forEachVariable(static_cast<const LayoutRecord&>(m_program.at(LayoutID(index))), [&](VariableID id) {
            mark |= m_variables.at(id.index());
        });
        if (mark)
            m_layouts.add(index, mark);
    });

    Node firstLayoutSet = nodeOf(LayoutSetID(0u));
    Node firstType = nodeOf(TypeID(0u));
    addMarksOfSources(0, numberOfNodes(), [&](Node node, const ScopedLambda<void(Node)>& functor) {
        if (node >= firstType) {
            forEachSourceOf(m_program.at(TypeID(node - firstType)), functor);
            return;
        }
        if (node >= firstLayoutSet) {
            forEachSourceOf(m_program.at(LayoutSetID(node - firstLayoutSet)), functor);
            return;
        }
        SourcesInRecord sources { *this, functor };
        forEachReference(static_cast<const LayoutRecord&>(m_program.at(LayoutID(node))), sources);
        forEachLayoutStoodFor(LayoutID(node), functor);
    });
}

Vector<FunctionID> RecordUsers::findFunctionsWhere(const Invocable<bool(unsigned)> auto& isFound) const
{
    return collectInParallel<FunctionID>(m_loops, m_program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        if (isFound(index))
            found.append(FunctionID(index));
    });
}

Vector<FunctionID> findFunctionsSeeing(const Program& program, const ChangedRecords& changed, std::span<const uint8_t> followsFieldsOfOperands, std::span<const uint8_t> isAsked, const ParallelLoops& loops)
{
    RELEASE_ASSERT(isAsked.size() == program.functions.size());
    if (std::ranges::none_of(isAsked, std::identity { }))
        return { };
    RecordUsers users(program, loops);
    for (LayoutID id : changed.layouts)
        users.add(id);
    users.add(changed.fields);
    for (VariableID id : changed.variables)
        users.add(id);
    auto followsFields = [&](unsigned index) {
        return index < followsFieldsOfOperands.size() && followsFieldsOfOperands[index] && isAsked[index];
    };

    bool hasChangedLayouts = !changed.layouts.empty() || !changed.fields.empty();
    if (!hasChangedLayouts) {
        return users.findFunctionsWhere([&](unsigned index) {
            return isAsked[index] && users.namesMarkedVariable(FunctionID(index));
        });
    }
    bool someFollowFields = false;
    for (unsigned index = 0; index < followsFieldsOfOperands.size() && !someFollowFields; ++index)
        someFollowFields = followsFields(index);

    users.addRecordsNamingMarkedRecords();
    Vector<FunctionID> functions = users.findFunctionsWhere([&](unsigned index) {
        return isAsked[index] && (!someFollowFields || !followsFields(index)) && users.seesMarkedRecord(FunctionID(index));
    });
    if (!someFollowFields)
        return functions;

    users.addRecordsLeadingToMarkedRecords();
    functions.appendVector(users.findFunctionsWhere([&](unsigned index) {
        return followsFields(index) && users.namesMarkedRecord(FunctionID(index));
    }));
    std::ranges::sort(functions);
    return functions;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
