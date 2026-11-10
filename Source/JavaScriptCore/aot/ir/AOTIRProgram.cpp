/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRProgram.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIterationHelpers.h"
#include "AOTMocksOfProgram.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <tuple>
#include <wtf/HashSet.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Program);

Program::Program(const BuiltinTable& builtins)
    : m_tableOfBuild(builtins)
    , types(m_tableOfBuild.table(), this)
    , m_builtins(m_tableOfBuild.table())
{
}

Program::~Program() = default;

void Program::adoptBuffer(OwnedBytes&& bytes)
{
    m_bytes.owned = WTF::move(bytes);
    m_bytes.borrowed = m_bytes.owned.span();
}

std::span<const uint8_t> Program::copyBorrowedBytes(std::span<const uint8_t> bytes, const ParallelLoops& loops)
{
    constexpr size_t chunkSize = 256 * KB;
    RELEASE_ASSERT(!ownsBuffer());
    m_bytes.owned = OwnedBytes::malloc(bytes.size());
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + chunkSize - 1) / chunkSize), [&](unsigned chunk) {
        size_t offset = chunk * chunkSize;
        memcpySpan(m_bytes.owned.mutableSpan().subspan(offset), bytes.subspan(offset, std::min(chunkSize, bytes.size() - offset)));
    });
    return m_bytes.owned.span();
}

void Program::copyBorrowedWordsOfPositions(const ParallelLoops& loops)
{
    Vector<size_t> firstWords(modules.size());
    size_t count = 0;
    for (size_t index = 0; index < modules.size(); ++index) {
        const SourcePositions& positions = modules[index].positions;
        firstWords[index] = std::exchange(count, count + positions.lineStarts.size());
    }
    m_bytes.wordsOfPositions = MallocSpan<uint32_t>::malloc(count * sizeof(uint32_t));
    forEachIndexInParallel(loops, static_cast<unsigned>(modules.size()), [&](unsigned index) {
        SourcePositions& positions = modules[index].positions;
        std::span<uint32_t> copy = m_bytes.wordsOfPositions.mutableSpan().subspan(firstWords[index], positions.lineStarts.size());
        memcpySpan(copy, positions.lineStarts);
        positions.lineStarts = copy;
    });
}

static auto sortKeyOfConstructionStart(const ConstructionStart& start)
{
    return std::tuple { start.reported, start.isForCallers, start.isShownAtStart, start.isWrittenAsCall, start.start };
}

void Program::setConstructionStarts(Vector<ConstructionStartInModule>&& startsInNoOrder)
{
    Vector<ConstructionStartInModule> all = WTF::move(startsInNoOrder);
    auto isInModuleOfProgram = [&](const ConstructionStartInModule& entry) {
        return entry.module && entry.module.index() < modules.size();
    };

    Vector<uint32_t> numberOfStarts;
    numberOfStarts.fill(0, modules.size());
    for (const ConstructionStartInModule& entry : all) {
        if (isInModuleOfProgram(entry))
            ++numberOfStarts[entry.module.index()];
    }
    m_constructionStartsOfModules = Vector<Vector<ConstructionStart>>(modules.size());
    for (size_t index = 0; index < modules.size(); ++index)
        m_constructionStartsOfModules[index].reserveInitialCapacity(numberOfStarts[index]);
    for (const ConstructionStartInModule& entry : all) {
        if (isInModuleOfProgram(entry))
            m_constructionStartsOfModules[entry.module.index()].append(entry.start);
    }

    for (Vector<ConstructionStart>& starts : m_constructionStartsOfModules) {
        AOT::sort(starts, { }, sortKeyOfConstructionStart);
        auto repeated = std::ranges::unique(starts, { }, &ConstructionStart::reported);
        starts.shrink(starts.size() - repeated.size());
        starts.shrinkToFit();
    }
}

const ConstructionStart* Program::findConstructionReportedAt(ModuleID id, uint32_t position) const
{
    if (!id || id.index() >= m_constructionStartsOfModules.size())
        return nullptr;
    const Vector<ConstructionStart>& starts = m_constructionStartsOfModules[id.index()];
    auto found = std::ranges::lower_bound(starts, position, { }, &ConstructionStart::reported);
    if (found == starts.end() || found->reported != position)
        return nullptr;
    return &*found;
}

BuiltinName Program::builtinNameOfKeyThatIsNoText(PropertyKeyID id, const PropertyKey& key) const
{
    switch (key.kind) {
    case PropertyKeyKind::Text:
        return strings.builtinName(key.text);
    case PropertyKeyKind::WellKnownSymbol:
        return m_builtins.nameOf(*key.symbol);
    case PropertyKeyKind::RegisteredSymbol:
        for (BuiltinName name : m_derivedTables.registeredSymbolNames) {
            if (keyOf(name) == id)
                return name;
        }
        return { };
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return { };

    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Program::setDeclaredMembers(LayoutID id, LayoutSetID set)
{
    Vector<LayoutSetID>& declaredMembers = m_derivedTables.declaredMembers;
    if (declaredMembers.size() <= id.index())
        declaredMembers.grow(id.index() + 1);
    declaredMembers[id.index()] = set;
}

std::span<const LayoutID> Program::layoutsExpectedBelow(LayoutID id) const
{
    const DerivedTables& tables = m_derivedTables;
    RELEASE_ASSERT(id.index() < tables.numberOfLayoutsBelow.size());
    uint32_t numberBelow = tables.numberOfLayoutsBelow[id.index()];
    if (!numberBelow)
        return { };
    return tables.layoutsInOrderOfBases.subspan(tables.placeInOrderOfBases[id.index()] + 1, numberBelow);
}

bool Program::prototypeMaySelectOtherClass(LayoutID id) const
{
    return isDefinedByClass(at(declaredLayoutOf(*this, id)));
}

static BitVector layoutsInRingsOfBases(const Vector<Layout>& layouts)
{
    enum class State : uint8_t { Unseen, OnTheWay, Settled };
    Vector<State> states;
    states.fill(State::Unseen, layouts.size());
    BitVector isInRing(layouts.size());
    Vector<uint32_t, 32> passed;
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        LayoutID current(index);
        for (; current && states[current.index()] == State::Unseen; current = layouts[current.index()].base) {
            states[current.index()] = State::OnTheWay;
            passed.append(current.index());
        }
        if (current && states[current.index()] == State::OnTheWay) {
            LayoutID member = current;
            do {
                isInRing.quickSet(member.index());
                member = layouts[member.index()].base;
            } while (member != current);
        }
        for (uint32_t layout : passed)
            states[layout] = State::Settled;
        passed.shrink(0);
    }
    return isInRing;
}

void Program::indexClasses()
{
    constexpr uint32_t none = UINT32_MAX;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());
    BitVector isInRing = layoutsInRingsOfBases(layouts);

    Vector<uint32_t> parentOf(numberOfLayouts, [&](size_t index) {
        LayoutID base = layouts[index].base;
        return base && !isInRing.quickGet(base.index()) ? base.index() : none;
    });

    Vector<uint32_t> firstChildOf;
    firstChildOf.fill(0, numberOfLayouts + 1);
    for (uint32_t parent : parentOf) {
        if (parent != none)
            ++firstChildOf[parent + 1];
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstChildOf[index + 1] += firstChildOf[index];
    Vector<uint32_t> children(firstChildOf[numberOfLayouts]);
    Vector<uint32_t> nextChildSlotOf(firstChildOf.span().first(numberOfLayouts));
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (parentOf[index] != none)
            children[nextChildSlotOf[parentOf[index]]++] = index;
    }

    Vector<LayoutID>& inOrder = m_derivedTables.layoutsInOrderOfBases;
    Vector<uint32_t>& places = m_derivedTables.placeInOrderOfBases;
    Vector<uint32_t>& numbersBelow = m_derivedTables.numberOfLayoutsBelow;
    inOrder.clear();
    inOrder.reserveInitialCapacity(numberOfLayouts);
    places.fill(0, numberOfLayouts);
    numbersBelow.fill(0, numberOfLayouts);

    Vector<uint32_t> worklist;
    for (uint32_t root = 0; root < numberOfLayouts; ++root) {
        if (parentOf[root] != none || isInRing.quickGet(root))
            continue;
        worklist.append(root);
        while (!worklist.isEmpty()) {
            uint32_t layout = worklist.takeLast();
            places[layout] = static_cast<uint32_t>(inOrder.size());
            inOrder.append(LayoutID(layout));
            for (uint32_t next = firstChildOf[layout + 1]; next > firstChildOf[layout]; --next)
                worklist.append(children[next - 1]);
        }
    }
    for (size_t place = inOrder.size(); place--;) {
        uint32_t layout = inOrder[place].index();
        if (parentOf[layout] != none)
            numbersBelow[parentOf[layout]] += numbersBelow[layout] + 1;
    }

    Vector<std::pair<uint32_t, uint32_t>> stepsDown;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        forEachOfSeveralParents(*this, LayoutID(index), [&](LayoutID parent) {
            if (!isInRing.quickGet(parent.index()))
                stepsDown.append({ parent.index(), index });
        });
    }
    if (stepsDown.isEmpty())
        return;
    AOT::sort(stepsDown);

    BitVector standsAboveStep(numberOfLayouts);
    for (auto [parent, child] : stepsDown)
        worklist.append(parent);
    while (!worklist.isEmpty()) {
        uint32_t layout = worklist.takeLast();
        if (standsAboveStep.quickSet(layout))
            continue;
        if (parentOf[layout] != none)
            worklist.append(parentOf[layout]);
        forEachOfSeveralParents(*this, LayoutID(layout), [&](LayoutID parent) {
            worklist.append(parent.index());
        });
    }

    BitVector isFound(numberOfLayouts);
    for (size_t above : standsAboveStep) {
        isFound.clearAll();
        isFound.quickSet(above);
        places[above] = static_cast<uint32_t>(inOrder.size());
        inOrder.append(LayoutID(static_cast<uint32_t>(above)));
        auto find = [&](uint32_t below) {
            if (!isFound.quickSet(below))
                inOrder.append(LayoutID(below));
        };
        for (size_t next = places[above]; next < inOrder.size(); ++next) {
            uint32_t layout = inOrder[next].index();
            for (uint32_t child = firstChildOf[layout]; child < firstChildOf[layout + 1]; ++child)
                find(children[child]);
            for (auto step = std::ranges::lower_bound(stepsDown, std::pair { layout, 0u }); step != stepsDown.end() && step->first == layout; ++step)
                find(step->second);
        }
        numbersBelow[above] = static_cast<uint32_t>(inOrder.size()) - places[above] - 1;
    }
}

static std::span<const LayoutID> entriesOfName(const Vector<uint32_t>& first, const Vector<LayoutID>& entries, PropertyKeyID key)
{
    if (!key || key.index() + 1 >= first.size())
        return { };
    return entries.subspan(first[key.index()], first[key.index() + 1] - first[key.index()]);
}

std::span<const LayoutID> Program::layoutsWithName(PropertyKeyID key) const
{
    return entriesOfName(m_derivedTables.firstLayoutWithName, m_derivedTables.layoutsWithName, key);
}

static bool keepsPropertiesInFields(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return !layout.writtenBuiltinBase;
    case LayoutKind::BuiltinResult:
        return layout.builtinLayout && !program.builtins().at(layout.builtinLayout).isTuple;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Program::indexNames()
{
    indexNames(ParallelLoops(1));
}

void Program::indexNames(const ParallelLoops& loops)
{
    struct Holder {
        PropertyKeyID key;
        LayoutID layout;
    };

    Vector<uint8_t> namesEveryFieldAbove;
    namesEveryFieldAbove.fill(0, layouts.size());
    forEachIndexInParallel(loops, layouts.size(), [&](unsigned index) {
        const Layout& layout = layouts[index];
        namesEveryFieldAbove[index] = layout.base && (baseWhoseFieldListIsCopiedWhole(*this, LayoutID(index)) || std::ranges::all_of(at(layout.base).fields, [&](const Field& field) {
            return !field.key || layout.findField(field.key);
        }));
    });
    BitVector namesEveryFieldOfRowAbove = answerAlongRows(layouts.size(), false, [&](LayoutID id) -> StepAlongRow {
        LayoutID base = at(id).base;
        if (!base)
            return { true, { } };
        if (!namesEveryFieldAbove[id.index()])
            return { false, { } };
        return { std::nullopt, base };
    });

    Vector<Holder> holders = collectInParallel<Holder>(loops, layouts.size(), [&](unsigned index, Vector<Holder>& found) {
        LayoutID id(index);
        if (at(id).has(LayoutFlags::IsNeverAllocated) || at(id).has(LayoutFlags::IsWrittenUnderDictionaryType) || !keepsPropertiesInFields(*this, id))
            return;

        LayoutID remembered;
        size_t steps = 0;
        size_t stepsToNextRemembered = 1;
        bool hasNamedFields = false;
        for (LayoutID current = id; current && current != remembered; current = at(current).base) {
            if (++steps == stepsToNextRemembered) {
                remembered = current;
                stepsToNextRemembered *= 2;
            }
            const Layout& layout = at(current);
            for (uint32_t field = 0; !hasNamedFields && field < layout.fields.size(); ++field) {
                if (PropertyKeyID key = layout.fields[field].key)
                    found.append({ key, id });
            }
            if (namesEveryFieldOfRowAbove.quickGet(current.index()))
                break;
            hasNamedFields = namesEveryFieldAbove[current.index()];
        }
    });

    uint32_t numberOfKeys = 0;
    for (const Holder& holder : holders)
        numberOfKeys = std::max(numberOfKeys, holder.key.index() + 1);
    Vector<uint32_t>& first = m_derivedTables.firstLayoutWithName;
    first.fill(0, holders.isEmpty() ? 0 : numberOfKeys + 1);
    for (const Holder& holder : holders)
        ++first[holder.key.index() + 1];
    for (size_t index = 1; index < first.size(); ++index)
        first[index] += first[index - 1];

    Vector<LayoutID> layoutsByName;
    layoutsByName.grow(holders.size());
    Vector<uint32_t> next(first);
    for (const Holder& holder : holders)
        layoutsByName[next[holder.key.index()]++] = holder.layout;

    uint32_t numberKept = 0;
    for (uint32_t key = 0; key < numberOfKeys; ++key) {
        uint32_t firstOfKey = std::exchange(first[key], numberKept);
        for (uint32_t place = firstOfKey; place < first[key + 1]; ++place) {
            if (place == firstOfKey || layoutsByName[place] != layoutsByName[place - 1])
                layoutsByName[numberKept++] = layoutsByName[place];
        }
    }
    if (numberOfKeys)
        first[numberOfKeys] = numberKept;
    layoutsByName.shrink(numberKept);
    m_derivedTables.layoutsWithName = WTF::move(layoutsByName);
}

std::span<const LayoutID> Program::viewsAbove(LayoutID id) const
{
    const Vector<uint32_t>& first = m_derivedTables.firstViewAbove;
    if (!id || id.index() + 1 >= first.size())
        return { };
    return m_derivedTables.viewsAbove.subspan(first[id.index()], first[id.index() + 1] - first[id.index()]);
}

std::span<const Program::FieldOfView> Program::fieldsOfViews(PropertyKeyID key) const
{
    const Vector<uint32_t>& first = m_derivedTables.firstFieldsByKey;
    if (!key || key.index() + 1 >= first.size())
        return { };
    return m_derivedTables.fieldsOfViewsByKey.subspan(first[key.index()], first[key.index() + 1] - first[key.index()]);
}

void Program::indexViews(const ParallelLoops& loops)
{
    struct ViewOfLayout {
        uint32_t layout;
        LayoutID view;
    };
    auto numberOfLayouts = static_cast<uint32_t>(layouts.size());
    BitVector leadsPastClassesWithObjects = answerAlongRows(numberOfLayouts, true, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = at(id);
        if (std::ranges::any_of(layout.supertypes, [&](LayoutID supertype) { return supertype != layout.base; }))
            return { true, { } };
        if (!layout.base || layout.base.index() >= numberOfLayouts)
            return { false, { } };
        if (at(layout.base).has(LayoutFlags::IsNeverAllocated))
            return { true, { } };
        return { std::nullopt, layout.base };
    });
    Vector<ViewOfLayout> viewsOfLayouts = collectInParallel<ViewOfLayout>(loops, numberOfLayouts, [&](unsigned index, Vector<ViewOfLayout>& found) {
        if (layouts[index].has(LayoutFlags::IsNeverAllocated) || !leadsPastClassesWithObjects.quickGet(index))
            return;
        constexpr size_t mostViewsLookedThrough = 16;
        Vector<LayoutID, mostViewsLookedThrough> views;
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> viewsWhereThereAreMany;
        auto addView = [&](LayoutID view) {
            if (views.size() < mostViewsLookedThrough) {
                if (views.contains(view))
                    return false;
            } else {
                if (viewsWhereThereAreMany.isEmpty()) {
                    for (LayoutID earlier : views)
                        viewsWhereThereAreMany.add(earlier.index());
                }
                if (!viewsWhereThereAreMany.add(view.index()).isNewEntry)
                    return false;
            }
            views.append(view);
            return true;
        };
        Vector<LayoutID, 16> pending;
        auto climbFrom = [&](const Layout& layout) {
            pending.append(layout.base);
            for (LayoutID supertype : layout.supertypes)
                pending.append(supertype);
        };
        climbFrom(layouts[index]);
        while (!pending.isEmpty()) {
            LayoutID view = pending.takeLast();
            if (!view || view.index() >= numberOfLayouts || view.index() == index || !addView(view))
                continue;
            climbFrom(layouts[view.index()]);
        }
        AOT::sort(views);
        for (LayoutID view : views)
            found.append({ index, view });
    });

    Vector<uint32_t>& first = m_derivedTables.firstViewAbove;
    first.fill(0, numberOfLayouts + 1);
    for (const ViewOfLayout& entry : viewsOfLayouts)
        ++first[entry.layout + 1];
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        first[index + 1] += first[index];
    m_derivedTables.viewsAbove = viewsOfLayouts.map([](const ViewOfLayout& entry) {
        return entry.view;
    });

    BitVector isIndexedByKey(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        std::span<const LayoutID> views = viewsAbove(LayoutID(index));
        if (views.size() < fewestViewsIndexedByKey)
            continue;
        for (LayoutID view : views)
            isIndexedByKey.quickSet(view.index());
    }
    struct FieldUnderKey {
        PropertyKeyID key;
        FieldOfView field;
    };
    Vector<FieldUnderKey> fieldsUnderKeys = collectInParallel<FieldUnderKey>(loops, numberOfLayouts, [&](unsigned index, Vector<FieldUnderKey>& found) {
        if (!isIndexedByKey.quickGet(index))
            return;
        const Vector<Field>& fields = layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (fields[field].key && !fields[field].flags.contains(FieldFlags::IsInherited))
                found.append({ fields[field].key, { LayoutID(index), field } });
        }
    });
    uint32_t numberOfKeys = 0;
    for (const FieldUnderKey& entry : fieldsUnderKeys)
        numberOfKeys = std::max(numberOfKeys, entry.key.index() + 1);
    Vector<uint32_t>& firstField = m_derivedTables.firstFieldsByKey;
    firstField.fill(0, numberOfKeys + 1);
    for (const FieldUnderKey& entry : fieldsUnderKeys)
        ++firstField[entry.key.index() + 1];
    for (uint32_t key = 0; key < numberOfKeys; ++key)
        firstField[key + 1] += firstField[key];
    Vector<FieldOfView> fieldsByKey;
    fieldsByKey.grow(fieldsUnderKeys.size());
    Vector<uint32_t> nextField(firstField);
    for (const FieldUnderKey& entry : fieldsUnderKeys)
        fieldsByKey[nextField[entry.key.index()]++] = entry.field;

    uint32_t numberKept = 0;
    for (uint32_t key = 0; key < numberOfKeys; ++key) {
        uint32_t firstOfKey = std::exchange(firstField[key], numberKept);
        for (uint32_t place = firstOfKey; place < firstField[key + 1]; ++place) {
            if (place == firstOfKey || fieldsByKey[place].view != fieldsByKey[numberKept - 1].view)
                fieldsByKey[numberKept++] = fieldsByKey[place];
        }
    }
    firstField[numberOfKeys] = numberKept;
    fieldsByKey.shrink(numberKept);
    m_derivedTables.fieldsOfViewsByKey = WTF::move(fieldsByKey);
}

void Program::internBuiltinNameKeys()
{
    Vector<PropertyKeyID>& keys = m_derivedTables.builtinNameKeys;
    RELEASE_ASSERT(keys.isEmpty());

    TypeArena arena(types, ArenaKeeps::InProgram);
    unsigned count = m_builtins.nameCount();
    keys.reserveInitialCapacity(count);
    for (unsigned index = 0; index < count; ++index) {
        BuiltinName name(index);
        if (m_builtins.isRegisteredSymbolName(name)) {
            m_derivedTables.registeredSymbolNames.append(name);
            keys.append(arena.registeredSymbolKey(strings.intern(m_builtins.textOf(name))));
        } else if (m_builtins.isSymbolName(name)) {
            std::optional<WellKnownSymbol> symbol = m_builtins.wellKnownSymbolOf(name);
            RELEASE_ASSERT(symbol);
            keys.append(arena.wellKnownSymbolKey(*symbol));
        } else
            keys.append(arena.textKey(strings.stringOf(name)));
    }
    m_derivedTables.keyOfWordOnWhatSeesAccesses = arena.textKey(strings.intern(StringView("what sees a read or a store above"_s)));
}

using ModuleOfNamespaceObject = std::pair<BuiltinID, ModuleID>;

static Vector<ModuleOfNamespaceObject> findCompiledModulesOfNamespaceObjects(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<ModuleOfNamespaceObject> found;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        if (BuiltinID object = builtins.namespaceOf(module.runtimeModule))
            found.append({ builtins.canonicalID(object), ModuleID(index) });
    }
    AOT::sort(found);
    auto repeated = std::ranges::unique(found, { }, &ModuleOfNamespaceObject::first);
    found.shrink(found.size() - repeated.size());
    found.shrinkToFit();
    return found;
}

void Program::appendBuiltinLayouts()
{
    m_derivedTables.compiledModulesOfNamespaceObjects = findCompiledModulesOfNamespaceObjects(*this);

    Vector<LayoutID>& builtinLayouts = m_derivedTables.builtinLayouts;
    RELEASE_ASSERT(builtinLayouts.isEmpty());

    unsigned count = m_builtins.layoutCount();
    builtinLayouts.reserveInitialCapacity(count);
    layouts.reserveCapacity(layouts.size() + count);
    for (unsigned index = 0; index < count; ++index) {
        Layout layout;
        layout.kind = LayoutKind::BuiltinResult;
        layout.builtinLayout = BuiltinLayoutID(index);
        builtinLayouts.append(LayoutID(layouts.size()));
        layouts.append(WTF::move(layout));
    }

    TypeArena arena(types, ArenaKeeps::InProgram);
    BuiltinTypeContext context { *this, arena };
    for (LayoutID id : builtinLayouts) {
        Layout& layout = at(id);
        std::span<const BuiltinLayoutField> builtinFields = m_builtins.at(layout.builtinLayout).fields;
        layout.fields.reserveInitialCapacity(builtinFields.size());
        for (const BuiltinLayoutField& builtinField : builtinFields) {
            Field field;
            if (builtinField.name)
                field.key = keyOf(builtinField.name);
            field.declared.establish(Proven::byPosition(typeOfBuiltinLayoutField(context, builtinField)));
            if (!builtinField.isOptional)
                field.flags.add(FieldFlags::IsRequired);
            layout.fields.append(WTF::move(field));
        }
        layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
        setDeclaredMembers(id, LayoutSets::of(id));
    }
}

static TypeID typeOfExportedNamespace(const Program& program, const ModuleExport& entry, TypeArena& arena)
{
    if (!entry.module || entry.module.index() >= program.modules.size())
        return TypeTable::object;
    switch (entry.phase) {
    case ImportPhase::Evaluation:
        if (LayoutID layout = program.layoutOfWholeNamespace(entry.module))
            return arena.forLayout(layout);
        return TypeTable::object;
    case ImportPhase::Defer:
        return TypeTable::object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DeclaredType declaredTypeOfExport(const Program& program, const ModuleExport& entry, TypeID typeOfNamespace)
{
    DeclaredType declared;
    declared.establish(Proven::byPosition(TypeTable::anything));
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        if (!entry.variable || entry.variable.index() >= program.variables.size())
            return declared;
        declared = program.at(entry.variable).declared;
        declared.site = { };
        return declared;
    case ModuleExportKind::Namespace:
        declared.establish(Proven::byPosition(typeOfNamespace));
        return declared;
    case ModuleExportKind::Indirect:
        return declared;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendFieldsOfNamespaceLayout(Program& program, LayoutID id, std::span<const PropertyKeyID> keys, std::span<const TypeID> typesOfExportedNamespaces)
{
    Layout& layout = program.at(id);
    const Module& module = program.at(layout.namespaceOf);
    layout.fields.reserveInitialCapacity(module.exports.size());
    for (size_t index = 0; index < module.exports.size(); ++index) {
        Field field;
        field.key = keys[index];
        field.declared = declaredTypeOfExport(program, module.exports[index], typesOfExportedNamespaces[index]);
        field.flags = { FieldFlags::IsRequired, FieldFlags::IsImmutable };
        layout.fields.append(WTF::move(field));
    }
    layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
}

void Program::appendNamespaceLayouts(const ParallelLoops& loops)
{
    layouts.reserveCapacity(layouts.size() + static_cast<size_t>(std::ranges::count_if(modules, [](const Module& module) {
        return hasNamespaceLayout(module.kind);
    })));
    for (size_t index = 0; index < modules.size(); ++index) {
        Module& module = modules[index];
        if (!hasNamespaceLayout(module.kind))
            continue;
        RELEASE_ASSERT(!module.namespaceLayout);
        module.namespaceLayout = LayoutID(layouts.size());
        layouts.constructAndAppend();
        Layout& layout = layouts.last();
        layout.kind = LayoutKind::ModuleNamespace;
        layout.module = ModuleID(index);
        layout.namespaceOf = ModuleID(index);
    }

    Vector<uint32_t> holders;
    Vector<uint32_t> firstExportOfHolder;
    uint32_t numberOfExports = 0;
    for (uint32_t index = 0; index < modules.size(); ++index) {
        if (!modules[index].namespaceLayout)
            continue;
        holders.append(index);
        firstExportOfHolder.append(numberOfExports);
        numberOfExports += static_cast<uint32_t>(modules[index].exports.size());
    }
    firstExportOfHolder.append(numberOfExports);
    auto exportNumbered = [&](uint32_t number) -> const ModuleExport& {
        size_t holder = std::ranges::upper_bound(firstExportOfHolder, number) - firstExportOfHolder.begin() - 1;
        return modules[holders[holder]].exports[number - firstExportOfHolder[holder]];
    };

    ArenasOfWorkers arenas(types, loops);
    Vector<PropertyKeyID> keys(numberOfExports);
    arenas.internInOrder(keys.mutableSpan(), [&](uint32_t number) {
        return PropertyKey { .kind = PropertyKeyKind::Text, .text = exportNumbered(number).name };
    });

    Vector<uint32_t> exportedNamespaces = collectInParallel<uint32_t>(loops, static_cast<unsigned>(holders.size()), [&](unsigned holder, Vector<uint32_t>& found) {
        const Module& module = modules[holders[holder]];
        for (uint32_t index = 0; index < module.exports.size(); ++index) {
            if (module.exports[index].kind == ModuleExportKind::Namespace)
                found.append(firstExportOfHolder[holder] + index);
        }
    });
    Vector<TypeID> typesOfExportedNamespaces(numberOfExports);
    for (uint32_t number : exportedNamespaces)
        typesOfExportedNamespaces[number] = typeOfExportedNamespace(*this, exportNumbered(number), arenas.of(0));

    forEachIndexInParallel(loops, static_cast<unsigned>(holders.size()), [&](unsigned holder) {
        size_t first = firstExportOfHolder[holder];
        size_t count = firstExportOfHolder[holder + 1] - first;
        appendFieldsOfNamespaceLayout(*this, modules[holders[holder]].namespaceLayout, keys.subspan(first, count), typesOfExportedNamespaces.subspan(first, count));
    });
    if (MocksOfProgram mocks = findMocksOfProgram(*this); mocks.isAnyModuleMocked) {
        DeclaredType declaredAnything;
        declaredAnything.establish(Proven::byPosition(TypeTable::anything));
        for (uint32_t holder : holders) {
            Layout& layout = at(modules[holder].namespaceLayout);
            for (uint32_t index = 0; index < layout.fields.size(); ++index) {
                if (mayComeThroughMock(*this, mocks, ModuleID(holder), index))
                    layout.fields[index].declared = declaredAnything;
            }
        }
    }
    for (uint32_t holder : holders) {
        LayoutID id = modules[holder].namespaceLayout;
        setDeclaredMembers(id, LayoutSets::of(id));
    }
}

VariableID Program::variableBehindExport(ModuleID module, uint32_t index) const
{
    for (size_t steps = 0; steps <= modules.size(); ++steps) {
        const auto& exports = at(module).exports;
        if (index >= exports.size())
            return { };
        const ModuleExport& entry = exports[index];
        if (entry.kind == ModuleExportKind::Variable)
            return entry.variable;
        if (entry.kind != ModuleExportKind::Indirect || !entry.module || !entry.importName)
            return { };
        const auto& exportsOfTarget = at(entry.module).exports;
        uint32_t found = 0;
        while (found < exportsOfTarget.size() && exportsOfTarget[found].name != entry.importName)
            ++found;
        module = entry.module;
        index = found;
    }
    return { };
}

LayoutID Program::layoutOfWholeNamespace(ModuleID id) const
{
    const Module& module = at(id);
    return module.exportRule == SyntheticExportRule::None ? module.namespaceLayout : LayoutID();
}

LayoutID Program::layoutOfNamespaceThatListsNoName(ModuleID id) const
{
    constexpr OptionSet<ModuleFlags> flagsOfModulesWithSeveralNamespaces { ModuleFlags::MayBeReevaluated, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted };
    const Module& module = at(id);
    if (module.kind != ModuleKind::RuntimeSource || (module.flags.containsAny(flagsOfModulesWithSeveralNamespaces)) || !objectOfLibraryFileIsResultOfBody(*this, id))
        return { };
    return module.namespaceLayout;
}

static bool hasNumberInText(const Layout& layout)
{
    return layout.kind != LayoutKind::BuiltinResult && layout.kind != LayoutKind::ModuleNamespace;
}

struct LayoutsWithoutNumberInText {
    uint32_t first { 0 };
    uint32_t count { 0 };
};

static LayoutsWithoutNumberInText findLayoutsWithoutNumberInText(const Vector<Layout>& layouts)
{
    auto first = std::ranges::partition_point(layouts, [](const Layout& layout) {
        return hasNumberInText(layout) && !layout.isDerived() && layout.kind != LayoutKind::Closure;
    });
    auto end = std::ranges::partition_point(first, layouts.end(), [](const Layout& layout) {
        return !hasNumberInText(layout);
    });
    return { static_cast<uint32_t>(first - layouts.begin()), static_cast<uint32_t>(end - first) };
}

uint32_t Program::numberInTextOf(LayoutID id) const
{
    RELEASE_ASSERT(hasNumberInText(at(id)));
    LayoutsWithoutNumberInText skipped = findLayoutsWithoutNumberInText(layouts);
    return id.index() < skipped.first ? id.index() : id.index() - skipped.count;
}

LayoutID Program::layoutNumberedInText(uint32_t number) const
{
    LayoutsWithoutNumberInText skipped = findLayoutsWithoutNumberInText(layouts);
    if (number >= layouts.size() - skipped.count)
        return { };
    return LayoutID(number < skipped.first ? number : number + skipped.count);
}

static constexpr uint64_t addToContentHashOfRecord(uint64_t hash, uint64_t bits)
{
    hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
    return hash ^ hash >> 32;
}

static uint64_t addTextToContentHashOfRecord(const StringTable& strings, uint64_t hash, StringID text)
{
    if (text.index() >= strings.size())
        return addToContentHashOfRecord(hash, UINT64_MAX);
    std::span<const uint8_t> bytes = strings[text];
    hash = addToContentHashOfRecord(hash, bytes.size());
    for (uint8_t byte : bytes)
        hash = addToContentHashOfRecord(hash, byte);
    return hash;
}

static uint8_t locationBitOfContentHash(uint64_t hash)
{
    return static_cast<uint8_t>(addToContentHashOfRecord(hash, 0) & 63);
}

void Program::computeLocationBits()
{
    computeLocationBits(ParallelLoops(1));
}

template<typename Result, typename Record>
static Vector<Result> mapRecordsOnPool(const ParallelLoops& loops, const Vector<Record>& records, const Invocable<Result(const Record&)> auto& functor)
{
    Vector<Result> results;
    results.grow(records.size());
    forEachRangeOfIndicesInParallel(loops, records.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            results[index] = functor(records[index]);
    });
    return results;
}

void Program::computeLocationBits(const ParallelLoops& loops)
{
    auto liesInModule = [&](ModuleID id) {
        return id && id.index() < modules.size();
    };
    m_derivedTables.locationBitsOfLayouts = mapRecordsOnPool<uint8_t>(loops, layouts, [&](const Layout& layout) -> uint8_t {
        if (liesInModule(layout.module)) {
            if (!layout.place)
                return 0;
            if (isEnvironmentKind(layout.kind))
                return static_cast<uint8_t>(*layout.place & 63);
            return locationBitOfContentHash(addToContentHashOfRecord(static_cast<uint64_t>(layout.kind), *layout.place));
        }
        uint64_t hash = addTextToContentHashOfRecord(strings, static_cast<uint64_t>(layout.kind), layout.name);
        return locationBitOfContentHash(addToContentHashOfRecord(hash, static_cast<uint64_t>(layout.position) << 32 | layout.builtinLayout.index()));
    });
    Vector<uint32_t> fieldsOfEnvironmentsListedInFront(FillWith { }, modules.size(), 0u);
    Vector<uint32_t> otherLayoutsListedInFront(FillWith { }, modules.size(), 0u);
    for (size_t index = 0; index < layouts.size(); ++index) {
        const Layout& layout = layouts[index];
        if (layout.place || !liesInModule(layout.module))
            continue;
        uint32_t& listedInFront = (isEnvironmentKind(layout.kind) ? fieldsOfEnvironmentsListedInFront : otherLayoutsListedInFront)[layout.module.index()];
        m_derivedTables.locationBitsOfLayouts[index] = static_cast<uint8_t>(listedInFront & 63);
        listedInFront += isEnvironmentKind(layout.kind) ? std::max<uint32_t>(1, static_cast<uint32_t>(layout.fields.size())) : 1;
    }

    static_assert(!locationBitOfVariablesThatPlanAdds);
    Vector<uint32_t> variablesListedInFront(FillWith { }, modules.size() + 1, 0u);
    m_derivedTables.locationBitsOfVariables.grow(variables.size());
    for (size_t index = 0; index < variables.size(); ++index) {
        const Variable& variable = variables[index];
        uint32_t listedInFront = variablesListedInFront[liesInModule(variable.module) ? variable.module.index() : modules.size()]++;
        m_derivedTables.locationBitsOfVariables[index] = static_cast<uint8_t>(1 + variable.place.value_or(listedInFront) % 63);
    }
}

ModuleID Program::compiledModuleOfNamespaceObject(BuiltinID canonical) const
{
    const Vector<ModuleOfNamespaceObject>& table = m_derivedTables.compiledModulesOfNamespaceObjects;
    auto found = std::ranges::lower_bound(table, canonical, { }, &ModuleOfNamespaceObject::first);
    return found != table.end() && found->first == canonical ? found->second : ModuleID();
}

bool Program::isEntry(FunctionID id) const
{
    return id && entries.containsIf([&](const Entry& entry) {
        return at(entry.module).body == id;
    });
}

LineColumn Program::lineColumnFor(ModuleID id, uint32_t position) const
{
    if (!id || id.index() >= modules.size())
        return { 1, 1 };
    const SourcePositions& positions = modules[id.index()].positions;
    auto nextLine = std::ranges::upper_bound(positions.lineStarts, position);
    bool hasLine = nextLine != positions.lineStarts.begin();
    uint32_t line = hasLine ? static_cast<uint32_t>(nextLine - positions.lineStarts.begin() - 1) : 0;
    uint32_t lineStart = hasLine ? positions.lineStarts[line] : 0;

    std::span<const ColumnAdjustment> adjustments = positions.columnAdjustments.span();
    auto nextAdjustment = std::ranges::upper_bound(adjustments, position, { }, &ColumnAdjustment::offset);
    if (nextAdjustment != adjustments.begin()) {
        const ColumnAdjustment& adjustment = *std::prev(nextAdjustment);
        if (!hasLine || adjustment.offset > lineStart)
            return { adjustment.line + 1, adjustment.column + (position - adjustment.offset) + 1 };
    }
    if (!hasLine)
        return { 1, 1 };
    return { line + 1, position - lineStart + 1 };
}

static bool isCalledByHostWhenRegistered(RuntimeFunctionRole role)
{
    switch (role) {
    case RuntimeFunctionRole::EngineBuiltin:
    case RuntimeFunctionRole::HostBuiltin:
        return true;
    case RuntimeFunctionRole::CompilerFunction:
    case RuntimeFunctionRole::Model:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isFoundByHostThroughRegistration(FunctionRole role)
{
    return role == FunctionRole::Library || role == FunctionRole::Model;
}

BitVector functionsCalledByHost(const Program& program, const ParallelLoops& loops)
{
    BitVector isCalledByHost(program.functions.size());
    Vector<uint32_t> functionsForHost = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        FunctionRole role = program.functions[index].role;
        return hasSourceBodyOf(role) && !capabilitiesOf(role).contains(FunctionCapabilities::IsNeverCalledByHost) && !isFoundByHostThroughRegistration(role);
    });
    for (uint32_t index : functionsForHost)
        isCalledByHost.quickSet(index);

    auto isRegisteredForHost = [&](const RuntimeFunctionEntry& entry) {
        return isCalledByHostWhenRegistered(program.builtins().at(entry.id).role);
    };
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function && !isRegisteredForHost(entry))
            isCalledByHost.clear(entry.function.index());
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function && isRegisteredForHost(entry))
            isCalledByHost.set(entry.function.index());
    }
    if (FunctionID appends = functionOfIterationHelper(program, IterationHelper::AppendIterable))
        isCalledByHost.set(appends.index());
    if (FunctionID takes = functionOfIterationHelper(program, IterationHelper::TakeFromIterable))
        isCalledByHost.set(takes.index());
    if (FunctionID next = functionOfIterationHelper(program, IterationHelper::NextOfStepsOverText))
        isCalledByHost.set(next.index());
    for (const Module& module : program.modules) {
        if (module.body)
            isCalledByHost.set(module.body.index());
    }
    return isCalledByHost;
}

BitVector functionsCalledByHost(const Program& program)
{
    return functionsCalledByHost(program, ParallelLoops(1));
}

FunctionID findNakedConstructorOf(const Program& program, BuiltinID builtin)
{
    if (!builtin)
        return { };
    FunctionID named;
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(program.builtins().canonicalID(builtin))) {
        if (overload.mode != BuiltinCallMode::Construct)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.libraryFunction());
        if (!held || !held->function || (named && named != held->function))
            return { };
        named = held->function;
    }
    if (!named)
        return { };
    const Function& function = program.at(named);
    if (!function.has(FunctionFlags::IsNakedConstructor) || function.hasAny({ FunctionFlags::HasThis, FunctionFlags::TakesCallee }) || !function.captures.isEmpty())
        return { };
    return named;
}

FunctionID functionThatSorts(const Program& program)
{
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(program.builtins().canonicalID(Builtins::ArrayPrototypeSort))) {
        if (const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.libraryFunction()); held && held->function)
            return held->function;
    }
    return { };
}

const RuntimeFunctionEntry* findRuntimeFunction(const Program& program, RuntimeFunctionID id)
{
    if (!id)
        return nullptr;
    auto entry = std::ranges::lower_bound(program.runtimeFunctions, id, { }, &RuntimeFunctionEntry::id);
    return entry != program.runtimeFunctions.end() && entry->id == id ? &*entry : nullptr;
}

bool isLibraryCode(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::StubBody || function.enclosingRuntimeFunction)
        return true;
    if (!function.module)
        return true;
    const Module& module = program.at(function.module);
    return module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary);
}

bool isBundlerHelperModule(const Program&, const Module& module)
{
    return module.flags.contains(ModuleFlags::IsBundlerHelper);
}

BitVector modulesThatHostAsksFor(const Program& program, bool beginsModulesOfData)
{
    BitVector modules(program.modules.size());
    for (const Entry& entry : program.entries)
        modules.quickSet(entry.module.index());
    for (size_t index = 0; index < program.modules.size(); ++index) {
        if (program.modules[index].flags.containsAny({ ModuleFlags::IsFoundByName, ModuleFlags::MayBeEvicted }) || (beginsModulesOfData && program.modules[index].kind == ModuleKind::Data))
            modules.quickSet(index);
    }
    for (size_t index = 0; program.admitsInspector && index < program.modules.size(); ++index) {
        if (program.modules[index].kind == ModuleKind::SourceText || program.modules[index].kind == ModuleKind::CommonJs)
            modules.quickSet(index);
    }
    return modules;
}

bool answersToPath(const Program& program, const Module& module)
{
    if (!isFoundByPathOf(module.kind))
        return false;
    return module.kind != ModuleKind::Runtime || !program.builtins().at(module.runtimeModule).flags.containsAny({ RuntimeModuleFlag::HasSource, RuntimeModuleFlag::HasOpenNamespace });
}

bool isBundlerHelperNamedByProgram(const Program& program, ModuleID id)
{
    if (!isBundlerHelperModule(program, program.at(id)))
        return false;
    return std::ranges::any_of(program.modules, [&](const Module& importer) {
        if (isBundlerHelperModule(program, importer))
            return false;
        return std::ranges::contains(importer.requests, id, &ModuleRequest::target) || std::ranges::contains(importer.requiredNames, id, &RequiredName::module);
    });
}

bool objectOfLibraryFileIsResultOfBody(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    if (module.requireResult != RequireResult::DefaultExport || module.exportRule != SyntheticExportRule::RuntimeModule || !module.body)
        return false;
    const Function& body = program.at(module.body);
    return body.hasBody() && !body.isCoroutine() && body.results.size() == 1;
}

ModuleID compiledModuleWithObject(const Program& program, BuiltinID builtin)
{
    const Builtin& entry = program.builtins().at(builtin);
    if (entry.holder || !isObjectKind(entry.kind) || entry.origin != BuiltinOrigin::Host)
        return { };
    return program.compiledModuleOfNamespaceObject(builtin);
}

ModuleID commonJsModuleCapturedBy(const Program& program, const Module& facade)
{
    if (facade.kind != ModuleKind::SourceText || facade.exportRule == SyntheticExportRule::None || facade.requests.isEmpty())
        return { };
    const ModuleRequest& first = facade.requests[0];
    if (first.kind != ModuleRequestKind::Require)
        return { };
    const Module& file = program.at(first.target);
    return file.kind == ModuleKind::CommonJs && file.exportsVariable ? first.target : ModuleID();
}

Vector<ModuleID, 2> formsInImage(const Program& program, ModuleID named)
{
    Vector<ModuleID, 2> forms;
    auto visitFormsOf = [&](ModuleID file) {
        std::span<const uint8_t> path = program.string(program.at(file).path);
        if (program.at(file).kind == ModuleKind::Data) {
            if (auto mark = std::find(path.rbegin(), path.rend(), '?'); mark != path.rend())
                path = path.first(path.rend() - mark - 1);
        }
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            std::span<const uint8_t> pathOfForm = program.string(module.path);
            if (module.kind != ModuleKind::Data || ModuleID(index) == file || !spanHasPrefix(pathOfForm, path))
                continue;
            if (pathOfForm.size() == path.size() || pathOfForm[path.size()] == '?')
                forms.append(ModuleID(index));
        }
    };
    visitFormsOf(named);
    const Module& module = program.at(named);
    if (module.kind == ModuleKind::SourceText && !module.requests.isEmpty() && module.requests[0].kind == ModuleRequestKind::Require)
        visitFormsOf(module.requests[0].target);
    return forms;
}

bool loaderEntersBodyAtMostOnce(const Module& module)
{
    if (!isEvaluatedAtMostOnceOf(module.kind) || module.flags.containsAny({ ModuleFlags::MayBeReevaluated, ModuleFlags::MayBeMocked }))
        return false;
    if (!module.flags.contains(ModuleFlags::MayBeEvicted))
        return true;
    return module.kind == ModuleKind::SourceText && module.syntheticExports.isEmpty();
}

bool isBodyWrittenInAnotherFile(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    return function.isModuleBody() && function.module && program.at(function.module).body != id;
}

bool importMayAnswerWithMock(const Module& module)
{
    return (module.runtimeModule || module.flags.contains(ModuleFlags::IsBundlerHelper)) && module.flags.contains(ModuleFlags::MayBeMocked);
}

bool requireMayAnswerWithMock(const Program& program, const Module& module)
{
    return importMayAnswerWithMock(module) && !equalSpans(program.string(module.path), byteCast<uint8_t>("bun"_span));
}

bool requireReadsModuleValue(const Program& program, const Module& module)
{
    return (module.kind == ModuleKind::RuntimeSource || module.kind == ModuleKind::Runtime) && !module.exportsVariable && !requireMayAnswerWithMock(program, module);
}

static bool isLibraryModule(const Program& program, const Module& module)
{
    return module.kind == ModuleKind::RuntimeSource || module.kind == ModuleKind::Runtime || module.flags.contains(ModuleFlags::IsLibrary) || isBundlerHelperModule(program, module);
}

bool requireAddsToRequireCache(const Program& program, ModuleID referrer, ModuleID target)
{
    if (!referrer || isLibraryModule(program, program.at(referrer)) || !isLibraryModule(program, program.at(target)))
        return false;
    std::span<const uint8_t> name = program.string(program.at(target).path);
    return !spanHasPrefix(name, byteCast<uint8_t>("node:"_span)) && !equalSpans(name, byteCast<uint8_t>("bun"_span)) && !equalSpans(name, byteCast<uint8_t>("bun:test"_span));
}

bool importStatementListsInRequireCache(const Program& program, ModuleID target)
{
    if (!isLibraryModule(program, program.at(target)))
        return false;
    std::span<const uint8_t> name = program.string(program.at(target).path);
    return !spanHasPrefix(name, byteCast<uint8_t>("node:"_span)) && !spanHasPrefix(name, byteCast<uint8_t>("bun:"_span)) && !equalSpans(name, byteCast<uint8_t>("bun"_span));
}

bool loaderFillsVariablesOffValueOf(const Module& module)
{
    return module.kind == ModuleKind::RuntimeSource && module.body && module.exportRule == SyntheticExportRule::RuntimeModule && module.requireResult == RequireResult::DefaultExport;
}

BuiltinID hostRequireFunctionFor(const Module& module)
{
    switch (module.kind) {
    case ModuleKind::CommonJs:
        return Builtins::PrivateGlobalsRequireCommonJS;
    case ModuleKind::SourceText:
        return Builtins::PrivateGlobalsEsmLoadSync;
    case ModuleKind::RuntimeSource:
        return Builtins::PrivateGlobalsCreateInternalModuleById;
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool reachesModuleObjects(const Program& program)
{
    return std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::ReachesModuleObjects);
    });
}

bool parsersCouldMakeRecordsOf(const Program& program, LayoutID id)
{
    constexpr OptionSet<LayoutFlags> flagsOfOtherLayouts { LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction, LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsNeverAllocated, LayoutFlags::IsWrittenUnderDictionaryType, LayoutFlags::IsDerived, LayoutFlags::IsOfCreatedObject };
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.constructor || layout.base || layout.flags.containsAny(flagsOfOtherLayouts) || layout.fields.isEmpty())
        return false;
    return std::ranges::all_of(layout.fields, [&](const Field& field) {
        if (!field.key)
            return false;
        TypeID written = field.declared.written();
        if (!written)
            return true;
        Type type = program.at(written);
        return type.isAnything() || !type.mayBeFunction();
    });
}

bool isCommonJSFacade(const Program& program, const Module& module)
{
    if (module.kind != ModuleKind::SourceText || module.requests.isEmpty() || module.requests[0].kind != ModuleRequestKind::Require)
        return false;
    const Module& file = program.at(module.requests[0].target);
    if (file.kind != ModuleKind::CommonJs || file.exportRule == SyntheticExportRule::None)
        return false;
    auto displayPath = [&](const Module& shown) {
        return shown.displayPath ? program.string(shown.displayPath) : std::span<const uint8_t>();
    };
    return std::ranges::equal(displayPath(module), displayPath(file));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
