/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTIRProgram.h"
#include "AOTLayoutUses.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessCache.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned minFieldsToIndex = 4;
static constexpr unsigned minStaticFieldsToIndex = 8;

void Layout::indexFields()
{
    m_indexedFieldCount = static_cast<uint32_t>(fields.size());
    m_fieldsByKey.clear();
    m_keysByHash = 0;
    if (fields.size() >= minFieldsToIndex)
        m_fieldsByKey.fill(IndexedField { }, WTF::roundUpToPowerOfTwo(2 * fields.size()));
    for (uint32_t i = 0; i < fields.size(); ++i)
        addToIndex(i);

    m_indexedStaticFieldCount = static_cast<uint32_t>(staticFields.size());
    m_staticFieldsByKey.clear();
    if (staticFields.size() < minStaticFieldsToIndex)
        return;
    m_staticFieldsByKey.fill(IndexedField { }, WTF::roundUpToPowerOfTwo(2 * staticFields.size()));
    uint32_t mask = static_cast<uint32_t>(m_staticFieldsByKey.size()) - 1;
    for (uint32_t i = 0; i < staticFields.size(); ++i) {
        PropertyKeyID key = staticFields[i].key;
        if (!key)
            continue;
        for (uint32_t place = hashOf(key) & mask;; place = (place + 1) & mask) {
            IndexedField& indexed = m_staticFieldsByKey[place];
            if (indexed.key == key)
                break;
            if (!indexed.key) {
                indexed = { key, i };
                break;
            }
        }
    }
}

std::optional<uint32_t> Layout::findStaticField(PropertyKeyID key) const
{
    RELEASE_ASSERT(m_indexedStaticFieldCount == staticFields.size());
    if (!key)
        return std::nullopt;
    if (m_staticFieldsByKey.isEmpty()) {
        for (uint32_t i = 0; i < staticFields.size(); ++i) {
            if (staticFields[i].key == key)
                return i;
        }
        return std::nullopt;
    }
    uint32_t mask = static_cast<uint32_t>(m_staticFieldsByKey.size()) - 1;
    for (uint32_t place = hashOf(key) & mask;; place = (place + 1) & mask) {
        const IndexedField& indexed = m_staticFieldsByKey[place];
        if (indexed.key == key)
            return indexed.field;
        if (!indexed.key)
            return std::nullopt;
    }
}

void Layout::addToIndex(uint32_t field)
{
    PropertyKeyID key = fields[field].key;
    if (!key)
        return;
    uint32_t hash = hashOf(key);
    m_keysByHash |= 1ull << (hash & 63);
    if (m_fieldsByKey.isEmpty())
        return;
    uint32_t mask = static_cast<uint32_t>(m_fieldsByKey.size()) - 1;
    for (uint32_t place = (hash >> 6) & mask;; place = (place + 1) & mask) {
        IndexedField& indexed = m_fieldsByKey[place];
        if (indexed.key == key)
            return;
        if (!indexed.key) {
            indexed = { key, field };
            return;
        }
    }
}

uint32_t Layout::appendField(Field&& field)
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    uint32_t index = static_cast<uint32_t>(fields.size());
    fields.append(WTF::move(field));
    if (fields.size() >= minFieldsToIndex && 2 * fields.size() > m_fieldsByKey.size()) {
        indexFields();
        return index;
    }
    ++m_indexedFieldCount;
    addToIndex(index);
    return index;
}

const Field& fieldAt(const Program& program, FieldReference reference)
{
    return program.at(reference.layout).fields[reference.field];
}

FieldReference declarationOf(const Program& program, FieldReference reference)
{
    for (size_t steps = 0; fieldAt(program, reference).flags.contains(FieldFlags::IsInherited); ++steps) {
        RELEASE_ASSERT(steps < program.layouts.size());
        const Field& field = fieldAt(program, reference);
        reference = { field.originLayout, *field.originField };
    }
    return reference;
}

const FieldFlow& flowOf(const Program& program, FieldReference reference)
{
    return fieldAt(program, declarationOf(program, reference)).flow;
}

bool hasOnlyCompiledBases(const Program& program, LayoutID id)
{
    LayoutID current = declaredLayoutOf(program, id);
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (!layout.has(LayoutFlags::HasHeritage))
            return true;
        current = layout.base;
    }
    return false;
}

bool isNeverRedefined(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).stability.contains(FieldStability::IsNeverRedefined);
}

bool isNeverDeleted(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).stability.contains(FieldStability::IsNeverDeleted);
}

bool isNeverMovedOutByStore(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).stability.contains(FieldStability::IsNeverStoredWithOtherType);
}

bool slotTakesEveryValue(const Program& program, const Field& field)
{
    if (field.form.contains(FieldForm::IsAccessor) || field.flags.contains(FieldFlags::IsEnforced) || field.flow.location.deferral != ClosureDeferral::Unknown)
        return false;
    return field.declared.isExpected() && slotTypeOf(program, field) == TypeTable::anything;
}

bool staysInSlot(const Program& program, FieldReference reference)
{
    return isNeverRedefined(program, reference) && (isNeverMovedOutByStore(program, reference) || slotTakesEveryValue(program, fieldAt(program, declarationOf(program, reference))));
}

bool staysFilled(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).occupancy == Occupancy::AlwaysFilled && isNeverDeleted(program, reference) && staysInSlot(program, reference);
}

bool isFilledAtBirth(const Program& program, FieldReference reference)
{
    Occupancy occupancy = flowOf(program, reference).occupancy;
    return occupancy == Occupancy::FilledAtBirth || occupancy == Occupancy::FilledWithValueAtBirth;
}

bool keepsStateOfBirth(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).occupancy != Occupancy::Unknown && isNeverDeleted(program, reference) && staysInSlot(program, reference);
}

bool isFilledWithValueAtBirth(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).occupancy == Occupancy::FilledWithValueAtBirth;
}

static bool findsFieldFilledAtBirth(const Program& program, LayoutID id)
{
    for (uint32_t field = 0; field < program.at(id).fields.size(); ++field) {
        if (isFilledAtBirth(program, { id, field }))
            return true;
    }
    return false;
}

bool hasFieldsFilledAtBirth(const Program& program, LayoutID id)
{
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || TypeTable::isProvisional(id) || program.at(id).fields.size() < PropertyAccessCache::fewestFields)
        return findsFieldFilledAtBirth(program, id);
    return findOrCompute(cache->layoutsFilledAtBirth, id, computeHash(id), [&] {
        return PropertyAccessCache::FilledAtBirth { id, findsFieldFilledAtBirth(program, id) };
    }).value;
}

bool isNeverStored(const Program& program, FieldReference reference)
{
    const Field& declaration = fieldAt(program, declarationOf(program, reference));
    const LocationFlow& location = declaration.flow.location;
    return location.stores == StoreCount::Never && (location.writers == WriterState::AllKnown || declaration.flags.contains(FieldFlags::IsMethod));
}

static LayoutID baseOf(const Layout& layout, BaseChain chain)
{
    switch (layout.flow.baseState) {
    case BaseState::Layout:
        return layout.flow.base;
    case BaseState::Unknown:
        return chain == BaseChain::Predicted ? predictedHeritageOf(layout) : LayoutID();
    case BaseState::None:
    case BaseState::BuiltinClass:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<FieldReference> findFieldByWalk(const Program& program, LayoutID id, PropertyKeyID key, BaseChain chain)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (auto field = layout.findField(key))
            return FieldReference { current, *field };
        current = baseOf(layout, chain);
    }
    return std::nullopt;
}

// All classes of a row from one that is asked up to the one that holds the field have the same answer. A walk that has gone some way keeps the answer for one in every few classes that it passes, and ends at the first for which it is kept.
static NEVER_INLINE std::optional<FieldReference> findFieldInLongRow(const Program& program, LayoutID id, PropertyKeyID key, BaseChain chain)
{
    PropertyAccessCache* cache = TypeTable::isProvisional(key) ? nullptr : program.propertyAccessCache();
    if (!cache)
        return findFieldByWalk(program, id, key, chain);
    Vector<LayoutID, 8> passed;
    auto isAnswerFor = [&](LayoutID wanted) {
        return [wanted, key, chain](const PropertyAccessCache::FieldAbove& entry) {
            return entry.key == PropertyAccessCache::FieldAboveKey { wanted, key, chain };
        };
    };
    auto keep = [&](std::optional<FieldReference> answer) {
        for (LayoutID layout : passed)
            cache->fieldsAbove.add(computeHash(layout, key, chain), PropertyAccessCache::FieldAbove { { layout, key, chain }, answer }, isAnswerFor(layout));
        RELEASE_ASSERT(!cache->checksAnswers || answer == findFieldByWalk(program, id, key, chain));
        return answer;
    };
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (auto field = layout.findField(key))
            return keep(FieldReference { current, *field });
        if (!TypeTable::isProvisional(current)) {
            if (const PropertyAccessCache::FieldAbove* kept = cache->fieldsAbove.find(computeHash(current, key, chain), isAnswerFor(current)))
                return keep(kept->value);
            if (steps % PropertyAccessCache::classesBetweenFieldsKept == PropertyAccessCache::classesBetweenFieldsKept - 1)
                passed.append(current);
        }
        current = baseOf(layout, chain);
    }
    return keep(std::nullopt);
}

std::optional<FieldReference> findField(const Program& program, LayoutID id, PropertyKeyID key, BaseChain chain)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < PropertyAccessCache::fewestClassesAbove; ++steps) {
        const Layout& layout = program.at(current);
        if (auto field = layout.findField(key))
            return FieldReference { current, *field };
        current = baseOf(layout, chain);
    }
    return current ? findFieldInLongRow(program, current, key, chain) : std::nullopt;
}

LayoutID expectedBaseOf(const Layout& layout)
{
    switch (layout.flow.baseState) {
    case BaseState::Layout:
        return layout.flow.base;
    case BaseState::Unknown:
        return layout.base;
    case BaseState::None:
        return layout.classFlow.prototypeBase;
    case BaseState::BuiltinClass:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LayoutID predictedHeritageOf(const Layout& layout)
{
    return layout.has(LayoutFlags::HasHeritage) ? layout.base : LayoutID();
}

// All classes of a row have the answer of the class at its upper end. A walk that has gone some way keeps the answer for the classes that it passes from there on, and ends at the first for which it is kept.
static NEVER_INLINE BuiltinClassID builtinBaseClassOfLongRow(const Program& program, LayoutID id)
{
    PropertyAccessCache* cache = program.propertyAccessCache();
    Vector<LayoutID, 8> passed;
    auto isAnswerFor = [](LayoutID wanted) {
        return [wanted](const PropertyAccessCache::BuiltinBase& entry) {
            return entry.key == wanted;
        };
    };
    auto keep = [&](BuiltinClassID answer) {
        for (LayoutID layout : passed)
            cache->builtinBases.add(computeHash(layout), PropertyAccessCache::BuiltinBase { layout, answer }, isAnswerFor(layout));
        return answer;
    };
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (cache && !TypeTable::isProvisional(current)) {
            if (const PropertyAccessCache::BuiltinBase* kept = cache->builtinBases.find(computeHash(current), isAnswerFor(current)))
                return keep(kept->value);
            passed.append(current);
        }
        const LayoutFlow& flow = program.at(current).flow;
        if (flow.baseState == BaseState::BuiltinClass)
            return keep(flow.baseClass);
        current = baseOf(program.at(current), BaseChain::Proven);
    }
    return keep({ });
}

BuiltinClassID builtinBaseClassOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < PropertyAccessCache::fewestClassesAbove; ++steps) {
        const LayoutFlow& flow = program.at(current).flow;
        if (flow.baseState == BaseState::BuiltinClass)
            return flow.baseClass;
        current = baseOf(program.at(current), BaseChain::Proven);
    }
    return current ? builtinBaseClassOfLongRow(program, current) : BuiltinClassID();
}

bool isEvaluatedOnce(const Program& program, const Layout& layout)
{
    if (layout.classFlow.evaluations != ExecutionCount::AtMostOnce)
        return false;
    if (!layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && program.at(layout.constructor).captures.isEmpty();
}

bool guardsAreNeverSet(const Program& program, const Layout& layout)
{
    return layout.has(LayoutFlags::IsConstructorFunction) && !isEvaluatedOnce(program, layout);
}

// The run time reads the prototype property at the first allocation of a site.
bool hasOnePrototypeObject(const Program& program, LayoutID allocatedClass)
{
    const Layout& constructed = program.at(declaredLayoutOf(program, allocatedClass));
    if (!isEvaluatedOnce(program, constructed))
        return false;
    const ClassFlow& classFlow = constructed.classFlow;
    return !constructed.has(LayoutFlags::IsConstructorFunction) || classFlow.prototypeProperty == MemberMutability::Fixed || classFlow.hasAssignedPrototype;
}

bool prototypePropertyMayEndGuards(const Program& program, FunctionID id)
{
    LayoutID constructed = program.at(id).constructedClass;
    if (!constructed)
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == id && !isEvaluatedOnce(program, layout);
}

bool hasSingletonClosure(const Program& program, FunctionID id)
{
    LayoutID constructed = program.at(id).constructedClass;
    if (!constructed)
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == id && isEvaluatedOnce(program, layout);
}

bool namesClosureOfTable(const Program& program, const Value& closure)
{
    ASSERT(closure.opcode == Opcode::NewClosure);
    return closure.isHeldByClosureTable() || hasSingletonClosure(program, closure.function());
}

Layout createDerivedLayout(const Program& program, LayoutID base)
{
    const Layout& declared = program.at(base);
    Layout layout;
    layout.kind = declared.kind;
    layout.name = declared.name;
    layout.module = declared.module;
    layout.position = declared.position;
    layout.flags = LayoutFlags::IsDerived;
    layout.flow.baseState = BaseState::Layout;
    layout.flow.base = base;
    return layout;
}

LayoutID declaredLayoutOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    while (current && program.at(current).isDerived())
        current = program.at(current).flow.base;
    return current;
}

Vector<LayoutSetID, 2> declaredFamiliesOf(const Program& program, LayoutID id)
{
    Vector<LayoutSetID, 2> families;
    for (LayoutID supertype : program.at(declaredLayoutOf(program, id)).supertypes) {
        if (program.at(supertype).kind == LayoutKind::Record)
            families.append(program.declaredMembersOf(supertype));
    }
    return families;
}

Vector<Vector<LayoutID>> declaredViewsOf(const Program& program, const BitVector& declaredLayouts)
{
    Vector<Vector<LayoutID>> views(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID view(index);
        if (program.at(view).kind != LayoutKind::Record)
            continue;
        for (LayoutID member : program.at(program.declaredMembersOf(view)).completeMembers()) {
            if (member != view && declaredLayouts.get(member.index()))
                views[member.index()].append(view);
        }
    }
    return views;
}

void initializeLayouts(Program& program, BuildContext& build)
{
    forEachIndexInParallel(build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        Layout& layout = program.layouts[index];
        if (!layout.ownFieldCount)
            layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
        layout.indexFields();
    });
}

void initializeAddedLayout(Program& program, LayoutID id)
{
    Layout& layout = program.at(id);
    layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
    layout.indexFields();
    program.setDeclaredMembers(id, program.types.intern(LayoutSet::complete({ id })));
}

static bool shouldAppendInheritedFields(const Layout& layout)
{
    if (layout.flow.baseState != BaseState::Layout)
        return false;
    return layout.isDerived() || std::ranges::none_of(layout.fields, [](const Field& field) {
        return field.flags.contains(FieldFlags::IsInherited);
    });
}

static void appendBaseFields(Program& program, LayoutID id)
{
    Layout& layout = program.at(id);
    LayoutID base = layout.flow.base;
    const Layout& baseLayout = program.at(base);
    for (uint32_t i = 0; i < baseLayout.fields.size(); ++i) {
        const Field& baseField = baseLayout.fields[i];
        if (std::optional<uint32_t> present = layout.findField(baseField.key)) {
            Field& field = layout.fields[*present];
            if (field.flags.contains(FieldFlags::IsInherited)) {
                field.declared = baseField.declared;
                field.slotType = baseField.slotType;
                field.setterType = baseField.setterType;
                field.form = baseField.form;
                field.flags.set(FieldFlags::IsEnforced, baseField.flags.contains(FieldFlags::IsEnforced));
            }
            continue;
        }
        FieldReference declaration = declarationOf(program, { base, i });
        Field field = baseField;
        field.flags.add(FieldFlags::IsInherited);
        field.originLayout = declaration.layout;
        field.originField = declaration.field;
        field.flow = { };
        layout.appendField(WTF::move(field));
    }
}

void appendInheritedFields(Program& program, LayoutID id)
{
    Vector<LayoutID, 8> chain;
    for (LayoutID current = id; shouldAppendInheritedFields(program.at(current)); current = program.at(current).flow.base) {
        RELEASE_ASSERT(chain.size() < program.layouts.size());
        chain.append(current);
    }
    while (!chain.isEmpty())
        appendBaseFields(program, chain.takeLast());
}

namespace {

class LayoutUsesOfBody {
public:
    LayoutUsesOfBody(const Program& program, const BitVector& typesWithLayouts, const Function& function, LayoutUses& uses)
        : m_program(program)
        , m_typesWithLayouts(typesWithLayouts)
        , m_function(function)
        , m_uses(uses)
    {
    }

    void collect();

private:
    bool namesLayouts(const DeclaredType& declared) const { return m_typesWithLayouts.quickGet(declared.written().index()); }
    bool isResultOfCall(ValueID) const;
    void addConversionOfResult(const DeclaredType&);
    void addTailCasts(const Terminator&);

    const Program& m_program;
    const BitVector& m_typesWithLayouts;
    const Function& m_function;
    LayoutUses& m_uses;
};

} // anonymous namespace

bool LayoutUsesOfBody::isResultOfCall(ValueID id) const
{
    while (true) {
        const Value& value = m_function.at(id);
        if (Value::opcodesWithExpectedResult.contains(value.opcode))
            return true;
        bool isSameValue = value.opcode == Opcode::Await || (value.opcode == Opcode::Convert && !namesLayouts(value.declared()));
        if (!isSameValue || value.valueOperand().index() >= id.index())
            return false;
        id = value.valueOperand();
    }
}

void LayoutUsesOfBody::addConversionOfResult(const DeclaredType& declared)
{
    bool isResultTypeOfCallee = declared.site && m_program.at(declared.site).kind == TypeSiteKind::CallResult;
    if (!isResultTypeOfCallee)
        m_uses.typesOfConvertedResults.quickSet(declared.written().index());
}

void LayoutUsesOfBody::addTailCasts(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall || !isResultOfCall(terminator.callOperand()))
        return;
    for (TailCast cast : terminator.casts()) {
        if (!namesLayouts(cast.declared))
            continue;
        addConversionOfResult(cast.declared);
        return;
    }
}

void LayoutUsesOfBody::collect()
{
    auto name = [&](LayoutID layout) {
        m_uses.name(layout);
    };
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            forEachLayout(value, name);
            if (value.opcode != Opcode::Convert)
                continue;
            DeclaredType declared = value.declared();
            if (namesLayouts(declared) && isResultOfCall(value.valueOperand()))
                addConversionOfResult(declared);
        }
        forEachLayout(block.terminator, name);
        addTailCasts(block.terminator);
    }
}

static unsigned findTypesWithLayouts(const Program& program, BitVector& result, unsigned firstType)
{
    unsigned numberOfTypes = program.types.size();
    result.ensureSize(numberOfTypes);
    for (unsigned index = firstType; index < numberOfTypes; ++index) {
        const Type& type = program.at(TypeID(index));
        bool hasLayouts = type.layouts || std::ranges::any_of(type.classes, [&](const ClassPart& part) {
            return std::ranges::any_of(part.writtenContents, [&](TypeID contents) {
                return contents && result.quickGet(contents.index());
            });
        });
        if (hasLayouts)
            result.quickSet(index);
    }
    return numberOfTypes;
}

LayoutUseCollector::LayoutUseCollector(const Program& program, size_t numberOfLayouts, unsigned numberOfWorkers)
    : m_program(program)
    , m_numberOfLayouts(numberOfLayouts)
    , m_usesOfWorkers(numberOfWorkers)
{
    m_numberOfTypes = findTypesWithLayouts(program, m_typesWithLayouts, 0);
}

LayoutUses& LayoutUseCollector::usesOf(unsigned worker)
{
    std::optional<LayoutUses>& uses = m_usesOfWorkers[worker];
    if (!uses)
        uses.emplace(m_numberOfLayouts, m_typesWithLayouts.size());
    return *uses;
}

void LayoutUseCollector::collect(const Function& function, unsigned worker)
{
    if (function.hasBody())
        LayoutUsesOfBody(m_program, m_typesWithLayouts, function, usesOf(worker)).collect();
}

void LayoutUseCollector::collect(const Data& data, unsigned worker)
{
    LayoutUses& uses = usesOf(worker);
    forEachLayout(data, [&](LayoutID layout) {
        uses.name(layout);
    });
}

LayoutUses LayoutUseCollector::takeUses()
{
    LayoutUses uses(m_numberOfLayouts, m_typesWithLayouts.size());
    for (std::optional<LayoutUses>& usesOfWorker : m_usesOfWorkers) {
        if (!usesOfWorker)
            continue;
        uses.named.merge(usesOfWorker->named);
        uses.typesOfConvertedResults.merge(usesOfWorker->typesOfConvertedResults);
        usesOfWorker = std::nullopt;
    }
    return uses;
}

static void nameLayoutsOfConvertedResults(const Program& program, const BitVector& typesWithLayouts, LayoutUses& uses)
{
    BitVector visitedTypes(program.types.size());
    BitVector visitedLayouts(program.layouts.size());
    Vector<TypeID, 16> worklist;
    auto visit = [&](TypeID type) {
        if (type && typesWithLayouts.quickGet(type.index()) && !visitedTypes.quickSet(type.index()))
            worklist.append(type);
    };

    for (size_t index : uses.typesOfConvertedResults)
        visit(TypeID(static_cast<uint32_t>(index)));
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.writtenContents)
                visit(contents);
        }
        for (LayoutID member : membersOf(program, type)) {
            const Layout& layout = program.at(member);
            if (layout.kind != LayoutKind::Record || visitedLayouts.quickSet(member.index()))
                continue;
            uses.name(member);
            for (const Field& field : layout.fields)
                visit(field.declared.written());
        }
    }
}

void markUnallocatedLayouts(Program& program, BuildContext& build)
{
    LayoutUseCollector collector(program, program.layouts.size(), build.parallelLoops().width());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        collector.collect(program.functions[index], worker);
    });
    markUnallocatedLayouts(program, build.parallelLoops(), collector);
}

void markUnallocatedLayouts(Program& program, const ParallelLoops& loops, LayoutUseCollector& collector)
{
    forEachIndexInParallel(loops, program.data.size(), [&](unsigned index, unsigned worker) {
        collector.collect(program.data[index], worker);
    });
    LayoutUses uses = collector.takeUses();
    unsigned numberOfTypesSeen = collector.numberOfTypes();
    BitVector typesWithLayouts = collector.takeTypesWithLayouts();
    findTypesWithLayouts(program, typesWithLayouts, numberOfTypesSeen);
    uses.named.ensureSize(program.layouts.size());
    auto name = [&](LayoutID layout) {
        uses.name(layout);
    };
    for (const ExpectedResult& expected : program.expectedResults)
        forEachLayout(expected, name);
    nameLayoutsOfConvertedResults(program, typesWithLayouts, uses);

    forEachIndexInParallel(loops, program.numberOfLayoutsAsRead(), [&](unsigned index) {
        Layout& layout = program.layouts[index];
        bool isDeclaredType = layout.kind == LayoutKind::Record && !layout.constructor;
        if (isDeclaredType && (layout.has(LayoutFlags::IsTypeParameter) || !uses.named.quickGet(index)))
            layout.flow.allocation = AllocationState::NeverAllocated;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
