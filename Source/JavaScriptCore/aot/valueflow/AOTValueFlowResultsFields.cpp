/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTClaimsOfFields.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTSharedMemo.h"
#include "AOTThreshold.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowTypedResults.h"
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

void ValueFlowUsesOfHeapDeleter::operator()(ValueFlowUsesOfHeap* usesOfHeap) const
{
    delete usesOfHeap;
}

std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> computeUsesOfHeap(const ValueFlowResultInputs& inputs)
{
    return std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter>(new ValueFlowUsesOfHeap(inputs));
}

ValueFlowHeapView& heapViewOf(ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.heap;
}

const LedgerPart& ledgerOfFieldsIn(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return *usesOfHeap.ledgerOfFields;
}

ValueSetID objectsThatMayBeEnumerated(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.uses.objectsThatMayBeEnumerated();
}

ValueSetID objectsWithAllKeysShown(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.uses.objectsWithAllKeysShown();
}

ValueSetID objectsWithEnumerableKeysShown(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.uses.objectsWithEnumerableKeysShown();
}

void listStoresByLayout(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    usesOfHeap.layoutsOfObjects.emplace(inputs, usesOfHeap.heap);
    times.didFinish("fields: layouts of objects"_s);
    usesOfHeap.stores.emplace(inputs, tables, usesOfHeap.heap, *usesOfHeap.layoutsOfObjects);
    usesOfHeap.ledgerOfFields.emplace(inputs.program);
}

static FieldFlow& flowOf(FlowApplication& application, FieldReference declaration)
{
    return application.layouts[declaration.layout.index()].fields[declaration.field].flow;
}

static LayoutID findOnProvenChain(const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<bool(LayoutID)> auto& isWanted)
{
    for (size_t steps = 0; id && steps < application.layouts.size(); ++steps, id = provenBaseOf(application, id)) {
        if (isWanted(id))
            return id;
    }
    return { };
}

static void forEachOnProvenChain(const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    for (size_t steps = 0; id && steps < application.layouts.size(); ++steps, id = provenBaseOf(application, id))
        functor(id);
}

AOT_THRESHOLD(size_t, maxLayoutsBelowAskedOneByOne, 8);

static void forEachFieldOfObjectsWith(const Program& program, const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<void(FieldReference declaration, const Field&)> auto& functor)
{
    Vector<LayoutID, maxLayoutsBelowAskedOneByOne + 1> layoutsBelow;
    Vector<FieldReference, 16> visited;
    bool isLongRow = false;
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keysBelow;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visitedInLongRow;
    auto numberOf = [](FieldReference declaration) {
        return static_cast<uint64_t>(declaration.layout.index()) << 32 | declaration.field;
    };
    auto addKeysOf = [&](const Layout& layout) {
        for (const Field& field : layout.fields) {
            if (field.key)
                keysBelow.add(field.key.index());
        }
    };
    forEachOnProvenChain(application, id, [&](LayoutID current) {
        const Layout& layout = program.at(current);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            bool isHidden = field.key && (isLongRow ? keysBelow.contains(field.key.index()) : std::ranges::any_of(layoutsBelow, [&](LayoutID below) {
                return program.at(below).findField(field.key).has_value();
            }));
            if (isHidden)
                continue;
            FieldReference declaration = declarationOf(program, { current, index });
            if (isLongRow) {
                if (!visitedInLongRow.add(numberOf(declaration)).isNewEntry)
                    continue;
            } else {
                if (!layoutsBelow.isEmpty() && visited.contains(declaration))
                    continue;
                visited.append(declaration);
            }
            functor(declaration, field);
        }
        if (isLongRow) {
            addKeysOf(layout);
            return;
        }
        layoutsBelow.append(current);
        if (layoutsBelow.size() <= maxLayoutsBelowAskedOneByOne)
            return;
        isLongRow = true;
        for (LayoutID below : layoutsBelow)
            addKeysOf(program.at(below));
        for (FieldReference declaration : visited)
            visitedInLongRow.add(numberOf(declaration));
    });
}

static Vector<uint32_t> numberFields(const Program& program)
{
    Vector<uint32_t> firstFieldOfLayout(FillWith { }, program.layouts.size() + 1, 0u);
    for (uint32_t index = 0; index < program.layouts.size(); ++index)
        firstFieldOfLayout[index + 1] = firstFieldOfLayout[index] + program.layouts[index].fields.size();
    return firstFieldOfLayout;
}

static Vector<uint32_t, 16> fieldsFilledBy(const Program& program, const Layout& declared, const DataNode::Object& object)
{
    Vector<uint32_t, 16> fields;
    for (const DataMember& member : object.members) {
        if (std::optional<uint32_t> field = declared.findField(member.key))
            fields.append(*field);
        else if (program.nameClassOf(member.key) != NameClass::Index)
            break;
    }
    return fields;
}

static void uniteAccessors(FlowAccessorField& known, FlowAccessorField&& other, bool isFirst)
{
    if (isFirst) {
        known = WTF::move(other);
        return;
    }
    if (known.form != other.form) {
        known = { };
        return;
    }
    for (FunctionID setter : other.setters)
        insertInUniquedSortedVector(known.setters, setter);
    if (known.getters.isEmpty() || other.getters.isEmpty())
        known.getters.clear();
    else {
        for (FunctionID getter : other.getters)
            insertInUniquedSortedVector(known.getters, getter);
    }
}

namespace {

using KeysByLayout = Vector<Vector<PropertyKeyID>>;
using LayoutsByKey = HashMap<uint32_t, Vector<LayoutID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

struct AllocatingOperation {
    FunctionID function;
    ValueID value;
    bool getsMoreFields { false };
};

struct CopyAtBirth {
    ValueID operation;
    uint32_t firstSource { 0 };
    bool assigns { false };
};

struct SourceOfCopyAtBirth {
    ValueSetID objects { ValueSets::nothing };
    bool isAssigned { false };
    std::span<const PropertyKeyID> excludedKeys;
};

// `{ ...defaults, ...options }`, `Object.assign({}, defaults, options)` and `{ a: first(), b: second() }`: nothing but the copies and the definitions can reach the object until they are over.
struct BirthWithCopies {
    FunctionID function;
    ValueID allocation;
    LayoutID layout;
    bool definesKeysLeftOutByAllocation { false };
    bool hasOnlyNamedData { true };
    bool startsWithObjectPrototype { true };
    Vector<PropertyKeyID, 8> definedKeys;
    Vector<CopyAtBirth, 2> copies;
    Vector<SourceOfCopyAtBirth, 2> sources;
};

enum class FieldFact : uint8_t {
    HasObjects = 1 << 0,
    IsMissingAtCreation = 1 << 1,
    MayBeObserved = 1 << 2,
    MayBeCalled = 1 << 3,
    MayBeAccessed = 1 << 4,
    IsStoredAfterCreation = 1 << 5,
    IsAskedForAccessor = 1 << 6,
    MayInheritName = 1 << 7,
};

struct NamesThroughPlaceholders {
    static constexpr uint32_t none = UINT32_MAX;

    void note(const StoresByLayout::PlaceholdersInSet& placeholders, uint32_t name)
    {
        bool isTaken[] { placeholders.unknownObject, placeholders.untrackedObject, placeholders.almostEverything };
        for (unsigned way = 0; way < 3; ++way) {
            if (isTaken[way] && name != none)
                names[way].set(name);
            mayBeAnyName[way] |= isTaken[way] && name == none;
        }
    }

    void takeFrom(const NamesThroughPlaceholders& other)
    {
        for (unsigned way = 0; way < 3; ++way) {
            names[way].merge(other.names[way]);
            mayBeAnyName[way] |= other.mayBeAnyName[way];
        }
    }

    bool reach(const StoresByLayout& stores, LayoutID layout, uint32_t key) const
    {
        bool isOpen[] { stores.hasEscapedObject(layout), stores.hasLostObject(layout), true };
        for (unsigned way = 0; way < 3; ++way) {
            if (isOpen[way] && (mayBeAnyName[way] || (key == none ? !names[way].isEmpty() : names[way].get(key))))
                return true;
        }
        return false;
    }

    std::array<BitVector, 3> names;
    std::array<bool, 3> mayBeAnyName { };
};

struct FieldResults {
    const ValueFlowResultInputs& inputs;
    FlowResultTables& tables;
    const ValueFlowConstructions& constructions;
    ValueFlowHeapView& heap;
    const UsesOfObjects& uses;
    const LayoutsOfObjects& layoutsOfObjects;
    StoresByLayout& stores;
    LedgerOfFields& ledger;
    const Program& program { inputs.program };
    const ParallelLoops& loops { inputs.loops() };
    BuildContext& build { inputs.build };
    const ValueSets& sets { inputs.sets() };
    FlowApplication& application { *tables.application };
    uint32_t numberOfLayouts { static_cast<uint32_t>(program.layouts.size()) };
    Vector<Vector<LayoutID>> layoutsDirectlyBelow { };
    mutable Lock lockOfChainsBuiltByProgram { };
    // For the layouts of literals that are born with copies: the keys that every object holds once its birth is over, sorted.
    KeysByLayout keysHeldSinceBirth { };
    Vector<std::pair<FunctionID, ValueID>> allocationsBornWithCopies { };
    BitVector isBornWithCopies { };
    BitVector isBornWithAssignedCopies { };
    mutable SharedMemo<KindsOfMembers> kindsOfMembers { };
    struct AnswersAboutObject {
        OptionSet<Actor> open;
        bool isMadeByRowThatNamesLayoutOrByOperation { false };
    };
    // For the layouts that have objects.
    Vector<OptionSet<Actor>> actorsOpenOnObjectsOfLayouts { };
    // By layout and key, where the receiver of a definition is only observed to be an object of the layout: ValueForms. A store by name counts as the plain form.
    mutable UncheckedKeyHashMap<uint64_t, uint8_t> formsOfValuesSeenDefined { };

    ValueSetID chainBuiltByProgram(ValueSetID objects) const
    {
        Locker locker { lockOfChainsBuiltByProgram };
        return heap.chainBuiltByProgram(objects);
    }

    bool isCompiled(FunctionID function) const { return inputs.isReached(function) || isUnsolvedCode(inputs, tables, function); }
    bool hasObjects(LayoutID id) const { return hasObjectsThatCodeAlters(program.at(id).kind) && application.layouts[id.index()].flow.allocation != AllocationState::NeverAllocated; }
    OptionSet<ObjectMutation> possibleMutationsOf(LayoutID id) const
    {
        const LayoutFlow& flow = application.layouts[id.index()].flow;
        return flow.allocationClasses.isEmpty() ? allObjectMutations : allObjectMutations - flow.impossibleMutations;
    }
    bool isSeenByFollowedCodeAlone(LayoutID id) const { return !possibleMutationsOf(id).containsAny(mutationsThatReach) && !stores.hasObjectOfCodeWithoutSolve(id); }

    bool holdsField(LayoutID id, PropertyKeyID key, const KeysByLayout* appendedKeys = nullptr) const
    {
        return !!findOnProvenChain(application, id, [&](LayoutID current) {
            return program.at(current).findField(key) || (appendedKeys && std::ranges::binary_search((*appendedKeys)[current.index()], key));
        });
    }

    // holdsField() for many keys of one layout.
    class KeysOfFieldsHeld {
    public:
        KeysOfFieldsHeld(const FieldResults& results, LayoutID id, const KeysByLayout* appendedKeys = nullptr)
            : m_results(results)
            , m_id(id)
            , m_appendedKeys(appendedKeys)
        {
        }

        bool contains(PropertyKeyID key)
        {
            if (!m_isLongRow) {
                size_t length = 0;
                m_isLongRow = !!findOnProvenChain(m_results.application, m_id, [&](LayoutID) {
                    return ++length > maxLayoutsBelowAskedOneByOne;
                });
                if (*m_isLongRow)
                    fill();
            }
            return *m_isLongRow ? m_keys.contains(key.index()) : m_results.holdsField(m_id, key, m_appendedKeys);
        }

    private:
        void fill()
        {
            forEachOnProvenChain(m_results.application, m_id, [&](LayoutID current) {
                for (const Field& field : m_results.program.at(current).fields) {
                    if (field.key)
                        m_keys.add(field.key.index());
                }
                if (!m_appendedKeys)
                    return;
                for (PropertyKeyID key : (*m_appendedKeys)[current.index()])
                    m_keys.add(key.index());
            });
        }

        const FieldResults& m_results;
        LayoutID m_id;
        const KeysByLayout* m_appendedKeys;
        std::optional<bool> m_isLongRow;
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keys;
    };

    void forEachLayoutBelow(LayoutID layout, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        Vector<LayoutID, 8> worklist { layout };
        while (!worklist.isEmpty()) {
            LayoutID current = worklist.takeLast();
            functor(current);
            worklist.appendVector(layoutsDirectlyBelow[current.index()]);
        }
    }

    Vector<LayoutID> layoutsOfOtherSites(NOESCAPE const Invocable<bool(ValueFlowObjectID, const ValueFlowSite&)> auto& counts) const
    {
        const ValueFlowUniverse& universe = inputs.universe();
        return collectInParallel<LayoutID>(loops, universe.numberOfSiteObjects(), [&](unsigned position, Vector<LayoutID>& layouts) {
            ValueFlowObjectID object = universe.siteObjectAt(position);
            if (!universe.layoutOf(object) || isSiteOfTypedResultThatNothingAllocates(inputs, object))
                return;
            const ValueFlowSite& site = *universe.siteOf(object);
            bool isAllocation = site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject;
            if (!isAllocation && site.origin != ValueFlowSiteOrigin::DataNode && counts(object, site))
                layouts.append(site.layout);
        });
    }

    Vector<AllocatingOperation> collectAllocatingOperations() const
    {
        return collectInParallel<AllocatingOperation>(loops, program.functions.size(), [&](unsigned index, Vector<AllocatingOperation>& operations) {
            if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::Data })) || !isCompiled(FunctionID(index)))
                return;
            const Function& function = program.functions[index];
            size_t numberOfOtherOperations = operations.size();
            for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
                Opcode opcode = function.values[valueIndex].opcode;
                if (opcode == Opcode::NewObject || opcode == Opcode::Data)
                    operations.append({ FunctionID(index), ValueID(valueIndex) });
            }
            for (const Value& value : function.values) {
                if (value.opcode != Opcode::PropDefine || !value.key())
                    continue;
                ValueID object = value.operand(OperandIndex::PropDefine::object);
                const Value& allocation = function.at(object);
                if (allocation.opcode != Opcode::NewObject)
                    continue;
                std::optional<uint32_t> field = program.at(declaredLayoutOf(program, allocation.layout())).findField(value.key());
                if (!field || !std::ranges::contains(allocation.fields(), *field))
                    std::ranges::lower_bound(operations.mutableSpan().subspan(numberOfOtherOperations), object, { }, &AllocatingOperation::value)->getsMoreFields = true;
            }
        });
    }

    bool holdsSinceBirth(LayoutID id, PropertyKeyID key) const
    {
        return key && isBornWithCopies.get(id.index()) && std::ranges::binary_search(keysHeldSinceBirth[id.index()], key);
    }

    bool canBeBornWithCopies(LayoutID id) const
    {
        return program.at(id).kind == LayoutKind::Record && !program.at(id).isDerived();
    }

    void collectBirthsWithCopiesIn(FunctionID id, Vector<BirthWithCopies>& births) const
    {
        const Function& function = program.at(id);
        const BuiltinTable& builtins = program.builtins();
        bool hasPieces = false;
        bool namesAssign = false;
        for (const Value& value : function.values) {
            hasPieces |= value.opcode == Opcode::CopyDataProperties || value.opcode == Opcode::PropDefine;
            namesAssign |= value.opcode == Opcode::Builtin && builtins.canonicalID(value.builtin()) == Builtins::ObjectAssign;
        }
        if (!hasPieces && !namesAssign)
            return;

        constexpr uint32_t none = UINT32_MAX;
        struct BornObject {
            BirthWithCopies birth;
            ValueID object;
        };
        Vector<BornObject, 8> born;
        // The keys of a creation site that defines many, by its index in `born`: a literal may define thousands, and each definition looks them up.
        struct ListedKeys {
            UncheckedKeyHashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keys;
            size_t numberListed { 0 };
        };
        constexpr size_t maxKeysForLinearSearch = 16;
        Vector<ListedKeys> keysOfLongBirths;
        auto defines = [&](uint32_t open, PropertyKeyID key) {
            const auto& definedKeys = born[open].birth.definedKeys;
            if (definedKeys.size() <= maxKeysForLinearSearch)
                return definedKeys.contains(key);
            if (keysOfLongBirths.size() <= open)
                keysOfLongBirths.grow(open + 1);
            ListedKeys& listed = keysOfLongBirths[open];
            for (; listed.numberListed < definedKeys.size(); ++listed.numberListed)
                listed.keys.add(definedKeys[listed.numberListed].index());
            return listed.keys.contains(key.index());
        };
        Vector<uint32_t> openBirthOf(FillWith { }, function.values.size(), none);
        Vector<ValueID, 8> openInBlock;
        for (const Block& block : function.blocks) {
            for (ValueID valueID : block.values) {
                const Value& value = function.at(valueID);
                for (ValueID operand : value.operands()) {
                    uint32_t open = operand ? openBirthOf[operand.index()] : none;
                    if (open == none)
                        continue;
                    BirthWithCopies& birth = born[open].birth;
                    bool isNamedOnce = std::ranges::count(value.operands(), operand) == 1;
                    // A store to a data property that the object has of its own runs no code.
                    if (value.opcode == Opcode::PropSet && isNamedOnce && value.operand(OperandIndex::PropSet::object) == operand && birth.hasOnlyNamedData && value.key() && defines(open, value.key()))
                        continue;
                    if (!isPieceOfLiteral(value, operand) || !isNamedOnce) {
                        if (value.opcode == Opcode::FinishObject)
                            born[open].object = valueID;
                        openBirthOf[operand.index()] = none;
                        continue;
                    }
                    if (value.opcode == Opcode::PropDefine && value.key()) {
                        birth.definesKeysLeftOutByAllocation |= !defines(open, value.key());
                        birth.definedKeys.append(value.key());
                    } else
                        birth.hasOnlyNamedData = false;
                    if (value.opcode == Opcode::CopyDataProperties && value.excludedOperands().empty())
                        birth.copies.append({ valueID, OperandIndex::CopyDataProperties::source, false });
                }
                if (value.opcode == Opcode::NewObject && canBeBornWithCopies(value.layout())) {
                    BirthWithCopies birth { .function = id, .allocation = valueID, .layout = value.layout(), .startsWithObjectPrototype = !value.prototypeOperand() };
                    for (uint32_t field : value.fields()) {
                        if (PropertyKeyID key = program.at(birth.layout).fields[field].key)
                            birth.definedKeys.append(key);
                    }
                    openBirthOf[valueID.index()] = born.size();
                    openInBlock.append(valueID);
                    born.append({ WTF::move(birth), valueID });
                } else if (value.opcode == Opcode::Data && namesAssign) {
                    const DataNode::Object* root = program.at(value.data()).root.tryAs<DataNode::Object>();
                    if (!root || !root->layout || !canBeBornWithCopies(root->layout))
                        continue;
                    BirthWithCopies birth { .function = id, .allocation = valueID, .layout = root->layout };
                    for (uint32_t field : fieldsFilledBy(program, program.at(birth.layout), *root))
                        birth.definedKeys.append(program.at(birth.layout).fields[field].key);
                    born.append({ WTF::move(birth), valueID });
                }
            }
            for (ValueID open : openInBlock)
                openBirthOf[open.index()] = none;
            openInBlock.shrink(0);
        }

        Vector<uint32_t> numberOfUses;
        Vector<ValueID> userOf;
        if (namesAssign) {
            numberOfUses.fill(0, function.values.size());
            userOf.fill(ValueID(), function.values.size());
            for (const Block& block : function.blocks) {
                for (ValueID user : block.values) {
                    for (ValueID operand : function.at(user).operands()) {
                        if (!operand)
                            continue;
                        ++numberOfUses[operand.index()];
                        userOf[operand.index()] = user;
                    }
                }
                block.forEachChildOutsideValues([&](ValueID operand) {
                    if (operand)
                        numberOfUses[operand.index()] += 2;
                });
            }
        }
        for (auto& [birth, object] : born) {
            while (namesAssign && numberOfUses[object.index()] == 1) {
                ValueID user = userOf[object.index()];
                const Value& call = function.at(user);
                if (call.opcode != Opcode::Call || call.numberOfOperands() <= OperandIndex::Call::arguments + 1 || call.operand(OperandIndex::Call::arguments) != object)
                    break;
                const Value& callee = function.at(call.operand(OperandIndex::Call::callee));
                if (callee.opcode != Opcode::Builtin || builtins.canonicalID(callee.builtin()) != Builtins::ObjectAssign)
                    break;
                birth.copies.append({ user, OperandIndex::Call::arguments + 1, true });
                object = user;
            }
            if (!birth.copies.isEmpty() || birth.definesKeysLeftOutByAllocation)
                births.append(WTF::move(birth));
        }
        if (births.isEmpty() || births.last().function != id)
            return;

        size_t first = births.size();
        while (first && births[first - 1].function == id)
            --first;
        struct CopyOfBirth {
            ValueID operation;
            uint32_t birth;
            uint32_t copy;
        };
        Vector<CopyOfBirth, 8> copies;
        for (size_t birth = first; birth < births.size(); ++birth) {
            for (uint32_t copy = 0; copy < births[birth].copies.size(); ++copy)
                copies.append({ births[birth].copies[copy].operation, static_cast<uint32_t>(birth), copy });
        }
        AOT::sort(copies, { }, &CopyOfBirth::operation);
        BorrowedValueSetMaker maker(inputs.makerPool());
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            auto found = std::ranges::lower_bound(copies, statement.site, { }, &CopyOfBirth::operation);
            if (found == copies.end() || found->operation != statement.site)
                continue;
            BirthWithCopies& birth = births[found->birth];
            const CopyAtBirth& copy = birth.copies[found->copy];
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            uint32_t end = copy.assigns ? operands.size() : copy.firstSource + 1;
            std::span<const PropertyKeyID> excludedKeys = copy.assigns ? std::span<const PropertyKeyID>() : function.at(copy.operation).excludedKeys();
            // A pattern has thrown for undefined and null before it copies the rest.
            bool mayBeNullish = copy.assigns || program.typeOf(function, function.at(copy.operation).sourceOperand()).mayBeUndefinedOrNull();
            for (uint32_t operand = copy.firstSource; operand < end && operand < operands.size(); ++operand) {
                ValueSetID objects = heap.possibleValuesOf(maker.get(), operands[operand]);
                if (!mayBeNullish)
                    objects = maker.get().intersect(objects, ValueSets::complement(maker.get().of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) })));
                birth.sources.append({ objects, copy.assigns, excludedKeys });
            }
        }
    }

    // What a literal without a copy lists. A copy that is born under its layout brings what its own sources hold.
    std::optional<Vector<PropertyKeyID>> keysListedByLiteral(ValueFlowObjectID object, LayoutID id) const
    {
        const ValueFlowUniverse& universe = inputs.universe();
        const ValueFlowSite* site = universe.siteOf(object);
        if (!site || site->layout != id)
            return std::nullopt;
        FunctionID function = universe.functionOf(object);
        const Value& operation = program.at(function).at(site->value);
        bool isFirstOfOperation = !site->ordinal;
        if (isFirstOfOperation && std::ranges::binary_search(allocationsBornWithCopies, std::pair { function, site->value }))
            return std::nullopt;
        Vector<PropertyKeyID> keys;
        auto list = [&](std::span<const uint32_t> fields) {
            for (uint32_t field : fields) {
                if (PropertyKeyID key = program.at(id).fields[field].key)
                    keys.append(key);
            }
        };
        if (site->origin == ValueFlowSiteOrigin::Operation && isFirstOfOperation && operation.opcode == Opcode::NewObject) {
            list(operation.fields());
            return keys;
        }
        if (site->origin != ValueFlowSiteOrigin::DataNode)
            return std::nullopt;
        forEachDataNodeInPreorder(program.at(operation.data()).root, [&](uint32_t ordinal, const DataNode& node) {
            if (ordinal == site->ordinal)
                list(fieldsFilledBy(program, program.at(id), node.as<DataNode::Object>()).span());
        });
        return keys;
    }

    // The own enumerable data properties that the object is never without, once it is born. What happens to another object of its layout takes none from it.
    Vector<PropertyKeyID> keysKeptBy(ValueSetMaker& maker, const BitVector& hasOtherSites, ValueFlowObjectID object, LayoutID id) const
    {
        Vector<PropertyKeyID> keys;
        LayoutKind kind = program.at(id).kind;
        ObjectsOfLayout reached = inputs.hasEscaped(object) || inputs.isLost(object) ? ObjectsOfLayout::All : ObjectsOfLayout::Followed;
        bool getsCopiedProperties = stores.getsCopiedProperties(id, reached) && (reached == ObjectsOfLayout::All || heap.writtenUnder(maker, maker.of(object), copiedFromName) != ValueSets::nothing);
        if ((kind != LayoutKind::Record && kind != LayoutKind::Instance) || !hasObjects(id) || getsCopiedProperties || stores.hasObjectOfCodeWithoutSolve(id))
            return keys;
        OptionSet<ObjectMutation> possible = reached == ObjectsOfLayout::All ? possibleMutationsOf(id) : markedMutationsOf(inputs, object);
        if (possible.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ReachUnknownJavaScript }))
            return keys;
        if (kind == LayoutKind::Record ? hasOtherSites.quickGet(id.index()) : !constructions.isFollowed(id))
            return keys;

        bool isBornHere = isBornWithCopies.get(id.index());
        if (isBornHere) {
            std::optional<Vector<PropertyKeyID>> listed = reached == ObjectsOfLayout::Followed ? keysListedByLiteral(object, id) : std::nullopt;
            keys = listed ? WTF::move(*listed) : keysHeldSinceBirth[id.index()];
        }
        Vector<PropertyKeyID> keysOfOtherForms;
        forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
            if (!field.key)
                return;
            if (field.computedKey || field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden }) || field.form.containsAny({ FieldForm::IsNotEnumerable, FieldForm::IsAccessor }))
                keysOfOtherForms.append(field.key);
            else if (!isBornHere && (kind == LayoutKind::Record ? constructions.isStoredAtBirth(id, field.key, declaration) : constructions.isStoredBeforeObjectLeaks(id, field.key, declaration)))
                keys.append(field.key);
        });
        AOT::sort(keys);
        removeRepeatedElements(keys);
        AOT::sort(keysOfOtherForms);
        keys.removeAllMatching([&](PropertyKeyID key) {
            return std::ranges::binary_search(keysOfOtherForms, key) || stores.mayGetAccessorUnder(id, key, reached) || stores.accessorDefinedUnder(maker, id, key).isAccessor();
        });
        return keys;
    }

    Vector<PropertyKeyID> keysSurelyCopiedFrom(ValueSetMaker& maker, const BitVector& hasOtherSites, ValueSetID objects) const
    {
        AOT_THRESHOLD(uint32_t, mostSourcesOfOneCopy, 8);
        const ValueFlowUniverse& universe = inputs.universe();
        if (ValueSets::isComplement(objects) || sets.isEmpty(objects) || sets.size(objects) > mostSourcesOfOneCopy || sets.meet(objects, universe.placeholdersAlone()) || sets.meet(objects, universe.primitives()))
            return { };
        std::optional<Vector<PropertyKeyID>> keys;
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            bool hasLayout = false;
            layoutsOfObjects.forEachLayoutOf(object, [&](LayoutID layout) {
                hasLayout = true;
                Vector<PropertyKeyID> kept = keysKeptBy(maker, hasOtherSites, object, layout);
                if (!keys) {
                    keys = WTF::move(kept);
                    return;
                }
                keys->removeAllMatching([&](PropertyKeyID key) {
                    return !std::ranges::binary_search(kept, key);
                });
            });
            if (!hasLayout || inputs.classOf(object))
                keys = Vector<PropertyKeyID>();
        });
        return keys.value_or(Vector<PropertyKeyID>());
    }

    // An assignment to such a name creates an own property on an object that inherits from Object.prototype.
    bool holdsOnlyNamesAbsentOnObjectPrototype(ValueSetMaker& maker, ValueSetID objects) const
    {
        const ValueFlowUniverse& universe = inputs.universe();
        if (ValueSets::isComplement(objects) || sets.meet(objects, universe.placeholdersAlone()))
            return false;
        ValueSetID objectPrototype = maker.of(universe.objectOf(Builtins::ObjectPrototype));
        bool holdsOnlyThose = true;
        sets.forEachMember(maker.intersect(objects, universe.objects()), [&](ValueFlowObjectID object) {
            bool hasLayout = false;
            layoutsOfObjects.forEachLayoutOf(object, [&](LayoutID layout) {
                hasLayout = true;
                if (stores.getsNameNobodyLists(layout) || stores.hasObjectOfCodeWithoutSolve(layout) || possibleMutationsOf(layout).containsAny(mutationsThatReach)) {
                    holdsOnlyThose = false;
                    return;
                }
                forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference, const Field& field) {
                    holdsOnlyThose = holdsOnlyThose && field.key && !program.isIndexLike(field.key) && findsNoProperty(maker, objectPrototype, field.key);
                });
                for (PropertyKeyID key : stores.keysStoredInto(layout))
                    holdsOnlyThose = holdsOnlyThose && !program.isIndexLike(key) && findsNoProperty(maker, objectPrototype, key);
            });
            holdsOnlyThose &= hasLayout && !inputs.classOf(object);
        });
        return holdsOnlyThose;
    }

    // `{ delay: 50, ...options }`: a copy that may bring anything still brings what every source that is listed is born with. Those names get fields of which nothing else is said.
    void nameKeysOfListedSources(std::span<const BirthWithCopies> births)
    {
        AOT_THRESHOLD(uint32_t, mostListedSourcesThatNameFields, 8);
        BorrowedValueSetMaker maker(inputs.makerPool());
        auto isBornWith = [&](LayoutID source, PropertyKeyID key) {
            const Layout& layout = program.at(source);
            std::optional<uint32_t> field = layout.kind == LayoutKind::Record ? layout.findField(key) : std::nullopt;
            return field && layout.fields[*field].flags.contains(FieldFlags::IsRequired) && layout.fields[*field].form.isEmpty() && !stores.mayGetAccessorUnder(source, key);
        };
        BitVector isLookedAt(numberOfLayouts);
        for (const BirthWithCopies& birth : births) {
            if (birth.copies.isEmpty() || birth.definedKeys.isEmpty() || isLookedAt.set(birth.layout.index()) || !hasObjects(birth.layout) || !stores.getsCopiedProperties(birth.layout))
                continue;
            ValueSetID sources = heap.writtenUnder(maker.get(), layoutsOfObjects.objectsWith(birth.layout), copiedFromName);
            if (ValueSets::isComplement(sources) || sets.size(sources) > mostListedSourcesThatNameFields)
                continue;
            Vector<LayoutID, 4> layouts;
            sets.forEachMember(sources, [&](ValueFlowObjectID object) {
                layoutsOfObjects.forEachLayoutOf(object, [&](LayoutID layout) {
                    layouts.append(layout);
                });
            });
            if (layouts.isEmpty())
                continue;
            for (const Field& field : program.at(layouts[0]).fields) {
                if (!field.key || !std::ranges::all_of(layouts, [&](LayoutID layout) { return isBornWith(layout, field.key); }))
                    continue;
                for (LayoutID layout : layouts)
                    stores.noteKeyBroughtByCopies(birth.layout, field.key, stores.storedUnder(maker.get(), layout, field.key));
            }
        }
    }

    void followCopiesAtBirth(std::span<const AllocatingOperation> operations)
    {
        isBornWithCopies = BitVector(numberOfLayouts);
        isBornWithAssignedCopies = BitVector(numberOfLayouts);
        Vector<BirthWithCopies> births = collectInParallel<BirthWithCopies>(loops, program.functions.size(), [&](unsigned index, Vector<BirthWithCopies>& found) {
            FunctionID function(index);
            if (inputs.isReached(function) && inputs.bodies().holdsAnyOf(function, notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::Data })) && inputs.bodies().holdsAnyOf(function, notedOperations({ ValueFlowNotedOperation::PieceOutsideSlots, ValueFlowNotedOperation::FinishObject, ValueFlowNotedOperation::Builtin })))
                collectBirthsWithCopiesIn(function, found);
        });
        if (births.isEmpty())
            return;
        nameKeysOfListedSources(births.span());

        BitVector hasOtherSites(numberOfLayouts);
        for (LayoutID layout : layoutsOfOtherSites([](ValueFlowObjectID, const ValueFlowSite&) { return true; })) {
            hasOtherSites.quickSet(layout.index());
            hasOtherSites.quickSet(declaredLayoutOf(program, layout).index());
        }
        births.removeAllMatching([&](const BirthWithCopies& birth) {
            return !hasObjects(birth.layout) || hasOtherSites.quickGet(birth.layout.index()) || stores.hasObjectOfCodeWithoutSolve(birth.layout) || !build.isEnabled(FlowResultKind::Initialization, birth.layout.index()) || !build.isEnabled(FlowResultKind::ConstructorFieldOrders, birth.layout.index());
        });
        keysHeldSinceBirth = KeysByLayout(numberOfLayouts);
        for (BirthWithCopies& birth : births) {
            isBornWithCopies.quickSet(birth.layout.index());
            if (std::ranges::any_of(birth.copies, &CopyAtBirth::assigns))
                isBornWithAssignedCopies.quickSet(birth.layout.index());
            AOT::sort(birth.definedKeys);
            removeRepeatedElements(birth.definedKeys);
        }

        // What the other allocations of those layouts list bounds what every object holds.
        Vector<std::optional<Vector<PropertyKeyID>>> keysOfOtherBirths(numberOfLayouts);
        auto keepCommonKeys = [](std::optional<Vector<PropertyKeyID>>& common, const auto& keys) {
            if (!common) {
                common = Vector<PropertyKeyID>(keys.span());
                return;
            }
            common->removeAllMatching([&](PropertyKeyID key) {
                return !std::ranges::binary_search(keys, key);
            });
        };
        auto isListed = [&](FunctionID function, ValueID allocation) {
            auto found = std::ranges::lower_bound(births, std::pair { function, allocation }, { }, [](const BirthWithCopies& birth) {
                return std::pair { birth.function, birth.allocation };
            });
            return found != births.end() && found->function == function && found->allocation == allocation;
        };
        AOT::sort(births, { }, [](const BirthWithCopies& birth) {
            return std::pair { birth.function, birth.allocation };
        });
        allocationsBornWithCopies = births.map([](const BirthWithCopies& birth) {
            return std::pair { birth.function, birth.allocation };
        });
        struct OtherBirth {
            LayoutID layout;
            Vector<PropertyKeyID, 8> keys;
        };
        for (const OtherBirth& birth : collectInParallel<OtherBirth>(loops, operations.size(), [&](unsigned index, Vector<OtherBirth>& found) {
            auto noteOtherBirth = [&](LayoutID layout, std::span<const uint32_t> fields) {
                Vector<PropertyKeyID, 8> keys;
                for (uint32_t field : fields) {
                    if (PropertyKeyID key = program.at(layout).fields[field].key)
                        keys.append(key);
                }
                AOT::sort(keys);
                found.append({ layout, WTF::move(keys) });
            };
            const AllocatingOperation& operation = operations[index];
            const Value& value = program.at(operation.function).at(operation.value);
            if (value.opcode == Opcode::NewObject) {
                if (isBornWithCopies.get(value.layout().index()) && !isListed(operation.function, operation.value))
                    noteOtherBirth(value.layout(), value.fields());
                return;
            }
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t position, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (object && object->layout && isBornWithCopies.get(object->layout.index()) && (position || !isListed(operation.function, operation.value)))
                    noteOtherBirth(object->layout, fieldsFilledBy(program, program.at(object->layout), *object).span());
            });
        }))
            keepCommonKeys(keysOfOtherBirths[birth.layout.index()], birth.keys);

        AOT_THRESHOLD(unsigned, mostRoundsOverCopies, 4);
        // The births of one layout stand together, in their order.
        Vector<uint32_t> birthsByLayout(births.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        AOT::sort(birthsByLayout, { }, [&](uint32_t index) {
            return std::pair { births[index].layout, index };
        });
        Vector<uint32_t> firstBirthOfLayout;
        for (uint32_t position = 0; position < birthsByLayout.size(); ++position) {
            if (!position || births[birthsByLayout[position]].layout != births[birthsByLayout[position - 1]].layout)
                firstBirthOfLayout.append(position);
        }
        unsigned numberOfLayoutsBornWithCopies = firstBirthOfLayout.size();
        firstBirthOfLayout.append(birthsByLayout.size());
        auto layoutBornWithCopiesAt = [&](unsigned position) {
            return births[birthsByLayout[firstBirthOfLayout[position]]].layout;
        };
        ValueSetMakers makers(inputs);
        Vector<ValueSetID> objectsAboveLayout(FillWith { }, numberOfLayoutsBornWithCopies, ValueSetID());
        auto holdWhatBirthsBring = [&](bool followsCopies) {
            Vector<Vector<PropertyKeyID>> held(numberOfLayoutsBornWithCopies);
            forEachIndexInParallel(loops, numberOfLayoutsBornWithCopies, [&](unsigned position, unsigned worker) {
                ValueSetMaker& maker = makers.of(worker);
                std::optional<Vector<PropertyKeyID>> common = keysOfOtherBirths[layoutBornWithCopiesAt(position).index()];
                for (uint32_t index : birthsByLayout.subspan(firstBirthOfLayout[position], firstBirthOfLayout[position + 1] - firstBirthOfLayout[position])) {
                    const BirthWithCopies& birth = births[index];
                    Vector<PropertyKeyID> keys(birth.definedKeys.span());
                    // Until a source with unknown keys has been assigned, the literal still has the prototype it was created with.
                    bool isJudgedAtCreation = stores.getsCopiedProperties(birth.layout);
                    bool inheritsFromObjectPrototype = birth.startsWithObjectPrototype;
                    for (const SourceOfCopyAtBirth& source : followsCopies ? birth.sources.span() : std::span<const SourceOfCopyAtBirth>()) {
                        if (source.isAssigned && isJudgedAtCreation)
                            inheritsFromObjectPrototype = inheritsFromObjectPrototype && holdsOnlyNamesAbsentOnObjectPrototype(maker, source.objects);
                        for (PropertyKeyID key : keysSurelyCopiedFrom(maker, hasOtherSites, source.objects)) {
                            if (std::ranges::contains(source.excludedKeys, key))
                                continue;
                            if (!source.isAssigned || (isJudgedAtCreation ? inheritsFromObjectPrototype : inheritsNothingUnder(maker, birth.layout, objectsAboveLayout[position], key)))
                                keys.append(key);
                        }
                    }
                    AOT::sort(keys);
                    removeRepeatedElements(keys);
                    keepCommonKeys(common, keys);
                }
                held[position] = WTF::move(*common);
            });
            bool hasChanged = false;
            for (unsigned position = 0; position < numberOfLayoutsBornWithCopies; ++position) {
                Vector<PropertyKeyID>& keys = keysHeldSinceBirth[layoutBornWithCopiesAt(position).index()];
                hasChanged |= keys != held[position];
                keys = WTF::move(held[position]);
            }
            return hasChanged;
        };
        auto settles = [&] {
            for (unsigned round = 0; round < mostRoundsOverCopies; ++round) {
                if (!holdWhatBirthsBring(true))
                    return true;
            }
            return false;
        };

        // A source is born before its copy. So what every birth brings where every source holds it is held by all of them: `{ ...state, label }` under the layout of `state` holds what the
        // literals of that layout list. The rounds begin with every name and take names away; only where they come to rest do they say anything.
        forEachIndexInParallel(loops, numberOfLayoutsBornWithCopies, [&](unsigned position) {
            LayoutID layout = layoutBornWithCopiesAt(position);
            Vector<PropertyKeyID>& keys = keysHeldSinceBirth[layout.index()];
            forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference, const Field& field) {
                if (field.key)
                    keys.append(field.key);
            });
            AOT::sort(keys);
            removeRepeatedElements(keys);
        });
        if (!settles()) {
            holdWhatBirthsBring(false);
            settles();
        }
        // `{ ...defaults, ...options }`: whatever the last spread adds, the names of the first are present.
        for (unsigned position = 0; position < numberOfLayoutsBornWithCopies; ++position) {
            LayoutID layout = layoutBornWithCopiesAt(position);
            if (!stores.getsCopiedProperties(layout))
                continue;
            ValueSetMaker& maker = makers.of(0);
            ValueSetID listedSources = ValueSets::nothing;
            for (uint32_t index : birthsByLayout.subspan(firstBirthOfLayout[position], firstBirthOfLayout[position + 1] - firstBirthOfLayout[position])) {
                for (const SourceOfCopyAtBirth& source : births[index].sources) {
                    if (!ValueSets::isComplement(source.objects))
                        listedSources = maker.unite(listedSources, maker.intersect(source.objects, inputs.universe().knownObjects()));
                }
            }
            for (PropertyKeyID key : keysHeldSinceBirth[layout.index()])
                stores.noteKeyBroughtByCopies(layout, key, heap.writtenUnder(maker, listedSources, ValueFlowName::property(program, key)));
        }
    }

    // `{ ...defaults, ...options }.retries`: a spread whose sources are unknown may add the name, and then the read finds it in a field. A literal that writes names of its own is
    // expected to hold those and no others.
    void nameKeysReadFromTargetsOfCopies()
    {
        BorrowedValueSetMaker maker(inputs.makerPool());
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            LayoutID id(index);
            if (program.at(id).kind != LayoutKind::Record || !program.at(id).fields.isEmpty() || !isBornWithCopies.get(index))
                continue;
            if (!hasObjects(id) || !stores.getsCopiedProperties(id) || stores.hasObjectOfCodeWithoutSolve(id) || stores.keysReadFrom(id).empty())
                continue;
            // A name that an object on the prototype chain is observed to hold is treated as a read of that object.
            ValueSetID above;
            bool mayInheritFromAnything = possibleMutationsOf(id).contains(ObjectMutation::ChangePrototype) || ValueSets::isComplement(objectsAbove(maker.get(), id, above));
            ValueSetID listedAbove = mayInheritFromAnything ? maker.get().of(inputs.universe().objectOf(Builtins::ObjectPrototype)) : maker.get().intersect(above, ValueSets::complement(inputs.universe().placeholdersAlone()));
            for (PropertyKeyID key : stores.keysReadFrom(id)) {
                if (!program.isIndexLike(key) && !mayFindNameIn(maker.get(), listedAbove, key))
                    stores.noteKeyBroughtByCopies(id, key, ValueSets::nothing);
            }
        }
    }

    Vector<FieldReference> findFieldsMissingAtCreation(std::span<const AllocatingOperation> operations) const
    {
        auto appendFieldsNotAmong = [&](std::span<const uint32_t> listed, LayoutID id, Vector<FieldReference>& found) {
            for (uint32_t field = 0; field < program.at(id).fields.size(); ++field) {
                if (!std::ranges::contains(listed, field) && !holdsSinceBirth(id, program.at(id).fields[field].key))
                    found.append(declarationOf(program, { id, field }));
            }
        };
        Vector<FieldReference> missing = collectInParallel<FieldReference>(loops, numberOfLayouts, [&](unsigned index, Vector<FieldReference>& found) {
            LayoutID id(index);
            const Layout& layout = program.layouts[index];
            bool isFollowed = constructions.isFollowed(id);
            if (!hasObjects(id) || (!isFollowed && !layout.flags.containsAny({ LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction })))
                return;
            for (uint32_t field = 0; !isFollowed && field < layout.fields.size(); ++field) {
                if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                    found.append(declarationOf(program, { id, field }));
            }
            forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
                if (isFollowed ? !constructions.isStoredBeforeObjectLeaks(id, field.key, declaration) : declaration.layout != id && (constructions.isNewTargetOfOtherFunctions(id) || constructions.mayBeKeptFromCreatingInheritedField(id, field.key)))
                    found.append(declaration);
            });
        });
        missing.appendVector(collectInParallel<FieldReference>(loops, operations.size(), [&](unsigned position, Vector<FieldReference>& found) {
            const Value& value = program.at(operations[position].function).at(operations[position].value);
            if (value.opcode == Opcode::NewObject) {
                if (!program.at(value.layout()).isEnvironment() && !constructions.isFollowed(value.layout()))
                    appendFieldsNotAmong(value.fields(), value.layout(), found);
                return;
            }
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (object && object->layout && !constructions.isFollowed(object->layout))
                    appendFieldsNotAmong(fieldsFilledBy(program, program.at(object->layout), *object).span(), object->layout, found);
            });
        }));
        BitVector layoutsOfOtherObjects(numberOfLayouts);
        BitVector layoutsOfParsedObjects(numberOfLayouts);
        for (LayoutID layout : layoutsOfOtherSites([&](ValueFlowObjectID object, const ValueFlowSite& site) {
            return !constructions.isFollowed(site.layout) && hasObjects(site.layout) && site.origin != ValueFlowSiteOrigin::TypedResult && isCompiled(inputs.universe().functionOf(object));
        }))
            layoutsOfOtherObjects.quickSet(layout.index());
        for (LayoutID layout : layoutsOfOtherSites([&](ValueFlowObjectID, const ValueFlowSite& site) {
            return !constructions.isFollowed(site.layout) && hasObjects(site.layout) && site.origin == ValueFlowSiteOrigin::TypedResult;
        }))
            layoutsOfParsedObjects.quickSet(layout.index());
        for (size_t layout : layoutsOfOtherObjects) {
            forEachFieldOfObjectsWith(program, application, LayoutID(layout), [&](FieldReference declaration, const Field&) {
                missing.append(declaration);
            });
        }
        for (size_t layout : layoutsOfParsedObjects) {
            forEachFieldOfObjectsWith(program, application, LayoutID(layout), [&](FieldReference declaration, const Field& field) {
                if (mayBeMissingInParsedText(program, field))
                    missing.append(declaration);
            });
        }
        return missing;
    }

    bool mayFindNameIn(ValueSetMaker& maker, ValueSetID objects, PropertyKeyID key) const
    {
        return Typed::mayFindNameIn(inputs, heap, maker, objects, key, [&](ValueSetID members) {
            return kindsOfMembersOf(members);
        });
    }

    KindsOfMembers kindsOfMembersOf(ValueSetID objects) const
    {
        AOT_THRESHOLD(size_t, fewestMembersToRemember, 8);
        if (sets.size(objects) < fewestMembersToRemember)
            return findKindsOfMembersOf(inputs, objects);
        uint32_t hash = IntHash<uint32_t>::hash(objects.index());
        auto isWanted = [&](const KindsOfMembers& known) {
            return known.objects == objects;
        };
        if (const KindsOfMembers* known = kindsOfMembers.find(hash, isWanted))
            return *known;
        return kindsOfMembers.add(hash, findKindsOfMembersOf(inputs, objects), isWanted);
    }

    ValueSetID objectsAbove(ValueSetMaker& maker, LayoutID id, ValueSetID& above) const
    {
        if (!above)
            above = heap.chainOf(maker, heap.prototypesOf(maker, layoutsOfObjects.objectsWith(id)));
        return above;
    }

    bool inheritsNothingUnder(ValueSetMaker& maker, LayoutID id, ValueSetID& above, PropertyKeyID key) const
    {
        if (program.isIndexLike(key) || stores.hasObjectOfCodeWithoutSolve(id) || (stores.getsCopiedProperties(id) && possibleMutationsOf(id).contains(ObjectMutation::ChangePrototype)))
            return false;
        return findsNoProperty(maker, objectsAbove(maker, id, above), key);
    }

    bool findsNoProperty(ValueSetMaker& maker, ValueSetID above, PropertyKeyID key) const
    {
        if (mayFindNameIn(maker, above, key) || !actorsOpenOnNamesOfObjectsOfProgramAmong(inputs, maker, above).isEmpty())
            return false;
        RELEASE_ASSERT(tables.builtins.areFilled);
        bool mayGainName = false;
        sets.forEachMember(maker.intersect(above, inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
            BuiltinID holder = program.builtins().canonicalID(inputs.universe().builtinOf(object));
            if (!listsEveryMemberUnder(program, holder, key))
                mayGainName = true;
            else if (!isClosedKey(program, holder, key))
                mayGainName |= !builtinHolderNeverGains(tables.builtins, holder, key);
        });
        return !mayGainName;
    }

    // The constructor at the top makes what is constructed below other classes: an operation of the program, or a built-in class. Such a class has accounted for what it makes where it lists the own names of its instances, as does every class above it.
    bool makerAboveClassesHasAccountedFor(LayoutID id) const
    {
        bool hasAccounted = program.at(id).has(LayoutFlags::HasHeritage);
        forEachOnProvenChain(application, id, [&](LayoutID current) {
            const LayoutFlow& flow = application.layouts[current.index()].flow;
            switch (flow.baseState) {
            case BaseState::Layout:
            case BaseState::None:
                return;
            case BaseState::BuiltinClass:
                for (BuiltinClassID builtinClass = flow.baseClass; builtinClass; builtinClass = program.builtins().at(builtinClass).parent)
                    hasAccounted &= program.builtins().at(builtinClass).listsNamesDefinedOnNewInstances;
                return;
            case BaseState::Unknown:
                hasAccounted = false;
                return;
            }
        });
        return hasAccounted;
    }

    // A row that answers with objects of a layout lays the fields that the layout lists and nothing else. An operation of the program makes the others.
    bool isMadeByRowThatNamesLayoutOrByOperation(ValueFlowObjectID object) const
    {
        const ValueFlowSite* site = inputs.universe().siteOf(object);
        bool isMadeByRowThatNamesLayout = site && site->origin == ValueFlowSiteOrigin::TypedResult;
        return isMadeByRowThatNamesLayout || sets.contains(inputs.universe().builtinTables().heap().objectsWithoutPropertiesOfNativeCode(), object);
    }

    // What a built-in makes has from its makers the own names that the classes above it list.
    bool everyClassAboveListsOwnNames(ValueSetID objects) const
    {
        BorrowedValueSetMaker maker(inputs.makerPool());
        ValueSetID above = heap.chainOf(maker.get(), heap.prototypesOf(maker.get(), objects));
        return !ValueSets::isComplement(above) && !sets.meet(above, inputs.universe().placeholdersAlone()) && !sets.meet(above, inputs.universe().builtinTables().heap().prototypeObjectsOfClassesThatListNoOwnNames());
    }

    // Who has not accounted for the objects of a layout.
    OptionSet<Actor> actorsOpenOnObjectsWith(LayoutID id) const { return actorsOpenOnObjectsOfLayouts[id.index()]; }
    // To a field that holds a plain value, a spread is a store of an unknown value.
    OptionSet<Actor> actorsOpenOnFieldsOfObjectsWith(LayoutID id) const { return stores.getsCopiedProperties(id) ? actorsOpenOnObjectsWith(id) - Actor::KeysNobodyKnows : actorsOpenOnObjectsWith(id); }

    OptionSet<Actor> findActorsOpenOnObjectsWith(LayoutID id, std::span<const AnswersAboutObject> answers) const
    {
        constexpr OptionSet<Actor> makers { Actor::Creation, Actor::Table, Actor::NativeCode };
        ValueSetID objects = layoutsOfObjects.objectsWith(id);
        RELEASE_ASSERT(!ValueSets::isComplement(objects));
        AnswersAboutObject all { { }, true };
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            all.open.add(answers[object.index()].open);
            all.isMadeByRowThatNamesLayoutOrByOperation &= answers[object.index()].isMadeByRowThatNamesLayoutOrByOperation;
        });
        OptionSet<Actor> open = all.open & actorsThatCanWrite(PlaceKind::Field);
        if (open.containsAny(makers) && (all.isMadeByRowThatNamesLayoutOrByOperation || makerAboveClassesHasAccountedFor(id) || everyClassAboveListsOwnNames(objects)))
            open.remove(makers);
        // No row describes the native code that holds an object.
        if (sets.meet(objects, inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::HeldByNativeCode))))
            open.add(Actor::NativeCode);
        // Nobody lists where the objects are made.
        if (application.layouts[id.index()].flow.allocationClasses.isEmpty())
            open.add(Actor::Creation);
        if (possibleMutationsOf(id).contains(ObjectMutation::ReachUnknownJavaScript) || stores.hasObjectOfCodeWithoutSolve(id))
            open.add(Actor::UnseenCode);
        // A copy brings names that nobody lists, and what it brings under them is not followed.
        if (stores.getsCopiedProperties(id))
            open.add(Actor::KeysNobodyKnows);
        return open;
    }

    // An object that takes its class from a prototype nobody knows is among the objects of every class that has escaped: each object is asked once, and each layout once.
    void findActorsOpenOnObjects()
    {
        Vector<ValueSetID> objectsOfLayouts;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            if (hasObjects(LayoutID(index)) && layoutsOfObjects.objectsWith(LayoutID(index)) != ValueSets::nothing)
                objectsOfLayouts.append(layoutsOfObjects.objectsWith(LayoutID(index)));
        }
        Vector<ValueFlowObjectID> objects;
        {
            BorrowedValueSetMaker maker(inputs.makerPool());
            sets.forEachMember(maker.get().uniteAll(objectsOfLayouts.span()), [&](ValueFlowObjectID object) {
                objects.append(object);
            });
        }
        Vector<AnswersAboutObject> answers(inputs.universe().numberOfObjects());
        forEachIndexInParallel(loops, objects.size(), [&](unsigned position) {
            ValueFlowObjectID object = objects[position];
            answers[object.index()] = { actorsOpenOn(inputs, object), isMadeByRowThatNamesLayoutOrByOperation(object) };
        });
        actorsOpenOnObjectsOfLayouts = Vector<OptionSet<Actor>>(numberOfLayouts);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            if (hasObjects(LayoutID(index)))
                actorsOpenOnObjectsOfLayouts[index] = findActorsOpenOnObjectsWith(LayoutID(index), answers.span());
        });
    }

    using WritesSeen = Vector<Write, 4>;
    static WritesSeen writesSeen(OptionSet<Actor> writers, ValueSetID stored, bool isStoredAfterCreation, OptionSet<ObjectMutation> possible)
    {
        WritesSeen writes;
        for (Actor writer : writers)
            writes.append(Write { writer, isStoredAfterCreation && writer != Actor::Creation ? WriteForm::Store : WriteForm::AtCreation, { }, stored });
        // A store that the index lists and whose writer it does not: what a constructor stores by name, what a birth with copies holds.
        if (isStoredAfterCreation && !isWrittenAs(writes.span(), { WriteForm::Store }))
            writes.append(Write { Actor::Statements, WriteForm::Store, { }, stored });
        if (possible.contains(ObjectMutation::DeleteField))
            writes.append(Write { Actor::Statements, WriteForm::Delete, { }, ValueSets::nothing });
        if (possible.contains(ObjectMutation::RedefineProperty))
            writes.append(Write { Actor::Statements, WriteForm::Define, { }, ValueSets::nothing });
        return writes;
    }

    ClaimsOfFieldsEnabled claimsEnabledFor(uint32_t layout) const
    {
        return { build.isEnabled(FlowResultKind::StoreCounts, layout), build.isEnabled(FlowResultKind::HolderMutations, layout), build.isEnabled(FlowResultKind::Initialization, layout) };
    }

    // Nothing tests a value that an allocation lists, so each has a write of its own.
    Vector<LedgerOfFields::Notes> collectValuesListedByAllocations() const
    {
        return collectChunksInParallel<LedgerOfFields::NumberedWrite>(loops, program.functions.size(), { }, [&](unsigned index, LedgerOfFields::Notes& found) {
            FunctionID id(index);
            if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewObject })) || !isCompiled(id))
                return;
            const Function& function = program.functions[index];
            auto note = [&](ValueID site, NOESCAPE const Invocable<ValueSetID(uint32_t position)> auto& arrivesAt) {
                const Value& allocation = function.at(site);
                if (!hasObjectsThatCodeAlters(program.at(allocation.layout()).kind))
                    return;
                std::span<const uint32_t> fields = allocation.fields();
                for (uint32_t position = 0; position < fields.size(); ++position)
                    ledger.note(found, declarationOf(program, { allocation.layout(), fields[position] }), Write { Actor::Creation, WriteForm::AtCreation, { }, arrivesAt(position), id, site });
            };
            if (!inputs.isReached(id)) {
                for (uint32_t value = 0; value < function.values.size(); ++value) {
                    if (function.values[value].opcode == Opcode::NewObject)
                        note(ValueID(value), [](uint32_t) { return ValueSets::anything; });
                }
                return;
            }
            BorrowedValueSetMaker maker(inputs.makerPool());
            for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::NewObject)
                    continue;
                std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
                note(statement.site, [&](uint32_t position) {
                    return inputs.possibleValuesOf(maker.get(), operands[OperandIndex::NewObject::values + position]);
                });
            }
        });
    }

    ValueFlowFieldContents computeDeclaredFields(std::span<const AllocatingOperation> allocatingOperations) const
    {
        struct FactsOfField {
            OptionSet<FieldFact> facts;
        };
        struct HeldField : FactsOfField {
            OptionSet<ObjectMutation> possibleMutations;
            OptionSet<Actor> open;
            OptionSet<Actor> writers;
            FieldReference declaration;
            FlowAccessorField accessor;
            ValueSetID stored { ValueSets::nothing };
            ValueFlowNumbers storedNumbers;
        };

        ValueFlowFieldContents contents;
        contents.stored = Vector<Vector<ValueSetID>>(numberOfLayouts);
        contents.storedNumbers = Vector<Vector<ValueFlowNumbers>>(numberOfLayouts);
        contents.storedInAppendedFields = Vector<Vector<ValueSetID>>(numberOfLayouts);
        contents.numbersStoredInAppendedFields = Vector<Vector<ValueFlowNumbers>>(numberOfLayouts);
        contents.appendedFieldsWithUnknownValues = Vector<BitVector>(numberOfLayouts);
        Vector<Vector<FactsOfField>> known(numberOfLayouts);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            size_t numberOfFields = program.layouts[index].fields.size();
            contents.stored[index] = Vector<ValueSetID>(FillWith { }, numberOfFields, ValueSets::nothing);
            contents.storedNumbers[index] = Vector<ValueFlowNumbers>(numberOfFields);
            known[index] = Vector<FactsOfField>(numberOfFields);
        });
        for (FieldReference declaration : findFieldsMissingAtCreation(allocatingOperations))
            known[declaration.layout.index()][declaration.field].facts.add(FieldFact::IsMissingAtCreation);

        ValueSetMakers makers(inputs);
        Vector<Vector<HeldField>> fieldsHeldByObjectsWith(numberOfLayouts);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            LayoutID id(index);
            if (!hasObjects(id))
                return;
            ValueSetMaker& maker = makers.of(worker);
            ValueSetID objects = layoutsOfObjects.objectsWith(id);
            bool getsCopiedProperties = stores.getsCopiedProperties(id);
            OptionSet<ObjectMutation> marked = possibleMutationsOf(id);
            OptionSet<ObjectMutation> possible = marked;
            OptionSet<Actor> open = actorsOpenOnFieldsOfObjectsWith(id);
            bool isSeenByOthers = !isSeenByFollowedCodeAlone(id);
            bool isDeletedByStatementsAlone = !isSeenByOthers || (heap.reaches(inputs.universe().unknownObjectAlone(), objects) && !stores.hasObjectOfCodeWithoutSolve(id) && !possible.contains(ObjectMutation::ReachUnknownJavaScript));
            auto isPutDown = [&](ValueFlowNameKind underKey, ValueFlowNameKind underUnknownKey, PropertyKeyID key) {
                return !sets.isEmpty(heap.writtenUnder(maker, objects, { underKey, key.index() })) || !sets.isEmpty(heap.writtenUnder(maker, objects, ValueFlowName::of(underUnknownKey)));
            };
            ValueSetID above;
            auto isObservedByStoresOfHeirs = [&](PropertyKeyID key) {
                if (!marked.contains(ObjectMutation::BecomePrototype) || marked.containsAny({ ObjectMutation::ChangeIntegrity, ObjectMutation::RedefineProperty }))
                    return marked.contains(ObjectMutation::BecomePrototype);
                return mayFindNameIn(maker, objectsAbove(maker, id, above), key);
            };
            fieldsHeldByObjectsWith[index].reserveInitialCapacity(program.layouts[index].fields.size());
            forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
                HeldField held;
                held.declaration = declaration;
                held.facts = { FieldFact::HasObjects, FieldFact::MayBeObserved, FieldFact::MayInheritName };
                held.possibleMutations = field.key ? possible : allObjectMutations;
                // Nobody lists what is written to a field without a name.
                held.open = !field.key ? actorsThatCanWrite(PlaceKind::Field) : hasNodeOfItsOwn(field) ? actorsOpenOnObjectsWith(id) : open;
                if (field.key) {
                    if (isDeletedByStatementsAlone && !isPutDown(ValueFlowNameKind::DeletedMember, ValueFlowNameKind::DeletedMemberUnderUnknownKey, field.key))
                        held.possibleMutations.remove(ObjectMutation::DeleteField);
                    if (!isSeenByOthers && !isPutDown(ValueFlowNameKind::RedefinedMember, ValueFlowNameKind::RedefinedMemberUnderUnknownKey, field.key))
                        held.possibleMutations.remove(ObjectMutation::RedefineProperty);
                    if (getsCopiedProperties && (!field.form.isEmpty() || stores.mayGetAccessorUnder(id, field.key)))
                        held.possibleMutations.add(ObjectMutation::RedefineProperty);
                    held.facts.set(FieldFact::MayBeObserved, isSeenByOthers || uses.mayBeObservedUnder(objects, field.key) || isObservedByStoresOfHeirs(field.key));
                    held.facts.set(FieldFact::MayBeCalled, uses.mayBeCalledUnder(objects, field.key));
                    held.facts.set(FieldFact::MayBeAccessed, uses.mayBeAccessedUnder(objects, field.key));
                    held.facts.set(FieldFact::IsStoredAfterCreation, stores.isStoredAfterCreation(id, field.key));
                    bool mayBeMissing = known[declaration.layout.index()][declaration.field].facts.contains(FieldFact::IsMissingAtCreation) || held.possibleMutations.containsAny({ ObjectMutation::DeleteField, ObjectMutation::ReachUnknownJavaScript });
                    held.facts.set(FieldFact::MayInheritName, !mayBeMissing || !build.isEnabled(FlowResultKind::OwnNames, declaration.layout.index()) || !inheritsNothingUnder(maker, id, above, field.key));
                }
                if (field.key && !hasNodeOfItsOwn(field)) {
                    if (build.isEnabled(FlowResultKind::AppendedFields, declaration.layout.index())) {
                        held.facts.add(FieldFact::IsAskedForAccessor);
                        held.accessor = stores.accessorDefinedUnder(maker, id, field.key);
                    }
                    held.stored = stores.storedUnder(maker, id, field.key);
                    held.writers = stores.writersSeenUnder(id, field.key);
                    held.storedNumbers = stores.numbersStoredUnder(id, field.key);
                    if (getsCopiedProperties) {
                        held.stored = ValueSets::anything;
                        held.writers.add(Actor::KeysNobodyKnows);
                        held.storedNumbers = ValueFlowNumbers::any();
                    }
                }
                fieldsHeldByObjectsWith[index].append(WTF::move(held));
            });
        });

        // What the layouts hold is joined declaration by declaration, for each in the order of the layouts.
        Vector<HeldField*> heldFields = collectInParallel<HeldField*>(loops, numberOfLayouts, [&](unsigned index, Vector<HeldField*>& found) {
            for (HeldField& held : fieldsHeldByObjectsWith[index])
                found.append(&held);
        });
        constexpr size_t fieldsPerShare = 1 << 10;
        size_t numberOfShares = (heldFields.size() + fieldsPerShare - 1) / fieldsPerShare;
        Vector<uint64_t> byDeclaration = createVectorWithCapacityInParallel<uint64_t>(loops, heldFields.size());
        byDeclaration.grow(heldFields.size());
        forEachIndexInParallel(loops, numberOfShares, [&](unsigned share) {
            for (size_t position = share * fieldsPerShare; position < std::min(heldFields.size(), (share + 1) * fieldsPerShare); ++position)
                byDeclaration[position] = static_cast<uint64_t>(ledger.numberOf(heldFields[position]->declaration)) << 32 | position;
        });
        parallelSort(loops, byDeclaration.mutableSpan(), std::less<uint64_t>());
        auto firstOfShare = [&](size_t share) {
            size_t position = std::min(share * fieldsPerShare, byDeclaration.size());
            while (position && position < byDeclaration.size() && byDeclaration[position] >> 32 == byDeclaration[position - 1] >> 32)
                ++position;
            return position;
        };
        Vector<LedgerOfFields::Notes> notes(numberOfShares);
        forEachIndexInParallel(loops, numberOfShares, [&](unsigned share, unsigned worker) {
            for (size_t position = firstOfShare(share), end = firstOfShare(share + 1); position < end; ++position) {
                HeldField& held = *heldFields[static_cast<uint32_t>(byDeclaration[position])];
                uint32_t layout = held.declaration.layout.index();
                uint32_t field = held.declaration.field;
                FactsOfField& all = known[layout][field];
                if (held.facts.contains(FieldFact::IsAskedForAccessor))
                    uniteAccessors(application.layouts[layout].fields[field].accessor, WTF::move(held.accessor), !all.facts.contains(FieldFact::IsAskedForAccessor));
                all.facts.add(held.facts);
                ledger.sign(notes[share], held.declaration, held.open, writesSeen(held.writers, held.stored, held.facts.contains(FieldFact::IsStoredAfterCreation), held.possibleMutations).span());
                contents.stored[layout][field] = makers.of(worker).unite(contents.stored[layout][field], held.stored);
                contents.storedNumbers[layout][field].unite(held.storedNumbers);
            }
        });
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            fieldsHeldByObjectsWith[index] = { };
        });
        notes.appendVector(collectValuesListedByAllocations());
        ledger.close(loops, WTF::move(notes));

        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            const Layout& layout = program.layouts[index];
            if (!hasObjectsThatCodeAlters(layout.kind))
                return;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                OptionSet<FieldFact> facts = known[index][field].facts;
                if (!facts.contains(FieldFact::HasObjects) || layout.fields[field].flags.contains(FieldFlags::IsInherited))
                    continue;
                FieldFlow& flow = application.layouts[index].fields[field].flow;
                Writers writers = ledger.writersOf(FieldReference { LayoutID(index), field });
                claimWhatNeverHappens(flow, writers, !facts.contains(FieldFact::IsMissingAtCreation), claimsEnabledFor(index));
                if (!facts.contains(FieldFact::MayInheritName))
                    flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
                if (hasNodeOfItsOwn(layout.fields[field])) {
                    contents.stored[index][field] = inputs.solver().possibleValuesOf(inputs.bodies().nodes().field(LayoutID(index), field));
                    contents.storedNumbers[index][field] = ValueFlowNumbers::any();
                    continue;
                }
                if (flow.location.writers != WriterState::AllKnown)
                    continue;
                claimStoreCount(flow, writers);
                if (!facts.contains(FieldFact::MayBeObserved) && build.isEnabled(FlowResultKind::ObjectUses, index) && build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
                    bool isCalled = facts.contains(FieldFact::MayBeCalled);
                    bool isAccessed = facts.contains(FieldFact::MayBeAccessed);
                    flow.location.reads = isCalled && isAccessed ? ReadState::Unknown : isCalled ? ReadState::OnlyCalled : isAccessed ? ReadState::OnlyAccessed : ReadState::NeverRead;
                }
            }
        });
        return contents;
    }

    bool mayHaveAccessor(LayoutID id) const
    {
        if (std::ranges::any_of(application.layouts[id.index()].fields, [](const FlowFieldUpdate& field) { return field.accessor.isAccessor(); }))
            return true;
        BorrowedValueSetMaker maker(inputs.makerPool());
        return std::ranges::any_of(stores.keysStoredInto(id), [&](PropertyKeyID key) {
            return stores.mayGetAccessorUnder(id, key) || stores.accessorDefinedUnder(maker.get(), id, key).isAccessor();
        });
    }

    void computeKeyEnumerations() const
    {
        for (uint32_t index : findIndicesInParallel(loops, numberOfLayouts, [&](unsigned index) {
            LayoutID id(index);
            if (!hasObjects(id) || application.layouts[index].flow.allocationClasses.isEmpty() || !build.isEnabled(FlowResultKind::ObjectUses, index))
                return false;
            // A copy into an object lists none of the keys that the object has.
            if (!(actorsOpenOnObjectsWith(id) - Actor::KeysNobodyKnows).isEmpty() || !isSeenByFollowedCodeAlone(id) || possibleMutationsOf(id).contains(ObjectMutation::BecomePrototype) || uses.mayBeEnumerated(layoutsOfObjects.objectsWith(id)))
                return false;
            // The loader reads the members in the order of the keys, which shows where a read runs a getter.
            return !uses.mayBeExportedByCommonJSModule(layoutsOfObjects.objectsWith(id)) || !mayHaveAccessor(id);
        })) {
            LayoutFlow& flow = application.layouts[index].flow;
            flow.keys = KeyEnumeration::NeverEnumerated;
            for (AllocationClassID allocationClass : flow.allocationClasses)
                tables.sites.allocationClasses[allocationClass.index()].keys = KeyEnumeration::NeverEnumerated;
        }
    }

    bool canAppendFieldsTo(LayoutID id) const
    {
        const Layout& layout = program.at(id);
        return (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance) && !layout.isDerived() && hasObjects(id)
            && !application.layouts[id.index()].flow.allocationClasses.isEmpty() && build.isEnabled(FlowResultKind::AppendedFields, id.index());
    }

    bool canNameField(PropertyKeyID key) const
    {
        PropertyKeyKind kind = program.at(key).kind;
        if (kind == PropertyKeyKind::Text)
            return !program.isIndexLike(key);
        if (kind == PropertyKeyKind::CreatedSymbol)
            return inputs.symbolsCreatedOnce && inputs.symbolsCreatedOnce->get(key.index());
        return kind == PropertyKeyKind::WellKnownSymbol || kind == PropertyKeyKind::RegisteredSymbol || kind == PropertyKeyKind::ModulePrivateName;
    }

    Vector<uint32_t> countOperationsNamingKeys() const
    {
        size_t numberOfKeys = program.types.numberOfPropertyKeys();
        Vector<Vector<uint32_t>> countsOfWorker(loops.width());
        forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index, unsigned worker) {
            if (!inputs.isReached(FunctionID(index)))
                return;
            if (countsOfWorker[worker].isEmpty())
                countsOfWorker[worker].fill(0, numberOfKeys);
            for (const Value& value : program.functions[index].values) {
                if (Value::opcodesWithKey.contains(value.opcode) && value.key())
                    ++countsOfWorker[worker][value.key().index()];
            }
        });
        Vector<uint32_t> counts(FillWith { }, numberOfKeys, 0u);
        for (const Vector<uint32_t>& countsOfOne : countsOfWorker) {
            for (size_t key = 0; key < countsOfOne.size(); ++key)
                counts[key] += countsOfOne[key];
        }
        return counts;
    }

    KeysByLayout collectKeysDeclaredBelow(const BitVector& isCandidate) const
    {
        KeysByLayout keysDeclaredBelow(numberOfLayouts);
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            Vector<PropertyKeyID, 8> ownKeys;
            for (const Field& field : program.layouts[index].fields) {
                if (field.key && !field.flags.contains(FieldFlags::IsInherited) && isCandidate.quickGet(field.key.index()))
                    ownKeys.append(field.key);
            }
            LayoutID above = ownKeys.isEmpty() ? LayoutID() : layoutExpectedAbove(program, LayoutID(index));
            for (size_t steps = 0; above && steps < numberOfLayouts; ++steps, above = layoutExpectedAbove(program, above))
                keysDeclaredBelow[above.index()].appendVector(ownKeys);
        }
        for (Vector<PropertyKeyID>& keys : keysDeclaredBelow) {
            AOT::sort(keys);
            removeRepeatedElements(keys);
        }
        return keysDeclaredBelow;
    }

    LayoutsByKey collectLayoutsWithFields(const BitVector& isCandidate) const
    {
        LayoutsByKey layoutsWithField;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            for (const Field& field : program.layouts[index].fields) {
                if (!field.key || !isCandidate.quickGet(field.key.index()))
                    continue;
                Vector<LayoutID>& layouts = layoutsWithField.add(field.key.index(), Vector<LayoutID>()).iterator->value;
                if (layouts.isEmpty() || layouts.last() != LayoutID(index))
                    layouts.append(LayoutID(index));
            }
        }
        return layoutsWithField;
    }

    bool findsAccessorAbove(ValueSetMaker& maker, LayoutID id, PropertyKeyID key) const
    {
        Vector<ValueSetID, 4> receiversOfDefinitions;
        for (ValueFlowName name : { ValueFlowName::getter(program, key), ValueFlowName::setter(program, key) }) {
            heap.forEachNodeWrittenUnder(name, [&](ValueSetID writtenThrough, ValueSetID value) {
                if (!sets.isEmpty(value))
                    receiversOfDefinitions.append(writtenThrough);
            });
        }
        if (receiversOfDefinitions.isEmpty())
            return false;
        ValueSetID objects = inputs.universe().objectsWith(id);
        ValueSetID above = maker.intersect(chainBuiltByProgram(objects), ValueSets::complement(objects));
        return std::ranges::any_of(receiversOfDefinitions, [&](ValueSetID writtenThrough) {
            return !sets.isEmpty(maker.intersect(writtenThrough, above));
        });
    }

    Vector<PropertyKeyID> keysStoredIntoThisByMembersAbove(const BitVector& layoutsHandedToObjectCreate, LayoutID created) const
    {
        Vector<PropertyKeyID> keys;
        if (!program.at(created).has(LayoutFlags::IsOfCreatedObject) || layoutsHandedToObjectCreate.quickGet(created.index()) || !possibleMutationsOf(created).contains(ObjectMutation::ReachUnknownJavaScript))
            return keys;
        LayoutID current = created;
        for (size_t steps = 0; current && steps < numberOfLayouts; ++steps) {
            const ClassFlow& classFlow = application.layouts[current.index()].classFlow;
            for (const ClassMember& member : classFlow.members) {
                const Function& function = program.at(member.function);
                std::optional<unsigned> thisParameter = function.hasBody() ? function.thisParameterIndex() : std::nullopt;
                if (!thisParameter)
                    continue;
                ValueID thisValue = function.at(entryBlock).parameters[*thisParameter];
                for (const Value& value : function.values) {
                    if (value.opcode == Opcode::PropSet && value.key() && sameObjectThroughConversionsToObject(function, value.objectOperand()) == thisValue)
                        keys.append(value.key());
                }
            }
            current = classFlow.prototypeBase;
        }
        AOT::sort(keys);
        removeRepeatedElements(keys);
        keys.removeAllMatching([&](PropertyKeyID key) {
            bool isNameOfAccessor = false;
            for (ValueFlowName name : { ValueFlowName::getter(program, key), ValueFlowName::setter(program, key) }) {
                heap.forEachNodeWrittenUnder(name, [&](ValueSetID, ValueSetID value) {
                    isNameOfAccessor |= !sets.isEmpty(value);
                });
            }
            return isNameOfAccessor;
        });
        return keys;
    }

    void keepMostAccessedKeys(std::optional<Vector<uint32_t>>& counts, LimitID limit, LayoutID layout, uint64_t numberOfOtherFields, Vector<PropertyKeyID>& keys) const
    {
        if (keys.isEmpty() || !build.exceeds(limit, numberOfOtherFields + keys.size(), Subject { .layout = layout }))
            return;
        if (!counts)
            counts = countOperationsNamingKeys();
        uint64_t allowed = build.limit(limit);
        size_t numberOfKeysKept = allowed > numberOfOtherFields ? allowed - numberOfOtherFields : 0;
        auto rankOf = [&](PropertyKeyID key) {
            return std::pair { stores.isStoredByConstructor(layout, key), (*counts)[key.index()] };
        };
        std::ranges::nth_element(keys, keys.begin() + numberOfKeysKept, [&](PropertyKeyID a, PropertyKeyID b) {
            return std::pair { rankOf(b), a } < std::pair { rankOf(a), b };
        });
        keys.shrink(numberOfKeysKept);
        AOT::sort(keys);
    }

    Vector<PropertyKeyID> keysOfAccessorsIn(ValueSetMaker& maker, LayoutID id) const
    {
        Vector<PropertyKeyID> keys = stores.keysOfAccessorsDefinedIn(maker, id);
        if (!stores.getsAccessorsUnderKeysNobodyKnows(id))
            return keys;
        ValueSetID objects = layoutsOfObjects.objectsWith(id);
        ValueSetID above = maker.intersect(chainBuiltByProgram(objects), ValueSets::complement(objects));
        auto isStoredAbove = [&](ValueFlowName name) {
            bool isStored = false;
            heap.forEachNodeWrittenUnder(name, [&](ValueSetID writtenThrough, ValueSetID value) {
                isStored |= !sets.isEmpty(value) && sets.meet(writtenThrough, above);
            });
            return isStored;
        };
        auto liesAbove = [&](PropertyKeyID key) {
            if (isStoredAbove(ValueFlowName::property(program, key)) || isStoredAbove(ValueFlowName::getter(program, key)))
                return true;
            bool isMemberOfBuiltin = false;
            if (BuiltinName name = program.builtinNameOf(key)) {
                sets.forEachMember(above, [&](ValueFlowObjectID holder) {
                    if (BuiltinID builtin = inputs.universe().builtinOf(holder))
                        isMemberOfBuiltin |= !!program.builtins().findOwnMember(builtin, name);
                });
            }
            return isMemberOfBuiltin;
        };
        Vector<PropertyKeyID> read(stores.keysReadFrom(id));
        read.removeAllMatching([&](PropertyKeyID key) {
            return !stores.accessorDefinedUnder(maker, id, key).isAccessor() || liesAbove(key);
        });
        uniteSortedKeys(keys, read);
        return keys;
    }

    // An object constructed by a function that only forwards `this` starts without properties, and no literal, class field or spread fills its slots: only stores by name and definitions create them.
    bool startsEmptyAndIsFilledByNameOnly(LayoutID id) const
    {
        const Layout& made = program.at(id);
        return made.kind == LayoutKind::Instance && made.has(LayoutFlags::IsConstructorFunction) && made.fields.isEmpty() && !stores.getsCopiedProperties(id);
    }

    // `fill(this)` .. `function fill(made) { made.name = ..; Object.defineProperty(made, "other", { .. }) }`, where other callers of `fill` may not be visible.
    // An object constructed by a function that only forwards `this` has the fields that the program is observed to give it.
    void forEachKeyStoredOnArrivingInstances(ValueSetMaker& maker, const Function& function, const ValueFlowStatement& statement, NOESCAPE const Invocable<void(LayoutID, PropertyKeyID, uint8_t forms)> auto& functor) const
    {
        AOT_THRESHOLD(size_t, maxLayoutsArrivingAtStore, 4);
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        const ValueFlowOperand* receiver = nullptr;
        PropertyKeyID key;
        uint8_t forms = 0;
        if (statement.opcode == Opcode::PropSet) {
            receiver = &operands[0];
            key = function.at(statement.site).key();
            forms = ValueForms::of(true, true, true);
        } else if (statement.opcode == Opcode::Call && operands.size() == OperandIndex::Call::arguments + 3) {
            const Value& callee = function.at(definitionThroughUpcasts(function, function.at(statement.site).calleeOperand()));
            BuiltinID builtin = callee.opcode == Opcode::Builtin ? program.builtins().canonicalID(callee.builtin()) : BuiltinID();
            if (builtin != Builtins::ObjectDefineProperty && builtin != Builtins::ReflectDefineProperty)
                return;
            receiver = &operands[OperandIndex::Call::arguments];
            ValueSetID keys = std::as_const(heap).possibleValuesOf(maker, operands[OperandIndex::Call::arguments + 1]);
            ValueFlowObjectID only = ValueSets::isComplement(keys) ? ValueFlowObjectID() : sets.onlyMember(keys);
            if (!only || inputs.universe().kindOf(only) != ValueFlowObjectKind::Key)
                return;
            key = inputs.universe().keyOf(only);
            forms = stores.formsOfValueDefinedByCall(maker, statement);
        }
        if (!receiver || !key || !receiver->node)
            return;
        // If every receiver is listed, the store tables have the key.
        ValueSetID possible = std::as_const(heap).possibleValuesOf(maker, *receiver);
        if (!ValueSets::isComplement(possible) && sets.isSubset(possible, inputs.universe().knownObjects()))
            return;
        ValueSetID seen = inputs.seenValuesOf(maker, *receiver);
        if (ValueSets::isComplement(seen))
            return;
        Vector<LayoutID, maxLayoutsArrivingAtStore + 1> layouts;
        sets.forEachMember(maker.intersect(seen, inputs.universe().knownObjects()), [&](ValueFlowObjectID object) {
            if (LayoutID layout = inputs.universe().layoutOf(object); layout && layouts.size() <= maxLayoutsArrivingAtStore && !layouts.contains(layout))
                layouts.append(layout);
        });
        if (layouts.size() > maxLayoutsArrivingAtStore)
            return;
        for (LayoutID layout : layouts) {
            if (startsEmptyAndIsFilledByNameOnly(layout))
                functor(layout, key, forms);
        }
    }

    // The form in which the objects are observed to get a value under the key. Every other layout has plain fields: what creates their properties does not ask.
    std::optional<OptionSet<FieldForm>> formOfValueGivenUnder(LayoutID layout, PropertyKeyID key) const
    {
        if (!startsEmptyAndIsFilledByNameOnly(layout))
            return OptionSet<FieldForm>();
        uint8_t forms = stores.formsOfValuesGivenUnder(layout, key);
        if (auto seen = formsOfValuesSeenDefined.find(static_cast<uint64_t>(layout.index()) << 32 | key.index()); seen != formsOfValuesSeenDefined.end())
            forms |= seen->value;
        return ValueForms::formToPlanFor(forms);
    }

    // `for (var name in defaults) options[name] = ..`: the store names what the enumerated objects list. A prediction: whatever else the enumeration shows is stored as a name that no field has.
    // Only for an object that the program names a field of. `copy[key] = clone(value[key])` fills an object that holds other names each time, and flow follows few of the objects that are copied.
    void forEachKeyOfEnumerationStoredUnder(ValueSetMaker& maker, const Function& function, uint32_t body, const ValueFlowStatement& store, size_t mostLayouts, NOESCAPE const Invocable<void(LayoutID, PropertyKeyID)> auto& functor) const
    {
        namespace At = OperandIndex;
        const Value& name = function.at(definitionThroughTests(function, function.at(store.site).operand(At::ElemSet::property)));
        if (name.opcode != Opcode::ResultAt || name.index() != static_cast<uint32_t>(ForInStepResult::Key) || function.at(name.resultsOperand()).opcode != Opcode::ForInStep)
            return;
        auto layoutsFollowedTo = [&](const ValueFlowOperand& operand) {
            Vector<LayoutID, 4> layouts;
            ValueSetID seen = inputs.seenValuesOf(maker, operand);
            if (seen == ValueSets::nothing || ValueSets::isComplement(seen))
                return layouts;
            sets.forEachMemberInLine(seen, [&](ValueFlowObjectID object) {
                LayoutID layout = inputs.universe().layoutOf(object);
                if (layout && layouts.size() <= mostLayouts && !layouts.contains(layout))
                    layouts.append(layout);
            });
            if (layouts.size() > mostLayouts)
                layouts.clear();
            return layouts;
        };
        Vector<LayoutID, 4> targets = layoutsFollowedTo(inputs.bodies().operandsOf(store)[At::ElemSet::object]);
        targets.removeAllMatching([&](LayoutID target) {
            return program.at(target).fields.isEmpty() || program.at(target).has(LayoutFlags::IsWrittenUnderDictionaryType);
        });
        if (targets.isEmpty())
            return;
        for (const ValueFlowStatement& step : inputs.bodies().statementsOf(body)) {
            if (step.kind != ValueFlowStatementKind::Operation || step.opcode != Opcode::ForInStep || step.site != name.resultsOperand())
                continue;
            for (LayoutID enumerated : layoutsFollowedTo(inputs.bodies().operandsOf(step)[At::ForInStep::object])) {
                for (const Field& field : program.at(enumerated).fields) {
                    if (field.key && program.at(field.key).kind == PropertyKeyKind::Text && !field.flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsBrand })) {
                        for (LayoutID target : targets)
                            functor(target, field.key);
                    }
                }
            }
        }
    }

    bool findsMemberOfClassUnder(LayoutID id, PropertyKeyID key) const
    {
        LayoutID current = declaredLayoutOf(program, id);
        for (size_t steps = 0; current && steps < numberOfLayouts; ++steps) {
            if (findDeclaredMember(application.layouts[current.index()].classFlow, ClassSide::Prototype, key).isDeclared())
                return true;
            // The answer only steers a prediction, so an unproven `extends` clause counts too.
            current = application.layouts[current.index()].flow.baseState == BaseState::Unknown ? program.at(current).base : provenBaseOf(application, current);
        }
        return false;
    }

    KeysByLayout chooseAppendedKeys(KeysByLayout& keysFilledByOutsideCreators) const
    {
        ValueSetMakers makers(inputs);
        KeysByLayout keysByLayout(numberOfLayouts);
        KeysByLayout keysStoredByMembersAbove(numberOfLayouts);
        Vector<uint64_t> fieldCountsAsAnalyzed(FillWith { }, numberOfLayouts, static_cast<uint64_t>(0));
        BitVector layoutsHandedToObjectCreate(numberOfLayouts);
        for (uint32_t index = program.numberOfLayoutsAsRead(); index < numberOfLayouts; ++index) {
            if (LayoutID base = program.layouts[index].has(LayoutFlags::IsOfCreatedObject) ? application.layouts[index].classFlow.prototypeBase : LayoutID())
                layoutsHandedToObjectCreate.quickSet(base.index());
        }
        // A store under a name into what a read under a name has answered names a field of the layouts that are written under the name of the read, where that name tells a few layouts from all others.
        AOT_THRESHOLD(size_t, mostLayoutsToldByName, 4);
        AOT_THRESHOLD(size_t, maxLayoutsFollowedToStore, 16);
        AOT_THRESHOLD(size_t, maxLayoutsFollowedToStoreIntoGivenObject, 4);
        KeysByLayout keysNamedBehindReads(numberOfLayouts);
        KeysByLayout keysNamedAtFollowedUses(numberOfLayouts);
        KeysByLayout keysNamedAtFollowedUsesOfGivenObjects(numberOfLayouts);
        KeysByLayout keysNamedOnlyByOutsideCreators(numberOfLayouts);
        KeysByLayout keysGivenToInstancesSeen(numberOfLayouts);
        KeysByLayout keysOfEnumerationsCopied(numberOfLayouts);
        AOT_THRESHOLD(size_t, mostLayoutsEnumerated, 2);
        // Every statement is processed independently, on all threads. The lists are sorted below, so the order in which keys arrive does not matter.
        struct KeyNamed {
            KeysByLayout* list;
            LayoutID layout;
            PropertyKeyID key;
            uint8_t forms;
        };
        constexpr uint32_t functionsPerShare = 64;
        Vector<Vector<KeyNamed>> keysNamedInShare((program.functions.size() + functionsPerShare - 1) / functionsPerShare);
        forEachIndexInParallel(loops, keysNamedInShare.size(), [&](unsigned share, unsigned worker) {
            for (uint32_t index = share * functionsPerShare; index < std::min<uint32_t>(program.functions.size(), (share + 1) * functionsPerShare); ++index) {
                if (!inputs.isReached(FunctionID(index)))
                    continue;
                for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
                    if (statement.kind != ValueFlowStatementKind::Operation)
                        continue;
                    forEachKeyStoredOnArrivingInstances(makers.of(worker), program.functions[index], statement, [&](LayoutID layout, PropertyKeyID key, uint8_t forms) {
                        keysNamedInShare[share].append({ &keysGivenToInstancesSeen, layout, key, forms });
                    });
                    if (statement.opcode == Opcode::ElemSet) {
                        forEachKeyOfEnumerationStoredUnder(makers.of(worker), program.functions[index], index, statement, mostLayoutsEnumerated, [&](LayoutID layout, PropertyKeyID key) {
                            keysNamedInShare[share].append({ &keysOfEnumerationsCopied, layout, key, 0 });
                        });
                    }
                    if (statement.opcode != Opcode::PropSet)
                        continue;
                    ValueFlowNodeID receiver = inputs.bodies().operandsOf(statement)[0].node;
                    PropertyKeyID key = program.functions[index].at(statement.site).key();
                    if (!key || !receiver)
                        continue;
                    auto nameFieldOfLayoutsIn = [&](ValueSetID seen, size_t mostLayouts, bool everyObjectMustHaveLayout, KeysByLayout& keysNamed) {
                        if (seen == ValueSets::nothing || ValueSets::isComplement(seen))
                            return;
                        Vector<LayoutID, maxLayoutsFollowedToStore + 1> layouts;
                        bool holdsObjectWithoutLayout = false;
                        sets.forEachMemberInLine(seen, [&](ValueFlowObjectID object) {
                            LayoutID layout = inputs.universe().layoutOf(object);
                            holdsObjectWithoutLayout |= !layout && inputs.universe().isObject(object) && object != ValueFlowUniverse::unknownObject() && object != ValueFlowUniverse::untrackedObject();
                            if (layout && layouts.size() <= mostLayouts && !layouts.contains(layout))
                                layouts.append(layout);
                        });
                        if (layouts.size() > mostLayouts || (holdsObjectWithoutLayout && everyObjectMustHaveLayout))
                            return;
                        for (LayoutID layout : layouts)
                            keysNamedInShare[share].append({ &keysNamed, layout, key, 0 });
                    };
                    nameFieldOfLayoutsIn(inputs.solver().writtenUnderNameOfReadSeenAt(receiver), mostLayoutsToldByName, false, keysNamedBehindReads);
                    // `const node = createNode(kind); node.left = left;`: code that has an object created and keeps filling it counts as one of its creators, even if the creating function is unknown. The store names a field of the layouts that were followed to it.
                    const Function& function = program.functions[index];
                    Opcode maker = function.at(sameObjectThroughCasts(function, function.at(statement.site).objectOperand())).opcode;
                    // `function route(request) { request.parsedUrl = ..; }`: code that is passed an object also keeps filling it. What was followed to a parameter is not all that arrives there, so fewer layouts are trusted.
                    bool isMadeHere = maker == Opcode::Call || maker == Opcode::Construct;
                    if (ValueSetID seen = inputs.solver().seenValuesOf(receiver); seen != inputs.solver().possibleValuesOf(receiver))
                        nameFieldOfLayoutsIn(seen, isMadeHere ? maxLayoutsFollowedToStore : maxLayoutsFollowedToStoreIntoGivenObject, isMadeHere, isMadeHere ? keysNamedAtFollowedUses : keysNamedAtFollowedUsesOfGivenObjects);
                }
            }
        });
        for (const Vector<KeyNamed>& keysNamed : keysNamedInShare) {
            for (const KeyNamed& named : keysNamed) {
                (*named.list)[named.layout.index()].append(named.key);
                if (named.list == &keysGivenToInstancesSeen)
                    formsOfValuesSeenDefined.add(static_cast<uint64_t>(named.layout.index()) << 32 | named.key.index(), 0).iterator->value |= named.forms;
            }
        }
        for (KeysByLayout* keysNamed : { &keysNamedBehindReads, &keysNamedAtFollowedUses, &keysNamedAtFollowedUsesOfGivenObjects, &keysGivenToInstancesSeen, &keysOfEnumerationsCopied }) {
            for (Vector<PropertyKeyID>& keys : *keysNamed) {
                AOT::sort(keys);
                keys.shrink(std::unique(keys.begin(), keys.end()) - keys.begin());
            }
        }
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            forEachOnProvenChain(application, LayoutID(index), [&](LayoutID current) {
                fieldCountsAsAnalyzed[index] += program.at(current).fields.size();
            });
            if (!canAppendFieldsTo(LayoutID(index)))
                return;
            Vector<PropertyKeyID> accessors = keysOfAccessorsIn(makers.of(worker), LayoutID(index));
            if (accessors.isEmpty() && program.layouts[index].has(LayoutFlags::IsOfCreatedObject) && application.layouts[index].classFlow.evaluations != ExecutionCount::AtMostOnce)
                return;
            keysByLayout[index] = stores.keysNamedByStoresIntoKnownObjects(LayoutID(index));
            uniteSortedKeys(keysByLayout[index], keysOfEnumerationsCopied[index]);
            // A literal says what it has. What a constructor makes is filled on by others.
            if (program.layouts[index].kind == LayoutKind::Instance)
                uniteSortedKeys(keysByLayout[index], keysNamedBehindReads[index]);
            keysNamedOnlyByOutsideCreators[index] = keysNamedAtFollowedUses[index];
            keysNamedOnlyByOutsideCreators[index].removeAllMatching([&](PropertyKeyID key) {
                return std::ranges::binary_search(keysByLayout[index], key);
            });
            uniteSortedKeys(keysByLayout[index], keysNamedAtFollowedUses[index]);
            // A literal shows what it has. An object from Object.create() inherits from an object that may have been given accessors. For a class that is evaluated repeatedly, the evaluation that created a given object is unknown.
            if (program.layouts[index].kind != LayoutKind::Instance || program.layouts[index].has(LayoutFlags::IsOfCreatedObject) || application.layouts[index].classFlow.evaluations != ExecutionCount::AtMostOnce)
                keysNamedAtFollowedUsesOfGivenObjects[index].clear();
            // `point.norm = replacement` on one object of many: the others use their class's method, and every one of them would have to check a row first.
            keysNamedAtFollowedUsesOfGivenObjects[index].removeAllMatching([&](PropertyKeyID key) {
                return findsMemberOfClassUnder(LayoutID(index), key);
            });
            uniteSortedKeys(keysByLayout[index], keysNamedAtFollowedUsesOfGivenObjects[index]);
            uniteSortedKeys(keysByLayout[index], accessors);
            if (startsEmptyAndIsFilledByNameOnly(LayoutID(index))) {
                uniteSortedKeys(keysByLayout[index], stores.keysOfValuesDefinedIn(LayoutID(index)));
                uniteSortedKeys(keysByLayout[index], keysGivenToInstancesSeen[index]);
            }
            keysStoredByMembersAbove[index] = keysStoredIntoThisByMembersAbove(layoutsHandedToObjectCreate, LayoutID(index));
            uniteSortedKeys(keysByLayout[index], keysStoredByMembersAbove[index]);
        });
        Vector<uint32_t> layoutsWithKeys;
        BitVector isCandidate(program.types.numberOfPropertyKeys());
        BitVector declaredLayoutsWithKeys(numberOfLayouts);
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            if (keysByLayout[index].isEmpty())
                continue;
            layoutsWithKeys.append(index);
            declaredLayoutsWithKeys.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
            for (PropertyKeyID key : keysByLayout[index])
                isCandidate.quickSet(key.index());
        }
        KeysByLayout keysDeclaredBelow;
        LayoutsByKey layoutsWithField;
        Vector<Vector<LayoutID>> declaredViews;
        runTasksInParallel(loops, {
            [&] { keysDeclaredBelow = collectKeysDeclaredBelow(isCandidate); },
            [&] { layoutsWithField = collectLayoutsWithFields(isCandidate); },
            [&] { declaredViews = declaredViewsOf(program, declaredLayoutsWithKeys); },
        });
        AOT::stableSort(layoutsWithKeys, std::greater { }, [&](uint32_t index) {
            return keysByLayout[index].size();
        });
        forEachIndexInParallel(loops, layoutsWithKeys.size(), [&](unsigned position, unsigned worker) {
            LayoutID id(layoutsWithKeys[position]);
            ValueSetMaker& maker = makers.of(worker);
            Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, id);
            std::span<const LayoutID> views = declaredViews[declaredLayoutOf(program, id).index()].span();
            KeysOfFieldsHeld keysOfFields(*this, id);
            keysByLayout[id.index()].removeAllMatching([&](PropertyKeyID key) {
                auto declarers = layoutsWithField.find(key.index());
                PropertyKeyKind kind = program.at(key).kind;
                bool canBeDeclared = kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol && kind != PropertyKeyKind::ModulePrivateName;
                bool isOutsideFamilies = canBeDeclared && !families.isEmpty() && (declarers == layoutsWithField.end() || (!haveCommonMember(views, declarers->value.span()) && std::ranges::none_of(families, [&](LayoutSetID family) {
                    return haveCommonMember(program.at(family).completeMembers(), declarers->value.span());
                })));
                // A store to a name that no type declares has bypassed the types.
                isOutsideFamilies = isOutsideFamilies && !std::ranges::binary_search(keysNamedAtFollowedUses[id.index()], key) && !std::ranges::binary_search(keysNamedAtFollowedUsesOfGivenObjects[id.index()], key);
                if (isOutsideFamilies || std::ranges::binary_search(keysDeclaredBelow[id.index()], key) || !canNameField(key) || keysOfFields.contains(key))
                    return true;
                if (stores.accessorDefinedUnder(maker, id, key).isAccessor())
                    return false;
                bool isStored = stores.isStoredByConstructor(id, key) || std::ranges::binary_search(keysStoredByMembersAbove[id.index()], key) || !sets.isEmpty(stores.storedUnder(maker, id, key));
                return stores.mayGetAccessorUnder(id, key) || !isStored || findsAccessorAbove(maker, id, key);
            });
        });

        Vector<std::pair<uint32_t, uint32_t>> fromBasesDown(layoutsWithKeys.size(), [&](size_t position) {
            uint32_t depth = 0;
            forEachOnProvenChain(application, LayoutID(layoutsWithKeys[position]), [&](LayoutID) {
                ++depth;
            });
            return std::pair { depth, layoutsWithKeys[position] };
        });
        AOT::sort(fromBasesDown);
        std::optional<Vector<uint32_t>> counts;
        for (auto [depth, index] : fromBasesDown) {
            LayoutID layout(index);
            Vector<PropertyKeyID>& keys = keysByLayout[index];
            if (keys.isEmpty())
                continue;
            uint64_t numberOfInheritedKeys = 0;
            forEachOnProvenChain(application, provenBaseOf(application, layout), [&](LayoutID ancestor) {
                const Vector<PropertyKeyID>& inheritedKeys = keysByLayout[ancestor.index()];
                numberOfInheritedKeys += inheritedKeys.size();
                keys.removeAllMatching([&](PropertyKeyID key) {
                    return std::ranges::binary_search(inheritedKeys, key);
                });
            });
            uint64_t largestFieldCountBelow = 0;
            forEachLayoutBelow(layout, [&](LayoutID below) {
                largestFieldCountBelow = std::max(largestFieldCountBelow, fieldCountsAsAnalyzed[below.index()]);
            });
            Vector<PropertyKeyID> accessors;
            keys.removeAllMatching([&](PropertyKeyID key) {
                bool isAccessor = stores.accessorDefinedUnder(layout, key).isAccessor();
                if (isAccessor)
                    accessors.append(key);
                return isAccessor;
            });
            keepMostAccessedKeys(counts, LimitID::MaxAppendedFieldsPerLayout, layout, 0, keys);
            // Fields that are added late are stored outside the cell, where there is room for this many. One more would make every cell larger, and which objects get these fields is unknown.
            Vector<PropertyKeyID> namedByOutsideCreators;
            keys.removeAllMatching([&](PropertyKeyID key) {
                bool isNamedOnlyByThem = std::ranges::binary_search(keysNamedOnlyByOutsideCreators[index], key);
                if (isNamedOnlyByThem)
                    namedByOutsideCreators.append(key);
                return isNamedOnlyByThem;
            });
            keepMostAccessedKeys(counts, LimitID::MaxExtensionGranules, layout, numberOfInheritedKeys + keys.size(), namedByOutsideCreators);
            uniteSortedKeys(keys, namedByOutsideCreators);
            uniteSortedKeys(keys, accessors);
            keepMostAccessedKeys(counts, LimitID::FieldsInKeyOrder, layout, largestFieldCountBelow + numberOfInheritedKeys, keys);
        }
        keysFilledByOutsideCreators = WTF::move(keysNamedAtFollowedUses);
        return keysByLayout;
    }

    void appendFields(const KeysByLayout& keysByLayout, const KeysByLayout& keysFilledByOutsideCreators, ValueFlowFieldContents& contents) const
    {
        ValueSetMakers makers(inputs);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            ValueSetMaker& maker = makers.of(worker);
            for (PropertyKeyID key : keysByLayout[index]) {
                FlowAppendedField field;
                field.key = key;
                field.isFilledByCreators = std::ranges::binary_search(keysFilledByOutsideCreators[index], key);
                OptionSet<ObjectMutation> possible;
                OptionSet<Actor> open;
                OptionSet<Actor> writers;
                ValueSetID stored = ValueSets::nothing;
                ValueFlowNumbers storedNumbers;
                bool isStoredBeforeObjectsLeak = true;
                bool inheritsNothing = build.isEnabled(FlowResultKind::OwnNames, index);
                bool isFirst = true;
                bool isGivenUnknownValues = false;
                std::optional<OptionSet<FieldForm>> formOfValue;
                bool hasOneFormOfValue = true;
                forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                    if (!hasObjects(below))
                        return;
                    std::optional<OptionSet<FieldForm>> formBelow = formOfValueGivenUnder(below, key);
                    hasOneFormOfValue &= formBelow && (isFirst || formOfValue == formBelow);
                    formOfValue = formBelow;
                    ValueSetID above;
                    inheritsNothing = inheritsNothing && inheritsNothingUnder(maker, below, above, key);
                    possible.add(possibleMutationsOf(below));
                    open.add(actorsOpenOnFieldsOfObjectsWith(below));
                    stored = maker.unite(stored, stores.getsCopiedProperties(below) ? stores.storedUnderExactName(maker, below, key) : stores.storedUnder(maker, below, key));
                    writers.add(stores.writersSeenUnder(below, key));
                    storedNumbers.unite(stores.numbersStoredUnder(below, key));
                    if (stores.getsCopiedProperties(below)) {
                        isGivenUnknownValues = true;
                        writers.add(Actor::KeysNobodyKnows);
                        if (stores.mayGetAccessorUnder(below, key))
                            possible.add(ObjectMutation::RedefineProperty);
                    }
                    isStoredBeforeObjectsLeak &= holdsSinceBirth(below, key) || (constructions.isFollowed(below) && constructions.isStoredBeforeObjectLeaks(below, key, { }));
                    uniteAccessors(field.accessor, stores.accessorDefinedUnder(maker, below, key), std::exchange(isFirst, false));
                });
                if (hasOneFormOfValue && formOfValue && !field.accessor.isAccessor())
                    field.formOfValue = *formOfValue;
                WritesSeen writes = writesSeen(writers, stored, true, possible);
                ClaimsOfFieldsEnabled enabled = claimsEnabledFor(index);
                enabled.storeCounts &= !sets.isEmpty(stored);
                claimWhatNeverHappens(field.flow, Writers(writes.span(), open), isStoredBeforeObjectsLeak, enabled);
                if (inheritsNothing)
                    field.flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
                application.layouts[index].appendedFields.append(WTF::move(field));
                if (isGivenUnknownValues)
                    contents.appendedFieldsWithUnknownValues[index].set(contents.storedInAppendedFields[index].size());
                contents.storedInAppendedFields[index].append(stored);
                contents.numbersStoredInAppendedFields[index].append(storedNumbers);
            }
        });
        uint64_t numberOfAppendedFields = 0;
        for (const Vector<PropertyKeyID>& keys : keysByLayout)
            numberOfAppendedFields += keys.size();
        build.statistics().count(StatisticID::FlowAppendedFields, numberOfAppendedFields);
    }

    BitVector findLayoutsConstructedByNativeCode() const
    {
        BitVector layouts(numberOfLayouts);
        Vector<ValueSetID> resultsOfConstructions = collectFromCallGroups<ValueSetID>(inputs, heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID first, Vector<ValueSetID>& found) {
            if (key.callKind() == ValueFlowCallKind::ConstructByNativeCode)
                found.append(inputs.solver().possibleValuesOf(callGroupAt(first).result));
        });
        if (resultsOfConstructions.isEmpty())
            return layouts;
        AOT::sort(resultsOfConstructions);
        removeRepeatedElements(resultsOfConstructions);
        ValueSetMaker& maker = heap.maker();
        ValueSetID made = maker.intersect(heap.expand(maker.uniteAll(resultsOfConstructions.span())), inputs.universe().knownObjects());
        for (uint32_t layout = 0; ValueSets::isComplement(made) && layout < numberOfLayouts; ++layout)
            layouts.quickSet(layout);
        if (!ValueSets::isComplement(made)) {
            layoutsOfObjects.forEachLayoutOfMembers(made, [&](LayoutID layout) {
                layouts.quickSet(layout.index());
            });
        }
        return layouts;
    }

    bool assignmentsMeetAccessorOfClass(LayoutID id, PropertyKeyID key) const
    {
        constexpr OptionSet<ObjectMutation> memberHidingMutations { ObjectMutation::ChangePrototype, ObjectMutation::RedefineProperty, ObjectMutation::ReachUnknownJavaScript };
        if (program.at(id).kind != LayoutKind::Instance || program.at(key).kind != PropertyKeyKind::Text || possibleMutationsOf(id).containsAny(memberHidingMutations))
            return false;
        if (stores.getsNameNobodyLists(id) || stores.hasObjectOfCodeWithoutSolve(id))
            return false;
        LayoutID current = declaredLayoutOf(program, id);
        for (size_t steps = 0; current && steps < numberOfLayouts; ++steps) {
            const LayoutFlow& flow = application.layouts[current.index()].flow;
            const ClassFlow& classFlow = application.layouts[current.index()].classFlow;
            if (program.at(current).kind != LayoutKind::Instance || !program.at(current).constructor || !classFlow.prototypeNamesAreComplete)
                return false;
            DeclaredMember member = findDeclaredMember(classFlow, ClassSide::Prototype, key);
            if (member.isDeclared() || member.mutability != MemberMutability::Fixed)
                return member.isAccessor() && member.mutability == MemberMutability::Fixed;
            if (classFlow.prototypeLink != MemberMutability::Fixed || flow.baseState != BaseState::Layout)
                return false;
            current = flow.base;
        }
        return false;
    }

    // Each evaluation of a class creates its own private names, but a field holds the name of only one evaluation. Only a derived class defines its names on an object it did not allocate.
    bool isReevaluatedPrivateName(PropertyKeyID key) const
    {
        if (!key || program.at(key).kind != PropertyKeyKind::PrivateName)
            return false;
        LayoutID owner = program.at(key).owner;
        return program.at(owner).has(LayoutFlags::HasHeritage) && application.layouts[owner.index()].classFlow.evaluations != ExecutionCount::AtMostOnce;
    }

    void computeAddedKeys(const KeysByLayout& appendedKeys) const
    {
        BitVector isConstructedByNativeCode = findLayoutsConstructedByNativeCode();
        const Vector<PropertyKeyID>& sharedKeys = stores.keysStoredThroughPlaceholders();
        tables.wholeProgram.keysStoredThroughUnknownReceivers = sharedKeys;
        tables.wholeProgram.keysDefinedOnObjectsWithoutKnownLayout = stores.keysDefinedOnObjectsWithoutKnownLayout();
        // The objects that the definition is observed to reach now have a field for it.
        tables.wholeProgram.keysDefinedOnObjectsWithoutKnownLayout.removeAllMatching([&](PropertyKeyID key) {
            return std::ranges::any_of(formsOfValuesSeenDefined.keys(), [&](uint64_t layoutAndKey) {
                return static_cast<uint32_t>(layoutAndKey) == key.index() && std::ranges::binary_search(appendedKeys[layoutAndKey >> 32], key);
            });
        });
        BitVector declaresReevaluatedPrivateName;
        for (unsigned index = 0; index < numberOfLayouts; ++index) {
            if (std::ranges::any_of(program.layouts[index].fields, [&](const Field& field) { return isReevaluatedPrivateName(field.key); }))
                declaresReevaluatedPrivateName.set(index);
        }
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            LayoutID id(index);
            LayoutFlow& flow = application.layouts[index].flow;
            LayoutKind kind = program.layouts[index].kind;
            if (!hasObjects(id) || flow.allocationClasses.isEmpty() || (kind != LayoutKind::Record && kind != LayoutKind::Instance))
                return;
            bool listsEveryName = actorsOpenOnObjectsWith(id).isEmpty() && !stores.getsNameNobodyLists(id) && !isConstructedByNativeCode.quickGet(index);
            Vector<PropertyKeyID> addedKeys;
            bool mayGainSharedKey = false;
            if (listsEveryName) {
                KeysOfFieldsHeld keysOfFields(*this, id, &appendedKeys);
                auto isField = [&](PropertyKeyID key) {
                    return !std::ranges::binary_search(tables.wholeProgram.keysOfSeveralSymbols, key) && keysOfFields.contains(key);
                };
                addedKeys = stores.keysStoredIntoKnownObjects(id);
                addedKeys.removeAllMatching(isField);
                Vector<PropertyKeyID> reevaluatedPrivateNames;
                if (!declaresReevaluatedPrivateName.isEmpty()) {
                    forEachOnProvenChain(application, id, [&](LayoutID current) {
                        if (!declaresReevaluatedPrivateName.get(current.index()))
                            return;
                        for (const Field& field : program.at(current).fields) {
                            if (isReevaluatedPrivateName(field.key) && !reevaluatedPrivateNames.contains(field.key))
                                reevaluatedPrivateNames.append(field.key);
                        }
                    });
                }
                AOT::sort(reevaluatedPrivateNames);
                uniteSortedKeys(addedKeys, reevaluatedPrivateNames);
                mayGainSharedKey = stores.isReachedThroughPlaceholders(id) && !std::ranges::all_of(sharedKeys, isField);
                // An assignment that hits an accessor of the class adds nothing. That only counts if nothing else is added either: then the layout says that none of its objects gains a property, which a checking build enforces for every object.
                bool everyKeyMeetsAccessor = !mayGainSharedKey && build.isEnabled(FlowResultKind::HolderMutations, index) && std::ranges::all_of(addedKeys, [&](PropertyKeyID key) {
                    return stores.isOnlyAssignedByStatements(id, key) && assignmentsMeetAccessorOfClass(id, key);
                });
                if (everyKeyMeetsAccessor)
                    addedKeys.clear();
            }
            bool mayAddProperty = !listsEveryName || !addedKeys.isEmpty() || mayGainSharedKey;
            auto apply = [&](auto& record) {
                record.impossibleMutations.set(ObjectMutation::AddProperty, !mayAddProperty && build.isEnabled(FlowResultKind::HolderMutations, index));
                if (!build.isEnabled(FlowResultKind::OwnNames, index))
                    return;
                record.addedKeys = addedKeys;
                record.addedKeysAreComplete = listsEveryName;
                record.isReachedThroughUnknownReceivers = mayGainSharedKey;
            };
            apply(flow);
            for (AllocationClassID allocationClass : flow.allocationClasses)
                apply(tables.sites.allocationClasses[allocationClass.index()]);
        });
    }

    bool createsOwnFieldsAtConstruction(const BitVector& layoutsOfObjectsWithoutListedOrder, LayoutID id) const
    {
        return !isBornWithCopies.get(id.index()) && createsOwnFieldsBeforeObjectsAreFinished(layoutsOfObjectsWithoutListedOrder, id);
    }

    bool createsOwnFieldsBeforeObjectsAreFinished(const BitVector& layoutsOfObjectsWithoutListedOrder, LayoutID id) const
    {
        const Layout& layout = program.at(id);
        bool hasListedOrders = layout.kind == LayoutKind::Record ? !layoutsOfObjectsWithoutListedOrder.quickGet(id.index()) : layout.kind == LayoutKind::Instance && constructions.createsFieldsOnlyInListedOrders(id);
        if (!hasListedOrders || possibleMutationsOf(id).contains(ObjectMutation::AddProperty) || !application.layouts[id.index()].appendedFields.isEmpty())
            return false;
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            if (field.flags.contains(FieldFlags::IsInherited) || application.layouts[id.index()].fields[index].flow.location.initialization == InitializationState::SetAtCreation)
                continue;
            // The initializer of the class defines the brand where the object has none, and nothing else does.
            if (field.flags.contains(FieldFlags::IsBrand))
                continue;
            if (!field.key || hasNodeOfItsOwn(field) || stores.isStoredAfterCreation(id, field.key))
                return false;
        }
        return true;
    }

    void computeFieldCreations() const
    {
        BitVector layoutsOfObjectsWithoutListedOrder(numberOfLayouts);
        for (LayoutID layout : layoutsOfOtherSites([](ValueFlowObjectID, const ValueFlowSite&) { return true; })) {
            layoutsOfObjectsWithoutListedOrder.quickSet(layout.index());
            layoutsOfObjectsWithoutListedOrder.quickSet(declaredLayoutOf(program, layout).index());
        }
        Vector<uint8_t> createsOwnFields(FillWith { }, numberOfLayouts, static_cast<uint8_t>(0));
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            createsOwnFields[index] = hasObjects(LayoutID(index)) && build.isEnabled(FlowResultKind::ConstructorFieldOrders, index) && createsOwnFieldsAtConstruction(layoutsOfObjectsWithoutListedOrder, LayoutID(index));
        });
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            bool isCreatedAtConstruction = !hasObjects(LayoutID(index)) || actorsOpenOnObjectsWith(LayoutID(index)).isEmpty();
            forEachOnProvenChain(application, LayoutID(index), [&](LayoutID current) {
                BaseState baseState = application.layouts[current.index()].flow.baseState;
                bool createsFieldsInObjectsBelow = current != LayoutID(index) && !hasObjects(current) && constructions.createsFieldsOnlyInListedOrders(current);
                isCreatedAtConstruction &= (baseState == BaseState::Layout || baseState == BaseState::None) && (createsOwnFields[current.index()] || createsFieldsInObjectsBelow);
            });
            if (isCreatedAtConstruction)
                application.layouts[index].flow.fieldCreation = FieldCreation::OnlyAtConstruction;
            // `{ port, host: name() }`: the allocation lists nothing and the definitions after it create the fields, each before anything else sees the object.
            LayoutID id(index);
            // Object.assign() creates fields inside the call, in the order of its sources' keys. No piece of a literal lists that order.
            application.layouts[index].createsFieldsOnlyInLiterals = program.layouts[index].kind == LayoutKind::Record && hasObjects(id) && isBornWithCopies.get(index) && !isBornWithAssignedCopies.get(index) && application.layouts[index].flow.baseState == BaseState::None
                && build.isEnabled(FlowResultKind::ConstructorFieldOrders, index) && actorsOpenOnObjectsWith(id).isEmpty() && createsOwnFieldsBeforeObjectsAreFinished(layoutsOfObjectsWithoutListedOrder, id);
        });
    }

    void computeInitialFieldOrders(std::span<const AllocatingOperation> operations) const
    {
        struct CountedOrder {
            Vector<uint32_t> fields;
            uint32_t numberOfSites { 0 };
            bool isOrderOfFinishedObjects { false };
        };
        Vector<Vector<CountedOrder>> ordersOfLayout(numberOfLayouts);
        auto count = [&](LayoutID declared, std::span<const uint32_t> fields, bool getsMoreFields) {
            if (!hasObjectsThatCodeAlters(program.at(declared).kind) || program.at(declared).constructor)
                return;
            Vector<CountedOrder>& orders = ordersOfLayout[declared.index()];
            auto found = std::ranges::find_if(orders, [&](const CountedOrder& order) {
                return std::ranges::equal(order.fields, fields);
            });
            if (found == orders.end()) {
                orders.append({ Vector<uint32_t>(fields) });
                found = orders.end() - 1;
            }
            ++found->numberOfSites;
            found->isOrderOfFinishedObjects |= !getsMoreFields;
        };
        for (const AllocatingOperation& operation : operations) {
            const Value& value = program.at(operation.function).at(operation.value);
            if (value.opcode == Opcode::NewObject) {
                count(declaredLayoutOf(program, value.layout()), value.fields(), operation.getsMoreFields);
                continue;
            }
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                if (const DataNode::Object* object = node.tryAs<DataNode::Object>()) {
                    LayoutID declared = declaredLayoutOf(program, object->layout);
                    count(declared, fieldsFilledBy(program, program.at(declared), *object).span(), false);
                }
            });
        }
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            Vector<CountedOrder>& orders = ordersOfLayout[index];
            if (orders.isEmpty() || !build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
                continue;
            AOT::sort(orders, [](const CountedOrder& a, const CountedOrder& b) {
                if (a.isOrderOfFinishedObjects != b.isOrderOfFinishedObjects)
                    return a.isOrderOfFinishedObjects;
                if (a.numberOfSites != b.numberOfSites)
                    return a.numberOfSites > b.numberOfSites;
                return std::ranges::lexicographical_compare(a.fields, b.fields);
            });
            for (CountedOrder& order : orders)
                application.layouts[index].flow.constructorOrders.append(FieldOrder { WTF::move(order.fields) });
        }
    }

    void computeFieldsStoredOnceByConstructors() const
    {
        struct StoreSeen {
            FunctionID function;
            ValueID store;
            bool isStoredOtherwise { false };

            void note(FunctionID otherFunction, ValueID otherStore)
            {
                isStoredOtherwise |= store && (function != otherFunction || store != otherStore);
                function = otherFunction;
                store = otherStore;
            }
        };
        struct Candidate : StoreSeen {
            LayoutID layout;
            uint32_t field { 0 };
        };

        BitVector hasObjectsBelow(numberOfLayouts);
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            if (!hasObjects(LayoutID(index)))
                continue;
            for (bool isProven : { true, false }) {
                auto above = [&](LayoutID layout) {
                    return isProven ? provenBaseOf(application, layout) : layoutExpectedAbove(program, layout);
                };
                LayoutID current = above(LayoutID(index));
                for (size_t steps = 0; current && steps < numberOfLayouts; ++steps, current = above(current))
                    hasObjectsBelow.quickSet(current.index());
            }
        }

        Vector<Candidate> candidates;
        HashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidatesUnderKey;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            LayoutID id(index);
            const Layout& layout = program.layouts[index];
            if (layout.kind != LayoutKind::Instance || !layout.constructor || !hasObjects(id) || hasObjectsBelow.quickGet(index) || !constructions.isFollowed(id) || !build.isEnabled(FlowResultKind::StoreCounts, index))
                continue;
            if (stores.hasEscapedObject(id) || stores.hasLostObject(id))
                continue;
            if (stores.getsNameNobodyLists(id) || heap.writtenUnder(layoutsOfObjects.objectsWith(id), copiedFromName) != ValueSets::nothing)
                continue;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                const Field& declaration = layout.fields[field];
                const FieldFlow& flow = application.layouts[index].fields[field].flow;
                if (!declaration.key || declaration.flags.contains(FieldFlags::IsInherited) || hasNodeOfItsOwn(declaration) || stores.mayGetAccessorUnder(id, declaration.key))
                    continue;
                bool staysProperty = flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined });
                if (flow.location.writers != WriterState::AllKnown || flow.location.stores != StoreCount::Unknown || flow.location.initialization != InitializationState::SetAtCreation || !staysProperty)
                    continue;
                candidatesUnderKey.add(declaration.key.index(), Vector<uint32_t, 2>()).iterator->value.append(candidates.size());
                Candidate candidate;
                candidate.layout = id;
                candidate.field = field;
                candidates.append(candidate);
            }
        }
        if (candidates.isEmpty())
            return;
        Vector<Vector<StoreSeen>> storesSeenBy(loops.width());
        stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
            if (key.name().kind != ValueFlowNameKind::Property)
                return;
            auto found = candidatesUnderKey.find(key.payload);
            if (found == candidatesUnderKey.end())
                return;
            Vector<StoreSeen>& storesSeen = storesSeenBy[worker];
            if (storesSeen.isEmpty())
                storesSeen.grow(candidates.size());
            const ValueFlowStatement& statement = writer.statement;
            bool storesByName = statement.kind == ValueFlowStatementKind::Operation && (statement.opcode == Opcode::PropSet || statement.opcode == Opcode::PropDefine);
            if (stores.placeholdersInSetOfWrite(key.set).almostEverything) {
                for (uint32_t index : found->value)
                    storesSeen[index].isStoredOtherwise = true;
            }
            stores.forEachLayoutListedInSetOfWrite(key.set, writer, [&](LayoutID layout) {
                auto candidate = std::ranges::lower_bound(found->value, layout, { }, [&](uint32_t index) {
                    return candidates[index].layout;
                });
                if (candidate == found->value.end() || candidates[*candidate].layout != layout)
                    return;
                storesSeen[*candidate].isStoredOtherwise |= !storesByName;
                storesSeen[*candidate].note(statement.function, statement.site);
            });
        });
        for (const Vector<StoreSeen>& storesSeen : storesSeenBy) {
            for (uint32_t index = 0; index < storesSeen.size(); ++index) {
                candidates[index].isStoredOtherwise |= storesSeen[index].isStoredOtherwise;
                if (storesSeen[index].store)
                    candidates[index].note(storesSeen[index].function, storesSeen[index].store);
            }
        }

        std::optional<ValueSetID> calleesOfCalls;
        Vector<uint8_t> isCalledDirectly;
        auto isOnlyConstructed = [&](FunctionID function) {
            if (!calleesOfCalls) {
                Vector<ValueSetID> calleesOfGroups = collectFromCallGroups<ValueSetID>(inputs, heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID, Vector<ValueSetID>& found) {
                    if (!isConstruction(key.callKind()))
                        found.append(key.set);
                });
                UnionOfSets callees;
                for (ValueSetID set : calleesOfGroups)
                    callees.add(set);
                calleesOfCalls = callees.take(heap.maker());
                isCalledDirectly.fill(0, program.functions.size());
                forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
                    if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::DirectCall })) || !isCompiled(FunctionID(index)))
                        return;
                    for (const Value& value : program.functions[index].values) {
                        if (accessClassOf(value.opcode) == AccessClass::DirectCall && value.function())
                            AtomicRef(isCalledDirectly[value.function().index()]).store(1, std::memory_order_relaxed);
                    }
                });
            }
            ValueFlowObjectID closure = inputs.universe().closureOf(function);
            return !inputs.hasEscaped(closure) && !inputs.isLost(closure) && !sets.contains(*calleesOfCalls, closure) && !isCalledDirectly[function.index()];
        };
        candidates.removeAllMatching([&](const Candidate& candidate) {
            if (candidate.isStoredOtherwise || !candidate.store)
                return true;
            switch (program.at(candidate.function).kind) {
            case FunctionKind::BaseConstructor:
            case FunctionKind::DerivedConstructor:
                return program.at(candidate.layout).constructor != candidate.function;
            case FunctionKind::Initializer:
                return false;
            case FunctionKind::Plain:
                return program.at(candidate.layout).constructor != candidate.function || !isOnlyConstructed(candidate.function);
            case FunctionKind::Arrow:
            case FunctionKind::Method:
            case FunctionKind::Getter:
            case FunctionKind::Setter:
            case FunctionKind::Module:
            case FunctionKind::CommonJsModule:
                return true;
            }
        });
        AOT::sort(candidates, { }, [](const Candidate& candidate) {
            return std::tuple { candidate.function, candidate.layout, candidate.field };
        });
        for (size_t first = 0; first < candidates.size();) {
            const Function& function = program.at(candidates[first].function);
            CFG cfg(function);
            BlockCycles cycles(cfg);
            BitVector isInCycle(function.values.size());
            for (uint32_t block = 0; block < function.blocks.size(); ++block) {
                if (!cycles.isInCycle(BlockID(block)))
                    continue;
                for (ValueID value : function.blocks[block].values)
                    isInCycle.quickSet(value.index());
            }
            size_t end = first;
            for (; end < candidates.size() && candidates[end].function == candidates[first].function; ++end) {
                if (!isInCycle.quickGet(candidates[end].store.index()))
                    application.layouts[candidates[end].layout.index()].fields[candidates[end].field].flow.location.stores = StoreCount::AtMostOnce;
            }
            first = end;
        }
    }

    void publishConstructions() const
    {
        constexpr uint8_t isStored = 1;
        constexpr uint8_t isLeftOut = 2;
        constexpr uint8_t isLeftWithoutValue = 4;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            if (build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
                application.layouts[index].flow.constructorOrders = constructions.ordersOf(LayoutID(index));
        }
        Vector<uint32_t> firstFieldOfLayout = numberFields(program);
        Vector<uint8_t> atBirth(FillWith { }, firstFieldOfLayout.last(), static_cast<uint8_t>(0));
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            LayoutID id(index);
            if (!hasObjects(id) || !build.isEnabled(FlowResultKind::Initialization, index))
                return;
            forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
                uint8_t said = (constructions.isStoredAtBirth(id, field.key, declaration) ? isStored : isLeftOut) | (constructions.isStoredWithValueAtBirth(id, field.key) ? 0 : isLeftWithoutValue);
                AtomicRef(atBirth[firstFieldOfLayout[declaration.layout.index()] + declaration.field]).fetch_or(said, std::memory_order_relaxed);
            });
        });
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            for (uint32_t field = 0; field < program.layouts[index].fields.size(); ++field) {
                uint8_t said = atBirth[firstFieldOfLayout[index] + field];
                FieldFlow& flow = application.layouts[index].fields[field].flow;
                if ((said & (isStored | isLeftOut)) == isStored && flow.occupancy == Occupancy::Unknown)
                    flow.occupancy = said & isLeftWithoutValue ? Occupancy::FilledAtBirth : Occupancy::FilledWithValueAtBirth;
            }
        });

        BitVector& functions = tables.functions.functionsSeeingOnlyFinishedObjects;
        functions = BitVector(program.functions.size());
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (build.isEnabled(FlowResultKind::Initialization, index) && constructions.seesOnlyFinishedObjects(FunctionID(index)))
                functions.quickSet(index);
        }
    }
};

} // anonymous namespace

ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const ValueFlowConstructions& constructions, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    FieldResults results { inputs, tables, constructions, usesOfHeap.heap, usesOfHeap.uses, *usesOfHeap.layoutsOfObjects, *usesOfHeap.stores, *usesOfHeap.ledgerOfFields };
    Vector<AllocatingOperation> allocatingOperations;
    runTasksInParallel(inputs.loops(), {
        [&] {
            usesOfHeap.stores->noteListedWrites();
            usesOfHeap.stores->setReceiversOfReads(usesOfHeap.uses.takeReceiversOfReads());
            times.didFinish("fields: stores by layout"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            results.publishConstructions();
            allocatingOperations = results.collectAllocatingOperations();
            results.computeInitialFieldOrders(allocatingOperations.span());
            timesBeside.didFinish("fields: initial orders, beside stores by layout"_s);
        },
    });
    times.didFinish("fields: the rest of the initial orders"_s);
    results.followCopiesAtBirth(allocatingOperations.span());
    results.nameKeysReadFromTargetsOfCopies();
    times.didFinish("fields: copies at birth"_s);
    results.findActorsOpenOnObjects();
    times.didFinish("fields: who is open on the objects of layouts"_s);
    ValueFlowFieldContents contents = results.computeDeclaredFields(allocatingOperations.span());
    allocatingOperations.clear();
    times.didFinish("fields: declared fields"_s);
    results.computeFieldsStoredOnceByConstructors();
    results.computeKeyEnumerations();
    times.didFinish("fields: stored once, enumerations"_s);

    results.layoutsDirectlyBelow.grow(results.numberOfLayouts);
    for (uint32_t index = 0; index < results.numberOfLayouts; ++index) {
        if (LayoutID base = provenBaseOf(results.application, LayoutID(index)))
            results.layoutsDirectlyBelow[base.index()].append(LayoutID(index));
    }
    KeysByLayout keysFilledByOutsideCreators;
    KeysByLayout appendedKeys = results.chooseAppendedKeys(keysFilledByOutsideCreators);
    times.didFinish("fields: appended keys chosen"_s);
    results.appendFields(appendedKeys, keysFilledByOutsideCreators, contents);
    times.didFinish("fields: fields appended"_s);
    results.computeAddedKeys(appendedKeys);
    times.didFinish("fields: added keys"_s);
    results.computeFieldCreations();
    times.didFinish("fields: creations"_s);
    return contents;
}

static void typeAppendedFields(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowApplication& application)
{
    bool hasAppendedFields = std::ranges::any_of(application.layouts, [](const FlowLayoutUpdate& update) {
        return !update.appendedFields.isEmpty();
    });
    if (!hasAppendedFields)
        return;

    Vector<BitVector> isDeclaredInFamilies = findAppendedFieldsDeclaredInFamilies(inputs.program, application.layouts, inputs.loops());
    ValueFlowPositionCounts counts(nullptr);
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        for (uint32_t index = 0; index < application.layouts.size(); ++index) {
            Vector<FlowAppendedField>& fields = application.layouts[index].appendedFields;
            if (fields.isEmpty())
                continue;
            ValueFlowPositions positions(inputs, arena, counts, index, &maker);
            for (uint32_t field = 0; field < fields.size(); ++field) {
                FieldFlow& flow = fields[field].flow;
                if (flow.location.writers != WriterState::AllKnown)
                    continue;
                ValueSetID stored = contents.storedInAppendedFields[index][field];
                ValueFlowPositionResult result = positions.resultAmongAllValues(maker.unite(stored, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent))));
                bool isGivenUnknownValues = contents.appendedFieldsWithUnknownValues[index].get(field);
                if (result.proof == ValueFlowProof::Proven && result.narrowed) {
                    const ValueFlowNumbers& storedNumbers = contents.numbersStoredInAppendedFields[index][field];
                    if (!storedNumbers.isEmpty())
                        storedNumbers.narrow(*result.narrowed);
                    fields[field].position.type = arena.intern(*result.narrowed);
                    fields[field].position.isProven = !isGivenUnknownValues;
                    fields[field].position.replacesExpectation = isGivenUnknownValues;
                }
                bool takesDeclaredType = !fields[field].position.isProven && !isGivenUnknownValues && isDeclaredInFamilies[index].get(field);
                if (inputs.build.isEnabled(FlowResultKind::HolderMutations, index) && !fields[field].accessor.isAccessor() && !takesDeclaredType)
                    flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
                if (flow.location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>() && !inputs.sets().isEmpty(stored)) {
                    flow.occupancy = Occupancy::AlwaysFilled;
                    flow.prototypeName = PrototypeNameState::Unknown;
                }
            }
        }
    }
    TypeRemap remap = phase.finish(StringRemap { }, &inputs.loops());
    for (FlowLayoutUpdate& update : application.layouts) {
        for (FlowAppendedField& field : update.appendedFields)
            remapTypes(field.position, remap);
    }
}

void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowResultTables& tables)
{
    FlowApplication& application = *tables.application;
    typeAppendedFields(inputs, contents, types, application);

    for (uint32_t index : findIndicesInParallel(inputs.loops(), application.layouts.size(), [&](unsigned index) {
        bool isNeverStoredOtherTypes = !application.layouts[index].flow.allocationClasses.isEmpty();
        if (!isNeverStoredOtherTypes)
            return false;
        forEachFieldOfObjectsWith(inputs.program, application, LayoutID(index), [&](FieldReference declaration, const Field&) {
            isNeverStoredOtherTypes &= flowOf(application, declaration).stability.contains(FieldStability::IsNeverStoredWithOtherType);
        });
        forEachOnProvenChain(application, LayoutID(index), [&](LayoutID current) {
            for (const FlowAppendedField& field : application.layouts[current.index()].appendedFields)
                isNeverStoredOtherTypes &= field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        });
        return isNeverStoredOtherTypes;
    })) {
        LayoutFlow& flow = application.layouts[index].flow;
        flow.impossibleMutations.add(ObjectMutation::StoreOfOtherType);
        for (AllocationClassID allocationClass : flow.allocationClasses)
            tables.sites.allocationClasses[allocationClass.index()].impossibleMutations.add(ObjectMutation::StoreOfOtherType);
    }
}

static bool leavesReadsToBeFound(ReadState reads)
{
    return reads == ReadState::OnlyCalled || reads == ReadState::OnlyAccessed;
}

static void keepFieldsOnlyCalledWhoseReadsHaveStamps(const ValueFlowResultInputs& inputs, FlowApplication& application, ValueFlowUsesOfHeap* usesOfHeap)
{
    struct Read {
        uint32_t key;
        uint32_t receivers;
        uint32_t statement;

        auto operator<=>(const Read&) const = default;
    };

    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    BitVector namesFieldOnlyCalled(program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; hasObjectsThatCodeAlters(layout.kind) && field < layout.fields.size(); ++field) {
            LocationFlow& location = application.layouts[index].fields[field].flow.location;
            if (!leavesReadsToBeFound(location.reads))
                continue;
            if (usesOfHeap)
                namesFieldOnlyCalled.quickSet(layout.fields[field].key.index());
            else
                location.reads = ReadState::Unknown;
        }
    }
    if (namesFieldOnlyCalled.isEmpty())
        return;

    const ValueFlowHeapView& heap = usesOfHeap->heap;
    StoresByLayout& stores = *usesOfHeap->stores;
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    ValueSetMakers makers(inputs);
    Vector<Read> readsWithoutStamp = collectWithMakers<Read>(inputs, makers, program.functions.size(), [&](unsigned body, ValueSetMaker& maker, Vector<Read>& found) {
        if (!inputs.isReached(FunctionID(body)))
            return;
        std::optional<BitVector> valuesBehindChecks;
        auto isRepeatedWithoutStamp = [&](ValueID read) {
            if (!valuesBehindChecks) {
                const Function& function = program.functions[body];
                BitVector checksThatTestNothing(function.values.size());
                for (const FlowClaimUpdate& claim : application.bodies[body].claims) {
                    if (claim.isRedundant || claim.arriving)
                        checksThatTestNothing.quickSet(claim.claim.index());
                }
                bool hasCheckThatStays = false;
                for (uint32_t index = 0; index < function.values.size() && !hasCheckThatStays; ++index)
                    hasCheckThatStays = function.values[index].opcode == Opcode::Check && !checksThatTestNothing.quickGet(index);
                valuesBehindChecks = hasCheckThatStays ? valuesTypedBehindChecks(program, function, CFG(function), &checksThatTestNothing) : BitVector();
            }
            return valuesBehindChecks->get(read.index());
        };
        for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index) {
            const ValueFlowStatement& statement = statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet)
                continue;
            PropertyKeyID key = program.at(statement.function).at(statement.site).key();
            if (!namesFieldOnlyCalled.quickGet(key.index()) || (findByValue(application.bodies[body].accesses.span(), statement.site, &FlowAccessStamp::value) && !isRepeatedWithoutStamp(statement.site)))
                continue;
            found.append(Read { key.index(), heap.possibleValuesOf(maker, inputs.bodies().operandsOf(statement)[OperandIndex::PropGet::object]).index(), index });
        }
    });
    sortAndRemoveDuplicates(loops, readsWithoutStamp, std::less<Read> { }, [](const Read& first, const Read& second) {
        return first.key == second.key && first.receivers == second.receivers;
    });
    Vector<ValueSetID> setsOfReceivers(readsWithoutStamp.size(), [&](size_t position) {
        return ValueSetID(readsWithoutStamp[position].receivers);
    });
    AOT::sort(setsOfReceivers, { }, [](ValueSetID set) { return set.index(); });
    removeRepeatedElements(setsOfReceivers);
    stores.lookAt(setsOfReceivers.span());

    Vector<uint32_t> firstReadOfName;
    for (uint32_t position = 0; position < readsWithoutStamp.size(); ++position) {
        if (!position || readsWithoutStamp[position].key != readsWithoutStamp[position - 1].key)
            firstReadOfName.append(position);
    }
    uint32_t numberOfNames = firstReadOfName.size();
    firstReadOfName.append(readsWithoutStamp.size());
    Vector<StoresByLayout::PlaceholdersInSet> placeholdersReadThrough(numberOfNames);
    forEachIndexInParallel(loops, numberOfNames, [&](unsigned name) {
        PropertyKeyID key(readsWithoutStamp[firstReadOfName[name]].key);
        StoresByLayout::PlaceholdersInSet& all = placeholdersReadThrough[name];
        BitVector layoutsSearched(program.layouts.size());
        for (const Read& read : readsWithoutStamp.span().subspan(firstReadOfName[name], firstReadOfName[name + 1] - firstReadOfName[name])) {
            StoresByLayout::PlaceholdersInSet placeholders = std::as_const(stores).placeholdersInSetOfWrite(ValueSetID(read.receivers));
            all.unknownObject |= placeholders.unknownObject;
            all.untrackedObject |= placeholders.untrackedObject;
            all.almostEverything |= placeholders.almostEverything;
            std::as_const(stores).forEachLayoutListedInSetOfWrite(ValueSetID(read.receivers), Writer { statements[read.statement] }, [&](LayoutID layout) {
                if (layoutsSearched.quickSet(layout.index()))
                    return;
                LayoutID holder = findOnProvenChain(application, layout, [&](LayoutID current) {
                    return program.at(current).findField(key).has_value();
                });
                if (!holder)
                    return;
                LocationFlow& location = flowOf(application, declarationOf(program, { holder, *program.at(holder).findField(key) })).location;
                if (leavesReadsToBeFound(location.reads))
                    location.reads = ReadState::Unknown;
            });
        }
    });
    NamesThroughPlaceholders namesRead;
    for (uint32_t name = 0; name < numberOfNames; ++name)
        namesRead.note(placeholdersReadThrough[name], readsWithoutStamp[firstReadOfName[name]].key);
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field& field) {
            AtomicRef reads(flowOf(application, declaration).location.reads);
            if (field.key && leavesReadsToBeFound(reads.load(std::memory_order_relaxed)) && namesRead.reach(stores, LayoutID(index), field.key.index()))
                reads.store(ReadState::Unknown, std::memory_order_relaxed);
        });
    });
}

void computeStoreNamingResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, FlowResultTables& tables)
{
    struct FieldUnderKey {
        uint32_t key;
        FieldReference declaration;
    };
    struct SeenByWorker {
        NamesThroughPlaceholders namesStored;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> nodesAndTypesOfReceiversSeen;
    };

    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    const ValueFlowSolver& solver = inputs.solver();
    FlowApplication& application = *tables.application;
    bool hasStamps = inputs.build.isEnabledEverywhere(FlowResultKind::ReceiverSets);
    keepFieldsOnlyCalledWhoseReadsHaveStamps(inputs, application, hasStamps ? &usesOfHeap : nullptr);
    if (!hasStamps)
        return;

    StoresByLayout& stores = *usesOfHeap.stores;
    Vector<Vector<FieldUnderKey>> fieldsOfObjectsWith(program.layouts.size());
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field& field) {
            fieldsOfObjectsWith[index].append({ field.key ? field.key.index() : NamesThroughPlaceholders::none, declaration });
        });
        AOT::stableSort(fieldsOfObjectsWith[index], { }, &FieldUnderKey::key);
    });
    Vector<uint32_t> firstFieldOfLayout = numberFields(program);
    Vector<uint8_t> isStoredWithoutName(FillWith { }, firstFieldOfLayout.last(), static_cast<uint8_t>(0));
    auto noteField = [&](const FieldUnderKey& field) {
        AtomicRef(isStoredWithoutName[firstFieldOfLayout[field.declaration.layout.index()] + field.declaration.field]).store(1, std::memory_order_relaxed);
    };
    auto namesLocation = [&](const ValueFlowStatement& statement, ValueFlowName name) {
        if (statement.kind != ValueFlowStatementKind::Operation || name.kind != ValueFlowNameKind::Property)
            return false;
        if (statement.opcode != Opcode::PropSet && statement.opcode != Opcode::PropDefine)
            return statement.opcode == Opcode::NewObject;
        return program.at(statement.function).at(statement.site).key().index() == name.payload && findByValue(application.bodies[statement.function.index()].accesses.span(), statement.site, &FlowAccessStamp::value);
    };
    Vector<SeenByWorker> seenBy(loops.width());
    Vector<uint8_t> nodesSeenWithoutTypeOfReceiver(FillWith { }, solver.graph().numberOfNodes() - solver.input().numberOfNodes, static_cast<uint8_t>(0));
    stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
        ValueFlowName name = key.name();
        ValueFlowNameShape shape = shapeOf(name.kind);
        bool mayBeAnyName = shape.mayBeAnyMember();
        if (!mayBeAnyName && (shape.reach != ValueFlowNameReach::OneKey || !shape.isValueOrAccessor()))
            return;
        if (namesLocation(writer.statement, name))
            return;
        uint32_t node = solver.indexOfMadeNodes(key);
        bool isSeen = writer.layoutsOfReceiver.empty() ? AtomicRef(nodesSeenWithoutTypeOfReceiver[node]).exchange(1, std::memory_order_relaxed) : !seenBy[worker].nodesAndTypesOfReceiversSeen.add(static_cast<uint64_t>(node) << 32 | writer.familyOfReceiver.index()).isNewEntry;
        if (isSeen)
            return;
        auto noteFieldsOf = [&](LayoutID layout) {
            const Vector<FieldUnderKey>& fields = fieldsOfObjectsWith[layout.index()];
            if (mayBeAnyName) {
                std::ranges::for_each(fields, noteField);
                return;
            }
            for (uint32_t keyOfField : { name.payload, NamesThroughPlaceholders::none })
                std::ranges::for_each(std::ranges::equal_range(fields, keyOfField, { }, &FieldUnderKey::key), noteField);
        };
        if (!writer.layoutsOfReceiver.empty()) {
            stores.forEachLayoutWrittenThroughSetOfWrite(key.set, writer, noteFieldsOf);
            return;
        }
        seenBy[worker].namesStored.note(stores.placeholdersInSetOfWrite(key.set), mayBeAnyName ? NamesThroughPlaceholders::none : name.payload);
        stores.forEachLayoutListedInSetOfWrite(key.set, writer, noteFieldsOf);
    });
    NamesThroughPlaceholders namesStored;
    for (const SeenByWorker& seen : seenBy)
        namesStored.takeFrom(seen.namesStored);
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        for (const FieldUnderKey& field : fieldsOfObjectsWith[index]) {
            if (namesStored.reach(stores, LayoutID(index), field.key))
                noteField(field);
        }
    });

    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; hasObjectsThatCodeAlters(layout.kind) && field < layout.fields.size(); ++field) {
            FieldFlow& flow = application.layouts[index].fields[field].flow;
            if (flow.location.writers == WriterState::AllKnown && flow.stability.contains(FieldStability::IsNeverRedefined) && !isStoredWithoutName[firstFieldOfLayout[index] + field] && !layout.fields[field].flags.contains(FieldFlags::IsInherited))
                flow.location.storeNaming = StoreNaming::EveryStoreNamesLocation;
        }
    });
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        for (size_t position = 0; stores.hasEscapedObject(LayoutID(index)) && position < fieldsOfObjectsWith[index].size(); ++position)
            AtomicRef(flowOf(application, fieldsOfObjectsWith[index][position].declaration).location.storeNaming).store(StoreNaming::Unknown, std::memory_order_relaxed);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
