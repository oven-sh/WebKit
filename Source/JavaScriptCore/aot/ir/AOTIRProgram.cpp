/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRProgram.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilyTables.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Program);

Program::Program(const BuiltinTable& builtins)
    : types(builtins, this)
    , m_builtins(builtins)
    , m_familyTables(makeUnique<FamilyTables>(*this))
{
}

Program::~Program() = default;

void Program::adoptBuffer(Vector<uint8_t>&& bytes)
{
    m_bytes.owned = WTF::move(bytes);
    m_bytes.borrowed = m_bytes.owned.span();
}

std::span<const uint8_t> Program::copyBorrowedBytes(std::span<const uint8_t> bytes, const ParallelLoops& loops)
{
    constexpr size_t chunkSize = 256 * KB;
    RELEASE_ASSERT(!ownsBuffer());
    m_bytes.owned.grow(bytes.size());
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + chunkSize - 1) / chunkSize), [&](unsigned chunk) {
        size_t offset = chunk * chunkSize;
        memcpySpan(m_bytes.owned.mutableSpan().subspan(offset), bytes.subspan(offset, std::min(chunkSize, bytes.size() - offset)));
    });
    return m_bytes.owned.span();
}

void Program::copyBorrowedLineStarts(const ParallelLoops& loops)
{
    Vector<size_t> firstLineStarts(modules.size());
    size_t count = 0;
    for (size_t index = 0; index < modules.size(); ++index)
        firstLineStarts[index] = std::exchange(count, count + modules[index].positions.lineStarts.size());
    m_bytes.lineStarts.grow(count);
    forEachIndexInParallel(loops, static_cast<unsigned>(modules.size()), [&](unsigned index) {
        std::span<const uint32_t>& lineStarts = modules[index].positions.lineStarts;
        std::span<uint32_t> copy = m_bytes.lineStarts.mutableSpan().subspan(firstLineStarts[index], lineStarts.size());
        memcpySpan(copy, lineStarts);
        lineStarts = copy;
    });
}

void Program::recordCountsAsRead()
{
    m_countsAsRead = { static_cast<unsigned>(functions.size()), static_cast<unsigned>(layouts.size()) };
}

void Program::setNumberOfFunctionsAsRead(unsigned count)
{
    RELEASE_ASSERT(count <= functions.size());
    m_countsAsRead.functions = count;
}

BuiltinName Program::builtinNameOf(PropertyKeyID id) const
{
    const PropertyKey& key = at(id);
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
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Program::mayBeStoredThroughUnknownReceiver(PropertyKeyID key) const
{
    return !m_keysStoredThroughUnknownReceivers || std::ranges::binary_search(*m_keysStoredThroughUnknownReceivers, key);
}

NameClass Program::nameClassOf(PropertyKeyID id) const
{
    const PropertyKey& key = at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        return strings.nameClass(key.text);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return NameClass::Ordinary;
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

std::span<const LayoutID> Program::layoutsBelow(LayoutID id) const
{
    const Vector<Vector<LayoutID>>& layoutsBelow = m_derivedTables.layoutsBelow;
    return id.index() < layoutsBelow.size() ? layoutsBelow[id.index()].span() : std::span<const LayoutID> { };
}

std::span<const LayoutID> Program::layoutsExpectedBelow(LayoutID id) const
{
    const Vector<Vector<LayoutID>>& layoutsExpectedBelow = m_derivedTables.layoutsExpectedBelow;
    return id.index() < layoutsExpectedBelow.size() ? layoutsExpectedBelow[id.index()].span() : std::span<const LayoutID> { };
}

static bool isDefinedByClass(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

bool Program::prototypeMaySelectOtherClass(LayoutID id) const
{
    return isDefinedByClass(at(declaredLayoutOf(*this, id))) && m_derivedTables.numberOfClasses > 1;
}

void Program::indexClasses()
{
    auto provenBaseOf = [&](LayoutID id) {
        const LayoutFlow& flow = at(id).flow;
        return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
    };

    Vector<Vector<LayoutID>>& layoutsBelow = m_derivedTables.layoutsBelow;
    layoutsBelow.clear();
    layoutsBelow.grow(layouts.size());
    Vector<Vector<LayoutID>>& layoutsExpectedBelow = m_derivedTables.layoutsExpectedBelow;
    layoutsExpectedBelow.clear();
    layoutsExpectedBelow.grow(layouts.size());
    m_derivedTables.numberOfClasses = 0;
    for (size_t index = 0; index < layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = at(id);
        m_derivedTables.numberOfClasses += isDefinedByClass(layout);
        if (layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (LayoutID base = provenBaseOf(id); base && base != id; base = provenBaseOf(base)) {
            Vector<LayoutID>& below = layoutsBelow[base.index()];
            bool isOnCycle = !below.isEmpty() && below.last() == id;
            if (isOnCycle)
                break;
            below.append(id);
        }
        for (LayoutID base = expectedBaseOf(layout); base && base != id; base = expectedBaseOf(at(base))) {
            Vector<LayoutID>& below = layoutsExpectedBelow[base.index()];
            bool isOnCycle = !below.isEmpty() && below.last() == id;
            if (isOnCycle)
                break;
            below.append(id);
        }
    }
    m_familyTables->didChangeLayouts();
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

std::span<const LayoutID> Program::declarersOfName(PropertyKeyID key) const
{
    return entriesOfName(m_derivedTables.firstLayoutWithName, m_derivedTables.declarersOfName, key);
}

static bool keepsPropertiesInFields(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return !builtinBaseClassOf(program, id);
    // The reader asks before the verifier has refused a layout of this kind that names no entry of the table.
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

static bool hasResolvableOrigin(const Program& program, const Field& field)
{
    if (!field.flags.contains(FieldFlags::IsInherited))
        return true;
    if (!field.originLayout || !field.originField)
        return false;
    const Vector<Field>& originFields = program.at(field.originLayout).fields;
    return *field.originField < originFields.size() && !originFields[*field.originField].flags.contains(FieldFlags::IsInherited);
}

void Program::indexNames()
{
    indexNames(ParallelLoops(1));
}

void Program::indexNames(const ParallelLoops& loops)
{
    // The reader and the text parser ask before the verifier has refused a field whose origin does not resolve. No name can be looked up through one.
    std::atomic<bool> hasResolvableOrigins { true };
    forEachIndexInParallel(loops, layouts.size(), [&](unsigned index) {
        bool areResolvable = std::ranges::all_of(layouts[index].fields, [&](const Field& field) {
            return hasResolvableOrigin(*this, field);
        });
        if (!areResolvable)
            hasResolvableOrigins.store(false, std::memory_order_relaxed);
    });
    if (!hasResolvableOrigins.load(std::memory_order_relaxed))
        return;

    struct Holder {
        PropertyKeyID key;
        LayoutID layout;
        LayoutID declarer;
    };
    auto keyAndLayoutOf = [](const Holder& holder) {
        return std::pair { holder.key, holder.layout };
    };

    Vector<Holder> holders = collectInParallel<Holder>(loops, layouts.size(), [&](unsigned index, Vector<Holder>& found) {
        LayoutID id(index);
        // The names of a map say nothing of who reads such a name elsewhere.
        if (at(id).flow.allocation == AllocationState::NeverAllocated || at(id).has(LayoutFlags::IsWrittenUnderDictionaryType) || !keepsPropertiesInFields(*this, id))
            return;

        // Expected bases may form a cycle.
        size_t firstOfLayout = found.size();
        Vector<LayoutID, 8> visitedClasses;
        bool inheritsFields = true;
        for (LayoutID current = id; current && !visitedClasses.contains(current); current = expectedBaseOf(at(current))) {
            visitedClasses.append(current);
            const Layout& layout = at(current);
            for (uint32_t field = 0; inheritsFields && field < layout.fields.size(); ++field) {
                if (PropertyKeyID key = layout.fields[field].key)
                    found.append({ key, id, declarationOf(*this, { current, field }).layout });
            }
            for (const Vector<ClassMember>* members : { &layout.classFlow.members, &layout.classFlow.namedMembers }) {
                for (const ClassMember& member : *members) {
                    if (member.key && member.side == ClassSide::Prototype)
                        found.append({ member.key, id, current });
                }
            }
            inheritsFields &= layout.flow.baseState == BaseState::Layout;
        }
        std::span<Holder> ofLayout = found.mutableSpan().subspan(firstOfLayout);
        std::ranges::stable_sort(ofLayout, { }, &Holder::key);
        found.shrink(firstOfLayout + (std::ranges::unique(ofLayout, { }, &Holder::key).begin() - ofLayout.begin()));
    });
    parallelSort(loops, holders.mutableSpan(), [&](const Holder& a, const Holder& b) {
        return keyAndLayoutOf(a) < keyAndLayoutOf(b);
    });

    Vector<uint32_t>& first = m_derivedTables.firstLayoutWithName;
    first.fill(0, holders.isEmpty() ? 0 : holders.last().key.index() + 2);
    m_derivedTables.layoutsWithName = holders.map([](const Holder& holder) {
        return holder.layout;
    });
    m_derivedTables.declarersOfName = holders.map([](const Holder& holder) {
        return holder.declarer;
    });
    for (const Holder& holder : holders)
        ++first[holder.key.index() + 1];
    for (size_t index = 1; index < first.size(); ++index)
        first[index] += first[index - 1];
    m_familyTables->didChangeLayouts();
}

void Program::internBuiltinNameKeys()
{
    Vector<PropertyKeyID>& keys = m_derivedTables.builtinNameKeys;
    RELEASE_ASSERT(keys.isEmpty());

    TypeArena arena(types);
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
}

void Program::appendBuiltinLayouts()
{
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

    TypeArena arena(types);
    for (LayoutID id : builtinLayouts) {
        Layout& layout = at(id);
        std::span<const BuiltinLayoutField> builtinFields = m_builtins.at(layout.builtinLayout).fields;
        layout.fields.reserveInitialCapacity(builtinFields.size());
        for (const BuiltinLayoutField& builtinField : builtinFields) {
            Field field;
            if (builtinField.name)
                field.key = keyOf(builtinField.name);
            field.declared.type = arena.intern(typeOfBuiltinLayoutField(*this, builtinField, arena));
            if (!builtinField.isOptional)
                field.flags.add(FieldFlags::IsRequired);
            layout.fields.append(WTF::move(field));
        }
        layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
        setDeclaredMembers(id, arena.completeLayoutSet(singleElementSpan(id)));
    }
}

static TypeID typeOfExportedNamespace(const Program& program, const ModuleExport& entry, TypeArena& arena)
{
    if (!entry.module)
        return TypeTable::object;
    switch (entry.phase) {
    case ImportPhase::Evaluation:
        if (LayoutID layout = program.layoutOfWholeNamespace(entry.module))
            return arena.intern(Type::forLayout(layout, arena));
        return TypeTable::object;
    case ImportPhase::Defer:
        return TypeTable::object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DeclaredType declaredTypeOfExport(const Program& program, const ModuleExport& entry, TypeArena& arena)
{
    DeclaredType declared;
    declared.type = TypeTable::anything;
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        if (!entry.variable)
            return declared;
        declared = program.at(entry.variable).declared;
        declared.site = { };
        return declared;
    case ModuleExportKind::Namespace:
        declared.type = typeOfExportedNamespace(program, entry, arena);
        return declared;
    case ModuleExportKind::Indirect:
        return declared;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Layout createNamespaceLayout(const Program& program, ModuleID id, TypeArena& arena)
{
    const Module& module = program.at(id);
    Layout layout;
    layout.kind = LayoutKind::ModuleNamespace;
    layout.module = id;
    layout.namespaceOf = id;
    layout.fields.reserveInitialCapacity(module.exports.size());
    for (const ModuleExport& entry : module.exports) {
        Field field;
        field.key = arena.textKey(entry.name);
        field.declared = declaredTypeOfExport(program, entry, arena);
        field.flags = { FieldFlags::IsRequired, FieldFlags::IsImmutable };
        layout.fields.append(WTF::move(field));
    }
    layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
    return layout;
}

void Program::appendNamespaceLayouts()
{
    size_t nextLayoutIndex = layouts.size();
    for (Module& module : modules) {
        if (module.kind != ModuleKind::SourceText)
            continue;
        RELEASE_ASSERT(!module.namespaceLayout);
        module.namespaceLayout = LayoutID(nextLayoutIndex++);
    }
    layouts.reserveCapacity(nextLayoutIndex);

    TypeArena arena(types);
    for (size_t index = 0; index < modules.size(); ++index) {
        LayoutID id = modules[index].namespaceLayout;
        if (!id)
            continue;
        RELEASE_ASSERT(id.index() == layouts.size());
        layouts.append(createNamespaceLayout(*this, ModuleID(index), arena));
        setDeclaredMembers(id, arena.completeLayoutSet(singleElementSpan(id)));
    }
}

LayoutID Program::layoutOfWholeNamespace(ModuleID id) const
{
    const Module& module = at(id);
    return module.exportRule == SyntheticExportRule::None ? module.namespaceLayout : LayoutID();
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

StringID Program::pathForLineColumns(ModuleID id) const
{
    const Module& module = at(id);
    return module.flags.contains(ModuleFlags::HasInstalledPositions) ? module.path : module.displayPath;
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

// The host can name a function of the library only as a module body or through `runtimeFunctions`.
static bool isFoundByHostThroughRegistration(FunctionRole role)
{
    return role == FunctionRole::Library || role == FunctionRole::Model;
}

BitVector functionsCalledByHost(const Program& program, const ParallelLoops& loops)
{
    BitVector isCalledByHost(program.functions.size());
    Vector<uint32_t> functionsForHost = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        return facts.mayBeCalledByHost() && !isFoundByHostThroughRegistration(facts.role);
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
    for (const Module& module : program.modules) {
        if (module.body)
            isCalledByHost.set(module.body.index());
    }
    return isCalledByHost;
}

Vector<uint32_t> largeFunctions(const Program& program, const ParallelLoops& loops)
{
    constexpr size_t fewestValues = 4096;
    Vector<uint64_t> keys = collectInParallel<uint64_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint64_t>& found) {
        size_t numberOfValues = numberOfValuesIn(program, FunctionID(index));
        if (numberOfValues >= fewestValues)
            found.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | index);
    });
    std::ranges::sort(keys);
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });
}

void sortLargestFirst(const Program& program, const ParallelLoops& loops, Vector<FunctionID>& functions)
{
    constexpr size_t functionsPerShare = 1024;
    auto forEachPosition = [&](const auto& functor) {
        forEachIndexInParallel(loops, static_cast<unsigned>((functions.size() + functionsPerShare - 1) / functionsPerShare), [&](unsigned share) {
            for (size_t position = share * functionsPerShare; position < std::min(functions.size(), (share + 1) * functionsPerShare); ++position)
                functor(position);
        });
    };
    Vector<uint64_t> keys(functions.size());
    forEachPosition([&](size_t position) {
        FunctionID id = functions[position];
        keys[position] = (UINT32_MAX - static_cast<uint64_t>(numberOfValuesIn(program, id))) << 32 | id.index();
    });
    parallelSort(loops, keys.mutableSpan(), std::less<uint64_t> { });
    forEachPosition([&](size_t position) {
        functions[position] = FunctionID(static_cast<uint32_t>(keys[position]));
    });
}

BitVector functionsCalledByHost(const Program& program)
{
    return functionsCalledByHost(program, ParallelLoops(1));
}

const RuntimeFunctionEntry* findRuntimeFunction(const Program& program, RuntimeFunctionID id)
{
    if (!id)
        return nullptr;
    auto entry = std::ranges::lower_bound(program.runtimeFunctions, id, { }, &RuntimeFunctionEntry::id);
    return entry != program.runtimeFunctions.end() && entry->id == id ? &*entry : nullptr;
}

// A pass that replaces a function in place gives it another role.
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

ModuleID compiledModuleWithObject(const Program& program, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (entry.holder || !isObjectKind(entry.kind) || entry.origin != BuiltinOrigin::Host)
        return { };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        BuiltinID object = builtins.namespaceOf(module.runtimeModule);
        if (object && builtins.canonicalID(object) == builtin)
            return ModuleID(index);
    }
    return { };
}

ModuleID commonJsModuleCapturedBy(const Program& program, const Module& face)
{
    if (face.kind != ModuleKind::SourceText || face.exportRule == SyntheticExportRule::None || face.requests.isEmpty())
        return { };
    const ModuleRequest& first = face.requests[0];
    if (first.kind != ModuleRequestKind::Require)
        return { };
    const Module& file = program.at(first.target);
    return file.kind == ModuleKind::CommonJs && file.exportsVariable ? first.target : ModuleID();
}

bool namesWaysToModuleObjects(const Program& program)
{
    return std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::NamesWayToModuleObjects);
    });
}

ProgramEditor::ProgramEditor(Program& program)
    : m_program(program)
    , m_changes(FillWith { }, program.functions.size(), static_cast<uint8_t>(0))
    , m_isChangedLayout(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0))
    , m_isChangedVariable(FillWith { }, program.variables.size(), static_cast<uint8_t>(0))
{
    if (BodyIndex* index = program.bodyIndex())
        index->growTo(program.functions.size());
}

void ProgramEditor::didChange(FunctionID id, uint8_t change)
{
    RELEASE_ASSERT(id.index() < m_changes.size());
    AtomicRef(m_changes[id.index()]).fetch_or(change, std::memory_order_relaxed);
    if (!(change & (hasChangedOperations | wasLowered)))
        return;
    if (BodyIndex* index = m_program.bodyIndex())
        index->didGiveOut(id);
}

Function& ProgramEditor::at(FunctionID id)
{
    didChange(id, hasChangedOperations);
    return m_program.functions[id.index()];
}

Function& ProgramEditor::atForPlacedWords(FunctionID id)
{
    didChange(id, hasChangedPlacedWords);
    return m_program.functions[id.index()];
}

Function& ProgramEditor::atForLowering(FunctionID id)
{
    didChange(id, wasLowered);
    return m_program.functions[id.index()];
}

Function& ProgramEditor::atForStorage(FunctionID id)
{
    didChange(id, hasChangedStorage);
    return m_program.functions[id.index()];
}

FunctionID ProgramEditor::addFunction(Function&& function)
{
    RELEASE_ASSERT(m_changes.size() == m_program.functions.size());
    FunctionID id(m_program.functions.size());
    m_program.functions.append(WTF::move(function));
    m_changes.append(hasChangedOperations);
    if (BodyIndex* index = m_program.bodyIndex())
        index->didAddFunctionsUpTo(m_program.functions.size());
    return id;
}

void ProgramEditor::inheritFrom(FunctionID original, Function& function) const
{
    const Function& originalFunction = m_program.functions[original.index()];
    if (!function.derivation.source) {
        FunctionID source = originalFunction.derivation.source;
        function.derivation.source = source ? source : original;
    }
    function.module = originalFunction.module;
}

FunctionID ProgramEditor::addFunctionLike(FunctionID original, Function&& function)
{
    inheritFrom(original, function);
    return addFunction(WTF::move(function));
}

FunctionID ProgramEditor::reserveFunctions(unsigned count)
{
    RELEASE_ASSERT(m_changes.size() == m_program.functions.size());
    FunctionID first(m_program.functions.size());
    m_program.functions.grow(m_program.functions.size() + count);
    m_changes.insertFill(m_changes.size(), hasChangedOperations, count);
    if (BodyIndex* index = m_program.bodyIndex())
        index->didAddFunctionsUpTo(m_program.functions.size());
    return first;
}

ProgramEditor::ReplacedFunctions::ReplacedFunctions(ProgramEditor& editor, std::initializer_list<std::span<const FunctionID>> lists)
    : m_program(editor.m_program)
    , m_functions(editor.m_program.functions.size())
{
    RELEASE_ASSERT(!m_program.m_functionsBeingReplaced);
    for (std::span<const FunctionID> list : lists) {
        for (FunctionID id : list)
            m_functions.quickSet(id.index());
    }
    m_program.m_functionsBeingReplaced = &m_functions;
}

ProgramEditor::ReplacedFunctions::~ReplacedFunctions()
{
    m_program.m_functionsBeingReplaced = nullptr;
}

void ProgramEditor::replaceFunction(FunctionID id, Function&& function)
{
    at(id) = WTF::move(function);
}

void ProgramEditor::replaceFunctionLike(FunctionID id, FunctionID original, Function&& function)
{
    inheritFrom(original, function);
    replaceFunction(id, WTF::move(function));
}

void ProgramEditor::removeBody(FunctionID id)
{
    Function& function = at(id);
    function.editAllBlocks().clear();
    function.editAllValues().clear();
    function.inlineFrames.clear();
}

LayoutID ProgramEditor::addLayout(Layout&& layout)
{
    m_program.familyTables().didChangeLayouts();
    LayoutID id(m_program.layouts.size());
    m_program.layouts.append(WTF::move(layout));
    m_isChangedLayout.append(static_cast<uint8_t>(0));
    initializeAddedLayout(m_program, id);
    return id;
}

Layout& ProgramEditor::at(LayoutID id)
{
    m_program.familyTables().didChangeLayouts();
    AtomicRef(m_isChangedLayout[id.index()]).fetch_or(isChangedLayout, std::memory_order_relaxed);
    return m_program.at(id);
}

Field& ProgramEditor::at(FieldReference reference)
{
    m_program.familyTables().didChangeLayouts();
    {
        Locker locker { m_changedFieldsLock };
        m_changedFields.append(reference);
    }
    return m_program.at(reference.layout).fields[reference.field];
}

Layout& ProgramEditor::atForHiddenFields(LayoutID id)
{
    m_program.familyTables().didChangeLayouts();
    Layout& layout = m_program.at(id);
    AtomicRef state(m_isChangedLayout[id.index()]);
    if (!(state.load(std::memory_order_relaxed) & mayHaveNewHiddenFields))
        state.fetch_or(layout.mayKeepFieldsOutOfLine() ? mayHaveNewHiddenFields : mayHaveNewHiddenFields | keptAllFieldsInline, std::memory_order_relaxed);
    return layout;
}

void ProgramEditor::setInnerObjects(LayoutID id, Vector<InnerObject>&& innerObjects)
{
    m_program.at(id).innerObjects = WTF::move(innerObjects);
}

Variable& ProgramEditor::at(VariableID id)
{
    WTF::atomicStore(&m_isChangedVariable[id.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
    return m_program.at(id);
}

VariableID ProgramEditor::addVariable(Variable&& variable)
{
    VariableID id(m_program.variables.size());
    m_program.variables.append(WTF::move(variable));
    m_isChangedVariable.append(static_cast<uint8_t>(0));
    return id;
}

Module& ProgramEditor::at(ModuleID id)
{
    return m_program.at(id);
}

DataID ProgramEditor::addData(Data&& data)
{
    DataID id(m_program.data.size());
    m_program.data.append(WTF::move(data));
    return id;
}

// Most passes mark a few records of hundreds of thousands, and each is asked several times what it marked: eight entries without a mark are passed over at a time.
static void forEachMarked(std::span<const uint8_t> marks, NOESCAPE const Invocable<void(size_t index, uint8_t mark)> auto& functor)
{
    size_t index = 0;
    for (; index + sizeof(uint64_t) <= marks.size(); index += sizeof(uint64_t)) {
        if (!WTF::unalignedLoad<uint64_t>(marks.subspan(index).data()))
            continue;
        for (size_t marked = index; marked < index + sizeof(uint64_t); ++marked) {
            if (marks[marked])
                functor(marked, marks[marked]);
        }
    }
    for (; index < marks.size(); ++index) {
        if (marks[index])
            functor(index, marks[index]);
    }
}

template<typename RecordID>
static Vector<RecordID> collectMarked(const Vector<uint8_t>& marks, uint8_t wanted)
{
    Vector<RecordID> marked;
    forEachMarked(marks.span(), [&](size_t index, uint8_t mark) {
        if (mark & wanted)
            marked.append(RecordID(index));
    });
    return marked;
}

Vector<FunctionID> ProgramEditor::changedFunctions() const
{
    return collectMarked<FunctionID>(m_changes, hasChangedOperations | hasChangedPlacedWords | wasLowered | hasChangedStorage);
}

Vector<FunctionID> ProgramEditor::functionsWithChangedOperations() const
{
    return collectMarked<FunctionID>(m_changes, hasChangedOperations | hasChangedStorage);
}

Vector<FunctionID> ProgramEditor::functionsThatWereOnlyLowered() const
{
    Vector<FunctionID> functions;
    forEachMarked(m_changes.span(), [&](size_t index, uint8_t changes) {
        if ((changes & (hasChangedOperations | wasLowered | hasChangedStorage)) == wasLowered)
            functions.append(FunctionID(index));
    });
    return functions;
}

bool ProgramEditor::wasGivenOutOnlyForStorage(FunctionID id) const
{
    return (m_changes[id.index()] & (hasChangedOperations | wasLowered | hasChangedStorage)) == hasChangedStorage;
}

Vector<LayoutID> ProgramEditor::changedLayouts() const
{
    Vector<LayoutID> layouts;
    for (size_t index = 0; index < m_isChangedLayout.size(); ++index) {
        uint8_t state = m_isChangedLayout[index];
        if ((state & isChangedLayout) || ((state & keptAllFieldsInline) && m_program.layouts[index].mayKeepFieldsOutOfLine()))
            layouts.append(LayoutID(index));
    }
    return layouts;
}

Vector<FieldReference> ProgramEditor::changedFields() const
{
    Locker locker { m_changedFieldsLock };
    Vector<FieldReference> fields = m_changedFields;
    std::ranges::sort(fields);
    removeRepeatedElements(fields);
    return fields;
}

Vector<VariableID> ProgramEditor::changedVariables() const
{
    return collectMarked<VariableID>(m_isChangedVariable, 1);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
