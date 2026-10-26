/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"

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

static void forEachFieldOfObjectsWith(const Program& program, const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<void(FieldReference declaration, const Field&)> auto& functor)
{
    Vector<LayoutID, 4> layoutsBelow;
    Vector<FieldReference, 16> visited;
    forEachOnProvenChain(application, id, [&](LayoutID current) {
        const Layout& layout = program.at(current);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            bool isHidden = field.key && std::ranges::any_of(layoutsBelow, [&](LayoutID below) {
                return program.at(below).findField(field.key).has_value();
            });
            if (isHidden)
                continue;
            FieldReference declaration = declarationOf(program, { current, index });
            if (!layoutsBelow.isEmpty() && visited.contains(declaration))
                continue;
            visited.append(declaration);
            functor(declaration, field);
        }
        layoutsBelow.append(current);
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
    BitVector isBornWithCopies { };

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
        return collectInParallel<LayoutID>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<LayoutID>& layouts) {
            ValueFlowObjectID object(index);
            if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object) || isSiteOfTypedResultThatNothingAllocates(inputs, object))
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
                    if (value.opcode == Opcode::PropSet && isNamedOnce && value.operand(OperandIndex::PropSet::object) == operand && birth.hasOnlyNamedData && value.key() && birth.definedKeys.contains(value.key()))
                        continue;
                    if (!isPieceOfLiteral(value, operand) || !isNamedOnce) {
                        if (value.opcode == Opcode::FinishObject)
                            born[open].object = valueID;
                        openBirthOf[operand.index()] = none;
                        continue;
                    }
                    if (value.opcode == Opcode::PropDefine && value.key()) {
                        birth.definesKeysLeftOutByAllocation |= !birth.definedKeys.contains(value.key());
                        birth.definedKeys.append(value.key());
                    } else
                        birth.hasOnlyNamedData = false;
                    if (value.opcode == Opcode::CopyDataProperties && value.excludedOperands().empty())
                        birth.copies.append({ valueID, OperandIndex::CopyDataProperties::source, false });
                }
                if (value.opcode == Opcode::NewObject && canBeBornWithCopies(value.layout())) {
                    BirthWithCopies birth { .function = id, .allocation = valueID, .layout = value.layout() };
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

    // The own enumerable data properties that the object is never without, once it is born. What happens to another object of its layout takes none from it.
    Vector<PropertyKeyID> keysKeptBy(ValueSetMaker& maker, const BitVector& hasOtherSites, ValueFlowObjectID object, LayoutID id) const
    {
        Vector<PropertyKeyID> keys;
        LayoutKind kind = program.at(id).kind;
        ObjectsOfLayout reached = inputs.hasEscaped(object) || inputs.isLost(object) ? ObjectsOfLayout::All : ObjectsOfLayout::Followed;
        if ((kind != LayoutKind::Record && kind != LayoutKind::Instance) || !hasObjects(id) || stores.getsCopiedProperties(id, reached) || stores.hasObjectOfCodeWithoutSolve(id))
            return keys;
        OptionSet<ObjectMutation> possible = reached == ObjectsOfLayout::All ? possibleMutationsOf(id) : markedMutationsOf(inputs, object);
        if (possible.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ReachUnknownJavaScript }))
            return keys;
        if (kind == LayoutKind::Record ? hasOtherSites.quickGet(id.index()) : !constructions.isFollowed(id))
            return keys;

        bool isBornHere = isBornWithCopies.get(id.index());
        if (isBornHere)
            keys = keysHeldSinceBirth[id.index()];
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
        auto noteOtherBirth = [&](LayoutID layout, std::span<const uint32_t> fields) {
            Vector<PropertyKeyID, 8> keys;
            for (uint32_t field : fields) {
                if (PropertyKeyID key = program.at(layout).fields[field].key)
                    keys.append(key);
            }
            AOT::sort(keys);
            keepCommonKeys(keysOfOtherBirths[layout.index()], keys);
        };
        for (const AllocatingOperation& operation : operations) {
            const Value& value = program.at(operation.function).at(operation.value);
            if (value.opcode == Opcode::NewObject) {
                if (isBornWithCopies.get(value.layout().index()) && !isListed(operation.function, operation.value))
                    noteOtherBirth(value.layout(), value.fields());
                continue;
            }
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t position, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (object && object->layout && isBornWithCopies.get(object->layout.index()) && (position || !isListed(operation.function, operation.value)))
                    noteOtherBirth(object->layout, fieldsFilledBy(program, program.at(object->layout), *object).span());
            });
        }

        AOT_THRESHOLD(unsigned, mostRoundsOverCopies, 4);
        BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
        ValueSetMaker& maker = borrowedMaker.get();
        Vector<ValueSetID> objectsAboveLayout(FillWith { }, numberOfLayouts, ValueSetID());
        for (unsigned round = 0; round <= mostRoundsOverCopies; ++round) {
            Vector<std::optional<Vector<PropertyKeyID>>> held = keysOfOtherBirths;
            for (const BirthWithCopies& birth : births) {
                Vector<PropertyKeyID> keys(birth.definedKeys.span());
                for (const SourceOfCopyAtBirth& source : round ? birth.sources.span() : std::span<const SourceOfCopyAtBirth>()) {
                    for (PropertyKeyID key : keysSurelyCopiedFrom(maker, hasOtherSites, source.objects)) {
                        if (std::ranges::contains(source.excludedKeys, key))
                            continue;
                        if (!source.isAssigned || inheritsNothingUnder(maker, birth.layout, objectsAboveLayout[birth.layout.index()], key))
                            keys.append(key);
                    }
                }
                AOT::sort(keys);
                removeRepeatedElements(keys);
                keepCommonKeys(held[birth.layout.index()], keys);
            }
            bool hasChanged = false;
            for (size_t layout : isBornWithCopies) {
                hasChanged |= keysHeldSinceBirth[layout] != *held[layout];
                keysHeldSinceBirth[layout] = WTF::move(*held[layout]);
            }
            if (round && !hasChanged)
                break;
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
        for (LayoutID layout : layoutsOfOtherSites([&](ValueFlowObjectID object, const ValueFlowSite& site) {
            return !constructions.isFollowed(site.layout) && hasObjects(site.layout) && (site.origin == ValueFlowSiteOrigin::TypedResult || isCompiled(inputs.universe().functionOf(object)));
        }))
            layoutsOfOtherObjects.quickSet(layout.index());
        for (size_t layout : layoutsOfOtherObjects) {
            forEachFieldOfObjectsWith(program, application, LayoutID(layout), [&](FieldReference declaration, const Field&) {
                missing.append(declaration);
            });
        }
        return missing;
    }

    bool mayFindNameIn(ValueSetMaker& maker, ValueSetID objects, PropertyKeyID key) const
    {
        const ValueFlowUniverse& universe = inputs.universe();
        if (ValueSets::isComplement(objects) || sets.meet(objects, universe.placeholdersAlone()))
            return true;
        const ValueFlowMembersUnderKey& members = universe.builtinTables().heap().under(key);
        if (sets.meet(objects, members.holders) || sets.meet(objects, members.holdersOfUndeclaredProperties))
            return true;
        for (ValueFlowName name : { ValueFlowName::property(key), ValueFlowName::getter(key), ValueFlowName::setter(key), unknownKeyName, ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey), ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey), copiedFromName }) {
            if (heap.writtenUnder(maker, objects, name) != ValueSets::nothing)
                return true;
        }
        bool mayHoldMore = false;
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            ValueFlowObjectKind kind = universe.kindOf(object);
            if (kind == ValueFlowObjectKind::Site || kind == ValueFlowObjectKind::Instance) {
                LayoutID layout = universe.layoutOf(object);
                mayHoldMore |= !layout || !hasObjectsThatCodeAlters(program.at(layout).kind) || inputs.classOf(object);
            } else if (kind == ValueFlowObjectKind::Prototype)
                mayHoldMore |= key == universe.builtinTables().heap().constructorKey();
            else
                mayHoldMore |= kind != ValueFlowObjectKind::Builtin && kind != ValueFlowObjectKind::ModuleBuiltin;
        });
        return mayHoldMore;
    }

    ValueSetID objectsAbove(ValueSetMaker& maker, LayoutID id, ValueSetID& above) const
    {
        if (!above)
            above = heap.chainOf(maker, heap.prototypesOf(maker, layoutsOfObjects.objectsWith(id)));
        return above;
    }

    bool inheritsNothingUnder(ValueSetMaker& maker, LayoutID id, ValueSetID& above, PropertyKeyID key) const
    {
        if (program.isIndexLike(key) || stores.hasObjectOfCodeWithoutSolve(id) || stores.getsCopiedProperties(id) || mayFindNameIn(maker, objectsAbove(maker, id, above), key))
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

    void noteWhatNeverHappens(FieldFlow& flow, uint32_t layout, OptionSet<ObjectMutation> possible, bool hasListedStores, bool isFilledAtCreation) const
    {
        if (!possible.contains(ObjectMutation::ReachUnknownJavaScript) && hasListedStores && build.isEnabled(FlowResultKind::StoreCounts, layout))
            flow.location.writers = WriterState::AllKnown;
        if (build.isEnabled(FlowResultKind::HolderMutations, layout)) {
            if (!possible.contains(ObjectMutation::DeleteField))
                flow.stability.add(FieldStability::IsNeverDeleted);
            if (!possible.contains(ObjectMutation::RedefineProperty))
                flow.stability.add(FieldStability::IsNeverRedefined);
        }
        if (isFilledAtCreation && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, layout))
            flow.location.initialization = InitializationState::SetAtCreation;
    }

    ValueFlowFieldContents computeDeclaredFields(std::span<const AllocatingOperation> allocatingOperations) const
    {
        struct FactsOfField {
            OptionSet<FieldFact> facts;
            OptionSet<ObjectMutation> possibleMutations;
        };
        struct HeldField : FactsOfField {
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
            OptionSet<ObjectMutation> possible = getsCopiedProperties ? allObjectMutations : marked;
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
                if (field.key) {
                    if (isDeletedByStatementsAlone && !getsCopiedProperties && !isPutDown(ValueFlowNameKind::DeletedMember, ValueFlowNameKind::DeletedMemberUnderUnknownKey, field.key))
                        held.possibleMutations.remove(ObjectMutation::DeleteField);
                    if (!isSeenByOthers && !getsCopiedProperties && !isPutDown(ValueFlowNameKind::RedefinedMember, ValueFlowNameKind::RedefinedMemberUnderUnknownKey, field.key))
                        held.possibleMutations.remove(ObjectMutation::RedefineProperty);
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
                    held.storedNumbers = stores.numbersStoredUnder(id, field.key);
                }
                fieldsHeldByObjectsWith[index].append(WTF::move(held));
            });
        });

        for (Vector<HeldField>& heldFields : fieldsHeldByObjectsWith) {
            for (HeldField& held : heldFields) {
                uint32_t layout = held.declaration.layout.index();
                uint32_t field = held.declaration.field;
                FactsOfField& all = known[layout][field];
                if (held.facts.contains(FieldFact::IsAskedForAccessor))
                    uniteAccessors(application.layouts[layout].fields[field].accessor, WTF::move(held.accessor), !all.facts.contains(FieldFact::IsAskedForAccessor));
                all.facts.add(held.facts);
                all.possibleMutations.add(held.possibleMutations);
                contents.stored[layout][field] = makers.of(0).unite(contents.stored[layout][field], held.stored);
                contents.storedNumbers[layout][field].unite(held.storedNumbers);
            }
            heldFields.clear();
        }

        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            const Layout& layout = program.layouts[index];
            if (!hasObjectsThatCodeAlters(layout.kind))
                return;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                auto [facts, possible] = known[index][field];
                if (!facts.contains(FieldFact::HasObjects) || layout.fields[field].flags.contains(FieldFlags::IsInherited))
                    continue;
                FieldFlow& flow = application.layouts[index].fields[field].flow;
                noteWhatNeverHappens(flow, index, possible, true, !facts.contains(FieldFact::IsMissingAtCreation));
                if (!facts.contains(FieldFact::MayInheritName))
                    flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
                if (hasNodeOfItsOwn(layout.fields[field])) {
                    contents.stored[index][field] = inputs.solver().possibleValuesOf(inputs.bodies().nodes().field(LayoutID(index), field));
                    contents.storedNumbers[index][field] = ValueFlowNumbers::any();
                    continue;
                }
                if (flow.location.writers != WriterState::AllKnown)
                    continue;
                if (!facts.contains(FieldFact::IsStoredAfterCreation) && !possible.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty })) {
                    flow.location.stores = StoreCount::AtMostOnce;
                    flow.location.isUnchangedDuringActivation = true;
                }
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
            if (!isSeenByFollowedCodeAlone(id) || possibleMutationsOf(id).contains(ObjectMutation::BecomePrototype) || uses.mayBeEnumerated(layoutsOfObjects.objectsWith(id)))
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
        for (ValueFlowName name : { ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
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
            for (ValueFlowName name : { ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
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
            if (isStoredAbove(ValueFlowName::property(key)) || isStoredAbove(ValueFlowName::getter(key)))
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

    KeysByLayout chooseAppendedKeys() const
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
            uniteSortedKeys(keysByLayout[index], accessors);
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
            keysByLayout[id.index()].removeAllMatching([&](PropertyKeyID key) {
                auto declarers = layoutsWithField.find(key.index());
                PropertyKeyKind kind = program.at(key).kind;
                bool canBeDeclared = kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol && kind != PropertyKeyKind::ModulePrivateName;
                bool isOutsideFamilies = canBeDeclared && !families.isEmpty() && (declarers == layoutsWithField.end() || (!haveCommonMember(views, declarers->value.span()) && std::ranges::none_of(families, [&](LayoutSetID family) {
                    return haveCommonMember(program.at(family).members.span(), declarers->value.span());
                })));
                if (isOutsideFamilies || std::ranges::binary_search(keysDeclaredBelow[id.index()], key) || !canNameField(key) || holdsField(id, key))
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
            uniteSortedKeys(keys, accessors);
            keepMostAccessedKeys(counts, LimitID::FieldsInKeyOrder, layout, largestFieldCountBelow + numberOfInheritedKeys, keys);
        }
        return keysByLayout;
    }

    void appendFields(const KeysByLayout& keysByLayout, ValueFlowFieldContents& contents) const
    {
        ValueSetMakers makers(inputs);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            ValueSetMaker& maker = makers.of(worker);
            for (PropertyKeyID key : keysByLayout[index]) {
                FlowAppendedField field;
                field.key = key;
                OptionSet<ObjectMutation> possible;
                ValueSetID stored = ValueSets::nothing;
                ValueFlowNumbers storedNumbers;
                bool isStoredBeforeObjectsLeak = true;
                bool inheritsNothing = build.isEnabled(FlowResultKind::OwnNames, index);
                bool isFirst = true;
                forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                    if (!hasObjects(below))
                        return;
                    ValueSetID above;
                    inheritsNothing = inheritsNothing && inheritsNothingUnder(maker, below, above, key);
                    possible.add(stores.getsCopiedProperties(below) ? allObjectMutations : possibleMutationsOf(below));
                    stored = maker.unite(stored, stores.storedUnder(maker, below, key));
                    storedNumbers.unite(stores.numbersStoredUnder(below, key));
                    isStoredBeforeObjectsLeak &= holdsSinceBirth(below, key) || (constructions.isFollowed(below) && constructions.isStoredBeforeObjectLeaks(below, key, { }));
                    uniteAccessors(field.accessor, stores.accessorDefinedUnder(maker, below, key), std::exchange(isFirst, false));
                });
                noteWhatNeverHappens(field.flow, index, possible, !sets.isEmpty(stored), isStoredBeforeObjectsLeak);
                if (inheritsNothing)
                    field.flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
                application.layouts[index].appendedFields.append(WTF::move(field));
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

    void computeAddedKeys(const KeysByLayout& appendedKeys) const
    {
        BitVector isConstructedByNativeCode = findLayoutsConstructedByNativeCode();
        const Vector<PropertyKeyID>& sharedKeys = stores.keysStoredThroughPlaceholders();
        tables.wholeProgram.keysStoredThroughUnknownReceivers = sharedKeys;
        tables.wholeProgram.keysDefinedOnObjectsWithoutKnownLayout = stores.keysDefinedOnObjectsWithoutKnownLayout();
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
            LayoutID id(index);
            LayoutFlow& flow = application.layouts[index].flow;
            LayoutKind kind = program.layouts[index].kind;
            if (!hasObjects(id) || flow.allocationClasses.isEmpty() || (kind != LayoutKind::Record && kind != LayoutKind::Instance))
                return;
            bool listsEveryName = !stores.getsNameNobodyLists(id) && !isConstructedByNativeCode.quickGet(index) && !possibleMutationsOf(id).contains(ObjectMutation::ReachUnknownJavaScript);
            Vector<PropertyKeyID> addedKeys;
            bool mayGainSharedKey = false;
            if (listsEveryName) {
                auto isField = [&](PropertyKeyID key) {
                    return !std::ranges::binary_search(tables.wholeProgram.keysOfSeveralSymbols, key) && holdsField(id, key, &appendedKeys);
                };
                addedKeys = stores.keysStoredIntoKnownObjects(id);
                addedKeys.removeAllMatching(isField);
                mayGainSharedKey = stores.isReachedThroughPlaceholders(id) && !std::ranges::all_of(sharedKeys, isField);
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
        const Layout& layout = program.at(id);
        bool hasListedOrders = layout.kind == LayoutKind::Record ? !layoutsOfObjectsWithoutListedOrder.quickGet(id.index()) : layout.kind == LayoutKind::Instance && constructions.createsFieldsOnlyInListedOrders(id);
        if (!hasListedOrders || possibleMutationsOf(id).contains(ObjectMutation::AddProperty) || !application.layouts[id.index()].appendedFields.isEmpty() || isBornWithCopies.get(id.index()))
            return false;
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            if (field.flags.contains(FieldFlags::IsInherited) || application.layouts[id.index()].fields[index].flow.location.initialization == InitializationState::SetAtCreation)
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
            bool isCreatedAtConstruction = true;
            forEachOnProvenChain(application, LayoutID(index), [&](LayoutID current) {
                BaseState baseState = application.layouts[current.index()].flow.baseState;
                bool createsFieldsInObjectsBelow = current != LayoutID(index) && !hasObjects(current) && constructions.createsFieldsOnlyInListedOrders(current);
                isCreatedAtConstruction &= (baseState == BaseState::Layout || baseState == BaseState::None) && (createsOwnFields[current.index()] || createsFieldsInObjectsBelow);
            });
            if (isCreatedAtConstruction)
                application.layouts[index].flow.fieldCreation = FieldCreation::OnlyAtConstruction;
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
    FieldResults results { inputs, tables, constructions, usesOfHeap.heap, usesOfHeap.uses, *usesOfHeap.layoutsOfObjects, *usesOfHeap.stores };
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
    times.didFinish("fields: copies at birth"_s);
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
    KeysByLayout appendedKeys = results.chooseAppendedKeys();
    times.didFinish("fields: appended keys chosen"_s);
    results.appendFields(appendedKeys, contents);
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
                if (result.proof == ValueFlowProof::Proven && result.narrowed) {
                    const ValueFlowNumbers& storedNumbers = contents.numbersStoredInAppendedFields[index][field];
                    if (!storedNumbers.isEmpty())
                        storedNumbers.narrow(*result.narrowed);
                    fields[field].position.type = arena.intern(*result.narrowed);
                    fields[field].position.isProven = true;
                }
                bool takesDeclaredType = !fields[field].position.isProven && isDeclaredInFamilies[index].get(field);
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
