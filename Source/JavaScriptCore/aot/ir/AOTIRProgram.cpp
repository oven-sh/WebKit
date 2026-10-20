/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRProgram.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <wtf/Atomics.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Program);

Program::Program(const BuiltinTable& builtins)
    : types(builtins, this)
    , m_builtins(builtins)
{
}

void Program::adoptBuffer(Vector<uint8_t>&& bytes)
{
    m_bytes.owned = WTF::move(bytes);
    m_bytes.borrowed = m_bytes.owned.span();
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
    case LayoutKind::BuiltinResult:
        return !program.builtins().at(layout.builtinLayout).isTuple;
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
    struct Holder {
        PropertyKeyID key;
        LayoutID layout;
        LayoutID declarer;
    };
    auto keyAndLayoutOf = [](const Holder& holder) {
        return std::pair { holder.key, holder.layout };
    };

    Vector<Holder> holders;
    Vector<LayoutID, 8> visitedClasses;
    for (size_t index = 0; index < layouts.size(); ++index) {
        LayoutID id(index);
        if (at(id).flow.allocation == AllocationState::NeverAllocated || !keepsPropertiesInFields(*this, id))
            continue;

        // Predicted bases may form a cycle.
        visitedClasses.shrink(0);
        bool inheritsFields = true;
        for (LayoutID current = id; current && !visitedClasses.contains(current); current = expectedBaseOf(at(current))) {
            visitedClasses.append(current);
            const Layout& layout = at(current);
            for (uint32_t field = 0; inheritsFields && field < layout.fields.size(); ++field) {
                if (PropertyKeyID key = layout.fields[field].key)
                    holders.append({ key, id, declarationOf(*this, { current, field }).layout });
            }
            for (const Vector<ClassMember>* members : { &layout.classFlow.members, &layout.classFlow.namedMembers }) {
                for (const ClassMember& member : *members) {
                    if (member.key && member.side == ClassSide::Prototype)
                        holders.append({ member.key, id, current });
                }
            }
            inheritsFields &= layout.flow.baseState == BaseState::Layout;
        }
    }
    // What a lookup finds first was added first.
    std::ranges::stable_sort(holders, { }, keyAndLayoutOf);
    holders.shrink(std::ranges::unique(holders, { }, keyAndLayoutOf).begin() - holders.begin());

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
        if (LayoutID layout = program.at(entry.module).namespaceLayout)
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
    if (nextLine == positions.lineStarts.begin())
        return { 1, 1 };
    uint32_t line = static_cast<uint32_t>(nextLine - positions.lineStarts.begin() - 1);
    uint32_t lineStart = positions.lineStarts[line];

    std::span<const ColumnAdjustment> adjustments = positions.columnAdjustments.span();
    size_t first = std::ranges::lower_bound(adjustments, lineStart, { }, &ColumnAdjustment::offset) - adjustments.begin();
    size_t end = std::ranges::lower_bound(adjustments, position, { }, &ColumnAdjustment::offset) - adjustments.begin();
    uint32_t column = position - lineStart;
    for (size_t index = first; index < end; ++index)
        column -= std::min(column, adjustments[index].excessBytes);
    return { line + 1, column + 1 };
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
        const Function& function = program.functions[index];
        return function.mayBeCalledByHost() && !isFoundByHostThroughRegistration(function.role);
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
        size_t numberOfValues = program.functions[index].values.size();
        if (numberOfValues >= fewestValues)
            found.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | index);
    });
    std::ranges::sort(keys);
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
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

ProgramEditor::ProgramEditor(Program& program)
    : m_program(program)
    , m_isChanged(FillWith { }, program.functions.size(), static_cast<uint8_t>(0))
{
}

void ProgramEditor::didChange(FunctionID id)
{
    RELEASE_ASSERT(id.index() < m_isChanged.size());
    WTF::atomicStore(&m_isChanged[id.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
}

void ProgramEditor::didGiveOutSharedRecord()
{
    WTF::atomicStore(&m_mayHaveChangedSharedRecords, static_cast<uint8_t>(1), std::memory_order_relaxed);
}

Function& ProgramEditor::at(FunctionID id)
{
    didChange(id);
    return m_program.functions[id.index()];
}

FunctionID ProgramEditor::addFunction(Function&& function)
{
    RELEASE_ASSERT(m_isChanged.size() == m_program.functions.size());
    FunctionID id(m_program.functions.size());
    m_program.functions.append(WTF::move(function));
    m_isChanged.append(static_cast<uint8_t>(1));
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
    RELEASE_ASSERT(m_isChanged.size() == m_program.functions.size());
    FunctionID first(m_program.functions.size());
    m_program.functions.grow(m_program.functions.size() + count);
    m_isChanged.insertFill(m_isChanged.size(), static_cast<uint8_t>(1), count);
    return first;
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
    function.blocks.clear();
    function.values.clear();
    function.inlineFrames.clear();
}

LayoutID ProgramEditor::addLayout(Layout&& layout)
{
    didGiveOutSharedRecord();
    LayoutID id(m_program.layouts.size());
    m_program.layouts.append(WTF::move(layout));
    initializeAddedLayout(m_program, id);
    return id;
}

LayoutID ProgramEditor::reserveLayouts(unsigned count)
{
    didGiveOutSharedRecord();
    LayoutID first(m_program.layouts.size());
    m_program.layouts.grow(m_program.layouts.size() + count);
    return first;
}

Layout& ProgramEditor::at(LayoutID id)
{
    didGiveOutSharedRecord();
    return m_program.at(id);
}

Variable& ProgramEditor::at(VariableID id)
{
    didGiveOutSharedRecord();
    return m_program.at(id);
}

VariableID ProgramEditor::addVariable(Variable&& variable)
{
    VariableID id(m_program.variables.size());
    m_program.variables.append(WTF::move(variable));
    return id;
}

Module& ProgramEditor::at(ModuleID id)
{
    didGiveOutSharedRecord();
    return m_program.at(id);
}

DataID ProgramEditor::addData(Data&& data)
{
    DataID id(m_program.data.size());
    m_program.data.append(WTF::move(data));
    return id;
}

Vector<FunctionID> ProgramEditor::changedFunctions() const
{
    Vector<FunctionID> changedFunctions;
    for (size_t index = 0; index < m_isChanged.size(); ++index) {
        if (m_isChanged[index])
            changedFunctions.append(FunctionID(index));
    }
    return changedFunctions;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
