/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTTypedCompactTable.h"
#include "FPRInfo.h"
#include "Options.h"
#include <numeric>
#include <wtf/Condition.h>
#include <wtf/MallocSpan.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static String keyOf(std::span<const uint8_t> bytes)
{
    return bytes.empty() ? emptyString() : String(byteCast<Latin1Character>(bytes));
}

void Layout::indexFields()
{
    static constexpr unsigned minFieldsToIndex = 8;
    if (fields.size() < minFieldsToIndex)
        return;
    fieldsByName.resize(fields.size());
    std::iota(fieldsByName.begin(), fieldsByName.end(), 0);
    std::ranges::sort(fieldsByName, [&](uint32_t a, uint32_t b) {
        return std::pair(fields[a].name.index(), a) < std::pair(fields[b].name.index(), b);
    });
}

std::optional<uint32_t> Layout::fieldNamed(StringID name) const
{
    if (fieldsByName.size() != fields.size()) {
        for (uint32_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == name && !(fields[i].flags & fieldIsNeverStored))
                return i;
        }
        return std::nullopt;
    }
    auto found = std::ranges::lower_bound(fieldsByName, name.index(), { }, [&](uint32_t i) {
        return fields[i].name.index();
    });
    for (; found != fieldsByName.end() && fields[*found].name == name; ++found) {
        if (!(fields[*found].flags & fieldIsNeverStored))
            return *found;
    }
    return std::nullopt;
}

LineColumn Program::lineColumnFor(ModuleID id, uint32_t position) const
{
    if (!id || id.index() >= modules.size())
        return { 1, 1 };
    auto& module = modules[id.index()];
    uint32_t line = static_cast<uint32_t>(std::ranges::upper_bound(module.lineStarts, position) - module.lineStarts.begin() - 1);
    uint32_t column = position - module.lineStarts[line];
    auto after = std::ranges::upper_bound(module.columnAdjustments, position, { }, &Module::ColumnAdjustment::offset);
    if (after != module.columnAdjustments.begin() && (after - 1)->line == line)
        column -= (after - 1)->excessBytes;
    return { line + 1, column + 1 };
}

uint32_t Function::allocationOf(Allocation&& allocation)
{
    size_t found = allocations.find(allocation);
    if (found == notFound) {
        found = allocations.size();
        allocations.append(WTF::move(allocation));
    }
    return found;
}

bool Program::isUpcast(LayoutID from, LayoutID to) const
{
    if (from == to || at(to).acceptsUncheckedObjects())
        return true;
    bool untypedObjectsPass = !at(from).acceptsUntypedObjects || (!at(from).acceptsUncheckedObjects() && at(to).acceptsOnlyOtherLayouts);
    return untypedObjectsPass && isObjectLayout(at(from).kind) && std::ranges::includes(at(to).subtypes, at(from).subtypes);
}

static bool isConstantOfType(const Value& constant, const Type& type)
{
    auto isIntegerOfType = [&](int64_t integer) {
        return type.primitives & Primitive::Int && type.integers.contains(integer);
    };
    switch (constant.opcode) {
    case Opcode::Undefined:
        return type.primitives & Primitive::Undefined;
    case Opcode::Null:
        return type.primitives & Primitive::Null;
    case Opcode::Boolean:
        return type.primitives & (constant.boolean() ? Primitive::True : Primitive::False);
    case Opcode::Integer:
        return isIntegerOfType(constant.integer());
    case Opcode::Double: {
        double number = std::bit_cast<double>(constant.bits());
        if (number == std::trunc(number) && std::abs(number) <= static_cast<double>(maxTrackedInteger) && !(!number && std::signbit(number)))
            return isIntegerOfType(static_cast<int64_t>(number));
        return type.primitives & Primitive::Double;
    }
    case Opcode::String:
        return type.primitives & (Primitive::String | Primitive::LiteralString);
    case Opcode::BigInt:
        return type.primitives & Primitive::BigInt;
    default:
        return false;
    }
}

ValueID Program::constantOf(const Function& function, ValueID id) const
{
    ValueID constant = id;
    while (function.at(constant).opcode == Opcode::Cast)
        constant = function.at(constant).child(0);
    if (!isConstantOfType(function.at(constant), at(types.anything)))
        return { };
    for (; id != constant; id = function.at(id).child(0)) {
        if (!isConstantOfType(function.at(constant), typeOf(function, id)))
            return { };
    }
    return constant;
}

ValueID Program::originOf(const Function& function, ValueID id) const
{
    Vector<ValueID, 8> casts;
    for (; function.at(id).opcode == Opcode::Cast; id = function.at(id).child(0))
        casts.append(id);
    for (ValueID castID : casts | std::views::reverse) {
        auto& cast = function.at(castID);
        CastReason reason = cast.castReason();
        if (reason != CastReason::As && reason != CastReason::Coerce)
            continue;
        auto& from = typeOf(function, id);
        auto& to = at(cast.type);
        bool isLeftUnchanged = !((reason == CastReason::As || cast.coercesFreshData()) && (from.anyObject || from.acceptsUntypedObjects(*this)));
        if (isLeftUnchanged && reason == CastReason::Coerce && at(cast.coercedType()).primitives & (Primitive::Number | Primitive::String | Primitive::Boolean | Primitive::BigInt)) {
            if (isConstantOfType(function.at(id), at(types.anything)))
                isLeftUnchanged = isConstantOfType(function.at(id), to);
            else
                isLeftUnchanged = !(from.primitives & ~to.primitives) && (!(from.primitives & Primitive::Int) || from.integers.isSubsetOf(to.integers)) && (!from.hasObjects() || to.hasObjects());
        }
        if (!isLeftUnchanged)
            id = castID;
    }
    return id;
}

static BuiltinPath builtinMemberPath(const Program& program, const BuiltinPath& base, StringID name)
{
    auto& builtins = Builtins::singleton();
    BuiltinName member = program.builtinName(name);
    if (!base || base.member)
        return { };
    if (base.isGlobalObject) {
        if (program.isGlobalThisName(name))
            return base;
        return builtins.isGlobal(member) ? BuiltinPath { false, false, name, { } } : BuiltinPath { };
    }
    if (!member)
        return { };
    BuiltinName holder = program.builtinName(base.global);
    const EngineClass* holderClass = builtins.classNamed(holder);
    if (base.isOnPrototype) {
        BuiltinID entry = builtins.findForInstance(BuiltinKind::Method, *holderClass, member);
        return entry && builtins.hasCell(entry) ? BuiltinPath { false, true, base.global, name } : BuiltinPath { };
    }
    if (member == builtins.prototypeName())
        return builtins.classWithFixedPrototypeNamed(holder) ? BuiltinPath { false, true, base.global, { } } : BuiltinPath { };
    if (BuiltinID inherited = builtins.hasOwnProperty(member); inherited && holderClass == &builtins.objectClass() && builtins.hasCell(inherited))
        return BuiltinPath { false, true, base.global, name };
    BuiltinID entry = builtins.findStatic(BuiltinKind::StaticFunction, holder, member);
    return entry && builtins.hasCell(entry) ? BuiltinPath { false, false, base.global, name } : BuiltinPath { };
}

struct PredictedBuiltinsQuery {
    const Program& program;
    const Function& function;
    bool joinsParameters;
    Vector<uint32_t>* queriedEntryParameters { nullptr };
    Vector<Vector<const Target*, 2>> incomingTargets { };
    Vector<ValueID, 8> parametersBeingJoined { };
    unsigned joinsLeft { 0 };

    PredictedBuiltins ask(ValueID id)
    {
        joinsLeft = joinsParameters ? 32 : 0;
        return of(id);
    }

    PredictedBuiltins forHolder(ValueID id)
    {
        auto& value = function.at(program.originOf(function, id));
        if ((value.opcode != Opcode::NewObject && value.opcode != Opcode::NewArray) || !value.arguments.isEmpty())
            return of(id);
        auto global = program.find(asBytes((value.opcode == Opcode::NewObject ? "Object"_s : "Array"_s).span8()));
        return global ? PredictedBuiltins { BuiltinPath { false, true, *global, { } }, { }, false } : PredictedBuiltins { };
    }

    PredictedBuiltins of(ValueID id)
    {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        auto& value = function.at(id);
        switch (value.opcode) {
        case Opcode::GetGlobal:
            if (program.isGlobalThisName(value.string()))
                return { BuiltinPath { true, false, { }, { } }, { }, false };
            if (Builtins::singleton().isGlobal(program.builtinName(value.string())))
                return { BuiltinPath { false, false, value.string(), { } }, { }, false };
            return { };
        case Opcode::GetModuleVariable:
            return program.variables[value.variable().index()].predictedBuiltins;
        case Opcode::GetField:
            return program.at(value.layout()).fields[value.field()].predictedBuiltins;
        case Opcode::GetById: {
            PredictedBuiltins base = forHolder(value.child(0));
            PredictedBuiltins members;
            members.add(builtinMemberPath(program, base.first, value.string()));
            members.add(builtinMemberPath(program, base.second, value.string()));
            return members;
        }
        case Opcode::Parameter:
            if (value.parameterBlock().index())
                return join(id, value);
            if (queriedEntryParameters)
                queriedEntryParameters->append(value.parameterIndex());
            if (value.parameterIndex() < function.predictedBuiltinsOfEntryParameters.size())
                return function.predictedBuiltinsOfEntryParameters[value.parameterIndex()];
            return { };
        case Opcode::GetCapture: {
            auto& expected = program.functions[value.function().index()].predictedBuiltinsOfEntryParameters;
            return value.captureIndex() < expected.size() ? expected[value.captureIndex()] : PredictedBuiltins { };
        }
        default:
            return { };
        }
    }

    PredictedBuiltins join(ValueID id, const Value& parameter)
    {
        if (!joinsLeft || parametersBeingJoined.size() == parametersBeingJoined.capacity() || parametersBeingJoined.contains(id))
            return { };
        if (incomingTargets.isEmpty()) {
            incomingTargets.grow(function.blocks.size());
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets)
                    incomingTargets[target.block.index()].append(&target);
            }
        }
        --joinsLeft;
        parametersBeingJoined.append(id);
        PredictedBuiltins joined;
        for (const Target* target : incomingTargets[parameter.parameterBlock().index()]) {
            if (parameter.parameterIndex() < target->arguments.size())
                joined.add(of(target->arguments[parameter.parameterIndex()]));
        }
        parametersBeingJoined.removeLast();
        return joined.hasTooMany ? PredictedBuiltins { } : joined;
    }
};

PredictedBuiltins Program::predictedBuiltins(const Function& function, ValueID id) const
{
    return PredictedBuiltinsQuery { *this, function, false }.ask(id);
}

FunctionID Program::functionByKey(StringID property, StringID key) const
{
    auto orderOf = [](StringID property, StringID key) {
        return static_cast<uint64_t>(property.index()) << 32 | key.index();
    };
    auto found = std::ranges::lower_bound(functionsByKey, orderOf(property, key), { }, [&](auto& entry) {
        return orderOf(entry.property, entry.key);
    });
    return found != functionsByKey.end() && found->property == property && found->key == key ? found->function : FunctionID();
}

std::optional<StringID> Program::find(std::span<const uint8_t> bytes) const
{
    StringView string(byteCast<Latin1Character>(bytes));
    auto& table = m_stringIDs[stringIDTableForHash(bytes.empty() ? WTF::emptyString().impl()->hash() : string.hash())];
    auto iterator = bytes.empty() ? table.find(keyOf(bytes)) : table.find<StringViewHashTranslator>(string);
    if (iterator == table.end())
        return std::nullopt;
    return StringID(iterator->value);
}

StringID Program::intern(std::span<const uint8_t> bytes)
{
    String key = keyOf(bytes);
    auto& table = m_stringIDs[stringIDTableForHash(key.impl()->hash())];
    auto result = table.add(WTF::move(key), static_cast<uint32_t>(strings.size()));
    if (result.isNewEntry) {
        m_ownStrings.append(Vector<uint8_t>(bytes));
        strings.append(m_ownStrings.last().span());
        recordString(StringID(result.iterator->value));
    }
    return StringID(result.iterator->value);
}

void Program::replaceString(std::span<const uint8_t> from, std::span<const uint8_t> to)
{
    RELEASE_ASSERT(std::ranges::all_of(m_stringIDs, [](auto& table) { return table.isEmpty(); }));
    for (auto& string : strings) {
        if (!equalSpans(string, from))
            continue;
        m_ownStrings.append(Vector<uint8_t>(to));
        string = m_ownStrings.last().span();
    }
}

bool Program::mayUseSlimClosures(const Function& function) const
{
    switch (function.kind) {
    case FunctionKind::Plain:
        if (function.thisType || function.assertedThisType || function.has(Format::FunctionHasNewTarget))
            return false;
        break;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Bound:
        break;
    case FunctionKind::Constructor:
    case FunctionKind::Module:
        return false;
    }
    if (function.isUsedAsObject || function.declaredAs & (functionIsGenerator | functionReadsNewTarget | functionIsInBuiltinModule))
        return false;
    return !function.has(Format::FunctionIsBuiltin) && !function.has(Format::FunctionIsInBuiltinModule) && !function.has(Format::FunctionIsGenerator) && !function.isGenericEntrypoint() && !function.takesCalleeClosure && !isResumeFunction(function);
}

std::pair<unsigned, unsigned> Program::closureReferenceAndCaptureCounts(const Function& function) const
{
    RELEASE_ASSERT(!function.isGenericEntrypoint());
    if (function.kind == FunctionKind::Constructor)
        return at(function.constructedClass).isEvaluatedOnce ? std::pair(0u, 0u) : std::pair(1u, 1u);
    unsigned references = 0;
    for (TypeID type : function.captureTypes)
        references += holdsCells(repOf(at(type)));
    return { references, static_cast<unsigned>(function.captureTypes.size()) };
}

void Program::collectNamesAndStringsFromValues()
{
    Vector<Vector<StringID>> functionNames(functions.size());
    Vector<NameUses> stringUses(strings.size());
    Vector<uint32_t> constantUses(FillWith { }, strings.size(), 0);
    auto recordAccessorNames = [&](unsigned i, const Function& function) {
        Vector<ValueID, 2> descriptorLiterals;
        auto definesField = [](const Value& value) {
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
        };
        static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
        std::as_const(functions).forEachValue(i, builtinCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.arguments.size() < 2)
                return;
            auto& builtin = Builtins::singleton().at(value.builtin());
            bool definesAccessor = builtin.implementation.kind == BuiltinImplementation::Operation && (builtin.implementation.operation == LanguageOperation::DefineGetter || builtin.implementation.operation == LanguageOperation::DefineSetter);
            bool isStaticFunction = builtin.kind == BuiltinKind::StaticFunction;
            bool takesDescriptor = isStaticFunction && (builtin.holder == "Object"_s || builtin.holder == "Reflect"_s) && builtin.name == "defineProperty"_s;
            if ((definesAccessor || takesDescriptor) && function.at(value.arguments[1]).opcode == Opcode::String)
                functionNames[i].append(function.at(value.arguments[1]).string());
            if (isStaticFunction && builtin.holder == "Object"_s && (builtin.name == "defineProperties"_s || builtin.name == "create"_s))
                descriptorLiterals.append(value.arguments[1]);
        });
        if (descriptorLiterals.isEmpty())
            return;
        std::as_const(functions).forEachValue(i, builtinCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.arguments.size() == 3 && definesField(value) && descriptorLiterals.contains(value.arguments[0]) && function.at(value.arguments[1]).opcode == Opcode::String)
                functionNames[i].append(function.at(value.arguments[1]).string());
        });
    };
    auto countNameUses = [&](unsigned i, const Function& function) {
        static constexpr OpcodeSet nameUses { Opcode::GetById, Opcode::PutById, Opcode::CallMethod, Opcode::InById, Opcode::DeleteById, Opcode::GetGlobal, Opcode::GetGlobalForTypeOf, Opcode::PutGlobal, Opcode::CallBuiltin };
        if (!functions.mayContain(i, nameUses))
            return;
        BitVector runOnceValues;
        if (runsOnce(FunctionID(i))) {
            BitVector isInLoop = blocksInLoops(std::as_const(functions), i);
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (isInLoop.get(b))
                    continue;
                for (ValueID id : function.blocks[b].values)
                    runOnceValues.set(id.index());
            }
        }
        std::as_const(functions).forEachValue(i, nameUses, [&](ValueID id) {
            auto& value = function.at(id);
            unsigned index = id.index();
            StringID name;
            AccessByName access = value.opcode == Opcode::GetById ? AccessByName::Read : value.opcode == Opcode::PutById ? AccessByName::Store : value.opcode == Opcode::CallMethod ? AccessByName::Call : AccessByName::Other;
            if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod || value.opcode == Opcode::InById || value.opcode == Opcode::DeleteById
                || value.opcode == Opcode::GetGlobal || value.opcode == Opcode::GetGlobalForTypeOf || value.opcode == Opcode::PutGlobal)
                name = value.string();
            else if (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).hasReceiver()) {
                name = stringOf(Builtins::singleton().at(value.builtin()).nameName);
                access = AccessByName::Call;
            }
            else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 3 && function.at(value.arguments[1]).opcode == Opcode::String) {
                auto& builtin = Builtins::singleton().at(value.builtin());
                bool isDefineField = builtin.implementation.kind == BuiltinImplementation::Operation && builtin.implementation.operation == LanguageOperation::DefineField;
                bool isDefineProperty = builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
                if (isDefineField || isDefineProperty)
                    name = function.at(value.arguments[1]).string();
                if (isDefineField)
                    access = AccessByName::Definition;
            }
            else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 4 && function.at(value.arguments[1]).opcode == Opcode::String) {
                auto& builtin = Builtins::singleton().at(value.builtin());
                if (builtin.implementation.kind == BuiltinImplementation::Operation && builtin.implementation.operation == LanguageOperation::SuperGet) {
                    name = function.at(value.arguments[1]).string();
                    access = AccessByName::Other;
                    if (auto prototype = find(asBytes("prototype"_s.span8()))) {
                        auto& ofPrototype = stringUses[prototype->index()];
                        WTF::atomicExchangeAdd(&ofPrototype.uses, 1u, std::memory_order_relaxed);
                        WTF::atomicExchangeAdd(&ofPrototype.usesByAccess[static_cast<unsigned>(AccessByName::Read)], 1u, std::memory_order_relaxed);
                    }
                }
            }
            if (!name)
                return;
            auto& nameEntry = stringUses[name.index()];
            WTF::atomicExchangeAdd(&nameEntry.uses, 1u, std::memory_order_relaxed);
            if (!runOnceValues.get(index)) {
                WTF::atomicExchangeAdd(&nameEntry.tableLookupUses, 1u, std::memory_order_relaxed);
                for (uint32_t first = WTF::atomicLoad(&nameEntry.firstTableLookupFunction, std::memory_order_relaxed); i < first && !WTF::atomicCompareExchangeWeakRelaxed(&nameEntry.firstTableLookupFunction, first, i);)
                    first = WTF::atomicLoad(&nameEntry.firstTableLookupFunction, std::memory_order_relaxed);
            }
            if (access != AccessByName::Other)
                WTF::atomicExchangeAdd(&nameEntry.usesByAccess[static_cast<unsigned>(access)], 1u, std::memory_order_relaxed);
        });
    };
    forEachIndexInParallel(functions.size(), [&](unsigned i) {
        auto& function = std::as_const(functions)[i];
        recordAccessorNames(i, function);
        countNameUses(i, function);
        static constexpr OpcodeSet strings { Opcode::String };
        std::as_const(functions).forEachValue(i, strings, [&](ValueID id) {
            WTF::atomicExchangeAdd(&constantUses[function.at(id).string().index()], 1u, std::memory_order_relaxed);
        });
    });
    m_valueNamesAndStrings = ValueNamesAndStrings { WTF::move(functionNames), WTF::move(stringUses), WTF::move(constantUses) };
}

void Program::findPossibleAccessorNames()
{
    if (!m_valueNamesAndStrings)
        collectNamesAndStringsFromValues();
    possibleAccessorNames.clearAll();
    for (auto& names : m_valueNamesAndStrings->accessorNames) {
        for (StringID name : names)
            possibleAccessorNames.set(name.index());
    }
    for (auto& layout : layouts) {
        if (!isObjectLayout(layout.kind))
            continue;
        for (auto& method : layout.methods) {
            if (method.name && method.kind != MethodKind::Method)
                possibleAccessorNames.set(method.name.index());
        }
    }
}

auto Program::genericAccessNameUses() const -> Vector<NameUses>
{
    RELEASE_ASSERT(m_valueNamesAndStrings && m_valueNamesAndStrings->nameUses.size() == strings.size());
    Vector<NameUses> stringUses = m_valueNamesAndStrings->nameUses;
    BitVector isNameInLayout(strings.size());
    auto note = [&](StringID name) {
        if (name)
            isNameInLayout.quickSet(name.index());
    };
    for (auto& layout : layouts) {
        if (!isObjectLayout(layout.kind))
            continue;
        for (auto& field : layout.fields)
            note(field.name);
        for (auto& method : layout.methods)
            note(method.name);
        for (auto& method : layout.staticMethods)
            note(method.name);
        for (auto& field : layout.staticFields)
            note(field.name);
    }
    Vector<NameUses> result;
    for (uint32_t index = 0; index < strings.size(); ++index) {
        auto& nameEntry = stringUses[index];
        if (!nameEntry.uses && !isNameInLayout.quickGet(index))
            continue;
        StringID name(index);
        if (!isConstantSymbol(string(name)) && !isEnginePrivateName(string(name)) && String::fromUTF8(string(name)).isNull())
            continue;
        nameEntry.name = name;
        result.append(nameEntry);
    }
    return result;
}

Vector<uint32_t> Program::mostUsedStringKeys(unsigned limit) const
{
    static constexpr uint32_t minUsesForDedicatedStringStub = isARM64() ? 20 : 7;
    RELEASE_ASSERT(m_valueNamesAndStrings && m_valueNamesAndStrings->stringConstantUses.size() == strings.size());
    auto& uses = m_valueNamesAndStrings->stringConstantUses;
    Vector<uint32_t> maximum;
    for (uint32_t index = 0; index < uses.size(); ++index) {
        if (uses[index] >= minUsesForDedicatedStringStub && string(StringID(index)).size() > 1)
            maximum.append(index);
    }
    std::ranges::sort(maximum, [&](uint32_t a, uint32_t b) {
        return uses[a] != uses[b] ? uses[a] > uses[b] : a < b;
    });
    if (maximum.size() > limit)
        maximum.shrink(limit);
    for (uint32_t& index : maximum)
        ++index;
    return maximum;
}

void Program::recordString(StringID id)
{
    BuiltinName name = Builtins::singleton().nameOf(string(id));
    m_builtinNames.append(name);
    if (!name)
        return;
    if (m_builtinNameStrings.size() <= name.index())
        m_builtinNameStrings.grow(name.index() + 1);
    m_builtinNameStrings[name.index()] = id;
}

namespace {

class ConstantSymbolResolution {
public:
    explicit ConstantSymbolResolution(Program& program)
        : m_program(program)
        , m_mutableProgram(program)
    {
    }

    void run()
    {
        auto symbol = m_program.find(asBytes("Symbol"_s.span8()));
        if (!symbol || !Builtins::singleton().isGlobal(m_program.builtinName(*symbol)))
            return;
        m_symbol = *symbol;
        m_for = m_program.find(asBytes("for"_s.span8())).value_or(StringID());
        collect();
        if (!resolveConstantSymbols())
            return;
        m_names.fill(StringID(), m_program.variables.size());
        m_nameRefutingErrors.fill(VariableID(), m_program.variables.size());
        collectExports();
        findSymbolVariableCandidates();
        trackLateEscapingLiterals();
        while (resolveSymbolVariables()) { }
        m_provenLiteralReads = m_literalReads;
        m_treatsFailureAsFinal = true;
        m_mayRunAgain.clear();
        m_literalValues.clear();
        m_literalReads.clear();
        trackLateEscapingLiterals();
        while (resolveSymbolVariables()) { }
        resolveConstantKeys();
    }

private:
    struct Location {
        FunctionID function;
        unsigned block { 0 };
        unsigned index { 0 };
    };

    struct FunctionFacts {
        BitVector blocksInLoops;
        BitVector blocksAfterFunctionValues;
        Vector<unsigned> firstFunctionValue;
    };

    struct Export {
        FunctionID function;
        ValueID value;
    };

    struct ModuleEvaluations {
        Vector<Vector<Location, 2>> callers;
        UncheckedKeyHashMap<uint32_t, Vector<std::pair<Location, unsigned>, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> calledIn;
        BitVector hasUnknownEvaluators;
        Vector<Vector<FunctionID, 2>> codeOf;
        BitVector waits;
    };

    enum class ChangeKind : uint8_t { TestBeforeNamedAccess, FirstEvaluationSymbol, FailureMarker };

    struct Change {
        ChangeKind kind;
        unsigned block;
        unsigned index;
        StringID name;
        VariableID error;
    };

    static ValueID withoutUpcasts(const Function& function, ValueID id)
    {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
            id = function.at(id).child(0);
        return id;
    }

    static std::optional<Location> placeOf(FunctionID id, const Function& function, ValueID value)
    {
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& values = function.blocks[b].values;
            for (unsigned k = 0; k < values.size(); ++k) {
                if (values[k] == value)
                    return Location { id, b, k };
            }
        }
        return std::nullopt;
    }

    static uint64_t exportKey(uint32_t module, StringID name) { return static_cast<uint64_t>(module) << 32 | name.index(); }

    std::optional<uint32_t> requiredBuiltinModule(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::Operation || m_program.operationOf(value) != Operations::RequireBuiltinModule || value.arguments.size() != 1)
            return std::nullopt;
        auto& number = function.at(value.arguments[0]);
        if (number.opcode != Opcode::Integer || number.integer() < 0 || number.integer() >= UINT32_MAX)
            return std::nullopt;
        return static_cast<uint32_t>(number.integer());
    }

    bool runsOnce(FunctionID id) const
    {
        auto& function = m_program.at(id);
        return (function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && m_program.modules[function.module.index()].initializer == id);
    }

    bool isSymbolCall(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::Call || value.arguments.size() > 1 || (!value.arguments.isEmpty() && function.at(value.arguments[0]).opcode != Opcode::String))
            return false;
        auto& callee = function.at(value.child(0));
        return callee.opcode == Opcode::GetGlobal && callee.string() == m_symbol;
    }

    bool isSymbolForCall(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::CallMethod || !m_for || value.string() != m_for || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::String)
            return false;
        auto& receiver = function.at(value.child(0));
        return receiver.opcode == Opcode::GetGlobal && receiver.string() == m_symbol;
    }

    void collect()
    {
        struct Found {
            Vector<std::pair<uint32_t, Location>> stores;
            Vector<std::pair<uint32_t, Location>> clearsInHandlers;
            Vector<uint32_t> escapedExports;
            Vector<Location> calls;
            Vector<std::pair<uint32_t, uint32_t>> moduleBodyCalls;
            Vector<std::pair<uint32_t, Location>> referencesToHiddenCode;
            bool isModuleCode { false };
        };
        unsigned count = m_program.functions.size();
        Vector<Found> foundIn(count);
        m_isReadFromOutside.fill(0, m_program.variables.size());
        m_hasUnprovenRead.fill(0, m_program.variables.size());
        m_isEnteredFromOutside.fill(0, m_program.modules.size());
        m_numberOfReads.fill(0, m_program.variables.size());
        m_isClearedInHandler.fill(0, m_program.variables.size());
        m_isClearedElsewhere.fill(0, m_program.variables.size());
        auto note = [](uint8_t& flag) {
            WTF::atomicStore(&flag, static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        forEachIndexInParallel(count, [&](unsigned i) {
            static constexpr OpcodeSet collected { Opcode::PutModuleVariable, Opcode::GetModuleVariable, Opcode::Call, Opcode::CallMethod, Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::CallDirect, Opcode::Operation };
            if (!m_program.functions.mayContain(i, collected))
                return;
            auto& function = m_program.functions[i];
            auto& found = foundIn[i];
            found.isModuleCode = function.kind == FunctionKind::Module && function.module;
            Vector<std::pair<ValueID, uint32_t>, 8> required;
            bool isRunOnce = runsOnce(FunctionID(i));
            m_program.functions.forEachIndexedValueWithBlock(i, collected, [&](unsigned b, unsigned k, ValueID id) {
                auto& value = function.at(id);
                switch (value.opcode) {
                case Opcode::PutModuleVariable:
                    found.stores.append({ value.variable().index(), Location { FunctionID(i), b, k } });
                    if (function.at(value.child(0)).opcode == Opcode::Undefined) {
                        bool isInHandler = function.at(function.blocks[b].values[0]).opcode == Opcode::CatchValue;
                        note((isInHandler ? m_isClearedInHandler : m_isClearedElsewhere)[value.variable().index()]);
                        if (isInHandler)
                            found.clearsInHandlers.append({ value.variable().index(), Location { FunctionID(i), b, k } });
                    }
                    break;
                case Opcode::GetModuleVariable:
                    WTF::atomicExchangeAdd(&m_numberOfReads[value.variable().index()], 1u, std::memory_order_relaxed);
                    if (m_program.variables[value.variable().index()].module != function.module)
                        note(m_isReadFromOutside[value.variable().index()]);
                    if (!value.readsInitializedVariable())
                        note(m_hasUnprovenRead[value.variable().index()]);
                    break;
                case Opcode::Call:
                    if (isRunOnce && isSymbolCall(function, value))
                        found.calls.append(Location { FunctionID(i), b, k });
                    break;
                case Opcode::CallMethod:
                    if (isSymbolForCall(function, value))
                        found.calls.append(Location { FunctionID(i), b, k });
                    break;
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                case Opcode::CallDirect:
                    if (ModuleID module = m_program.at(value.function()).module; module && module != function.module && !runsOnce(value.function()))
                        note(m_isEnteredFromOutside[module.index()]);
                    if (m_program.at(value.function()).kind == FunctionKind::Module)
                        found.moduleBodyCalls.append({ value.function().index(), value.opcode == Opcode::CallDirect ? i : UINT32_MAX });
                    if (auto& callee = m_program.at(value.function()); callee.module && (callee.kind == FunctionKind::Module || callee.has(Format::FunctionIsHiddenInStackTraces)))
                        found.referencesToHiddenCode.append({ value.function().index(), Location { value.opcode == Opcode::CallDirect ? FunctionID(i) : FunctionID(), b, k } });
                    break;
                case Opcode::Operation:
                    if (auto module = requiredBuiltinModule(function, value))
                        required.append({ id, *module });
                    break;
                default:
                    break;
                }
            });
            if (required.isEmpty())
                return;
            auto noteUse = [&](ValueID used) {
                for (auto [exports, module] : required) {
                    if (exports == used)
                        found.escapedExports.append(module);
                }
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::GetById && value.opcode != Opcode::IsUndefinedOrNull)
                        value.forEachChild(noteUse);
                }
                const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& used) {
                    noteUse(used);
                });
            }
        });
        m_numberOfStores.fill(0, m_program.variables.size());
        m_stores.fill(Location { }, m_program.variables.size());
        for (unsigned i = 0; i < count; ++i) {
            auto& found = foundIn[i];
            if (found.isModuleCode)
                m_moduleCode.append(i);
            for (auto& [variable, location] : found.stores) {
                m_numberOfStores[variable] = static_cast<uint8_t>(std::min(m_numberOfStores[variable] + 1, 3));
                m_stores[variable] = location;
            }
            m_clearsInHandlers.appendVector(found.clearsInHandlers);
            for (uint32_t module : found.escapedExports)
                m_escapedExports.add(module);
            m_calls.appendVector(found.calls);
            m_referencesToHiddenCode.appendVector(found.referencesToHiddenCode);
            for (auto [callee, caller] : found.moduleBodyCalls) {
                if (!m_moduleBodyCallers.add(callee, caller).isNewEntry)
                    m_moduleBodyCallers.set(callee, UINT32_MAX);
            }
        }
        destroyOnHelperThread(WTF::move(foundIn));
    }

    bool mayThrow(const Function& function) const
    {
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch && block.terminator.kind != TerminatorKind::Return)
                return true;
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                switch (value.opcode) {
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Integer:
                case Opcode::Double:
                case Opcode::String:
                case Opcode::NewObject:
                case Opcode::FunctionReference:
                case Opcode::PutModuleVariable:
                case Opcode::ConstantSymbol:
                    break;
                case Opcode::GetGlobal:
                    if (value.string() != m_symbol)
                        return true;
                    break;
                case Opcode::GetModuleVariable:
                    if (m_program.variables[value.variable().index()].mayBeReadUninitialized() && !value.readsInitializedVariable())
                        return true;
                    break;
                case Opcode::Call:
                    if (!isSymbolCall(function, value))
                        return true;
                    break;
                case Opcode::CallMethod:
                    if (!isSymbolForCall(function, value))
                        return true;
                    break;
                case Opcode::Operation:
                    if (m_program.operationOf(value) != Operations::DefineField || value.arguments.size() != 3 || function.at(value.arguments[0]).opcode != Opcode::NewObject || function.at(value.arguments[1]).opcode != Opcode::String)
                        return true;
                    break;
                default:
                    return true;
                }
            }
        }
        return false;
    }

    bool mayRunAgain(FunctionID id)
    {
        return m_mayRunAgain.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            if (!function.module)
                return true;
            FunctionID initializer = m_program.modules[function.module.index()].initializer;
            if (initializer == id)
                return false;
            auto caller = m_moduleBodyCallers.find(id.index());
            if (!initializer || caller == m_moduleBodyCallers.end() || caller->value != initializer.index())
                return true;
            auto& evaluates = m_program.at(initializer);
            if (evaluates.blocks.isEmpty() || evaluates.blocks[0].values.isEmpty())
                return true;
            auto& flag = evaluates.at(evaluates.blocks[0].values[0]);
            if (flag.opcode != Opcode::GetModuleVariable)
                return true;
            unsigned variable = flag.variable().index();
            if (m_isClearedElsewhere[variable])
                return true;
            if (!m_isClearedInHandler[variable] || !mayThrow(function))
                return false;
            return !m_treatsFailureAsFinal || !moduleErrorVariable(function.module);
        }).iterator->value;
    }

    VariableID moduleErrorVariable(ModuleID module) const
    {
        FunctionID initializer = module ? m_program.modules[module.index()].initializer : FunctionID();
        if (!initializer)
            return { };
        auto& function = m_program.at(initializer);
        if (function.blocks.isEmpty() || function.blocks[0].terminator.kind != TerminatorKind::Branch)
            return { };
        auto& otherwise = function.blocks[function.blocks[0].terminator.targets[1].block.index()];
        if (otherwise.terminator.kind != TerminatorKind::Branch)
            return { };
        auto& hasThrown = function.blocks[otherwise.terminator.targets[0].block.index()];
        if (hasThrown.values.size() != 1 || hasThrown.terminator.kind != TerminatorKind::Throw || hasThrown.terminator.value != hasThrown.values[0])
            return { };
        auto& error = function.at(hasThrown.values[0]);
        if (error.opcode != Opcode::GetModuleVariable || !m_program.variables[error.variable().index()].startsUndefined)
            return { };
        return error.variable();
    }

    static bool hidesFunctions(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
            return false;
        default:
            return true;
        }
    }

    const FunctionFacts& factsOf(FunctionID id)
    {
        return *m_facts.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            auto facts = makeUniqueWithoutFastMallocCheck<FunctionFacts>();
            unsigned count = function.blocks.size();
            facts->blocksInLoops = blocksInLoops(function);
            facts->blocksAfterFunctionValues.ensureSize(count);
            facts->firstFunctionValue.fill(UINT_MAX, count);
            Vector<unsigned> worklist;
            for (unsigned b = 0; b < count; ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    if (hidesFunctions(function.at(values[k])))
                        continue;
                    facts->firstFunctionValue[b] = k;
                    worklist.append(b);
                    break;
                }
            }
            while (!worklist.isEmpty()) {
                for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                    if (!facts->blocksAfterFunctionValues.set(target.block.index()))
                        worklist.append(target.block.index());
                }
            }
            return facts;
        }).iterator->value;
    }

    bool isAfter(const Location& first, unsigned block, unsigned index) const
    {
        if (first.block == block)
            return first.index < index;
        if (!first.block)
            return true;
        auto& function = m_program.at(first.function);
        BitVector seen(function.blocks.size());
        Vector<unsigned, 16> worklist;
        worklist.append(0);
        seen.set(0);
        while (!worklist.isEmpty()) {
            unsigned current = worklist.takeLast();
            if (current == block)
                return false;
            for (auto& target : function.blocks[current].terminator.targets) {
                unsigned next = target.block.index();
                if (next != first.block && !seen.set(next))
                    worklist.append(next);
            }
        }
        return true;
    }

    bool hasInitialValue(unsigned variable) const
    {
        return m_program.variables[variable].startsUndefined || m_program.variables[variable].startsZero;
    }

    bool isStoredOnceBeforeAllReads(unsigned variable)
    {
        const Location& store = m_stores[variable];
        if (m_numberOfStores[variable] != 1 || !runsOnce(store.function))
            return false;
        auto& facts = factsOf(store.function);
        if (facts.blocksInLoops.get(store.block))
            return false;
        if (!hasInitialValue(variable) || !m_hasUnprovenRead[variable])
            return true;
        ModuleID module = m_program.at(store.function).module;
        if (!module || m_program.variables[variable].module != module || m_isReadFromOutside[variable] || m_isEnteredFromOutside[module.index()])
            return false;
        return !facts.blocksAfterFunctionValues.get(store.block) && facts.firstFunctionValue[store.block] > store.index;
    }

    bool holdsSymbolAtRead(unsigned variable, FunctionID reader, const Function& function, ValueID read) const
    {
        if (!hasInitialValue(variable) || m_stores[variable].function != reader)
            return true;
        auto location = placeOf(reader, function, read);
        return location && isAfter(m_stores[variable], location->block, location->index);
    }

    bool resolveConstantSymbols()
    {
        bool hasAny = false;
        for (const Location& location : m_calls) {
            auto& function = m_mutableProgram.functions[location.function.index()];
            auto& block = function.blocks[location.block];
            auto& call = function.at(block.values[location.index]);
            if (block.terminator.kind == TerminatorKind::Invoke && location.index + 1 == block.values.size())
                continue;
            Vector<uint8_t, 64> name;
            VariableID error;
            name.append(static_cast<uint8_t>(0xfc));
            if (call.opcode == Opcode::CallMethod)
                name.append(static_cast<uint8_t>('r'));
            else {
                if (factsOf(location.function).blocksInLoops.get(location.block) || (m_program.mayRunInMultipleInstances && !function.has(Format::FunctionIsInBuiltinModule)))
                    continue;
                if (mayRunAgain(location.function)) {
                    m_treatsFailureAsFinal = true;
                    m_mayRunAgain.remove(location.function.index());
                    bool mayRunAgainByOtherPaths = mayRunAgain(location.function);
                    m_mayRunAgain.remove(location.function.index());
                    m_treatsFailureAsFinal = false;
                    if (mayRunAgainByOtherPaths)
                        continue;
                    error = moduleErrorVariable(function.module);
                }
                CString numbers = makeString(location.function.index(), '.', block.values[location.index].index()).ascii();
                name.append(byteCast<uint8_t>(numbers.span()));
            }
            name.append(static_cast<uint8_t>(call.arguments.isEmpty() ? ';' : ':'));
            if (!call.arguments.isEmpty())
                name.append(m_program.string(function.at(call.arguments[0]).string()));
            if (error) {
                StringID forFirstEvaluation = m_mutableProgram.intern(name.span());
                m_firstEvaluationSymbols.add(static_cast<uint64_t>(location.function.index()) << 32 | block.values[location.index].index(), forFirstEvaluation);
                m_changes.add(location.function.index(), Vector<Change>()).iterator->value.append(Change { ChangeKind::FirstEvaluationSymbol, location.block, location.index, forFirstEvaluation, error });
                m_failureMarkerErrors.add(error.index());
                hasAny = true;
                continue;
            }
            call.opcode = Opcode::ConstantSymbol;
            call.children = { };
            call.arguments.clear();
            call.immediates = { m_mutableProgram.intern(name.span()).index(), Format::none };
            hasAny = true;
        }
        return hasAny;
    }

    void collectExports()
    {
        if (!m_program.usesOperation(Operations::DefineField))
            return;
        for (auto& [module, id] : m_program.builtinModules) {
            auto& function = m_program.at(id);
            if (m_escapedExports.contains(module) || !function.module || !spanHasPrefix(m_program.string(m_program.modules[function.module.index()].path), asBytes("internal:"_s.span8())))
                continue;
            ValueID exports;
            unsigned returnBlock = 0;
            unsigned numberOfReturns = 0;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (function.blocks[b].terminator.kind != TerminatorKind::Return)
                    continue;
                ++numberOfReturns;
                returnBlock = b;
                exports = function.blocks[b].terminator.value;
            }
            if (numberOfReturns != 1 || !exports)
                continue;
            ValueID exportsStore;
            if (auto& returned = function.at(exports); returned.opcode == Opcode::GetModuleVariable) {
                unsigned variable = returned.variable().index();
                const Location& store = m_stores[variable];
                auto read = placeOf(id, function, exports);
                if (m_numberOfStores[variable] != 1 || m_numberOfReads[variable] != 1 || store.function != id || !read || !isAfter(store, read->block, read->index))
                    continue;
                exportsStore = function.blocks[store.block].values[store.index];
                exports = function.at(exportsStore).child(0);
            }
            if (function.at(exports).opcode != Opcode::NewObject)
                continue;
            Vector<std::pair<StringID, ValueID>, 32> defined;
            bool isOnlyDefinedAndReturned = true;
            for (unsigned b = 0; b < function.blocks.size() && isOnlyDefinedAndReturned; ++b) {
                auto& block = function.blocks[b];
                for (unsigned k = 0; k < block.values.size(); ++k) {
                    auto& value = function.at(block.values[k]);
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= child == exports;
                    });
                    if (!uses || block.values[k] == exportsStore)
                        continue;
                    bool isDefinition = value.opcode == Opcode::Operation && m_program.operationOf(value) == Operations::DefineField && value.arguments.size() == 3 && value.arguments[0] == exports && value.arguments[1] != exports && value.arguments[2] != exports
                        && function.at(value.arguments[1]).opcode == Opcode::String && isAfter(Location { id, b, k }, returnBlock, UINT_MAX);
                    if (!isDefinition) {
                        isOnlyDefinedAndReturned = false;
                        break;
                    }
                    defined.append({ function.at(value.arguments[1]).string(), value.arguments[2] });
                }
                for (auto& target : block.terminator.targets)
                    isOnlyDefinedAndReturned &= !target.arguments.contains(exports);
                if (block.terminator.value == exports && (block.terminator.kind != TerminatorKind::Return || exportsStore))
                    isOnlyDefinedAndReturned = false;
            }
            if (!isOnlyDefinedAndReturned)
                continue;
            for (auto& [name, value] : defined) {
                if (!m_exports.add(exportKey(module, name), Export { id, value }).isNewEntry)
                    m_exports.set(exportKey(module, name), Export { });
            }
        }
    }

    using EdgesIntoBlocks = Vector<Vector<const Target*, 2>>;

    struct ReturnedSymbol {
        StringID name;
        VariableID refutingError;

        explicit operator bool() const { return !!name; }
    };

    ReturnedSymbol returnedSymbolOf(FunctionID id, ValueID returnedValue, EdgesIntoBlocks& edges, unsigned depth = 0) const
    {
        auto& function = m_program.at(id);
        returnedValue = withoutUpcasts(function, returnedValue);
        auto& value = function.at(returnedValue);
        switch (value.opcode) {
        case Opcode::ConstantSymbol:
            return { value.string(), { } };
        case Opcode::Call: {
            if (!m_treatsFailureAsFinal)
                return { };
            auto found = m_firstEvaluationSymbols.find(static_cast<uint64_t>(id.index()) << 32 | returnedValue.index());
            if (found == m_firstEvaluationSymbols.end())
                return { };
            return { found->value, moduleErrorVariable(function.module) };
        }
        case Opcode::GetModuleVariable: {
            unsigned variable = value.variable().index();
            if (!m_names[variable] || !holdsSymbolAtRead(variable, id, function, returnedValue))
                return { };
            return { m_names[variable], m_nameRefutingErrors[variable] };
        }
        case Opcode::GetById: {
            uint64_t read = static_cast<uint64_t>(id.index()) << 32 | returnedValue.index();
            if (!depth && m_literalReads.contains(read)) {
                auto found = m_literalValues.find(exportKey(function.at(withoutUpcasts(function, value.child(0))).variable().index(), value.string()));
                if (found == m_literalValues.end() || !found->value.function)
                    return { };
                ReturnedSymbol created = returnedSymbolOf(found->value.function, found->value.value, edges, depth + 1);
                if (!created || m_provenLiteralReads.contains(read))
                    return created;
                if (created.refutingError)
                    return { };
                return { created.name, moduleErrorVariable(function.module) };
            }
            auto module = requiredBuiltinModule(function, function.at(value.child(0)));
            if (!module || depth)
                return { };
            auto found = m_exports.find(exportKey(*module, value.string()));
            return found != m_exports.end() && found->value.function ? returnedSymbolOf(found->value.function, found->value.value, edges, depth + 1) : ReturnedSymbol { };
        }
        case Opcode::ToPropertyKey:
            return returnedSymbolOf(id, value.child(0), edges, depth + 1);
        case Opcode::Parameter: {
            unsigned b = value.immediates[0];
            if (depth || !b || b >= function.blocks.size())
                return { };
            size_t index = function.blocks[b].parameters.find(returnedValue);
            if (index == notFound)
                return { };
            if (edges.isEmpty()) {
                edges.grow(function.blocks.size());
                for (auto& block : function.blocks) {
                    for (auto& target : block.terminator.targets)
                        edges[target.block.index()].append(&target);
                }
            }
            ReturnedSymbol symbol;
            for (const Target* target : edges[b]) {
                ReturnedSymbol given = index < target->arguments.size() ? returnedSymbolOf(id, target->arguments[index], edges, 1) : ReturnedSymbol { };
                if (!given || (symbol && (given.name != symbol.name || given.refutingError != symbol.refutingError)))
                    return { };
                symbol = given;
            }
            return symbol;
        }
        default:
            return { };
        }
    }

    std::optional<unsigned> evaluatedModuleOf(FunctionID id) const
    {
        auto& function = m_program.at(id);
        if (!function.module)
            return std::nullopt;
        FunctionID initializer = m_program.modules[function.module.index()].initializer;
        if (initializer == id)
            return function.module.index();
        if (function.kind != FunctionKind::Plain || !function.parameterTypes.isEmpty() || function.has(Format::FunctionIsAsync))
            return std::nullopt;
        bool evaluates = false;
        for (auto& value : function.values) {
            switch (value.opcode) {
            case Opcode::CallDirect:
                if (value.function() != initializer)
                    return std::nullopt;
                evaluates = true;
                break;
            case Opcode::Call:
            case Opcode::CallMethod:
            case Opcode::Construct:
            case Opcode::Operation:
            case Opcode::FunctionReference:
            case Opcode::NewClosure:
            case Opcode::NewClosureOnce:
                return std::nullopt;
            default:
                break;
            }
        }
        return evaluates ? std::optional<unsigned>(function.module.index()) : std::nullopt;
    }

    static bool hasHandlers(const Function& function)
    {
        return std::ranges::any_of(function.blocks, [](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke;
        });
    }

    bool isOnAllExitPaths(const Location& location) const
    {
        auto& function = m_program.at(location.function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (function.blocks[b].terminator.kind == TerminatorKind::Return && !isAfter(location, b, UINT_MAX))
                return false;
        }
        return true;
    }

    bool isReevaluatedAfterFailure(const Function& function) const
    {
        FunctionID initializer = m_program.modules[function.module.index()].initializer;
        if (!initializer)
            return false;
        auto& evaluates = m_program.at(initializer);
        if (evaluates.blocks.isEmpty() || evaluates.blocks[0].values.isEmpty())
            return false;
        auto& mark = evaluates.at(evaluates.blocks[0].values[0]);
        return mark.opcode == Opcode::GetModuleVariable && m_isClearedInHandler[mark.variable().index()];
    }

    ModuleEvaluations findModuleEvaluations() const
    {
        unsigned numberOfModules = m_program.modules.size();
        ModuleEvaluations result;
        result.callers.grow(numberOfModules);
        result.hasUnknownEvaluators.ensureSize(numberOfModules);
        for (auto& [callee, location] : m_referencesToHiddenCode) {
            auto module = evaluatedModuleOf(FunctionID(callee));
            if (!module)
                continue;
            if (!location.function || !m_program.at(location.function).module) {
                result.hasUnknownEvaluators.set(*module);
                continue;
            }
            if (m_program.at(location.function).module.index() == *module)
                continue;
            result.callers[*module].append(location);
            result.calledIn.add(location.function.index(), Vector<std::pair<Location, unsigned>, 4> { }).iterator->value.append({ location, *module });
        }
        result.codeOf.grow(numberOfModules);
        result.waits.ensureSize(numberOfModules);
        for (unsigned i : m_moduleCode) {
            auto& function = std::as_const(m_program).functions[i];
            if (function.has(Format::FunctionIsAsync) || function.has(Format::FunctionIsGenerator))
                result.waits.set(function.module.index());
            if (runsOnce(FunctionID(i)))
                result.codeOf[function.module.index()].append(FunctionID(i));
        }
        return result;
    }

    void trackLateEscapingLiterals()
    {
        if (!m_program.usesOperation(Operations::DefineField))
            return;
        struct Literal {
            unsigned variable;
            Vector<Location, 2> escapes;
            Vector<std::pair<Location, ValueID>, 8> reads;
        };
        Vector<Literal> literals;
        Vector<uint32_t> literalOf;
        literalOf.fill(UINT32_MAX, m_program.variables.size());
        for (unsigned i : m_moduleCode) {
            auto& function = std::as_const(m_program).functions[i];
            if (function.blocks.size() != 1 || !runsOnce(FunctionID(i)) || mayThrow(function) || mayRunAgain(FunctionID(i)) || !isReevaluatedAfterFailure(function))
                continue;
            auto& values = function.blocks[0].values;
            for (unsigned last = values.size(); last--;) {
                auto& store = function.at(values[last]);
                if (store.opcode != Opcode::PutModuleVariable || function.at(store.child(0)).opcode != Opcode::NewObject)
                    continue;
                unsigned variable = store.variable().index();
                if (literalOf[variable] != UINT32_MAX || m_program.variables[variable].module != function.module)
                    continue;
                ValueID literal = store.child(0);
                unsigned numberOfStores = 0;
                bool isOnlyDefinedAndStored = true;
                Vector<std::pair<StringID, ValueID>, 32> defined;
                for (unsigned k = 0; k < values.size() && isOnlyDefinedAndStored; ++k) {
                    auto& value = function.at(values[k]);
                    if (value.opcode == Opcode::PutModuleVariable && value.variable().index() == variable) {
                        ++numberOfStores;
                        isOnlyDefinedAndStored &= k <= last;
                    }
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= child == literal;
                    });
                    if (!uses || k == last)
                        continue;
                    bool isDefinition = k < last && value.opcode == Opcode::Operation && m_program.operationOf(value) == Operations::DefineField && value.arguments.size() == 3 && value.arguments[0] == literal && value.arguments[1] != literal
                        && value.arguments[2] != literal && function.at(value.arguments[1]).opcode == Opcode::String;
                    if (!isDefinition) {
                        isOnlyDefinedAndStored = false;
                        break;
                    }
                    defined.append({ function.at(value.arguments[1]).string(), value.arguments[2] });
                }
                if (!isOnlyDefinedAndStored || numberOfStores > 2 || numberOfStores != m_numberOfStores[variable] || function.blocks[0].terminator.value == literal)
                    continue;
                literalOf[variable] = literals.size();
                literals.append(Literal { variable, { }, { } });
                for (auto& [name, value] : defined) {
                    if (!m_literalValues.add(exportKey(variable, name), Export { FunctionID(i), value }).isNewEntry)
                        m_literalValues.set(exportKey(variable, name), Export { });
                }
            }
        }
        if (literals.isEmpty())
            return;

        struct LiteralUse {
            uint32_t literal;
            Location location;
            ValueID read;
        };
        auto uses = collectInParallel<LiteralUse>(m_program.functions.size(), [&](unsigned i, Vector<LiteralUse>& collected) {
            auto& function = m_program.functions[i];
            Vector<std::pair<ValueID, uint32_t>, 4> held;
            static constexpr OpcodeSet variableReads { Opcode::GetModuleVariable };
            m_program.functions.forEachIndexedValue(i, variableReads, [&](ValueID id) {
                if (uint32_t literal = literalOf[function.at(id).variable().index()]; literal != UINT32_MAX)
                    held.append({ id, literal });
            });
            if (held.isEmpty())
                return;
            auto literalOfRead = [&](ValueID id) {
                for (auto [read, literal] : held) {
                    if (read == id)
                        return literal;
                }
                return UINT32_MAX;
            };
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& block = function.blocks[b];
                for (unsigned k = 0; k < block.values.size(); ++k) {
                    auto& value = function.at(block.values[k]);
                    value.forEachChild([&](ValueID child) {
                        uint32_t literal = literalOfRead(child);
                        if (literal == UINT32_MAX || value.opcode == Opcode::IsUndefinedOrNull)
                            return;
                        collected.append({ literal, Location { FunctionID(i), b, k }, value.opcode == Opcode::GetById ? block.values[k] : ValueID() });
                    });
                }
                auto recordEscape = [&](ValueID child) {
                    if (uint32_t literal = literalOfRead(child); literal != UINT32_MAX)
                        collected.append({ literal, Location { FunctionID(i), b, UINT_MAX }, ValueID() });
                };
                if (block.terminator.value)
                    recordEscape(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        recordEscape(argument);
                }
            }
        });
        for (auto& use : uses) {
            if (use.read)
                literals[use.literal].reads.append({ use.location, use.read });
            else
                literals[use.literal].escapes.append(use.location);
        }

        unsigned numberOfModules = m_program.modules.size();
        if (!m_moduleEvaluations)
            m_moduleEvaluations = findModuleEvaluations();
        auto& [evaluationCallers, evaluationsCalledIn, hasUnknownEvaluators, codeOf, waits] = *m_moduleEvaluations;

        auto modulesFinishedAt = [&](const Location& escape) -> std::optional<BitVector> {
            auto& escaping = m_program.at(escape.function);
            if (escaping.kind != FunctionKind::Module || !escaping.module || !runsOnce(escape.function) || mayRunAgain(escape.function) || hasHandlers(escaping))
                return std::nullopt;
            unsigned own = escaping.module.index();
            if (waits.get(own))
                return std::nullopt;
            BitVector ended(numberOfModules);
            Vector<unsigned, 16> worklist;
            auto take = [&](unsigned module) {
                if (!waits.get(module) && !ended.set(module))
                    worklist.append(module);
            };
            for (FunctionID code : codeOf[own]) {
                auto called = evaluationsCalledIn.find(code.index());
                if (called == evaluationsCalledIn.end())
                    continue;
                for (auto& [location, module] : called->value) {
                    if (code == escape.function ? isAfter(location, escape.block, escape.index) : code == m_program.modules[own].initializer)
                        take(module);
                }
            }
            while (!worklist.isEmpty()) {
                unsigned module = worklist.takeLast();
                for (FunctionID code : codeOf[module]) {
                    auto called = evaluationsCalledIn.find(code.index());
                    if (called == evaluationsCalledIn.end())
                        continue;
                    bool isInitializer = code == m_program.modules[module].initializer;
                    if (!isInitializer && (hasHandlers(m_program.at(code)) || mayRunAgain(code)))
                        continue;
                    for (auto& [location, evaluated] : called->value) {
                        if (isInitializer || isOnAllExitPaths(location))
                            take(evaluated);
                    }
                }
            }
            if (ended.get(own))
                return std::nullopt;
            for (bool changed = true; changed;) {
                changed = false;
                for (unsigned module : ended) {
                    bool startsInside = !hasUnknownEvaluators.get(module) && std::ranges::all_of(evaluationCallers[module], [&](const Location& caller) {
                        unsigned calling = m_program.at(caller.function).module.index();
                        return (calling == own || ended.get(calling)) && (runsOnce(caller.function) || !m_isEnteredFromOutside[calling]);
                    });
                    if (!startsInside) {
                        ended.clear(module);
                        changed = true;
                    }
                }
            }
            return ended;
        };

        for (auto& literal : literals) {
            Vector<std::pair<Location, BitVector>, 2> escapes;
            bool isFollowed = true;
            for (auto& escape : literal.escapes) {
                auto ended = modulesFinishedAt(escape);
                if (!ended) {
                    isFollowed = false;
                    break;
                }
                escapes.append({ escape, WTF::move(*ended) });
            }
            if (!isFollowed)
                continue;
            for (auto& taken : literal.reads) {
                const Location& location = taken.first;
                auto& reader = m_program.at(location.function);
                bool isBeforeAllEscapes = escapes.isEmpty() || (reader.kind == FunctionKind::Module && reader.module && runsOnce(location.function) && !mayRunAgain(location.function) && std::ranges::all_of(escapes, [&](auto& escape) {
                    return escape.first.function == location.function ? isAfter(location, escape.first.block, escape.first.index) : escape.second.get(reader.module.index());
                }));
                if (isBeforeAllEscapes)
                    m_literalReads.add(static_cast<uint64_t>(location.function.index()) << 32 | taken.second.index());
            }
        }
    }

    void findSymbolVariableCandidates()
    {
        Vector<uint8_t> isCandidate(FillWith { }, m_program.variables.size(), static_cast<uint8_t>(0));
        forEachIndexInParallel(isCandidate.size(), [&](unsigned variable) {
            if (m_numberOfStores[variable] != 1)
                return;
            const Location& store = m_stores[variable];
            auto& function = std::as_const(m_program).at(store.function);
            switch (function.at(withoutUpcasts(function, function.at(function.blocks[store.block].values[store.index]).child(0))).opcode) {
            case Opcode::ConstantSymbol:
            case Opcode::Call:
            case Opcode::GetModuleVariable:
            case Opcode::GetById:
            case Opcode::ToPropertyKey:
            case Opcode::Parameter:
                isCandidate[variable] = 1;
                break;
            default:
                break;
            }
        });
        for (unsigned variable = 0; variable < isCandidate.size(); ++variable) {
            if (isCandidate[variable])
                m_symbolVariableCandidates.append(variable);
        }
    }

    bool resolveSymbolVariables()
    {
        bool changed = false;
        UncheckedKeyHashMap<uint32_t, EdgesIntoBlocks, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> edgesByFunction;
        for (unsigned variable : m_symbolVariableCandidates) {
            if (m_names[variable])
                continue;
            const Location& store = m_stores[variable];
            auto& function = m_program.at(store.function);
            EdgesIntoBlocks& edges = edgesByFunction.add(store.function.index(), EdgesIntoBlocks()).iterator->value;
            ReturnedSymbol symbol = returnedSymbolOf(store.function, function.at(function.blocks[store.block].values[store.index]).child(0), edges);
            if (!symbol || !isStoredOnceBeforeAllReads(variable))
                continue;
            if (symbol.refutingError && (symbol.refutingError != moduleErrorVariable(m_program.variables[variable].module) || function.module != m_program.variables[variable].module))
                continue;
            m_names[variable] = symbol.name;
            m_nameRefutingErrors[variable] = symbol.refutingError;
            changed = true;
        }
        return changed;
    }

    static bool isObjectWithoutLayout(const Type& type) { return type.anyObject && type.objectLayouts.isEmpty() && type.engineClasses.isEmpty(); }

    static void convertToNamedAccess(Value& value, StringID name)
    {
        ValueID stored = value.opcode == Opcode::PutByVal ? value.child(2) : ValueID();
        value.opcode = value.opcode == Opcode::PutByVal ? Opcode::PutById : Opcode::GetById;
        value.children = { value.child(0), stored, ValueID() };
        value.immediates = { name.index(), Format::none };
        value.setPredictedByKey(StringID());
        value.hasStringKeysByType = false;
    }

    void resolveConstantKeys()
    {
        for (VariableID error : m_nameRefutingErrors) {
            if (error)
                m_failureMarkerErrors.add(error.index());
        }
        for (auto& [mark, location] : m_clearsInHandlers) {
            VariableID error = moduleErrorVariable(m_program.variables[mark].module);
            if (!error || !m_failureMarkerErrors.contains(error.index()))
                continue;
            m_mutableProgram.variables[error.index()].isStoredOnce = false;
            m_changes.add(location.function.index(), Vector<Change>()).iterator->value.append(Change { ChangeKind::FailureMarker, location.block, location.index, { }, error });
        }
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            static constexpr OpcodeSet keyedAccesses { Opcode::GetByVal, Opcode::PutByVal };
            auto& function = m_program.functions[i];
            EdgesIntoBlocks edges;
            Vector<Change> changes;
            if (auto found = m_changes.find(i); found != m_changes.end())
                changes = found->value;
            else if (!m_program.functions.mayContain(i, keyedAccesses))
                return;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    auto& value = function.at(values[k]);
                    if (!keyedAccesses.contains(value.opcode))
                        continue;
                    if ((value.opcode == Opcode::GetByVal && value.immediates[0] != Format::none) || !isObjectWithoutLayout(m_program.typeOf(function, value.child(0))))
                        continue;
                    ValueID key = withoutUpcasts(function, value.child(1));
                    Opcode keyOpcode = function.at(key).opcode;
                    if (keyOpcode != Opcode::GetModuleVariable && keyOpcode != Opcode::ConstantSymbol && keyOpcode != Opcode::Parameter && keyOpcode != Opcode::ToPropertyKey)
                        continue;
                    ReturnedSymbol symbol = returnedSymbolOf(FunctionID(i), key, edges);
                    if (!symbol)
                        continue;
                    if (!symbol.refutingError)
                        convertToNamedAccess(m_mutableProgram.functions[i].at(values[k]), symbol.name);
                    else if (m_failureMarkerErrors.contains(symbol.refutingError.index()))
                        changes.append(Change { ChangeKind::TestBeforeNamedAccess, b, k, symbol.name, symbol.refutingError });
                }
            }
            std::ranges::sort(changes, [](const Change& a, const Change& b) {
                return a.block != b.block ? a.block > b.block : a.index > b.index;
            });
            for (auto& change : changes)
                apply(m_mutableProgram.functions[i], change);
        });
    }

    Value makeValueLike(const Value& other, Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = other.position;
        value.inlineFrame = other.inlineFrame;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    static ValueID add(Function& function, Value&& value)
    {
        function.values.append(WTF::move(value));
        return ValueID(function.values.size() - 1);
    }

    void apply(Function& function, const Change& change) const
    {
        ValueID id = function.blocks[change.block].values[change.index];
        Value replaced = function.at(id);
        if (change.kind == ChangeKind::FailureMarker) {
            Value yes = makeValueLike(replaced, Opcode::Boolean, m_program.types.boolean);
            yes.immediates[0] = 1;
            Value store = replaced;
            store.children[0] = add(function, WTF::move(yes));
            store.immediates[0] = change.error.index();
            ValueID marks[] = { store.children[0], add(function, WTF::move(store)) };
            function.blocks[change.block].values.insertSpan(change.index + 1, std::span<const ValueID> { marks });
            return;
        }
        bool returnsNothing = replaced.rep == Rep::Void;
        if (returnsNothing) {
            bool isUsed = false;
            auto note = [&](ValueID child) {
                isUsed |= child == id;
            };
            for (auto& block : function.blocks) {
                block.terminator.forEachChild(note);
                for (ValueID other : block.values)
                    function.at(other).forEachChild(note);
            }
            if (isUsed)
                return;
        }
        bool isWatched = function.blocks[change.block].terminator.kind == TerminatorKind::Invoke && change.index + 1 == function.blocks[change.block].values.size();
        BlockID after(function.blocks.size());
        BlockID expected(function.blocks.size() + 1);
        BlockID otherwise(function.blocks.size() + 2);
        function.blocks.grow(function.blocks.size() + 3);
        {
            auto& from = function.blocks[change.block];
            auto& rest = function.blocks[after.index()];
            rest.values.append(from.values.subspan(change.index + 1));
            from.values.shrink(change.index);
            rest.terminator = std::exchange(from.terminator, Terminator { });
        }
        Target handler;
        if (isWatched) {
            auto& invoke = function.blocks[after.index()].terminator;
            handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        Value error = makeValueLike(replaced, Opcode::GetModuleVariable, m_program.types.anything);
        error.immediates[0] = change.error.index();
        Value hasNotFailed = makeValueLike(replaced, Opcode::StrictEq, m_program.types.boolean);
        hasNotFailed.children = { add(function, WTF::move(error)), add(function, makeValueLike(replaced, Opcode::Undefined, m_program.types.undefined)), ValueID() };
        ValueID read = hasNotFailed.children[0];
        ValueID undefined = hasNotFailed.children[1];
        ValueID test = add(function, WTF::move(hasNotFailed));
        {
            auto& from = function.blocks[change.block];
            from.values.append(read);
            from.values.append(undefined);
            from.values.append(test);
            from.terminator.kind = TerminatorKind::Branch;
            from.terminator.value = test;
            from.terminator.targets.append(Target { expected, { } });
            from.terminator.targets.append(Target { otherwise, { } });
        }
        Value asPredicted = replaced;
        if (change.kind == ChangeKind::FirstEvaluationSymbol) {
            asPredicted.opcode = Opcode::ConstantSymbol;
            asPredicted.children = { };
            asPredicted.arguments.clear();
            asPredicted.immediates = { change.name.index(), Format::none };
        } else
            convertToNamedAccess(asPredicted, change.name);
        auto finish = [&](BlockID block, ValueID value) {
            auto& finished = function.blocks[block.index()];
            finished.values.append(value);
            finished.terminator.kind = isWatched ? TerminatorKind::Invoke : TerminatorKind::Jump;
            finished.terminator.targets.append(Target { after, returnsNothing ? Arguments { } : Arguments { value } });
            if (isWatched)
                finished.terminator.targets.append(handler);
        };
        finish(expected, add(function, WTF::move(asPredicted)));
        if (returnsNothing) {
            finish(otherwise, id);
            return;
        }
        finish(otherwise, add(function, Value(replaced)));
        Value parameter = makeValueLike(replaced, Opcode::Parameter, replaced.type);
        parameter.rep = replaced.rep;
        parameter.copyPredictionFrom(replaced);
        parameter.immediates = { after.index(), 0 };
        function.at(id) = WTF::move(parameter);
        function.blocks[after.index()].parameters.append(id);
    }

    const Program& m_program;
    Program& m_mutableProgram;
    StringID m_symbol;
    StringID m_for;
    Vector<Location> m_calls;
    Vector<uint32_t> m_moduleCode;
    Vector<uint32_t> m_symbolVariableCandidates;
    Vector<uint32_t> m_numberOfReads;
    Vector<uint8_t> m_numberOfStores;
    Vector<Location> m_stores;
    Vector<uint8_t> m_isReadFromOutside;
    Vector<uint8_t> m_hasUnprovenRead;
    Vector<uint8_t> m_isEnteredFromOutside;
    Vector<uint8_t> m_isClearedInHandler;
    Vector<uint8_t> m_isClearedElsewhere;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_moduleBodyCallers;
    UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_mayRunAgain;
    Vector<StringID> m_names;
    Vector<VariableID> m_nameRefutingErrors;
    bool m_treatsFailureAsFinal { false };
    Vector<std::pair<uint32_t, Location>> m_clearsInHandlers;
    UncheckedKeyHashMap<uint64_t, StringID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_firstEvaluationSymbols;
    UncheckedKeyHashMap<uint32_t, Vector<Change>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_changes;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_failureMarkerErrors;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_provenLiteralReads;
    Vector<std::pair<uint32_t, Location>> m_referencesToHiddenCode;
    std::optional<ModuleEvaluations> m_moduleEvaluations;
    UncheckedKeyHashMap<uint64_t, Export, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_literalValues;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_literalReads;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_escapedExports;
    UncheckedKeyHashMap<uint64_t, Export, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_exports;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<FunctionFacts>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_facts;
};

} // anonymous namespace

class Preparation {
public:
    explicit Preparation(Program& program)
        : m_program(program)
    {
    }

    void run()
    {
        Vector<String> keys(m_program.strings.size());
        Vector<uint8_t> tableOf(keys.size());
        forEachIndexInParallel(keys.size(), [&](unsigned i) {
            keys[i] = keyOf(m_program.strings[i]);
            tableOf[i] = Program::stringIDTableForHash(keys[i].impl()->hash());
        });
        std::array<Vector<uint32_t>, Program::numberOfStringIDTables> numbersIn;
        for (uint32_t i = 0; i < keys.size(); ++i)
            numbersIn[tableOf[i]].append(i);
        forEachIndexInParallel(numbersIn.size(), [&](unsigned number) {
            auto& table = m_program.m_stringIDs[number];
            table.reserveInitialCapacity(numbersIn[number].size());
            for (uint32_t i : numbersIn[number])
                table.add(WTF::move(keys[i]), i);
        });
        m_program.emptyString = m_program.intern({ });
        ConstantSymbolResolution(m_program).run();
        if (m_program.usesOperation(Operations::NewDisposableStack)) {
            m_program.intern(asBytes("The value of a using declaration must be an object, null or undefined"_s.span8()));
            m_program.intern(asBytes("The value of a using declaration has no callable dispose method"_s.span8()));
            for (ASCIILiteral name : { "Symbol"_s, "dispose"_s, "TypeError"_s, "SuppressedError"_s })
                m_program.intern(asBytes(name.span8()));
        }
        expandInitialize();
        linkForInKeys();
        serializeLargeLiterals();
        fillLongLiterals();
        markFunctionsUsedAsObjects();

        m_program.closureLayout = LayoutID(m_program.layouts.size());
        Layout closure;
        closure.kind = LayoutKind::Closure;
        closure.isAllocated = true;
        m_program.layouts.append(WTF::move(closure));

        auto& builtins = Builtins::singleton();
        m_program.preludeFunctions.fill(FunctionID(), numberOfPreludeFunctions);
        auto namedBuiltinFunctions = findIndicesInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = std::as_const(m_program).functions[i];
            return function.has(Format::FunctionIsBuiltin) && function.name && !function.blocks.isEmpty();
        });
        for (uint32_t i : namedBuiltinFunctions) {
            auto& function = std::as_const(m_program).functions[i];
            auto which = builtins.preludeFunctionNamed(m_program.string(function.name));
            if (!which)
                continue;
            RELEASE_ASSERT(function.kind == FunctionKind::Plain && function.captureTypes.isEmpty() && !function.parameterTypes.isEmpty());
            RELEASE_ASSERT(!function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis));
            m_program.preludeFunctions[static_cast<unsigned>(*which)] = FunctionID(i);
            if (*which == PreludeFunction::SetConstruct)
                m_program.functions[i].flags |= Format::FunctionIsHiddenInStackTraces;
        }
        for (auto& engineClass : builtins.classes())
            m_program.engineClassTypes.append(engineClass.canCheckOriginalStructure() ? m_program.types.intern(Type::engineClass(engineClass)) : TypeID());
        m_program.engineClassTypes[builtins.stringClass().index] = m_program.types.string;
        for (unsigned i = 0; i < builtins.size(); ++i) {
            constexpr uint16_t others = BuiltinResult::Undefined | BuiltinResult::Null | BuiltinResult::Boolean | BuiltinResult::String | BuiltinResult::Symbol | BuiltinResult::BigInt;
            static constexpr std::pair<uint16_t, uint16_t> primitives[] = {
                { BuiltinResult::Undefined, Primitive::Undefined },
                { BuiltinResult::Null, Primitive::Null },
                { BuiltinResult::Boolean, Primitive::Boolean },
                { BuiltinResult::String, Primitive::String },
                { BuiltinResult::Symbol, Primitive::Symbol },
                { BuiltinResult::BigInt, Primitive::BigInt },
            };
            auto& result = builtins.at(BuiltinID(i)).result;
            if (!(result.kinds & BuiltinResult::Integer) || result.kinds & ~(BuiltinResult::Integer | others) || result.arguments) {
                m_program.builtinIntegerResultTypes.append(TypeID());
                continue;
            }
            Type returnedValue = Type::integer({ result.minimum, result.maximum });
            for (auto [kind, primitive] : primitives) {
                if (result.kinds & kind)
                    returnedValue.primitives |= primitive;
            }
            m_program.builtinIntegerResultTypes.append(m_program.types.intern(returnedValue));
        }
    }

private:
    void expandInitialize()
    {
        if (!m_program.usesOperation(Operations::Initialize))
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned functionIndex) {
            static constexpr OpcodeSet operations { Opcode::Operation };
            bool initializesObject = false;
            std::as_const(m_program).functions.forEachIndexedValue(functionIndex, operations, [&](ValueID id) {
                initializesObject |= m_program.operationOf(std::as_const(m_program).functions[functionIndex].at(id)) == Operations::Initialize;
            });
            if (!initializesObject)
                return;
            auto& function = m_program.functions[functionIndex];
            function.allocatesForUnknownCallers = true;
            for (auto& block : function.blocks) {
                for (size_t at = 0; at < block.values.size(); ++at) {
                    if (function.at(block.values[at]).opcode != Opcode::Operation || m_program.operationOf(function.at(block.values[at])) != Operations::Initialize)
                        continue;
                    Value operation = function.at(block.values[at]);
                    RELEASE_ASSERT(operation.arguments.size() % 2);
                    auto& receiver = m_program.typeOf(function, operation.arguments[0]);
                    bool areInitialStores = !Options::auditAOTTypedAssertions();
                    for (size_t i = 1; i < operation.arguments.size(); i += 2) {
                        RELEASE_ASSERT(function.at(operation.arguments[i]).opcode == Opcode::String || function.at(operation.arguments[i]).opcode == Opcode::ConstantSymbol);
                        StringID name = function.at(operation.arguments[i]).string();
                        areInitialStores = areInitialStores && fieldOf(m_program, receiver, name) && !inheritsThroughLayoutPrototype(m_program, receiver.singleLayout(), name);
                    }
                    Vector<ValueID> stores;
                    for (size_t i = 1; i < operation.arguments.size(); i += 2) {
                        Value store;
                        store.opcode = Opcode::PutById;
                        store.type = m_program.types.undefined;
                        store.rep = repOf(m_program.at(store.type));
                        store.position = operation.position;
                        store.children = { operation.arguments[0], operation.arguments[i + 1], ValueID() };
                        store.immediates = { function.at(operation.arguments[i]).string().index(), areInitialStores ? static_cast<uint32_t>(stores.size()) : Format::none };
                        stores.append(ValueID(function.values.size()));
                        function.values.append(WTF::move(store));
                    }
                    auto& unusedValue = function.at(block.values[at]);
                    unusedValue.opcode = Opcode::Undefined;
                    unusedValue.arguments = { };
                    unusedValue.immediates = { Format::none, Format::none };
                    unusedValue.setAssertedType({ });
                    block.values.removeAt(at);
                    block.values.insertVector(at, stores);
                    at += stores.size() - 1;
                }
            }
        });
    }

    void serializeLargeLiterals()
    {
        constexpr uint32_t minLeaves = 4096;
        constexpr uint32_t maxDepth = 64;
        constexpr uint32_t none = UINT32_MAX;
        enum class Kind : uint8_t { Nothing, Leaf, Array, OpenObject, Object };
        struct Node {
            Kind kind { Kind::Nothing };
            bool isTaken { false };
            uint32_t block { none };
            uint32_t leaves { 0 };
            uint32_t depth { 0 };
            uint32_t firstField { none };
            uint32_t lastField { none };
            uint32_t numberOfFields { 0 };
        };
        struct Field {
            ValueID key;
            ValueID value;
            ValueID definition;
            uint32_t next { none };
        };
        Vector<uint8_t> hasLargeBlock(FillWith { }, m_program.functions.size(), static_cast<uint8_t>(0));
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            hasLargeBlock[i] = std::as_const(m_program).functions[i].kind == FunctionKind::Module && std::as_const(m_program).functions[i].blocks.containsIf([&](auto& block) {
                return block.values.size() >= minLeaves;
            });
        });
        for (unsigned functionIndex = 0; functionIndex < m_program.functions.size(); ++functionIndex) {
            if (!hasLargeBlock[functionIndex])
                continue;
            auto& function = m_program.functions[functionIndex];
            unsigned numberOfValuesBefore = function.values.size();
            Vector<uint32_t> uses(FillWith { }, numberOfValuesBefore, 0u);
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                        ++uses[child.index()];
                    });
                }
                block.terminator.forEachChild([&](ValueID& child) {
                    ++uses[child.index()];
                });
            }
            Vector<Node> nodes(numberOfValuesBefore);
            Vector<Field> fields;
            BitVector isInLoop = blocksInLoops(function);
            for (uint32_t b = 0; b < function.blocks.size(); ++b) {
                if (function.blocks[b].values.size() < minLeaves || isInLoop.get(b))
                    continue;
                fields.shrink(0);
                auto isUsable = [&](ValueID id) {
                    auto& node = nodes[id.index()];
                    switch (node.kind) {
                    case Kind::Leaf:
                        return true;
                    case Kind::Array:
                        return node.block == b && !node.isTaken && uses[id.index()] == 1 && node.depth < maxDepth;
                    case Kind::OpenObject:
                    case Kind::Object:
                        return node.block == b && !node.isTaken && uses[id.index()] == node.numberOfFields + 1 && node.depth < maxDepth;
                    case Kind::Nothing:
                        break;
                    }
                    return false;
                };
                auto take = [&](Node& parent, ValueID id) {
                    auto& node = nodes[id.index()];
                    parent.leaves += std::max<uint32_t>(node.leaves, 1);
                    parent.depth = std::max(parent.depth, node.depth + 1);
                    if (node.kind == Kind::Leaf)
                        return;
                    node.isTaken = true;
                    node.kind = node.kind == Kind::OpenObject ? Kind::Object : node.kind;
                };
                auto close = [&](ValueID id) {
                    if (nodes[id.index()].kind == Kind::OpenObject)
                        nodes[id.index()].kind = Kind::Object;
                };
                auto& blockValues = function.blocks[b].values;
                bool hasHandlerAtEnd = function.blocks[b].terminator.kind == TerminatorKind::Invoke;
                for (uint32_t at = 0; at < blockValues.size(); ++at) {
                    ValueID id = blockValues[at];
                    auto& value = std::as_const(function).at(id);
                    auto& node = nodes[id.index()];
                    bool isWatched = hasHandlerAtEnd && at + 1 == blockValues.size();
                    bool isAnything = value.type == m_program.types.anything;
                    bool isPart = false;
                    switch (value.opcode) {
                    case Opcode::Null:
                    case Opcode::Boolean:
                    case Opcode::Integer:
                    case Opcode::String:
                        isPart = true;
                        node = Node { Kind::Leaf, false, b, 1 };
                        break;
                    case Opcode::Double:
                        isPart = std::isfinite(std::bit_cast<double>(value.bits()));
                        if (isPart)
                            node = Node { Kind::Leaf, false, b, 1 };
                        break;
                    case Opcode::NewArray:
                        isPart = !isWatched && isAnything && std::ranges::all_of(value.arguments, isUsable);
                        if (isPart) {
                            node = Node { Kind::Array, false, b };
                            for (ValueID element : value.arguments)
                                take(node, element);
                        }
                        break;
                    case Opcode::NewObject:
                        isPart = !isWatched && isAnything && !value.child(0) && value.arguments.isEmpty() && value.immediates[0] == Format::none && value.immediates[1] == Format::none;
                        if (isPart)
                            node = Node { Kind::OpenObject, false, b };
                        break;
                    case Opcode::Operation: {
                        if (isWatched || m_program.operationOf(value) != Operations::DefineField || value.arguments.size() != 3 || uses[id.index()])
                            break;
                        ValueID object = value.arguments[0];
                        ValueID key = value.arguments[1];
                        ValueID stored = value.arguments[2];
                        auto& parent = nodes[object.index()];
                        if (parent.kind != Kind::OpenObject || parent.block != b || function.at(key).opcode != Opcode::String || stored == object || !isUsable(stored))
                            break;
                        isPart = true;
                        take(parent, stored);
                        uint32_t field = fields.size();
                        fields.append(Field { key, stored, id });
                        if (parent.lastField == none)
                            parent.firstField = field;
                        else
                            fields[parent.lastField].next = field;
                        parent.lastField = field;
                        ++parent.numberOfFields;
                        break;
                    }
                    default:
                        break;
                    }
                    if (!isPart)
                        value.forEachChild(close);
                }
                for (ValueID id : blockValues)
                    close(id);

                BitVector isRemoved(numberOfValuesBefore);
                Vector<uint8_t> text;
                auto appendString = [&](StringID string) {
                    text.append('"');
                    for (uint8_t byte : m_program.string(string)) {
                        if (byte == '"' || byte == '\\') {
                            text.append('\\');
                            text.append(byte);
                        } else if (byte < 0x20) {
                            text.append(asBytes("\\u00"_s.span8()));
                            text.append("0123456789abcdef"[byte >> 4]);
                            text.append("0123456789abcdef"[byte & 15]);
                        } else
                            text.append(byte);
                    }
                    text.append('"');
                };
                auto appendNode = [&](auto& self, ValueID id) -> void {
                    auto& value = std::as_const(function).at(id);
                    switch (nodes[id.index()].kind) {
                    case Kind::Leaf:
                        switch (value.opcode) {
                        case Opcode::Null:
                            text.append(asBytes("null"_s.span8()));
                            break;
                        case Opcode::Boolean:
                            text.append(value.boolean() ? asBytes("true"_s.span8()) : asBytes("false"_s.span8()));
                            break;
                        case Opcode::Integer:
                            text.append(byteCast<uint8_t>(String::number(value.integer()).utf8().span()));
                            break;
                        case Opcode::Double: {
                            double number = std::bit_cast<double>(value.bits());
                            NumberToStringBuffer buffer;
                            if (!number && std::signbit(number))
                                text.append(asBytes("-0"_s.span8()));
                            else
                                text.append(byteCast<uint8_t>(numberToStringAndSize(number, buffer)));
                            break;
                        }
                        default:
                            appendString(value.string());
                            break;
                        }
                        return;
                    case Kind::Array:
                        text.append('[');
                        for (unsigned i = 0; i < value.arguments.size(); ++i) {
                            if (i)
                                text.append(',');
                            ValueID element = value.arguments[i];
                            self(self, element);
                            --uses[element.index()];
                            if (nodes[element.index()].kind != Kind::Leaf)
                                isRemoved.quickSet(element.index());
                        }
                        text.append(']');
                        return;
                    default:
                        text.append('{');
                        for (uint32_t field = nodes[id.index()].firstField; field != none; field = fields[field].next) {
                            if (field != nodes[id.index()].firstField)
                                text.append(',');
                            appendString(function.at(fields[field].key).string());
                            text.append(':');
                            self(self, fields[field].value);
                            --uses[fields[field].key.index()];
                            --uses[fields[field].value.index()];
                            --uses[id.index()];
                            isRemoved.quickSet(fields[field].definition.index());
                            if (nodes[fields[field].value.index()].kind != Kind::Leaf)
                                isRemoved.quickSet(fields[field].value.index());
                        }
                        text.append('}');
                        return;
                    }
                };
                UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rootSource;
                for (uint32_t at = 0; at < function.blocks[b].values.size(); ++at) {
                    ValueID id = function.blocks[b].values[at];
                    auto& node = nodes[id.index()];
                    if ((node.kind != Kind::Array && node.kind != Kind::Object) || node.isTaken || node.leaves < minLeaves || (node.kind == Kind::Array && node.depth <= 1))
                        continue;
                    text.shrink(0);
                    appendNode(appendNode, id);
                    Value constant;
                    constant.opcode = Opcode::String;
                    constant.type = m_program.types.string;
                    constant.rep = repOf(m_program.at(constant.type));
                    constant.position = function.at(id).position;
                    constant.inlineFrame = function.at(id).inlineFrame;
                    constant.immediates = { m_program.intern(text.span()).index(), Format::none };
                    ValueID textID(function.values.size());
                    function.values.append(WTF::move(constant));
                    rootSource.add(id.index(), textID);
                    auto& root = function.at(id);
                    root.opcode = Opcode::Operation;
                    root.children = { };
                    root.immediates = { Operations::ParseJSON.index(), Format::none };
                    root.arguments = Arguments { textID };
                }
                if (rootSource.isEmpty())
                    continue;
                BlockValues kept;
                for (ValueID id : function.blocks[b].values) {
                    if (isRemoved.quickGet(id.index()) || (nodes[id.index()].kind == Kind::Leaf && !uses[id.index()]))
                        continue;
                    if (auto found = rootSource.find(id.index()); found != rootSource.end())
                        kept.append(found->value);
                    kept.append(id);
                }
                function.blocks[b].values = WTF::move(kept);
            }
        }
    }

    void fillLongLiterals()
    {
        constexpr unsigned maxElementsPerBatch = 32;
        constexpr uint32_t nowhere = UINT32_MAX;
        auto isConstant = [](const Value& value) {
            switch (value.opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
                return true;
            default:
                return false;
            }
        };
        Vector<uint8_t> hasLongLiteral(FillWith { }, m_program.functions.size(), static_cast<uint8_t>(0));
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            static constexpr OpcodeSet arrayLiterals { Opcode::NewArray };
            auto& functions = std::as_const(m_program).functions;
            functions.forEachValue(i, arrayLiterals, [&](ValueID id) {
                if (functions[i].at(id).arguments.size() > maxElementsPerBatch)
                    hasLongLiteral[i] = 1;
            });
        });
        for (unsigned functionIndex = 0; functionIndex < m_program.functions.size(); ++functionIndex) {
            if (!hasLongLiteral[functionIndex])
                continue;
            auto& function = m_program.functions[functionIndex];
            Vector<uint32_t> indexInBlock;
            for (auto& block : function.blocks) {
                Vector<uint32_t, 4> literals;
                for (uint32_t at = 0; at < block.values.size(); ++at) {
                    auto& value = function.at(block.values[at]);
                    if (value.opcode != Opcode::NewArray || value.arguments.size() <= maxElementsPerBatch)
                        continue;
                    if (block.terminator.kind == TerminatorKind::Invoke && at + 1 == block.values.size())
                        continue;
                    if (std::ranges::all_of(value.arguments, [&](ValueID element) { return isConstant(function.at(element)) || m_program.typeOf(function, element).isNumber(); }))
                        continue;
                    literals.append(at);
                }
                if (literals.isEmpty())
                    continue;

                unsigned numberOfValuesBefore = function.values.size();
                indexInBlock.fill(nowhere, numberOfValuesBefore);
                for (uint32_t at = 0; at < block.values.size(); ++at)
                    indexInBlock[block.values[at].index()] = at;
                UncheckedKeyHashMap<uint32_t, Vector<ValueID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> createdBefore;
                UncheckedKeyHashMap<uint32_t, Vector<std::pair<ValueID, uint32_t>, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> locations;
                BitVector isMoved(numberOfValuesBefore);
                for (uint32_t at : literals) {
                    ValueID id = block.values[at];
                    auto& elements = function.at(id).arguments;
                    uint32_t first = nowhere;
                    for (ValueID element : elements)
                        first = std::min(first, indexInBlock[element.index()]);
                    if (first == nowhere)
                        continue;
                    RELEASE_ASSERT(first < at);
                    isMoved.set(id.index());
                    indexInBlock[id.index()] = first;
                    createdBefore.add(first, Vector<ValueID, 1>()).iterator->value.append(id);
                    for (uint32_t k = 0; k < elements.size(); ++k)
                        locations.add(elements[k].index(), Vector<std::pair<ValueID, uint32_t>, 1>()).iterator->value.append({ id, k });
                }
                if (createdBefore.isEmpty())
                    continue;

                BlockValues values;
                BitVector isCreated(numberOfValuesBefore);
                for (uint32_t i = 0; i < numberOfValuesBefore; ++i) {
                    if (indexInBlock[i] == nowhere)
                        isCreated.set(i);
                }
                auto add = [&](Value&& value, ValueID like) {
                    value.rep = repOf(m_program.at(value.type));
                    value.position = function.at(like).position;
                    value.inlineFrame = function.at(like).inlineFrame;
                    values.append(ValueID(function.values.size()));
                    function.values.append(WTF::move(value));
                    return values.last();
                };
                auto addInteger = [&](uint32_t number, ValueID like) {
                    Value constant;
                    constant.opcode = Opcode::Integer;
                    constant.type = m_program.types.int32;
                    constant.setBits(number);
                    return add(WTF::move(constant), like);
                };
                auto store = [&](ValueID literal, uint32_t location, ValueID element) {
                    ValueID index = addInteger(location, literal);
                    Value operation;
                    operation.opcode = Opcode::Operation;
                    operation.type = m_program.types.undefined;
                    operation.immediates = { Operations::PutContiguousIndex.index(), Format::none };
                    operation.arguments = Arguments { literal, index, element };
                    add(WTF::move(operation), literal);
                };
                UncheckedKeyHashMap<uint32_t, Vector<std::pair<uint32_t, ValueID>, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> waiting;
                auto flushPendingStores = [&](ValueID literal) {
                    auto found = waiting.find(literal.index());
                    if (found == waiting.end())
                        return;
                    for (auto [location, element] : std::exchange(found->value, { }))
                        store(literal, location, element);
                };
                auto willCreate = [&](ValueID id) {
                    auto found = locations.find(id.index());
                    if (found == locations.end())
                        return;
                    for (auto [literal, location] : found->value) {
                        if (isCreated.get(literal.index()))
                            flushPendingStores(literal);
                    }
                };
                auto didCreate = [&](ValueID id) {
                    isCreated.set(id.index());
                    auto found = locations.find(id.index());
                    if (found == locations.end())
                        return;
                    for (auto [literal, location] : found->value) {
                        if (isCreated.get(literal.index()))
                            waiting.add(literal.index(), Vector<std::pair<uint32_t, ValueID>, 2>()).iterator->value.append({ location, id });
                    }
                };
                auto make = [&](ValueID id) {
                    willCreate(id);
                    Arguments elements = std::exchange(function.at(id).arguments, { });
                    ValueID length = addInteger(elements.size(), id);
                    auto& literal = function.at(id);
                    literal.opcode = Opcode::Operation;
                    literal.immediates = { Operations::NewContiguousArray.index(), Format::none };
                    literal.arguments = Arguments { length };
                    values.append(id);
                    auto& early = waiting.add(id.index(), Vector<std::pair<uint32_t, ValueID>, 2>()).iterator->value;
                    for (uint32_t k = 0; k < elements.size(); ++k) {
                        if (isCreated.get(elements[k].index()))
                            early.append({ k, elements[k] });
                    }
                    didCreate(id);
                };
                for (uint32_t at = 0; at < block.values.size(); ++at) {
                    if (auto found = createdBefore.find(at); found != createdBefore.end()) {
                        for (ValueID id : found->value | std::views::reverse)
                            make(id);
                    }
                    ValueID id = block.values[at];
                    if (isMoved.get(id.index())) {
                        flushPendingStores(id);
                        Value operation;
                        operation.opcode = Opcode::Operation;
                        operation.type = function.at(id).type;
                        operation.immediates = { Operations::TruncateNewArray.index(), Format::none };
                        operation.arguments = Arguments { id, function.at(id).arguments[0] };
                        add(WTF::move(operation), id);
                        continue;
                    }
                    willCreate(id);
                    values.append(id);
                    didCreate(id);
                }
                block.values = WTF::move(values);
            }
        }
    }

    void markFunctionsUsedAsObjects()
    {
        auto prototype = m_program.find(asBytes("prototype"_s.span8()));
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned functionIndex) {
            auto& function = std::as_const(m_program).functions[functionIndex];
            auto note = [&](ValueID id) {
                FunctionID predicted = function.at(id).predictedClosure();
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                auto& definition = function.at(id);
                FunctionID used;
                switch (definition.opcode) {
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                    used = definition.function();
                    break;
                case Opcode::GetModuleVariable:
                    used = m_program.variables[definition.variable().index()].singleFunction;
                    break;
                case Opcode::GetField:
                    used = m_program.at(definition.layout()).fields[definition.field()].singleFunction;
                    break;
                default:
                    break;
                }
                for (FunctionID each : { used, predicted, definition.predictedClosure() }) {
                    if (each)
                        WTF::atomicStore(&m_program.functions.atKeepingValueIndex(each.index()).isUsedAsObject, true, std::memory_order_relaxed);
                }
            };
            bool isCalledByOwnArguments = false;
            static constexpr OpcodeSet usesOfObjects { Opcode::Construct, Opcode::PutById, Opcode::PutByVal, Opcode::DeleteById, Opcode::DeleteByVal, Opcode::InstanceOf, Opcode::GetById, Opcode::Operation };
            std::as_const(m_program).functions.forEachValue(functionIndex, usesOfObjects, [&](ValueID id) {
                auto& value = function.at(id);
                switch (value.opcode) {
                case Opcode::Construct:
                case Opcode::PutById:
                case Opcode::PutByVal:
                case Opcode::DeleteById:
                case Opcode::DeleteByVal:
                    note(value.child(0));
                    break;
                case Opcode::InstanceOf:
                    note(value.child(1));
                    break;
                case Opcode::GetById:
                    if (prototype && value.string() == *prototype)
                        note(value.child(0));
                    break;
                case Opcode::Operation:
                    if (m_program.operationOf(value) == Operations::CreateMappedArguments) {
                        isCalledByOwnArguments = true;
                        note(value.arguments[1]);
                    } else if (m_program.operationOf(value) == Operations::MakeClass)
                        note(value.arguments[1]);
                    break;
                default:
                    break;
                }
            });
            if (isCalledByOwnArguments)
                WTF::atomicStore(&m_program.functions.atKeepingValueIndex(functionIndex).isUsedAsObject, true, std::memory_order_relaxed);
        });
    }

    void linkForInKeys()
    {
        if (!m_program.usesOperation(Operations::ForInKeys) || !m_program.usesOperation(Operations::HasEnumerableProperty))
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned functionIndex) {
            auto& function = std::as_const(m_program).functions[functionIndex];
            auto originOf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            static constexpr OpcodeSet operations { Opcode::Operation };
            std::as_const(m_program).functions.forEachValue(functionIndex, operations, [&](ValueID id) {
                auto& value = function.at(id);
                if (m_program.operationOf(value) != Operations::HasEnumerableProperty || value.arguments.size() != 2)
                    return;
                auto& key = function.at(originOf(value.arguments[1]));
                if (key.opcode != Opcode::GetByVal)
                    return;
                ValueID keys = originOf(key.child(0));
                auto& listing = function.at(keys);
                if (listing.opcode == Opcode::Operation && m_program.operationOf(listing) == Operations::ForInKeys && listing.arguments[0] == value.arguments[0])
                    m_program.functions.atKeepingValueIndex(functionIndex).at(id).arguments.append(keys);
            });
        });
    }

    Program& m_program;
};

static void preallocateConstructorFields(Program& program)
{
    BitVector isExtended;
    for (auto& layout : program.layouts) {
        if (layout.base)
            isExtended.set(layout.base.index());
    }
    unsigned numberOfFields = 0;
    unsigned numberOfClasses = 0;
    for (unsigned number = 0; number < program.layouts.size(); ++number) {
        auto& layout = program.layouts[number];
        if (layout.kind != LayoutKind::Instance || layout.base || isExtended.get(number) || !layout.hasFixedLayout || layout.acceptsUntypedObjects || layout.mayDespecialize || !layout.constructor)
            continue;
        auto& function = std::as_const(program).at(layout.constructor);
        if (function.kind != FunctionKind::Constructor || !function.has(Format::FunctionHasThis) || function.isGenericEntrypoint() || function.blocks.isEmpty() || function.blocks[0].parameters.size() <= function.captureTypes.size())
            continue;
        if (std::ranges::any_of(function.blocks, [](auto& block) { return block.terminator.kind == TerminatorKind::Invoke; }))
            continue;
        ValueID self = function.blocks[0].parameters[function.captureTypes.size()];
        auto isSelf = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                id = function.at(id).child(0);
            return id == self;
        };
        uint32_t last = 0;
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            if (!(layout.fields[index].flags & (fieldIsAbsentAtCreation | fieldIsNeverStored)))
                last = index + 1;
        }
        BitVector holdsSelf;
        Vector<uint32_t, 16> initializedFields;
        for (ValueID id : function.blocks[0].values) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && isSelf(id))
                continue;
            if (value.opcode == Opcode::NewClosure) {
                if (std::ranges::any_of(value.arguments, isSelf))
                    holdsSelf.set(id.index());
                continue;
            }
            if (value.opcode == Opcode::PutById && isSelf(value.child(0)) && !isSelf(value.child(1))) {
                auto named = layout.fieldNamed(value.string());
                if (!named)
                    break;
                auto& field = layout.fields[*named];
                if (!(field.flags & fieldIsAbsentAtCreation) || initializedFields.contains(*named))
                    continue;
                if (*named < last || program.prototypePropertyNames.get(value.string().index()) || !field.type || !(program.at(field.type).primitives & Primitive::Undefined))
                    break;
                last = *named + 1;
                initializedFields.append(*named);
                continue;
            }
            bool mayEscapeSelf = false;
            value.forEachChild([&](ValueID child) {
                mayEscapeSelf |= child && (isSelf(child) || holdsSelf.get(child.index()));
            });
            if (mayEscapeSelf)
                break;
        }
        for (uint32_t index : initializedFields)
            layout.fields[index].flags &= ~fieldIsAbsentAtCreation;
        numberOfFields += initializedFields.size();
        numberOfClasses += !initializedFields.isEmpty();
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfFields, " fields of ", numberOfClasses, " classes are preallocated because the constructor stores to them before the object escapes");
}

static void findLazyClosureNames(Program& program)
{
    if (program.mayRunInMultipleInstances)
        return;
    Vector<Vector<uint32_t>> found(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned number) {
        auto& function = std::as_const(program).functions[number];
        auto withoutUpcasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                id = function.at(id).child(0);
            return id;
        };
        ValueID self;
        if (function.kind == FunctionKind::Constructor && function.has(Format::FunctionHasThis) && !function.blocks.isEmpty() && function.blocks[0].parameters.size() > function.captureTypes.size())
            self = function.blocks[0].parameters[function.captureTypes.size()];
        static constexpr OpcodeSet closures { Opcode::NewClosure };
        if (!std::as_const(program).functions.mayContain(number, closures))
            return;
        static constexpr OpcodeSet stores { Opcode::PutById, Opcode::PutField };
        std::as_const(program).functions.forEachValue(number, stores, [&](ValueID id) {
            auto& value = function.at(id);
            if (!closures.contains(function.at(value.child(1)).opcode) || function.at(value.child(1)).arguments.size() > 1)
                return;
            ValueID holder = withoutUpcasts(value.child(0));
            if (holder != self && function.at(holder).opcode != Opcode::NewObjectWithShape)
                return;
            if (auto& captured = function.at(value.child(1)).arguments; !captured.isEmpty() && withoutUpcasts(captured[0]) != holder)
                return;
            found[number].append((value.opcode == Opcode::PutById ? value.string() : program.at(value.layout()).fields[value.field()].name).index());
        });
    });
    for (auto& names : found) {
        for (uint32_t name : names)
            program.lazyClosureNames.set(name);
    }
}

void prepare(Program& program)
{
    Preparation(program).run();
    findLazyClosureNames(program);
}

static LayoutID predictedCloneLayout(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin || !value.prediction())
        return { };
    auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Operation || (implementation.operation != LanguageOperation::CloneObject && implementation.operation != LanguageOperation::CloneObjectWith && implementation.operation != LanguageOperation::CloneObjectAfter && implementation.operation != LanguageOperation::CopyDataPropertiesExcluding))
        return { };
    LayoutID supertype = program.at(value.prediction()).singleLayout();
    return supertype && program.at(supertype).kind == LayoutKind::Record ? supertype : LayoutID();
}

static bool castsFreshObjectToRecord(const Function& function, const Value& value)
{
    return value.opcode == Opcode::Cast && value.castReason() == CastReason::As && function.at(value.child(0)).opcode == Opcode::NewObject;
}

void linkAllocations(Program& program)
{
    UncheckedKeyHashMap<unsigned, Vector<uint32_t, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> byHash;
    auto hashOf = [](const Allocation& allocation) {
        Hasher hasher;
        add(hasher, allocation.layout, allocation.constructedBy, allocation.initialFieldCount, allocation.numberOfCaptures);
        for (uint16_t field : allocation.order)
            add(hasher, field);
        return hasher.hash();
    };
    auto numberWithHash = [&](const Allocation& allocation, unsigned hash) -> uint32_t {
        auto& candidates = byHash.add(hash, Vector<uint32_t, 1>()).iterator->value;
        for (uint32_t candidate : candidates) {
            if (program.allocations[candidate] == allocation)
                return candidate;
        }
        candidates.append(program.allocations.size());
        program.allocations.append(allocation);
        return candidates.last();
    };
    auto numberOf = [&](const Allocation& allocation) {
        return numberWithHash(allocation, hashOf(allocation));
    };
    auto ensureClosureAllocation = [&](unsigned numberOfCaptures) {
        RELEASE_ASSERT(numberOfCaptures <= UINT8_MAX);
        unsigned index = numberOfCaptures / 2;
        while (program.closureAllocations.size() <= index)
            program.closureAllocations.append(Format::none);
        if (program.closureAllocations[index] != Format::none)
            return;
        Allocation allocation { program.closureLayout, { }, { }, 0 };
        allocation.numberOfCaptures = static_cast<uint16_t>(2 * index + 1);
        program.closureAllocations[index] = numberOf(allocation);
    };
    ensureClosureAllocation(0);
    struct AllocationSite {
        enum Kind : uint8_t { Closure, Object, Clone, Cast };
        uint32_t function;
        ValueID value;
        Kind kind;
        uint32_t operand;
        unsigned hash { 0 };
    };
    auto allocationSites = collectInParallel<AllocationSite>(program.functions.size(), [&](unsigned i, Vector<AllocationSite>& collected) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet mayBeAllocations { Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::NewObjectWithShape, Opcode::CallBuiltin, Opcode::Cast };
        std::as_const(program).functions.forEachIndexedValue(i, mayBeAllocations, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                collected.append({ i, id, AllocationSite::Closure, static_cast<uint32_t>(value.arguments.size()) });
            else if (value.opcode == Opcode::NewObjectWithShape)
                collected.append({ i, id, AllocationSite::Object, value.allocation(), hashOf(function.allocations[value.allocation()]) });
            else if (LayoutID supertype = predictedCloneLayout(program, value))
                collected.append({ i, id, AllocationSite::Clone, supertype.index() });
            else if (castsFreshObjectToRecord(function, value))
                collected.append({ i, id, AllocationSite::Cast, value.type.index() });
        });
    });
    for (auto& site : allocationSites) {
        if (site.kind == AllocationSite::Closure)
            ensureClosureAllocation(site.operand);
        if (site.kind != AllocationSite::Object)
            continue;
        auto& allocation = std::as_const(program).functions[site.function].allocations[site.operand];
        program.at(allocation.layout).isAllocated = true;
        site.operand = numberWithHash(allocation, site.hash);
    }
    forEachIndexInParallel(allocationSites.size(), [&](unsigned k) {
        auto& site = allocationSites[k];
        if (site.kind == AllocationSite::Object)
            program.functions.atKeepingValueIndex(site.function).at(site.value).immediates[1] = site.operand;
    });
    for (auto& site : allocationSites) {
        if (site.kind == AllocationSite::Clone && program.at(LayoutID(site.operand)).isAllocated)
            numberOf(Allocation { LayoutID(site.operand), { }, { }, 0 });
        if (site.kind != AllocationSite::Cast)
            continue;
        for (LayoutID supertype : program.at(TypeID(site.operand)).objectLayouts) {
            if (program.at(supertype).kind == LayoutKind::Record && program.at(supertype).isAllocated)
                numberOf(Allocation { supertype, { }, { }, 0 });
        }
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        program.functions.atKeepingValueIndex(i).allocations.clear();
    });
    auto functionsAndThisLayouts = collectInParallel<std::pair<uint32_t, LayoutID>>(program.functions.size(), [&](unsigned i, auto& collected) {
        auto& function = std::as_const(program).functions[i];
        TypeID thisType = function.assertedThisType ? function.assertedThisType : function.thisType;
        if (function.kind != FunctionKind::Plain || function.declaredAs & (functionIsAsync | functionIsGenerator) || !thisType || !program.genericEntrypoints.contains(i) || !function.allocatesForUnknownCallers)
            return;
        LayoutID supertype = program.at(thisType).singleLayout();
        if (supertype && std::as_const(program).at(supertype).kind == LayoutKind::Record && std::as_const(program).at(supertype).isAllocated)
            collected.append({ i, supertype });
    });
    for (auto [i, supertype] : functionsAndThisLayouts)
        program.functions.atKeepingValueIndex(i).thisAllocation = numberOf(Allocation { supertype, { }, { }, 0 });
    for (uint32_t i = 0; i < program.allocations.size(); ++i) {
        auto& allocation = program.allocations[i];
        auto& layout = program.at(allocation.layout);
        if (layout.kind == LayoutKind::Record && !allocation.constructedBy && allocation.discriminant == UINT16_MAX && layout.cloneAllocation == Format::none)
            layout.cloneAllocation = i;
    }
}

static Vector<unsigned> resumeBlocks(const Function& function)
{
    Vector<unsigned> result;
    if (function.suspensionPositions.isEmpty() || function.blocks.isEmpty())
        return result;
    auto& dispatch = function.blocks[0].terminator;
    if (dispatch.kind != TerminatorKind::Switch || function.blocks[0].parameters.isEmpty())
        return result;
    ValueID frame = function.blocks[0].parameters[0];
    auto isFrameStateAccess = [&](const Value& value) {
        return value.child(0) == frame && value.field() == CoroutineFrame::stateField;
    };
    if (function.at(dispatch.value).opcode != Opcode::GetField || !isFrameStateAccess(function.at(dispatch.value)))
        return result;
    result.fill(UINT_MAX, function.blocks.size());
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        for (ValueID id : function.blocks[b].values) {
            auto& store = function.at(id);
            if (store.opcode != Opcode::PutField || !isFrameStateAccess(store) || function.at(store.child(1)).opcode != Opcode::Integer)
                continue;
            size_t index = dispatch.cases.find(function.at(store.child(1)).integer());
            if (index != notFound)
                result[b] = dispatch.targets[index].block.index();
        }
    }
    return result;
}

Vector<unsigned> computeBlockCycles(const Function& function, Vector<unsigned>* componentRank)
{
    constexpr unsigned notVisited = UINT_MAX;
    unsigned numberOfBlocks = function.blocks.size();
    Vector<unsigned> result;
    result.fill(notInCycle, numberOfBlocks);
    unsigned numberOfComponents = 0;
    if (componentRank)
        componentRank->fill(0, numberOfBlocks);
    BitVector hasSelfEdge(numberOfBlocks);
    BitVector isInComponentStack(numberOfBlocks);
    Vector<unsigned> order;
    order.fill(notVisited, numberOfBlocks);
    Vector<unsigned> lowest = order;
    Vector<unsigned> componentStack;
    Vector<std::pair<unsigned, unsigned>> walk;
    unsigned numberVisited = 0;
    for (unsigned root = 0; root < numberOfBlocks; ++root) {
        if (order[root] != notVisited)
            continue;
        walk.append({ root, 0 });
        while (!walk.isEmpty()) {
            auto [block, nextTarget] = walk.last();
            if (!nextTarget) {
                order[block] = lowest[block] = numberVisited++;
                componentStack.append(block);
                isInComponentStack.set(block);
            }
            auto& targets = function.blocks[block].terminator.targets;
            if (nextTarget < targets.size()) {
                ++walk.last().second;
                unsigned target = targets[nextTarget].block.index();
                if (target == block)
                    hasSelfEdge.set(block);
                if (order[target] == notVisited)
                    walk.append({ target, 0 });
                else if (isInComponentStack.get(target))
                    lowest[block] = std::min(lowest[block], order[target]);
                continue;
            }
            walk.removeLast();
            if (!walk.isEmpty())
                lowest[walk.last().first] = std::min(lowest[walk.last().first], lowest[block]);
            if (lowest[block] != order[block])
                continue;
            bool isCycle = componentStack.last() != block || hasSelfEdge.get(block);
            for (unsigned member = notVisited; member != block;) {
                member = componentStack.takeLast();
                isInComponentStack.clear(member);
                if (isCycle)
                    result[member] = block;
                if (componentRank)
                    componentRank->at(member) = numberOfComponents;
            }
            ++numberOfComponents;
        }
    }
    return result;
}

Vector<unsigned> computeBlockCycles(const Function& function)
{
    return computeBlockCycles(function, nullptr);
}

static constexpr OpcodeSet loopHints { Opcode::LoopHint };

BitVector blocksInLoops(const Functions& functions, size_t index)
{
    if (!functions.mayContain(index, loopHints))
        return { };
    return blocksInLoops(functions[index]);
}

BitVector blocksInLoops(const Function& function)
{
    unsigned numberOfBlocks = function.blocks.size();
    BitVector result(numberOfBlocks);
    Vector<unsigned, 8> headers;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        bool isHeader = function.blocks[b].values.containsIf([&](ValueID id) {
            return loopHints.contains(function.at(id).opcode);
        });
        if (isHeader)
            headers.append(b);
    }
    if (headers.isEmpty())
        return result;
    Vector<unsigned> cycles = computeBlockCycles(function);
    BitVector containsHeader(numberOfBlocks);
    for (unsigned header : headers) {
        if (cycles[header] != notInCycle)
            containsHeader.set(cycles[header]);
    }
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        if (cycles[b] != notInCycle && containsHeader.get(cycles[b]))
            result.set(b);
    }
    return result;
}

static void sinkFunctionReferences(Function& function)
{
    if (function.blocks.size() < 2)
        return;
    constexpr unsigned none = UINT_MAX;
    Vector<unsigned> definingBlock;
    definingBlock.fill(none, function.values.size());
    bool hasAny = false;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        for (ValueID id : function.blocks[b].values) {
            if (function.at(id).opcode != Opcode::FunctionReference)
                continue;
            definingBlock[id.index()] = b;
            hasAny = true;
        }
    }
    if (!hasAny)
        return;
    BitVector isInLoop = blocksInLoops(function);
    BitVector stays(function.values.size());
    UncheckedKeyHashMap<uint32_t, Vector<unsigned, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> userBlocks;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        for (ValueID id : block.values) {
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                if (definingBlock[child.index()] == none)
                    return;
                auto& blocks = userBlocks.add(child.index(), Vector<unsigned, 2>()).iterator->value;
                if (!blocks.contains(b))
                    blocks.append(b);
                if (definingBlock[child.index()] == b || isInLoop.get(b))
                    stays.set(child.index());
            });
        }
        block.terminator.forEachChild([&](ValueID& child) {
            if (definingBlock[child.index()] != none)
                stays.set(child.index());
        });
    }
    Vector<unsigned> componentRank;
    Vector<uint32_t> isReachedFor;
    for (auto& [reference, blocks] : userBlocks) {
        if (blocks.size() < 2 || stays.get(reference))
            continue;
        if (componentRank.isEmpty()) {
            computeBlockCycles(function, &componentRank);
            isReachedFor.fill(none, function.blocks.size());
        }
        unsigned minRank = UINT_MAX;
        Vector<unsigned, 16> worklist;
        for (unsigned b : blocks) {
            minRank = std::min(minRank, componentRank[b]);
            worklist.append(b);
        }
        while (!worklist.isEmpty()) {
            for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned to = target.block.index();
                if (componentRank[to] < minRank || isReachedFor[to] == reference)
                    continue;
                isReachedFor[to] = reference;
                worklist.append(to);
            }
        }
        if (blocks.containsIf([&](unsigned b) { return isReachedFor[b] == reference; }))
            stays.set(reference);
    }
    unsigned numberOfValues = function.values.size();
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> copies;
        BlockValues values;
        size_t allocationSite = notFound;
        for (ValueID id : function.blocks[b].values) {
            if (id.index() < numberOfValues && definingBlock[id.index()] == b && userBlocks.contains(id.index()) && !stays.get(id.index()))
                continue;
            Vector<ValueID, 2> wanted;
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                if (child.index() < numberOfValues && definingBlock[child.index()] != none && !stays.get(child.index()) && !copies.contains(child.index()) && !wanted.contains(child))
                    wanted.append(child);
            });
            Opcode opcode = function.at(id).opcode;
            bool storesToNewObject = allocationSite != notFound && (opcode == Opcode::PutField || opcode == Opcode::PutById) && function.at(id).child(0) == values[allocationSite] && function.at(id).child(1) != values[allocationSite];
            for (ValueID original : wanted) {
                ValueID copy(function.values.size());
                function.values.append(Value(function.at(original)));
                copies.add(original.index(), copy);
                if (storesToNewObject)
                    values.insert(allocationSite++, copy);
                else
                    values.append(copy);
            }
            function.at(id).forEachChild([&](ValueID& child) {
                if (auto copy = copies.get(child.index()))
                    child = copy;
            });
            values.append(id);
            if (opcode == Opcode::NewObjectWithShape)
                allocationSite = values.size() - 1;
            else if (!storesToNewObject)
                allocationSite = notFound;
        }
        function.blocks[b].values = WTF::move(values);
    }
}

static void findFunctionsInCycles(Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<unsigned>> callees(count);
    Vector<unsigned> index;
    index.fill(0, count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        std::as_const(program).functions.forEachValue(i, directCalls, [&](ValueID id) {
            callees[i].append(std::as_const(program).functions[i].at(id).function().index());
        });
        if (callees[i].isEmpty())
            index[i] = 1;
    });
    struct Frame {
        unsigned function;
        unsigned next;
    };
    BitVector result(count);
    BitVector isOnStack(count);
    Vector<unsigned> low;
    low.fill(0, count);
    Vector<unsigned> stack;
    Vector<Frame> frames;
    unsigned counter = 0;
    auto enter = [&](unsigned function) {
        index[function] = low[function] = ++counter;
        stack.append(function);
        isOnStack.set(function);
        frames.append(Frame { function, 0 });
    };
    for (unsigned root = 0; root < count; ++root) {
        if (index[root])
            continue;
        enter(root);
        while (!frames.isEmpty()) {
            unsigned function = frames.last().function;
            if (frames.last().next < callees[function].size()) {
                unsigned callee = callees[function][frames.last().next++];
                if (callee == function)
                    result.set(function);
                if (!index[callee])
                    enter(callee);
                else if (isOnStack.get(callee))
                    low[function] = std::min(low[function], index[callee]);
                continue;
            }
            frames.removeLast();
            if (!frames.isEmpty())
                low[frames.last().function] = std::min(low[frames.last().function], low[function]);
            if (low[function] != index[function])
                continue;
            bool isSingleton = stack.last() == function;
            for (;;) {
                unsigned member = stack.takeLast();
                isOnStack.clear(member);
                if (!isSingleton)
                    result.set(member);
                if (member == function)
                    break;
            }
        }
    }
    for (auto [function, entry] : program.genericEntrypoints) {
        if (result.get(function))
            result.set(entry.index());
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", result.bitCount(), " of ", count, " functions are in a cycle of calls by name");
    program.functionsInCycles = WTF::move(result);
    destroyOnHelperThread(WTF::move(callees));
}

static void findStaticallyHotFunctions(Program& program)
{
    findFunctionsInCycles(program);
    unsigned count = program.functions.size();
    auto forEachReferencedFunction = [&](const Function& function, const BitVector* blocks, bool includesCreated, auto&& functor) {
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (blocks && !blocks->get(b))
                continue;
            for (ValueID id : function.blocks[b].values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::CallDirect || (includesCreated && (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::FunctionReference)))
                    functor(function.at(id).function().index());
            }
        }
    };
    BitVector result(count);
    BitVector isCalled(count);
    Vector<unsigned> frontier;
    Vector<std::optional<Vector<unsigned>>> calledInLoops(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        BitVector isInLoop = blocksInLoops(std::as_const(program).functions, i);
        bool isInCycle = program.functionsInCycles.get(i);
        if (isInLoop.isEmpty() && !isInCycle)
            return;
        calledInLoops[i] = Vector<unsigned>();
        forEachReferencedFunction(std::as_const(program).functions[i], isInCycle ? nullptr : &isInLoop, false, [&](unsigned callee) {
            calledInLoops[i]->append(callee);
        });
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!calledInLoops[i])
            continue;
        result.set(i);
        for (unsigned callee : *calledInLoops[i]) {
            if (!isCalled.set(callee))
                frontier.append(callee);
        }
    }
    static constexpr ASCIILiteral perRequestRuntimeModules[] = {
        "node:events"_s, "node:_http_common"_s, "node:_http_incoming"_s, "node:_http_outgoing"_s, "node:_http_server"_s, "internal:http"_s,
        "internal:streams/readable"_s, "internal:streams/state"_s, "internal:streams/writable"_s, "internal:process/next_tick_queue"_s, "internal:validators"_s,
    };
    BitVector runsPerRequest(program.modules.size());
    for (auto& [number, code] : program.builtinModules) {
        ModuleID module = code ? std::as_const(program).at(code).module : ModuleID();
        if (!module)
            continue;
        auto path = program.string(program.modules[module.index()].path);
        if (std::ranges::any_of(perRequestRuntimeModules, [&](ASCIILiteral name) { return equalSpans(path, name.span8()); }))
            runsPerRequest.set(module.index());
    }
    program.modulesRunPerRequest = runsPerRequest;
    if (!runsPerRequest.isEmpty()) {
        BitVector isModuleBody(count);
        for (auto& [number, code] : program.builtinModules) {
            if (code)
                isModuleBody.set(code.index());
        }
        auto functionsRunPerRequest = findIndicesInParallel(count, [&](unsigned i) {
            auto& function = std::as_const(program).functions[i];
            return function.module && runsPerRequest.get(function.module.index()) && !function.isGenericEntrypoint() && !isModuleBody.get(i);
        });
        for (unsigned i : functionsRunPerRequest) {
            if (!isCalled.set(i))
                frontier.append(i);
        }
    }
    BitVector hasLoop = result;
    isCalled.merge(program.functionsInCycles);
    auto functionsSavingFrames = findIndicesInParallel(count, [&](unsigned i) {
        return std::as_const(program).functions[i].savesFrameOnSuspend;
    });
    for (unsigned i : functionsSavingFrames)
        isCalled.clear(i);
    BitVector isCalledOrCreated = isCalled;
    auto say = [](Vector<uint8_t>& said, unsigned index) {
        WTF::atomicStore(&said[index], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    auto forEachFlagged = [&](const Vector<uint8_t>& said, auto&& functor) {
        for (unsigned i = 0; i < count; ++i) {
            if (said[i])
                functor(i);
        }
    };
    Vector<uint8_t> isReferencedByFrontier(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> isCalledByFrontier(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(frontier.size(), [&](unsigned k) {
        auto& function = std::as_const(program).functions[frontier[k]];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::CallDirect)
                    say(isCalledByFrontier, function.at(id).function().index());
                else if (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::FunctionReference)
                    say(isReferencedByFrontier, function.at(id).function().index());
            }
        }
    });
    forEachFlagged(isReferencedByFrontier, [&](unsigned callee) {
        isCalledOrCreated.set(callee);
    });
    forEachFlagged(isCalledByFrontier, [&](unsigned callee) {
        isCalledOrCreated.set(callee);
    });
    unsigned forced = Options::aotHotByTextForTesting();
    if (forced == 1) [[unlikely]] {
        isCalled.clearAll();
        isCalledOrCreated.clearAll();
    }
    if (forced == 2) [[unlikely]] {
        for (unsigned i = 0; i < count; ++i) {
            isCalled.set(i);
            isCalledOrCreated.set(i);
        }
    }
    BitVector warm = result;
    warm.merge(isCalledOrCreated);
    result.merge(isCalled);
    BitVector created(count);
    Vector<uint8_t> isCreatedByWarmFunction(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!warm.get(i))
            return;
        static constexpr OpcodeSet functionValues { Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::FunctionReference };
        std::as_const(program).functions.forEachValue(i, functionValues, [&](ValueID id) {
            say(isCreatedByWarmFunction, std::as_const(program).functions[i].at(id).function().index());
        });
    });
    forEachFlagged(isCreatedByWarmFunction, [&](unsigned callee) {
        created.set(callee);
    });
    if (forced == 1) [[unlikely]]
        created.clearAll();
    warm.merge(created);
    hasLoop.exclude(isCalled);
    program.functionsHotOnlyInLoops = WTF::move(hasLoop);
    Vector<uint8_t> isCalledUnderHiddenHandler(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayEndWith(i, TerminatorKind::Invoke))
            return;
        auto& function = std::as_const(program).functions[i];
        if (!function.has(Format::FunctionIsHiddenInStackTraces) || function.isGenericEntrypoint())
            return;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Invoke)
                continue;
            for (ValueID id : function.at(block.terminator.targets[1].block).values) {
                if (function.at(id).opcode == Opcode::CallDirect)
                    say(isCalledUnderHiddenHandler, function.at(id).function().index());
            }
        }
    });
    forEachFlagged(isCalledUnderHiddenHandler, [&](unsigned callee) {
        result.clear(callee);
        warm.clear(callee);
    });
    for (auto [function, entry] : program.genericEntrypoints) {
        if (result.get(function))
            result.set(entry.index());
        if (warm.get(function))
            warm.set(entry.index());
    }
    program.staticallyHotFunctions = WTF::move(result);
    program.staticallyWarmFunctions = WTF::move(warm);
}

static void findStaticallyRareFunctions(Program& program)
{
    unsigned count = program.functions.size();
    Vector<unsigned> usedAsValueCount;
    usedAsValueCount.fill(0, count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (program.functions.mayContainPredictedClosure(i)) {
            for (auto& value : function.values) {
                if (value.predictedClosure())
                    WTF::atomicExchangeAdd(&usedAsValueCount[value.predictedClosure().index()], 1u, std::memory_order_relaxed);
            }
        }
        static constexpr OpcodeSet functionValues { Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce };
        std::as_const(program).functions.forEachValue(i, functionValues, [&](ValueID id) {
            WTF::atomicExchangeAdd(&usedAsValueCount[function.at(id).function().index()], 1u, std::memory_order_relaxed);
        });
    });
    BitVector isUsedAsValue(count);
    for (unsigned i = 0; i < count; ++i) {
        if (usedAsValueCount[i])
            isUsedAsValue.set(i);
    }
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isUsedAsValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isUsedAsValue.set(method.function.index());
        }
    }
    for (auto& module : program.builtinModules)
        isUsedAsValue.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isUsedAsValue.set(builtin.function.index());
    for (auto [index, entry] : program.genericEntrypoints) {
        isUsedAsValue.set(index);
        isUsedAsValue.set(entry.index());
    }
    auto mayBeRare = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        return isPlain && !isUsedAsValue.get(i) && !function.isGenericEntrypoint() && !function.has(Format::FunctionIsBuiltin) && !program.isResumeFunction(function) && !program.isEntry(FunctionID(i)) && !function.resumeFunction && !function.startFunction;
    };
    Vector<uint32_t> callsInRareCode;
    callsInRareCode.fill(0, count);
    Vector<unsigned> worklist;
    auto functionsRunOnce = findIndicesInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        return (function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i));
    });
    for (unsigned i : functionsRunOnce) {
        program.staticallyRareFunctions.set(i);
        worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        Vector<Vector<unsigned>> calledOutsideLoops(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            auto& function = std::as_const(program).functions[worklist[k]];
            BitVector isInLoop = blocksInLoops(function);
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (isInLoop.get(b))
                    continue;
                for (ValueID id : function.blocks[b].values) {
                    if (function.at(id).opcode != Opcode::CallDirect)
                        continue;
                    unsigned callee = function.at(id).function().index();
                    if (mayBeRare(callee))
                        calledOutsideLoops[k].append(callee);
                }
            }
        });
        worklist.shrink(0);
        for (auto& callees : calledOutsideLoops) {
            for (unsigned callee : callees) {
                if (++callsInRareCode[callee] == program.numberOfDirectCalls[callee] && !program.staticallyRareFunctions.set(callee))
                    worklist.append(callee);
            }
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.staticallyRareFunctions.bitCount(), " of ", count, " functions are statically rare");
}

static bool isInlinedFromHotFunction(const Program& program, const Function& function, const Value& value)
{
    if (!value.inlineFrame)
        return false;
    FunctionID original = function.inlineFrames[value.inlineFrame].callee;
    return program.isHotOutsideLoops(original);
}

static BitVector findValuesOpenedAsIterables(const Function& function)
{
    BitVector result(function.values.size());
    Vector<ValueID, 8> worklist;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::IteratorOpen)
                worklist.append(function.at(id).child(0));
        }
    }
    while (!worklist.isEmpty()) {
        ValueID id = worklist.takeLast();
        if (result.set(id.index()))
            continue;
        auto& value = function.at(id);
        if (value.opcode == Opcode::Cast)
            worklist.append(value.child(0));
        if (value.opcode != Opcode::Parameter)
            continue;
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (parameters[i] == id)
                        worklist.append(target.arguments[i]);
                }
            }
        }
    }
    return result;
}

static void printOutliningStatistics(ASCIILiteral what, size_t bodies, std::initializer_list<std::pair<ASCIILiteral, uint64_t>> rows)
{
    uint64_t all = 0;
    for (auto& row : rows)
        all += row.second;
    StringPrintStream out;
    out.print("AOT: one body: ", what, ": ", all, " sites, ", bodies, " bodies");
    for (auto& [reason, number] : rows)
        out.print(" | ", reason, " ", number);
    dataLogLn(out.toString());
}

static bool consistsOfNumbersAndConstants(const Program& program, const Function& function, ValueID id, unsigned& remainingValueBudget)
{
    if (!remainingValueBudget)
        return false;
    --remainingValueBudget;
    auto& value = function.at(id);
    switch (value.opcode) {
    case Opcode::String:
        return true;
    case Opcode::ToString:
        return consistsOfNumbersAndConstants(program, function, value.child(0), remainingValueBudget);
    case Opcode::Add:
        if (program.at(value.type).isString())
            return consistsOfNumbersAndConstants(program, function, value.child(0), remainingValueBudget) && consistsOfNumbersAndConstants(program, function, value.child(1), remainingValueBudget);
        break;
    case Opcode::Strcat:
        return std::ranges::all_of(value.arguments, [&](ValueID part) {
            return consistsOfNumbersAndConstants(program, function, part, remainingValueBudget);
        });
    default:
        break;
    }
    auto& type = program.typeOf(function, id);
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
}

static bool consistsOfNumbersAndConstants(const Program& program, const Function& function, ValueID id)
{
    unsigned remainingValueBudget = 64;
    return consistsOfNumbersAndConstants(program, function, id, remainingValueBudget);
}

static void outlineConcatenations(Program& program)
{
    constexpr unsigned maxOperands = 6;
    unsigned count = program.functions.size();
    struct Site {
        ValueID value;
        String signature;
        BitVector isStringCreatedInBody;
        String stringSignature;
        uint32_t body { 0 };
    };
    Vector<Vector<Site>> sites(count);
    static constexpr OpcodeSet concatenations { Opcode::Strcat };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayContain(i, concatenations))
            return;
        auto& function = std::as_const(program).functions[i];
        if (program.isHotOutsideLoops(FunctionID(i)) || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        std::optional<BitVector> keys;
        std::optional<StringBuilderAnalysis> builders;
        Vector<uint32_t> numberOfUses;
        auto isUsedOnce = [&](ValueID id) {
            if (numberOfUses.isEmpty()) {
                numberOfUses.fill(0, function.values.size());
                for (auto& block : function.blocks) {
                    for (ValueID user : block.values) {
                        function.at(user).forEachChild([&](ValueID child) {
                            ++numberOfUses[child.index()];
                        });
                    }
                    if (block.terminator.value)
                        ++numberOfUses[block.terminator.value.index()];
                    for (auto& target : block.terminator.targets) {
                        for (ValueID argument : target.arguments)
                            ++numberOfUses[argument.index()];
                    }
                }
            }
            return numberOfUses[id.index()] == 1;
        };
        Vector<uint32_t> blockOf;
        auto isListedIn = [&](unsigned b, ValueID id) {
            if (blockOf.isEmpty()) {
                blockOf.fill(UINT32_MAX, function.values.size());
                for (unsigned block = 0; block < function.blocks.size(); ++block) {
                    for (ValueID listed : function.blocks[block].values)
                        blockOf[listed.index()] = block;
                }
            }
            return blockOf[id.index()] == b;
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::Strcat || value.arguments.size() > maxOperands)
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                if (isInlinedFromHotFunction(program, function, value))
                    continue;
                if (!builders)
                    builders = analyzeStringBuilders(program, FunctionID(i), function);
                if (builders->isState(id))
                    continue;
                if (consistsOfNumbersAndConstants(program, function, id)) {
                    if (!keys)
                        keys = computeMapKeyValues(program, FunctionID(i), function);
                    if (keys->get(id.index()))
                        continue;
                }
                StringBuilder signature;
                StringBuilder stringSignature;
                signature.append(value.type.index());
                stringSignature.append(value.type.index());
                BitVector isStringCreatedInBody;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    auto& argument = function.at(value.arguments[k]);
                    stringSignature.append(' ', argument.type.index());
                    if (argument.opcode == Opcode::ToString && isListedIn(b, value.arguments[k]) && isUsedOnce(value.arguments[k])) {
                        auto& given = function.at(argument.child(0));
                        if (auto& type = program.at(given.type); !type.hasObjects() && !(type.primitives & Primitive::Symbol)) {
                            isStringCreatedInBody.set(k);
                            signature.append(" s"_s, given.type.index());
                            continue;
                        }
                    }
                    signature.append(' ', argument.type.index());
                }
                sites[i].append({ id, signature.toString(), WTF::move(isStringCreatedInBody), stringSignature.toString() });
            }
        }
    });
    enum : unsigned { IsOutlined, IsStaticallyHotReason, HasTooManyParts, IsInLoop, IsInlinedFromHot, IsKey, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        bool isHot = program.isHotOutsideLoops(FunctionID(i));
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Strcat)
                    ++numbers[isHot ? IsStaticallyHotReason : value.arguments.size() > maxOperands ? HasTooManyParts : isInLoop.get(b) ? IsInLoop : isInlinedFromHotFunction(program, function, value) ? IsInlinedFromHot : consistsOfNumbersAndConstants(program, function, id) && computeMapKeyValues(program, FunctionID(i), function).get(id.index()) ? IsKey : IsOutlined];
            }
        }
    }
    constexpr unsigned minSitesForStringBody = 3;
    UncheckedKeyHashMap<String, unsigned> signatureSites;
    for (auto& perFunction : sites) {
        for (auto& site : perFunction) {
            if (!site.isStringCreatedInBody.isEmpty())
                ++signatureSites.add(site.signature, 0).iterator->value;
        }
    }
    for (auto& perFunction : sites) {
        for (auto& site : perFunction) {
            if (site.isStringCreatedInBody.isEmpty() || signatureSites.get(site.signature) >= minSitesForStringBody)
                continue;
            site.isStringCreatedInBody.clearAll();
            site.signature = site.stringSignature;
        }
    }
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> signatureBody;
    for (unsigned i = 0; i < count; ++i) {
        if (sites[i].isEmpty())
            continue;
        auto& function = std::as_const(program).functions[i];
        for (auto& site : sites[i]) {
            auto& value = function.at(site.value);
            site.body = signatureBody.ensure(site.signature, [&] {
                Function created;
                created.name = program.emptyString;
                created.module = function.module;
                created.position = function.position;
                created.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
                created.isSharedOutlinedBody = true;
                created.length = value.arguments.size();
                created.resultType = value.type;
                created.blocks.append(Block { });
                Value concatenation(value);
                concatenation.inlineFrame = 0;
                concatenation.position = function.position;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    bool isCreatedString = site.isStringCreatedInBody.get(k);
                    auto& argument = function.at(isCreatedString ? function.at(value.arguments[k]).child(0) : value.arguments[k]);
                    Value parameter;
                    parameter.opcode = Opcode::Parameter;
                    parameter.position = function.position;
                    parameter.type = argument.type;
                    parameter.rep = argument.rep;
                    parameter.immediates = { 0, k };
                    created.parameterTypes.append(argument.type);
                    created.blocks[0].parameters.append(ValueID(created.values.size()));
                    concatenation.arguments[k] = ValueID(created.values.size());
                    created.values.append(WTF::move(parameter));
                }
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    if (!site.isStringCreatedInBody.get(k))
                        continue;
                    Value conversion(function.at(value.arguments[k]));
                    conversion.inlineFrame = 0;
                    conversion.position = function.position;
                    conversion.children[0] = concatenation.arguments[k];
                    concatenation.arguments[k] = ValueID(created.values.size());
                    created.blocks[0].values.append(ValueID(created.values.size()));
                    created.values.append(WTF::move(conversion));
                }
                created.blocks[0].values.append(ValueID(created.values.size()));
                created.blocks[0].terminator.kind = TerminatorKind::Return;
                created.blocks[0].terminator.value = ValueID(created.values.size());
                created.values.append(WTF::move(concatenation));
                bodies.append(WTF::move(created));
                return count + static_cast<uint32_t>(bodies.size()) - 1;
            }).iterator->value;
        }
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        for (auto& site : sites[i]) {
            auto& value = function.at(site.value);
            value.opcode = Opcode::CallDirect;
            value.immediates = { site.body, Format::none };
            value.copyPredictionFrom(Value { });
            for (size_t k : site.isStringCreatedInBody) {
                ValueID conversion = value.arguments[k];
                value.arguments[k] = function.at(conversion).child(0);
                for (auto& block : function.blocks) {
                    if (block.values.removeFirst(conversion))
                        break;
                }
            }
        }
    });
    if (Options::verboseAOTCompilation()) {
        uint64_t taken = 0;
        for (auto& perFunction : sites)
            taken += perFunction.size();
        RELEASE_ASSERT(numbers[IsOutlined] == taken);
        printOutliningStatistics("concatenations"_s, bodies.size(), { { "call a body"_s, numbers[IsOutlined] }, { "the function is hot by the text"_s, numbers[IsStaticallyHotReason] }, { "more than six parts"_s, numbers[HasTooManyParts] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsInlinedFromHot] }, { "a key"_s, numbers[IsKey] } });
    }
    for (size_t k = 0; k < bodies.size(); ++k)
        program.staticallyHotFunctions.set(program.functions.size() + k);
    program.functions.appendVector(WTF::move(bodies));
    destroyOnHelperThread(std::tuple { WTF::move(sites), WTF::move(signatureSites), WTF::move(signatureBody) });
}

static std::optional<uint64_t> encodedConstantInImage(const Program& program, const Value& constant)
{
    switch (constant.opcode) {
    case Opcode::Undefined:
        return JSValue::encode(jsUndefined());
    case Opcode::Null:
        return JSValue::encode(jsNull());
    case Opcode::Boolean:
        return JSValue::encode(jsBoolean(constant.boolean()));
    case Opcode::Integer:
        return JSValue::encode(jsNumber(static_cast<double>(constant.integer())));
    case Opcode::Double:
        return JSValue::encode(jsDoubleNumber(purifyNaN(std::bit_cast<double>(constant.bits()))));
    case Opcode::String: {
        auto bytes = program.string(constant.string());
        if (bytes.empty() || (bytes.size() == 1 && bytes[0] && isASCII(bytes[0])))
            return Data::encodedSmallString(bytes.empty() ? 0 : bytes[0]);
        return Data::encodedStringConstant(constant.string().index());
    }
    default:
        return std::nullopt;
    }
}

static void moveConstantTablesIntoImage(Program& program)
{
    if (Options::useAOTWideReferences())
        return;
    auto& builtins = Builtins::singleton();
    unsigned count = program.functions.size();
    Vector<Vector<std::pair<ValueID, Program::ConstantTable>>> found(count);
    static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)) || !program.functions.mayContain(i, builtinCalls))
            return;
        auto& function = std::as_const(program).functions[i];
        Vector<uint32_t> useCount;
        auto isUsedOnce = [&](ValueID id) {
            if (useCount.isEmpty()) {
                useCount.fill(0, function.values.size());
                for (auto& block : function.blocks) {
                    for (ValueID user : block.values) {
                        function.at(user).forEachChild([&](ValueID child) {
                            ++useCount[child.index()];
                        });
                    }
                    if (block.terminator.value)
                        ++useCount[block.terminator.value.index()];
                    for (auto& target : block.terminator.targets) {
                        for (ValueID argument : target.arguments)
                            ++useCount[argument.index()];
                    }
                }
            }
            return useCount[id.index()] == 1;
        };
        auto literalUsedOnlyHere = [&](ValueID list) -> const Value* {
            while (function.at(list).opcode == Opcode::Cast && isUsedOnce(list))
                list = function.at(list).child(0);
            auto& literal = function.at(list);
            return literal.opcode == Opcode::NewArray && isUsedOnce(list) ? &literal : nullptr;
        };
        std::as_const(program).functions.forEachIndexedValue(i, builtinCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (builtins.at(value.builtin()).kind != BuiltinKind::Constructor || value.arguments.size() != 1 || value.engineClass() == Format::none || value.child(0) || value.child(1) || value.child(2))
                return;
            JSType created = builtins.classes()[value.engineClass()].type;
            if (created != JSSetType && created != JSMapType)
                return;
            auto* literal = literalUsedOnlyHere(value.arguments[0]);
            if (!literal || literal->arguments.isEmpty() || literal->arguments.size() > CompactTable::maxCapacity)
                return;
            Program::ConstantTable table;
            UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> entryOf;
            for (ValueID element : literal->arguments) {
                ValueID keyID = element;
                std::optional<uint64_t> held;
                if (created == JSMapType) {
                    auto* pair = literalUsedOnlyHere(element);
                    if (!pair || pair->arguments.size() != 2)
                        return;
                    keyID = pair->arguments[0];
                    held = encodedConstantInImage(program, function.at(pair->arguments[1]));
                    if (!held)
                        return;
                }
                auto& key = function.at(keyID);
                if (key.opcode != Opcode::String)
                    return;
                if (!std::ranges::all_of(program.string(key.string()), isASCII<uint8_t>))
                    return;
                auto entry = entryOf.add(key.string().index(), static_cast<uint32_t>(table.keys.size()));
                if (entry.isNewEntry) {
                    table.keys.append(key.string());
                    if (held)
                        table.values.append(*held);
                } else if (held)
                    table.values[entry.iterator->value] = *held;
            }
            found[i].append({ id, WTF::move(table) });
        });
    });
    UncheckedKeyHashMap<String, uint32_t> numbers;
    size_t numberOfTables = 0;
    size_t numberOfKeys = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (found[i].isEmpty())
            continue;
        auto& function = program.functions[i];
        for (auto& [created, table] : found[i]) {
            ++numberOfTables;
            numberOfKeys += table.keys.size();
            String contents = makeString(table.keys.size(), table.values.isEmpty() ? " keys "_s : " pairs "_s, String(byteCast<Latin1Character>(asBytes(table.keys.span()))), String(byteCast<Latin1Character>(asBytes(table.values.span()))));
            uint32_t number = numbers.ensure(contents, [&] {
                program.tablesInImage.append(WTF::move(table));
                return static_cast<uint32_t>(program.tablesInImage.size() - 1);
            }).iterator->value;
            function.at(created).arguments.clear();
            function.at(created).setTableInImage(number);
        }
        removeUnusedValues(program, function);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfTables, " sets and maps with ", numberOfKeys, " constant strings as keys are ", program.tablesInImage.size(), " tables in the image");
}

static void outlineBuiltinCalls(Program& program)
{
    constexpr unsigned maxOperands = 6;
    auto& builtins = Builtins::singleton();
    std::array<BuiltinID, 3> operations { Operations::AppendSpread, Operations::Apply, Operations::CallWithSpread };
    auto isCandidate = [&](const Value& value) {
        if (value.opcode == Opcode::Add)
            return program.at(value.type).isString();
        if (value.opcode == Opcode::CallMethod && value.predictedReceiverClassPlusOne)
            return !value.isInTailPosition && value.arguments.size() + 1 + !!value.child(1) <= maxOperands;
        if (value.opcode != Opcode::CallBuiltin || value.takesElementsOfArrayLiteral || value.child(2) || value.arguments.size() + !!value.child(0) + !!value.child(1) > maxOperands)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush)
            return false;
        if (implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::MapSize || implementation.inlined == InlineBuiltin::SetSize))
            return true;
        BuiltinKind kind = builtins.at(value.builtin()).kind;
        return kind == BuiltinKind::Method || kind == BuiltinKind::StaticFunction || kind == BuiltinKind::Function || (!value.isInTailPosition && std::ranges::find(operations, value.builtin()) != operations.end());
    };
    auto isPredictedClassMethodRead = [&](const Function& function, const Value& value) {
        return value.opcode == Opcode::GetById && value.methodGetByIdBuiltin() && value.predictedReceiverClassPlusOne && !program.typeOf(function, value.child(0)).singleEngineClass();
    };
    auto definitionOf = [](const Function& function, ValueID id) -> const Value& {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        return function.at(id);
    };
    auto expectedKind = [&](const Function& function, ValueID id) {
        auto& operand = function.at(id);
        Type expected = operand.prediction() ? program.at(operand.prediction()).withoutPrimitives(Primitive::UndefinedOrNull) : program.at(operand.type);
        if (expected.isString())
            return program.types.string;
        if (expected.isInteger())
            return program.types.int32;
        if (expected.isNumber())
            return program.types.number;
        return TypeID();
    };
    auto predictedMethodName = [&](const Function& function, const Value& value) {
        if (value.isInTailPosition || value.isSignatureCall || value.speculatedMethod != SpeculatedMethod::None || value.predictedReceiverClassPlusOne || value.type != program.types.anything || value.arguments.size() + 2 > maxOperands)
            return StringID();
        StringID name;
        ValueID receiver;
        if (value.opcode == Opcode::CallMethod && !value.child(1)) {
            receiver = value.child(0);
            name = value.string();
        } else if (value.opcode == Opcode::Call && value.child(1)) {
            receiver = value.child(1);
            if (auto& read = definitionOf(function, value.child(0)); read.opcode == Opcode::GetById && &definitionOf(function, read.child(0)) == &definitionOf(function, receiver))
                name = read.string();
        }
        if (!name || !function.at(receiver).prediction() || !program.builtinName(name))
            return StringID();
        if (expectedKind(function, receiver) != program.types.string)
            return StringID();
        BuiltinID first = builtins.findForInstance(BuiltinKind::Method, builtins.stringClass(), program.builtinName(name));
        if (!first || builtins.at(first).implementation.kind != BuiltinImplementation::Inline)
            return StringID();
        return name;
    };
    auto isDescribedByLiteral = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3 || function.at(value.arguments[2]).opcode != Opcode::NewObject)
            return false;
        auto& builtin = builtins.at(value.builtin());
        return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
    };
    auto staysInBody = [](const Value& operand) {
        return operand.opcode == Opcode::Integer || operand.opcode == Opcode::Double || operand.opcode == Opcode::Boolean || operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null || operand.opcode == Opcode::String;
    };
    auto forEachOperand = [](auto& value, auto&& functor) {
        for (auto& child : value.children) {
            if (child)
                functor(child);
        }
        for (auto& argument : value.arguments)
            functor(argument);
    };
    auto createsNullPrototypeObject = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::Null)
            return false;
        auto& builtin = builtins.at(value.builtin());
        return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
    };
    auto isInlineBuiltin = [&](const Value& value, InlineBuiltin inlined) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == inlined;
    };
    auto createsWithPrototypeOf = [&](const Function& function, const Value& value) {
        return isInlineBuiltin(value, InlineBuiltin::ObjectCreate) && value.arguments.size() == 1 && isInlineBuiltin(function.at(value.arguments[0]), InlineBuiltin::ObjectGetPrototypeOf);
    };
    auto isPartOfCloneWithPrototype = [&](const Function& function, unsigned block, unsigned index, ValueID id) {
        auto& value = function.at(id);
        if (createsWithPrototypeOf(function, value))
            return true;
        if (isInlineBuiltin(value, InlineBuiltin::ObjectAssign))
            return value.arguments.size() >= 2 && createsWithPrototypeOf(function, function.at(value.arguments[0]));
        auto& values = function.blocks[block].values;
        return isInlineBuiltin(value, InlineBuiltin::ObjectGetPrototypeOf) && index + 1 < values.size() && createsWithPrototypeOf(function, function.at(values[index + 1])) && function.at(values[index + 1]).arguments[0] == id;
    };
    auto populatesFirstArgument = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() < 2 || function.at(value.arguments[0]).opcode != Opcode::NewObject)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign;
    };
    auto writesLiteralAsJSON = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || function.at(program.originOf(function, value.arguments[0])).opcode != Opcode::NewObject)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::JSONStringify || implementation.inlined == InlineBuiltin::ResponseJSON || implementation.inlined == InlineBuiltin::NewJSONResponse);
    };
    auto isArithmetic = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& builtin = builtins.at(value.builtin());
        if (builtin.effects != Effect::Pure || builtin.implementation.kind != BuiltinImplementation::Inline)
            return false;
        InlineBuiltin inlined = builtin.implementation.inlined;
        if (inlined == InlineBuiltin::NumberIsFinite || inlined == InlineBuiltin::NumberIsInteger || inlined == InlineBuiltin::NumberIsNaN || inlined == InlineBuiltin::NumberIsSafeInteger)
            return true;
        bool result = program.at(value.type).hasOnlyPrimitives(Primitive::Number | Primitive::Boolean);
        forEachOperand(value, [&](ValueID operand) {
            result &= program.typeOf(function, operand).hasOnlyPrimitives(Primitive::Number | Primitive::Boolean);
        });
        return result;
    };
    unsigned count = program.functions.size();
    struct Site {
        ValueID value;
        String signature;
        uint32_t signatureNumber { 0 };
        bool isPredictedMethod { false };
        bool usesPredictions { false };
    };
    Vector<Vector<Site>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet candidates { Opcode::Add, Opcode::Call, Opcode::CallMethod, Opcode::CallBuiltin, Opcode::GetById };
        if (program.isHotOutsideLoops(FunctionID(i)) || !program.functions.mayContain(i, candidates))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        std::optional<BitVector> keys;
        std::optional<StringBuilderAnalysis> builders;
        std::optional<SplitsOnlyTakenApart> splitsOnlyTakenApart;
        std::optional<MatchesOnlyTakenApart> matchesOnlyTakenApart;
        std::optional<BitVector> iterables;
        auto isOpenedAsIterable = [&](ValueID id) {
            static constexpr OpcodeSet opens { Opcode::IteratorOpen };
            if (!program.functions.mayContain(i, opens))
                return false;
            if (!iterables)
                iterables = findValuesOpenedAsIterables(function);
            return iterables->get(id.index());
        };
        auto isOnlyTakenApart = [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = builtins.at(value.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Inline)
                return false;
            if (implementation.inlined == InlineBuiltin::RegExpExec || implementation.inlined == InlineBuiltin::StringMatch) {
                if (!matchesOnlyTakenApart)
                    matchesOnlyTakenApart = findMatchesOnlyTakenApart(program, function);
                return matchesOnlyTakenApart->contains(id.index());
            }
            if (implementation.inlined != InlineBuiltin::StringSplit)
                return false;
            if (!splitsOnlyTakenApart)
                splitsOnlyTakenApart = findSplitsOnlyTakenApart(program, function);
            return splitsOnlyTakenApart->contains(id.index());
        };
        auto usesStringBuilder = [&](ValueID id) {
            if (!builders)
                builders = analyzeStringBuilders(program, FunctionID(i), function);
            return builders->isState(id);
        };
        auto isLocallyCreatedKey = [&](ValueID key) {
            auto& created = function.at(key);
            if (created.opcode != Opcode::Add && (created.opcode != Opcode::Strcat || !consistsOfNumbersAndConstants(program, function, key)))
                return false;
            if (!keys)
                keys = computeMapKeyValues(program, FunctionID(i), function);
            return keys->get(key.index());
        };
        std::as_const(program).functions.forEachIndexedValueWithBlock(i, candidates, [&](unsigned b, unsigned index, ValueID id) {
            auto& value = function.at(id);
            StringID predictedMethod = predictedMethodName(function, value);
            if ((!isCandidate(value) && !isPredictedClassMethodRead(function, value) && !predictedMethod) || isDescribedByLiteral(function, value))
                return;
            if (value.opcode == Opcode::Add ? isLocallyCreatedKey(id) || usesStringBuilder(id) : value.opcode == Opcode::CallBuiltin && !value.arguments.isEmpty() && isLocallyCreatedKey(value.arguments[0]))
                return;
            if (isArithmetic(function, value) || createsNullPrototypeObject(function, value) || populatesFirstArgument(function, value) || isPartOfCloneWithPrototype(function, b, index, id) || writesLiteralAsJSON(function, value) || isOnlyTakenApart(id) || isOpenedAsIterable(id))
                return;
            if (!isInLoop)
                isInLoop = blocksInLoops(function);
            if (isInLoop->get(b))
                return;
            if (isInlinedFromHotFunction(program, function, value))
                return;
            StringBuilder signature;
            if (predictedMethod) {
                signature.append('p', static_cast<unsigned>(value.opcode), ' ', predictedMethod.index());
                forEachOperand(value, [&](ValueID operandID) {
                    signature.append(' ', expectedKind(function, operandID).index());
                    if (auto& operand = function.at(operandID); staysInBody(operand))
                        signature.append('=', static_cast<unsigned>(operand.opcode), ':', operand.immediates[0], ':', operand.immediates[1]);
                });
                sites[i].append({ id, signature.toString(), 0, true, true });
                sites[i].last().signature.impl()->hash();
                return;
            }
            signature.append(static_cast<unsigned>(value.opcode), ' ', value.immediates[0], ' ', value.immediates[1], ' ', value.type.index(), ' ', value.prediction().index(), value.children[0] ? ' ' : '-');
            if (value.predictedReceiverClassPlusOne)
                signature.append('e', value.predictedReceiverClassPlusOne);
            if (value.receiverHasOriginalStructure)
                signature.append('m');
            if (value.child(1))
                signature.append('r');
            bool usesPredictions = false;
            bool takesObject = false;
            forEachOperand(value, [&](ValueID operandID) {
                auto& operand = function.at(operandID);
                usesPredictions |= !!operand.prediction();
                takesObject |= program.at(operand.prediction() ? operand.prediction() : operand.type).hasObjects();
                signature.append(' ', operand.type.index(), '~', operand.prediction().index(), '~', operand.predictedClosure().index());
                if (staysInBody(operand))
                    signature.append('=', static_cast<unsigned>(operand.opcode), ':', operand.immediates[0], ':', operand.immediates[1]);
            });
            sites[i].append({ id, signature.toString(), 0, false, value.opcode == Opcode::CallBuiltin && usesPredictions && !takesObject });
            sites[i].last().signature.impl()->hash();
        });
    });
    struct Signature {
        uint32_t numberOfSites;
        uint32_t firstSiteFunction;
        uint32_t firstSite;
        uint32_t body;
    };
    Vector<Signature> signatures;
    {
        UncheckedKeyHashMap<String, uint32_t> signatureNumber;
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned k = 0; k < sites[i].size(); ++k) {
                auto result = signatureNumber.add(sites[i][k].signature, static_cast<uint32_t>(signatures.size()));
                if (result.isNewEntry)
                    signatures.append(Signature { 0, i, k, 0 });
                sites[i][k].signatureNumber = result.iterator->value;
                ++signatures[result.iterator->value].numberOfSites;
            }
        }
    }
    enum : unsigned { IsOutlined, OccursOnce, IsInHotFunction, IsGetterOrSetter, IsOperation, HasOtherKind, IsPush, HasTooManyOperands, IsDescribedByLiteral, IsArithmetic, IsAllocation, IsIterable, IsInLoop, IsUnchanged, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    uint64_t numberOfSplitsTakenApart = 0;
    uint64_t numberOfMatchesTakenApart = 0;
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        numberOfSplitsTakenApart += findSplitsOnlyTakenApart(program, function).size();
        numberOfMatchesTakenApart += findMatchesOnlyTakenApart(program, function).size();
        BitVector isSite;
        BitVector isSharedSite;
        for (auto& site : sites[i]) {
            isSite.set(site.value.index());
            if (signatures[site.signatureNumber].numberOfSites >= 2)
                isSharedSite.set(site.value.index());
        }
        bool isHot = program.isHotOutsideLoops(FunctionID(i));
        BitVector isInLoop = blocksInLoops(function);
        BitVector iterables = findValuesOpenedAsIterables(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::CallBuiltin)
                    continue;
                BuiltinKind kind = builtins.at(value.builtin()).kind;
                auto& implementation = builtins.at(value.builtin()).implementation;
                unsigned reason = IsUnchanged;
                if (isSite.get(id.index()))
                    reason = isSharedSite.get(id.index()) ? IsOutlined : OccursOnce;
                else if (isHot)
                    reason = IsInHotFunction;
                else if (!isCandidate(value)) {
                    if (value.takesElementsOfArrayLiteral)
                        reason = IsDescribedByLiteral;
                    else if (value.child(2) || value.arguments.size() + !!value.child(0) + !!value.child(1) > maxOperands)
                        reason = HasTooManyOperands;
                    else if (kind == BuiltinKind::Getter || kind == BuiltinKind::Setter)
                        reason = IsGetterOrSetter;
                    else if (kind == BuiltinKind::Operation)
                        reason = IsOperation;
                    else if (kind != BuiltinKind::Method && kind != BuiltinKind::StaticFunction && kind != BuiltinKind::Function)
                        reason = HasOtherKind;
                    else {
                        RELEASE_ASSERT(implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush);
                        reason = IsPush;
                    }
                } else if (isDescribedByLiteral(function, value))
                    reason = IsDescribedByLiteral;
                else if (isArithmetic(function, value))
                    reason = IsArithmetic;
                else if (createsNullPrototypeObject(function, value) || populatesFirstArgument(function, value))
                    reason = IsAllocation;
                else if (iterables.get(id.index()))
                    reason = IsIterable;
                else if (isInLoop.get(b))
                    reason = IsInLoop;
                ++numbers[reason];
            }
        }
    }
    Vector<uint32_t> signaturesWithBody;
    for (uint32_t number = 0; number < signatures.size(); ++number) {
        auto& signature = signatures[number];
        if (signature.numberOfSites < 2 && !sites[signature.firstSiteFunction][signature.firstSite].usesPredictions)
            continue;
        signature.body = count + static_cast<uint32_t>(signaturesWithBody.size());
        signaturesWithBody.append(number);
    }
    Vector<Function> bodies(signaturesWithBody.size());
    forEachIndexInParallel(signaturesWithBody.size(), [&](unsigned k) {
        auto& signature = signatures[signaturesWithBody[k]];
        auto& function = std::as_const(program).functions[signature.firstSiteFunction];
        auto& site = sites[signature.firstSiteFunction][signature.firstSite];
        auto& value = function.at(site.value);
        Function created;
        created.name = program.emptyString;
        created.module = function.module;
        created.position = function.position;
        created.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        created.isSharedOutlinedBody = true;
        created.resultType = value.type;
        created.blocks.append(Block { });
        Value call(value);
        call.inlineFrame = 0;
        call.position = function.position;
        if (site.isPredictedMethod) {
            call.immediates = { predictedMethodName(function, value).index(), Format::none };
            call.receiverHasOriginalStructure = false;
            call.copyPredictionFrom(Value { });
        }
        forEachOperand(call, [&](ValueID& operandID) {
            Value operand(function.at(operandID));
            TypeID kind = site.isPredictedMethod ? expectedKind(function, operandID) : TypeID();
            operand.inlineFrame = 0;
            operand.position = function.position;
            operandID = ValueID(created.values.size());
            if (staysInBody(operand))
                created.blocks[0].values.append(operandID);
            else {
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = function.position;
                parameter.type = site.isPredictedMethod ? program.types.anything : operand.type;
                parameter.rep = site.isPredictedMethod ? repOf(program.at(program.types.anything)) : operand.rep;
                if (site.isPredictedMethod)
                    parameter.setPrediction(kind);
                else
                    parameter.copyPredictionFrom(operand);
                parameter.immediates = { 0, static_cast<uint32_t>(created.parameterTypes.size()) };
                created.parameterTypes.append(parameter.type);
                created.blocks[0].parameters.append(operandID);
                operand = WTF::move(parameter);
            }
            created.values.append(WTF::move(operand));
        });
        created.length = created.parameterTypes.size();
        created.blocks[0].values.append(ValueID(created.values.size()));
        created.blocks[0].terminator.kind = TerminatorKind::Return;
        created.blocks[0].terminator.value = ValueID(created.values.size());
        created.values.append(WTF::move(call));
        bodies[k] = WTF::move(created);
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        for (auto& site : sites[i]) {
            auto& signature = signatures[site.signatureNumber];
            if (signature.numberOfSites < 2 && !site.usesPredictions)
                continue;
            auto& value = function.at(site.value);
            Arguments passed;
            forEachOperand(value, [&](ValueID operandID) {
                if (!staysInBody(function.at(operandID)))
                    passed.append(operandID);
            });
            value.opcode = Opcode::CallDirect;
            value.immediates = { signature.body, Format::none };
            value.children = { };
            value.arguments = WTF::move(passed);
        }
    });
    if (Options::verboseAOTCompilation()) {
        dataLogLn("AOT: ", numberOfSplitsTakenApart, " results of split and ", numberOfMatchesTakenApart, " of exec and match are only taken apart and never made");
        printOutliningStatistics("calls of built-ins"_s, bodies.size(), { { "call a body"_s, numbers[IsOutlined] }, { "a signature that occurs once"_s, numbers[OccursOnce] }, { "the function is hot"_s, numbers[IsInHotFunction] }, { "a getter or a setter"_s, numbers[IsGetterOrSetter] }, { "an operation of the engine"_s, numbers[IsOperation] }, { "a constructor, a constant"_s, numbers[HasOtherKind] }, { "push"_s, numbers[IsPush] }, { "more than six operands, or a second or third child"_s, numbers[HasTooManyOperands] }, { "defineProperty with a literal"_s, numbers[IsDescribedByLiteral] }, { "arithmetic"_s, numbers[IsArithmetic] }, { "an object born without a prototype"_s, numbers[IsAllocation] }, { "what a loop goes over"_s, numbers[IsIterable] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsUnchanged] } });
    }
    for (size_t k = 0; k < bodies.size(); ++k)
        program.staticallyHotFunctions.set(program.functions.size() + k);
    program.functions.appendVector(WTF::move(bodies));
    destroyOnHelperThread(WTF::move(sites));
}

bool canUseConstructBody(const Program& program, FunctionID id, const Function& function, const Value& value)
{
    if (function.isSharedOutlinedBody || program.isHotOutsideLoops(id))
        return false;
    return !isInlinedFromHotFunction(program, function, value);
}

static void outlineConstructs(Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<uint8_t, 0>> sites(count);
    static constexpr OpcodeSet constructions { Opcode::Construct };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayContain(i, constructions))
            return;
        auto& function = std::as_const(program).functions[i];
        std::optional<BitVector> isInLoop;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::Construct || value.arguments.size() > numberOfArgumentGPRs || function.isSharedOutlinedBody || program.isHotOutsideLoops(FunctionID(i)))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                if (!canUseConstructBody(program, FunctionID(i), function, value))
                    continue;
                sites[i].append(value.arguments.size());
            }
        }
    });
    std::array<unsigned, numberOfArgumentGPRs + 1> numberOfSites { };
    std::array<unsigned, numberOfArgumentGPRs + 1> first { };
    for (unsigned i = 0; i < count; ++i) {
        for (uint8_t numberOfArguments : sites[i]) {
            if (!numberOfSites[numberOfArguments]++)
                first[numberOfArguments] = i;
        }
    }
    for (unsigned numberOfArguments = 0; numberOfArguments <= numberOfArgumentGPRs; ++numberOfArguments) {
        if (numberOfSites[numberOfArguments] < 2)
            continue;
        Function created;
        created.name = program.emptyString;
        created.module = std::as_const(program).functions[first[numberOfArguments]].module;
        created.position = std::as_const(program).functions[first[numberOfArguments]].position;
        created.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        created.isSharedOutlinedBody = true;
        created.resultType = program.types.object;
        created.blocks.append(Block { });
        auto addParameter = [&](TypeID type) {
            ValueID id(created.values.size());
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = created.position;
            parameter.type = type;
            parameter.rep = repOf(program.at(type));
            parameter.immediates = { 0, static_cast<uint32_t>(created.parameterTypes.size()) };
            created.parameterTypes.append(type);
            created.blocks[0].parameters.append(id);
            created.values.append(WTF::move(parameter));
            return id;
        };
        Value construct;
        construct.opcode = Opcode::Construct;
        construct.position = created.position;
        construct.type = program.types.object;
        construct.rep = repOf(program.at(program.types.object));
        construct.immediates = { Format::none, Format::none };
        construct.children[0] = addParameter(program.types.anything);
        for (unsigned k = 0; k < numberOfArguments; ++k)
            construct.arguments.append(addParameter(program.types.anything));
        construct.children[1] = addParameter(program.types.integer);
        created.length = created.parameterTypes.size();
        created.blocks[0].values.append(ValueID(created.values.size()));
        created.blocks[0].terminator.kind = TerminatorKind::Return;
        created.blocks[0].terminator.value = ValueID(created.values.size());
        created.values.append(WTF::move(construct));
        program.constructBodies[numberOfArguments] = FunctionID(program.functions.size());
        program.functions.append(WTF::move(created));
    }
    if (!Options::verboseAOTCompilation())
        return;
    enum : unsigned { IsOutlined, OccursOnce, HasTooManyArguments, IsInBody, IsInHotFunction, IsInLoop, IsInlinedFromHot, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::Construct)
                    continue;
                ++numbers[value.arguments.size() > numberOfArgumentGPRs ? HasTooManyArguments : function.isSharedOutlinedBody ? IsInBody : program.isHotOutsideLoops(FunctionID(i)) ? IsInHotFunction : isInLoop.get(b) ? IsInLoop
                    : !canUseConstructBody(program, FunctionID(i), function, value) ? IsInlinedFromHot : numberOfSites[value.arguments.size()] < 2 ? OccursOnce : IsOutlined];
            }
        }
    }
    uint64_t taken = 0;
    for (auto& perFunction : sites)
        taken += perFunction.size();
    RELEASE_ASSERT(numbers[IsOutlined] + numbers[OccursOnce] == taken);
    printOutliningStatistics("constructions"_s, std::ranges::count_if(numberOfSites, [](unsigned sites) { return sites >= 2; }), { { "call a body"_s, numbers[IsOutlined] }, { "a number of arguments that occurs once"_s, numbers[OccursOnce] }, { "more arguments than registers"_s, numbers[HasTooManyArguments] }, { "in a body of several sites"_s, numbers[IsInBody] }, { "the function is hot"_s, numbers[IsInHotFunction] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsInlinedFromHot] } });
}

static void outlineCasts(Program& program)
{
    constexpr unsigned minSites = 4;
    unsigned count = program.functions.size();
    Vector<Vector<std::pair<ValueID, uint64_t>>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet casts { Opcode::Cast };
        FunctionID id(i);
        if (!program.isColdInCallGraph(id) || !program.functions.mayContain(i, casts))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.isSharedOutlinedBody || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        std::as_const(program).functions.forEachIndexedValueWithBlock(i, casts, [&](unsigned b, unsigned, ValueID valueID) {
            auto& value = function.at(valueID);
            if (!Program::mayHaveBody(value) || value.rep == Rep::Void || value.operandClassIsProven)
                return;
            Rep given = function.at(value.child(0)).rep;
            if ((given != Rep::JSValue && given != Rep::Cell) || program.castIsRedundant(program.typeOf(function, value.child(0)), program.at(value.type), value))
                return;
            if (!isInLoop)
                isInLoop = blocksInLoops(function);
            if (isInLoop->get(b))
                return;
            sites[i].append({ valueID, Program::castBodyKey(value) });
        });
    });
    struct FirstSite {
        uint64_t key;
        uint32_t function;
        ValueID value;
    };
    Vector<FirstSite> firstSites;
    UncheckedKeyHashMap<uint64_t, uint32_t> numberOfSites;
    for (unsigned i = 0; i < count; ++i) {
        for (auto [valueID, key] : sites[i]) {
            auto result = numberOfSites.add(key, 0);
            if (result.isNewEntry)
                firstSites.append(FirstSite { key, i, valueID });
            ++result.iterator->value;
        }
    }
    Vector<Function> bodies;
    for (auto& first : firstSites) {
        if (numberOfSites.get(first.key) < minSites)
            continue;
        auto& function = std::as_const(program).functions[first.function];
        auto& value = function.at(first.value);
        Function created;
        created.name = program.emptyString;
        created.module = function.module;
        created.position = function.position;
        created.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        created.isSharedOutlinedBody = true;
        created.length = 1;
        created.resultType = value.type;
        created.blocks.append(Block { });
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.position = function.position;
        parameter.type = program.types.anything;
        parameter.rep = repOf(program.at(program.types.anything));
        parameter.immediates = { 0, 0 };
        created.parameterTypes.append(program.types.anything);
        ValueID given(created.values.size());
        created.blocks[0].parameters.append(given);
        created.values.append(WTF::move(parameter));
        Value cast(value);
        cast.inlineFrame = 0;
        cast.setParameterCheckCallee(FunctionID());
        cast.position = function.position;
        cast.children[0] = given;
        cast.copyPredictionFrom(Value { });
        created.blocks[0].values.append(ValueID(created.values.size()));
        created.blocks[0].terminator.kind = TerminatorKind::Return;
        created.blocks[0].terminator.value = ValueID(created.values.size());
        created.values.append(WTF::move(cast));
        bodies.append(WTF::move(created));
        program.castBodies.add(first.key, count + static_cast<uint32_t>(bodies.size()) - 1);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", bodies.size(), " casts are outlined for their cold sites");
    enum : unsigned { IsOutlined, HasTooFewSites, IsOutsideSet, MayHaveNoBody, IsNotInRegisterOrChecked, IsInLoop, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        FunctionID id(i);
        bool isInSet = !function.isSharedOutlinedBody && program.isColdInCallGraph(id);
        BitVector isSite;
        for (auto& site : sites[i])
            isSite.set(site.first.index());
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID valueID : function.blocks[b].values) {
                auto& value = function.at(valueID);
                if (value.opcode != Opcode::Cast)
                    continue;
                Rep given = function.at(value.child(0)).rep;
                unsigned reason = IsInLoop;
                if (isSite.get(valueID.index()))
                    reason = numberOfSites.get(Program::castBodyKey(value)) < minSites ? HasTooFewSites : IsOutlined;
                else if (!isInSet)
                    reason = IsOutsideSet;
                else if (!Program::mayHaveBody(value) || value.rep == Rep::Void)
                    reason = MayHaveNoBody;
                else if ((given != Rep::JSValue && given != Rep::Cell) || program.castIsRedundant(program.typeOf(function, value.child(0)), program.at(value.type), value))
                    reason = IsNotInRegisterOrChecked;
                ++numbers[reason];
            }
        }
    }
    if (Options::verboseAOTCompilation())
        printOutliningStatistics("casts"_s, bodies.size(), { { "call a body"_s, numbers[IsOutlined] }, { "fewer than four sites"_s, numbers[HasTooFewSites] }, { "outside the set"_s, numbers[IsOutsideSet] }, { "a cast that may have no body"_s, numbers[MayHaveNoBody] }, { "given in no register, or held already"_s, numbers[IsNotInRegisterOrChecked] }, { "in a loop"_s, numbers[IsInLoop] } });
    program.functions.appendVector(WTF::move(bodies));
}

struct ExternalModuleFetch {
    uint32_t precedingCalleeCount { 0 };
    ValueID read;
    ValueID undefined;
    ValueID comparison;
    ValueID fetch;
    ValueID store;
};

struct FoundModuleEvaluation {
    Program::ModuleEvaluation evaluation;
    Vector<ExternalModuleFetch, 2> fetches;
};

static bool isNoOp(const Function& function)
{
    return function.blocks.size() == 1 && function.blocks[0].values.isEmpty() && function.blocks[0].terminator.kind == TerminatorKind::Return && !function.blocks[0].terminator.value;
}

static std::optional<FoundModuleEvaluation> asModuleEvaluation(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (function.kind != FunctionKind::Module || !function.has(Format::FunctionIsHiddenInStackTraces) || function.blocks.size() < 5 || function.resultType || !function.parameterTypes.isEmpty()
        || !function.captureTypes.isEmpty() || !function.inlineFrames.isEmpty() || program.isEntry(id) || function.isGenericEntrypoint() || program.genericEntrypoints.contains(id.index()))
        return std::nullopt;
    for (auto& block : function.blocks) {
        if (!block.parameters.isEmpty())
            return std::nullopt;
        for (auto& target : block.terminator.targets) {
            if (!target.arguments.isEmpty())
                return std::nullopt;
        }
    }
    auto has = [&](const Block& block, std::initializer_list<Opcode> opcodes, TerminatorKind kind) {
        if (block.values.size() != opcodes.size() || block.terminator.kind != kind)
            return false;
        size_t index = 0;
        for (Opcode opcode : opcodes) {
            if (function.at(block.values[index++]).opcode != opcode)
                return false;
        }
        return true;
    };
    auto at = [&](const Block& block, size_t index) -> const Value& {
        return function.at(block.values[index]);
    };
    auto blockAt = [&](const Block& block, size_t target) -> const Block& {
        return function.blocks[block.terminator.targets[target].block.index()];
    };
    auto isPlain = [&](VariableID variable) {
        auto& record = program.variables[variable.index()];
        return record.startsUndefined && !record.type && record.store == Store::JSValue && !record.singleFunction;
    };
    auto isCall = [&](const Value& value) {
        if (value.opcode != Opcode::CallDirect || !value.arguments.isEmpty() || value.isJumpToContinuation() || value.inlineFrame)
            return false;
        auto& callee = program.at(value.function());
        return callee.kind == FunctionKind::Module && !callee.resultType && callee.resultTypes.isEmpty() && callee.parameterTypes.isEmpty() && callee.captureTypes.isEmpty();
    };
    FoundModuleEvaluation found;
    auto& result = found.evaluation;
    result.function = id;
    auto& entry = function.blocks[0];
    if (!has(entry, { Opcode::GetModuleVariable, Opcode::Undefined, Opcode::StrictEq }, TerminatorKind::Branch) || entry.terminator.value != entry.values[2]
        || at(entry, 2).child(0) != entry.values[0] || at(entry, 2).child(1) != entry.values[1])
        return std::nullopt;
    ValueID known = entry.values[0];
    result.isEvaluated = at(entry, 0).variable();
    if (!isPlain(result.isEvaluated))
        return std::nullopt;
    auto& otherwise = blockAt(entry, 1);
    bool forgets = has(otherwise, { }, TerminatorKind::Return) && !otherwise.terminator.value;
    if (!forgets) {
        if (!has(otherwise, { Opcode::Boolean, Opcode::StrictEq }, TerminatorKind::Branch) || otherwise.terminator.value != otherwise.values[1] || at(otherwise, 0).boolean()
            || at(otherwise, 1).child(0) != known || at(otherwise, 1).child(1) != otherwise.values[0])
            return std::nullopt;
        auto& hasThrown = blockAt(otherwise, 0);
        if (!has(hasThrown, { Opcode::GetModuleVariable }, TerminatorKind::Throw) || hasThrown.terminator.value != hasThrown.values[0])
            return std::nullopt;
        result.error = at(hasThrown, 0).variable();
        if (result.error == result.isEvaluated || !isPlain(result.error))
            return std::nullopt;
        if (auto& isDone = blockAt(otherwise, 1); !has(isDone, { }, TerminatorKind::Return) || isDone.terminator.value)
            return std::nullopt;
    }
    auto& first = blockAt(entry, 0);
    if (first.values.size() < 3 || at(first, 0).opcode != Opcode::Boolean || !at(first, 0).boolean() || at(first, 1).opcode != Opcode::PutModuleVariable || at(first, 1).child(0) != first.values[0]
        || at(first, 1).variable() != result.isEvaluated)
        return std::nullopt;
    BlockID handlerID;
    auto isWatched = [&](const Block& block) {
        if (block.terminator.kind != TerminatorKind::Invoke)
            return false;
        if (!handlerID)
            handlerID = block.terminator.targets[1].block;
        return block.terminator.targets[1].block == handlerID;
    };
    size_t numberOfBlocks = forgets ? 3 : 5;
    size_t start = 2;
    for (const Block* block = &first;; start = 0) {
        ++numberOfBlocks;
        if (numberOfBlocks > function.blocks.size())
            return std::nullopt;
        size_t size = block->values.size() - start;
        if (!size && block->terminator.kind == TerminatorKind::Return && !block->terminator.value)
            break;
        if (size == 1 && isWatched(*block) && isCall(at(*block, start))) {
            if (FunctionID callee = at(*block, start).function(); !isNoOp(program.at(callee)))
                result.callees.append(callee);
            block = &blockAt(*block, 0);
            continue;
        }
        if (size != 3 || block->terminator.kind != TerminatorKind::Branch || block->terminator.value != block->values[start + 2] || at(*block, start).opcode != Opcode::GetModuleVariable
            || at(*block, start + 1).opcode != Opcode::Undefined || at(*block, start + 2).opcode != Opcode::StrictEq || at(*block, start + 2).child(0) != block->values[start]
            || at(*block, start + 2).child(1) != block->values[start + 1])
            return std::nullopt;
        auto& fetches = blockAt(*block, 0);
        if (!has(fetches, { Opcode::GetExternalModule }, TerminatorKind::Invoke) || !isWatched(fetches))
            return std::nullopt;
        auto& stores = blockAt(fetches, 0);
        if (!has(stores, { Opcode::PutModuleVariable }, TerminatorKind::Jump) || at(stores, 0).variable() != at(*block, start).variable() || at(stores, 0).child(0) != fetches.values[0]
            || stores.terminator.targets[0].block != block->terminator.targets[1].block)
            return std::nullopt;
        numberOfBlocks += 2;
        found.fetches.append({ static_cast<uint32_t>(result.callees.size()), block->values[start], block->values[start + 1], block->values[start + 2], fetches.values[0], stores.values[0] });
        block = &blockAt(*block, 1);
    }
    if (!handlerID || numberOfBlocks != function.blocks.size())
        return std::nullopt;
    auto& handler = function.blocks[handlerID.index()];
    if (forgets) {
        if (!has(handler, { Opcode::CatchValue, Opcode::Undefined, Opcode::PutModuleVariable }, TerminatorKind::Throw) || handler.terminator.value != handler.values[0]
            || at(handler, 2).child(0) != handler.values[1] || at(handler, 2).variable() != result.isEvaluated)
            return std::nullopt;
    } else if (!has(handler, { Opcode::CatchValue, Opcode::Boolean, Opcode::PutModuleVariable, Opcode::PutModuleVariable }, TerminatorKind::Throw) || handler.terminator.value != handler.values[0] || at(handler, 1).boolean()
        || at(handler, 2).child(0) != handler.values[1] || at(handler, 2).variable() != result.isEvaluated || at(handler, 3).child(0) != handler.values[0] || at(handler, 3).variable() != result.error)
        return std::nullopt;
    return found;
}

static bool moduleEvaluationsHaveCycle(const Program& program)
{
    uint32_t numberOfRows = program.moduleEvaluations.size();
    auto forEachOtherEvaluationCalledBy = [&](uint32_t row, auto&& functor) {
        for (FunctionID callee : program.moduleEvaluations[row].callees) {
            auto other = program.moduleEvaluationIndexByFunction.find(callee.index());
            if (other != program.moduleEvaluationIndexByFunction.end() && other->value != row)
                functor(other->value);
        }
    };
    Vector<uint32_t> numberOfCalls(FillWith { }, numberOfRows, 0u);
    for (uint32_t row = 0; row < numberOfRows; ++row) {
        forEachOtherEvaluationCalledBy(row, [&](uint32_t callee) {
            ++numberOfCalls[callee];
        });
    }
    Vector<uint32_t> worklist;
    for (uint32_t row = 0; row < numberOfRows; ++row) {
        if (!numberOfCalls[row])
            worklist.append(row);
    }
    uint32_t acyclicPrefixCount = 0;
    while (!worklist.isEmpty()) {
        ++acyclicPrefixCount;
        forEachOtherEvaluationCalledBy(worklist.takeLast(), [&](uint32_t callee) {
            if (!--numberOfCalls[callee])
                worklist.append(callee);
        });
    }
    return acyclicPrefixCount != numberOfRows;
}

static bool requestsModuleEvaluation(const Value& value, const Vector<uint8_t>& evaluatesModule)
{
    return value.opcode == Opcode::CallDirect && value.arguments.isEmpty() && !value.isJumpToContinuation() && evaluatesModule[value.function().index()];
}

static unsigned removeRedundantModuleEvaluations(Function& function, const Vector<uint8_t>& evaluatesModule)
{
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> moduleNumber;
    unsigned numberOfRequests = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (!requestsModuleEvaluation(value, evaluatesModule))
                continue;
            ++numberOfRequests;
            unsigned next = moduleNumber.size();
            moduleNumber.add(value.function().index(), next);
        }
    }
    if (numberOfRequests == moduleNumber.size())
        return 0;
    auto suspends = [](const Value& value) {
        return value.opcode == Opcode::Await || value.opcode == Opcode::Yield;
    };
    Vector<BitVector> evaluatedAtBlockStart(function.blocks.size());
    BitVector isReached(function.blocks.size());
    isReached.set(0);
    Vector<unsigned, 16> worklist { 0u };
    auto flowInto = [&](unsigned block, const BitVector& evaluated) {
        if (!isReached.set(block)) {
            evaluatedAtBlockStart[block] = evaluated;
            worklist.append(block);
            return;
        }
        BitVector onAllPaths = evaluatedAtBlockStart[block];
        onAllPaths.filter(evaluated);
        if (onAllPaths == evaluatedAtBlockStart[block])
            return;
        evaluatedAtBlockStart[block] = WTF::move(onAllPaths);
        worklist.append(block);
    };
    while (!worklist.isEmpty()) {
        unsigned b = worklist.takeLast();
        auto& block = function.blocks[b];
        BitVector evaluated = evaluatedAtBlockStart[b];
        BitVector evaluatedBeforeLast = evaluated;
        for (unsigned i = 0; i < block.values.size(); ++i) {
            auto& value = function.at(block.values[i]);
            if (suspends(value))
                evaluated.clearAll();
            if (i + 1 == block.values.size())
                evaluatedBeforeLast = evaluated;
            if (requestsModuleEvaluation(value, evaluatesModule))
                evaluated.set(moduleNumber.get(value.function().index()));
        }
        bool isWatched = block.terminator.kind == TerminatorKind::Invoke;
        for (unsigned target = 0; target < block.terminator.targets.size(); ++target)
            flowInto(block.terminator.targets[target].block.index(), isWatched && target == 1 ? evaluatedBeforeLast : evaluated);
    }
    BitVector isUsed(function.values.size());
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                isUsed.set(child.index());
            });
        }
        block.terminator.forEachChild([&](ValueID& child) {
            isUsed.set(child.index());
        });
    }
    unsigned removed = 0;
    for (unsigned b : isReached) {
        auto& block = function.blocks[b];
        BitVector evaluated = evaluatedAtBlockStart[b];
        unsigned removedHere = 0;
        bool removedLast = false;
        for (unsigned i = 0; i < block.values.size(); ++i) {
            ValueID id = block.values[i];
            auto& value = function.at(id);
            if (suspends(value))
                evaluated.clearAll();
            if (!requestsModuleEvaluation(value, evaluatesModule))
                continue;
            if (!evaluated.set(moduleNumber.get(value.function().index())) || isUsed.get(id.index()))
                continue;
            removedLast = i + 1 == block.values.size();
            block.values[i] = ValueID();
            ++removedHere;
        }
        if (!removedHere)
            continue;
        block.values.removeAllMatching([](ValueID id) {
            return !id;
        });
        if (removedLast && block.terminator.kind == TerminatorKind::Invoke) {
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
        removed += removedHere;
    }
    return removed;
}

static void shareModuleEvaluation(Program& program)
{
    unsigned count = program.functions.size();
    auto found = collectInParallel<FoundModuleEvaluation>(count, [&](unsigned i, Vector<FoundModuleEvaluation>& collected) {
        if (auto evaluation = asModuleEvaluation(program, FunctionID(i)))
            collected.append(WTF::move(*evaluation));
    });
    Vector<uint8_t> isReachedByOtherPaths(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (program.functions.mayContainPredictedClosure(i)) {
            for (auto& value : function.values) {
                if (value.predictedClosure())
                    WTF::atomicStore(&isReachedByOtherPaths[value.predictedClosure().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        }
        static constexpr OpcodeSet functionReferences { Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::IsClosureOf, Opcode::GetCapture, Opcode::CallDirect };
        std::as_const(program).functions.forEachValue(i, functionReferences, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode != Opcode::CallDirect || !value.arguments.isEmpty() || value.isJumpToContinuation())
                WTF::atomicStore(&isReachedByOtherPaths[value.function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    found.removeAllMatching([&](auto& one) {
        return isReachedByOtherPaths[one.evaluation.function.index()];
    });
    for (auto& one : found) {
        program.moduleEvaluationIndexByFunction.add(one.evaluation.function.index(), program.moduleEvaluations.size());
        program.moduleEvaluations.append(WTF::move(one.evaluation));
    }
    bool hasCycle = moduleEvaluationsHaveCycle(program);
    constexpr unsigned minSitesWorthSharing = 4;
    if (program.moduleEvaluations.size() < minSitesWorthSharing && !hasCycle) {
        program.moduleEvaluations.clear();
        program.moduleEvaluationIndexByFunction.clear();
        return;
    }
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> externalModuleNumber;
    Vector<std::pair<FunctionID, ExternalModuleFetch>> externalModules;
    for (unsigned number = 0; number < found.size(); ++number) {
        auto& evaluation = program.moduleEvaluations[number];
        auto& function = std::as_const(program).at(evaluation.function);
        for (auto& fetch : found[number].fetches) {
            uint64_t key = static_cast<uint64_t>(function.at(fetch.read).variable().index()) << 32 | function.at(fetch.fetch).string().index();
            auto number = externalModuleNumber.add(key, static_cast<uint32_t>(externalModules.size()));
            if (number.isNewEntry)
                externalModules.append({ evaluation.function, fetch });
            evaluation.fetches.append({ fetch.precedingCalleeCount, number.iterator->value });
        }
    }

    uint32_t flags = std::as_const(program).at(program.moduleEvaluations[0].function).flags;
    uint32_t numberOfRows = program.moduleEvaluations.size();
    auto& types = program.types;
    Function* underConstruction = nullptr;
    ValueID fetchMarker;
    auto make = [&](unsigned numberOfBlocks, auto&& fill) {
        Function created;
        underConstruction = &created;
        created.name = program.emptyString;
        created.kind = FunctionKind::Module;
        created.flags = flags;
        created.parameterTypes.append(types.int32);
        created.length = 1;
        created.blocks.grow(numberOfBlocks);
        auto add = [&](unsigned block, Opcode opcode, TypeID type, ValueID child = { }, ValueID second = { }, uint32_t immediate = Format::none) {
            Value value;
            value.opcode = opcode;
            value.type = type;
            value.rep = repOf(program.at(type));
            value.children[0] = child;
            value.children[1] = second;
            value.immediates[0] = immediate;
            ValueID id(created.values.size());
            if (opcode == Opcode::Parameter)
                value.immediates = { block, static_cast<uint32_t>(created.blocks[block].parameters.size()) };
            created.values.append(WTF::move(value));
            if (opcode == Opcode::Parameter)
                created.blocks[block].parameters.append(id);
            else
                created.blocks[block].values.append(id);
            return id;
        };
        auto end = [&](unsigned block, TerminatorKind kind, ValueID value = { }, std::initializer_list<std::pair<unsigned, ValueID>> targets = { }) {
            auto& terminator = created.blocks[block].terminator;
            terminator.kind = kind;
            terminator.value = value;
            for (auto [to, argument] : targets) {
                Target target;
                target.block = BlockID(to);
                if (argument)
                    target.arguments.append(argument);
                terminator.targets.append(WTF::move(target));
            }
        };
        fill(add, end);
        program.functions.append(WTF::move(created));
    };
    program.moduleEvaluator = FunctionID(program.functions.size());
    program.moduleEvaluatorIfNeeded = FunctionID(program.functions.size() + 1);
    if (hasCycle)
        program.externalModuleEvaluator = FunctionID(program.functions.size() + 2);
    program.staticallyHotFunctions.set(program.moduleEvaluator.index());
    program.staticallyWarmFunctions.set(program.moduleEvaluator.index());
    make(externalModules.isEmpty() ? 14 : 16 + 3 * externalModules.size(), [&](auto& add, auto& end) {
        auto word = [&](unsigned block, ValueID index, uint32_t displacement) {
            return add(block, Opcode::ModuleEvaluationWord, types.int32, index, { }, displacement);
        };
        auto boolean = [&](unsigned block, bool value) {
            return add(block, Opcode::Boolean, types.boolean, { }, { }, value);
        };
        auto isSame = [&](unsigned block, ValueID a, ValueID b) {
            return add(block, Opcode::StrictEq, types.boolean, a, b);
        };
        constexpr unsigned entry = 0, first = 1, otherwise = 2, hasThrown = 3, isDone = 4, head = 5, pair = 6, isFunction = 7, isRow = 8, evaluate = 9, next = 10, handler = 11, forgets = 12, keeps = 13;
        ValueID row = add(entry, Opcode::Parameter, types.int32);
        ValueID flagAt = word(entry, row, 0);
        constexpr uint32_t isFlag = 1;
        ValueID flag = add(entry, Opcode::GetModuleVariableAt, types.anything, flagAt, { }, isFlag);
        end(entry, TerminatorKind::Branch, isSame(entry, flag, boolean(entry, true)), { { isDone, { } }, { otherwise, { } } });
        end(otherwise, TerminatorKind::Branch, isSame(otherwise, flag, boolean(otherwise, false)), { { hasThrown, { } }, { first, { } } });
        end(hasThrown, TerminatorKind::Throw, add(hasThrown, Opcode::GetModuleVariableAt, types.anything, word(hasThrown, word(hasThrown, row, numberOfRows), 0)));
        end(isDone, TerminatorKind::Return);
        add(first, Opcode::PutModuleVariableAt, types.undefined, flagAt, boolean(first, true), isFlag);
        ValueID start = word(first, row, numberOfRows);
        ValueID last = word(first, start, 1);
        end(first, TerminatorKind::Jump, { }, { { head, add(first, Opcode::OffsetIndex, types.int32, start, { }, 2) } });
        ValueID cursor = add(head, Opcode::Parameter, types.int32);
        end(head, TerminatorKind::Branch, isSame(head, cursor, last), { { isDone, { } }, { pair, { } } });
        ValueID location = word(pair, cursor, 0);
        ValueID following = add(pair, Opcode::OffsetIndex, types.int32, cursor, { }, 2);
        ValueID none = add(pair, Opcode::Integer, types.int32);
        constexpr unsigned notFunction = 14, isFetch = 15, firstExternalModule = 16;
        end(pair, TerminatorKind::Branch, isSame(pair, location, none), { { isFunction, { } }, { externalModules.isEmpty() ? isRow : notFunction, { } } });
        if (!externalModules.isEmpty()) {
            fetchMarker = add(notFunction, Opcode::Integer, types.int32);
            end(notFunction, TerminatorKind::Branch, isSame(notFunction, location, fetchMarker), { { isFetch, { } }, { isRow, { } } });
            end(isFetch, TerminatorKind::Switch, word(isFetch, cursor, 1));
            auto& dispatch = underConstruction->blocks[isFetch].terminator;
            for (unsigned number = 0; number < externalModules.size(); ++number) {
                auto& fetch = externalModules[number].second;
                auto& function = std::as_const(program).at(externalModules[number].first);
                unsigned asks = firstExternalModule + 3 * number;
                unsigned fetches = asks + 1;
                unsigned stores = asks + 2;
                auto copy = [&](unsigned block, ValueID copied, ValueID child = { }, ValueID second = { }) {
                    Value value = function.at(copied);
                    value.children[0] = child;
                    value.children[1] = second;
                    ValueID id(underConstruction->values.size());
                    underConstruction->values.append(WTF::move(value));
                    underConstruction->blocks[block].values.append(id);
                    return id;
                };
                ValueID known = copy(asks, fetch.read);
                ValueID undefined = copy(asks, fetch.undefined);
                end(asks, TerminatorKind::Branch, copy(asks, fetch.comparison, known, undefined), { { fetches, { } }, { next, { } } });
                ValueID fetched = copy(fetches, fetch.fetch);
                end(fetches, TerminatorKind::Invoke, { }, { { stores, { } }, { handler, { } } });
                copy(stores, fetch.store, fetched);
                end(stores, TerminatorKind::Jump, { }, { { next, { } } });
                dispatch.cases.append(number);
                dispatch.targets.append(Target { BlockID(asks), { } });
            }
            dispatch.targets.append(Target { BlockID(next), { } });
        }
        add(isFunction, Opcode::CallModuleFunction, types.anything, word(isFunction, cursor, 1));
        end(isFunction, TerminatorKind::Invoke, { }, { { next, { } }, { handler, { } } });
        end(isRow, TerminatorKind::Branch, isSame(isRow, add(isRow, Opcode::GetModuleVariableAt, types.anything, location, { }, isFlag), boolean(isRow, true)), { { next, { } }, { evaluate, { } } });
        word(evaluate, cursor, 1);
        add(evaluate, Opcode::CallDirect, types.anything, { }, { }, program.moduleEvaluator.index());
        end(evaluate, TerminatorKind::Invoke, { }, { { next, { } }, { handler, { } } });
        end(next, TerminatorKind::Jump, { }, { { head, following } });
        ValueID caught = add(handler, Opcode::CatchValue, types.anything);
        ValueID errorAt = word(handler, word(handler, row, numberOfRows), 0);
        end(handler, TerminatorKind::Branch, isSame(handler, errorAt, add(handler, Opcode::Integer, types.int32)), { { forgets, { } }, { keeps, { } } });
        add(forgets, Opcode::PutModuleVariableAt, types.undefined, flagAt, add(forgets, Opcode::Undefined, types.undefined), isFlag);
        end(forgets, TerminatorKind::Throw, caught);
        add(keeps, Opcode::PutModuleVariableAt, types.undefined, flagAt, boolean(keeps, false), isFlag);
        add(keeps, Opcode::PutModuleVariableAt, types.undefined, errorAt, caught);
        if (hasCycle)
            add(keeps, Opcode::FailModuleCycle, types.undefined, row, caught, numberOfRows);
        end(keeps, TerminatorKind::Throw, caught);
    });
    {
        auto& created = program.at(program.moduleEvaluator);
        for (auto& value : created.values) {
            if (value.opcode == Opcode::Integer)
                value.setBits(static_cast<uint64_t>(-1));
            if (value.opcode == Opcode::CallDirect)
                value.arguments.append(created.blocks[9].values[0]);
        }
        if (fetchMarker)
            created.values[fetchMarker.index()].setBits(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Program::ModuleEvaluation::fetchMarker))));
    }
    make(3, [&](auto& add, auto& end) {
        ValueID row = add(0, Opcode::Parameter, types.int32);
        ValueID flag = add(0, Opcode::GetModuleVariableAt, types.anything, add(0, Opcode::ModuleEvaluationWord, types.int32, row, { }, 0), { }, 1);
        end(0, TerminatorKind::Branch, add(0, Opcode::StrictEq, types.boolean, flag, add(0, Opcode::Boolean, types.boolean, { }, { }, true)), { { 1, { } }, { 2, { } } });
        end(1, TerminatorKind::Return);
        add(2, Opcode::JumpToModuleEvaluator, types.undefined, row);
        end(2, TerminatorKind::Return);
    });
    if (hasCycle) {
        make(3, [&](auto& add, auto& end) {
            auto addToDepth = [&](unsigned block, int32_t addend) {
                add(block, Opcode::AddToModuleEvaluationDepth, types.undefined, { }, { }, static_cast<uint32_t>(addend));
            };
            add(0, Opcode::Parameter, types.int32);
            addToDepth(0, 1);
            add(0, Opcode::CallDirect, types.anything, { }, { }, program.moduleEvaluator.index());
            end(0, TerminatorKind::Invoke, { }, { { 1, { } }, { 2, { } } });
            addToDepth(1, -1);
            end(1, TerminatorKind::Return);
            ValueID caught = add(2, Opcode::CatchValue, types.anything);
            addToDepth(2, -1);
            end(2, TerminatorKind::Throw, caught);
        });
        auto& created = program.at(program.externalModuleEvaluator);
        for (auto& value : created.values) {
            if (value.opcode == Opcode::CallDirect)
                value.arguments.append(created.blocks[0].parameters[0]);
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.moduleEvaluations.size(), " modules are evaluated by one function, which fetches ", externalModules.size(), " external modules");
}

static void removeModuleEvaluations(Program& program)
{
    for (auto& evaluation : program.moduleEvaluations) {
        auto& function = program.at(evaluation.function);
        program.uncalledFunctions.set(evaluation.function.index());
        function.blocks.shrink(1);
        function.blocks[0].values.clear();
        function.blocks[0].terminator = Terminator { };
    }
    if (program.moduleEvaluations.isEmpty())
        return;
    Vector<uint8_t> isNamed(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet variableAccesses { Opcode::GetModuleVariable, Opcode::PutModuleVariable };
        if (!program.functions.mayContain(i, variableAccesses) || program.moduleEvaluationIndexByFunction.contains(i))
            return;
        std::as_const(program).functions.forEachValue(i, variableAccesses, [&](ValueID id) {
            WTF::atomicStore(&isNamed[std::as_const(program).functions[i].at(id).variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    for (auto& evaluation : program.moduleEvaluations) {
        for (VariableID variable : { evaluation.isEvaluated, evaluation.error }) {
            if (variable && !isNamed[variable.index()])
                program.variables[variable.index()].startsUndefined = false;
        }
    }
}

static bool fitInRegisters(const Program& program, const Vector<TypeID, 4>& types, bool withOneMore)
{
    unsigned numberOfGPRs = withOneMore;
    unsigned numberOfFPRs = 0;
    for (TypeID type : types) {
        Rep rep = repOf(program.at(type));
        ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
    }
    return numberOfGPRs <= numberOfArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
}

static constexpr OpcodeSet freshResultAllocations { Opcode::NewArray, Opcode::NewObject, Opcode::NewObjectWithShape };

static bool definesNamedFieldOf(const Program& program, const Function& function, const Value& value, ValueID object)
{
    return value.arguments.size() == 3 && value.arguments[0] == object && languageOperationOf(program, value) == LanguageOperation::DefineField && function.at(value.arguments[1]).opcode == Opcode::String;
}

static bool initializesLiteral(const Program& program, const Function& function, ValueID id, ValueID literal)
{
    auto& value = function.at(id);
    return (value.opcode == Opcode::PutField && value.child(0) == literal) || definesNamedFieldOf(program, function, value, literal);
}

std::optional<FreshResults> computeFreshResults(const Program& program, const Function& function)
{
    constexpr unsigned maxResults = numberOfArgumentGPRs + FPRInfo::numberOfArgumentRegisters;
    bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Method;
    if (!isPlain || function.blocks.isEmpty() || function.worker || !function.resultTypes.isEmpty() || function.has(FunctionUsesArgumentBuffer) || function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin) || function.has(Format::FunctionHasRestParameter) || program.isResumeFunction(function) || function.resumeFunction || function.startFunction)
        return std::nullopt;
    FreshResults result;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& terminator = function.blocks[b].terminator;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        Opcode returned = terminator.value ? function.at(terminator.value).opcode : Opcode::Undefined;
        if (returned == Opcode::Undefined || returned == Opcode::Null) {
            uint8_t nullishKind = returned == Opcode::Undefined ? 1 : 2;
            if (result.nullishKind && result.nullishKind != nullishKind)
                return std::nullopt;
            result.nullishKind = nullishKind;
            result.nullishReturns.append(b);
            continue;
        }
        if (!freshResultAllocations.contains(returned) || !function.blocks[b].values.contains(terminator.value))
            return std::nullopt;
        result.returns.append({ b, { } });
    }
    if (result.returns.isEmpty())
        return std::nullopt;
    Vector<uint32_t> uses;
    uses.fill(0, function.values.size());
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            function.at(id).forEachChild([&](ValueID child) {
                ++uses[child.index()];
            });
        }
        if (block.terminator.value)
            ++uses[block.terminator.value.index()];
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                ++uses[argument.index()];
        }
    }
    for (auto& [b, held] : result.returns) {
        auto& block = function.blocks[b];
        ValueID created = block.terminator.value;
        auto& value = function.at(created);
        bool isFirst = &held == &result.returns[0].second;
        if (value.opcode == Opcode::NewArray) {
            if ((!isFirst && !result.isArray) || uses[created.index()] != 1 || !value.arguments.size() || value.arguments.size() > maxResults)
                return std::nullopt;
            result.isArray = true;
            held.appendRange(value.arguments.begin(), value.arguments.end());
        } else if (value.opcode == Opcode::NewObject) {
            if ((!isFirst && result.names.isEmpty()) || result.nullishKind || value.child(0) || !value.arguments.isEmpty())
                return std::nullopt;
            Vector<StringID, 4> names;
            for (ValueID id : block.values) {
                auto& definition = function.at(id);
                if (!definesNamedFieldOf(program, function, definition, created))
                    continue;
                StringID name = function.at(definition.arguments[1]).string();
                auto text = program.string(name);
                if (definition.arguments[2] == created || text.empty() || isASCIIDigit(text[0]) || equalSpans(text, asBytes("__proto__"_s.span8())) || uses[id.index()] || names.contains(name))
                    return std::nullopt;
                names.append(name);
                held.append(definition.arguments[2]);
            }
            if (names.isEmpty() || names.size() > maxResults || uses[created.index()] != names.size() + 1 || (!isFirst && names != result.names))
                return std::nullopt;
            result.names = WTF::move(names);
        } else {
            auto& layout = program.at(value.layout());
            unsigned size = layout.fields.size();
            if ((!isFirst && (result.isArray || result.layout != value.layout())) || value.child(0) || !isObjectLayout(layout.kind) || !size || size > maxResults || uses[created.index()] != size + 1)
                return std::nullopt;
            auto& firstAllocation = function.allocations[function.at(function.blocks[result.returns[0].first].terminator.value).allocation()];
            auto& currentAllocation = function.allocations[value.allocation()];
            if (firstAllocation.order != currentAllocation.order || firstAllocation.initialFieldCount != currentAllocation.initialFieldCount || firstAllocation.constructedBy != currentAllocation.constructedBy)
                return std::nullopt;
            result.layout = value.layout();
            held.fill(ValueID(), size);
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (store.opcode != Opcode::PutField || store.child(0) != created)
                    continue;
                if (store.layout() != value.layout() || held[store.field()])
                    return std::nullopt;
                held[store.field()] = store.child(1);
            }
            if (held.contains(ValueID()))
                return std::nullopt;
        }
        if (held.size() != result.returns[0].second.size())
            return std::nullopt;
    }
    for (unsigned k = 0; k < result.returns[0].second.size(); ++k) {
        TypeID type = function.at(result.returns[0].second[k]).type;
        for (auto& entry : result.returns) {
            TypeID other = function.at(entry.second[k]).type;
            if (other == type)
                continue;
            TypeID joined = program.types.anything;
            for (TypeID wider : { program.types.boolean, program.types.int32, program.types.number, program.types.string }) {
                if (program.at(type).isSubsetOf(program.at(wider), program) && program.at(other).isSubsetOf(program.at(wider), program)) {
                    joined = wider;
                    break;
                }
            }
            type = joined;
        }
        if (TypeID declared = result.layout ? program.at(result.layout).fields[k].type : TypeID(); declared && !program.at(type).isSubsetOf(program.at(declared), program))
            type = declared;
        if (repOf(program.at(type)) == Rep::Void)
            type = program.types.anything;
        result.types.append(type);
    }
    if (!fitInRegisters(program, result.types, result.nullishKind))
        return std::nullopt;
    return result;
}

std::optional<FreshResultReads> computeFreshResultReads(const Program& program, const Function& function, ValueID call, const FreshResults& fresh)
{
    FreshResultReads result;
    result.aliases.append(call);
    for (unsigned next = 0; next < result.aliases.size(); ++next) {
        ValueID object = result.aliases[next];
        for (auto& block : function.blocks) {
            if (block.terminator.value == object)
                return std::nullopt;
            for (auto& target : block.terminator.targets) {
                if (target.arguments.contains(object))
                    return std::nullopt;
            }
            for (ValueID user : block.values) {
                auto& value = function.at(user);
                bool uses = false;
                value.forEachChild([&](ValueID child) {
                    uses |= child == object;
                });
                if (!uses)
                    continue;
                std::optional<uint32_t> index;
                switch (value.opcode) {
                case Opcode::GetByVal: {
                    auto& key = function.at(value.child(1));
                    if (fresh.isArray && value.child(0) == object && value.child(1) != object && key.opcode == Opcode::Integer && key.integer() >= 0 && key.integer() < static_cast<int64_t>(fresh.types.size()))
                        index = static_cast<uint32_t>(key.integer());
                    break;
                }
                case Opcode::GetField:
                    if (fresh.layout && isObjectLayout(program.at(value.layout()).kind))
                        index = program.at(fresh.layout).fieldNamed(program.at(value.layout()).fields[value.field()].name);
                    break;
                case Opcode::GetById:
                    if (fresh.layout && !value.methodGetByIdBuiltin())
                        index = program.at(fresh.layout).fieldNamed(value.string());
                    else if (size_t found = value.methodGetByIdBuiltin() ? notFound : fresh.names.find(value.string()); found != notFound)
                        index = static_cast<uint32_t>(found);
                    break;
                case Opcode::IsUndefinedOrNull:
                    result.nullishTests.append(user);
                    continue;
                case Opcode::IteratorOpen:
                    if (fresh.isArray) {
                        result.aliases.append(user);
                        continue;
                    }
                    break;
                case Opcode::Cast:
                    if (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull) {
                        result.aliases.append(user);
                        continue;
                    }
                    break;
                default:
                    break;
                }
                if (!index || !program.at(fresh.types[*index]).isSubsetOf(program.at(value.type), program))
                    return std::nullopt;
                result.reads.append({ user, *index });
            }
        }
    }
    return result;
}

static unsigned readJoinedResultsOnEdges(const Program& program, Function& function)
{
    auto literalReturnedTo = [&](ValueID id) -> const Value* {
        auto& call = std::as_const(function).at(id);
        if ((call.opcode == Opcode::NewObjectWithShape || call.opcode == Opcode::NewObject) && !call.child(0) && call.arguments.isEmpty())
            return &call;
        if (call.opcode != Opcode::CallDirect || call.isJumpToContinuation())
            return nullptr;
        auto& wrapper = program.at(call.function());
        if (!wrapper.worker || wrapper.nullishResultKind)
            return nullptr;
        for (auto& value : wrapper.values) {
            if (value.opcode == Opcode::NewObjectWithShape || value.opcode == Opcode::NewObject)
                return &value;
        }
        return nullptr;
    };
    auto returnsField = [&](ValueID call, StringID name) {
        auto* literal = literalReturnedTo(call);
        if (literal->opcode == Opcode::NewObjectWithShape)
            return !!program.at(literal->layout()).fieldNamed(name);
        auto& wrapper = literal == &std::as_const(function).at(call) ? std::as_const(function) : program.at(std::as_const(function).at(call).function());
        return wrapper.values.containsIf([&](auto& value) {
            return value.arguments.size() == 3 && &wrapper.at(value.arguments[0]) == literal && languageOperationOf(program, value) == LanguageOperation::DefineField && wrapper.at(value.arguments[1]).opcode == Opcode::String && wrapper.at(value.arguments[1]).string() == name;
        });
    };
    struct Arm {
        unsigned block;
        ValueID call;
    };
    auto tryJoin = [&](unsigned b, unsigned position) {
        constexpr unsigned maxNestedJoins = 8;
        ValueID joined = function.blocks[b].parameters[position];
        Vector<Arm, 4> arms;
        Vector<Arm, 4> leaves;
        Vector<Arm, 2> joins { { b, joined } };
        for (unsigned next = 0; next < joins.size(); ++next) {
            unsigned to = joins[next].block;
            size_t at = function.blocks[to].parameters.find(joins[next].call);
            for (unsigned from = 0; from < function.blocks.size(); ++from) {
                auto& block = function.blocks[from];
                for (auto& target : block.terminator.targets) {
                    if (target.block.index() != to)
                        continue;
                    ValueID given = target.arguments[at];
                    if (block.terminator.kind != TerminatorKind::Jump)
                        return false;
                    if (!next)
                        arms.append({ from, given });
                    if (literalReturnedTo(given) && block.values.contains(given)) {
                        leaves.append({ from, given });
                        continue;
                    }
                    if (!block.parameters.contains(given) || joins.size() == maxNestedJoins || joins.containsIf([&](auto& join) { return join.call == given; }))
                        return false;
                    joins.append({ from, given });
                }
            }
        }
        Vector<ValueID, 4> aliases { joined };
        Vector<ValueID, 8> reads;
        Vector<ValueID, 2> nullishTests;
        for (unsigned next = 0; next < aliases.size(); ++next) {
            ValueID object = aliases[next];
            for (auto& block : function.blocks) {
                if (block.terminator.value == object)
                    return false;
                for (auto& target : block.terminator.targets) {
                    if (target.arguments.contains(object))
                        return false;
                }
                for (ValueID user : block.values) {
                    auto& value = std::as_const(function).at(user);
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= child == object;
                    });
                    if (!uses)
                        continue;
                    StringID name;
                    switch (value.opcode) {
                    case Opcode::GetField:
                        if (isObjectLayout(program.at(value.layout()).kind))
                            name = program.at(value.layout()).fields[value.field()].name;
                        break;
                    case Opcode::GetById:
                        if (!value.methodGetByIdBuiltin())
                            name = value.string();
                        break;
                    case Opcode::IsUndefinedOrNull:
                        nullishTests.append(user);
                        continue;
                    case Opcode::Cast:
                        if (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull) {
                            aliases.append(user);
                            continue;
                        }
                        break;
                    default:
                        break;
                    }
                    if (!name)
                        return false;
                    for (auto& leaf : leaves) {
                        if (!returnsField(leaf.call, name))
                            return false;
                    }
                    reads.append(user);
                }
            }
        }
        leaves.append(joins.span().subspan(1));
        for (auto& arm : leaves) {
            unsigned uses = 0;
            for (auto& block : function.blocks) {
                for (ValueID user : block.values) {
                    unsigned usesByValue = 0;
                    std::as_const(function).at(user).forEachChild([&](ValueID child) {
                        usesByValue += child == arm.call;
                    });
                    if (usesByValue == 1 && &block == &function.blocks[arm.block] && initializesLiteral(program, function, user, arm.call))
                        continue;
                    if (auto& store = std::as_const(function).at(user); usesByValue == 1 && &block == &function.blocks[arm.block] && store.opcode == Opcode::PutById && store.child(0) == arm.call && literalReturnedTo(arm.call) == &std::as_const(function).at(arm.call) && returnsField(arm.call, store.string()))
                        continue;
                    uses += usesByValue;
                }
                uses += block.terminator.value == arm.call;
                for (auto& target : block.terminator.targets)
                    uses += std::ranges::count(target.arguments, arm.call);
            }
            if (uses != 1)
                return false;
        }
        function.blocks[b].parameters.removeAt(position);
        for (auto& arm : arms)
            function.blocks[arm.block].terminator.targets[0].arguments.removeAt(position);
        for (ValueID read : reads) {
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = function.at(read).position;
            parameter.inlineFrame = function.at(joined).inlineFrame;
            parameter.type = function.at(read).type;
            parameter.rep = function.at(read).rep;
            parameter.copyPredictionFrom(function.at(read));
            ValueID held(function.values.size());
            function.values.append(WTF::move(parameter));
            function.blocks[b].parameters.append(held);
            for (auto& arm : arms) {
                Value onEdge(function.at(read));
                onEdge.children[0] = arm.call;
                ValueID id(function.values.size());
                function.values.append(WTF::move(onEdge));
                function.blocks[arm.block].values.append(id);
                function.blocks[arm.block].terminator.targets[0].arguments.append(id);
            }
            auto& value = function.at(read);
            value.opcode = Opcode::Cast;
            value.children = { held, ValueID(), ValueID() };
            value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
        }
        for (unsigned k = 0; k < function.blocks[b].parameters.size(); ++k)
            function.at(function.blocks[b].parameters[k]).immediates = { b, k };
        for (ValueID test : nullishTests) {
            auto& value = function.at(test);
            value.opcode = Opcode::Boolean;
            value.children = { };
            value.immediates = { 0, Format::none };
        }
        if (aliases.size() > 1) {
            for (auto& block : function.blocks) {
                block.values.removeAllMatching([&](ValueID id) {
                    return id != joined && aliases.contains(id);
                });
            }
        }
        return true;
    };
    unsigned changed = 0;
    BitVector refused;
    for (bool found = true; std::exchange(found, false);) {
        for (unsigned from = 0; from < function.blocks.size() && !found; ++from) {
            auto& terminator = function.blocks[from].terminator;
            if (terminator.kind != TerminatorKind::Jump)
                continue;
            unsigned b = terminator.targets[0].block.index();
            for (unsigned position = 0; position < function.blocks[from].terminator.targets[0].arguments.size() && !found; ++position) {
                if (!literalReturnedTo(function.blocks[from].terminator.targets[0].arguments[position]) || refused.get(function.blocks[b].parameters[position].index()))
                    continue;
                found = tryJoin(b, position);
                if (found)
                    refused.clearAll();
                else
                    refused.set(function.blocks[b].parameters[position].index());
            }
        }
        changed += found;
    }
    return changed;
}

static Type unionOf(const Type& a, const Type& b)
{
    Type result = a;
    result.primitives |= b.primitives;
    result.integers = a.integers.unionWith(b.integers);
    result.anyObject |= b.anyObject;
    result.anyFunction |= b.anyFunction;
    for (LayoutID supertype : b.objectLayouts) {
        if (!result.objectLayouts.contains(supertype))
            result.objectLayouts.append(supertype);
    }
    std::ranges::sort(result.objectLayouts, { }, [](LayoutID supertype) {
        return supertype.index();
    });
    for (uint16_t engineClass : b.engineClasses) {
        if (!result.engineClasses.contains(engineClass))
            result.engineClasses.append(engineClass);
    }
    std::ranges::sort(result.engineClasses);
    if (a.elements != b.elements)
        result.elements = TypeID();
    result.normalizeLiteralString();
    return result;
}

Vector<unsigned> immediateDominatorsOf(const Function& function)
{
    unsigned count = function.blocks.size();
    Vector<unsigned> postorder;
    Vector<unsigned> numberOf;
    numberOf.fill(UINT_MAX, count);
    {
        BitVector seen(count);
        Vector<std::pair<unsigned, unsigned>> stack { { 0, 0 } };
        seen.set(0);
        while (!stack.isEmpty()) {
            auto& [block, next] = stack.last();
            auto& targets = function.blocks[block].terminator.targets;
            if (next < targets.size()) {
                unsigned target = targets[next++].block.index();
                if (!seen.set(target))
                    stack.append({ target, 0 });
                continue;
            }
            numberOf[block] = postorder.size();
            postorder.append(block);
            stack.removeLast();
        }
    }
    Vector<Vector<unsigned, 2>> predecessors(count);
    for (unsigned b : postorder) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    Vector<unsigned> result;
    result.fill(UINT_MAX, count);
    result[0] = 0;
    bool changed = !tryFindImmediateDominators(postorder.size(), [&](unsigned i) { return postorder[postorder.size() - 1 - i]; }, predecessors, result);
    while (changed) {
        changed = false;
        for (unsigned i = postorder.size(); i--;) {
            unsigned block = postorder[i];
            if (!block)
                continue;
            unsigned dominator = UINT_MAX;
            for (unsigned predecessor : predecessors[block]) {
                if (result[predecessor] == UINT_MAX)
                    continue;
                if (dominator == UINT_MAX) {
                    dominator = predecessor;
                    continue;
                }
                unsigned a = predecessor;
                unsigned b = dominator;
                while (a != b) {
                    while (numberOf[a] < numberOf[b])
                        a = result[a];
                    while (numberOf[b] < numberOf[a])
                        b = result[b];
                }
                dominator = a;
            }
            if (dominator != UINT_MAX && result[block] != dominator) {
                result[block] = dominator;
                changed = true;
            }
        }
    }
    for (unsigned b = 0; b < count; ++b) {
        if (result[b] == UINT_MAX)
            result[b] = b;
    }
    return result;
}

bool scalarizeNullableResult(const Program& program, Function& function, ValueID call)
{
    auto& wrapper = program.at(function.at(call).function());
    auto& types = program.at(wrapper.worker).resultTypes;
    const Value* created = nullptr;
    for (auto& value : wrapper.values) {
        if (value.opcode == Opcode::NewArray || value.opcode == Opcode::NewObjectWithShape)
            created = &value;
    }
    RELEASE_ASSERT(created && wrapper.nullishResultKind);
    bool isArray = created->opcode == Opcode::NewArray;
    Opcode nullishOpcode = wrapper.nullishResultKind == 1 ? Opcode::Undefined : Opcode::Null;

    enum class Test : uint8_t { HasCheck, IsNullish, Never };
    Vector<ValueID, 4> aliases { call };
    Vector<std::pair<ValueID, uint32_t>, 8> reads;
    Vector<std::pair<ValueID, Test>, 4> tests;
    BitVector needsObject(function.blocks.size());
    for (unsigned next = 0; next < aliases.size(); ++next) {
        ValueID object = aliases[next];
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (block.terminator.value == object)
                return false;
            for (auto& target : block.terminator.targets) {
                if (target.arguments.contains(object))
                    return false;
            }
            for (ValueID user : block.values) {
                auto& value = function.at(user);
                bool uses = false;
                value.forEachChild([&](ValueID child) {
                    uses |= child == object;
                });
                if (!uses)
                    continue;
                std::optional<uint32_t> index;
                switch (value.opcode) {
                case Opcode::AssertNotAllocated:
                    aliases.append(user);
                    continue;
                case Opcode::IteratorOpen:
                    if (!isArray)
                        return false;
                    needsObject.set(b);
                    aliases.append(user);
                    continue;
                case Opcode::ToBoolean:
                    tests.append({ user, Test::HasCheck });
                    continue;
                case Opcode::IsUndefinedOrNull:
                    tests.append({ user, Test::IsNullish });
                    continue;
                case Opcode::StrictEq:
                case Opcode::Eq: {
                    ValueID other = value.child(0) == object ? value.child(1) : value.child(0);
                    Opcode constant = function.at(other).opcode;
                    if (other == object || (constant != Opcode::Undefined && constant != Opcode::Null))
                        return false;
                    tests.append({ user, value.opcode == Opcode::Eq || constant == nullishOpcode ? Test::IsNullish : Test::Never });
                    continue;
                }
                case Opcode::Cast:
                    if (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull) {
                        if (value.castReason() != CastReason::Upcast)
                            needsObject.set(b);
                        aliases.append(user);
                        continue;
                    }
                    return false;
                case Opcode::GetByVal: {
                    auto& key = function.at(value.child(1));
                    if (isArray && value.child(0) == object && value.child(1) != object && key.opcode == Opcode::Integer && key.integer() >= 0 && key.integer() + 1 < static_cast<int64_t>(types.size()))
                        index = static_cast<uint32_t>(key.integer());
                    break;
                }
                case Opcode::GetField:
                    if (!isArray && isObjectLayout(program.at(value.layout()).kind))
                        index = program.at(created->layout()).fieldNamed(program.at(value.layout()).fields[value.field()].name);
                    break;
                case Opcode::GetById:
                    if (!isArray && !value.methodGetByIdBuiltin())
                        index = program.at(created->layout()).fieldNamed(value.string());
                    break;
                default:
                    break;
                }
                if (!index || !program.at(types[*index + 1]).isSubsetOf(program.at(value.type), program))
                    return false;
                reads.append({ user, *index + 1 });
                needsObject.set(b);
            }
        }
    }

    if (!needsObject.isEmpty()) {
        UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isTrueIfPresent;
        for (auto [test, kind] : tests) {
            if (kind != Test::Never)
                isTrueIfPresent.add(test.index(), kind == Test::HasCheck);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::LogicalNot || isTrueIfPresent.contains(id.index()))
                        continue;
                    if (auto found = isTrueIfPresent.find(value.child(0).index()); found != isTrueIfPresent.end()) {
                        bool flipped = !found->value;
                        isTrueIfPresent.add(id.index(), flipped);
                        changed = true;
                    }
                }
            }
        }
        Vector<unsigned> numberOfPredecessors;
        numberOfPredecessors.fill(0, function.blocks.size());
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets)
                ++numberOfPredecessors[target.block.index()];
        }
        BitVector isTargetWithObject(function.blocks.size());
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto found = isTrueIfPresent.find(block.terminator.value.index());
            if (found == isTrueIfPresent.end())
                continue;
            unsigned arm = block.terminator.targets[found->value ? 0 : 1].block.index();
            if (numberOfPredecessors[arm] == 1)
                isTargetWithObject.set(arm);
        }
        auto dominators = immediateDominatorsOf(function);
        for (unsigned b : needsObject) {
            bool isGuarded = false;
            for (unsigned at = b;; at = dominators[at]) {
                if (isTargetWithObject.get(at)) {
                    isGuarded = true;
                    break;
                }
                if (dominators[at] == at)
                    break;
            }
            if (!isGuarded)
                return false;
        }
    }

    auto resultAt = [&](const Value& like, uint32_t index) {
        Value result;
        result.opcode = Opcode::ResultAt;
        result.position = like.position;
        result.inlineFrame = like.inlineFrame;
        result.type = types[index];
        result.rep = repOf(program.at(result.type));
        result.children = { call, ValueID(), ValueID() };
        result.immediates = { index, Format::none };
        return result;
    };
    {
        auto& value = function.at(call);
        value.immediates[0] = wrapper.worker.index();
        value.type = program.types.undefined;
        value.rep = repOf(program.at(value.type));
        value.copyPredictionFrom(Value { });
    }
    for (auto [read, index] : reads)
        function.at(read) = resultAt(function.at(read), index);
    for (auto [test, kind] : tests) {
        if (kind == Test::HasCheck) {
            function.at(test) = resultAt(function.at(test), 0);
            continue;
        }
        auto& value = function.at(test);
        if (kind == Test::Never) {
            value.opcode = Opcode::Boolean;
            value.children = { };
            value.immediates = { 0, Format::none };
            continue;
        }
        ValueID presenceCheck(function.values.size());
        function.values.append(resultAt(function.at(test), 0));
        for (auto& block : function.blocks) {
            if (size_t at = block.values.find(test); at != notFound)
                block.values.insert(at, presenceCheck);
        }
        auto& negated = function.at(test);
        negated.opcode = Opcode::LogicalNot;
        negated.children = { presenceCheck, ValueID(), ValueID() };
        negated.immediates = { Format::none, Format::none };
    }
    for (auto& block : function.blocks) {
        block.values.removeAllMatching([&](ValueID id) {
            return id != call && aliases.contains(id);
        });
    }
    return true;
}

static void outlineDestructuredParameterReads(Program& program)
{
    static constexpr unsigned maxBodyParameters = 6;
    unsigned count = program.functions.size();
    Vector<uint32_t> parametersPassedNewObjects(FillWith { }, count, 0u);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet newObjects { Opcode::NewObjectWithShape };
        if (!program.functions.mayContain(i, newObjects))
            return;
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        std::as_const(program).functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
            auto& call = function.at(id);
            for (unsigned k = 0; k < call.arguments.size() && k < 32; ++k) {
                ValueID argument = call.arguments[k];
                while (function.at(argument).opcode == Opcode::Cast)
                    argument = function.at(argument).child(0);
                if (newObjects.contains(function.at(argument).opcode))
                    WTF::atomicExchangeOr(&parametersPassedNewObjects[call.function().index()], 1u << k, std::memory_order_relaxed);
            }
        });
    });
    Vector<Function> bodies;
    for (unsigned i = 0; i < count; ++i) {
        if (!parametersPassedNewObjects[i])
            continue;
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty() || !function.isTyped() || (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method))
            continue;
        if (function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionIsBuiltin | Format::FunctionIsSloppy | Format::FunctionHasRestParameter | Format::FunctionIsHiddenInStackTraces | FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer | FunctionLogsTypeMismatches))
            continue;
        if (function.declaredAs || function.resumeFunction || function.startFunction || function.savesFrameOnSuspend || function.worker || !function.resultTypes.isEmpty() || function.constructedClass || function.isValueEntry || function.takesCalleeClosure || function.rampForClosures || function.isRampForClosures || function.untypedClone || function.declaredGenericEntrypoint || function.isSharedOutlinedBody || function.isGuardedPreludeEntry || program.isResumeFunction(function))
            continue;
        auto& entry = function.blocks[0];
        unsigned first = function.captureTypes.size() + function.has(Format::FunctionHasThis);
        if (entry.parameters.size() != first + function.parameterTypes.size())
            continue;

        ValueID test;
        unsigned thrower = 0;
        unsigned reader = 0;
        if (entry.terminator.kind == TerminatorKind::Branch && entry.values.size() == 1 && entry.terminator.value == entry.values[0] && function.at(entry.values[0]).opcode == Opcode::IsUndefinedOrNull) {
            test = entry.values[0];
            thrower = entry.terminator.targets[0].block.index();
            reader = entry.terminator.targets[1].block.index();
            if (!thrower || !reader || thrower == reader || !entry.terminator.targets[0].arguments.isEmpty() || !entry.terminator.targets[1].arguments.isEmpty())
                continue;
            if (function.blocks[thrower].terminator.kind != TerminatorKind::Throw || !function.blocks[thrower].terminator.targets.isEmpty())
                continue;
        }
        auto& reading = function.blocks[reader];
        if (reading.values.isEmpty() || function.at(reading.values[0]).opcode != Opcode::GetById)
            continue;
        ValueID parameter = function.at(reading.values[0]).child(0);
        if (test && function.at(test).child(0) != parameter)
            continue;
        size_t index = entry.parameters.find(parameter);
        if (index == notFound || index < first || index >= 32 || !(parametersPassedNewObjects[i] & 1u << index))
            continue;
        Vector<ValueID, 8> reads;
        for (ValueID id : reading.values) {
            auto& read = function.at(id);
            if (read.opcode != Opcode::GetById || read.child(0) != parameter || read.immediates[1] != Format::none || !fieldOf(program, program.typeOf(function, parameter), read.string()))
                break;
            reads.append(id);
        }
        if (reads.isEmpty() || entry.parameters.size() - 1 + reads.size() > maxBodyParameters)
            continue;

        unsigned uses = 0;
        bool isEnteredAgain = false;
        BitVector belongsToThrower(function.values.size());
        if (test) {
            for (ValueID id : function.blocks[thrower].values)
                belongsToThrower.set(id.index());
        }
        bool throwerIsStandalone = true;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            auto use = [&](ValueID child) {
                uses += child == parameter;
                if (test && b == thrower)
                    throwerIsStandalone &= belongsToThrower.get(child.index());
            };
            for (ValueID id : block.values)
                function.at(id).forEachChild(use);
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
                unsigned to = target.block.index();
                isEnteredAgain |= !to || (b && test && (to == thrower || to == reader));
            }
        }
        if (uses != reads.size() + !!test || isEnteredAgain || !throwerIsStandalone || !function.blocks[reader].parameters.isEmpty() || (test && !function.blocks[thrower].parameters.isEmpty()))
            continue;

        FunctionID bodyID(count + bodies.size());
        unsigned declared = index - first;
        Function body = function;
        body.flags &= ~(FunctionHasUnknownCallers | FunctionIsEnteredByValue);
        body.hasManyClosures = false;
        body.closuresHaveOwnProperties = false;
        body.isUsedAsObject = false;
        {
            Block start = std::exchange(body.blocks[reader], Block { });
            start.values.removeAt(0, reads.size());
            start.parameters = { };
            for (ValueID id : entry.parameters) {
                if (id != parameter)
                    start.parameters.append(id);
            }
            for (ValueID id : reads)
                start.parameters.append(id);
            if (test)
                body.blocks[thrower] = Block { };
            body.blocks[0] = WTF::move(start);
            for (unsigned k = 0; k < body.blocks[0].parameters.size(); ++k) {
                auto& value = body.at(body.blocks[0].parameters[k]);
                if (value.opcode != Opcode::Parameter) {
                    Value taken;
                    taken.opcode = Opcode::Parameter;
                    taken.position = value.position;
                    taken.type = value.type;
                    taken.rep = value.rep;
                    taken.copyPredictionFrom(value);
                    value = WTF::move(taken);
                }
                value.immediates = { 0, k };
            }
            for (ValueID id : { parameter, test }) {
                if (!id)
                    continue;
                Value nothing;
                nothing.position = body.at(id).position;
                nothing.type = program.types.undefined;
                nothing.rep = repOf(program.at(nothing.type));
                body.at(id) = WTF::move(nothing);
            }
            body.parameterTypes.removeAt(declared);
            for (ValueID id : reads)
                body.parameterTypes.append(function.at(id).type);
            BitVector annotated;
            for (unsigned k : function.annotatedParameters) {
                if (k != declared)
                    annotated.set(k < declared ? k : k - 1);
            }
            body.annotatedParameters = WTF::move(annotated);
            if (declared < body.assertedParameterTypes.size())
                body.assertedParameterTypes.removeAt(declared);
        }

        Function wrapper = function;
        wrapper.onlyDestructuresParameter = true;
        wrapper.blocks.clear();
        wrapper.values.clear();
        wrapper.inlineFrames.clear();
        wrapper.allocations.clear();
        wrapper.allocationNotes.clear();
        wrapper.allocationNoteIndices.clear();
        auto add = [&](Value&& value) {
            wrapper.values.append(WTF::move(value));
            return ValueID(wrapper.values.size() - 1);
        };
        wrapper.blocks.append(Block { });
        for (ValueID id : entry.parameters)
            wrapper.blocks[0].parameters.append(add(Value(function.at(id))));
        ValueID taken = wrapper.blocks[0].parameters[index];
        unsigned last = 0;
        if (test) {
            Value copied(function.at(test));
            copied.children[0] = taken;
            wrapper.blocks[0].values.append(add(WTF::move(copied)));
            wrapper.blocks[0].terminator.kind = TerminatorKind::Branch;
            wrapper.blocks[0].terminator.value = wrapper.blocks[0].values[0];
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(1u), { } });
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(2u), { } });
            wrapper.blocks.append(Block { });
            wrapper.blocks.append(Block { });
            UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> copies;
            auto mapped = [&](ValueID& child) {
                child = copies.get(child.index());
            };
            for (ValueID id : function.blocks[thrower].values) {
                Value thrown(function.at(id));
                thrown.forEachChild(mapped);
                ValueID created = add(WTF::move(thrown));
                copies.add(id.index(), created);
                wrapper.blocks[1].values.append(created);
            }
            wrapper.blocks[1].terminator = function.blocks[thrower].terminator;
            wrapper.blocks[1].terminator.forEachChild(mapped);
            last = 2;
        }
        Value call;
        call.opcode = Opcode::CallDirect;
        call.position = function.at(reads[0]).position;
        call.type = function.resultType ? function.resultType : program.types.anything;
        call.rep = repOf(program.at(call.type));
        call.immediates = { bodyID.index(), Format::none };
        call.isInTailPosition = true;
        for (unsigned k = 0; k < entry.parameters.size(); ++k) {
            if (k != index)
                call.arguments.append(wrapper.blocks[0].parameters[k]);
        }
        for (ValueID id : reads) {
            Value read(function.at(id));
            read.children[0] = taken;
            call.arguments.append(add(WTF::move(read)));
            wrapper.blocks[last].values.append(call.arguments.last());
        }
        wrapper.blocks[last].terminator.kind = TerminatorKind::Return;
        wrapper.blocks[last].terminator.value = add(WTF::move(call));
        wrapper.blocks[last].values.append(wrapper.blocks[last].terminator.value);

        program.numberOfDirectCalls.append(program.numberOfDirectCalls[i] + 1);
        if (program.staticallyHotFunctions.get(i))
            program.staticallyHotFunctions.set(bodyID.index());
        if (program.staticallyWarmFunctions.get(i))
            program.staticallyWarmFunctions.set(bodyID.index());
        if (program.functionsHotOnlyInLoops.get(i))
            program.functionsHotOnlyInLoops.set(bodyID.index());
        if (program.functionsInCycles.get(i))
            program.functionsInCycles.set(bodyID.index());
        if (program.functionsWithInliningDisabled.get(i))
            program.functionsWithInliningDisabled.set(bodyID.index());
        if (program.callGraphColdFunctions.get(i))
            program.callGraphColdFunctions.set(bodyID.index());
        program.functions[i] = WTF::move(wrapper);
        bodies.append(WTF::move(body));
    }
    program.functions.appendVector(WTF::move(bodies));
}

static bool makeForwardedResultWhereReturned(const Program& program, Function& function, ValueID call)
{
    Value site(function.at(call));
    auto& wrapper = program.at(site.function());
    auto& body = wrapper.blocks[0];
    ValueID made = body.terminator.value;
    size_t numberOfValuesBeforeLiteral = body.values.find(made);
    auto sourceOf = [&](StringID name) {
        for (ValueID id : body.values) {
            auto& value = wrapper.at(id);
            if (value.opcode == Opcode::PutField && value.child(0) == made && program.at(value.layout()).fields[value.field()].name == name)
                return value.child(1);
            if (definesNamedFieldOf(program, wrapper, value, made) && wrapper.at(value.arguments[1]).string() == name)
                return value.arguments[2];
        }
        return ValueID();
    };
    Vector<std::pair<ValueID, ValueID>, 4> reads;
    Vector<ValueID, 2> nullishTests;
    Vector<unsigned, 2> returningBlocks;
    unsigned home = UINT_MAX;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        if (block.terminator.value == call) {
            if (block.terminator.kind != TerminatorKind::Return)
                return false;
            returningBlocks.append(b);
        }
        for (auto& target : block.terminator.targets) {
            if (target.arguments.contains(call))
                return false;
        }
        for (ValueID user : block.values) {
            if (user == call) {
                home = b;
                continue;
            }
            auto& value = std::as_const(function).at(user);
            bool uses = false;
            value.forEachChild([&](ValueID child) {
                uses |= child == call;
            });
            if (!uses)
                continue;
            if (value.opcode == Opcode::IsUndefinedOrNull) {
                nullishTests.append(user);
                continue;
            }
            StringID name;
            if (value.opcode == Opcode::GetField && isObjectLayout(program.at(value.layout()).kind))
                name = program.at(value.layout()).fields[value.field()].name;
            else if (value.opcode == Opcode::GetById && !value.methodGetByIdBuiltin())
                name = value.string();
            ValueID source = name ? sourceOf(name) : ValueID();
            if (!source || !program.at(wrapper.at(source).type).isSubsetOf(program.at(value.type), program))
                return false;
            reads.append({ user, source });
        }
    }
    if (home == UINT_MAX || numberOfValuesBeforeLiteral == notFound || (function.blocks[home].terminator.kind == TerminatorKind::Invoke && function.blocks[home].values.last() == call))
        return false;
    Vector<ValueID> copies(wrapper.values.size());
    for (unsigned k = 0; k < site.arguments.size(); ++k)
        copies[body.parameters[k].index()] = site.arguments[k];
    auto copyOf = [&](ValueID id) {
        Value copy(wrapper.at(id));
        copy.position = site.position;
        copy.inlineFrame = site.inlineFrame;
        copy.isInTailPosition = copy.opcode == Opcode::CallDirect && site.isInTailPosition;
        copy.forEachChild([&](ValueID& child) {
            child = copies[child.index()];
        });
        if (copy.opcode == Opcode::NewObjectWithShape) {
            copy.immediates[1] = function.allocations.size();
            function.allocations.append(Allocation(wrapper.allocations[wrapper.at(id).allocation()]));
        }
        copies[id.index()] = ValueID(function.values.size());
        function.values.append(WTF::move(copy));
        return copies[id.index()];
    };
    size_t at = function.blocks[home].values.find(call);
    function.blocks[home].values.removeAt(at);
    for (size_t k = 0; k < numberOfValuesBeforeLiteral; ++k)
        function.blocks[home].values.insert(at + k, copyOf(body.values[k]));
    for (auto [read, source] : reads) {
        auto& value = function.at(read);
        value.opcode = Opcode::Cast;
        value.children = { copies[source.index()], ValueID(), ValueID() };
        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
    }
    for (ValueID test : nullishTests) {
        auto& value = function.at(test);
        value.opcode = Opcode::Boolean;
        value.children = { };
        value.immediates = { 0, Format::none };
    }
    for (unsigned b : returningBlocks) {
        for (size_t k = numberOfValuesBeforeLiteral; k < body.values.size(); ++k)
            function.blocks[b].values.append(copyOf(body.values[k]));
        function.blocks[b].terminator.value = copies[made.index()];
    }
    return true;
}

static BitVector makeForwardedResultsFresh(Program& program)
{
    unsigned count = program.functions.size();
    static constexpr OpcodeSet directCalls { Opcode::CallDirect };
    Vector<std::unique_ptr<Function>> changed(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (function.worker || !function.resultTypes.isEmpty() || function.blocks.isEmpty() || program.functionsInCycles.get(i) || !program.functions.mayContain(i, directCalls))
            return;
        Vector<ValueID, 4> forwardedCalls;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return || !block.terminator.value || function.at(block.terminator.value).opcode != Opcode::CallDirect || forwardedCalls.contains(block.terminator.value))
                continue;
            auto& call = function.at(block.terminator.value);
            if (call.isJumpToContinuation())
                return;
            auto& wrapper = program.at(call.function());
            if (!wrapper.worker || wrapper.nullishResultKind || wrapper.blocks.size() != 1 || wrapper.blocks[0].parameters.size() != call.arguments.size())
                return;
            forwardedCalls.append(block.terminator.value);
        }
        if (forwardedCalls.isEmpty())
            return;
        Function copy = function;
        for (ValueID call : forwardedCalls) {
            if (!makeForwardedResultWhereReturned(program, copy, call))
                return;
        }
        if (computeFreshResults(program, copy))
            changed[i] = makeUniqueWithoutFastMallocCheck<Function>(WTF::move(copy));
    });
    BitVector result(count);
    for (unsigned i = 0; i < count; ++i) {
        if (!changed[i])
            continue;
        program.functions[i] = WTF::move(*changed[i]);
        result.set(i);
    }
    return result;
}

static void splitOffWorkers(Program& program, const BitVector* considered)
{
    unsigned count = program.functions.size();
    program.types.thaw();
    Vector<std::unique_ptr<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if ((considered && !considered->get(i)) || !program.functions.mayContain(i, freshResultAllocations))
            return;
        if (auto fresh = computeFreshResults(program, std::as_const(program).functions[i]))
            freshResults[i] = makeUniqueWithoutFastMallocCheck<FreshResults>(WTF::move(*fresh));
    });
    Vector<uint8_t> hasCallThatOnlyReads(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        auto& function = std::as_const(program).functions[i];
        Vector<ValueID, 8> calls;
        std::as_const(program).functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
            if (auto& fresh = freshResults[function.at(id).function().index()]; fresh && !fresh->names.isEmpty() && !function.at(id).isJumpToContinuation())
                calls.append(id);
        });
        if (calls.isEmpty())
            return;
        BitVector isKept(function.values.size());
        for (auto& block : function.blocks) {
            for (ValueID user : block.values) {
                auto& value = function.at(user);
                bool reads = value.opcode == Opcode::IsUndefinedOrNull || value.opcode == Opcode::Cast || (value.opcode == Opcode::GetById && !value.methodGetByIdBuiltin());
                if (reads)
                    continue;
                value.forEachChild([&](ValueID child) {
                    isKept.set(child.index());
                });
            }
        }
        for (ValueID call : calls) {
            if (!isKept.get(call.index()))
                WTF::atomicStore(&hasCallThatOnlyReads[function.at(call).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
    });
    Vector<unsigned> functionsSplit;
    for (unsigned i = 0; i < count; ++i) {
        auto& fresh = freshResults[i];
        if (!fresh || (!fresh->names.isEmpty() && !hasCallThatOnlyReads[i]))
            continue;
        auto& function = std::as_const(program).functions[i];
        for (unsigned k = 0; k < fresh->types.size(); ++k) {
            Type type = program.at(function.at(fresh->returns[0].second[k]).type);
            for (auto& entry : fresh->returns)
                type = unionOf(type, program.at(function.at(entry.second[k]).type));
            TypeID joined = program.types.intern(type);
            if (repOf(program.at(joined)) != Rep::Void && program.at(joined).isSubsetOf(program.at(fresh->types[k]), program))
                fresh->types[k] = joined;
        }
        if (fitInRegisters(program, fresh->types, fresh->nullishKind))
            functionsSplit.append(i);
    }
    program.functions.grow(count + functionsSplit.size());
    forEachIndexInParallel(functionsSplit.size(), [&](unsigned numberOfSplit) {
        unsigned i = functionsSplit[numberOfSplit];
        auto& fresh = freshResults[i];
        FunctionID workerID(count + numberOfSplit);
        Function worker = std::as_const(program).functions[i];

        Function wrapper = worker;
        wrapper.blocks.clear();
        wrapper.values.clear();
        wrapper.inlineFrames.clear();
        wrapper.allocations.clear();
        wrapper.allocationNotes.clear();
        wrapper.flags |= Format::FunctionIsHiddenInStackTraces;
        wrapper.worker = workerID;
        wrapper.blocks.append(Block { });
        auto add = [&](Value&& value) {
            wrapper.values.append(WTF::move(value));
            return ValueID(wrapper.values.size() - 1);
        };
        unsigned building = 0;
        auto append = [&](Value&& value) {
            ValueID id = add(WTF::move(value));
            wrapper.blocks[building].values.append(id);
            return id;
        };
        auto& first = worker.blocks[fresh->returns[0].first];
        Value created(worker.at(first.terminator.value));
        Value call;
        call.opcode = Opcode::CallDirect;
        call.position = created.position;
        call.type = program.types.undefined;
        call.rep = repOf(program.at(call.type));
        call.immediates = { workerID.index(), Format::none };
        for (ValueID parameter : worker.blocks[0].parameters) {
            call.arguments.append(add(Value(worker.at(parameter))));
            wrapper.blocks[0].parameters.append(call.arguments.last());
        }
        ValueID callID = append(WTF::move(call));
        if (fresh->nullishKind) {
            Value presenceCheck;
            presenceCheck.opcode = Opcode::ResultAt;
            presenceCheck.position = created.position;
            presenceCheck.type = program.types.boolean;
            presenceCheck.rep = repOf(program.at(presenceCheck.type));
            presenceCheck.children = { callID, ValueID(), ValueID() };
            presenceCheck.immediates = { 0, Format::none };
            wrapper.blocks[0].terminator.kind = TerminatorKind::Branch;
            wrapper.blocks[0].terminator.value = append(WTF::move(presenceCheck));
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(1u), { } });
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(2u), { } });
            wrapper.blocks.append(Block { });
            wrapper.blocks.append(Block { });
            Value nothing;
            nothing.opcode = fresh->nullishKind == 1 ? Opcode::Undefined : Opcode::Null;
            nothing.position = created.position;
            ValueID returned = worker.blocks[fresh->nullishReturns[0]].terminator.value;
            nothing.type = returned ? worker.at(returned).type : program.types.undefined;
            nothing.rep = repOf(program.at(nothing.type));
            wrapper.blocks[2].terminator.kind = TerminatorKind::Return;
            wrapper.blocks[2].terminator.value = add(WTF::move(nothing));
            wrapper.blocks[2].values.append(wrapper.blocks[2].terminator.value);
            wrapper.nullishResultKind = fresh->nullishKind;
            building = 1;
        }
        Arguments held;
        for (unsigned k = 0; k < fresh->types.size(); ++k) {
            const Value& text = worker.at(fresh->returns[0].second[k]);
            bool isSingleString = !fresh->isArray && text.opcode == Opcode::String;
            for (auto& entry : fresh->returns)
                isSingleString = isSingleString && worker.at(entry.second[k]).opcode == Opcode::String && worker.at(entry.second[k]).string() == text.string();
            if (isSingleString) {
                Value constant(text);
                constant.position = created.position;
                held.append(append(WTF::move(constant)));
                continue;
            }
            Value result;
            result.opcode = Opcode::ResultAt;
            result.position = created.position;
            result.type = fresh->types[k];
            result.rep = repOf(program.at(result.type));
            result.children = { callID, ValueID(), ValueID() };
            result.immediates = { k + !!fresh->nullishKind, Format::none };
            held.append(append(WTF::move(result)));
        }
        if (fresh->isArray)
            created.arguments = held;
        else if (fresh->layout) {
            wrapper.allocations.append(Allocation(worker.allocations[created.allocation()]));
            created.immediates[1] = 0;
        }
        ValueID object = append(WTF::move(created));
        if (fresh->layout) {
            for (ValueID id : first.values) {
                if (worker.at(id).opcode != Opcode::PutField || worker.at(id).child(0) != first.terminator.value)
                    continue;
                Value store(worker.at(id));
                store.children[0] = object;
                store.children[1] = held[store.field()];
                append(WTF::move(store));
            }
        }
        if (!fresh->names.isEmpty()) {
            unsigned next = 0;
            for (ValueID id : first.values) {
                if (!definesNamedFieldOf(program, worker, worker.at(id), first.terminator.value))
                    continue;
                Value definition(worker.at(id));
                definition.arguments[0] = object;
                definition.arguments[1] = append(Value(worker.at(definition.arguments[1])));
                definition.arguments[2] = held[next++];
                append(WTF::move(definition));
            }
        }
        wrapper.blocks[building].terminator.kind = TerminatorKind::Return;
        wrapper.blocks[building].terminator.value = object;

        for (auto& [b, operands] : fresh->returns) {
            auto& block = worker.blocks[b];
            ValueID literal = block.terminator.value;
            block.values.removeAllMatching([&](ValueID id) {
                return id == literal || initializesLiteral(program, worker, id, literal);
            });
            Value results;
            results.opcode = Opcode::Results;
            results.position = worker.at(literal).position;
            results.type = program.types.undefined;
            results.rep = repOf(program.at(results.type));
            if (fresh->nullishKind) {
                Value presenceCheck;
                presenceCheck.opcode = Opcode::Boolean;
                presenceCheck.position = results.position;
                presenceCheck.type = program.types.boolean;
                presenceCheck.rep = repOf(program.at(presenceCheck.type));
                presenceCheck.immediates = { 1, Format::none };
                results.arguments.append(ValueID(worker.values.size()));
                block.values.append(results.arguments.last());
                worker.values.append(WTF::move(presenceCheck));
            }
            results.arguments.appendRange(operands.begin(), operands.end());
            block.terminator.value = ValueID(worker.values.size());
            block.values.append(block.terminator.value);
            worker.values.append(WTF::move(results));
        }
        for (unsigned b : fresh->nullishReturns) {
            auto& block = worker.blocks[b];
            Value results;
            results.opcode = Opcode::Results;
            results.position = block.terminator.value ? worker.at(block.terminator.value).position : worker.position;
            results.type = program.types.undefined;
            results.rep = repOf(program.at(results.type));
            for (unsigned k = 0; k <= fresh->types.size(); ++k) {
                Value operand;
                operand.opcode = k ? Opcode::Unspecified : Opcode::Boolean;
                operand.position = results.position;
                operand.type = k ? fresh->types[k - 1] : program.types.boolean;
                operand.rep = repOf(program.at(operand.type));
                if (!k)
                    operand.immediates = { 0, Format::none };
                results.arguments.append(ValueID(worker.values.size()));
                block.values.append(results.arguments.last());
                worker.values.append(WTF::move(operand));
            }
            block.terminator.value = ValueID(worker.values.size());
            block.values.append(block.terminator.value);
            worker.values.append(WTF::move(results));
        }
        worker.resultType = program.types.undefined;
        worker.resultTypes = fresh->types;
        if (fresh->nullishKind)
            worker.resultTypes.insert(0, program.types.boolean);

        program.functions[i] = WTF::move(wrapper);
        program.functions[workerID.index()] = WTF::move(worker);
    });
    for (unsigned numberOfSplit = 0; numberOfSplit < functionsSplit.size(); ++numberOfSplit) {
        unsigned i = functionsSplit[numberOfSplit];
        FunctionID workerID(count + numberOfSplit);
        program.numberOfDirectCalls.append(program.numberOfDirectCalls[i] + 1);
        if (program.staticallyHotFunctions.get(i))
            program.staticallyHotFunctions.set(workerID.index());
        if (program.staticallyWarmFunctions.get(i))
            program.staticallyWarmFunctions.set(workerID.index());
        if (program.functionsHotOnlyInLoops.get(i))
            program.functionsHotOnlyInLoops.set(workerID.index());
        if (program.functionsInCycles.get(i))
            program.functionsInCycles.set(workerID.index());
        if (program.functionsWithInliningDisabled.get(i))
            program.functionsWithInliningDisabled.set(workerID.index());
        if (program.callGraphColdFunctions.get(i))
            program.callGraphColdFunctions.set(workerID.index());
    }
    program.types.freeze();
}

static void splitOffWorkers(Program& program)
{
    splitOffWorkers(program, nullptr);
    constexpr unsigned maxForwardingDepth = 4;
    unsigned numberOfForwarders = 0;
    for (unsigned depth = 0; depth < maxForwardingDepth; ++depth) {
        BitVector forwarders = makeForwardedResultsFresh(program);
        if (forwarders.isEmpty())
            break;
        numberOfForwarders += forwarders.bitCount();
        splitOffWorkers(program, &forwarders);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfForwarders, " functions that forward a fresh result return it in registers");
}

static void outlineHostObjectReads(Program& program)
{
    struct Reader {
        TypeID hostObjectType;
        TypeID assertedHostObjectType;
        uint32_t flags;
        unsigned created;
    };
    constexpr uint32_t sourceFlags = Format::FunctionIsTyped | Format::FunctionIsInBuiltinModule | Format::FunctionIsSloppy;
    unsigned count = program.functions.size();
    UncheckedKeyHashMap<uint64_t, Vector<Reader, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> readers;
    Vector<Function> created;
    Vector<uint32_t> callCounts;
    Vector<uint32_t> useCounts;
    Vector<uint8_t> readsHostObject(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet globalReads { Opcode::GetGlobal };
        if (!program.functions.mayContain(i, globalReads))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin))
            return;
        std::as_const(program).functions.forEachIndexedValueWithBlock(i, globalReads, [&](unsigned block, unsigned k, ValueID id) {
            auto& values = function.blocks[block].values;
            if (k + 1 == values.size())
                return;
            auto& read = function.at(values[k + 1]);
            if (read.opcode == Opcode::GetById && program.isHostObjectDataProperty(function.at(id).string(), read.string()))
                readsHostObject[i] = 1;
        });
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!readsHostObject[i])
            continue;
        auto& function = program.functions[i];
        bool hasCountedUses = false;
        for (auto& block : function.blocks) {
            for (unsigned k = 1; k < block.values.size(); ++k) {
                ValueID base = block.values[k - 1];
                auto& read = function.at(block.values[k]);
                auto& global = function.at(base);
                if (read.opcode != Opcode::GetById || read.child(0) != base || read.immediates[1] != Format::none || global.opcode != Opcode::GetGlobal)
                    continue;
                if (!program.isHostObjectDataProperty(global.string(), read.string()))
                    continue;
                if (!std::exchange(hasCountedUses, true)) {
                    useCounts.fill(0, function.values.size());
                    auto use = [&](ValueID child) { ++useCounts[child.index()]; };
                    for (auto& value : function.values)
                        std::as_const(value).forEachChild(use);
                    for (auto& other : function.blocks)
                        other.terminator.forEachChild(use);
                }
                if (useCounts[base.index()] != 1)
                    continue;
                uint32_t flags = function.flags & sourceFlags;
                auto& pairReaders = readers.add(static_cast<uint64_t>(global.string().index()) << 32 | read.string().index(), Vector<Reader, 2> { }).iterator->value;
                size_t found = pairReaders.findIf([&](auto& reader) { return reader.hostObjectType == global.type && reader.assertedHostObjectType == global.assertedType() && reader.flags == flags; });
                if (found == notFound) {
                    found = pairReaders.size();
                    pairReaders.append(Reader { global.type, global.assertedType(), flags, static_cast<unsigned>(created.size()) });
                    Function reader;
                    reader.name = read.string();
                    reader.module = function.module;
                    reader.position = read.position;
                    reader.flags = flags | Format::FunctionIsHiddenInStackTraces;
                    reader.readsHostObjectMember = true;
                    reader.isSharedOutlinedBody = true;
                    reader.resultType = program.types.anything;
                    reader.blocks.append(Block { });
                    reader.blocks[0].terminator.kind = TerminatorKind::Return;
                    Function general = reader;
                    Value returnedValue(read);
                    returnedValue.type = program.types.anything;
                    returnedValue.rep = repOf(program.at(returnedValue.type));
                    returnedValue.setAssertedType(TypeID());
                    returnedValue.copyPredictionFrom(Value { });
                    general.values.append(Value(global));
                    general.values.append(Value(returnedValue));
                    general.values[1].children[0] = ValueID(0u);
                    general.blocks[0].values.append(ValueID(0u));
                    general.blocks[0].values.append(ValueID(1u));
                    general.blocks[0].terminator.value = ValueID(1u);
                    returnedValue.opcode = Opcode::CallDirect;
                    returnedValue.children = { };
                    returnedValue.immediates = { count + static_cast<unsigned>(created.size()) + 1, Format::none };
                    reader.cachedHostObjectName = global.string();
                    reader.genericHostObjectReader = FunctionID(count + static_cast<unsigned>(created.size()) + 1);
                    reader.values.append(WTF::move(returnedValue));
                    reader.blocks[0].values.append(ValueID(0u));
                    reader.blocks[0].terminator.value = ValueID(0u);
                    created.append(WTF::move(reader));
                    created.append(WTF::move(general));
                    callCounts.append(0);
                    callCounts.append(1);
                }
                unsigned number = pairReaders[found].created;
                ++callCounts[number];
                read.opcode = Opcode::CallDirect;
                read.children = { };
                read.immediates = { count + number, Format::none };
                block.values.removeAt(k - 1);
                --k;
            }
        }
    }
    for (unsigned number = 0; number < created.size(); ++number) {
        program.numberOfDirectCalls.append(callCounts[number]);
        program.staticallyHotFunctions.set(count + number);
        program.staticallyWarmFunctions.set(count + number);
        program.functions.append(WTF::move(created[number]));
    }
}

static void outlineGetStoreCalls(Program& program)
{
    auto getStore = program.find(asBytes("getStore"_s.span8()));
    if (!getStore)
        return;
    constexpr uint32_t sourceFlags = Format::FunctionIsTyped | Format::FunctionIsInBuiltinModule | Format::FunctionIsSloppy;
    static constexpr OpcodeSet methodCalls { Opcode::CallMethod };
    unsigned count = program.functions.size();
    auto& functions = std::as_const(program).functions;
    Vector<Vector<ValueID, 0>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!functions.mayContain(i, methodCalls))
            return;
        auto& function = functions[i];
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin))
            return;
        functions.forEachIndexedValue(i, methodCalls, [&](ValueID id) {
            auto& call = function.at(id);
            if (call.string() == *getStore && call.arguments.isEmpty() && !call.isSignatureCall && !program.at(function.at(call.child(0)).type).singleLayout())
                sites[i].append(id);
        });
    });
    struct Reader {
        uint32_t flags;
        uint32_t numberOfCalls;
    };
    Vector<Reader, 4> readers;
    Vector<Function> created;
    Vector<uint32_t> calledReader(FillWith { }, count, static_cast<uint32_t>(Format::none));
    for (unsigned i = 0; i < count; ++i) {
        if (sites[i].isEmpty())
            continue;
        auto& function = functions[i];
        uint32_t flags = function.flags & sourceFlags;
        size_t found = readers.findIf([&](auto& reader) { return reader.flags == flags; });
        if (found == notFound) {
            found = readers.size();
            readers.append(Reader { flags, 0 });
            auto& site = function.at(sites[i][0]);
            uint32_t genericReaderIndex = count + 2 * static_cast<uint32_t>(found) + 1;
            Function reader;
            reader.name = *getStore;
            reader.module = function.module;
            reader.position = site.position;
            reader.flags = flags | Format::FunctionIsHiddenInStackTraces;
            reader.readsAsyncLocalStorage = true;
            reader.isSharedOutlinedBody = true;
            reader.resultType = program.types.anything;
            reader.parameterTypes.append(program.types.anything);
            reader.length = 1;
            reader.blocks.append(Block { });
            reader.blocks[0].parameters.append(ValueID(0u));
            reader.blocks[0].values.append(ValueID(1u));
            reader.blocks[0].terminator.kind = TerminatorKind::Return;
            reader.blocks[0].terminator.value = ValueID(1u);
            Value storage;
            storage.opcode = Opcode::Parameter;
            storage.position = site.position;
            storage.type = program.types.anything;
            storage.rep = repOf(program.at(storage.type));
            storage.immediates = { 0, 0 };
            reader.values.append(WTF::move(storage));
            Function general = reader;
            Value call;
            call.opcode = Opcode::CallMethod;
            call.position = site.position;
            call.type = program.types.anything;
            call.rep = repOf(program.at(call.type));
            call.immediates = site.immediates;
            call.children[0] = ValueID(0u);
            general.values.append(Value(call));
            call.opcode = Opcode::CallDirect;
            call.children = { };
            call.immediates = { genericReaderIndex, Format::none };
            call.arguments.append(ValueID(0u));
            reader.genericAsyncLocalStorageReader = FunctionID(genericReaderIndex);
            reader.values.append(WTF::move(call));
            created.append(WTF::move(reader));
            created.append(WTF::move(general));
        }
        readers[found].numberOfCalls += sites[i].size();
        calledReader[i] = count + 2 * static_cast<uint32_t>(found);
    }
    if (created.isEmpty())
        return;
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        for (ValueID id : sites[i]) {
            auto& call = function.at(id);
            Arguments passed;
            passed.append(call.child(0));
            call.opcode = Opcode::CallDirect;
            call.isInTailPosition = false;
            call.setCallSignature(Format::none);
            call.children = { };
            call.immediates = { calledReader[i], Format::none };
            call.arguments = WTF::move(passed);
        }
    });
    for (unsigned number = 0; number < created.size(); ++number) {
        program.numberOfDirectCalls.append(number & 1 ? 1 : readers[number / 2].numberOfCalls);
        program.staticallyHotFunctions.set(count + number);
        program.staticallyWarmFunctions.set(count + number);
        program.functions.append(WTF::move(created[number]));
    }
}

static void shareCallsOfFunctionFields(Program& program)
{
    constexpr uint32_t sourceFlags = Format::FunctionIsTyped | Format::FunctionIsInBuiltinModule | Format::FunctionIsSloppy;
    constexpr unsigned maxArguments = 5;
    static constexpr OpcodeSet methodCalls { Opcode::CallMethod };
    unsigned count = program.functions.size();
    auto& functions = std::as_const(program).functions;
    auto onlyFunctionCalledBy = [&](const Function& function, const Value& call) -> FunctionID {
        auto access = fieldOf(program, program.typeOf(function, call.child(0)), call.string());
        if (!access)
            return { };
        FunctionID callee = std::as_const(program).at(access->layout).fields[access->field].singleFunction;
        if (!callee || !program.genericEntrypoints.contains(callee.index()))
            return { };
        auto& target = std::as_const(program).at(callee);
        if (target.blocks.isEmpty() || target.isGenericEntrypoint() || target.kind == FunctionKind::Constructor || target.kind == FunctionKind::Module)
            return { };
        return callee;
    };
    Vector<Vector<ValueID, 0>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!functions.mayContain(i, methodCalls))
            return;
        auto& function = functions[i];
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin))
            return;
        bool isWarm = program.isStaticallyWarm(FunctionID(i));
        functions.forEachIndexedValue(i, methodCalls, [&](ValueID id) {
            auto& call = function.at(id);
            if (call.isSignatureCall || call.arguments.size() > maxArguments || !onlyFunctionCalledBy(function, call))
                return;
            if (!isWarm || functionPinnedToField(program, program.typeOf(function, call.child(0)), call.string()))
                sites[i].append(id);
        });
    });
    struct Body {
        TypeID receiver;
        TypeID result;
        uint32_t signature;
        uint32_t flags;
        Vector<TypeID, 4> parameters;
        uint32_t numberOfCalls { 0 };
        uint32_t firstFunction;
        ValueID firstSite;
        uint32_t created { Format::none };
        uint32_t pinnedTest { Format::none };
        uint32_t numberOfColdCalls { 0 };
        bool hasWarmSite { false };
    };
    Vector<Body> bodies;
    UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> bodiesByReceiverAndName;
    Vector<Vector<uint32_t, 0>> bodyOfSite(count);
    auto& types = program.types;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = functions[i];
        for (ValueID id : sites[i]) {
            auto& call = function.at(id);
            auto& target = std::as_const(program).at(onlyFunctionCalledBy(function, call));
            Vector<TypeID, 4> parameters;
            for (unsigned k = 0; k < call.arguments.size(); ++k) {
                TypeID given = function.at(call.arguments[k]).type;
                auto& type = program.at(given);
                TypeID taken = given;
                for (TypeID wide : { types.number, types.string, types.boolean }) {
                    if (type.isSubsetOf(program.at(wide), program))
                        taken = wide;
                }
                bool isDeclared = !target.has(Format::FunctionHasRestParameter) && !target.takesCalleeClosure && k < target.parameterTypes.size() && target.parameterTypes[k];
                TypeID declared = isDeclared ? target.parameterTypes[k] : types.anything;
                if (type.isSubsetOf(program.at(declared), program))
                    taken = declared;
                parameters.append(taken);
            }
            TypeID receiver = function.at(call.child(0)).type;
            uint32_t flags = function.flags & sourceFlags;
            auto& candidates = bodiesByReceiverAndName.add(static_cast<uint64_t>(receiver.index()) << 32 | call.string().index(), Vector<uint32_t, 2> { }).iterator->value;
            size_t found = candidates.findIf([&](uint32_t number) {
                auto& body = bodies[number];
                return body.result == call.type && body.signature == call.callSignature() && body.flags == flags && body.parameters == parameters;
            });
            if (found == notFound) {
                found = candidates.size();
                candidates.append(bodies.size());
                bodies.append(Body { receiver, call.type, call.callSignature(), flags, WTF::move(parameters), 0, i, id, Format::none });
            }
            ++bodies[candidates[found]].numberOfCalls;
            bodies[candidates[found]].hasWarmSite |= program.isStaticallyWarm(FunctionID(i));
            bodies[candidates[found]].numberOfColdCalls += !program.isStaticallyWarm(FunctionID(i));
            bodyOfSite[i].append(candidates[found]);
        }
    }
    Vector<Function> created;
    for (auto& body : bodies) {
        if (body.numberOfColdCalls < 2)
            continue;
        auto& function = functions[body.firstFunction];
        auto& site = function.at(body.firstSite);
        Function shared;
        shared.name = site.string();
        shared.module = function.module;
        shared.position = site.position;
        shared.flags = body.flags | Format::FunctionIsHiddenInStackTraces;
        shared.isSharedCallBody = true;
        shared.resultType = body.result;
        shared.blocks.append(Block { });
        auto addParameter = [&](TypeID type) {
            ValueID id(shared.values.size());
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = site.position;
            parameter.type = type;
            parameter.rep = repOf(program.at(type));
            parameter.immediates = { 0, static_cast<uint32_t>(shared.parameterTypes.size()) };
            shared.parameterTypes.append(type);
            shared.blocks[0].parameters.append(id);
            shared.values.append(WTF::move(parameter));
            return id;
        };
        Value call(site);
        call.inlineFrame = 0;
        call.isInTailPosition = true;
        call.children = { addParameter(body.receiver), ValueID(), ValueID() };
        call.arguments.clear();
        for (TypeID type : body.parameters)
            call.arguments.append(addParameter(type));
        shared.length = shared.parameterTypes.size();
        shared.blocks[0].values.append(ValueID(shared.values.size()));
        shared.blocks[0].terminator.kind = TerminatorKind::Return;
        shared.blocks[0].terminator.value = ValueID(shared.values.size());
        shared.values.append(WTF::move(call));
        body.created = count + created.size();
        created.append(WTF::move(shared));
    }
    BitVector mayCaptureOnlyHolder;
    for (auto& body : bodies) {
        if (!body.hasWarmSite)
            continue;
        FunctionID callee = functionPinnedToField(program, program.at(body.receiver), functions[body.firstFunction].at(body.firstSite).string());
        if (callee && std::as_const(program).at(callee).captureTypes.size() == 1)
            mayCaptureOnlyHolder.set(callee.index());
    }
    if (!mayCaptureOnlyHolder.isEmpty()) {
        static constexpr OpcodeSet closures { Opcode::NewClosure, Opcode::NewClosureOnce };
        static constexpr OpcodeSet stores { Opcode::PutById, Opcode::PutField };
        Vector<Vector<uint32_t, 0>> capturingAnotherValue(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            if (!functions.mayContain(i, closures))
                return;
            auto& function = functions[i];
            auto withoutCasts = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            functions.forEachIndexedValue(i, stores, [&](ValueID id) {
                auto& store = function.at(id);
                auto& stored = function.at(withoutCasts(store.child(1)));
                if (!closures.contains(stored.opcode) || !mayCaptureOnlyHolder.get(stored.function().index()))
                    return;
                if (stored.arguments.size() != 1 || withoutCasts(stored.arguments[0]) != withoutCasts(store.child(0)))
                    capturingAnotherValue[i].append(stored.function().index());
            });
        });
        for (auto& found : capturingAnotherValue) {
            for (uint32_t index : found)
                mayCaptureOnlyHolder.clear(index);
        }
        for (size_t index : mayCaptureOnlyHolder)
            program.functions.atKeepingValueIndex(index).closuresCaptureOnlyTheirHolder = true;
    }
    for (auto& body : bodies) {
        if (!body.hasWarmSite)
            continue;
        auto& function = functions[body.firstFunction];
        auto& site = function.at(body.firstSite);
        if (!functionPinnedToField(program, program.at(body.receiver), site.string()))
            continue;
        auto access = fieldOf(program, program.at(body.receiver), site.string());
        program.layouts[access->layout.index()].fields[access->field].isPinnedToFunction = true;
        for (LayoutID subtype : std::as_const(program).at(access->layout).subtypes) {
            auto& layout = program.layouts[subtype.index()];
            layout.fields[*layout.fieldNamed(site.string())].isPinnedToFunction = true;
        }
        Function tests;
        tests.name = site.string();
        tests.module = function.module;
        tests.position = site.position;
        tests.flags = body.flags | Format::FunctionIsHiddenInStackTraces;
        tests.testsPinnedFunction = true;
        tests.resultType = body.result;
        tests.blocks.grow(3);
        auto addParameter = [&](TypeID type) {
            ValueID id(tests.values.size());
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = site.position;
            parameter.type = type;
            parameter.rep = repOf(program.at(type));
            parameter.immediates = { 0, static_cast<uint32_t>(tests.parameterTypes.size()) };
            tests.parameterTypes.append(type);
            tests.blocks[0].parameters.append(id);
            tests.values.append(WTF::move(parameter));
            return id;
        };
        Value call(site);
        call.inlineFrame = 0;
        call.isInTailPosition = true;
        call.children = { addParameter(body.receiver), ValueID(), ValueID() };
        call.arguments.clear();
        for (TypeID type : body.parameters)
            call.arguments.append(addParameter(type));
        tests.length = tests.parameterTypes.size();
        Value test;
        test.opcode = Opcode::IsClassObjectUnmodified;
        test.position = site.position;
        test.type = types.boolean;
        test.rep = repOf(program.at(types.boolean));
        test.immediates = { access->layout.index(), access->field };
        test.guardsFastPath = true;
        ValueID testID(tests.values.size());
        tests.values.append(WTF::move(test));
        tests.blocks[0].values.append(testID);
        tests.blocks[0].terminator.kind = TerminatorKind::Branch;
        tests.blocks[0].terminator.value = testID;
        tests.blocks[0].terminator.targets.append(Target { BlockID(1u), { } });
        tests.blocks[0].terminator.targets.append(Target { BlockID(2u), { } });
        ValueID direct(tests.values.size());
        tests.values.append(Value(call));
        tests.blocks[1].values.append(direct);
        tests.blocks[1].terminator.kind = TerminatorKind::Return;
        tests.blocks[1].terminator.value = direct;
        if (body.created != Format::none) {
            Arguments passed;
            passed.append(call.child(0));
            passed.append(call.arguments.span());
            call.opcode = Opcode::CallDirect;
            call.setCallSignature(Format::none);
            call.children = { };
            call.immediates = { body.created, Format::none };
            call.arguments = WTF::move(passed);
        }
        ValueID general(tests.values.size());
        tests.values.append(WTF::move(call));
        tests.blocks[2].values.append(general);
        tests.blocks[2].terminator.kind = TerminatorKind::Return;
        tests.blocks[2].terminator.value = general;
        body.pinnedTest = count + created.size();
        created.append(WTF::move(tests));
    }
    if (created.isEmpty())
        return;
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        for (unsigned k = 0; k < sites[i].size(); ++k) {
            auto& body = bodies[bodyOfSite[i][k]];
            uint32_t target = body.pinnedTest != Format::none && program.isStaticallyWarm(FunctionID(i)) ? body.pinnedTest : body.created;
            if (target == Format::none)
                continue;
            auto& call = function.at(sites[i][k]);
            Arguments passed;
            passed.append(call.child(0));
            passed.append(call.arguments.span());
            call.opcode = Opcode::CallDirect;
            call.setCallSignature(Format::none);
            call.children = { };
            call.immediates = { target, Format::none };
            call.arguments = WTF::move(passed);
        }
    });
    program.numberOfDirectCalls.grow(count + created.size());
    for (auto& body : bodies) {
        if (body.pinnedTest != Format::none)
            program.numberOfDirectCalls[body.pinnedTest] = body.numberOfCalls;
        if (body.created == Format::none)
            continue;
        program.numberOfDirectCalls[body.created] = body.numberOfCalls;
        program.staticallyHotFunctions.set(body.created);
        program.staticallyWarmFunctions.set(body.created);
    }
    program.functions.appendVector(WTF::move(created));
}

static void returnResultsInRegisters(Program& program)
{
    unsigned count = program.functions.size();
    struct Candidate : FreshResults {
        uint32_t worker { Format::none };
    };
    Vector<Candidate> found;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidates;
    Vector<uint8_t> isCalled(FillWith { }, count, static_cast<uint8_t>(0));
    auto& functions = std::as_const(program.functions);
    static constexpr OpcodeSet directCalls { Opcode::CallDirect };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
            if (!function.at(id).isJumpToContinuation())
                WTF::atomicStore(&isCalled[function.at(id).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    Vector<std::unique_ptr<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isCalled[i])
            return;
        if (auto fresh = computeFreshResults(program, functions[i]))
            freshResults[i] = makeUniqueWithoutFastMallocCheck<FreshResults>(WTF::move(*fresh));
    });
    for (unsigned i = 0; i < count; ++i) {
        if (auto& fresh = freshResults[i]; fresh && !fresh->nullishKind) {
            candidates.add(i, found.size());
            found.append(Candidate { WTF::move(*fresh) });
        }
    }
    if (candidates.isEmpty())
        return;

    Vector<Vector<ValueID, 4>> callsOf(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (!value.isJumpToContinuation() && candidates.contains(value.function().index()))
                callsOf[i].append(id);
        });
    });
    Vector<Function> workers;
    for (unsigned i = 0; i < count; ++i) {
        auto& calls = callsOf[i];
        if (calls.isEmpty())
            continue;
        auto& function = program.functions[i];
        BitVector removed(function.values.size());
        for (ValueID call : calls) {
            auto& candidate = found[candidates.get(function.at(call).function().index())];
            auto looked = computeFreshResultReads(program, function, call, candidate);
            if (!looked)
                continue;
            auto& [aliases, reads, nullishTests] = *looked;
            if (candidate.worker == Format::none) {
                uint32_t original = function.at(call).function().index();
                Function worker = functions[original];
                for (auto& [b, held] : candidate.returns) {
                    auto& block = worker.blocks[b];
                    ValueID created = block.terminator.value;
                    block.values.removeAllMatching([&](ValueID id) {
                        return id == created || initializesLiteral(program, worker, id, created);
                    });
                    Value results;
                    results.opcode = Opcode::Results;
                    results.position = worker.at(created).position;
                    results.inlineFrame = worker.at(created).inlineFrame;
                    results.type = program.types.undefined;
                    results.rep = repOf(program.at(results.type));
                    results.arguments.appendRange(held.begin(), held.end());
                    block.terminator.value = ValueID(worker.values.size());
                    block.values.append(block.terminator.value);
                    worker.values.append(WTF::move(results));
                }
                worker.resultType = program.types.undefined;
                worker.resultTypes = candidate.types;
                for (auto& note : worker.allocationNotes) {
                    note.left.clear();
                    note.forcedBy = { };
                }
                finalizeAllocationNotes(program, worker);
                candidate.worker = count + workers.size();
                if (program.isStaticallyHot(FunctionID(original)))
                    program.staticallyHotFunctions.set(candidate.worker);
                if (program.isStaticallyWarm(FunctionID(original)))
                    program.staticallyWarmFunctions.set(candidate.worker);
                if (program.functionsInCycles.get(original))
                    program.functionsInCycles.set(candidate.worker);
                workers.append(WTF::move(worker));
            }
            auto& value = function.at(call);
            value.immediates[0] = candidate.worker;
            value.type = program.types.undefined;
            value.rep = repOf(program.at(value.type));
            value.copyPredictionFrom(Value { });
            for (auto [read, index] : reads) {
                auto& result = function.at(read);
                result.opcode = Opcode::ResultAt;
                result.children = { call, ValueID(), ValueID() };
                result.immediates = { index, Format::none };
                result.arguments.clear();
                result.type = candidate.types[index];
                result.rep = repOf(program.at(result.type));
                result.copyPredictionFrom(Value { });
            }
            for (ValueID test : nullishTests) {
                auto& result = function.at(test);
                result.opcode = Opcode::Boolean;
                result.children = { };
                result.immediates = { 0, Format::none };
            }
            for (unsigned k = 1; k < aliases.size(); ++k)
                removed.set(aliases[k].index());
        }
        if (removed.isEmpty())
            continue;
        for (auto& block : function.blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                return removed.get(id.index());
            });
        }
    }
    program.functions.appendVector(WTF::move(workers));
}

static bool returnsOnlyResults(const Function& function)
{
    return std::ranges::all_of(function.blocks, [&](auto& block) {
        return block.terminator.kind != TerminatorKind::Return || (function.at(block.terminator.value).opcode == Opcode::Results && function.at(block.terminator.value).arguments.size() == function.resultTypes.size());
    });
}

static void keepOnlyResults(const Program& program, Function& function, uint32_t keptResults)
{
    unsigned resultCount = function.resultTypes.size();
    auto keep = [&](auto& list) {
        unsigned kept = 0;
        for (unsigned k = 0; k < resultCount; ++k) {
            if (keptResults >> k & 1)
                list[kept++] = list[k];
        }
        list.shrink(kept);
    };
    for (auto& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return && function.at(block.terminator.value).arguments.size() == resultCount)
            keep(function.at(block.terminator.value).arguments);
    }
    keep(function.resultTypes);
    removeUnusedValues(program, function);
    removeDeadCountedLoops(program, function);
}

static void specializeResultsByCaller(Program& program)
{
    auto& functions = std::as_const(program.functions);
    unsigned count = functions.size();
    static constexpr OpcodeSet callsAndResultReads { Opcode::CallDirect, Opcode::ResultAt };
    constexpr unsigned maxClones = 4;
    struct CallSite {
        ValueID call;
        uint32_t callee;
        uint32_t readResults;
    };
    Vector<Vector<CallSite>> callSites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachIndexedValue(i, callsAndResultReads, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::CallDirect) {
                if (!value.isJumpToContinuation() && !functions[value.function().index()].resultTypes.isEmpty())
                    callSites[i].append({ id, value.function().index(), 0 });
                return;
            }
            for (auto& site : callSites[i]) {
                if (site.call == value.child(0))
                    site.readResults |= 1u << value.resultIndex();
            }
        });
    });
    Vector<Vector<uint32_t, maxClones>> resultSets(count);
    BitVector hasTooManySets(count);
    for (auto& sites : callSites) {
        for (auto& site : sites) {
            if (!site.readResults)
                site.readResults = 1;
            auto& sets = resultSets[site.callee];
            if (sets.contains(site.readResults))
                continue;
            if (sets.size() == maxClones)
                hasTooManySets.set(site.callee);
            else
                sets.append(site.readResults);
        }
    }
    auto sizeOf = [](const Function& function) {
        size_t size = 0;
        for (auto& block : function.blocks)
            size += block.values.size();
        return size;
    };
    Vector<unsigned> candidates;
    for (unsigned i = 0; i < count; ++i) {
        if (resultSets[i].size() > 1 && !hasTooManySets.get(i) && !functions[i].blocks.isEmpty() && !program.functionsInCycles.get(i) && returnsOnlyResults(functions[i]))
            candidates.append(i);
    }
    Vector<Vector<Function>> clones(candidates.size());
    forEachIndexInParallel(candidates.size(), [&](unsigned k) {
        unsigned original = candidates[k];
        uint32_t allRead = 0;
        for (uint32_t set : resultSets[original])
            allRead |= set;
        Function shared = functions[original];
        keepOnlyResults(program, shared, allRead);
        size_t total = 0;
        Vector<Function> created;
        for (uint32_t set : resultSets[original]) {
            Function clone = functions[original];
            keepOnlyResults(program, clone, set);
            total += sizeOf(clone);
            created.append(WTF::move(clone));
        }
        if (total <= sizeOf(shared) + sizeOf(shared) / 8)
            clones[k] = WTF::move(created);
    });
    Vector<uint32_t> firstClone(FillWith { }, count, Format::none);
    unsigned cloneCount = 0;
    for (unsigned k = 0; k < candidates.size(); ++k) {
        if (clones[k].isEmpty())
            continue;
        unsigned original = candidates[k];
        firstClone[original] = program.functions.size();
        for (auto& clone : clones[k]) {
            unsigned index = program.functions.size();
            if (program.isStaticallyHot(FunctionID(original)))
                program.staticallyHotFunctions.set(index);
            if (program.isStaticallyWarm(FunctionID(original)))
                program.staticallyWarmFunctions.set(index);
            program.functions.append(WTF::move(clone));
            ++cloneCount;
        }
    }
    if (!cloneCount)
        return;
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!callSites[i].containsIf([&](auto& site) { return firstClone[site.callee] != Format::none; }))
            return;
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& read = function.at(id);
                if (read.opcode != Opcode::ResultAt)
                    continue;
                for (auto& site : callSites[i]) {
                    if (site.call == read.child(0) && firstClone[site.callee] != Format::none)
                        read.immediates[0] = std::popcount(site.readResults & ((1u << read.resultIndex()) - 1));
                }
            }
        }
        for (auto& site : callSites[i]) {
            if (firstClone[site.callee] != Format::none)
                function.at(site.call).immediates[0] = firstClone[site.callee] + resultSets[site.callee].find(site.readResults);
        }
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", cloneCount, " clones of multi-result functions, one per set of results read");
}

static void removeUnusedResults(Program& program)
{
    auto& functions = std::as_const(program.functions);
    unsigned count = functions.size();
    static constexpr OpcodeSet resultReads { Opcode::ResultAt };
    Vector<uint32_t> usedResults(FillWith { }, count, 0u);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachIndexedValue(i, resultReads, [&](ValueID id) {
            auto& read = function.at(id);
            WTF::atomicExchangeOr(&usedResults[function.at(read.child(0)).function().index()], 1u << read.resultIndex(), std::memory_order_relaxed);
        });
    });
    BitVector discardsResults(count);
    for (unsigned i = 0; i < count; ++i) {
        auto& function = functions[i];
        unsigned resultCount = function.resultTypes.size();
        if (!resultCount || function.blocks.isEmpty())
            continue;
        if (!usedResults[i])
            usedResults[i] = 1;
        if (usedResults[i] == (1u << resultCount) - 1)
            continue;
        if (returnsOnlyResults(function))
            discardsResults.set(i);
    }
    if (discardsResults.isEmpty())
        return;
    std::atomic<unsigned> droppedCount { 0 };
    forEachIndexInParallel(count, [&](unsigned i) {
        bool readsRenumberedResult = false;
        functions.forEachIndexedValue(i, resultReads, [&](ValueID id) {
            readsRenumberedResult |= discardsResults.get(functions[i].at(functions[i].at(id).child(0)).function().index());
        });
        if (!readsRenumberedResult && !discardsResults.get(i))
            return;
        auto& function = program.functions[i];
        if (readsRenumberedResult) {
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& read = function.at(id);
                    if (read.opcode != Opcode::ResultAt)
                        continue;
                    unsigned callee = function.at(read.child(0)).function().index();
                    if (discardsResults.get(callee))
                        read.immediates[0] = std::popcount(usedResults[callee] & ((1u << read.resultIndex()) - 1));
                }
            }
        }
        if (!discardsResults.get(i))
            return;
        unsigned resultCount = function.resultTypes.size();
        keepOnlyResults(program, function, usedResults[i]);
        droppedCount.fetch_add(resultCount - function.resultTypes.size(), std::memory_order_relaxed);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", droppedCount.load(), " unused results dropped from ", discardsResults.bitCount(), " multi-result functions");
}

template<typename ForEachCallee>
static void findFunctionsWithCheckedStack(Program& program, const BitVector& mayBeEnteredWithoutCheck, const ForEachCallee& forEachCallee)
{
    unsigned count = program.functions.size();
    BitVector isCandidate(count);
    auto candidates = findIndicesInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (mayBeEnteredWithoutCheck.get(i) || program.uncalledFunctions.get(i) || function.blocks.isEmpty())
            return false;
        if (function.resumeFunction || function.startFunction || function.declaredAs & (functionIsAsync | functionIsGenerator) || function.has(FunctionIsPartOfHiddenCoroutine) || function.has(FunctionHasUnknownCallers))
            return false;
        return !function.takesCalleeClosure && !function.rampForClosures && !function.isRampForClosures;
    });
    for (unsigned i : candidates)
        isCandidate.set(i);
    for (FunctionID function : { program.moduleEvaluator, program.moduleEvaluatorIfNeeded, program.externalModuleEvaluator }) {
        if (function)
            isCandidate.clear(function.index());
    }
    for (FunctionID function : program.preludeFunctions) {
        if (function)
            isCandidate.clear(function.index());
    }
    enum : uint8_t { NotSeen, InProgress, Done };
    Vector<uint8_t> state(FillWith { }, count, static_cast<uint8_t>(NotSeen));
    Vector<uint8_t> height(FillWith { }, count, static_cast<uint8_t>(0));
    static_assert(maxNestedFunctionsWithoutStackCheck < std::numeric_limits<uint8_t>::max());
    Vector<Vector<uint32_t, 4>> callees(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isCandidate.get(i))
            return;
        static constexpr size_t maxCalleesToCompare = 16;
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> calleeSet;
        forEachCallee(std::as_const(program).functions[i], [&](unsigned callee) {
            if (!isCandidate.get(callee))
                return;
            if (callees[i].size() < maxCalleesToCompare) {
                if (callees[i].contains(callee))
                    return;
            } else {
                if (calleeSet.isEmpty())
                    calleeSet.addAll(callees[i]);
                if (!calleeSet.add(callee).isNewEntry)
                    return;
            }
            callees[i].append(callee);
        });
        if (callees[i].isEmpty()) {
            state[i] = Done;
            height[i] = 1;
        }
    });
    Vector<std::pair<uint32_t, uint32_t>> inProgress;
    for (unsigned root = 0; root < count; ++root) {
        if (!isCandidate.get(root) || state[root] != NotSeen)
            continue;
        state[root] = InProgress;
        inProgress.append({ root, 0 });
        while (!inProgress.isEmpty()) {
            auto [function, next] = inProgress.last();
            if (next < callees[function].size()) {
                ++inProgress.last().second;
                uint32_t callee = callees[function][next];
                if (state[callee] == InProgress)
                    isCandidate.clear(callee);
                else if (state[callee] == NotSeen) {
                    state[callee] = InProgress;
                    inProgress.append({ callee, 0 });
                }
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : callees[function]) {
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, height[callee]);
            }
            if (below >= maxNestedFunctionsWithoutStackCheck)
                isCandidate.clear(function);
            height[function] = isCandidate.get(function) ? below + 1 : 0;
            state[function] = Done;
            inProgress.removeLast();
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", isCandidate.bitCount(), " functions are entered with a checked stack and skip the stack check");
    program.genericEntrypointsWithCalleeStackCheck = BitVector(count);
    for (auto [index, entry] : program.genericEntrypoints) {
        if (!isCandidate.get(index))
            program.genericEntrypointsWithCalleeStackCheck.set(entry.index());
    }
    program.functionsWithCheckedStack = WTF::move(isCandidate);
    destroyOnHelperThread(WTF::move(callees));
}

template<typename Functor, typename ExtraVisitor>
static void forEachCalleeOf(const Program& program, FunctionID indexedFunction, const Function& function, const Functor& functor, const OpcodeSet& extraOpcodes, const ExtraVisitor& extraVisitor)
{
    auto entryOf = [&](FunctionID created) {
        FunctionID entry = program.genericEntrypoints.get(created.index());
        if (!entry)
            return;
        functor(entry.index());
        if (FunctionID valueEntry = program.at(entry).valueEntry; valueEntry && program.isUsedBySomeCall(valueEntry))
            functor(valueEntry.index());
    };
    std::optional<BitVector> callees;
    auto mayBeEnteredThrough = [&](ValueID expected) {
        if (!callees) {
            callees.emplace();
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::Call) {
                        callees->set(value.child(0).index());
                        if (ValueID receiver = value.child(1))
                            callees->set(receiver.index());
                    } else if (value.opcode == Opcode::CallMethod)
                        callees->set(value.child(0).index());
                    else if (value.opcode == Opcode::CallBuiltin && !value.arguments.isEmpty())
                        callees->set(value.arguments[0].index());
                }
            }
        }
        return callees->get(expected.index());
    };
    static constexpr OpcodeSet calleeReferences { Opcode::CallDirect, Opcode::Call, Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::NewObjectWithShape, Opcode::Construct, Opcode::Cast };
    auto visit = [&](ValueID id) {
        auto& value = function.at(id);
        if (extraOpcodes.contains(value.opcode))
            extraVisitor(value);
        if (value.opcode == Opcode::CallDirect) {
            functor(value.function().index());
            if (program.at(value.function()).has(Format::FunctionHasRestParameter))
                entryOf(value.function());
        } else if (FunctionID adapter = value.opcode != Opcode::Call ? FunctionID() : value.isSignatureCall ? FunctionID(value.immediates[1]) : program.didResolveCallsBySignature ? FunctionID() : program.adapterForCall(value)) {
            functor(adapter.index());
        } else if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
            entryOf(value.function());
        else if (value.opcode == Opcode::NewObjectWithShape) {
            if (FunctionID constructor = function.allocations[value.allocation()].constructedBy)
                entryOf(constructor);
        } else if (value.opcode == Opcode::Construct && !function.isSharedOutlinedBody) {
            if (FunctionID body = program.constructBody(value))
                functor(body.index());
        } else if (value.opcode == Opcode::Cast && !function.isSharedOutlinedBody) {
            if (FunctionID body = program.castBody(value))
                functor(body.index());
        }
    };
    OpcodeSet visited = calleeReferences | extraOpcodes;
    if (indexedFunction && !program.functions.mayContainPredictedClosure(indexedFunction.index())) {
        program.functions.forEachIndexedValue(indexedFunction.index(), visited, visit);
        return;
    }
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (visited.contains(value.opcode))
                visit(id);
            if (value.predictedClosure()) {
                if (mayBeEnteredThrough(id))
                    functor(value.predictedClosure().index());
                entryOf(value.predictedClosure());
            }
        }
        for (ValueID id : block.parameters) {
            if (FunctionID predicted = function.at(id).predictedClosure()) {
                if (mayBeEnteredThrough(id))
                    functor(predicted.index());
                entryOf(predicted);
            }
        }
    }
}

template<typename Functor>
static void forEachCalleeOf(const Program& program, const Function& function, const Functor& functor)
{
    forEachCalleeOf(program, FunctionID(), function, functor, OpcodeSet { }, [](const Value&) { });
}

template<typename Functor>
static void forEachCalleeOf(const Program& program, FunctionID id, const Functor& functor)
{
    forEachCalleeOf(program, id, program.at(id), functor, OpcodeSet { }, [](const Value&) { });
}

static BitVector functionsWithOtherEntries(const Program& program)
{
    unsigned count = program.functions.size();
    BitVector isReachedByOtherPaths(count);
    BitVector isRegisteredFunction(count);
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isRegisteredFunction.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isRegisteredFunction.set(method.function.index());
        }
    }
    for (auto& module : program.builtinModules)
        isRegisteredFunction.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isRegisteredFunction.set(builtin.function.index());
    BitVector isEnteredOnlyByClosures(count);
    Vector<std::pair<uint32_t, uint32_t>> functionsAndEntries;
    functionsAndEntries.reserveInitialCapacity(program.genericEntrypoints.size());
    for (auto [index, entry] : program.genericEntrypoints)
        functionsAndEntries.append({ index, entry.index() });
    Vector<uint8_t> hasOnlyKnownEntries(FillWith { }, functionsAndEntries.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(functionsAndEntries.size(), [&](unsigned k) {
        uint32_t index = functionsAndEntries[k].first;
        auto& function = program.functions[index];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        hasOnlyKnownEntries[k] = !(!isPlain || isRegisteredFunction.get(index) || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || function.resumeFunction || function.startFunction || program.isEntry(FunctionID(index)));
    });
    for (unsigned k = 0; k < functionsAndEntries.size(); ++k) {
        if (!hasOnlyKnownEntries[k])
            continue;
        isEnteredOnlyByClosures.set(functionsAndEntries[k].first);
        isEnteredOnlyByClosures.set(functionsAndEntries[k].second);
    }
    BitVector isOnlyReachedByCalls(count);
    for (unsigned which = 0; which < program.preludeFunctions.size(); ++which) {
        if (program.preludeFunctions[which] && isSelectedAtCallSite(static_cast<PreludeFunction>(which)))
            isOnlyReachedByCalls.set(program.preludeFunctions[which].index());
    }
    Vector<uint8_t> isReached(FillWith { }, count, static_cast<uint8_t>(0));
    auto reach = [&](unsigned index) {
        WTF::atomicStore(&isReached[index], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || ((function.isGenericEntrypoint() || program.genericEntrypoints.contains(i)) && !isEnteredOnlyByClosures.get(i)) || (function.has(Format::FunctionIsBuiltin) && !isOnlyReachedByCalls.get(i)) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)))
            reach(i);
        if (function.resumeFunction && (function.declaredAs & (functionIsAsync | functionIsGenerator)) != functionIsGenerator) {
            reach(i);
            reach(function.resumeFunction.index());
        }
        if (function.adapterResult != Format::none && !program.didResolveCallsBySignature)
            reach(i);
    });
    for (unsigned i = 0; i < count; ++i) {
        if (isReached[i])
            isReachedByOtherPaths.set(i);
    }
    for (auto [layout, serializer] : program.jsonSerializers)
        isReachedByOtherPaths.set(serializer.index());
    if (program.jsonDispatcher)
        isReachedByOtherPaths.set(program.jsonDispatcher.index());
    return isReachedByOtherPaths;
}

static void removeBody(Function& function)
{
    if (function.blocks.isEmpty())
        return;
    function.blocks.shrink(1);
    function.blocks[0].values.clear();
    function.blocks[0].terminator = Terminator { };
    function.inlineFrames.clear();
    function.allocations.clear();
}

static void removeUncalledFunctions(Program& program)
{
    unsigned count = program.functions.size();
    BitVector isReachedByOtherPaths = functionsWithOtherEntries(program);
    Vector<uint32_t> calls;
    calls.fill(0, count);
    Vector<uint32_t> usedAsValueCount;
    usedAsValueCount.fill(0, count);
    Vector<Vector<uint32_t>> calleesOf(count);
    Vector<Vector<uint32_t>> functionsUsedAsValuesBy(count);
    Vector<Vector<uint32_t>> methodNamesCalledBy(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet functionValuesAndMethodCalls { Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::CallMethod };
        forEachCalleeOf(program, FunctionID(i), std::as_const(program).functions[i], [&](unsigned callee) {
            calleesOf[i].append(callee);
            WTF::atomicExchangeAdd(&calls[callee], 1u, std::memory_order_relaxed);
        }, functionValuesAndMethodCalls, [&](const Value& value) {
            if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce) {
                functionsUsedAsValuesBy[i].append(value.function().index());
                WTF::atomicExchangeAdd(&usedAsValueCount[value.function().index()], 1u, std::memory_order_relaxed);
            } else if (value.opcode == Opcode::CallMethod)
                methodNamesCalledBy[i].append(value.string().index());
        });
    });
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        if (!calls[i] && !isReachedByOtherPaths.get(i))
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        for (unsigned i : worklist)
            program.uncalledFunctions.set(i);
        Vector<Vector<unsigned>> becameUncalled(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            for (uint32_t callee : calleesOf[worklist[k]]) {
                if (WTF::atomicExchangeSub(&calls[callee], 1u, std::memory_order_relaxed) == 1 && !isReachedByOtherPaths.get(callee))
                    becameUncalled[k].append(callee);
            }
            for (uint32_t created : functionsUsedAsValuesBy[worklist[k]])
                WTF::atomicExchangeSub(&usedAsValueCount[created], 1u, std::memory_order_relaxed);
        });
        worklist.shrink(0);
        for (auto& callees : becameUncalled)
            worklist.appendVector(callees);
    }
    if (Options::verboseAOTCompilation() && program.disabledInlineReasons) [[unlikely]] {
        Vector<uint32_t> declined;
        declined.fill(0, count);
        std::array<uint64_t, 16> byInlineReason { };
        uint64_t sites = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (program.uncalledFunctions.get(i))
                continue;
            auto& function = std::as_const(program).functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (!function.at(id).isCallWithDeclinedInlining())
                        continue;
                    ++declined[function.at(id).function().index()];
                    ++byInlineReason[function.at(id).declinedInliningReason()];
                    ++sites;
                }
            }
        }
        uint64_t bodies = 0;
        for (unsigned i = 0; i < count; ++i)
            bodies += declined[i] && declined[i] == calls[i] && !isReachedByOtherPaths.get(i) && !program.uncalledFunctions.get(i);
        dataLog("AOT: no copies into cold callers: ", sites, " calls stay calls:");
        for (unsigned licence = 0; licence < inlineReasonLetters.length(); ++licence) {
            if (byInlineReason[licence])
                dataLog(" ", inlineReasonLetters[licence], " ", byInlineReason[licence]);
        }
        dataLogLn("; ", bodies, " functions are compiled only for such calls");
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.uncalledFunctions.get(i))
            return;
        removeBody(program.functions[i]);
        calleesOf[i].shrink(0);
        methodNamesCalledBy[i].shrink(0);
        forEachCalleeOf(program, std::as_const(program).functions[i], [&](unsigned callee) {
            calleesOf[i].append(callee);
        });
    });
    BitVector mayBeEnteredWithoutCheck = isReachedByOtherPaths;
    BitVector isRequestedByIndex(count);
    for (auto& module : program.builtinModules)
        isRequestedByIndex.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isRequestedByIndex.set(builtin.function.index());
    auto isOnlyEnteredByCallsAndClosures = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method && function.kind != FunctionKind::Constructor)
            return false;
        return !isRequestedByIndex.get(i) && !function.has(Format::FunctionIsBuiltin) && !program.isResumeFunction(function) && !function.resumeFunction && !function.startFunction && !program.isEntry(FunctionID(i));
    };
    enum : uint8_t { IsOnlyEnteredByCallsAndClosures = 1, IsGenericEntry = 2, RunsOnce = 4 };
    Vector<uint8_t> entryKinds(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        bool runsOnce = (function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i));
        entryKinds[i] = (isOnlyEnteredByCallsAndClosures(i) ? IsOnlyEnteredByCallsAndClosures : 0) | (function.isGenericEntrypoint() ? IsGenericEntry : 0) | (runsOnce ? RunsOnce : 0);
    });
    for (unsigned i = 0; i < count; ++i) {
        if ((entryKinds[i] & (IsOnlyEnteredByCallsAndClosures | IsGenericEntry)) == IsOnlyEnteredByCallsAndClosures)
            mayBeEnteredWithoutCheck.clear(i);
    }
    for (auto [index, entry] : program.genericEntrypoints) {
        if (entryKinds[index] & IsOnlyEnteredByCallsAndClosures)
            mayBeEnteredWithoutCheck.clear(entry.index());
    }
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> methodsByName;
    for (auto& layout : program.layouts) {
        for (auto& method : layout.methods) {
            if (method.name && method.kind == MethodKind::Method)
                methodsByName.add(method.name.index(), Vector<uint32_t, 2> { }).iterator->value.append(method.function.index());
        }
    }
    findFunctionsWithCheckedStack(program, mayBeEnteredWithoutCheck, [&](const Function& function, auto&& functor) {
        size_t i = &function - std::as_const(program).functions.begin();
        for (uint32_t callee : calleesOf[i])
            functor(callee);
        for (uint32_t name : methodNamesCalledBy[i]) {
            if (auto found = methodsByName.find(name); found != methodsByName.end()) {
                for (uint32_t method : found->value)
                    functor(method);
            }
        }
    });

    BitVector isUsedAsValue(count);
    for (unsigned i = 0; i < count; ++i) {
        if (usedAsValueCount[i])
            isUsedAsValue.set(i);
    }
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isUsedAsValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isUsedAsValue.set(method.function.index());
        }
    }
    auto hasOneCall = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || isUsedAsValue.get(i) || function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)) || function.resumeFunction || function.startFunction)
            return false;
        uint32_t fromEntrypoint = 0;
        if (auto entry = program.genericEntrypoints.find(i); entry != program.genericEntrypoints.end()) {
            for (uint32_t callee : calleesOf[entry->value.index()])
                fromEntrypoint += callee == i;
        }
        return calls[i] - fromEntrypoint == 1;
    };
    for (unsigned i = 0; i < count; ++i) {
        if (entryKinds[i] & RunsOnce) {
            program.runOnceFunctions.set(i);
            worklist.append(i);
        }
    }
    while (!worklist.isEmpty()) {
        Vector<Vector<unsigned>> calledOnce(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            auto& function = std::as_const(program).functions[worklist[k]];
            BitVector isInLoop = blocksInLoops(function);
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (isInLoop.get(b))
                    continue;
                for (ValueID id : function.blocks[b].values) {
                    if (function.at(id).opcode != Opcode::CallDirect)
                        continue;
                    unsigned callee = function.at(id).function().index();
                    if (hasOneCall(callee))
                        calledOnce[k].append(callee);
                }
            }
        });
        worklist.shrink(0);
        for (auto& callees : calledOnce) {
            for (unsigned callee : callees) {
                if (!program.runOnceFunctions.set(callee))
                    worklist.append(callee);
            }
        }
    }
    Vector<uint8_t> fetchCount;
    fetchCount.fill(0, program.strings.size());
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet strings { Opcode::String };
        std::as_const(program).functions.forEachIndexedValue(i, strings, [&](ValueID id) {
            uint8_t* times = &fetchCount[function.at(id).string().index()];
            for (uint8_t seen = WTF::atomicLoad(times, std::memory_order_relaxed); seen < 2 && !WTF::atomicCompareExchangeWeakRelaxed(times, seen, static_cast<uint8_t>(seen + 1)); seen = WTF::atomicLoad(times, std::memory_order_relaxed)) { }
        });
    });
    Vector<unsigned> runOnce;
    for (size_t i : program.runOnceFunctions)
        runOnce.append(static_cast<unsigned>(i));
    Vector<Vector<uint32_t>> createdOnce(runOnce.size());
    forEachIndexInParallel(runOnce.size(), [&](unsigned k) {
        unsigned i = runOnce[k];
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = std::as_const(program).functions[i];
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (isInLoop.get(b))
                continue;
            for (ValueID id : function.blocks[b].values) {
                if (function.at(id).opcode == Opcode::String && fetchCount[function.at(id).string().index()] == 1)
                    createdOnce[k].append(function.at(id).string().index());
            }
        }
    });
    for (auto& strings : createdOnce) {
        for (uint32_t string : strings)
            program.stringsCreatedOnce.set(string);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.stringsCreatedOnce.bitCount(), " of ", program.strings.size(), " strings are created once and not cached");
    destroyOnHelperThread(std::tuple { WTF::move(calleesOf), WTF::move(functionsUsedAsValuesBy), WTF::move(methodNamesCalledBy) });
}

static void verifyExceptionPropagation(Program& program)
{
    std::atomic<bool> propagatesExceptionObjects { false };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        BitVector isPassedOn;
        static constexpr OpcodeSet catches { Opcode::CatchValue };
        std::as_const(program).functions.forEachValue(i, catches, [&](ValueID id) {
            if (!function.at(id).catchesExceptionObject())
                return;
            RELEASE_ASSERT(function.at(id).type == program.types.anything);
            isPassedOn.set(id.index());
        });
        if (isPassedOn.isEmpty())
            return;
        propagatesExceptionObjects.store(true, std::memory_order_relaxed);
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    RELEASE_ASSERT(target.arguments.size() == parameters.size());
                    for (unsigned k = 0; k < parameters.size(); ++k) {
                        if (isPassedOn.get(target.arguments[k].index()))
                            hasChanged |= !isPassedOn.set(parameters[k].index());
                    }
                }
            }
        }
        for (auto& block : function.blocks) {
            unsigned catchingCount = 0;
            bool catchesExceptionCell = false;
            for (ValueID id : block.values) {
                function.at(id).forEachChild([&](ValueID child) {
                    RELEASE_ASSERT(!isPassedOn.get(child.index()));
                });
                if (function.at(id).opcode != Opcode::CatchValue)
                    continue;
                if (function.at(id).catchesExceptionObject()) {
                    RELEASE_ASSERT(!catchingCount);
                    catchesExceptionCell = true;
                }
                ++catchingCount;
            }
            RELEASE_ASSERT(catchingCount <= 1u + catchesExceptionCell);
            auto& terminator = block.terminator;
            RELEASE_ASSERT(terminator.kind == TerminatorKind::Throw || !terminator.value || !isPassedOn.get(terminator.value.index()));
        }
    });
    program.propagatesExceptionObjects = propagatesExceptionObjects.load(std::memory_order_relaxed);
}

static void verifyVariablesStoredOnce(const Program& program)
{
    Vector<unsigned> numberOfStores;
    numberOfStores.fill(0, program.variables.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
        program.functions.forEachIndexedValue(i, stores, [&](ValueID id) {
            WTF::atomicExchangeAdd(&numberOfStores[program.functions[i].at(id).variable().index()], 1u, std::memory_order_relaxed);
        });
    });
    for (unsigned i = 0; i < program.variables.size(); ++i)
        RELEASE_ASSERT(!program.variables[i].isStoredOnce || numberOfStores[i] <= 1);
}

static ValueID originThroughTrivialParameters(const Function& function, ValueID id)
{
    auto stripCasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast && function.at(value).castReason() != CastReason::As && function.at(value).castReason() != CastReason::Coerce)
            value = function.at(value).child(0);
        return value;
    };
    for (unsigned steps = 0; steps < 8; ++steps) {
        id = stripCasts(id);
        auto& value = function.at(id);
        if (value.opcode != Opcode::Parameter || !value.immediates[0])
            return id;
        ValueID only;
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (target.block.index() != value.immediates[0])
                    continue;
                if (value.immediates[1] >= target.arguments.size())
                    return id;
                ValueID given = stripCasts(target.arguments[value.immediates[1]]);
                if (given == id)
                    continue;
                if (only && only != given)
                    return id;
                only = given;
            }
        }
        if (!only)
            return id;
        id = only;
    }
    return id;
}

static void findFunctionPairsInEnvironments(Program& program)
{
    struct Held {
        FunctionID first;
        FunctionID second;
        bool isUnknown { false };
        void add(FunctionID function)
        {
            if (function == first || function == second)
                return;
            if (!first)
                first = function;
            else if (!second)
                second = function;
            else
                isUnknown = true;
        }
    };
    Vector<Vector<Held>> held(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind == LayoutKind::Environment && !layout.isCoroutineFrame)
            held[i].grow(layout.fields.size());
    }
    struct Stored {
        LayoutID layout;
        uint32_t field;
        FunctionID function;
        bool isUnknown;
    };
    Vector<Vector<Stored>> storedIn(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet fieldStores { Opcode::PutField };
        std::as_const(program).functions.forEachIndexedValue(i, fieldStores, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.layout().index() >= held.size() || value.field() >= held[value.layout().index()].size())
                return;
            auto& stored = function.at(originThroughTrivialParameters(function, value.child(1)));
            switch (stored.opcode) {
            case Opcode::NewClosure:
                storedIn[i].append(Stored { value.layout(), value.field(), stored.function(), false });
                break;
            case Opcode::Undefined:
            case Opcode::Null:
                break;
            case Opcode::GetField:
                if (stored.layout() != value.layout() || stored.field() != value.field())
                    storedIn[i].append(Stored { value.layout(), value.field(), FunctionID(), true });
                break;
            default:
                storedIn[i].append(Stored { value.layout(), value.field(), FunctionID(), true });
                break;
            }
        });
    });
    for (auto& storedInFunction : storedIn) {
        for (auto& stored : storedInFunction) {
            auto& ofField = held[stored.layout.index()][stored.field];
            if (stored.isUnknown)
                ofField.isUnknown = true;
            else
                ofField.add(stored.function);
        }
    }
    unsigned numberOfPairs = 0;
    for (unsigned i = 0; i < held.size(); ++i) {
        for (unsigned k = 0; k < held[i].size(); ++k) {
            auto& field = program.layouts[i].fields[k];
            auto& ofField = held[i][k];
            if (ofField.isUnknown || !ofField.second || field.singleFunction || field.flags & fieldHoldsCachedClosure)
                continue;
            field.singleFunction = ofField.first;
            field.otherFunction = ofField.second;
            ++numberOfPairs;
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPairs, " variables of environments hold closures of one of two functions");
}

static void removeUnreadExports(Program& program)
{
    if (!program.usesOperation(Operations::MakeModuleNamespace))
        return;
    auto& constProgram = std::as_const(program);
    unsigned count = constProgram.functions.size();
    struct Namespace {
        FunctionID maker;
        FunctionID importer;
        FunctionID reader;
        VariableID variable;
        Vector<StringID> names;
        Vector<StringID> namesRead;
        Vector<StringID> methodsCalled;
        bool hasEscaped { false };
    };
    Vector<Namespace> namespaces;
    using NamespaceIndexMap = UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    NamespaceIndexMap byCreator;
    NamespaceIndexMap byImporter;
    NamespaceIndexMap byVariable;
    Vector<uint8_t> createsNamespace(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet operations { Opcode::Operation };
        constProgram.functions.forEachValue(i, operations, [&](ValueID id) {
            if (program.operationOf(constProgram.functions[i].at(id)) == Operations::MakeModuleNamespace)
                createsNamespace[i] = 1;
        });
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!createsNamespace[i])
            continue;
        auto& function = constProgram.functions[i];
        Namespace created;
        created.maker = FunctionID(i);
        unsigned createdCount = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutModuleVariable && function.at(value.child(0)).opcode == Opcode::Operation && program.operationOf(function.at(value.child(0))) == Operations::MakeModuleNamespace)
                    created.variable = value.variable();
                if (value.opcode != Opcode::Operation || program.operationOf(value) != Operations::MakeModuleNamespace)
                    continue;
                ++createdCount;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    auto& operand = function.at(value.arguments[k]);
                    if (!k && operand.opcode == Opcode::FunctionReference)
                        created.reader = operand.function();
                    else if (k && operand.opcode == Opcode::String)
                        created.names.append(operand.string());
                    else
                        created.hasEscaped = true;
                }
            }
        }
        if (createdCount != 1 || !created.reader || !created.variable || function.kind != FunctionKind::Plain || !function.blocks[0].parameters.isEmpty())
            created.hasEscaped = true;
        byCreator.add(i, namespaces.size());
        if (created.variable)
            byVariable.add(created.variable.index(), namespaces.size());
        namespaces.append(WTF::move(created));
    }
    struct Uses {
        unsigned number { 0 };
        FunctionID user;
        Vector<std::pair<unsigned, StringID>, 4> namesRead;
        Vector<std::pair<unsigned, StringID>, 4> methodsCalled;
        Vector<VariableID, 1> retainedIn;
        bool isAwaited { false };
        bool isReturned { false };
        bool hasEscaped { false };
    };
    auto promise = program.find(asBytes("Promise"_s.span8()));
    auto all = program.find(asBytes("all"_s.span8()));
    auto length = program.find(asBytes("length"_s.span8()));
    auto esModule = program.find(asBytes("__esModule"_s.span8()));
    auto thenName = program.find(asBytes("then"_s.span8()));
    auto catchName = program.find(asBytes("catch"_s.span8()));
    auto finallyName = program.find(asBytes("finally"_s.span8()));
    enum class Held : uint8_t { Itself, ListOfPromises, PromiseOfList, List, IteratorOfList };
    struct Followed {
        FunctionID in;
        ValueID value;
        unsigned stage;
        Held as;
        unsigned index;
    };
    static constexpr uint32_t usedByTerminator = UINT32_MAX;
    struct Use {
        uint32_t block;
        uint32_t user;
    };
    struct UsesInFunction {
        std::span<const Use> of(ValueID id) const { return uses.span().subspan(firstUse[id.index()], firstUse[id.index() + 1] - firstUse[id.index()]); }

        Vector<uint32_t> firstUse;
        Vector<Use> uses;
    };
    using UsesByFunction = UncheckedKeyHashMap<uint32_t, std::unique_ptr<UsesInFunction>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    auto usesIn = [&](UsesByFunction& listed, FunctionID id) -> const UsesInFunction& {
        auto result = listed.add(id.index(), nullptr);
        if (!result.isNewEntry)
            return *result.iterator->value;
        auto& function = constProgram.at(id);
        auto forEachUseInOrder = [&](auto&& functor) {
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& block = function.blocks[b];
                for (ValueID user : block.values) {
                    function.at(user).forEachChild([&](ValueID child) {
                        functor(child, Use { b, user.index() });
                    });
                }
                if (block.terminator.value)
                    functor(block.terminator.value, Use { b, usedByTerminator });
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        functor(argument, Use { b, usedByTerminator });
                }
            }
        };
        auto inFunction = makeUniqueWithoutFastMallocCheck<UsesInFunction>();
        inFunction->firstUse.fill(0, function.values.size() + 1);
        forEachUseInOrder([&](ValueID used, Use) {
            ++inFunction->firstUse[used.index() + 1];
        });
        for (unsigned i = 1; i < inFunction->firstUse.size(); ++i)
            inFunction->firstUse[i] += inFunction->firstUse[i - 1];
        inFunction->uses.grow(inFunction->firstUse.last());
        Vector<uint32_t> next = inFunction->firstUse;
        forEachUseInOrder([&](ValueID used, Use use) {
            inFunction->uses[next[used.index()]++] = use;
        });
        result.iterator->value = WTF::move(inFunction);
        return *result.iterator->value;
    };
    auto usesOf = [&](FunctionID startsIn, ValueID start, Uses& uses, bool isPromise, UsesByFunction& listed) {
        struct Handler {
            FunctionID function;
            Followed createdPromise;
        };
        Vector<Handler, 2> handlers;
        Vector<Followed, 2> returnedByHandlers;
        Vector<Followed, 4> worklist { Followed { startsIn, start, 0, Held::Itself, 0 } };
        Vector<Followed, 8> seen;
        auto resolve = [&](const Followed& createdPromise, const Followed& returned) {
            if (returned.as == Held::Itself)
                worklist.append(Followed { createdPromise.in, createdPromise.value, 0, Held::Itself, 0 });
            else if (returned.as == Held::List || returned.as == Held::PromiseOfList)
                worklist.append(Followed { createdPromise.in, createdPromise.value, returned.stage, Held::PromiseOfList, returned.index });
            else
                uses.hasEscaped = true;
        };
        while (!worklist.isEmpty() && !uses.hasEscaped) {
            Followed next = worklist.takeLast();
            ValueID held = next.value;
            unsigned stage = next.stage;
            if (seen.containsIf([&](auto& other) { return other.in == next.in && other.value == held && other.stage == stage && other.as == next.as && other.index == next.index; }))
                continue;
            seen.append(next);
            auto& function = constProgram.at(next.in);
            auto withoutCasts = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() != CastReason::Coerce)
                    id = function.at(id).child(0);
                return id;
            };
            auto isPromiseConstructor = [&](ValueID id) {
                return promise && function.at(id).opcode == Opcode::GetGlobal && function.at(id).string() == *promise;
            };
            bool isPromiseNow = next.as == Held::PromiseOfList || (next.as == Held::Itself && isPromise && !stage);
            auto isReaction = [&](StringID name) {
                return (thenName && name == *thenName) || (catchName && name == *catchName) || (finallyName && name == *finallyName);
            };
            auto& usesInFunction = usesIn(listed, next.in);
            auto isOnlyCalledOnHolder = [&](ValueID member) {
                auto memberUses = usesInFunction.of(member);
                for (size_t at = 0; at < memberUses.size(); ++at) {
                    if (memberUses[at].user == usedByTerminator || (at + 1 < memberUses.size() && memberUses[at + 1].user == memberUses[at].user))
                        return false;
                    auto& user = function.at(ValueID(memberUses[at].user));
                    if (user.opcode != Opcode::Call || user.child(0) != member || user.child(1) != held)
                        return false;
                }
                return true;
            };
            auto followThrough = [&](ValueID id, const Value& user) {
                if (!isPromiseNow || user.arguments.contains(held))
                    return false;
                StringID name;
                if (user.opcode == Opcode::CallMethod && user.child(0) == held)
                    name = user.string();
                else if (user.opcode == Opcode::Call && user.child(1) == held && user.child(0) != held && function.at(user.child(0)).opcode == Opcode::GetById && function.at(user.child(0)).child(0) == held)
                    name = function.at(user.child(0)).string();
                else
                    return false;
                Followed createdPromise { next.in, id, stage, next.as, next.index };
                if ((catchName && name == *catchName) || (finallyName && name == *finallyName)) {
                    worklist.append(createdPromise);
                    return true;
                }
                if (!thenName || name != *thenName || user.arguments.isEmpty())
                    return false;
                auto& given = function.at(withoutCasts(user.arguments[0]));
                if (given.opcode == Opcode::Undefined || given.opcode == Opcode::Null) {
                    worklist.append(createdPromise);
                    return true;
                }
                if (given.opcode != Opcode::FunctionReference && given.opcode != Opcode::NewClosure)
                    return false;
                auto& handler = constProgram.at(given.function());
                if ((handler.kind != FunctionKind::Plain && handler.kind != FunctionKind::Arrow) || handler.has(Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsSloppy | Format::FunctionIsBuiltin) || handler.blocks.isEmpty())
                    return false;
                if (!handler.parameterTypes.isEmpty() && handler.parameterTypes[0] && !constProgram.at(handler.parameterTypes[0]).objectLayouts.isEmpty())
                    return false;
                if (handler.blocks[0].parameters.size() > handler.captureTypes.size())
                    worklist.append(Followed { given.function(), handler.blocks[0].parameters[handler.captureTypes.size()], next.as == Held::Itself ? 1 : stage, next.as == Held::Itself ? Held::Itself : Held::List, next.index });
                handlers.append(Handler { given.function(), createdPromise });
                for (auto& returned : returnedByHandlers) {
                    if (returned.in == given.function())
                        resolve(createdPromise, returned);
                }
                return true;
            };
            auto heldValueUses = usesInFunction.of(held);
            for (size_t at = 0; at < heldValueUses.size();) {
                unsigned blockNumber = heldValueUses[at].block;
                auto& block = function.blocks[blockNumber];
                while (at < heldValueUses.size() && heldValueUses[at].block == blockNumber && heldValueUses[at].user != usedByTerminator) {
                    ValueID id(heldValueUses[at].user);
                    auto& user = function.at(id);
                    unsigned useCount = 0;
                    for (; at < heldValueUses.size() && heldValueUses[at].user == id.index(); ++at)
                        ++useCount;
                    switch (user.opcode) {
                    case Opcode::Cast: {
                        CastReason reason = user.castReason();
                        bool inspectsKindOnly = (reason == CastReason::As || reason == CastReason::Store) && constProgram.at(user.type).objectLayouts.isEmpty() && constProgram.at(user.type).engineClasses.isEmpty();
                        if (reason == CastReason::Claim || reason == CastReason::Upcast || reason == CastReason::Flow || reason == CastReason::NonNull || inspectsKindOnly)
                            worklist.append(Followed { next.in, id, stage, next.as, next.index });
                        else
                            uses.hasEscaped = true;
                        continue;
                    }
                    case Opcode::IsUndefinedOrNull:
                    case Opcode::StrictEq:
                    case Opcode::TypeOfIs:
                    case Opcode::LogicalNot:
                    case Opcode::ToBoolean:
                        continue;
                    default:
                        break;
                    }
                    if (followThrough(id, user))
                        continue;
                    if (isPromiseNow && user.opcode == Opcode::GetById && isReaction(user.string()) && isOnlyCalledOnHolder(id))
                        continue;
                    switch (next.as) {
                    case Held::Itself:
                        break;
                    case Held::ListOfPromises:
                        if (all && user.opcode == Opcode::Call && useCount == 1 && user.arguments.size() == 1 && user.arguments[0] == held && isPromiseConstructor(user.child(1))
                            && function.at(user.child(0)).opcode == Opcode::GetById && function.at(user.child(0)).string() == *all && isPromiseConstructor(function.at(user.child(0)).child(0)))
                            worklist.append(Followed { next.in, id, stage, Held::PromiseOfList, next.index });
                        else
                            uses.hasEscaped = true;
                        continue;
                    case Held::PromiseOfList:
                        if (user.opcode == Opcode::Await)
                            worklist.append(Followed { next.in, id, stage, Held::List, next.index });
                        else
                            uses.hasEscaped = true;
                        continue;
                    case Held::List:
                        if (user.opcode == Opcode::GetByVal && useCount == 1 && user.child(0) == held && function.at(user.child(1)).opcode == Opcode::Integer) {
                            if (function.at(user.child(1)).integer() == static_cast<int64_t>(next.index))
                                worklist.append(Followed { next.in, id, 1, Held::Itself, 0 });
                        } else if (user.opcode == Opcode::IteratorOpen)
                            worklist.append(Followed { next.in, id, stage, Held::IteratorOfList, next.index });
                        else if (user.opcode != Opcode::GetById || !length || user.string() != *length)
                            uses.hasEscaped = true;
                        continue;
                    case Held::IteratorOfList:
                        if (user.opcode == Opcode::IteratorValue && useCount == 1 && user.child(0) == held && function.at(user.child(1)).opcode == Opcode::Integer) {
                            if (function.at(user.child(1)).integer() == static_cast<int64_t>(next.index))
                                worklist.append(Followed { next.in, id, 1, Held::Itself, 0 });
                        } else if (user.opcode != Opcode::IteratorHasNext && user.opcode != Opcode::IteratorClose)
                            uses.hasEscaped = true;
                        continue;
                    }
                    switch (user.opcode) {
                    case Opcode::Await:
                        uses.isAwaited = true;
                        if (stage > 1)
                            uses.hasEscaped = true;
                        else
                            worklist.append(Followed { next.in, id, stage + 1, Held::Itself, 0 });
                        break;
                    case Opcode::GetById:
                        uses.namesRead.append({ stage, user.string() });
                        break;
                    case Opcode::CallMethod:
                        if (useCount == 1 && user.child(0) == held)
                            uses.methodsCalled.append({ stage, user.string() });
                        else
                            uses.hasEscaped = true;
                        break;
                    case Opcode::Call: {
                        ValueID callee = withoutCasts(user.child(0));
                        if (useCount == 1 && user.child(1) == held && function.at(callee).opcode == Opcode::GetById && function.at(callee).child(0) == held)
                            uses.methodsCalled.append({ stage, function.at(callee).string() });
                        else
                            uses.hasEscaped = true;
                        break;
                    }
                    case Opcode::Operation:
                        if (esModule && program.operationOf(user) == Operations::DefineField && useCount == 1 && user.arguments.size() == 3 && user.arguments[0] == held
                            && function.at(user.arguments[1]).opcode == Opcode::String && function.at(user.arguments[1]).string() == *esModule && function.at(user.arguments[2]).opcode == Opcode::Boolean)
                            worklist.append(Followed { next.in, id, stage, Held::Itself, 0 });
                        else
                            uses.hasEscaped = true;
                        break;
                    case Opcode::PutModuleVariable:
                        if (stage == (isPromise ? 1u : 0u))
                            uses.retainedIn.append(user.variable());
                        else
                            uses.hasEscaped = true;
                        break;
                    case Opcode::NewArray:
                        if (!isPromise || stage) {
                            uses.hasEscaped = true;
                            break;
                        }
                        for (unsigned k = 0; k < user.arguments.size(); ++k) {
                            if (user.arguments[k] == held)
                                worklist.append(Followed { next.in, id, stage, Held::ListOfPromises, k });
                        }
                        break;
                    default:
                        uses.hasEscaped = true;
                        break;
                    }
                }
                while (at < heldValueUses.size() && heldValueUses[at].block == blockNumber)
                    ++at;
                auto& terminator = block.terminator;
                if (terminator.value == held) {
                    if (terminator.kind == TerminatorKind::Return && next.in != startsIn) {
                        returnedByHandlers.append(next);
                        for (auto& handler : handlers) {
                            if (handler.function == next.in)
                                resolve(handler.createdPromise, next);
                        }
                    } else if (terminator.kind == TerminatorKind::Return && next.as == Held::Itself)
                        uses.isReturned = true;
                    else if (terminator.kind != TerminatorKind::Branch)
                        uses.hasEscaped = true;
                }
                for (auto& target : terminator.targets) {
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        if (target.arguments[k] == held)
                            worklist.append(Followed { next.in, function.blocks[target.block.index()].parameters[k], stage, next.as, next.index });
                    }
                }
            }
        }
    };
    auto forEachUse = [&](Opcode yields, const NamespaceIndexMap& byProducer, bool isPromise, auto&& functor) {
        Vector<Vector<Uses>> usesIn(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            UsesByFunction listed;
            constProgram.functions.forEachIndexedValue(i, OpcodeSet { yields }, [&](ValueID id) {
                auto found = byProducer.find(constProgram.functions[i].at(id).immediates[0]);
                if (found == byProducer.end())
                    return;
                Uses uses;
                uses.number = found->value;
                uses.user = FunctionID(i);
                usesOf(FunctionID(i), id, uses, isPromise, listed);
                usesIn[i].append(WTF::move(uses));
            });
        });
        for (auto& usesInFunction : usesIn) {
            for (auto& uses : usesInFunction)
                functor(uses);
        }
    };
    auto note = [&](Namespace& created, const Uses& uses, unsigned objectStage) {
        created.hasEscaped |= uses.hasEscaped || uses.isReturned;
        for (auto* list : { &uses.namesRead, &uses.methodsCalled }) {
            for (auto [stage, name] : *list) {
                if (stage != objectStage)
                    created.hasEscaped = true;
                else
                    (list == &uses.namesRead ? created.namesRead : created.methodsCalled).append(name);
            }
        }
    };
    NamespaceIndexMap byHoldingVariable;
    NamespaceIndexMap newlyRetained;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> trackedStores;
    auto recordRetainingLocations = [&](const Uses& uses) {
        for (VariableID variable : uses.retainedIn) {
            auto result = byHoldingVariable.add(variable.index(), uses.number);
            if (result.isNewEntry)
                newlyRetained.add(variable.index(), uses.number);
            else if (result.iterator->value != uses.number) {
                namespaces[uses.number].hasEscaped = true;
                namespaces[result.iterator->value].hasEscaped = true;
            }
            ++trackedStores.add(variable.index(), 0).iterator->value;
        }
    };
    forEachUse(Opcode::CallDirect, byCreator, false, [&](const Uses& uses) {
        auto& created = namespaces[uses.number];
        auto& user = constProgram.at(uses.user);
        bool isOnlyReturned = uses.isReturned && !uses.hasEscaped && !uses.isAwaited && uses.namesRead.isEmpty() && uses.methodsCalled.isEmpty() && uses.retainedIn.isEmpty();
        if (isOnlyReturned && !created.importer && user.kind == FunctionKind::Plain && user.has(Format::FunctionIsAsync) && user.has(Format::FunctionIsHiddenInStackTraces) && !user.has(Format::FunctionIsUsedAsValue) && user.blocks[0].parameters.isEmpty() && !constProgram.isEntry(uses.user)) {
            created.importer = uses.user;
            byImporter.add(uses.user.index(), uses.number);
            return;
        }
        created.hasEscaped |= uses.isAwaited;
        note(created, uses, 0);
        recordRetainingLocations(uses);
    });
    forEachUse(Opcode::CallDirect, byImporter, true, [&](const Uses& uses) {
        note(namespaces[uses.number], uses, 1);
        recordRetainingLocations(uses);
    });
    forEachUse(Opcode::GetModuleVariable, byVariable, false, [&](const Uses& uses) {
        auto& created = namespaces[uses.number];
        if (uses.user == created.maker)
            return;
        created.hasEscaped |= uses.isAwaited;
        note(created, uses, 0);
        recordRetainingLocations(uses);
    });
    for (unsigned round = 0; round < 3 && !newlyRetained.isEmpty(); ++round) {
        forEachUse(Opcode::GetModuleVariable, std::exchange(newlyRetained, NamespaceIndexMap()), false, [&](const Uses& uses) {
            namespaces[uses.number].hasEscaped |= uses.isAwaited;
            note(namespaces[uses.number], uses, 0);
            recordRetainingLocations(uses);
        });
    }
    for (auto& entry : newlyRetained)
        namespaces[entry.value].hasEscaped = true;
    if (!byHoldingVariable.isEmpty()) {
        Vector<uint32_t> numberOfStores(FillWith { }, constProgram.variables.size(), 0u);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = constProgram.functions[i];
            static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
            constProgram.functions.forEachValue(i, stores, [&](ValueID id) {
                auto& value = function.at(id);
                if (!byHoldingVariable.contains(value.variable().index()))
                    return;
                Opcode stored = function.at(value.child(0)).opcode;
                if (stored != Opcode::Undefined && stored != Opcode::Null)
                    WTF::atomicExchangeAdd(&numberOfStores[value.variable().index()], 1u, std::memory_order_relaxed);
            });
        });
        for (auto& entry : byHoldingVariable) {
            if (numberOfStores[entry.key] != trackedStores.get(entry.key))
                namespaces[entry.value].hasEscaped = true;
        }
    }
    unsigned remainingCount = 0;
    unsigned numberOfExports = 0;
    unsigned unreadCount = 0;
    unsigned numberOfFunctions = 0;
    unsigned numberOfVariables = 0;
    unsigned numberOfClasses = 0;
    auto report = [&] {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", namespaces.size(), " namespace objects of modules are made; ", remainingCount, " of them are only read by constant names: of their ", numberOfExports, " exports nobody reads ", unreadCount, "; ", numberOfFunctions, " functions that nothing else names are dropped, those of ", numberOfClasses, " classes among them; ", numberOfVariables, " module variables that nothing reads any more are not stored to");
    };
    if (!namespaces.containsIf([](auto& created) { return !created.hasEscaped; })) {
        report();
        return;
    }
    Vector<uint8_t> isReferencedElsewhere(FillWith { }, namespaces.size(), static_cast<uint8_t>(0));
    BitVector createsOrImports(count);
    for (auto* table : { &byCreator, &byImporter }) {
        for (auto& entry : *table)
            createsOrImports.set(entry.key);
    }
    BitVector containsNamespace(constProgram.variables.size());
    for (auto& entry : byVariable)
        containsNamespace.set(entry.key);
    auto switchTargetFor = [&](const Namespace& created, const Function& reader, StringID name) -> std::optional<unsigned> {
        auto& terminator = reader.blocks[0].terminator;
        size_t index = created.names.find(name);
        size_t which = index == notFound ? notFound : terminator.cases.find(static_cast<int64_t>(index));
        return which == notFound ? std::nullopt : std::optional<unsigned>(terminator.targets[which].block.index());
    };
    auto returnedVariableOf = [&](const Function& reader, unsigned arm) -> VariableID {
        VariableID returnedValue;
        Vector<FunctionID, 1> makers;
        BitVector isSeen(reader.blocks.size());
        Vector<unsigned, 8> blocksToVisit { arm };
        while (!blocksToVisit.isEmpty()) {
            unsigned block = blocksToVisit.takeLast();
            if (isSeen.set(block))
                continue;
            for (ValueID id : reader.blocks[block].values) {
                auto& value = reader.at(id);
                switch (value.opcode) {
                case Opcode::GetModuleVariable:
                    if (returnedValue && returnedValue != value.variable())
                        return { };
                    returnedValue = value.variable();
                    break;
                case Opcode::CallDirect:
                    if (constProgram.at(value.function()).kind != FunctionKind::Module || !value.arguments.isEmpty())
                        return { };
                    makers.append(value.function());
                    break;
                case Opcode::IsUndefinedOrNull:
                case Opcode::StrictEq:
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Cast:
                case Opcode::GetGlobal:
                case Opcode::String:
                case Opcode::Construct:
                    break;
                default:
                    return { };
                }
            }
            for (auto& target : reader.blocks[block].terminator.targets)
                blocksToVisit.append(target.block.index());
        }
        for (FunctionID id : makers) {
            auto& maker = constProgram.at(id);
            for (auto& block : maker.blocks) {
                if (block.terminator.kind != TerminatorKind::Return)
                    continue;
                ValueID returned = block.terminator.value;
                while (returned && maker.at(returned).opcode == Opcode::Cast && maker.at(returned).castReason() != CastReason::Coerce)
                    returned = maker.at(returned).child(0);
                if (!returned || maker.at(returned).opcode != Opcode::GetModuleVariable || maker.at(returned).variable() != returnedValue)
                    return { };
            }
        }
        return returnedValue;
    };
    BitVector isCalledOnNamespace(constProgram.variables.size());
    for (auto& created : namespaces) {
        if (created.hasEscaped || !created.reader || constProgram.at(created.reader).blocks.isEmpty() || constProgram.at(created.reader).blocks[0].terminator.kind != TerminatorKind::Switch)
            continue;
        for (StringID name : created.methodsCalled) {
            if (auto arm = switchTargetFor(created, constProgram.at(created.reader), name)) {
                if (VariableID returnedValue = returnedVariableOf(constProgram.at(created.reader), *arm))
                    isCalledOnNamespace.set(returnedValue.index());
            }
        }
    }
    Vector<uint8_t> mayHoldThisReader(FillWith { }, constProgram.variables.size(), static_cast<uint8_t>(0));
    auto countNameReferencesIn = [&](unsigned i, Vector<uint32_t>& referenceCount) {
        auto& function = constProgram.functions[i];
        auto name = [&](FunctionID named, bool isValue) {
            if (!named)
                return;
            ++referenceCount[named.index()];
            if (!isValue || !createsOrImports.get(named.index()))
                return;
            for (auto* table : { &byCreator, &byImporter }) {
                if (auto found = table->find(named.index()); found != table->end())
                    WTF::atomicStore(&isReferencedElsewhere[found->value], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        };
        if (constProgram.functions.mayContainPredictedClosure(i)) {
            for (auto& block : function.blocks) {
                for (ValueID id : block.parameters)
                    name(function.at(id).predictedClosure(), true);
                for (ValueID id : block.values)
                    name(function.at(id).predictedClosure(), true);
            }
        }
        static constexpr OpcodeSet references { Opcode::CallDirect, Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::IsClosureOf, Opcode::GetCapture, Opcode::NewObjectWithShape, Opcode::GetModuleVariable, Opcode::PutModuleVariable };
        constProgram.functions.forEachIndexedValue(i, references, [&](ValueID id) {
            auto& value = function.at(id);
            switch (value.opcode) {
            case Opcode::CallDirect:
                name(value.function(), false);
                break;
            case Opcode::FunctionReference:
            case Opcode::NewClosure:
            case Opcode::NewClosureOnce:
            case Opcode::IsClosureOf:
            case Opcode::GetCapture:
                name(value.function(), true);
                break;
            case Opcode::NewObjectWithShape:
                if (value.allocation() < function.allocations.size())
                    name(function.allocations[value.allocation()].constructedBy, true);
                break;
            case Opcode::GetModuleVariable:
            case Opcode::PutModuleVariable:
                if (value.opcode == Opcode::PutModuleVariable && isCalledOnNamespace.get(value.variable().index())) {
                    ValueID stored = value.child(0);
                    while (function.at(stored).opcode == Opcode::Cast && function.at(stored).castReason() != CastReason::Coerce)
                        stored = function.at(stored).child(0);
                    Opcode opcode = function.at(stored).opcode;
                    bool isClosure = opcode == Opcode::FunctionReference || opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce;
                    if (isClosure ? constProgram.at(function.at(stored).function()).has(Format::FunctionHasThis) : opcode != Opcode::Undefined && opcode != Opcode::Null)
                        WTF::atomicStore(&mayHoldThisReader[value.variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                }
                if (!containsNamespace.get(value.variable().index()))
                    break;
                if (auto found = byVariable.find(value.variable().index()); value.opcode == Opcode::PutModuleVariable && namespaces[found->value].maker != FunctionID(i))
                    WTF::atomicStore(&isReferencedElsewhere[found->value], static_cast<uint8_t>(1), std::memory_order_relaxed);
                break;
            default:
                break;
            }
        });
    };
    constexpr unsigned numberOfParts = 16;
    Vector<Vector<uint32_t>> referenceCountInPart(numberOfParts);
    forEachIndexInParallel(numberOfParts, [&](unsigned part) {
        referenceCountInPart[part].fill(0, count);
        for (unsigned i = part; i < count; i += numberOfParts)
            countNameReferencesIn(i, referenceCountInPart[part]);
    });
    Vector<uint32_t> referenceCount = WTF::move(referenceCountInPart[0]);
    for (unsigned part = 1; part < numberOfParts; ++part) {
        for (unsigned i = 0; i < count; ++i)
            referenceCount[i] += referenceCountInPart[part][i];
    }
    BitVector hasOtherEntries(count);
    for (auto& layout : constProgram.layouts) {
        if (layout.constructor)
            hasOtherEntries.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                hasOtherEntries.set(method.function.index());
        }
    }
    for (auto& module : constProgram.builtinModules)
        hasOtherEntries.set(module.second.index());
    for (auto& builtin : constProgram.builtinFunctions)
        hasOtherEntries.set(builtin.function.index());
    for (FunctionID function : constProgram.preludeFunctions) {
        if (function)
            hasOtherEntries.set(function.index());
    }
    for (auto& [property, key, function] : constProgram.functionsByKey)
        hasOtherEntries.set(function.index());
    for (auto& module : constProgram.modules) {
        if (module.initializer)
            hasOtherEntries.set(module.initializer.index());
    }
    auto then = program.find(asBytes("then"_s.span8()));
    Vector<unsigned> maybeDead;
    BitVector mayBecomeUnread(constProgram.variables.size());
    for (unsigned n = 0; n < namespaces.size(); ++n) {
        auto& created = namespaces[n];
        if (created.hasEscaped || isReferencedElsewhere[n] || (then && created.names.contains(*then)))
            continue;
        bool isReachedFromOutside = false;
        for (FunctionID function : { created.maker, created.importer, created.reader }) {
            if (function)
                isReachedFromOutside |= constProgram.isEntry(function) || hasOtherEntries.get(function.index()) || constProgram.at(function).has(FunctionHasUnknownCallers | FunctionIsEnteredByValue);
        }
        if (isReachedFromOutside)
            continue;
        auto& reader = program.at(created.reader);
        if (referenceCount[created.reader.index()] != 1 || reader.blocks.isEmpty() || reader.blocks[0].terminator.kind != TerminatorKind::Switch)
            continue;
        unsigned refusal = reader.blocks.size();
        reader.blocks.append(Block { });
        auto& terminator = reader.blocks[0].terminator;
        auto blockOf = [&](StringID name) {
            return switchTargetFor(created, reader, name);
        };
        bool escapesSelf = false;
        for (StringID name : created.methodsCalled) {
            auto block = blockOf(name);
            if (!block)
                continue;
            auto& values = reader.blocks[*block].values;
            if (values.size() == 1 && reader.at(values[0]).opcode == Opcode::FunctionReference) {
                escapesSelf |= constProgram.at(reader.at(values[0]).function()).has(Format::FunctionHasThis);
                continue;
            }
            VariableID returnedValue = returnedVariableOf(reader, *block);
            escapesSelf |= !returnedValue || mayHoldThisReader[returnedValue.index()];
        }
        if (escapesSelf) {
            reader.blocks.removeLast();
            continue;
        }
        ++remainingCount;
        numberOfExports += created.names.size();
        BitVector isRead(reader.blocks.size());
        isRead.set(0);
        isRead.set(terminator.targets.last().block.index());
        for (auto* list : { &created.namesRead, &created.methodsCalled }) {
            for (StringID name : *list) {
                if (auto block = blockOf(name))
                    isRead.set(*block);
            }
        }
        for (unsigned which = 0; which < terminator.cases.size(); ++which) {
            auto& target = terminator.targets[which];
            if (isRead.get(target.block.index()) || !target.arguments.isEmpty())
                continue;
            ++unreadCount;
            target.block = BlockID(refusal);
        }
        BitVector isReached(reader.blocks.size());
        Vector<unsigned, 16> blocksToVisit { 0u };
        while (!blocksToVisit.isEmpty()) {
            unsigned block = blocksToVisit.takeLast();
            if (isReached.set(block))
                continue;
            for (auto& target : reader.blocks[block].terminator.targets)
                blocksToVisit.append(target.block.index());
        }
        auto removeReference = [&](FunctionID named) {
            if (!named)
                return;
            --referenceCount[named.index()];
            maybeDead.append(named.index());
        };
        for (unsigned block = 0; block < refusal; ++block) {
            if (isReached.get(block))
                continue;
            for (ValueID id : reader.blocks[block].parameters)
                removeReference(reader.at(id).predictedClosure());
            for (ValueID id : reader.blocks[block].values) {
                auto& value = reader.at(id);
                removeReference(value.predictedClosure());
                if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::CallDirect || value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::GetCapture)
                    removeReference(value.function());
                else if (value.opcode == Opcode::NewObjectWithShape && value.allocation() < reader.allocations.size())
                    removeReference(reader.allocations[value.allocation()].constructedBy);
                else if (value.opcode == Opcode::GetModuleVariable)
                    mayBecomeUnread.set(value.variable().index());
            }
            reader.blocks[block] = Block { };
        }
        bool isRejectionReached = false;
        for (auto& target : terminator.targets)
            isRejectionReached |= target.block.index() == refusal;
        if (!isRejectionReached) {
            reader.blocks.removeLast();
            continue;
        }
        StringID errorName = program.intern(asBytes("Error"_s.span8()));
        StringID rejectionMessage = program.intern(asBytes("An export that nothing in the program reads was read from the namespace object of its module"_s.span8()));
        auto append = [&](Opcode opcode, TypeID type) -> Value& {
            Value value;
            value.opcode = opcode;
            value.position = reader.position;
            value.type = type;
            value.rep = repOf(constProgram.at(type));
            reader.blocks[refusal].values.append(ValueID(reader.values.size()));
            reader.values.append(WTF::move(value));
            return reader.values.last();
        };
        append(Opcode::GetGlobal, constProgram.types.anything).immediates[0] = errorName.index();
        append(Opcode::String, constProgram.types.string).immediates[0] = rejectionMessage.index();
        auto& construct = append(Opcode::Construct, constProgram.types.anything);
        construct.immediates = { Format::none, Format::none };
        construct.children[0] = ValueID(reader.values.size() - 3);
        construct.arguments.append(ValueID(reader.values.size() - 2));
        reader.blocks[refusal].terminator.kind = TerminatorKind::Throw;
        reader.blocks[refusal].terminator.value = ValueID(reader.values.size() - 1);
    }
    auto forEachFunctionReferencedIn = [&](const Function& function, auto&& functor) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                FunctionID named;
                if (value.opcode == Opcode::CallDirect || value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::GetCapture)
                    named = value.function();
                else if (value.opcode == Opcode::NewObjectWithShape && value.allocation() < function.allocations.size())
                    named = function.allocations[value.allocation()].constructedBy;
                for (FunctionID other : { named, value.predictedClosure() }) {
                    if (other)
                        functor(other);
                }
            }
            for (ValueID id : block.parameters) {
                if (FunctionID other = function.at(id).predictedClosure())
                    functor(other);
            }
        }
    };
    auto hasBody = [&](const Function& function) {
        return !function.blocks.isEmpty() && (!function.blocks[0].values.isEmpty() || function.blocks[0].terminator.kind != TerminatorKind::Unreachable);
    };
    auto isHostFunction = [&](unsigned i) {
        return constProgram.isEntry(FunctionID(i)) || constProgram.functions[i].has(Format::FunctionIsBuiltin | Format::FunctionIsModuleBody | Format::FunctionIsDeclaredGenericEntrypoint | FunctionHasUnknownCallers);
    };
    auto drop = [&](unsigned i) {
        auto& function = program.functions[i];
        ++numberOfFunctions;
        forEachFunctionReferencedIn(function, [&](FunctionID other) {
            --referenceCount[other.index()];
            maybeDead.append(other.index());
        });
        removeBody(function);
        function.flags &= ~static_cast<uint32_t>(Format::FunctionIsUsedAsValue);
    };
    Vector<unsigned> lessReferencedConstructors;
    auto removeUnreferencedFunctions = [&] {
        while (!maybeDead.isEmpty()) {
            unsigned i = maybeDead.takeLast();
            auto& function = constProgram.functions[i];
            if (function.kind == FunctionKind::Constructor)
                lessReferencedConstructors.append(i);
            bool isOnlyEnteredByName = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Module;
            if (!referenceCount[i] && isOnlyEnteredByName && hasBody(function) && !hasOtherEntries.get(i) && !isHostFunction(i))
                drop(i);
        }
    };
    removeUnreferencedFunctions();
    if (!lessReferencedConstructors.isEmpty()) {
        NamespaceIndexMap constructorLayout;
        BitVector isExtended(constProgram.layouts.size());
        BitVector isSuperclassConstructor(count);
        for (unsigned k = 0; k < constProgram.layouts.size(); ++k) {
            auto& layout = constProgram.layouts[k];
            if (layout.constructor && !constructorLayout.add(layout.constructor.index(), k).isNewEntry)
                isSuperclassConstructor.set(layout.constructor.index());
            if (layout.base)
                isExtended.set(layout.base.index());
            if (layout.declaredSuperclassConstructor)
                isSuperclassConstructor.set(layout.declaredSuperclassConstructor.index());
        }
        while (!lessReferencedConstructors.isEmpty()) {
            unsigned constructor = lessReferencedConstructors.takeLast();
            auto found = constructorLayout.find(constructor);
            if (found == constructorLayout.end() || isExtended.get(found->value) || isSuperclassConstructor.get(constructor) || !hasBody(constProgram.functions[constructor]))
                continue;
            auto& layout = constProgram.layouts[found->value];
            Vector<unsigned, 16> members { constructor };
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (!members.contains(method.function.index()))
                        members.append(method.function.index());
                }
            }
            Vector<uint32_t, 16> internalReferenceCount(FillWith { }, members.size(), 0u);
            for (unsigned member : members) {
                forEachFunctionReferencedIn(constProgram.functions[member], [&](FunctionID other) {
                    if (size_t which = members.find(other.index()); which != notFound)
                        ++internalReferenceCount[which];
                });
            }
            bool isReferencedExternally = false;
            for (unsigned which = 0; which < members.size(); ++which)
                isReferencedExternally |= referenceCount[members[which]] != internalReferenceCount[which] || isHostFunction(members[which]);
            if (isReferencedExternally)
                continue;
            ++numberOfClasses;
            for (unsigned member : members) {
                if (hasBody(constProgram.functions[member]))
                    drop(member);
            }
            removeUnreferencedFunctions();
        }
    }
    if (!mayBecomeUnread.isEmpty()) {
        Vector<uint8_t> isRead(FillWith { }, constProgram.variables.size(), static_cast<uint8_t>(0));
        Vector<uint8_t> storesToSingleVariable(FillWith { }, count, static_cast<uint8_t>(0));
        forEachIndexInParallel(count, [&](unsigned i) {
            static constexpr OpcodeSet variableAccesses { Opcode::GetModuleVariable, Opcode::PutModuleVariable };
            constProgram.functions.forEachIndexedValue(i, variableAccesses, [&](ValueID id) {
                auto& value = constProgram.functions[i].at(id);
                if (!mayBecomeUnread.get(value.variable().index()))
                    return;
                if (value.opcode == Opcode::GetModuleVariable)
                    isRead[value.variable().index()] = 1;
                else
                    storesToSingleVariable[i] = 1;
            });
        });
        for (unsigned variable : mayBecomeUnread)
            numberOfVariables += !isRead[variable];
        forEachIndexInParallel(count, [&](unsigned i) {
            if (!storesToSingleVariable[i])
                return;
            auto& function = program.functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::PutModuleVariable || !mayBecomeUnread.get(value.variable().index()) || isRead[value.variable().index()])
                        continue;
                    value.opcode = Opcode::Undefined;
                    value.children = { };
                    value.immediates = { Format::none, Format::none };
                }
            }
        });
    }
    report();
}

static void findInternalModuleExports(Program& program)
{
    auto& constProgram = std::as_const(program);
    if (!constProgram.usesOperation(Operations::RequireBuiltinModule) || constProgram.builtinModules.isEmpty())
        return;
    unsigned count = constProgram.functions.size();
    struct Location {
        unsigned block { 0 };
        unsigned index { 0 };
    };
    struct Exports {
        FunctionID code;
        Location returnedLocation;
        ValueID returned;
        VariableID holder;
        bool isInternal { false };
        bool hasEscaped { false };
    };
    using ExportsByModule = UncheckedKeyHashMap<uint32_t, Exports, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ExportsByModule exportsOf;
    BitVector isHolder(constProgram.variables.size());
    for (auto& [module, id] : constProgram.builtinModules) {
        auto& function = constProgram.at(id);
        if (!function.module)
            continue;
        Exports exports;
        exports.code = id;
        exports.isInternal = spanHasPrefix(constProgram.string(constProgram.modules[function.module.index()].path), asBytes("internal:"_s.span8()));
        unsigned numberOfReturns = 0;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (function.blocks[b].terminator.kind != TerminatorKind::Return)
                continue;
            ++numberOfReturns;
            exports.returned = function.blocks[b].terminator.value;
            exports.returnedLocation = { b, UINT_MAX };
        }
        if (numberOfReturns != 1 || !exports.returned)
            continue;
        if (auto& returned = function.at(exports.returned); returned.opcode == Opcode::GetModuleVariable) {
            exports.holder = returned.variable();
            isHolder.set(exports.holder.index());
        }
        exportsOf.add(module, exports);
    }
    if (exportsOf.isEmpty())
        return;

    auto requiredModule = [&](const Function& function, const Value& value) -> std::optional<uint32_t> {
        if (value.opcode != Opcode::Operation || constProgram.operationOf(value) != Operations::RequireBuiltinModule || value.arguments.size() != 1)
            return std::nullopt;
        auto& number = function.at(value.arguments[0]);
        if (number.opcode != Opcode::Integer || number.integer() < 0 || number.integer() >= UINT32_MAX)
            return std::nullopt;
        return static_cast<uint32_t>(number.integer());
    };
    struct Read {
        ValueID value;
        uint32_t module;
    };
    struct Found {
        Vector<Read> reads;
        Vector<Read> methodsCalled;
        Vector<uint32_t> escapingModules;
        Vector<std::pair<ValueID, uint32_t>> required;
    };
    Vector<Found> foundIn(count);
    Vector<uint32_t> numberOfStores(FillWith { }, constProgram.variables.size(), 0u);
    Vector<uint32_t> numberOfReads(FillWith { }, constProgram.variables.size(), 0u);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = constProgram.functions[i];
        Vector<std::pair<ValueID, uint32_t>, 8> required;
        static constexpr OpcodeSet variableAccessesAndOperations { Opcode::PutModuleVariable, Opcode::GetModuleVariable, Opcode::Operation };
        constProgram.functions.forEachIndexedValue(i, variableAccessesAndOperations, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::PutModuleVariable && isHolder.get(value.variable().index()))
                WTF::atomicExchangeAdd(&numberOfStores[value.variable().index()], 1u, std::memory_order_relaxed);
            else if (value.opcode == Opcode::GetModuleVariable && isHolder.get(value.variable().index()))
                WTF::atomicExchangeAdd(&numberOfReads[value.variable().index()], 1u, std::memory_order_relaxed);
            else if (auto module = requiredModule(function, value))
                required.append({ id, *module });
        });
        if (required.isEmpty())
            return;
        auto& found = foundIn[i];
        found.required.appendVector(required);
        auto moduleOfExports = [&](ValueID used) -> std::optional<uint32_t> {
            for (auto [exports, module] : required) {
                if (exports == used)
                    return module;
            }
            return std::nullopt;
        };
        auto hasEscaped = [&](ValueID used) {
            if (auto module = moduleOfExports(used))
                found.escapingModules.append(*module);
        };
        for (auto& block : function.blocks) {
            for (unsigned k = 0; k < block.values.size(); ++k) {
                auto& value = function.at(block.values[k]);
                if (value.opcode == Opcode::IsUndefinedOrNull)
                    continue;
                if (value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod) {
                    bool isByName = value.opcode == Opcode::CallMethod;
                    ValueID receiver = value.child(isByName ? 0 : 1);
                    ValueID named = isByName ? block.values[k] : value.child(0);
                    auto receiverModule = receiver ? moduleOfExports(receiver) : std::nullopt;
                    if (receiverModule && !value.arguments.contains(receiver) && (isByName || (function.at(named).opcode == Opcode::GetById && function.at(named).child(0) == receiver))) {
                        found.methodsCalled.append(Read { named, *receiverModule });
                        for (ValueID argument : value.arguments)
                            hasEscaped(argument);
                        continue;
                    }
                }
                if (value.opcode != Opcode::GetById) {
                    value.forEachChild(hasEscaped);
                    continue;
                }
                auto module = moduleOfExports(value.child(0));
                if (module && (block.terminator.kind != TerminatorKind::Invoke || k + 1 < block.values.size()))
                    found.reads.append(Read { block.values[k], *module });
            }
            if (block.terminator.value)
                hasEscaped(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    hasEscaped(argument);
            }
        }
    });
    for (auto& found : foundIn) {
        for (uint32_t module : found.escapingModules) {
            if (auto exports = exportsOf.find(module); exports != exportsOf.end())
                exports->value.hasEscaped = true;
        }
    }

    auto dominatesLocation = [](const Function& function, Location first, Location second) {
        if (first.block == second.block)
            return first.index < second.index;
        if (!first.block)
            return true;
        BitVector seen(function.blocks.size());
        Vector<unsigned, 16> worklist;
        worklist.append(0);
        seen.set(0);
        while (!worklist.isEmpty()) {
            unsigned current = worklist.takeLast();
            if (current == second.block)
                return false;
            for (auto& target : function.blocks[current].terminator.targets) {
                unsigned next = target.block.index();
                if (next != first.block && !seen.set(next))
                    worklist.append(next);
            }
        }
        return true;
    };
    auto keyOf = [](uint32_t module, StringID name) {
        return static_cast<uint64_t>(module) << 32 | name.index();
    };
    UncheckedKeyHashMap<uint64_t, FunctionID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> functionsExported;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> returnedFunctions;
    auto withoutUpcasts = [](const Function& function, ValueID id) {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
            id = function.at(id).child(0);
        return id;
    };
    unsigned numberOfModules = 0;
    for (auto& [module, exports] : exportsOf) {
        auto& function = constProgram.at(exports.code);
        ValueID object = exports.returned;
        ValueID objectStore;
        if (exports.holder) {
            if (numberOfStores[exports.holder.index()] != 1)
                continue;
            Location storeLocation;
            Location readLocation;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    if (values[k] == exports.returned)
                        readLocation = { b, k };
                    else if (function.at(values[k]).opcode == Opcode::PutModuleVariable && function.at(values[k]).variable() == exports.holder) {
                        objectStore = values[k];
                        storeLocation = { b, k };
                    }
                }
            }
            if (!objectStore || !dominatesLocation(function, storeLocation, readLocation))
                continue;
            object = function.at(objectStore).child(0);
        }
        if (auto& returnedValue = function.at(withoutUpcasts(function, object)); returnedValue.opcode == Opcode::FunctionReference) {
            returnedFunctions.add(module, returnedValue.function());
            continue;
        }
        if (!exports.isInternal || exports.hasEscaped || (exports.holder && numberOfReads[exports.holder.index()] != 1))
            continue;
        if (function.at(object).opcode != Opcode::NewObject && function.at(object).opcode != Opcode::NewObjectWithShape)
            continue;
        Vector<std::pair<StringID, ValueID>, 32> filled;
        bool isOnlyFilledAndReturned = true;
        for (unsigned b = 0; b < function.blocks.size() && isOnlyFilledAndReturned; ++b) {
            auto& block = function.blocks[b];
            for (unsigned k = 0; k < block.values.size() && isOnlyFilledAndReturned; ++k) {
                auto& value = function.at(block.values[k]);
                bool uses = false;
                value.forEachChild([&](ValueID child) {
                    uses |= child == object;
                });
                if (!uses || block.values[k] == objectStore)
                    continue;
                StringID name;
                ValueID stored;
                if (value.opcode == Opcode::Operation && constProgram.operationOf(value) == Operations::DefineField && value.arguments.size() == 3 && value.arguments[0] == object && function.at(value.arguments[1]).opcode == Opcode::String) {
                    name = function.at(value.arguments[1]).string();
                    stored = value.arguments[2];
                } else if (value.opcode == Opcode::PutField && value.child(0) == object && value.layout().index() < constProgram.layouts.size() && value.field() < constProgram.at(value.layout()).fields.size()) {
                    name = constProgram.at(value.layout()).fields[value.field()].name;
                    stored = value.child(1);
                }
                if (!name || !stored || stored == object || !dominatesLocation(function, Location { b, k }, exports.returnedLocation))
                    isOnlyFilledAndReturned = false;
                else
                    filled.append({ name, stored });
            }
            for (auto& target : block.terminator.targets)
                isOnlyFilledAndReturned &= !target.arguments.contains(object);
            if (block.terminator.value == object && (block.terminator.kind != TerminatorKind::Return || objectStore))
                isOnlyFilledAndReturned = false;
        }
        if (!isOnlyFilledAndReturned)
            continue;
        ++numberOfModules;
        for (auto& [name, stored] : filled) {
            ValueID origin = withoutUpcasts(function, stored);
            FunctionID exported = function.at(origin).opcode == Opcode::FunctionReference ? function.at(origin).function() : FunctionID();
            if (!functionsExported.add(keyOf(module, name), exported).isNewEntry)
                functionsExported.set(keyOf(module, name), FunctionID());
        }
    }
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> escapedModules;
    for (unsigned i = 0; i < count; ++i) {
        for (auto& call : foundIn[i].methodsCalled) {
            FunctionID callee = functionsExported.get(keyOf(call.module, constProgram.functions[i].at(call.value).string()));
            if (!callee || constProgram.at(callee).has(Format::FunctionHasThis))
                escapedModules.add(call.module);
        }
    }
    unsigned knownReadCount = 0;
    for (unsigned i = 0; i < count && !functionsExported.isEmpty(); ++i) {
        for (auto& read : foundIn[i].reads) {
            FunctionID exported = functionsExported.get(keyOf(read.module, constProgram.functions[i].at(read.value).string()));
            if (!exported || escapedModules.contains(read.module))
                continue;
            auto& value = program.functions[i].at(read.value);
            value.opcode = Opcode::FunctionReference;
            value.children = { };
            value.immediates = { exported.index(), Format::none };
            value.setPrediction({ });
            value.setPredictedClosure(exported);
            value.setPredictedByKey({ });
            value.receiverHasSingleStructure = false;
            value.predictedReceiverClassPlusOne = 0;
            ++knownReadCount;
        }
    }
    unsigned knownRequireCount = 0;
    for (unsigned i = 0; i < count && !returnedFunctions.isEmpty(); ++i) {
        for (auto [required, module] : foundIn[i].required) {
            FunctionID returnedValue = returnedFunctions.get(module);
            if (!returnedValue)
                continue;
            auto& function = program.functions[i];
            for (auto& block : function.blocks) {
                size_t index = block.values.find(required);
                if (index == notFound)
                    continue;
                if (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size())
                    break;
                Value closure;
                closure.opcode = Opcode::FunctionReference;
                closure.type = function.at(required).type;
                closure.rep = function.at(required).rep;
                closure.position = function.at(required).position;
                closure.inlineFrame = function.at(required).inlineFrame;
                closure.immediates = { returnedValue.index(), Format::none };
                closure.setPredictedClosure(returnedValue);
                ValueID id(function.values.size());
                function.values.append(WTF::move(closure));
                block.values.insert(index + 1, id);
                auto useClosure = [&](ValueID& used) {
                    if (used == required)
                        used = id;
                };
                for (auto& value : function.values)
                    value.forEachChild(useClosure);
                for (auto& other : function.blocks)
                    other.terminator.forEachChild(useClosure);
                ++knownRequireCount;
                break;
            }
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfModules, " internal modules return an object that is only read by name: ", knownReadCount, " reads of it are known to return a function of the program; ", returnedFunctions.size(), " runtime modules return a function: ", knownRequireCount, " requires of them resolve to it");
}

class ResultOfCallWithConstants {
public:
    ResultOfCallWithConstants(const Program& program, const Function& caller, FunctionID callee, const Value& call)
        : m_program(program)
        , m_callee(program.at(callee))
    {
        constexpr uint32_t disqualifyingFlags = Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsBuiltin | Format::FunctionIsModuleBody | Format::FunctionIsSloppy | Format::FunctionIsDeclaredGenericEntrypoint;
        if ((m_callee.kind != FunctionKind::Plain && m_callee.kind != FunctionKind::Arrow) || m_callee.has(disqualifyingFlags) || !m_callee.captureTypes.isEmpty() || m_callee.blocks.isEmpty() || m_callee.blocks[0].parameters.size() < call.arguments.size())
            return;
        auto& parameters = m_callee.blocks[0].parameters;
        for (unsigned i = 0; i < parameters.size(); ++i)
            m_known.add(parameters[i].index(), i < call.arguments.size() ? literal(caller, call.arguments[i]) : Constant { Constant::Undefined, 0, { }, { } });
        run();
    }

    FunctionID closure() const { return m_closure; }
    const Vector<FunctionID, 2>& functionsCaptured() const { return m_functionsCaptured; }

private:
    struct Constant {
        enum Kind : uint8_t { Unknown, Integer, String, Boolean, Undefined, Null, Closure, TypeName };
        Kind kind { Unknown };
        int64_t integer { 0 };
        StringID string;
        FunctionID function;
    };

    static ValueID withoutCasts(const Function& function, ValueID id)
    {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() != CastReason::Coerce)
            id = function.at(id).child(0);
        return id;
    }

    static Constant literal(const Function& function, ValueID id)
    {
        auto& value = function.at(withoutCasts(function, id));
        switch (value.opcode) {
        case Opcode::Integer:
            return { Constant::Integer, value.integer(), { }, { } };
        case Opcode::String:
            return { Constant::String, 0, value.string(), { } };
        case Opcode::Boolean:
            return { Constant::Boolean, value.boolean(), { }, { } };
        case Opcode::Undefined:
            return { Constant::Undefined, 0, { }, { } };
        case Opcode::Null:
            return { Constant::Null, 0, { }, { } };
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
            return { Constant::Closure, 0, { }, value.function() };
        default:
            return { };
        }
    }

    static std::optional<TypeofType> typeOf(const Constant& constant)
    {
        switch (constant.kind) {
        case Constant::Integer:
            return TypeofType::Number;
        case Constant::String:
            return TypeofType::String;
        case Constant::Boolean:
            return TypeofType::Boolean;
        case Constant::Undefined:
            return TypeofType::Undefined;
        case Constant::Null:
            return TypeofType::Object;
        case Constant::Closure:
            return TypeofType::Function;
        default:
            return std::nullopt;
        }
    }

    static ASCIILiteral nameOf(TypeofType type)
    {
        switch (type) {
        case TypeofType::Undefined:
            return "undefined"_s;
        case TypeofType::BigInt:
            return "bigint"_s;
        case TypeofType::Boolean:
            return "boolean"_s;
        case TypeofType::Number:
            return "number"_s;
        case TypeofType::String:
            return "string"_s;
        case TypeofType::Symbol:
            return "symbol"_s;
        case TypeofType::Object:
            return "object"_s;
        case TypeofType::Function:
            return "function"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::optional<bool> areStrictlyEqual(const Constant& a, const Constant& b) const
    {
        if (a.kind == Constant::Unknown || b.kind == Constant::Unknown)
            return std::nullopt;
        if (a.kind == Constant::TypeName || b.kind == Constant::TypeName) {
            auto& name = a.kind == Constant::TypeName ? a : b;
            auto& other = a.kind == Constant::TypeName ? b : a;
            if (other.kind == Constant::TypeName)
                return name.integer == other.integer;
            if (other.kind != Constant::String)
                return false;
            return equalSpans(nameOf(static_cast<TypeofType>(name.integer)).span8(), m_program.string(other.string));
        }
        if (a.kind != b.kind)
            return false;
        switch (a.kind) {
        case Constant::Integer:
        case Constant::Boolean:
            return a.integer == b.integer;
        case Constant::String:
            return equalSpans(m_program.string(a.string), m_program.string(b.string));
        case Constant::Undefined:
        case Constant::Null:
            return true;
        default:
            return std::nullopt;
        }
    }

    Constant evaluate(ValueID id, unsigned depth = 0) const
    {
        id = withoutCasts(m_callee, id);
        auto& value = m_callee.at(id);
        auto boolean = [](std::optional<bool> answer) {
            return answer ? Constant { Constant::Boolean, *answer, { }, { } } : Constant { };
        };
        if (depth == 8)
            return { };
        switch (value.opcode) {
        case Opcode::Parameter:
            return m_known.get(id.index());
        case Opcode::TypeOf:
            if (auto type = typeOf(evaluate(value.child(0), depth + 1)))
                return { Constant::TypeName, static_cast<int64_t>(*type), { }, { } };
            return { };
        case Opcode::TypeOfIs:
            if (auto type = typeOf(evaluate(value.child(0), depth + 1)))
                return boolean(*type == value.typeofType());
            return { };
        case Opcode::StrictEq:
            return boolean(areStrictlyEqual(evaluate(value.child(0), depth + 1), evaluate(value.child(1), depth + 1)));
        case Opcode::LogicalNot:
            if (Constant operand = evaluate(value.child(0), depth + 1); operand.kind == Constant::Boolean)
                return boolean(!operand.integer);
            return { };
        default:
            return literal(m_callee, id);
        }
    }

    void run()
    {
        BlockID block(0u);
        for (unsigned steps = 0; steps < 64; ++steps) {
            auto& terminator = m_callee.at(block).terminator;
            const Target* taken = nullptr;
            switch (terminator.kind) {
            case TerminatorKind::Jump:
                taken = &terminator.targets[0];
                break;
            case TerminatorKind::Branch: {
                Constant condition = evaluate(terminator.value);
                if (condition.kind != Constant::Boolean)
                    return;
                taken = &terminator.targets[condition.integer ? 0 : 1];
                break;
            }
            case TerminatorKind::Return:
                if (Constant returned = evaluate(terminator.value); returned.kind == Constant::Closure) {
                    m_closure = returned.function;
                    if (auto& created = m_callee.at(withoutCasts(m_callee, terminator.value)); created.opcode == Opcode::NewClosure) {
                        for (ValueID captured : created.arguments) {
                            Constant held = evaluate(captured);
                            m_functionsCaptured.append(held.kind == Constant::Closure ? held.function : FunctionID());
                        }
                    }
                }
                return;
            default:
                return;
            }
            auto& parameters = m_callee.at(taken->block).parameters;
            if (parameters.size() != taken->arguments.size())
                return;
            Vector<Constant, 4> passed;
            for (ValueID argument : taken->arguments)
                passed.append(evaluate(argument));
            for (unsigned i = 0; i < parameters.size(); ++i)
                m_known.set(parameters[i].index(), passed[i]);
            block = taken->block;
        }
    }

    const Program& m_program;
    const Function& m_callee;
    UncheckedKeyHashMap<uint32_t, Constant, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_known;
    FunctionID m_closure;
    Vector<FunctionID, 2> m_functionsCaptured;
};

static unsigned findFunctionsInModuleVariables(Program& program)
{
    auto& constProgram = std::as_const(program);
    unsigned count = constProgram.functions.size();
    struct Source {
        enum Kind : uint8_t { Same, Closure, Parameter, Nil, Unknown };
        Kind kind { Unknown };
        FunctionID function;
        uint32_t parameter { 0 };
        Vector<FunctionID, 2> functionsCaptured { };
    };
    using Incoming = Vector<Vector<const Target*, 2>>;
    auto forEachSource = [&](const Function& function, Incoming& incoming, ValueID stored, VariableID variable, auto&& functor) {
        Vector<ValueID, 8> worklist { stored };
        Vector<ValueID, 8> joins;
        for (unsigned steps = 0; !worklist.isEmpty(); ++steps) {
            if (steps == 32) {
                functor(Source { });
                return;
            }
            ValueID id = worklist.takeLast();
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() != CastReason::Coerce)
                id = function.at(id).child(0);
            auto& origin = function.at(id);
            switch (origin.opcode) {
            case Opcode::FunctionReference:
            case Opcode::NewClosure:
                functor(Source { Source::Closure, origin.function(), 0 });
                continue;
            case Opcode::Undefined:
            case Opcode::Null:
                functor(Source { Source::Nil, { }, 0 });
                continue;
            case Opcode::CallDirect:
            case Opcode::Call: {
                FunctionID callee = origin.opcode == Opcode::CallDirect ? origin.function() : FunctionID();
                if (!callee) {
                    ValueID called = origin.child(0);
                    while (function.at(called).opcode == Opcode::Cast && function.at(called).castReason() != CastReason::Coerce)
                        called = function.at(called).child(0);
                    if (function.at(called).opcode == Opcode::FunctionReference)
                        callee = function.at(called).function();
                    else if (function.at(called).opcode == Opcode::GetModuleVariable) {
                        if (auto& held = constProgram.variables[function.at(called).variable().index()]; held.singleFunctionIsProven)
                            callee = held.singleFunction;
                    }
                }
                if (!callee)
                    break;
                if (ResultOfCallWithConstants result(constProgram, function, callee, origin); result.closure()) {
                    functor(Source { Source::Closure, result.closure(), 0, result.functionsCaptured() });
                    continue;
                }
                break;
            }
            case Opcode::GetModuleVariable: {
                if (origin.variable() == variable) {
                    functor(Source { Source::Same, { }, 0 });
                    continue;
                }
                auto& other = constProgram.variables[origin.variable().index()];
                if (!other.singleFunctionIsProven)
                    break;
                functor(Source { Source::Closure, other.singleFunction, 0, other.functionsCapturedBySingleFunction });
                continue;
            }
            case Opcode::Parameter:
                if (!origin.immediates[0]) {
                    functor(Source { Source::Parameter, { }, origin.immediates[1] });
                    continue;
                }
                if (joins.contains(id))
                    continue;
                joins.append(id);
                if (incoming.isEmpty()) {
                    incoming.grow(function.blocks.size());
                    for (auto& block : function.blocks) {
                        for (auto& target : block.terminator.targets)
                            incoming[target.block.index()].append(&target);
                    }
                }
                for (auto* target : incoming[origin.immediates[0]]) {
                    if (origin.immediates[1] < target->arguments.size())
                        worklist.append(target->arguments[origin.immediates[1]]);
                }
                continue;
            default:
                break;
            }
            functor(Source { });
        }
    };
    struct Held {
        FunctionID function;
        Vector<FunctionID, 2> functionsCaptured;
        bool mayHoldOtherValues { false };
        bool mayHoldNullish { false };
        void note(const Source& source)
        {
            if (source.kind == Source::Same)
                return;
            if (source.kind == Source::Nil) {
                mayHoldNullish = true;
                return;
            }
            if (source.kind != Source::Closure || (function && function != source.function))
                mayHoldOtherValues = true;
            else {
                if (!function)
                    functionsCaptured = source.functionsCaptured;
                else if (functionsCaptured != source.functionsCaptured)
                    functionsCaptured.clear();
                function = source.function;
            }
        }
    };
    Vector<Held> held(constProgram.variables.size());
    Vector<Vector<std::pair<VariableID, Source>>> storesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = constProgram.functions[i];
        Incoming incoming;
        static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
        constProgram.functions.forEachIndexedValue(i, stores, [&](ValueID id) {
            auto& value = function.at(id);
            auto& variable = constProgram.variables[value.variable().index()];
            if (variable.singleFunction || (variable.type && !constProgram.at(variable.type).anyFunction))
                return;
            forEachSource(function, incoming, value.child(0), value.variable(), [&](Source source) {
                storesIn[i].append(std::pair { value.variable(), source });
            });
        });
    });
    BitVector hasOtherEntries(count);
    for (auto& layout : constProgram.layouts) {
        if (layout.constructor)
            hasOtherEntries.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                hasOtherEntries.set(method.function.index());
        }
    }
    for (auto& module : constProgram.builtinModules)
        hasOtherEntries.set(module.second.index());
    for (auto& builtin : constProgram.builtinFunctions)
        hasOtherEntries.set(builtin.function.index());
    for (FunctionID function : constProgram.preludeFunctions) {
        if (function)
            hasOtherEntries.set(function.index());
    }
    auto parametersMatchDirectCallArguments = [&](unsigned i) {
        auto& function = constProgram.functions[i];
        constexpr uint32_t disqualifyingFlags = Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsBuiltin | Format::FunctionIsModuleBody | Format::FunctionIsSloppy | Format::FunctionIsDeclaredGenericEntrypoint;
        return (function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow) && !function.has(disqualifyingFlags) && function.captureTypes.isEmpty();
    };
    auto mayHaveOtherEntries = [&](unsigned i) {
        return constProgram.functions[i].has(Format::FunctionIsUsedAsValue | FunctionHasUnknownCallers | FunctionIsEnteredByValue) || hasOtherEntries.get(i) || constProgram.isEntry(FunctionID(i));
    };
    struct Passed {
        VariableID variable;
        uint32_t parameter;
    };
    using ForwardedArguments = UncheckedKeyHashMap<uint32_t, Vector<Passed, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ForwardedArguments followed;
    ForwardedArguments toTrack;
    auto follow = [&](unsigned function, uint32_t parameter, VariableID variable) {
        if (held[variable.index()].mayHoldOtherValues)
            return;
        if (!parametersMatchDirectCallArguments(function) || mayHaveOtherEntries(function)) {
            held[variable.index()].mayHoldOtherValues = true;
            return;
        }
        auto& known = followed.add(function, Vector<Passed, 1>()).iterator->value;
        if (known.containsIf([&](auto& passed) { return passed.variable == variable && passed.parameter == parameter; }))
            return;
        known.append(Passed { variable, parameter });
        toTrack.add(function, Vector<Passed, 1>()).iterator->value.append(Passed { variable, parameter });
    };
    for (auto& storesInFunction : storesIn) {
        for (auto& [variable, source] : storesInFunction) {
            if (source.kind != Source::Parameter)
                held[variable.index()].note(source);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        for (auto& [variable, source] : storesIn[i]) {
            if (source.kind == Source::Parameter)
                follow(i, source.parameter, variable);
        }
    }
    for (unsigned round = 0; round < 3 && !toTrack.isEmpty(); ++round) {
        ForwardedArguments setters = std::exchange(toTrack, ForwardedArguments());
        BitVector isSetter(count);
        for (auto& entry : setters)
            isSetter.set(entry.key);
        struct Seen {
            FunctionID setter;
            VariableID variable;
            Source source;
        };
        Vector<Vector<Seen>> seenIn(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = constProgram.functions[i];
            Incoming incoming;
            auto isValue = [&](FunctionID named) {
                if (named && isSetter.get(named.index()))
                    seenIn[i].append(Seen { named, { }, { } });
            };
            static constexpr OpcodeSet references { Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::NewObjectWithShape, Opcode::CallDirect };
            auto visit = [&](ValueID id) {
                auto& value = function.at(id);
                switch (value.opcode) {
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                    isValue(value.function());
                    break;
                case Opcode::NewObjectWithShape:
                    if (value.allocation() < function.allocations.size())
                        isValue(function.allocations[value.allocation()].constructedBy);
                    break;
                case Opcode::CallDirect: {
                    if (!isSetter.get(value.function().index()))
                        break;
                    for (auto& passed : setters.find(value.function().index())->value) {
                        if (passed.parameter >= value.arguments.size()) {
                            seenIn[i].append(Seen { value.function(), passed.variable, Source { } });
                            continue;
                        }
                        forEachSource(function, incoming, value.arguments[passed.parameter], passed.variable, [&](Source source) {
                            seenIn[i].append(Seen { value.function(), passed.variable, source });
                        });
                    }
                    break;
                }
                default:
                    break;
                }
            };
            if (!constProgram.functions.mayContainPredictedClosure(i)) {
                constProgram.functions.forEachIndexedValue(i, references, visit);
                return;
            }
            for (auto& block : function.blocks) {
                for (ValueID id : block.parameters)
                    isValue(function.at(id).predictedClosure());
                for (ValueID id : block.values) {
                    isValue(function.at(id).predictedClosure());
                    if (references.contains(function.at(id).opcode))
                        visit(id);
                }
            }
        });
        for (unsigned i = 0; i < count; ++i) {
            for (auto& seen : seenIn[i]) {
                if (!seen.variable) {
                    for (auto& passed : setters.find(seen.setter.index())->value)
                        held[passed.variable.index()].mayHoldOtherValues = true;
                } else if (seen.source.kind == Source::Parameter)
                    follow(i, seen.source.parameter, seen.variable);
                else
                    held[seen.variable.index()].note(seen.source);
            }
        }
    }
    for (auto& entry : toTrack) {
        for (auto& passed : entry.value)
            held[passed.variable.index()].mayHoldOtherValues = true;
    }
    unsigned singleCount = 0;
    for (unsigned i = 0; i < held.size(); ++i) {
        auto& variable = program.variables[i];
        if (variable.singleFunction || !held[i].function || held[i].mayHoldOtherValues || variable.startsZero)
            continue;
        variable.singleFunction = held[i].function;
        variable.functionsCapturedBySingleFunction = WTF::move(held[i].functionsCaptured);
        if (variable.startsUndefined || held[i].mayHoldNullish)
            variable.singleFunctionOrNullish = true;
        else
            variable.singleFunctionIsProven = true;
        ++singleCount;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: whole-program store analysis: ", singleCount, " more module variables only hold closures of a single function");
    return singleCount;
}

static void findBuiltinsInVariables(Program& program)
{
    enum class Location : uint8_t { Field, Variable, EntryParameter };
    struct Stored {
        Location location;
        uint32_t owner;
        uint32_t index;
        ValueID value;
    };
    auto predictionFor = [&](const Stored& stored) -> PredictedBuiltins& {
        switch (stored.location) {
        case Location::Field:
            return program.layouts[stored.owner].fields[stored.index].predictedBuiltins;
        case Location::Variable:
            return program.variables[stored.index].predictedBuiltins;
        case Location::EntryParameter:
            break;
        }
        return program.functions.atKeepingValueIndex(stored.owner).predictedBuiltinsOfEntryParameters[stored.index];
    };
    unsigned count = program.functions.size();
    Vector<Vector<Stored>> candidates(count);
    Vector<Vector<Stored>> fresh(count);
    Vector<Vector<Stored>> others(count);
    Vector<uint8_t> isActive(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<unsigned> activeFunctions;
    auto sort = [&](unsigned i, Stored stored) {
        isActive[i] = true;
        auto& function = std::as_const(program).functions[i];
        while (function.at(stored.value).opcode == Opcode::Cast)
            stored.value = function.at(stored.value).child(0);
        switch (function.at(stored.value).opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
            break;
        case Opcode::Parameter:
        case Opcode::GetGlobal:
        case Opcode::GetById:
        case Opcode::GetField:
        case Opcode::GetModuleVariable:
            fresh[i].append(stored);
            break;
        default:
            others[i].append(stored);
            break;
        }
    };
    constexpr PredictedBuiltins holdsSomethingElse { { }, { }, true };
    auto takeFreshCandidates = [&] {
        activeFunctions = findIndicesInParallel(count, [&](unsigned i) {
            return isActive[i];
        });
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            for (auto& stored : others[activeFunctions[k]])
                WTF::atomicStore(&predictionFor(stored).hasTooMany, true, std::memory_order_relaxed);
        });
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            unsigned i = activeFunctions[k];
            for (auto& stored : fresh[i]) {
                if (!WTF::atomicLoad(&predictionFor(stored).hasTooMany, std::memory_order_relaxed))
                    candidates[i].append(stored);
            }
            fresh[i].shrink(0);
        });
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            unsigned i = activeFunctions[k];
            for (auto& stored : others[i]) {
                auto& prediction = predictionFor(stored);
                if (WTF::atomicExchange(&prediction.hasTooMany, false, std::memory_order_relaxed)) {
                    prediction.first = { };
                    prediction.second = { };
                }
            }
            others[i].shrink(0);
        });
    };
    Vector<Vector<uint32_t>> queriedEntryParameters(count);
    struct Site {
        uint32_t value;
        uint32_t callee;
    };
    Vector<Vector<Site>> sitesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet sites { Opcode::CallDirect, Opcode::NewClosure, Opcode::NewClosureOnce };
        if (std::as_const(program).functions.mayContain(i, sites) && !function.isGenericEntrypoint()) {
            std::as_const(program).functions.forEachValue(i, sites, [&](ValueID id) {
                sitesIn[i].append(Site { id.index(), function.at(id).function().index() });
            });
        }
        static constexpr OpcodeSet storesAndCalls { Opcode::PutField, Opcode::PutModuleVariable, Opcode::Call, Opcode::Construct, Opcode::CallMethod };
        std::as_const(program).functions.forEachIndexedValue(i, storesAndCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::PutField) {
                auto& layout = std::as_const(program).at(value.layout());
                if (layout.kind == LayoutKind::Environment && !layout.isCoroutineFrame)
                    sort(i, { Location::Field, value.layout().index(), value.field(), value.child(1) });
            } else if (value.opcode == Opcode::PutModuleVariable)
                sort(i, { Location::Variable, 0, value.variable().index(), value.child(0) });
            else if (value.opcode == Opcode::Call || value.opcode == Opcode::Construct || (value.opcode == Opcode::CallMethod && program.builtinName(value.string()))) {
                ValueID called = value.child(0);
                while (function.at(called).opcode == Opcode::Cast)
                    called = function.at(called).child(0);
                if (auto& parameter = function.at(called); parameter.opcode == Opcode::Parameter && !parameter.parameterBlock().index()) {
                    isActive[i] = true;
                    queriedEntryParameters[i].append(parameter.parameterIndex());
                }
            }
        });
    });
    takeFreshCandidates();

    Vector<Vector<PredictedBuiltins>> found(count);
    Vector<Vector<PredictedBuiltins*>> locations(count);
    struct Candidate {
        PredictedBuiltins* location;
        uint32_t function;
        uint32_t index;
    };
    constexpr unsigned numberOfShares = 64;
    std::array<Vector<Candidate>, numberOfShares> shares;
    Vector<BitVector> trackedEntryParameters(count);
    BitVector functionsToTrack(count);
    unsigned numberOfPlaces = 0;
    bool sharesAreCurrent = false;
    Vector<uint8_t> tracksMoreParameters;
    for (unsigned round = 0; round < 8; ++round) {
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            unsigned i = activeFunctions[k];
            locations[i].shrink(0);
            if (candidates[i].isEmpty())
                return;
            PredictedBuiltinsQuery query { program, std::as_const(program).functions[i], true, &queriedEntryParameters[i] };
            found[i].shrink(0);
            for (auto& stored : candidates[i]) {
                found[i].append(query.ask(stored.value));
                locations[i].append(&predictionFor(stored));
            }
        });
        if (!std::exchange(sharesAreCurrent, true)) {
            for (auto& share : shares)
                share.shrink(0);
            for (unsigned i : activeFunctions) {
                for (uint32_t k = 0; k < locations[i].size(); ++k)
                    shares[PtrHash<PredictedBuiltins*>::hash(locations[i][k]) % numberOfShares].append(Candidate { locations[i][k], i, k });
            }
        }
        std::array<unsigned, numberOfShares> locationCounts { };
        std::array<bool, numberOfShares> hasChanged { };
        forEachIndexInParallel(numberOfShares, [&](unsigned number) {
            auto& share = shares[number];
            Vector<PredictedBuiltins> before(share.size(), [&](size_t k) {
                return *share[k].location;
            });
            for (auto& candidate : share)
                *candidate.location = { };
            for (auto& candidate : share) {
                auto& one = found[candidate.function][candidate.index];
                candidate.location->add(one ? one : holdsSomethingElse);
            }
            for (auto& candidate : share) {
                if (candidate.location->hasTooMany)
                    *candidate.location = { };
            }
            for (size_t k = 0; k < share.size(); ++k) {
                hasChanged[number] |= *share[k].location != before[k];
                locationCounts[number] += !!*share[k].location;
            }
        });
        bool changed = false;
        numberOfPlaces = 0;
        for (unsigned number = 0; number < numberOfShares; ++number) {
            changed |= hasChanged[number];
            numberOfPlaces += locationCounts[number];
        }

        functionsToTrack.clearAll();
        tracksMoreParameters.fill(0, activeFunctions.size());
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            unsigned i = activeFunctions[k];
            for (uint32_t parameter : queriedEntryParameters[i]) {
                if (trackedEntryParameters[i].set(parameter))
                    continue;
                tracksMoreParameters[k] = 1;
                auto& expected = program.functions.atKeepingValueIndex(i).predictedBuiltinsOfEntryParameters;
                if (expected.size() <= parameter)
                    expected.grow(parameter + 1);
            }
            queriedEntryParameters[i].shrink(0);
        });
        for (unsigned k = 0; k < activeFunctions.size(); ++k) {
            if (tracksMoreParameters[k])
                functionsToTrack.set(activeFunctions[k]);
        }
        if (!functionsToTrack.isEmpty()) {
            changed = true;
            sharesAreCurrent = false;
            forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
                candidates[activeFunctions[k]].removeAllMatching([&](auto& stored) {
                    return stored.location == Location::EntryParameter && functionsToTrack.get(stored.owner);
                });
            });
            forEachIndexInParallel(count, [&](unsigned i) {
                auto& function = std::as_const(program).functions[i];
                for (auto [index, callee] : sitesIn[i]) {
                    if (!functionsToTrack.get(callee))
                        continue;
                    auto& value = function.values[index];
                    for (size_t parameter : trackedEntryParameters[callee]) {
                        if (parameter < value.arguments.size())
                            sort(i, { Location::EntryParameter, callee, static_cast<uint32_t>(parameter), value.arguments[parameter] });
                    }
                }
            });
            takeFreshCandidates();
        }
        if (!changed)
            break;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPlaces, " stores to variables and parameters are predicted to store a built-in");
    destroyOnHelperThread(std::tuple { WTF::move(sitesIn), WTF::move(candidates), WTF::move(fresh), WTF::move(others), WTF::move(queriedEntryParameters), WTF::move(found), WTF::move(locations), WTF::move(trackedEntryParameters) });
}

static void findComparedBuiltinCells(Program& program)
{
    auto& builtins = Builtins::singleton();
    static constexpr OpcodeSet guards { Opcode::IsBuiltinFunction };
    Lock lock;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        std::as_const(program).functions.forEachValue(i, guards, [&](ValueID id) {
            auto& value = std::as_const(program).functions[i].at(id);
            BuiltinName global = value.comparedGlobal();
            Locker locker { lock };
            program.comparedBuiltinCells.set(global ? builtins.cellOf(global) : builtins.cellOf(value.builtin()));
        });
    });
}

static void findFunctionsWithClosureProperties(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet propertyStores { Opcode::PutById, Opcode::PutByVal };
        std::as_const(program).functions.forEachIndexedValue(i, propertyStores, [&](ValueID id) {
            auto& object = function.at(originThroughTrivialParameters(function, function.at(id).child(0)));
            FunctionID given;
            switch (object.opcode) {
            case Opcode::NewClosure:
            case Opcode::NewClosureOnce:
                given = object.function();
                break;
            case Opcode::GetField:
                given = std::as_const(program).at(object.layout()).fields[object.field()].singleFunction;
                break;
            case Opcode::GetModuleVariable:
                given = program.variables[object.variable().index()].singleFunction;
                break;
            default:
                break;
            }
            if (given)
                WTF::atomicStore(&program.functions.atKeepingValueIndex(given.index()).closuresHaveOwnProperties, true, std::memory_order_relaxed);
        });
    });
}

static void findInitialStructures(Program& program, Vector<String>& malformed)
{
    struct Found {
        Vector<Program::InitialStructure> rows;
        Vector<uint32_t> allocationSites;
        Vector<uint32_t> storesWithInitialStructure;
        Vector<String> complaints;
    };
    unsigned count = program.functions.size();
    Vector<Found> foundIn(count);
    forEachIndexInParallel(count, [&](unsigned index) {
        static constexpr OpcodeSet storesByName { Opcode::PutById };
        if (!program.functions.mayContain(index, storesByName))
            return;
        auto& function = program.functions.atKeepingValueIndex(index);
        auto& found = foundIn[index];
        if (function.blocks.isEmpty())
            return;
        auto complain = [&](ASCIILiteral what) {
            found.complaints.append(makeString(what, String::fromUTF8(program.string(function.name))));
        };
        UncheckedKeyHashMap<uint32_t, Vector<StringID, 8>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rows;
        auto note = [&](auto& row, const Value& store) {
            if (row.size() <= store.knownSlot())
                row.grow(store.knownSlot() + 1);
            StringID& name = row[store.knownSlot()];
            if (name && name != store.string())
                complain("two names share one slot of a fully initialized object in "_s);
            name = store.string();
        };
        Vector<StringID, 8> thisStructure;
        Vector<std::pair<uint32_t, uint32_t>, 8> storesAndAllocations;
        ValueID self = function.has(Format::FunctionHasThis) && function.captureTypes.size() < function.blocks[0].parameters.size() ? function.blocks[0].parameters[function.captureTypes.size()] : ValueID();
        std::as_const(program).functions.forEachIndexedValue(index, storesByName, [&](ValueID id) {
            auto& store = function.at(id);
            if (!store.isStoreIntoInitialStructure() && !store.isFirstStoreToThis())
                return;
            if (store.initialStructureOfFirstStore() || store.firstStoreAttributes())
                complain("a first store carries more than its slot in "_s);
            ValueID object = store.child(0);
            while (object.index() < function.values.size()) {
                auto& origin = function.at(object);
                if (origin.opcode != Opcode::Cast || origin.castReason() == CastReason::As || origin.castReason() == CastReason::Flow || origin.castReason() == CastReason::Coerce)
                    break;
                object = origin.child(0);
            }
            if (store.isFirstStoreToThis()) {
                if (!self || object != self)
                    complain("a store in the structure of this targets another object in "_s);
                note(thisStructure, store);
                return;
            }
            if (object.index() < function.values.size() && function.at(object).opcode == Opcode::NewObject) {
                note(rows.ensure(object.index(), [] { return Vector<StringID, 8>(); }).iterator->value, store);
                storesAndAllocations.append({ id.index(), object.index() });
            } else
                complain("a first store has no allocation in "_s);
        });
        if (!thisStructure.isEmpty()) {
            Program::InitialStructure row;
            row.isForThis = true;
            row.names = thisStructure;
            if (row.names.contains(StringID()))
                complain("the structure has a gap: this of "_s);
            for (size_t i = 0; i < row.names.size(); ++i) {
                if (row.names.find(row.names[i]) != i)
                    complain("one name has two slots of this in "_s);
            }
            for (unsigned k = 0; k < function.values.size(); ++k) {
                if (!function.values[k].isFirstStoreToThis())
                    continue;
                function.values[k].setInitialStructureOfFirstStore(1 + found.rows.size());
                found.storesWithInitialStructure.append(k);
            }
            found.rows.append(WTF::move(row));
        }
        if (rows.isEmpty())
            return;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbers;
        for (auto& [allocationSite, names] : rows) {
            Program::InitialStructure row;
            while (row.firstSlot < names.size() && !names[row.firstSlot])
                ++row.firstSlot;
            row.names.append(names.subspan(row.firstSlot));
            if (row.names.contains(StringID()))
                complain("the structure has a gap: a literal of "_s);
            for (size_t i = 0; i < row.names.size(); ++i) {
                if (row.names.find(row.names[i]) != i)
                    complain("one name has two slots of a fully initialized object in "_s);
            }
            numbers.add(allocationSite, found.rows.size());
            function.values[allocationSite].immediates[0] = found.rows.size();
            found.allocationSites.append(allocationSite);
            found.rows.append(WTF::move(row));
        }
        for (auto [store, allocationSite] : storesAndAllocations) {
            function.values[store].setInitialStructureOfFirstStore(1 + numbers.get(allocationSite));
            found.storesWithInitialStructure.append(store);
        }
    });
    Vector<uint32_t> firstEvaluationOf(count);
    for (unsigned index = 0; index < count; ++index) {
        firstEvaluationOf[index] = program.initialStructures.size();
        program.initialStructures.appendVector(WTF::move(foundIn[index].rows));
        malformed.appendVector(WTF::move(foundIn[index].complaints));
    }
    forEachIndexInParallel(count, [&](unsigned index) {
        auto& found = foundIn[index];
        uint32_t first = firstEvaluationOf[index];
        if (!first || (found.allocationSites.isEmpty() && found.storesWithInitialStructure.isEmpty()))
            return;
        auto& function = program.functions.atKeepingValueIndex(index);
        for (uint32_t allocationSite : found.allocationSites)
            function.values[allocationSite].immediates[0] += first;
        for (uint32_t store : found.storesWithInitialStructure)
            function.values[store].setInitialStructureOfFirstStore(function.values[store].initialStructureOfFirstStore() + first);
    });
}

Vector<size_t, 6> fieldsInitializedWithAllocation(const Program& program, const Function& function, const Block& block, size_t allocation, bool isCreateThis, bool hasNullPrototype)
{
    static constexpr unsigned maxFields = 64;
    static constexpr unsigned maxLiteralFields = 1024;
    auto isDefineField = [](const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
    };
    auto& values = block.values;
    Vector<size_t, 6> fields;
    for (size_t k = allocation + 1; k < values.size() && fields.size() < (isCreateThis ? maxFields : maxLiteralFields); ++k) {
        auto& value = function.at(values[k]);
        auto memberName = [&](const Value& member) {
            return member.opcode == Opcode::PutById ? member.string() : function.at(member.arguments[1]).string();
        };
        bool isDefinition = isDefineField(value) && value.arguments[0] == values[allocation] && value.arguments[2] != values[allocation] && function.at(value.arguments[1]).opcode == Opcode::String;
        bool isStore = isCreateThis && value.opcode == Opcode::PutById && !value.isInitialStore() && value.child(0) == values[allocation] && value.child(1) != values[allocation];
        bool isDefiningStore = false;
        if (!isCreateThis && value.opcode == Opcode::PutById && !value.isInitialStore() && value.child(0) == values[allocation] && value.child(1) != values[allocation]) {
            auto name = program.string(value.string());
            isDefiningStore = !name.empty() && !isASCIIDigit(name[0]) && (hasNullPrototype || !equalSpans(name, asBytes("__proto__"_s.span8())));
        }
        if (isDefinition || isStore || isDefiningStore) {
            bool isAlreadyAvailable = std::ranges::any_of(fields, [&](size_t earlier) {
                return memberName(function.at(values[earlier])) == memberName(value);
            });
            if (isAlreadyAvailable)
                break;
            fields.append(k);
            continue;
        }
        bool dependsOnNothing = value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String
            || value.opcode == Opcode::NewArray || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
        if (isCreateThis && !dependsOnNothing)
            break;
        bool usesObject = false;
        value.forEachChild([&](ValueID child) {
            usesObject |= child == values[allocation];
        });
        if (usesObject)
            break;
    }
    if (block.terminator.kind == TerminatorKind::Invoke && !fields.isEmpty() && fields.last() + 1 == values.size())
        fields.removeLast();
    return fields;
}

bool isAllocatedWithInitialStructure(const Program& program, const Function& function, const Block& block, size_t allocation)
{
    auto& row = program.initialStructures[function.at(block.values[allocation]).initialStructure()];
    Vector<StringID, 6> names;
    for (size_t index : fieldsInitializedWithAllocation(program, function, block, allocation, false, false)) {
        auto& field = function.at(block.values[index]);
        names.append(field.opcode == Opcode::PutById ? field.string() : function.at(field.arguments[1]).string());
    }
    bool isGapless = names.size() >= row.firstSlot;
    for (size_t i = 0; isGapless && i < row.names.size(); ++i)
        isGapless = row.firstSlot + i < names.size() ? names[row.firstSlot + i] == row.names[i] : !names.contains(row.names[i]);
    return isGapless;
}

static void finalizeInitialStructures(Program& program)
{
    if (Options::validateAOTTypedFacts())
        return;
    Vector<unsigned> notAllocatedWithInitialStructureCount;
    notAllocatedWithInitialStructureCount.fill(0, program.initialStructures.size());
    Vector<unsigned> withoutSlotsCount;
    withoutSlotsCount.fill(0, program.initialStructures.size());
    static constexpr OpcodeSet allocationSites { Opcode::NewObject };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.functions.mayContain(i, allocationSites))
            return;
        auto& function = std::as_const(program).functions[i];
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (!value.hasInitialStructure())
                    continue;
                if (Options::auditAOTTypedAssertions() || !isAllocatedWithInitialStructure(program, function, block, index))
                    WTF::atomicExchangeAdd(&notAllocatedWithInitialStructureCount[value.initialStructure()], 1u, std::memory_order_relaxed);
            }
        }
        size_t allocationSites = std::ranges::count_if(function.values, [](const Value& value) { return value.hasInitialStructure(); });
        if (2 * allocationSites <= maxFunctionSlots())
            return;
        for (auto& value : function.values) {
            if (value.hasInitialStructure())
                WTF::atomicExchangeAdd(&withoutSlotsCount[value.initialStructure()], 1u, std::memory_order_relaxed);
        }
    });
    unsigned notAllocatedWithInitialStructure = 0;
    unsigned withoutSlots = 0;
    for (unsigned row = 0; row < program.initialStructures.size(); ++row) {
        if (!program.initialStructures[row].isWhole || (!notAllocatedWithInitialStructureCount[row] && !withoutSlotsCount[row]))
            continue;
        ++(notAllocatedWithInitialStructureCount[row] ? notAllocatedWithInitialStructure : withoutSlots);
        program.initialStructures[row].isWhole = false;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: rows of births: ", program.initialStructures.size(), "; not whole: ", notAllocatedWithInitialStructure, " not born by their rows, ", withoutSlots, " without slots");
}

static void clearLazyClosureSlots(Program& program)
{
    Vector<unsigned> possiblyLazyCount;
    possiblyLazyCount.fill(0, program.strings.size());
    std::atomic<unsigned> numberOfPlaces { 0 };
    static constexpr OpcodeSet closures { Opcode::NewClosure };
    static constexpr OpcodeSet accessesByName { Opcode::GetById, Opcode::PutById };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        unsigned localLocations = 0;
        auto withoutUpcasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                id = function.at(id).child(0);
            return id;
        };
        auto note = [&](const Value& store, ValueID stored) {
            auto& closure = function.at(stored);
            if (closure.opcode == Opcode::NewClosure && closure.arguments.size() == 1 && withoutUpcasts(closure.arguments[0]) == withoutUpcasts(store.child(0)))
                WTF::atomicExchangeAdd(&possiblyLazyCount[store.string().index()], 1u, std::memory_order_relaxed);
        };
        if (!program.functions.mayContain(i, closures)) {
            std::as_const(program).functions.forEachIndexedValue(i, accessesByName, [&](ValueID id) {
                localLocations += function.at(id).isAtKnownSlot();
            });
            numberOfPlaces += localLocations;
            return;
        }
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                localLocations += value.isAtKnownSlot();
                if (value.opcode == Opcode::PutById && !value.isInitialStore())
                    note(value, value.child(1));
            }
            for (auto& target : block.terminator.targets) {
                auto& to = function.at(target.block);
                if (to.values.isEmpty() || function.at(to.values.first()).opcode != Opcode::PutById)
                    continue;
                auto& store = function.at(to.values.first());
                size_t index = to.parameters.find(store.child(1));
                if (index < target.arguments.size())
                    note(store, target.arguments[index]);
            }
        }
        numberOfPlaces += localLocations;
    });
    if (!numberOfPlaces)
        return;
    std::atomic<unsigned> clearedCount { 0 };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        Vector<ValueID, 8> toClear;
        std::as_const(program).functions.forEachIndexedValue(i, accessesByName, [&](ValueID id) {
            auto& value = std::as_const(program).functions[i].at(id);
            if (value.isAtKnownSlot() && possiblyLazyCount[value.string().index()])
                toClear.append(id);
        });
        if (toClear.isEmpty())
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        for (ValueID id : toClear) {
            auto& value = function.at(id);
            value.immediates[1] = value.isStoreIntoInitialStructure() ? value.immediates[1] | Value::usesNameLookup : Format::none;
        }
        clearedCount += toClear.size();
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", clearedCount.load(), " of ", numberOfPlaces.load(), " accesses at a known slot use the name instead: the slot may hold an unmaterialized closure");
}

static void passClosuresToGenerators(Program& program)
{
    BitVector hasManyClosures(program.functions.size());
    BitVector isCalledWithoutClosure(program.functions.size());
    BitVector escapesOwnAllocation(program.functions.size());
    auto next = program.find(asBytes("next"_s.span8()));
    auto returnName = program.find(asBytes("return"_s.span8()));
    auto throwName = program.find(asBytes("throw"_s.span8()));
    unsigned count = program.functions.size();
    constexpr uint8_t flaggedHasManyClosures = 1;
    constexpr uint8_t flaggedCalledWithoutClosure = 2;
    constexpr uint8_t flaggedEscapesOwnAllocation = 4;
    Vector<uint8_t> said(FillWith { }, count, static_cast<uint8_t>(0));
    auto say = [&](FunctionID function, uint8_t what) {
        WTF::atomicExchangeOr(&said[function.index()], what, std::memory_order_relaxed);
    };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        Vector<unsigned> numberOfOtherUses;
        static constexpr OpcodeSet closuresAndDirectCalls { Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::CallDirect };
        std::as_const(program).functions.forEachValue(i, closuresAndDirectCalls, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                say(value.function(), flaggedHasManyClosures);
            else if (std::as_const(program).at(value.function()).declaredAs & functionIsGenerator && numberOfOtherUses.isEmpty())
                numberOfOtherUses.fill(0, function.values.size());
        });
        if (numberOfOtherUses.isEmpty())
            return;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& user = function.at(id);
                if (user.opcode == Opcode::IteratorOpen || user.opcode == Opcode::Cast)
                    continue;
                bool isStep = user.opcode == Opcode::CallMethod && ((next && user.string() == *next) || (returnName && user.string() == *returnName) || (throwName && user.string() == *throwName));
                user.forEachChild([&](ValueID& child) {
                    if (!isStep || &child != &user.children[0])
                        ++numberOfOtherUses[child.index()];
                });
            }
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                ++numberOfOtherUses[child.index()];
            });
        }
        for (bool isGrowing = true; isGrowing;) {
            isGrowing = false;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (function.at(id).opcode == Opcode::Cast && numberOfOtherUses[id.index()] && !numberOfOtherUses[function.at(id).child(0).index()]) {
                        ++numberOfOtherUses[function.at(id).child(0).index()];
                        isGrowing = true;
                    }
                }
            }
        }
        for (unsigned k = 0; k < function.values.size(); ++k) {
            auto& value = function.values[k];
            if (value.opcode != Opcode::CallDirect)
                continue;
            say(value.function(), numberOfOtherUses[k] ? flaggedCalledWithoutClosure | flaggedEscapesOwnAllocation : flaggedCalledWithoutClosure);
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        if (said[i] & flaggedHasManyClosures)
            hasManyClosures.set(i);
        if (said[i] & flaggedCalledWithoutClosure)
            isCalledWithoutClosure.set(i);
        if (said[i] & flaggedEscapesOwnAllocation)
            escapesOwnAllocation.set(i);
    }
    for (auto& layout : program.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                if (layout.isEvaluatedOnce) {
                    isCalledWithoutClosure.set(method.function.index());
                    escapesOwnAllocation.set(method.function.index());
                } else
                    hasManyClosures.set(method.function.index());
            }
        }
    }
    for (unsigned i : hasManyClosures) {
        if (!(std::as_const(program).functions[i].declaredAs & functionIsGenerator) || !std::as_const(program).functions[i].resumeFunction || std::as_const(program).functions[i].blocks.isEmpty() || escapesOwnAllocation.get(i))
            continue;
        if (isCalledWithoutClosure.get(i)) {
            Function copy = std::as_const(program).functions[i];
            copy.flags &= Format::FunctionIsTyped | Format::FunctionHasThis | Format::FunctionHasRestParameter | Format::FunctionIsInBuiltinModule;
            copy.isRampForClosures = true;
            copy.isUsedAsObject = false;
            program.functions[i].rampForClosures = FunctionID(program.functions.size());
            program.functions.append(WTF::move(copy));
        }
        auto& ramp = std::as_const(program).functions[i].rampForClosures ? program.at(program.functions[i].rampForClosures) : program.functions[i];
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.position = ramp.position;
        parameter.type = program.types.anything;
        parameter.rep = repOf(program.at(program.types.anything));
        ValueID closure(ramp.values.size());
        ramp.values.append(WTF::move(parameter));
        auto& parameters = ramp.blocks[0].parameters;
        unsigned first = ramp.captureTypes.size() + ramp.has(Format::FunctionHasThis);
        parameters.insert(first, closure);
        for (unsigned k = first; k < parameters.size(); ++k)
            ramp.at(parameters[k]).immediates = { 0, k };
        ramp.parameterTypes.insert(0, TypeID());
        BitVector annotated;
        for (unsigned k : ramp.annotatedParameters)
            annotated.set(k + 1);
        ramp.annotatedParameters = WTF::move(annotated);
        if (!ramp.assertedParameterTypes.isEmpty())
            ramp.assertedParameterTypes.insert(0, TypeID());
        for (auto& value : ramp.values) {
            if (value.opcode == Opcode::Operation && (program.operationOf(value) == Operations::NewGenerator || program.operationOf(value) == Operations::NewAsyncGenerator))
                value.arguments.append(closure);
        }
        ramp.takesCalleeClosure = true;
    }
}

static void createGuardedPreludeEntries(Program& program)
{
    auto& builtins = Builtins::singleton();
    unsigned count = program.functions.size();
    Vector<Function> entries;
    for (unsigned i = 0; i < builtins.size(); ++i) {
        auto& builtin = builtins.at(BuiltinID(i));
        if (builtin.kind != BuiltinKind::Method || builtin.implementation.kind != BuiltinImplementation::Prelude || isSelectedAtCallSite(builtin.implementation.prelude))
            continue;
        FunctionID prelude = program.preludeFunctions[static_cast<unsigned>(builtin.implementation.prelude)];
        StringID name = program.stringOf(builtin.nameName);
        auto* engineClass = builtins.classNamed(builtin.holderName);
        if (!prelude || !name || !engineClass || !engineClass->canCheckOriginalStructure() || !program.engineClassTypes[engineClass->index])
            continue;
        unsigned numberOfParameters = program.at(prelude).parameterTypes.size();
        constexpr unsigned firstArgumentKindCount = ClosureRequest::maxSlimClosureCaptures + 2;
        for (unsigned variant = 0; variant < 2 * firstArgumentKindCount * std::min(numberOfParameters, Program::maxGuardedPreludeEntryArguments + 1); ++variant) {
            unsigned numberOfArguments = variant / (2 * firstArgumentKindCount);
            unsigned firstArgumentKind = variant / 2 % firstArgumentKindCount;
            std::optional<unsigned> copiedArgumentCaptureCount = firstArgumentKind ? std::optional(firstArgumentKind - 1) : std::nullopt;
            bool checksResultType = variant & 1;
            if ((copiedArgumentCaptureCount && !numberOfArguments) || (checksResultType && !program.at(prelude).resultType) || program.guardedPreludeEntries.contains(Program::guardedPreludeEntryKey(prelude, numberOfArguments, copiedArgumentCaptureCount, checksResultType)))
                continue;
            Function entry;
            entry.name = program.emptyString;
            entry.module = program.at(prelude).module;
            entry.position = program.at(prelude).position;
            entry.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
            entry.isGuardedPreludeEntry = true;
            entry.copiesFirstArgumentToHeap = !!copiedArgumentCaptureCount;
            entry.copiedArgumentCaptureCount = copiedArgumentCaptureCount.value_or(0);
            entry.resultType = checksResultType ? program.at(prelude).resultType : program.types.anything;
            entry.blocks.append(Block { });
            Value call;
            call.opcode = Opcode::CallMethod;
            call.position = entry.position;
            call.type = entry.resultType;
            call.rep = repOf(program.at(call.type));
            call.immediates = { name.index(), Format::none };
            call.isInTailPosition = true;
            for (unsigned k = 0; k <= numberOfArguments; ++k) {
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = entry.position;
                parameter.type = program.types.anything;
                parameter.rep = repOf(program.at(parameter.type));
                parameter.immediates = { 0, k };
                ValueID id(entry.values.size());
                if (k)
                    call.arguments.append(id);
                else {
                    parameter.setPrediction(program.engineClassTypes[engineClass->index]);
                    call.children[0] = id;
                }
                entry.parameterTypes.append(parameter.type);
                entry.blocks[0].parameters.append(id);
                entry.values.append(WTF::move(parameter));
            }
            entry.length = entry.parameterTypes.size();
            entry.blocks[0].values.append(ValueID(entry.values.size()));
            entry.blocks[0].terminator.kind = TerminatorKind::Return;
            entry.blocks[0].terminator.value = ValueID(entry.values.size());
            entry.values.append(WTF::move(call));
            program.guardedPreludeEntries.add(Program::guardedPreludeEntryKey(prelude, numberOfArguments, copiedArgumentCaptureCount, checksResultType), FunctionID(count + entries.size()));
            entries.append(WTF::move(entry));
        }
    }
    program.functions.appendVector(WTF::move(entries));
}

static void markCallsInTailPosition(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        if (program.implementsBuiltin(function) || program.isInBuiltinFunctionModule(function)) {
            for (auto& value : function.values)
                value.isInTailPosition = false;
        }
        if (!program.usesBuiltinTailCalls(function) || function.isValueEntry)
            return;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return || !block.terminator.value)
                continue;
            ValueID returned = block.terminator.value;
            while (function.at(returned).opcode == Opcode::Cast)
                returned = function.at(returned).child(0);
            size_t end = block.values.size();
            while (end && block.values[end - 1] != returned && function.at(block.values[end - 1]).opcode == Opcode::Cast)
                --end;
            if (!end || block.values[end - 1] != returned)
                continue;
            auto& value = function.at(returned);
            bool isCallingOperation = value.opcode == Opcode::Operation && (program.operationOf(value) == Operations::Apply || program.operationOf(value) == Operations::ResumeGenerator);
            if (isCallingOperation || value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect)
                value.isInTailPosition = true;
        }
    });
}

static constexpr OpcodeSet concatenations { Opcode::Add, Opcode::Strcat };

static void fuseConcatenations(Program& program, FunctionID functionID)
{
    if (!program.functions.mayContain(functionID.index(), concatenations))
        return;
    auto& function = program.functions.atKeepingValueIndex(functionID.index());
    auto isStringOrNumber = [&](ValueID id) {
        auto& type = program.typeOf(function, id);
        return type.isString() || type.isNumber();
    };
    auto isNotObject = [&](ValueID id) {
        return !program.typeOf(function, id).hasObjects();
    };
    auto isConcatenation = [&](const Value& value) {
        if (!concatenations.contains(value.opcode) || value.rep != Rep::Cell || !program.at(value.type).isString())
            return false;
        if (value.opcode == Opcode::Add)
            return isNotObject(value.child(0)) && isNotObject(value.child(1)) && (program.typeOf(function, value.child(0)).isString() || program.typeOf(function, value.child(1)).isString());
        return std::ranges::all_of(value.arguments, isNotObject);
    };
    Vector<uint32_t> numberOfUses;
    BitVector keys;
    for (auto& block : function.blocks) {
        for (size_t i = 0; i < block.values.size(); ++i) {
            ValueID id = block.values[i];
            if (!isConcatenation(function.at(id)))
                continue;
            if (numberOfUses.isEmpty()) {
                numberOfUses.fill(0, function.values.size());
                keys = computeMapKeyValues(program, functionID, function);
                for (auto& other : function.blocks) {
                    for (ValueID user : other.values) {
                        std::as_const(function.at(user)).forEachChild([&](ValueID child) {
                            ++numberOfUses[child.index()];
                        });
                    }
                    other.terminator.forEachChild([&](ValueID& child) {
                        ++numberOfUses[child.index()];
                    });
                }
            }
            Arguments operands = function.at(id).arguments;
            if (function.at(id).opcode == Opcode::Add)
                operands = { function.at(id).child(0), function.at(id).child(1) };
            Arguments reversedParts;
            Vector<size_t, 4> fused;
            size_t limit = i;
            for (size_t o = operands.size(); o--;) {
                ValueID operand = operands[o];
                std::optional<size_t> found;
                auto hasOnlyStringsAndNumbers = [&](const Value& inner) {
                    bool result = true;
                    inner.forEachChild([&](ValueID part) {
                        result &= isStringOrNumber(part);
                    });
                    return result;
                };
                constexpr size_t maxAbsorbedParts = 256;
                if (numberOfUses[operand.index()] == 1 && function.at(operand).arguments.size() < maxAbsorbedParts && isConcatenation(function.at(operand)) && (!keys.get(id.index()) || hasOnlyStringsAndNumbers(function.at(operand)))) {
                    for (size_t k = limit; k--;) {
                        if (block.values[k] == operand) {
                            found = k;
                            break;
                        }
                        if (mayThrow(program, function, function.at(block.values[k])) || writesOf(program, function, block, k) > Writes::RuntimeState)
                            break;
                    }
                }
                if (!found) {
                    reversedParts.append(operand);
                    continue;
                }
                auto& inner = function.at(operand);
                if (inner.opcode == Opcode::Add) {
                    reversedParts.append(inner.child(1));
                    reversedParts.append(inner.child(0));
                } else {
                    for (size_t p = inner.arguments.size(); p--;)
                        reversedParts.append(inner.arguments[p]);
                }
                fused.append(*found);
                limit = *found;
            }
            if (fused.isEmpty())
                continue;
            program.functions.invalidateValueIndex(functionID.index());
            for (size_t index : fused)
                block.values.removeAt(index);
            i -= fused.size();
            reversedParts.reverse();
            auto& value = function.at(id);
            value.opcode = Opcode::Strcat;
            value.children = { };
            value.immediates = { Format::none, Format::none };
            value.arguments = WTF::move(reversedParts);
        }
    }
}

static void refuteIteratorsWithMutableNext(Program& program)
{
    auto next = program.find(asBytes("next"_s.span8()));
    if (!next || !program.usesOperation(Operations::GetIterator))
        return;
    BitVector mayChange(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        bool isField = layout.fields.containsIf([&](auto& field) {
            return field.name == *next;
        });
        bool isReplaced = layout.methods.containsIf([&](auto& method) {
            return method.name == *next && method.mayBeReplaced;
        });
        if (isField || isReplaced)
            mayChange.set(i);
    }
    if (mayChange.isEmpty())
        return;
    BitVector isRefuted(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        bool findsProperty = false;
        for (LayoutID at = LayoutID(i); at && !findsProperty; at = program.at(at).base)
            findsProperty = mayChange.get(at.index());
        for (LayoutID at = LayoutID(i); at && findsProperty; at = program.at(at).base)
            isRefuted.set(at.index());
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet classTests { Opcode::IsInstanceOfClass };
        if (!program.functions.mayContain(i, classTests))
            return;
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (!classTests.contains(value.opcode) || !isRefuted.get(value.layout().index()))
                    continue;
                auto& operand = function.at(value.child(0));
                if (operand.opcode != Opcode::Operation || program.operationOf(operand) != Operations::GetIterator)
                    continue;
                value.opcode = Opcode::Boolean;
                value.children = { };
                value.immediates = { 0u, Format::none };
                if (block.terminator.kind == TerminatorKind::Invoke && block.values.last() == id) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
        }
    });
}

static constexpr uint8_t vigintileNearRoots = 25;

static Vector<uint8_t> computeHeatVigintiles(const Program&);

static void findCallGraphColdFunctions(Program& program, const Vector<uint8_t>& vigintiles)
{
    static constexpr uint8_t maxColdHeat = 4;
    BitVector result(vigintiles.size());
    auto coldFunctions = findIndicesInParallel(vigintiles.size(), [&](unsigned i) {
        return vigintiles[i] <= maxColdHeat && !std::as_const(program).functions[i].isValueEntry;
    });
    for (unsigned i : coldFunctions)
        result.set(i);
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: cold by the graph: ", result.bitCount(), " of ", vigintiles.size(), " functions");
    program.callGraphColdFunctions = WTF::move(result);
}

static bool isTooSmallForColdCode(const Program& program)
{
    static constexpr uint64_t minSizeWithColdCode = 65536;
    uint64_t size = 0;
    for (auto& function : program.functions) {
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin | Format::FunctionIsInBuiltinModule))
            continue;
        size += function.values.size();
        if (size >= minSizeWithColdCode)
            return false;
    }
    return true;
}

static void applyCallGraphHeat(Program& program, const Vector<uint8_t>& vigintiles)
{
    static constexpr uint8_t maxCooledHeat = 5;
    uint64_t cooled = 0;
    Vector<uint8_t> isWarmAndHasLoops(FillWith { }, vigintiles.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(vigintiles.size(), [&](unsigned i) {
        isWarmAndHasLoops[i] = vigintiles[i] <= maxCooledHeat && program.staticallyWarmFunctions.get(i) && !blocksInLoops(std::as_const(program).functions, i).isEmpty();
    });
    for (unsigned i = 0; i < vigintiles.size(); ++i) {
        if (vigintiles[i] > maxCooledHeat)
            continue;
        if (isWarmAndHasLoops[i]) {
            program.functionsHotOnlyInLoops.clear(i);
            program.staticallyHotFunctions.clear(i);
            continue;
        }
        cooled += program.staticallyHotFunctions.get(i) || program.staticallyWarmFunctions.get(i);
        program.staticallyHotFunctions.clear(i);
        program.staticallyWarmFunctions.clear(i);
        program.functionsHotOnlyInLoops.clear(i);
    }
    for (auto [function, entry] : program.genericEntrypoints) {
        for (auto* marks : { &program.staticallyHotFunctions, &program.staticallyWarmFunctions, &program.functionsHotOnlyInLoops })
            marks->set(entry.index(), marks->get(function));
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: heat by the graph: ", cooled, " functions lose the mark hot or warm, of ", vigintiles.size());
}

static void findFunctionsWithInliningDisabled(Program& program, const Vector<uint8_t>& vigintiles)
{
    static constexpr uint8_t maxHeatWithoutInlining = 5;
    constexpr uint16_t hasNoDuplicates = 1 << inlineReasonForLetter('m') | 1 << inlineReasonForLetter('w') | 1 << inlineReasonForLetter('o');
    BitVector result(vigintiles.size());
    for (unsigned i = 0; i < vigintiles.size(); ++i) {
        if (vigintiles[i] <= maxHeatWithoutInlining)
            result.set(i);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: no copies into cold callers: ", result.bitCount(), " of ", vigintiles.size(), " functions take none");
    program.functionsWithInliningDisabled = WTF::move(result);
    program.disabledInlineReasons = ((1 << inlineReasonLetters.length()) - 2) & ~hasNoDuplicates;
}

bool IntegralNumberAnalysis::contains(ValueID id, unsigned depth)
{
    auto& value = m_function.at(id);
    auto& type = m_program.at(value.type);
    if (type.isNever() || !type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
        return false;
    if (!(type.primitives & Primitive::Double))
        return value.opcode != Opcode::Unspecified;
    if (!depth)
        return false;
    switch (value.opcode) {
    case Opcode::GetField:
        return m_program.at(value.layout()).fields[value.field()].holdsIntegralNumbers;
    case Opcode::CallDirect:
        return !value.isJumpToContinuation() && m_program.at(value.function()).returnsIntegralNumbers;
    case Opcode::Cast:
    case Opcode::ToNumeric:
    case Opcode::ToNumber:
        return contains(value.child(0), depth - 1);
    case Opcode::Add:
    case Opcode::Sub:
        return contains(value.child(0), depth - 1) && contains(value.child(1), depth - 1);
    case Opcode::Parameter: {
        if (!value.parameterBlock().index())
            return false;
        if (m_assumed.contains(id))
            return true;
        if (!m_hasFoundIncoming) {
            m_hasFoundIncoming = true;
            for (auto& block : m_function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < parameters.size(); ++i)
                        m_incoming.add(parameters[i].index(), Vector<ValueID, 2> { }).iterator->value.append(i < target.arguments.size() ? target.arguments[i] : ValueID());
                }
            }
        }
        auto incoming = m_incoming.find(id.index());
        if (incoming == m_incoming.end())
            return false;
        m_assumed.append(id);
        bool result = std::ranges::all_of(incoming->value, [&](ValueID passed) {
            return passed && contains(passed, depth - 1);
        });
        m_assumed.removeLast();
        return result;
    }
    default:
        return false;
    }
}

static void findIntegralNumbers(Program& program)
{
    auto mayHoldNonIntegers = [&](TypeID id) {
        if (!id)
            return false;
        auto& type = program.at(id);
        return type.primitives & Primitive::Double && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    };
    for (auto& layout : program.layouts) {
        if (layout.kind != LayoutKind::Environment || layout.isCoroutineFrame)
            continue;
        for (auto& field : layout.fields)
            field.holdsIntegralNumbers = mayHoldNonIntegers(field.type);
    }
    struct Site {
        unsigned function;
        ValueID stored;
        LayoutID layout;
        uint32_t field;
    };
    Vector<Vector<Site>> functionSites(program.functions.size());
    Vector<Vector<LayoutID>> layoutsAllocatedWithArguments(program.functions.size());
    static constexpr OpcodeSet allocationsAndStores { Opcode::NewObjectWithShape, Opcode::PutField };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        if (function.blocks.isEmpty())
            return;
        function.returnsIntegralNumbers = function.resultTypes.isEmpty() && mayHoldNonIntegers(function.resultType);
        if (!function.returnsIntegralNumbers && !program.functions.mayContain(i, allocationsAndStores))
            return;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::NewObjectWithShape && !value.arguments.isEmpty())
                    layoutsAllocatedWithArguments[i].append(value.layout());
                if (value.opcode == Opcode::PutField && std::as_const(program).at(value.layout()).fields[value.field()].holdsIntegralNumbers)
                    functionSites[i].append({ i, value.child(1), value.layout(), value.field() });
            }
            if (block.terminator.kind == TerminatorKind::Return && function.returnsIntegralNumbers) {
                if (block.terminator.value)
                    functionSites[i].append({ i, block.terminator.value, LayoutID(), 0 });
                else
                    function.returnsIntegralNumbers = false;
            }
        }
    });
    Vector<Site> sites;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        for (LayoutID layout : layoutsAllocatedWithArguments[i]) {
            for (auto& field : program.at(layout).fields)
                field.holdsIntegralNumbers = false;
        }
        sites.appendVector(functionSites[i]);
    }
    Vector<unsigned> functionFirstSite;
    for (unsigned at = 0; at < sites.size(); ++at) {
        if (!at || sites[at].function != sites[at - 1].function)
            functionFirstSite.append(at);
    }
    functionFirstSite.append(sites.size());
    Vector<uint8_t> noLongerHolds(FillWith { }, sites.size(), static_cast<uint8_t>(0));
    for (bool changed = true; changed;) {
        changed = false;
        forEachIndexInParallel(functionFirstSite.size() - 1, [&](unsigned k) {
            auto& function = std::as_const(program).functions[sites[functionFirstSite[k]].function];
            IntegralNumberAnalysis integralNumbers(program, function);
            for (unsigned at = functionFirstSite[k]; at < functionFirstSite[k + 1]; ++at) {
                auto& site = sites[at];
                bool holds = site.layout ? std::as_const(program).at(site.layout).fields[site.field].holdsIntegralNumbers : function.returnsIntegralNumbers;
                noLongerHolds[at] = holds && !integralNumbers.contains(site.stored);
            }
        });
        for (unsigned at = 0; at < sites.size(); ++at) {
            if (!noLongerHolds[at])
                continue;
            auto& site = sites[at];
            (site.layout ? program.at(site.layout).fields[site.field].holdsIntegralNumbers : program.functions.atKeepingValueIndex(site.function).returnsIntegralNumbers) = false;
            changed = true;
        }
    }
}

static void boxResultsOfIndirectOnlyFunctions(Program& program)
{
    Vector<FunctionID> enteredBy(program.functions.size());
    for (auto [function, entry] : program.genericEntrypoints)
        enteredBy[entry.index()] = FunctionID(function);
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (FunctionID function = std::as_const(program).functions[i].valueEntryTarget)
            enteredBy[i] = function;
    });
    Vector<uint8_t> isCalledDirectly(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& caller = std::as_const(program).functions[i];
        auto note = [&](FunctionID callee) {
            WTF::atomicStore(&isCalledDirectly[callee.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        if (!program.functions.mayContainPredictedClosure(i)) {
            std::as_const(program).functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
                auto& value = caller.at(id);
                if (!value.isJumpToContinuation() && value.function() != enteredBy[i])
                    note(value.function());
            });
            return;
        }
        for (auto& block : caller.blocks) {
            for (ValueID id : block.values) {
                auto& value = caller.at(id);
                if (value.predictedClosure())
                    note(value.predictedClosure());
                if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && value.function() != enteredBy[i])
                    note(value.function());
            }
        }
    });
    auto mayJumpFrom = [&](const Function& entry, FunctionID function) {
        if (!entry.mayPointIntoFrame.isEmpty())
            return false;
        bool jumps = false;
        for (auto& block : entry.blocks) {
            for (ValueID id : block.values) {
                auto& call = entry.at(id);
                if (call.opcode != Opcode::CallDirect || call.function() != function)
                    continue;
                if (block.terminator.kind != TerminatorKind::Return || block.terminator.value != id || block.values.last() != id)
                    return false;
                unsigned numberOfGPRs = 0;
                unsigned numberOfFPRs = 0;
                for (ValueID argument : call.arguments) {
                    LayoutID supertype = program.typeOf(entry, argument).singleLayout();
                    if (supertype && program.at(supertype).mayBeStackAllocated)
                        return false;
                    Rep rep = entry.at(argument).rep;
                    ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
                }
                if (numberOfGPRs > numberOfDirectArgumentGPRs || numberOfFPRs > FPRInfo::numberOfArgumentRegisters)
                    return false;
                jumps = true;
            }
        }
        return jumps;
    };
    Vector<std::pair<uint32_t, FunctionID>> entries;
    entries.reserveInitialCapacity(program.genericEntrypoints.size());
    for (auto [index, entry] : program.genericEntrypoints)
        entries.append({ index, entry });
    forEachIndexInParallel(entries.size(), [&](unsigned k) {
        auto [index, entry] = entries[k];
        auto& function = program.functions.atKeepingValueIndex(index);
        function.isCalledDirectly = isCalledDirectly[index];
        function.takesCapturesFromClosure = !isCalledDirectly[index] && !function.captureTypes.isEmpty() && program.compiledValueEntryOf(entry) && function.blocks[0].parameters.size() - function.captureTypes.size() <= numberOfArgumentGPRs;
        if ((function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow) || !function.resultTypes.isEmpty() || function.blocks.isEmpty())
            return;
        Rep rep = resultRepOf(program, FunctionID(index));
        if (isCalledDirectly[index] && rep != Rep::Void)
            return;
        function.returnsBoxedValue = rep != Rep::JSValue && rep != Rep::Cell && mayJumpFrom(std::as_const(program).at(entry), FunctionID(index));
    });
}

namespace {

struct TailCallSite {
    BlockID block;
    ValueID call;
    unsigned numberOfCasts { 0 };
    FunctionID callee;
    FunctionID calledClosureFunction;
    bool returnsArgument { false };
    Vector<std::pair<BlockID, unsigned>, 2> forwardedThrough;
};

}

static void collectTailCallSites(const Program& program, FunctionID id, const Function& function, Vector<TailCallSite>& sites)
{
    constexpr unsigned maxBlocks = 8;
    if (program.isEntry(id) || function.isSharedOutlinedBody || !function.resultTypes.isEmpty() || function.worker || function.kind == FunctionKind::Module || function.kind == FunctionKind::Constructor)
        return;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        if (block.values.isEmpty() || block.terminator.kind == TerminatorKind::Invoke)
            continue;
        TailCallSite site;
        site.block = BlockID(b);
        size_t at = block.values.size();
        while (at > 1 && function.at(block.values[at - 1]).opcode == Opcode::Cast && function.at(block.values[at - 1]).child(0) == block.values[at - 2])
            --at;
        site.call = block.values[at - 1];
        site.numberOfCasts = block.values.size() - at;
        auto& call = function.at(site.call);
        if (!call.isInTailPosition)
            continue;
        if (call.opcode == Opcode::CallDirect) {
            if (call.isJumpToContinuation())
                continue;
            site.callee = call.function();
            auto& callee = program.at(site.callee);
            if (callee.blocks.isEmpty() || !callee.resultTypes.isEmpty() || callee.kind == FunctionKind::Module || program.moduleEvaluationIndexByFunction.contains(site.callee.index()))
                continue;
        } else if (call.opcode != Opcode::Call && call.opcode != Opcode::CallMethod && languageOperationOf(program, call) != LanguageOperation::Apply && languageOperationOf(program, call) != LanguageOperation::CallWithSpread)
            continue;
        if (!site.callee && call.opcode != Opcode::CallMethod) {
            auto& called = function.at(program.originOf(function, call.opcode == Opcode::Call ? call.child(0) : call.arguments[0]));
            if (called.opcode == Opcode::BuiltinFunction)
                continue;
            if (called.opcode == Opcode::FunctionReference || called.opcode == Opcode::NewClosure)
                site.calledClosureFunction = called.function();
        }
        if (function.isGenericEntrypoint() && !call.inlineFrame)
            continue;
        bool dropsFrame = true;
        for (uint32_t frame = call.inlineFrame; frame && dropsFrame; frame = function.inlineFrames[frame].parent) {
            auto& inlinedCall = function.inlineFrames[frame];
            if (inlinedCall.isTailCall)
                continue;
            auto entry = function.isGenericEntrypoint() && !inlinedCall.parent ? program.genericEntrypoints.find(inlinedCall.callee.index()) : program.genericEntrypoints.end();
            dropsFrame = entry != program.genericEntrypoints.end() && entry->value == id;
        }
        if (!dropsFrame)
            continue;
        ValueID forwarded = block.values.last();
        const Terminator* terminator = &block.terminator;
        bool isForwarded = true;
        while (terminator->kind != TerminatorKind::Return && isForwarded) {
            isForwarded = terminator->kind == TerminatorKind::Jump && site.forwardedThrough.size() < maxBlocks && function.at(terminator->targets[0].block).values.isEmpty();
            if (!isForwarded)
                break;
            auto& target = terminator->targets[0];
            size_t index = forwarded ? target.arguments.find(forwarded) : notFound;
            if (index == notFound)
                forwarded = ValueID();
            else {
                forwarded = function.at(target.block).parameters[index];
                site.forwardedThrough.append({ target.block, static_cast<unsigned>(index) });
            }
            terminator = &function.at(target.block).terminator;
        }
        if (!isForwarded)
            continue;
        if (!forwarded || terminator->value != forwarded) {
            if (site.numberOfCasts || !site.callee)
                continue;
            site.returnsArgument = true;
            site.forwardedThrough.clear();
        }
        sites.append(WTF::move(site));
    }
}

namespace {

struct TailCalls {
    Vector<Vector<TailCallSite>> sites;
    BitVector isInIndirectCycle;
    Vector<uint32_t> cycleOf;
    Vector<Vector<unsigned, 2>> cycleMembers;
};

}

static TailCalls findTailCalls(const Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<TailCallSite>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (program.hasCode(FunctionID(i)))
            collectTailCallSites(program, FunctionID(i), std::as_const(program).functions[i], sites[i]);
    });
    auto knownClosureCallers = findIndicesInParallel(count, [&](unsigned i) {
        return sites[i].containsIf([](auto& site) { return !!site.calledClosureFunction; });
    });
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i : knownClosureCallers) {
            bool removed = sites[i].removeAllMatching([&](auto& site) {
                if (!site.calledClosureFunction || !sites[site.calledClosureFunction.index()].isEmpty())
                    return false;
                auto entry = program.genericEntrypoints.find(site.calledClosureFunction.index());
                return entry == program.genericEntrypoints.end() || sites[entry->value.index()].isEmpty();
            });
            changed |= removed;
        }
    }
    unsigned indirectCalleeNode = count;
    Vector<uint8_t> hasOrIsGenericEntrypoint(FillWith { }, count, static_cast<uint8_t>(0));
    for (auto [function, entry] : program.genericEntrypoints) {
        hasOrIsGenericEntrypoint[function] = 1;
        hasOrIsGenericEntrypoint[entry.index()] = 1;
    }
    auto indirectlyCallable = findIndicesInParallel(count, [&](unsigned i) {
        return hasOrIsGenericEntrypoint[i] && !sites[i].isEmpty();
    });
    struct Frame {
        unsigned function;
        unsigned next;
    };
    Vector<uint32_t> cycleOf(FillWith { }, count + 1, 0u);
    BitVector isOnStack(count + 1);
    Vector<unsigned> index(FillWith { }, count + 1, 0u);
    Vector<unsigned> low(FillWith { }, count + 1, 0u);
    Vector<unsigned> stack;
    Vector<Frame> frames;
    Vector<Vector<unsigned, 2>> cycleMembers;
    cycleMembers.append({ });
    unsigned counter = 0;
    auto enter = [&](unsigned function) {
        index[function] = low[function] = ++counter;
        stack.append(function);
        isOnStack.set(function);
        frames.append(Frame { function, 0 });
    };
    for (unsigned root = 0; root <= count; ++root) {
        if (index[root] || (root < count && sites[root].isEmpty()))
            continue;
        enter(root);
        while (!frames.isEmpty()) {
            unsigned function = frames.last().function;
            if (frames.last().next < (function == indirectCalleeNode ? indirectlyCallable.size() : sites[function].size())) {
                unsigned next = frames.last().next++;
                unsigned callee = function == indirectCalleeNode ? indirectlyCallable[next] : sites[function][next].callee ? sites[function][next].callee.index() : indirectCalleeNode;
                if (!index[callee])
                    enter(callee);
                else if (isOnStack.get(callee))
                    low[function] = std::min(low[function], index[callee]);
                continue;
            }
            frames.removeLast();
            if (!frames.isEmpty())
                low[frames.last().function] = std::min(low[frames.last().function], low[function]);
            if (low[function] != index[function])
                continue;
            bool isCycle = stack.last() != function || (function < count && sites[function].containsIf([&](auto& site) { return site.callee == FunctionID(function); }));
            if (isCycle)
                cycleMembers.append({ });
            for (;;) {
                unsigned member = stack.takeLast();
                isOnStack.clear(member);
                if (isCycle) {
                    cycleOf[member] = cycleMembers.size() - 1;
                    if (member < count)
                        cycleMembers.last().append(member);
                }
                if (member == function)
                    break;
            }
        }
    }
    BitVector isInIndirectCycle(count);
    if (cycleOf[indirectCalleeNode]) {
        for (unsigned member : cycleMembers[cycleOf[indirectCalleeNode]])
            isInIndirectCycle.set(member);
    }
    return { WTF::move(sites), WTF::move(isInIndirectCycle), WTF::move(cycleOf), WTF::move(cycleMembers) };
}

static void markMandatoryTailCalls(Program& program)
{
    if (Options::auditAOTTypedAssertions())
        return;
    auto [sites, isInIndirectCycle, cycleOf, cycleMembers] = findTailCalls(program);
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        for (auto& site : sites[i]) {
            if (site.callee ? cycleOf[i] && cycleOf[i] == cycleOf[site.callee.index()] : isInIndirectCycle.get(i))
                program.functions.atKeepingValueIndex(i).at(site.call).mustBeProperTailCall = true;
        }
    });
    destroyOnHelperThread(WTF::move(sites));
}

static unsigned numberOfStackArguments(const Program& program, const Function& function)
{
    if (function.blocks.isEmpty() || function.isGenericEntrypoint())
        return 0;
    auto& parameters = function.blocks[0].parameters;
    if (parameters.size() <= numberOfDirectArgumentGPRs)
        return 0;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        Rep rep = repOf(program.declaredArgumentType(function, i));
        ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
    }
    return numberOfGPRs - std::min<unsigned>(numberOfGPRs, numberOfDirectArgumentGPRs) + numberOfFPRs - std::min<unsigned>(numberOfFPRs, FPRInfo::numberOfArgumentRegisters);
}

static void reserveStackArgumentsForTailCalls(Program& program, const Vector<Vector<TailCallSite>>& sites)
{
    unsigned count = program.functions.size();
    Vector<Vector<unsigned, 2>> callers(count);
    for (unsigned i = 0; i < count; ++i) {
        for (auto& site : sites[i]) {
            if (site.callee)
                callers[site.callee.index()].append(i);
        }
    }
    Vector<unsigned> worklist = collectInParallel<unsigned>(count, [&](unsigned i, Vector<unsigned>& collected) {
        if (callers[i].isEmpty() && sites[i].isEmpty())
            return;
        program.functions.atKeepingValueIndex(i).reservedStackArgumentCount = numberOfStackArguments(program, std::as_const(program).functions[i]);
        if (std::as_const(program).functions[i].reservedStackArgumentCount)
            collected.append(i);
    });
    while (!worklist.isEmpty()) {
        unsigned callee = worklist.takeLast();
        for (unsigned caller : callers[callee]) {
            if (std::as_const(program).functions[caller].isGenericEntrypoint() || std::as_const(program).functions[caller].reservedStackArgumentCount >= std::as_const(program).functions[callee].reservedStackArgumentCount)
                continue;
            program.functions.atKeepingValueIndex(caller).reservedStackArgumentCount = std::as_const(program).functions[callee].reservedStackArgumentCount;
            worklist.append(caller);
        }
    }
}

static bool returnsOwnResults(const Function& function, const Vector<TailCallSite>& sites)
{
    unsigned count = function.blocks.size();
    Vector<Vector<unsigned, 2>> predecessors(count);
    for (unsigned b = 0; b < count; ++b) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    BitVector returnsOnlyCalledValues(count);
    for (auto& site : sites) {
        if (!site.callee)
            returnsOnlyCalledValues.set(site.block.index());
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 1; b < count; ++b) {
            if (returnsOnlyCalledValues.get(b) || !function.blocks[b].values.isEmpty() || predecessors[b].isEmpty())
                continue;
            if (std::ranges::all_of(predecessors[b], [&](unsigned predecessor) { return returnsOnlyCalledValues.get(predecessor); })) {
                returnsOnlyCalledValues.set(b);
                changed = true;
            }
        }
    }
    for (unsigned b = 0; b < count; ++b) {
        if (function.blocks[b].terminator.kind == TerminatorKind::Return && !returnsOnlyCalledValues.get(b) && (!b || !predecessors[b].isEmpty()))
            return true;
    }
    return false;
}

static BitVector findFunctionsKeepingResultType(const Program& program, const Vector<Vector<TailCallSite>>& sites, const BitVector& isInIndirectCycle)
{
    BitVector keeps(program.functions.size());
    auto mayKeep = [&](unsigned i) {
        auto& function = program.functions[i];
        if (function.isGenericEntrypoint() || function.isValueEntry || function.returnsBoxedValue || !function.resultType || program.at(function.resultType).isAnything())
            return false;
        if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method)
            return false;
        if (function.takesCalleeClosure || function.takesCapturesFromClosure || function.rampForClosures || function.isRampForClosures)
            return false;
        if (auto entry = program.genericEntrypoints.find(i); entry != program.genericEntrypoints.end() && (!function.isCalledDirectly || function.has(Format::FunctionHasRestParameter)))
            return false;
        for (auto& site : sites[i]) {
            if (site.callee)
                continue;
            auto& call = function.at(site.call);
            if (call.opcode != Opcode::Call && call.opcode != Opcode::CallMethod)
                continue;
            if (call.arguments.size() > numberOfArgumentGPRs)
                return false;
        }
        return returnsOwnResults(function, sites[i]);
    };
    Vector<unsigned> members;
    for (unsigned i : isInIndirectCycle)
        members.append(i);
    Vector<uint8_t> mayKeepBySubtype(FillWith { }, members.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(members.size(), [&](unsigned k) {
        mayKeepBySubtype[k] = mayKeep(members[k]);
    });
    for (unsigned k = 0; k < members.size(); ++k) {
        if (mayKeepBySubtype[k])
            keeps.set(members[k]);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned member : members) {
            for (auto& site : sites[member]) {
                if (!site.callee || !isInIndirectCycle.get(site.callee.index()))
                    continue;
                unsigned callee = site.callee.index();
                if (!keeps.get(member))
                    continue;
                bool remainsHolder;
                if (keeps.get(callee)) {
                    Rep own = resultRepOf(program, FunctionID(member));
                    Rep returnedValue = resultRepOf(program, site.callee);
                    remainsHolder = !site.returnsArgument && !site.numberOfCasts && (own == returnedValue || (returnedValue == Rep::Cell && own == Rep::JSValue)) && program.at(program.functions[callee].resultType).isSubsetOf(program.at(program.functions[member].at(site.call).type), program);
                } else
                    remainsHolder = !site.returnsArgument && !program.functions[callee].reservedStackArgumentCount && !numberOfStackArguments(program, program.functions[callee]);
                if (remainsHolder)
                    continue;
                keeps.clear(member);
                changed = true;
            }
        }
    }
    return keeps;
}

static void prepareTailCalls(Program& program)
{
    if (Options::auditAOTTypedAssertions())
        return;
    unsigned count = program.functions.size();
    auto [sites, isInIndirectCycle, cycleOf, cycleMembers] = findTailCalls(program);
    reserveStackArgumentsForTailCalls(program, sites);
    auto resultTypeOf = [&](unsigned i) -> const Type& {
        auto& function = std::as_const(program).functions[i];
        return program.at(function.resultType ? function.resultType : program.types.anything);
    };
    BitVector keepsResultType = findFunctionsKeepingResultType(program, sites, isInIndirectCycle);
    BitVector isWidened = isInIndirectCycle;
    isWidened.exclude(keepsResultType);
    BitVector isBoxed(count);
    for (unsigned cycle = 1; cycle < cycleMembers.size(); ++cycle) {
        bool needsCheckAfterCall = false;
        bool representationsDiffer = false;
        for (unsigned member : cycleMembers[cycle]) {
            needsCheckAfterCall |= isWidened.get(member);
            for (auto& site : sites[member]) {
                if (!site.callee || cycleOf[site.callee.index()] != cycle || site.returnsArgument)
                    continue;
                needsCheckAfterCall |= site.numberOfCasts || !resultTypeOf(site.callee.index()).isSubsetOf(program.at(std::as_const(program).functions[member].at(site.call).type), program);
                Rep own = resultRepOf(program, FunctionID(member));
                Rep returnedValue = resultRepOf(program, site.callee);
                representationsDiffer |= own != returnedValue && !(returnedValue == Rep::Cell && own == Rep::JSValue);
            }
        }
        for (unsigned member : cycleMembers[cycle]) {
            if (keepsResultType.get(member))
                continue;
            if (needsCheckAfterCall)
                isWidened.set(member);
            else if (representationsDiffer)
                isBoxed.set(member);
        }
    }
    std::atomic<unsigned> widenedCount { 0 };
    std::atomic<unsigned> castsMovedToCallersCount { 0 };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        function.hasTailCallSites = true;
        function.isInIndirectTailCallCycle = isInIndirectCycle.get(i);
        if (isBoxed.get(i))
            function.returnsBoxedValue = true;
        if (keepsResultType.get(i)) {
            function.isCalledFromDroppableFrame = true;
            for (auto& site : sites[i]) {
                if (!site.callee || (isInIndirectCycle.get(site.callee.index()) && !keepsResultType.get(site.callee.index())))
                    function.at(site.call).dropsDroppableCallerFrame = true;
            }
        } else if (isInIndirectCycle.get(i)) {
            for (auto& site : sites[i]) {
                if (site.callee && keepsResultType.get(site.callee.index()))
                    function.at(site.call).leavesDroppableFrame = true;
            }
        }
        if (!isWidened.get(i))
            return;
        program.functions.invalidateValueIndex(i);
        bool isCoerced = false;
        Vector<Value> callerCasts;
        for (auto& site : sites[i]) {
            if (site.returnsArgument || (site.callee ? cycleOf[i] != cycleOf[site.callee.index()] : !isInIndirectCycle.get(i)))
                continue;
            auto& block = function.at(site.block);
            ValueID returned = block.values.last();
            bool hasExplicitCast = false;
            for (unsigned k = 0; k < site.numberOfCasts; ++k) {
                isCoerced |= function.at(block.values[block.values.size() - 1 - k]).castReason() == CastReason::As;
                hasExplicitCast |= function.at(block.values[block.values.size() - 1 - k]).castReason() == CastReason::Coerce;
            }
            if (hasExplicitCast && callerCasts.isEmpty()) {
                for (unsigned k = site.numberOfCasts; k; --k)
                    callerCasts.append(function.at(block.values[block.values.size() - k]));
            }
            castsMovedToCallersCount.fetch_add(site.numberOfCasts, std::memory_order_relaxed);
            block.values.shrink(block.values.size() - site.numberOfCasts);
            block.terminator.forEachChild([&](ValueID& child) {
                if (child == returned)
                    child = site.call;
            });
            auto retype = [&](ValueID id) {
                function.at(id).type = program.types.anything;
                function.at(id).rep = Rep::JSValue;
            };
            retype(site.call);
            for (auto [forwardedTo, parameter] : site.forwardedThrough)
                retype(function.at(forwardedTo).parameters[parameter]);
        }
        if (function.isGenericEntrypoint())
            return;
        function.callerResultCasts = WTF::move(callerCasts);
        if (!function.resultType || program.at(function.resultType).isAnything())
            return;
        function.resultTypeCheckedByCallers = function.resultType;
        function.resultIsCoercedByCallers = isCoerced;
        function.resultType = TypeID();
        widenedCount.fetch_add(1, std::memory_order_relaxed);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: tail calls: ", isInIndirectCycle.bitCount(), " functions are in the cycle of indirect tail calls, ", cycleMembers.size() - 1, " cycles in total; callers check the result type of ", widenedCount.load(), " functions, ", castsMovedToCallersCount.load(), " casts moved to callers, ", isBoxed.bitCount(), " functions return a boxed value to match their cycle, ", keepsResultType.bitCount(), " functions keep their result type and are called from droppable frames");
    destroyOnHelperThread(WTF::move(sites));
}

static void findConstructorsReturningObjects(Program& program)
{
    if (Options::auditAOTTypedAssertions())
        return;
    const Program& constProgram = program;
    unsigned count = program.functions.size();
    auto& object = constProgram.at(program.types.object);
    Vector<uint8_t> returnsObject(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = constProgram.functions[i];
        if (function.kind != FunctionKind::Constructor || function.isGenericEntrypoint() || function.resultType || !function.resultTypes.isEmpty() || function.blocks.isEmpty() || function.worker
            || function.has(FunctionHasUnknownCallers | FunctionIsEnteredByValue | FunctionUsesArgumentBuffer))
            return;
        bool returns = false;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            if (!block.terminator.value || !constProgram.typeOf(function, block.terminator.value).isSubsetOf(object, constProgram))
                return;
            returns = true;
        }
        returnsObject[i] = returns;
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (returnsObject[i])
            program.functions.atKeepingValueIndex(i).resultType = program.types.object;
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        constProgram.functions.forEachValue(i, directCalls, [&](ValueID id) {
            auto& value = constProgram.functions[i].at(id);
            if (value.isJumpToContinuation() || !returnsObject[value.function().index()] || !constProgram.at(value.type).isAnything())
                return;
            auto& call = program.functions.atKeepingValueIndex(i).at(id);
            call.type = program.types.object;
            call.rep = repOf(object);
        });
    });
}

static void findConstructedRecords(Program& program)
{
    Vector<uint8_t> isCreated(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    uint8_t isAnyRecord = 0;
    auto note = [&](uint8_t& flag) {
        WTF::atomicStore(&flag, static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    const Program& constProgram = program;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = constProgram.functions[i];
        bool storesFirst = !function.blocks.isEmpty() && std::ranges::any_of(function.blocks[0].values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::Operation && program.operationOf(function.at(id)) == Operations::Initialize;
        });
        for (TypeID thisType : { function.thisType, function.assertedThisType }) {
            if (LayoutID supertype = thisType && storesFirst ? constProgram.at(thisType).singleLayout() : LayoutID(); supertype && constProgram.at(supertype).kind == LayoutKind::Record)
                note(isCreated[supertype.index()]);
        }
        static constexpr OpcodeSet operations { Opcode::Operation };
        constProgram.functions.forEachValue(i, operations, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.arguments.size() < 2 || !(program.operationOf(value) == Operations::ConstructWith || program.operationOf(value) == Operations::ConstructWithSpread))
                return;
            auto& object = function.at(value.arguments[1]);
            note(object.opcode == Opcode::NewObjectWithShape ? isCreated[object.layout().index()] : isAnyRecord);
        });
    });
    Vector<uint32_t> worklist;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (isCreated[i] || isAnyRecord)
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        auto& layout = program.layouts[worklist.takeLast()];
        if (std::exchange(layout.mayBeConstructed, true))
            continue;
        for (LayoutID specialization : layout.specializations)
            worklist.append(specialization.index());
    }
    BitVector mayHaveUnknownPrototypeAtCreation;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].mayBeConstructed)
            continue;
        mayHaveUnknownPrototypeAtCreation.set(i);
        for (LayoutID subtype : program.layouts[i].subtypes)
            mayHaveUnknownPrototypeAtCreation.set(subtype.index());
    }
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].kind == LayoutKind::Record && program.layouts[i].mayDespecialize && !mayHaveUnknownPrototypeAtCreation.get(i) && !program.layouts[i].prototypeIsSetByTypedCode)
            program.layouts[i].mayHaveOtherPrototype = false;
    }
}

static constexpr OpcodeSet objectLiterals { Opcode::NewObject };

static void markCastsOfUnsharedLiterals(const Program& program, Function& function)
{
    Vector<unsigned> numberOfUses;
    auto canReplaceParameter = [&](const Value& value) {
        if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast)
            return value.child(0);
        if (value.opcode == Opcode::Operation && program.operationOf(value) == Operations::CloneObjectAfter && value.arguments.size() == 1)
            return value.arguments[0];
        return ValueID();
    };
    for (auto& block : function.blocks) {
        for (size_t i = 0; i < block.values.size(); ++i) {
            auto& cast = function.at(block.values[i]);
            if (cast.opcode != Opcode::Cast || cast.castReason() != CastReason::As || cast.coercesFreshData())
                continue;
            if (numberOfUses.isEmpty()) {
                numberOfUses.fill(0, function.values.size());
                auto count = [&](ValueID child) {
                    ++numberOfUses[child.index()];
                };
                for (auto& other : function.blocks) {
                    for (ValueID id : other.values)
                        function.at(id).forEachChild(count);
                    other.terminator.forEachChild(count);
                }
            }
            ValueID literal = cast.child(0);
            while (ValueID behind = numberOfUses[literal.index()] == 1 ? canReplaceParameter(function.at(literal)) : ValueID())
                literal = behind;
            if (!objectLiterals.contains(function.at(literal).opcode))
                continue;
            size_t allocationSite = block.values.find(literal);
            if (allocationSite == notFound || allocationSite > i)
                continue;
            unsigned seenCount = 0;
            bool isUnshared = true;
            for (size_t k = allocationSite + 1; k <= i && isUnshared; ++k) {
                auto& user = function.at(block.values[k]);
                unsigned times = 0;
                user.forEachChild([&](ValueID child) {
                    times += child == literal;
                });
                if (!times)
                    continue;
                seenCount += times;
                bool storesIntoObject = user.opcode == Opcode::Operation && (program.operationOf(user) == Operations::DefineField || program.operationOf(user) == Operations::CopyDataProperties) && !user.arguments.isEmpty() && user.arguments[0] == literal;
                isUnshared = times == 1 && (storesIntoObject || k == i || canReplaceParameter(user) == literal);
            }
            if (isUnshared && seenCount == numberOfUses[literal.index()])
                cast.immediates[1] |= static_cast<uint32_t>(Format::Freshness::TopLevel) << Format::freshnessShift;
        }
    }
}

static void skipConversionsToLayoutsWithoutFieldAccess(Program& program)
{
    const Program& constProgram = program;
    auto& builtins = Builtins::singleton();
    unsigned layoutCount = program.layouts.size();
    Vector<uint8_t> isAccessedDirectly(FillWith { }, layoutCount, static_cast<uint8_t>(0));
    std::atomic<bool> hasFreshCast { false };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = constProgram.functions[i];
        auto markLayout = [&](LayoutID layout) {
            if (!WTF::atomicLoad(&isAccessedDirectly[layout.index()], std::memory_order_relaxed))
                WTF::atomicStore(&isAccessedDirectly[layout.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        auto markStaticType = [&](ValueID id) {
            for (LayoutID supertype : constProgram.typeOf(function, id).objectLayouts)
                markLayout(supertype);
        };
        auto mark = [&](ValueID id) {
            markStaticType(id);
            if (TypeID prediction = function.at(id).prediction()) {
                for (LayoutID supertype : constProgram.at(prediction).objectLayouts)
                    markLayout(supertype);
            }
        };
        auto isAllocatedHere = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                id = function.at(id).child(0);
            auto& value = function.at(id);
            if (value.opcode == Opcode::NewObjectWithShape || value.opcode == Opcode::NewObject)
                return true;
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = builtins.at(value.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Operation)
                return false;
            return implementation.operation == LanguageOperation::CloneObject || implementation.operation == LanguageOperation::CloneObjectWith || implementation.operation == LanguageOperation::CloneObjectAfter || implementation.operation == LanguageOperation::CopyDataPropertiesExcluding;
        };
        static constexpr OpcodeSet accesses { Opcode::GetField, Opcode::HasShapeOf, Opcode::GetById, Opcode::InById, Opcode::DeleteById, Opcode::CallMethod, Opcode::PutById, Opcode::PutField, Opcode::Cast, Opcode::CallBuiltin };
        constProgram.functions.forEachValue(i, accesses, [&](ValueID id) {
            auto& value = function.at(id);
            switch (value.opcode) {
            case Opcode::GetField:
            case Opcode::HasShapeOf:
                markLayout(value.layout());
                [[fallthrough]];
            case Opcode::GetById:
            case Opcode::InById:
            case Opcode::DeleteById:
            case Opcode::CallMethod:
                mark(value.child(0));
                break;
            case Opcode::PutById:
            case Opcode::PutField:
                if (!isAllocatedHere(value.child(0)))
                    mark(value.child(0));
                break;
            case Opcode::Cast:
                if (value.castReason() == CastReason::As || value.castReason() == CastReason::Flow || value.castReason() == CastReason::Coerce)
                    markStaticType(value.child(0));
                if (value.castReason() == CastReason::As && value.coercesFreshData())
                    hasFreshCast.store(true, std::memory_order_relaxed);
                break;
            case Opcode::CallBuiltin: {
                auto& builtin = builtins.at(value.builtin());
                constexpr uint16_t accessesProperties = OperandUse::Gets | OperandUse::WritesProperties;
                if (builtin.hasReceiver() && value.child(0) && builtin.thisUses & accessesProperties)
                    mark(value.child(0));
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    if (builtin.argumentUses(k) & accessesProperties && !isAllocatedHere(value.arguments[k]))
                        mark(value.arguments[k]);
                }
                break;
            }
            default:
                break;
            }
        });
    });
    if (!hasFreshCast.load())
        return;
    BitVector isAccessed(layoutCount);
    BitVector isSpecialization(layoutCount);
    for (unsigned i = 0; i < layoutCount; ++i) {
        for (LayoutID specialization : program.layouts[i].specializations)
            isSpecialization.set(specialization.index());
        if (!isAccessedDirectly[i])
            continue;
        isAccessed.set(i);
        for (LayoutID subtype : program.layouts[i].subtypes)
            isAccessed.set(subtype.index());
    }
    BitVector staysUntyped(layoutCount);
    for (unsigned i = 0; i < layoutCount; ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Record || !layout.acceptsUntypedObjects || layout.checksUntypedObjects() || isAccessed.get(i) || isSpecialization.get(i) || program.retagTargets.get(i))
            continue;
        bool hasAccessedSubtype = std::ranges::any_of(layout.subtypes, [&](LayoutID subtype) {
            return isAccessed.get(subtype.index());
        });
        if (hasAccessedSubtype)
            continue;
        staysUntyped.set(i);
        program.layoutsWithTrivialCoercion.set(i);
    }
    if (staysUntyped.isEmpty())
        return;
    std::atomic<unsigned> castCount { 0 };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet casts { Opcode::Cast };
        constProgram.functions.forEachValue(i, casts, [&](ValueID id) {
            auto& value = constProgram.functions[i].at(id);
            if (value.castReason() != CastReason::As || !value.coercesFreshData())
                return;
            auto& to = constProgram.at(value.type);
            bool staysAsAllocated = !to.objectLayouts.isEmpty() && std::ranges::all_of(to.objectLayouts, [&](LayoutID supertype) {
                return staysUntyped.get(supertype.index());
            });
            if (!staysAsAllocated)
                return;
            program.functions.atKeepingValueIndex(i).at(id).immediates[1] &= ~(3u << Format::freshnessShift);
            castCount.fetch_add(1, std::memory_order_relaxed);
        });
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", staysUntyped.bitCount(), " of ", layoutCount, " layouts accept untyped objects and have no field access: ", castCount.load(), " casts of fresh objects do not convert to them");
}

static void allocateLiteralsWithComputedKeysAsRecords(Program& program)
{
    const Program& constProgram = program;
    std::atomic<unsigned> literalCount { 0 };
    static constexpr OpcodeSet plainAllocations { Opcode::NewObject };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!constProgram.functions.mayContain(i, plainAllocations))
            return;
        auto& function = constProgram.functions[i];
        struct Literal {
            ValueID object;
            ValueID cast;
            LayoutID layout;
            Vector<ValueID, 6> definitions;
            Vector<uint16_t> order;
        };
        Vector<Literal> literals;
        Vector<uint32_t> useCounts;
        auto useCountOf = [&](ValueID id) {
            if (useCounts.isEmpty()) {
                useCounts.fill(0, function.values.size());
                for (auto& block : function.blocks) {
                    for (ValueID user : block.values) {
                        function.at(user).forEachChild([&](ValueID child) {
                            ++useCounts[child.index()];
                        });
                    }
                    if (block.terminator.value)
                        ++useCounts[block.terminator.value.index()];
                    for (auto& target : block.terminator.targets) {
                        for (ValueID argument : target.arguments)
                            ++useCounts[argument.index()];
                    }
                }
            }
            return useCounts[id.index()];
        };
        for (auto& block : function.blocks) {
            for (size_t at = 0; at < block.values.size(); ++at) {
                ValueID object = block.values[at];
                if (function.at(object).opcode != Opcode::NewObject || function.at(object).child(0))
                    continue;
                Literal literal { object, { }, { }, { }, { } };
                unsigned computedKeys = 0;
                bool isRegular = true;
                for (size_t k = at + 1; k < block.values.size() && isRegular && !literal.cast; ++k) {
                    auto& user = function.at(block.values[k]);
                    bool isDefinition = user.opcode == Opcode::CallBuiltin && user.arguments.size() == 3 && user.arguments[0] == object && languageOperationOf(constProgram, user) == LanguageOperation::DefineField;
                    if (isDefinition) {
                        bool isNamed = function.at(user.arguments[1]).opcode == Opcode::String;
                        isRegular = user.arguments[1] != object && user.arguments[2] != object && !(isNamed && computedKeys);
                        if (isNamed)
                            literal.definitions.append(block.values[k]);
                        else
                            ++computedKeys;
                        continue;
                    }
                    if (user.opcode == Opcode::Cast && user.castReason() == CastReason::As && user.child(0) == object) {
                        literal.cast = block.values[k];
                        continue;
                    }
                    user.forEachChild([&](ValueID child) {
                        isRegular &= child != object;
                    });
                }
                if (!isRegular || !literal.cast || !computedKeys || useCountOf(object) != literal.definitions.size() + computedKeys + 1)
                    continue;
                literal.layout = constProgram.typeOf(function, literal.cast).singleLayout();
                if (!literal.layout)
                    continue;
                auto& layout = constProgram.at(literal.layout);
                bool mayBeBornHere = layout.kind == LayoutKind::Record && layout.isAllocated && layout.specializations.isEmpty() && !layout.fields.containsIf([](const Field& field) {
                    return field.flags & fieldIsMethod || !field.literals.isEmpty();
                });
                if (!mayBeBornHere)
                    continue;
                BitVector isDefined;
                for (ValueID definition : literal.definitions) {
                    auto& arguments = function.at(definition).arguments;
                    auto field = layout.fieldNamed(function.at(arguments[1]).string());
                    isRegular = isRegular && field && !isDefined.set(*field) && !(layout.fields[*field].flags & fieldIsNeverStored) && !useCountOf(definition)
                        && constProgram.typeOf(function, arguments[2]).isSubsetOf(constProgram.at(layout.fields[*field].type), constProgram, false);
                    if (!isRegular)
                        break;
                    literal.order.append(static_cast<uint16_t>(*field));
                }
                for (unsigned field = 0; field < layout.fields.size() && isRegular; ++field)
                    isRegular = isDefined.get(field) || layout.fields[field].flags & (fieldIsOptional | fieldIsNeverStored);
                if (isRegular)
                    literals.append(WTF::move(literal));
            }
        }
        if (literals.isEmpty())
            return;
        auto& changed = program.functions[i];
        for (auto& literal : literals) {
            for (unsigned k = 0; k < literal.definitions.size(); ++k) {
                auto& store = changed.at(literal.definitions[k]);
                store.children = { literal.object, store.arguments[2], ValueID() };
                store.arguments = { };
                store.opcode = Opcode::PutField;
                store.immediates = { literal.layout.index(), literal.order[k] };
                store.type = constProgram.types.undefined;
                store.rep = repOf(constProgram.at(store.type));
                store.setPrediction({ });
            }
            Allocation allocation;
            allocation.layout = literal.layout;
            allocation.initialFieldCount = literal.order.size();
            allocation.order = WTF::move(literal.order);
            auto& birth = changed.at(literal.object);
            birth.opcode = Opcode::NewObjectWithShape;
            birth.immediates = { literal.layout.index(), static_cast<uint32_t>(changed.allocations.size()) };
            birth.type = constProgram.at(literal.layout).type;
            birth.setPrediction({ });
            changed.allocations.append(WTF::move(allocation));
        }
        literalCount.fetch_add(literals.size(), std::memory_order_relaxed);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", literalCount.load(), " object literals with computed keys are born as the record type expected of them");
}

static void predictClonesByCastsInOtherBlocks(Program& program)
{
    const Program& constProgram = program;
    std::atomic<unsigned> cloneCount { 0 };
    static constexpr OpcodeSet calls { Opcode::CallBuiltin };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!constProgram.functions.mayContain(i, calls))
            return;
        auto& function = constProgram.functions[i];
        struct Clone {
            ValueID value;
            unsigned block;
            ValueID cast;
            bool isCastInOtherBlock { false };
            bool hasOtherUses { false };
        };
        Vector<Clone, 4> clones;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (function.blocks[b].terminator.kind != TerminatorKind::Invoke)
                continue;
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::CallBuiltin || value.prediction())
                    continue;
                auto operation = languageOperationOf(constProgram, value);
                if (operation == LanguageOperation::CloneObject || operation == LanguageOperation::CloneObjectWith || operation == LanguageOperation::CloneObjectAfter)
                    clones.append({ id, b, { } });
            }
        }
        if (clones.isEmpty())
            return;
        auto cloneFor = [&](ValueID id) -> Clone* {
            for (auto& clone : clones) {
                if (clone.value == id)
                    return &clone;
            }
            return nullptr;
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (ValueID id : block.values) {
                auto& user = function.at(id);
                user.forEachChild([&](ValueID child) {
                    Clone* clone = cloneFor(child);
                    if (!clone)
                        return;
                    bool isStore = user.opcode == Opcode::PutById && user.child(0) == child && user.child(1) != child;
                    bool isCast = user.opcode == Opcode::Cast && user.castReason() == CastReason::As && !clone->cast;
                    if (isCast) {
                        clone->cast = id;
                        clone->isCastInOtherBlock = b != clone->block;
                    } else if (!isStore)
                        clone->hasOtherUses = true;
                });
            }
            auto usesInTerminator = [&](ValueID child) {
                if (Clone* clone = cloneFor(child))
                    clone->hasOtherUses = true;
            };
            if (block.terminator.value)
                usesInTerminator(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    usesInTerminator(argument);
            }
        }
        clones.removeAllMatching([&](const Clone& clone) {
            if (!clone.cast || !clone.isCastInOtherBlock || clone.hasOtherUses)
                return true;
            LayoutID layout = constProgram.typeOf(function, clone.cast).singleLayout();
            return !layout || constProgram.at(layout).kind != LayoutKind::Record || !constProgram.at(layout).isAllocated;
        });
        if (clones.isEmpty())
            return;
        auto& changed = program.functions.atKeepingValueIndex(i);
        for (auto& clone : clones)
            changed.at(clone.value).setPrediction(function.at(clone.cast).type);
        cloneCount.fetch_add(clones.size(), std::memory_order_relaxed);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", cloneCount.load(), " copies of objects are predicted to have the record type they are cast to in another block");
}

static void addPlaceholderFields(Program& program)
{
    const Program& constProgram = program;
    unsigned count = program.functions.size();
    Vector<Vector<uint64_t>> functionReads(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = constProgram.functions[i];
        static constexpr OpcodeSet readsByName { Opcode::GetById };
        constProgram.functions.forEachValue(i, readsByName, [&](ValueID id) {
            auto& value = function.at(id);
            LayoutID supertype = constProgram.typeOf(function, value.child(0)).singleLayout();
            if (!supertype || constProgram.at(supertype).kind != LayoutKind::Record || constProgram.at(supertype).acceptsUntypedObjects || !constProgram.at(supertype).fieldNamed(value.string()))
                return;
            functionReads[i].append(static_cast<uint64_t>(supertype.index()) << 32 | value.string().index());
        });
    });
    Vector<uint64_t> reads;
    for (auto& functionReadList : functionReads)
        reads.appendVector(functionReadList);
    std::ranges::sort(reads);
    Vector<uint8_t> isAllocatedWithFinalLayout(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> isAllocatedByDiscriminant(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet allocations { Opcode::NewObjectWithShape };
        constProgram.functions.forEachValue(i, allocations, [&](ValueID id) {
            auto& value = constProgram.functions[i].at(id);
            WTF::atomicStore(&(value.child(0) ? isAllocatedByDiscriminant : isAllocatedWithFinalLayout)[value.layout().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    BitVector hasSizeAtAllocation(program.layouts.size());
    BitVector isSpecialization(program.layouts.size());
    Vector<uint32_t> worklist;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (isAllocatedByDiscriminant[i])
            worklist.append(i);
        for (LayoutID specialization : constProgram.layouts[i].specializations)
            isSpecialization.set(specialization.index());
    }
    while (!worklist.isEmpty()) {
        uint32_t layout = worklist.takeLast();
        if (hasSizeAtAllocation.set(layout))
            continue;
        for (LayoutID specialization : constProgram.layouts[layout].specializations)
            worklist.append(specialization.index());
    }
    auto isAllocatedWithSpecializationCapacity = [&](LayoutID subtype) {
        bool hasSpecializations = !constProgram.at(subtype).specializations.isEmpty();
        return !hasSizeAtAllocation.get(subtype.index()) && (hasSpecializations || (isSpecialization.get(subtype.index()) && !isAllocatedWithFinalLayout[subtype.index()]));
    };
    struct Candidate {
        StringID name;
        TypeID type;
        Vector<LayoutID, 16> subtypesWithoutField;
        size_t numberOfReads;
    };
    Vector<Candidate> candidates;
    for (size_t next = 0; next < reads.size();) {
        uint64_t read = reads[next];
        size_t numberOfReads = 0;
        for (; next < reads.size() && reads[next] == read; ++next)
            ++numberOfReads;
        LayoutID supertype(static_cast<uint32_t>(read >> 32));
        StringID name(static_cast<uint32_t>(read));
        Vector<LayoutID, 16> subtypesWithoutField;
        unsigned subtypesWithFieldTotal = 0;
        bool areRecords = true;
        TypeID type;
        for (LayoutID subtype : constProgram.at(supertype).subtypes) {
            auto& layout = constProgram.at(subtype);
            if (auto field = layout.fieldNamed(name)) {
                if (!subtypesWithFieldTotal++)
                    type = layout.fields[*field].type;
                continue;
            }
            areRecords &= layout.kind == LayoutKind::Record && isAllocatedWithSpecializationCapacity(subtype);
            subtypesWithoutField.append(subtype);
        }
        if (!areRecords || subtypesWithoutField.isEmpty() || subtypesWithFieldTotal < subtypesWithoutField.size())
            continue;
        candidates.append(Candidate { name, type, WTF::move(subtypesWithoutField), numberOfReads });
    }
    auto haveSamePlaceholder = [](const Candidate& a, const Candidate& b) {
        return a.name == b.name && a.subtypesWithoutField == b.subtypesWithoutField;
    };
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.name != b.name)
            return a.name.index() < b.name.index();
        return std::ranges::lexicographical_compare(a.subtypesWithoutField, b.subtypesWithoutField, { }, &LayoutID::index, &LayoutID::index);
    });
    BitVector hasNewField(program.layouts.size());
    unsigned numberOfPlaceholders = 0;
    constexpr size_t readsPerPlaceholder = 1;
    for (size_t next = 0; next < candidates.size();) {
        auto& candidate = candidates[next];
        size_t numberOfReads = 0;
        for (; next < candidates.size() && haveSamePlaceholder(candidates[next], candidate); ++next)
            numberOfReads += candidates[next].numberOfReads;
        if (numberOfReads < readsPerPlaceholder * candidate.subtypesWithoutField.size())
            continue;
        Vector<uint8_t> text { 0xfe };
        text.append(asBytes("placeholder for "_s.span8()));
        text.append(constProgram.string(candidate.name));
        StringID placeholderName = program.intern(text.span());
        for (LayoutID subtype : candidate.subtypesWithoutField) {
            auto& layout = program.at(subtype);
            bool hasPlaceholder = layout.fields.containsIf([&](auto& field) {
                return field.name == placeholderName;
            });
            if (hasPlaceholder)
                continue;
            Field field;
            field.name = placeholderName;
            field.type = candidate.type;
            field.flags = fieldIsOptional | fieldIsPlaceholder;
            field.placeholderFor = candidate.name;
            layout.fields.append(WTF::move(field));
            hasNewField.set(subtype.index());
            ++numberOfPlaceholders;
        }
    }
    for (size_t index : hasNewField)
        program.layouts[index].indexFields();
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPlaceholders, " placeholder fields in ", hasNewField.bitCount(), " layouts");
}

static void widenUnprofitableLiteralStrings(Program& program, size_t index, const Vector<TypeID>& widened)
{
    auto& function = program.functions.atKeepingValueIndex(index);
    auto hasLiteralString = [&](ValueID id) {
        return program.typeOf(function, id).hasLiteralString();
    };
    BitVector candidates;
    BitVector castsIn;
    for (unsigned b = 1; b < function.blocks.size(); ++b) {
        for (ValueID parameter : function.blocks[b].parameters) {
            if (hasLiteralString(parameter))
                candidates.set(parameter.index());
        }
    }
    static constexpr OpcodeSet casts { Opcode::Cast };
    std::as_const(program).functions.forEachIndexedValue(index, casts, [&](ValueID id) {
        if (hasLiteralString(id) && !hasLiteralString(function.at(id).child(0))) {
            candidates.set(id.index());
            castsIn.set(id.index());
        }
    });
    if (candidates.isEmpty())
        return;

    Vector<unsigned> parent(function.values.size(), [](size_t index) {
        return static_cast<unsigned>(index);
    });
    auto groupOf = [&](ValueID id) {
        unsigned group = id.index();
        while (parent[group] != group)
            group = parent[group] = parent[parent[group]];
        return group;
    };
    BitVector needsWord;
    Vector<unsigned> tests(FillWith { }, function.values.size(), 0u);
    Vector<unsigned> conversions(FillWith { }, function.values.size(), 0u);
    auto isCandidate = [&](ValueID id) {
        return candidates.get(id.index());
    };
    auto markNeedsWord = [&](ValueID id) {
        if (isCandidate(id))
            needsWord.set(id.index());
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            switch (user.opcode) {
            case Opcode::IsUndefinedOrNull:
                continue;
            case Opcode::ToBoolean:
                ++tests[user.child(0).index()];
                continue;
            case Opcode::Cast:
                if (!hasLiteralString(id)) {
                    ++conversions[user.child(0).index()];
                    continue;
                }
                break;
            case Opcode::StrictEq: {
                ValueID left = user.child(0);
                ValueID right = user.child(1);
                if (isCandidate(left) && isCandidate(right))
                    parent[groupOf(left)] = groupOf(right);
                for (auto [one, other] : { std::pair { left, right }, std::pair { right, left } }) {
                    if (!isCandidate(one))
                        continue;
                    ++tests[one.index()];
                    if (!isCandidate(other) && hasLiteralString(other) && function.at(other).opcode != Opcode::String)
                        needsWord.set(one.index());
                }
                continue;
            }
            default:
                break;
            }
            user.forEachChild(markNeedsWord);
        }
        if (block.terminator.value)
            markNeedsWord(block.terminator.value);
        for (auto& target : block.terminator.targets) {
            auto& parameters = function.at(target.block).parameters;
            if (target.arguments.size() != parameters.size()) {
                for (ValueID argument : target.arguments)
                    markNeedsWord(argument);
                for (ValueID parameter : parameters)
                    markNeedsWord(parameter);
                continue;
            }
            for (unsigned i = 0; i < parameters.size(); ++i) {
                ValueID argument = target.arguments[i];
                ValueID parameter = parameters[i];
                if (isCandidate(argument) && isCandidate(parameter))
                    parent[groupOf(argument)] = groupOf(parameter);
                else if (isCandidate(argument)) {
                    if (hasLiteralString(parameter))
                        needsWord.set(argument.index());
                    else
                        ++conversions[argument.index()];
                } else if (isCandidate(parameter) && hasLiteralString(argument) && function.at(argument).opcode != Opcode::String)
                    needsWord.set(parameter.index());
            }
        }
    }

    BitVector groupNeedsWord;
    BitVector groupHasCastIn;
    for (size_t index : candidates) {
        unsigned group = groupOf(ValueID(static_cast<unsigned>(index)));
        if (needsWord.get(index))
            groupNeedsWord.set(group);
        if (castsIn.get(index))
            groupHasCastIn.set(group);
        if (group != index) {
            tests[group] += tests[index];
            conversions[group] += conversions[index];
        }
    }
    auto isWidened = [&](ValueID id) {
        if (!isCandidate(id))
            return false;
        unsigned group = groupOf(id);
        return !groupNeedsWord.get(group) && (groupHasCastIn.get(group) || conversions[group] > tests[group] || !tests[group]);
    };
    BitVector toWiden;
    for (size_t index : candidates) {
        if (isWidened(ValueID(static_cast<unsigned>(index))))
            toWiden.set(index);
    }
    if (toWiden.isEmpty())
        return;

    program.functions.invalidateValueIndex(index);
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        unsigned at = !function.blocks[b].values.isEmpty() && function.at(function.blocks[b].values[0]).opcode == Opcode::CatchValue ? 1 : 0;
        for (unsigned t = 0; t < function.blocks[b].terminator.targets.size(); ++t) {
            BlockID successor = function.blocks[b].terminator.targets[t].block;
            for (unsigned i = 0; i < function.blocks[b].terminator.targets[t].arguments.size(); ++i) {
                ValueID argument = function.blocks[b].terminator.targets[t].arguments[i];
                if (i >= function.at(successor).parameters.size() || !toWiden.get(function.at(successor).parameters[i].index()))
                    continue;
                if (function.at(argument).opcode != Opcode::String || !hasLiteralString(argument))
                    continue;
                ValueID string;
                for (ValueID id : function.blocks[b].values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::String && value.string() == function.at(argument).string() && value.type == program.types.string)
                        string = id;
                }
                if (!string) {
                    Value constant = function.at(argument);
                    constant.type = program.types.string;
                    constant.rep = repOf(program.at(constant.type));
                    string = ValueID(function.values.size());
                    function.values.append(WTF::move(constant));
                    function.blocks[b].values.insert(at, string);
                }
                function.blocks[b].terminator.targets[t].arguments[i] = string;
            }
        }
    }
    for (size_t index : toWiden) {
        auto& value = function.at(ValueID(static_cast<unsigned>(index)));
        value.type = widened[value.type.index()];
        value.rep = repOf(program.at(value.type));
    }

    Vector<ValueID> replacements(function.values.size(), [](size_t index) {
        return ValueID(static_cast<unsigned>(index));
    });
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& cast = function.at(id);
            if (cast.opcode == Opcode::Cast && cast.castReason() == CastReason::Upcast && toWiden.get(cast.child(0).index()) && function.at(cast.child(0)).type == cast.type)
                replacements[id.index()] = cast.child(0);
        }
    }
    BitVector isUsed;
    auto replace = [&](ValueID& child) {
        child = replacements[child.index()];
        isUsed.set(child.index());
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(replace);
        block.terminator.forEachChild(replace);
    }
    for (auto& block : function.blocks) {
        block.values.removeAllMatching([&](ValueID id) {
            return replacements[id.index()] != id || (function.at(id).opcode == Opcode::String && hasLiteralString(id) && !isUsed.get(id.index()));
        });
    }
}

static void widenUnprofitableLiteralStrings(Program& program, bool typesAreFrozen)
{
    Vector<TypeID> widened(program.types.size());
    bool hasAny = false;
    program.types.thaw();
    for (unsigned i = 0; i < widened.size(); ++i) {
        Type type = program.at(TypeID(i));
        if (type.hasLiteralString()) {
            widened[i] = program.types.intern(type.withLiteralStringWidened());
            hasAny = true;
        }
    }
    if (typesAreFrozen)
        program.types.freeze();
    if (!hasAny)
        return;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        widenUnprofitableLiteralStrings(program, i, widened);
    });
}

static void assignLiteralStringIndices(Program& program)
{
    auto number = [&](StringID string) {
        if (program.literalStrings.size() < maxLiteralStrings && program.literalStringIndices.add(string.index(), program.literalStrings.size()).isNewEntry)
            program.literalStrings.append(string);
    };
    number(program.emptyString);
    auto& constProgram = std::as_const(program);
    Vector<Vector<StringID>> stringsIn(constProgram.functions.size());
    forEachIndexInParallel(constProgram.functions.size(), [&](unsigned i) {
        if (!constProgram.hasCode(FunctionID(i)))
            return;
        auto& function = constProgram.functions[i];
        static constexpr OpcodeSet stringsAndComparisons { Opcode::String, Opcode::StrictEq };
        constProgram.functions.forEachIndexedValue(i, stringsAndComparisons, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::String) {
                if (constProgram.at(value.type).hasLiteralString())
                    stringsIn[i].append(value.string());
                return;
            }
            for (unsigned side = 0; side < 2; ++side) {
                ValueID constant = constProgram.constantOf(function, value.child(side));
                ValueID other = value.child(1 - side);
                while (function.at(other).opcode == Opcode::Cast && function.at(other).castReason() == CastReason::Upcast)
                    other = function.at(other).child(0);
                if (constant && function.at(constant).opcode == Opcode::String && constProgram.typeOf(function, other).hasLiteralString())
                    stringsIn[i].append(function.at(constant).string());
            }
        });
    });
    for (auto& strings : stringsIn) {
        for (StringID string : strings)
            number(string);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.literalStrings.size(), " literal strings are numbered");
}

static void createTypedOnlySupertypes(Program& program)
{
    if (program.mayRunInMultipleInstances || Options::auditAOTTypedAssertions())
        return;
    unsigned numberOfLayouts = program.layouts.size();
    program.typedOnlySupertypes.fill(LayoutID(), numberOfLayouts);
    BitVector isSubtype(numberOfLayouts);
    auto subtypesAreStable = [&](const Layout& supertype) {
        for (LayoutID subtype : supertype.subtypes)
            isSubtype.set(subtype.index());
        bool stay = std::ranges::all_of(supertype.subtypes, [&](LayoutID subtype) {
            return std::ranges::all_of(program.at(subtype).specializations, [&](LayoutID specialization) {
                return isSubtype.get(specialization.index());
            });
        });
        for (LayoutID subtype : supertype.subtypes)
            isSubtype.clear(subtype.index());
        return stay;
    };
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (isObjectLayout(program.layouts[i].kind) && !mayDespecialize(program, LayoutID(i)) && subtypesAreStable(program.layouts[i]))
            program.supertypesWithStableSubtypes.set(i);
    }
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (program.layouts[i].kind != LayoutKind::Record || !program.layouts[i].acceptsUntypedObjects || program.layouts[i].isRelaxedForLogging || program.layouts[i].subtypes.isEmpty() || !program.supertypesWithStableSubtypes.get(i))
            continue;
        Layout supertype = program.layouts[i];
        supertype.acceptsUntypedObjects = false;
        supertype.acceptsOnlyOtherLayouts = false;
        supertype.isTypedOnlySupertype = true;
        supertype.isAllocated = false;
        supertype.mayDespecialize = false;
        supertype.specializations.clear();
        supertype.orders.clear();
        supertype.cloneAllocation = Format::none;
        LayoutID id(program.layouts.size());
        supertype.type = program.types.intern(Type::forLayout(id));
        program.typedOnlySupertypes[i] = id;
        program.layouts.append(WTF::move(supertype));
    }
}

static TypeID findOnceByLayoutAndName(std::array<Program::LayoutAndNameTypeCache, 64>& cache, LayoutID layout, StringID name, const Invocable<TypeID()> auto& compute)
{
    uint64_t key = static_cast<uint64_t>(layout.index()) << 32 | name.index();
    auto& table = cache[(layout.index() * 31 + name.index()) % cache.size()];
    {
        Locker locker { table.lock };
        if (auto found = table.types.find(key); found != table.types.end())
            return found->value;
    }
    TypeID type = compute();
    Locker locker { table.lock };
    table.types.add(key, type);
    return type;
}

static bool widenTo(const Program& program, TypeID& widest, TypeID type)
{
    if (!widest || program.at(widest).isSubsetOf(program.at(type), program, false)) {
        widest = type;
        return true;
    }
    return program.at(type).isSubsetOf(program.at(widest), program, false);
}

static TypeID widestTypeStoredThroughSupertypes(const Program& program, LayoutID subtype, StringID name)
{
    return findOnceByLayoutAndName(program.widestTypeStoredThroughSupertypesCache, subtype, name, [&] {
        TypeID widest;
        for (uint32_t i = program.firstSupertypeOfLayout[subtype.index()]; i < program.firstSupertypeOfLayout[subtype.index() + 1]; ++i) {
            LayoutID other = program.supertypesOfLayouts[i];
            if (!program.fieldsStoredThroughLayouts.contains(static_cast<uint64_t>(other.index()) << 32 | name.index()))
                continue;
            auto field = program.at(other).fieldNamed(name);
            if (!field)
                continue;
            TypeID type = program.at(other).fields[*field].type;
            if (!type || !widenTo(program, widest, type))
                return program.types.anything;
        }
        return widest;
    });
}

static TypeID computeWidestStoredFieldType(const Program& program, LayoutID supertype, StringID name)
{
    TypeID widest;
    for (LayoutID subtype : program.at(supertype).subtypes) {
        auto& layout = program.at(subtype);
        auto field = layout.fieldNamed(name);
        if (!field)
            return { };
        TypeID type = layout.fields[*field].type;
        if (!type)
            return { };
        if (!(layout.fields[*field].flags & fieldIsNeverObservedAbsentAsDeclared) && !(program.at(type).primitives & Primitive::Undefined))
            return { };
        if (!widenTo(program, widest, type))
            return { };
        if (TypeID stored = widestTypeStoredThroughSupertypes(program, subtype, name); stored && !widenTo(program, widest, stored))
            return { };
    }
    return widest;
}

TypeID widestStoredFieldType(const Program& program, LayoutID supertype, StringID name)
{
    if (program.firstSupertypeOfLayout.isEmpty())
        return { };
    return findOnceByLayoutAndName(program.widestStoredFieldTypeCache, supertype, name, [&] {
        return computeWidestStoredFieldType(program, supertype, name);
    });
}

static constexpr int bytesSavedByAccess = 32;
static constexpr int maxSavedBytes = 512;

static unsigned estimatedCloneSize(const Value& value)
{
    switch (value.opcode) {
    case Opcode::Parameter:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::Unspecified:
    case Opcode::Results:
    case Opcode::ResultAt:
    case Opcode::LoopHint:
    case Opcode::UnlikelyHint:
        return 0;
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::FunctionReference:
    case Opcode::BuiltinFunction:
        return 7;
    case Opcode::Cast:
        return value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Member ? 0 : 16;
    case Opcode::CallDirect:
        return 8 + 4 * value.arguments.size();
    case Opcode::Call:
    case Opcode::Construct:
    case Opcode::CallMethod:
    case Opcode::CallBuiltin:
    case Opcode::Operation:
        return 24 + 4 * value.arguments.size();
    case Opcode::NewObjectWithShape:
    case Opcode::NewObject:
    case Opcode::NewArray:
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
        return 48;
    case Opcode::GetById:
    case Opcode::PutById:
    case Opcode::GetByVal:
    case Opcode::PutByVal:
        return 20;
    default:
        return 8;
    }
}

static bool suspendsFunction(const Value& value)
{
    return value.opcode == Opcode::Await || value.opcode == Opcode::Yield || value.opcode == Opcode::GetResumeMode;
}

static bool canExecuteTwice(const Value& value, Writes writes)
{
    if (writes <= Writes::Nothing)
        return true;
    if (writes != Writes::RuntimeState)
        return false;
    switch (value.opcode) {
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::FunctionReference:
    case Opcode::IsBuiltinFunction:
    case Opcode::BuiltinFunction:
    case Opcode::StrictEq:
    case Opcode::Eq:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::ToBoolean:
    case Opcode::TypeOf:
    case Opcode::Add:
    case Opcode::Strcat:
    case Opcode::GetById:
        return true;
    default:
        return false;
    }
}

static bool foldRepeatedShapeGuards(const Program& program, Function& function)
{
    auto shapeTestOf = [&](const Block& block) -> const Value* {
        if (block.terminator.kind != TerminatorKind::Branch || block.terminator.targets[0].block == block.terminator.targets[1].block)
            return nullptr;
        auto& test = function.at(block.terminator.value);
        return test.opcode == Opcode::HasShapeOf && (!test.immediates[1] || test.immediates[1] == shapeTestGuardsFastPath) && program.supertypesWithStableSubtypes.get(test.layout().index()) ? &test : nullptr;
    };
    auto keyOf = [](const Value& test) {
        return static_cast<uint64_t>(test.child(0).index()) << 32 | test.layout().index();
    };
    unsigned numberOfBlocks = function.blocks.size();
    unsigned numberOfGuards = 0;
    unsigned numberOfTests = 0;
    for (auto& block : function.blocks) {
        if (auto* test = shapeTestOf(block)) {
            ++numberOfTests;
            numberOfGuards += test->immediates[1] == shapeTestGuardsFastPath;
        }
    }
    if (!numberOfGuards || numberOfTests < 2)
        return false;
    Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    BitVector suspends(numberOfBlocks);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        for (ValueID id : function.blocks[b].values) {
            if (suspendsFunction(function.at(id)))
                suspends.set(b);
        }
    }
    using Passed = Vector<uint64_t, 4>;
    Vector<std::optional<Passed>> testsPassedAtEnd(numberOfBlocks);
    auto testsPassedAtEndOf = [&](unsigned b) {
        std::optional<Passed> passed;
        if (!b || suspends.get(b))
            return std::optional<Passed>(Passed());
        for (unsigned predecessor : predecessors[b]) {
            if (!testsPassedAtEnd[predecessor])
                continue;
            Passed onEdge = *testsPassedAtEnd[predecessor];
            auto& from = function.blocks[predecessor];
            if (auto* test = shapeTestOf(from); test && from.terminator.targets[0].block.index() == b && !onEdge.contains(keyOf(*test)))
                onEdge.append(keyOf(*test));
            if (!passed)
                passed = WTF::move(onEdge);
            else {
                passed->removeAllMatching([&](uint64_t key) {
                    return !onEdge.contains(key);
                });
            }
        }
        if (passed)
            std::ranges::sort(*passed);
        return passed;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto passed = testsPassedAtEndOf(b);
            if (passed && (!testsPassedAtEnd[b] || *testsPassedAtEnd[b] != *passed)) {
                testsPassedAtEnd[b] = WTF::move(passed);
                changed = true;
            }
        }
    }
    bool folded = false;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        auto* test = shapeTestOf(block);
        if (!test || test->immediates[1] != shapeTestGuardsFastPath || !testsPassedAtEnd[b] || !testsPassedAtEnd[b]->contains(keyOf(*test)))
            continue;
        block.terminator.kind = TerminatorKind::Jump;
        block.terminator.value = ValueID();
        block.terminator.targets.shrink(1);
        folded = true;
    }
    return folded;
}

enum class SplitOutcome : uint8_t { Split, FoundNothingToSplit, SkippedAccesses, CouldNotJoinPaths };

static SplitOutcome trySplitSubtypeFastPaths(const Program& program, FunctionID functionID, Function& function, BitVector& skippedAccesses, bool allocationsInvalidateGuards)
{
    static constexpr uint32_t none = UINT32_MAX;
    struct GuardedLayout {
        LayoutID tested;
        LayoutID onFastPath;
    };
    auto guardedLayoutOf = [&](ValueID receiver) {
        auto& value = function.at(receiver);
        LayoutID supertype = value.type ? program.at(value.type).singleLayout() : LayoutID();
        if (!supertype || supertype.index() >= program.typedOnlySupertypes.size())
            return GuardedLayout { };
        return GuardedLayout { supertype, program.typedOnlySupertypes[supertype.index()] };
    };
    auto isGuardableAccess = [&](unsigned b, size_t index) {
        auto& block = function.blocks[b];
        auto& value = function.at(block.values[index]);
        if (value.opcode != Opcode::GetById && value.opcode != Opcode::PutById)
            return false;
        if (skippedAccesses.get(block.values[index].index()))
            return false;
        if (value.hasKnownSlotWord() || value.immediates[1] != Format::none)
            return false;
        if (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size())
            return false;
        GuardedLayout supertype = guardedLayoutOf(value.child(0));
        if (!supertype.onFastPath)
            return false;
        if (!program.at(supertype.onFastPath).fieldNamed(value.string()) || !prototypeChainLacksProperty(program, supertype.onFastPath, value.string()))
            return false;
        return !isLocalAllocation(function, value.child(0));
    };
    bool hasSharedReceiver = false;
    BitVector accessReceivers;
    for (unsigned b = 0; b < function.blocks.size() && !hasSharedReceiver; ++b) {
        for (size_t index = 0; index < function.blocks[b].values.size() && !hasSharedReceiver; ++index)
            hasSharedReceiver = isGuardableAccess(b, index) && accessReceivers.set(function.at(function.blocks[b].values[index]).child(0).index());
    }
    for (unsigned b = 0; b < function.blocks.size() && !hasSharedReceiver && !accessReceivers.isEmpty(); ++b) {
        auto& terminator = function.blocks[b].terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        auto& test = function.at(terminator.value);
        hasSharedReceiver = test.opcode == Opcode::HasShapeOf && !test.immediates[1] && accessReceivers.get(test.child(0).index());
    }
    if (!hasSharedReceiver)
        return SplitOutcome::FoundNothingToSplit;

    unsigned numberOfValuesAtStart = function.values.size();
    UncheckedKeyHashMap<uint32_t, Value, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> readsOnSlowPath;
    auto isGuard = [&](ValueID id) {
        return id && id.index() >= numberOfValuesAtStart && function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Member;
    };
    std::optional<Function> functionWithGuards;
    auto giveUp = [&](SplitOutcome outcome) {
        if (functionWithGuards)
            function = WTF::move(*functionWithGuards);
        for (auto& block : function.blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                if (isGuard(id))
                    return true;
                if (ValueID guard = function.at(id).child(0); isGuard(guard))
                    function.at(id).children[0] = function.at(guard).child(0);
                return false;
            });
        }
        for (auto& read : readsOnSlowPath) {
            ValueID receiver = function.at(ValueID(read.key)).child(0);
            function.at(ValueID(read.key)) = WTF::move(read.value);
            function.at(ValueID(read.key)).children[0] = receiver;
        }
        function.values.shrink(numberOfValuesAtStart);
        return outcome;
    };
    auto add = [&](Value&& value) {
        function.values.append(WTF::move(value));
        return ValueID(function.values.size() - 1);
    };

    auto addGuard = [&](ValueID receiver, LayoutID supertype, uint32_t position, uint32_t inlineFrame) {
        Value cast;
        cast.opcode = Opcode::Cast;
        cast.position = position;
        cast.inlineFrame = inlineFrame;
        cast.type = program.at(supertype).type;
        cast.rep = repOf(program.at(cast.type));
        cast.children = { receiver };
        cast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Member) };
        return add(WTF::move(cast));
    };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutTestedBy;
    BitVector isGuardedReceiver;
    BitVector isReadWithNarrowedType;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        for (size_t index = 0; index < block.values.size(); ++index) {
            if (!isGuardableAccess(b, index))
                continue;
            ValueID access = block.values[index];
            ValueID receiver = function.at(access).child(0);
            GuardedLayout supertype = guardedLayoutOf(receiver);
            ValueID guard = addGuard(receiver, supertype.onFastPath, function.at(access).position, function.at(access).inlineFrame);
            layoutTestedBy.add(guard.index(), supertype.tested.index());
            isGuardedReceiver.set(receiver.index());
            block.values.insert(index++, guard);
            auto& value = function.at(access);
            value.children[0] = guard;
            if (value.opcode != Opcode::GetById || !value.type)
                continue;
            if (!program.at(value.type).isAnything())
                continue;
            TypeID type = widestStoredFieldType(program, supertype.onFastPath, value.string());
            if (!type || type == value.type)
                continue;
            readsOnSlowPath.add(access.index(), Value(value));
            isReadWithNarrowedType.set(access.index());
            value.type = type;
            value.rep = repOf(program.at(type));
            value.copyPredictionFrom(Value { });
        }
    }

    unsigned numberOfBlocks = function.blocks.size();
    Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    BitVector isAlreadyTested;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        if (predecessors[b].size() != 1)
            continue;
        auto& terminator = function.blocks[predecessors[b][0]].terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.targets[0].block.index() != b || terminator.targets[1].block.index() == b)
            continue;
        ValueID tested = function.at(terminator.value).child(0);
        if (function.at(terminator.value).opcode != Opcode::HasShapeOf || function.at(terminator.value).immediates[1] || !isGuardedReceiver.get(tested.index()))
            continue;
        GuardedLayout supertype = guardedLayoutOf(tested);
        if (supertype.tested != function.at(terminator.value).layout())
            continue;
        ValueID guard = addGuard(tested, supertype.onFastPath, function.at(terminator.value).position, function.at(terminator.value).inlineFrame);
        isAlreadyTested.set(guard.index());
        function.blocks[b].values.insert(0, guard);
    }
    BitVector invalidatesAllGuards(function.values.size());
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (!isGuard(id) && (suspendsFunction(function.at(id)) || (allocationsInvalidateGuards && function.at(id).opcode == Opcode::NewObjectWithShape)))
                invalidatesAllGuards.set(id.index());
        }
    }
    Vector<uint32_t> receiverIndex(FillWith { }, numberOfValuesAtStart, none);
    Vector<uint32_t> narrowedReadIndex(FillWith { }, isReadWithNarrowedType.isEmpty() ? 0 : numberOfValuesAtStart, none);
    unsigned numberOfFacts = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isGuard(id)) {
                if (uint32_t& number = receiverIndex[function.at(id).child(0).index()]; number == none)
                    number = numberOfFacts++;
            } else if (isReadWithNarrowedType.get(id.index()))
                narrowedReadIndex[id.index()] = numberOfFacts++;
        }
    }
    auto receiverIndexOf = [&](ValueID guard) {
        return receiverIndex[function.at(guard).child(0).index()];
    };
    Vector<std::optional<BitVector>> testedAtEnd(numberOfBlocks);
    auto testsAtStartOf = [&](unsigned b) {
        std::optional<BitVector> tested;
        if (!b)
            tested = BitVector();
        for (unsigned predecessor : predecessors[b]) {
            if (!testedAtEnd[predecessor])
                continue;
            if (!tested)
                tested = *testedAtEnd[predecessor];
            else
                tested->filter(*testedAtEnd[predecessor]);
        }
        return tested;
    };
    Vector<int> savedBytesAtEnd(FillWith { }, numberOfBlocks, 0);
    auto savedBytesAtStartOf = [&](unsigned b) {
        int savedBytes = maxSavedBytes;
        for (unsigned predecessor : predecessors[b]) {
            if (testedAtEnd[predecessor])
                savedBytes = std::min(savedBytes, savedBytesAtEnd[predecessor]);
        }
        return savedBytes;
    };
    auto walkForward = [&](unsigned b, BitVector& tested, const auto& atGuard, const auto& onSavingsSpent) {
        int savedBytes = savedBytesAtStartOf(b);
        auto& block = function.blocks[b];
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            if (isGuard(id)) {
                bool beginsStretch = tested.isEmpty();
                bool wasTested = tested.set(receiverIndexOf(id));
                savedBytes = std::min(maxSavedBytes, (beginsStretch ? 0 : std::max(savedBytes, 0)) + bytesSavedByAccess);
                atGuard(id, wasTested);
                continue;
            }
            if (invalidatesAllGuards.get(id.index())) {
                tested.clearAll();
                continue;
            }
            if (tested.isEmpty() || isGuard(function.at(id).child(0)))
                continue;
            savedBytes -= estimatedCloneSize(function.at(id));
            if (savedBytes < 0) {
                tested.clearAll();
                onSavingsSpent(id);
            }
        }
        return tested.isEmpty() ? 0 : std::max(savedBytes - (function.blocks[b].terminator.kind == TerminatorKind::Jump ? 0 : 8), -1);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto tested = testsAtStartOf(b);
            if (!tested)
                continue;
            int savedBytes = walkForward(b, *tested, [](ValueID, bool) { }, [](ValueID) { });
            if (!testedAtEnd[b] || *testedAtEnd[b] != *tested || savedBytesAtEnd[b] != savedBytes) {
                testedAtEnd[b] = WTF::move(tested);
                savedBytesAtEnd[b] = savedBytes;
                changed = true;
            }
        }
    }
    BitVector isCoveredByEarlierTests;
    BitVector spendsSavings;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto tested = testsAtStartOf(b);
        if (!tested)
            continue;
        walkForward(b, *tested, [&](ValueID id, bool wasTested) {
            if (wasTested && !isAlreadyTested.get(id.index()))
                isCoveredByEarlierTests.set(id.index());
        }, [&](ValueID id) {
            spendsSavings.set(id.index());
        });
    }
    invalidatesAllGuards.merge(spendsSavings);
    BitVector isUsedGuard;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (ValueID receiver = function.at(id).child(0); isGuard(receiver))
                isUsedGuard.set(receiver.index());
        }
    }
    for (auto& block : function.blocks) {
        block.values.removeAllMatching([&](ValueID id) {
            return isGuard(id) && !isUsedGuard.get(id.index());
        });
    }

    Vector<BitVector> liveAtStart(numberOfBlocks);
    auto walkBackward = [&](unsigned b, const auto& atPoint) {
        auto& block = function.blocks[b];
        BitVector live;
        for (auto& target : block.terminator.targets)
            live.merge(liveAtStart[target.block.index()]);
        auto use = [&](ValueID child) {
            if (isGuard(child))
                live.set(receiverIndexOf(child));
            else if (isReadWithNarrowedType.get(child.index()))
                live.set(narrowedReadIndex[child.index()]);
        };
        block.terminator.forEachChild([&](ValueID child) {
            use(child);
            if (function.at(child).opcode == Opcode::Results)
                std::as_const(function).at(child).forEachChild(use);
        });
        atPoint(block.values.size(), live);
        for (size_t index = block.values.size(); index--;) {
            ValueID id = block.values[index];
            if (invalidatesAllGuards.get(id.index()))
                live.clearAll();
            else if (isGuard(id)) {
                if (!isCoveredByEarlierTests.get(id.index()))
                    live.clear(receiverIndexOf(id));
            } else {
                if (isReadWithNarrowedType.get(id.index()))
                    live.clear(narrowedReadIndex[id.index()]);
                std::as_const(function).at(id).forEachChild(use);
            }
            atPoint(index, live);
        }
        return live;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = numberOfBlocks; b--;) {
            BitVector live = walkBackward(b, [](size_t, const BitVector&) { });
            if (live != liveAtStart[b]) {
                liveAtStart[b] = WTF::move(live);
                changed = true;
            }
        }
    }
    if (!liveAtStart[0].isEmpty())
        return giveUp(SplitOutcome::FoundNothingToSplit);

    struct Segment {
        BlockValues values;
        bool isGuarded { false };
        ValueID guardAtEnd;
        unsigned block { 0 };
        unsigned slowBlock { none };
    };
    Vector<Vector<Segment, 2>> segments(numberOfBlocks);
    unsigned numberOfNewBlocks = numberOfBlocks;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        Vector<bool> isGuardedBefore(FillWith { }, block.values.size() + 1, false);
        walkBackward(b, [&](size_t index, const BitVector& live) {
            isGuardedBefore[index] = !live.isEmpty();
        });
        auto& list = segments[b];
        list.append(Segment { { }, isGuardedBefore[0], { }, b, none });
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            if (isCoveredByEarlierTests.get(id.index()) && !isGuardedBefore[index])
                return giveUp(SplitOutcome::FoundNothingToSplit);
            if (isGuard(id) && !isCoveredByEarlierTests.get(id.index())) {
                if (!isGuardedBefore[index + 1])
                    return giveUp(SplitOutcome::FoundNothingToSplit);
                if (!isAlreadyTested.get(id.index()))
                    list.last().guardAtEnd = id;
                else if (list.last().isGuarded) {
                    list.last().values.append(id);
                    continue;
                }
                list.append(Segment { { id }, true, { }, numberOfNewBlocks++, none });
                continue;
            }
            if (isGuardedBefore[index] != list.last().isGuarded)
                list.append(Segment { { }, isGuardedBefore[index], { }, numberOfNewBlocks++, none });
            list.last().values.append(id);
        }
        if (isGuardedBefore[block.values.size()] != list.last().isGuarded)
            list.append(Segment { { }, isGuardedBefore[block.values.size()], { }, numberOfNewBlocks++, none });
    }
    for (auto& list : segments) {
        for (auto& segment : list) {
            if (segment.isGuarded)
                segment.slowBlock = numberOfNewBlocks++;
        }
    }
    {
        Vector<unsigned> firstSegmentOf(numberOfBlocks);
        unsigned numberOfSegments = 0;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            firstSegmentOf[b] = numberOfSegments;
            numberOfSegments += segments[b].size();
        }
        Vector<unsigned> stretchOf(numberOfSegments);
        for (unsigned i = 0; i < numberOfSegments; ++i)
            stretchOf[i] = i;
        auto find = [&](unsigned i) {
            while (stretchOf[i] != i) {
                stretchOf[i] = stretchOf[stretchOf[i]];
                i = stretchOf[i];
            }
            return i;
        };
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& list = segments[b];
            for (unsigned s = 1; s < list.size(); ++s) {
                if (list[s - 1].isGuarded && list[s].isGuarded)
                    stretchOf[find(firstSegmentOf[b] + s)] = find(firstSegmentOf[b] + s - 1);
            }
            if (!list.last().isGuarded)
                continue;
            for (auto& target : function.blocks[b].terminator.targets) {
                if (segments[target.block.index()][0].isGuarded)
                    stretchOf[find(firstSegmentOf[target.block.index()])] = find(firstSegmentOf[b] + list.size() - 1);
            }
        }
        Vector<unsigned> numberOfAccessesIn(FillWith { }, numberOfSegments, 0u);
        auto forEachCoveredAccess = [&](const auto& functor) {
            for (unsigned b = 0; b < numberOfBlocks; ++b) {
                for (unsigned s = 0; s < segments[b].size(); ++s) {
                    for (ValueID id : segments[b][s].values) {
                        if (ValueID guard = function.at(id).child(0); isGuard(guard))
                            functor(find(firstSegmentOf[b] + s), id, guard);
                    }
                }
            }
        };
        forEachCoveredAccess([&](unsigned stretch, ValueID, ValueID guard) {
            numberOfAccessesIn[stretch] += isAlreadyTested.get(guard.index()) ? 2 : 1;
        });
        bool leavesAccesses = false;
        forEachCoveredAccess([&](unsigned stretch, ValueID access, ValueID) {
            if (numberOfAccessesIn[stretch] < 2) {
                skippedAccesses.set(access.index());
                leavesAccesses = true;
            }
        });
        if (leavesAccesses)
            return giveUp(SplitOutcome::SkippedAccesses);
    }
    BitVector isExecutedOnce(function.values.size());
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        size_t index = 0;
        for (auto& segment : segments[b]) {
            for (ValueID id : segment.values) {
                RELEASE_ASSERT(block.values[index] == id);
                if (segment.isGuarded && !isGuard(id) && (invalidatesAllGuards.get(id.index()) || function.at(id).opcode == Opcode::CallDirect || !canExecuteTwice(function.at(id), writesOf(program, function, block, index))))
                    isExecutedOnce.set(id.index());
                ++index;
            }
        }
    }
    static constexpr uint32_t unknown = UINT32_MAX - 1;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> slowBlockAfter;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> restartGuard;
    Vector<uint32_t> restartAtStart(FillWith { }, numberOfBlocks, unknown);
    Vector<uint32_t> restartAtEnd(FillWith { }, numberOfBlocks, unknown);
    auto findRestartPoints = [&](unsigned b, bool records) {
        uint32_t guard = b ? unknown : none;
        for (unsigned predecessor : predecessors[b]) {
            uint32_t other = restartAtEnd[predecessor];
            if (other == unknown)
                continue;
            guard = guard == unknown || guard == other ? other : none;
        }
        if (guard == unknown)
            return unknown;
        if (restartAtStart[b] != unknown && restartAtStart[b] != guard)
            guard = none;
        restartAtStart[b] = guard;
        auto& list = segments[b];
        for (unsigned s = 0; s < list.size(); ++s) {
            if (!list[s].isGuarded)
                guard = none;
            for (ValueID id : list[s].values) {
                if (isExecutedOnce.get(id.index()))
                    guard = none;
            }
            if (!list[s].guardAtEnd)
                continue;
            if (guard == none)
                guard = list[s].guardAtEnd.index();
            if (records) {
                slowBlockAfter.set(list[s].guardAtEnd.index(), list[s + 1].slowBlock);
                restartGuard.set(list[s].guardAtEnd.index(), guard);
            }
        }
        return guard;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            uint32_t guard = findRestartPoints(b, false);
            if (guard != restartAtEnd[b]) {
                restartAtEnd[b] = guard;
                changed = true;
            }
        }
    }
    for (unsigned b = 0; b < numberOfBlocks; ++b)
        findRestartPoints(b, true);

    functionWithGuards = function;
    Vector<uint32_t> slowCopyOf(FillWith { }, function.values.size(), none);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto copy = [&](ValueID id) {
            auto found = readsOnSlowPath.find(id.index());
            Value value(found == readsOnSlowPath.end() ? function.at(id) : found->value);
            value.isOnSlowPath = isGuard(value.child(0));
            slowCopyOf[id.index()] = add(WTF::move(value)).index();
        };
        if (segments[b][0].isGuarded) {
            for (ValueID id : function.blocks[b].parameters)
                copy(id);
        }
        for (auto& segment : segments[b]) {
            if (!segment.isGuarded)
                continue;
            for (ValueID id : segment.values) {
                if (!isGuard(id))
                    copy(id);
            }
        }
    }
    Vector<uint32_t> slowCopyOriginal(FillWith { }, function.values.size(), none);
    for (unsigned i = 0; i < slowCopyOf.size(); ++i) {
        if (slowCopyOf[i] != none)
            slowCopyOriginal[slowCopyOf[i]] = i;
    }
    auto onSlowPath = [&](ValueID id) {
        while (isGuard(id))
            id = function.at(id).child(0);
        return id && id.index() < slowCopyOf.size() && slowCopyOf[id.index()] != none ? ValueID(slowCopyOf[id.index()]) : id;
    };
    auto addShapeTest = [&](ValueID guard) {
        ValueID receiver = function.at(guard).child(0);
        Value test;
        test.opcode = Opcode::HasShapeOf;
        test.position = function.at(guard).position;
        test.inlineFrame = function.at(guard).inlineFrame;
        test.type = program.types.boolean;
        test.rep = repOf(program.at(test.type));
        test.children[0] = receiver;
        test.immediates = { layoutTestedBy.get(guard.index()), shapeTestGuardsFastPath };
        return add(WTF::move(test));
    };

    Vector<Block> blocks;
    blocks.grow(numberOfNewBlocks);
    BitVector isSlowBlock(numberOfNewBlocks);
    auto jump = [&](Block& block, unsigned to) {
        block.terminator.kind = TerminatorKind::Jump;
        block.terminator.targets.append(Target { BlockID(to), { } });
    };
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        Block old = WTF::move(function.blocks[b]);
        auto& list = segments[b];
        for (unsigned s = 0; s < list.size(); ++s) {
            auto& segment = list[s];
            bool isLast = s + 1 == list.size();
            if (segment.isGuarded) {
                Block& slow = blocks[segment.slowBlock];
                isSlowBlock.set(segment.slowBlock);
                if (!s) {
                    for (ValueID id : old.parameters) {
                        function.at(onSlowPath(id)).immediates = { segment.slowBlock, static_cast<uint32_t>(slow.parameters.size()) };
                        slow.parameters.append(onSlowPath(id));
                    }
                }
                for (ValueID id : segment.values) {
                    if (isGuard(id))
                        continue;
                    function.at(onSlowPath(id)).forEachChild([&](ValueID& child) {
                        child = onSlowPath(child);
                    });
                    slow.values.append(onSlowPath(id));
                }
                if (isLast) {
                    slow.terminator = old.terminator;
                    slow.terminator.forEachChild([&](ValueID& child) {
                        child = onSlowPath(child);
                    });
                    for (auto& target : slow.terminator.targets) {
                        auto& first = segments[target.block.index()][0];
                        if (first.isGuarded)
                            target.block = BlockID(first.slowBlock);
                    }
                } else
                    jump(slow, list[s + 1].isGuarded ? list[s + 1].slowBlock : list[s + 1].block);
            }
            Block& block = blocks[segment.block];
            if (!s)
                block.parameters = WTF::move(old.parameters);
            block.values = segment.values;
            if (segment.guardAtEnd) {
                RELEASE_ASSERT(!isLast && list[s + 1].isGuarded);
                ValueID test = addShapeTest(segment.guardAtEnd);
                block.values.append(test);
                block.terminator.kind = TerminatorKind::Branch;
                block.terminator.value = test;
                block.terminator.targets.append(Target { BlockID(list[s + 1].block), { } });
                auto restartAt = restartGuard.find(segment.guardAtEnd.index());
                block.terminator.targets.append(Target { BlockID(restartAt == restartGuard.end() ? list[s + 1].slowBlock : slowBlockAfter.get(restartAt->value)), { } });
            } else if (isLast)
                block.terminator = old.terminator;
            else
                jump(block, list[s + 1].block);
        }
    }
    function.blocks = WTF::move(blocks);
    BitVector isReachable(numberOfNewBlocks);
    {
        Vector<unsigned> worklist { 0u };
        isReachable.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReachable.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
    }
    for (unsigned b = 0; b < numberOfNewBlocks; ++b) {
        if (!isReachable.get(b)) {
            function.blocks[b] = Block { };
            isSlowBlock.clear(b);
        }
    }

    Vector<uint32_t> definedIn(FillWith { }, function.values.size(), none);
    struct Edge {
        unsigned from;
        unsigned target;
    };
    Vector<Vector<Edge, 2>> incoming(numberOfNewBlocks);
    for (unsigned b = 0; b < numberOfNewBlocks; ++b) {
        auto& block = function.blocks[b];
        for (ValueID id : block.parameters)
            definedIn[id.index()] = b;
        for (ValueID id : block.values)
            definedIn[id.index()] = b;
        for (unsigned t = 0; t < block.terminator.targets.size(); ++t)
            incoming[block.terminator.targets[t].block.index()].append(Edge { b, t });
    }
    BitVector isReachedFromSlowBlocks(numberOfNewBlocks);
    {
        Vector<unsigned> worklist;
        for (unsigned b : isSlowBlock)
            worklist.append(b);
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            for (auto& target : function.blocks[b].terminator.targets) {
                if (!isReachedFromSlowBlocks.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
    }
    auto isConstant = [](Opcode opcode) {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
        case Opcode::Unspecified:
            return true;
        default:
            return false;
        }
    };
    Vector<unsigned> order;
    Vector<uint32_t> orderIndex(FillWith { }, numberOfNewBlocks, none);
    {
        BitVector seen(numberOfNewBlocks);
        seen.set(0);
        Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
        while (!stack.isEmpty()) {
            auto& [block, next] = stack.last();
            auto& targets = function.blocks[block].terminator.targets;
            if (next == targets.size()) {
                order.append(block);
                stack.removeLast();
                continue;
            }
            unsigned target = targets[next++].block.index();
            if (!seen.set(target))
                stack.append({ target, 0u });
        }
        order.reverse();
        for (unsigned i = 0; i < order.size(); ++i)
            orderIndex[order[i]] = i;
    }
    Vector<uint32_t> dominator(FillWith { }, numberOfNewBlocks, none);
    dominator[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = 1; i < order.size(); ++i) {
            uint32_t found = none;
            for (auto& edge : incoming[order[i]]) {
                uint32_t other = edge.from;
                if (dominator[other] == none)
                    continue;
                if (found == none) {
                    found = other;
                    continue;
                }
                while (found != other) {
                    while (orderIndex[other] > orderIndex[found])
                        other = dominator[other];
                    while (orderIndex[found] > orderIndex[other])
                        found = dominator[found];
                }
            }
            if (dominator[order[i]] != found) {
                dominator[order[i]] = found;
                changed = true;
            }
        }
    }
    Vector<uint32_t> enter(FillWith { }, numberOfNewBlocks, 0u);
    Vector<uint32_t> leave(FillWith { }, numberOfNewBlocks, 0u);
    {
        Vector<Vector<unsigned, 2>> dominated(numberOfNewBlocks);
        for (unsigned i = 1; i < order.size(); ++i)
            dominated[dominator[order[i]]].append(order[i]);
        uint32_t clock = 0;
        Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
        enter[0] = ++clock;
        while (!stack.isEmpty()) {
            auto& [block, next] = stack.last();
            if (next == dominated[block].size()) {
                leave[block] = ++clock;
                stack.removeLast();
                continue;
            }
            unsigned inner = dominated[block][next++];
            enter[inner] = ++clock;
            stack.append({ inner, 0u });
        }
    }
    auto dominates = [&](unsigned a, unsigned b) {
        return enter[a] <= enter[b] && leave[b] <= leave[a];
    };
    Vector<Vector<unsigned, 2>> frontier(numberOfNewBlocks);
    for (unsigned b : order) {
        if (incoming[b].size() < 2)
            continue;
        for (auto& edge : incoming[b]) {
            for (unsigned runner = edge.from; runner != dominator[b]; runner = dominator[runner]) {
                if (!frontier[runner].contains(b))
                    frontier[runner].append(b);
                if (!runner)
                    break;
            }
        }
    }

    Vector<BlockValues> constantsAtStart(numberOfNewBlocks);
    UncheckedKeyHashMap<uint64_t, ValueID> constantAtStart;
    bool failed = false;
    struct Joins {
        Vector<unsigned, 4> blocks;
        Vector<ValueID, 4> parameters;
    };
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Joins>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> variableJoins;
    auto joinsOf = [&](uint32_t variable) -> Joins& {
        auto result = variableJoins.add(variable, nullptr);
        if (result.isNewEntry) {
            result.iterator->value = makeUniqueWithoutFastMallocCheck<Joins>();
            auto& joins = *result.iterator->value;
            Vector<unsigned, 8> worklist;
            for (uint32_t definition : { variable, slowCopyOf[variable] }) {
                if (definedIn[definition] != none)
                    worklist.append(definedIn[definition]);
            }
            while (!worklist.isEmpty()) {
                for (unsigned block : frontier[worklist.takeLast()]) {
                    if (!joins.blocks.contains(block)) {
                        joins.blocks.append(block);
                        worklist.append(block);
                    }
                }
            }
            joins.parameters.fill(ValueID(), joins.blocks.size());
        }
        return *result.iterator->value;
    };
    std::function<ValueID(uint32_t, unsigned, bool)> definitionReaching;
    definitionReaching = [&](uint32_t variable, unsigned b, bool atEnd) -> ValueID {
        auto& joins = joinsOf(variable);
        uint32_t nearest = none;
        ValueID definition;
        unsigned join = 0;
        bool isJoin = false;
        for (unsigned i = 0; i < joins.blocks.size(); ++i) {
            unsigned block = joins.blocks[i];
            if (dominates(block, b) && (nearest == none || enter[block] > enter[nearest])) {
                nearest = block;
                join = i;
                isJoin = true;
            }
        }
        for (uint32_t candidate : { variable, slowCopyOf[variable] }) {
            uint32_t block = definedIn[candidate];
            if (block == none || (block == b ? !atEnd : !dominates(block, b)))
                continue;
            if (nearest == none || enter[block] >= enter[nearest]) {
                nearest = block;
                definition = ValueID(candidate);
                isJoin = false;
            }
        }
        if (nearest == none) {
            failed = true;
            return ValueID(variable);
        }
        if (!isJoin)
            return definition;
        if (joins.parameters[join])
            return joins.parameters[join];
        auto& joined = function.at(ValueID(variable));
        if (joined.opcode == Opcode::NewObjectWithShape || joined.opcode == Opcode::Results || (joined.opcode == Opcode::CallDirect && !program.at(joined.function()).resultTypes.isEmpty())) {
            failed = true;
            return ValueID(variable);
        }
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.type = function.at(ValueID(slowCopyOf[variable])).type;
        parameter.rep = function.at(ValueID(slowCopyOf[variable])).rep;
        parameter.copyPredictionFrom(function.at(ValueID(slowCopyOf[variable])));
        parameter.position = function.at(ValueID(variable)).position;
        parameter.immediates = { nearest, static_cast<uint32_t>(function.blocks[nearest].parameters.size()) };
        ValueID created = add(WTF::move(parameter));
        function.blocks[nearest].parameters.append(created);
        joins.parameters[join] = created;
        for (auto& edge : incoming[nearest]) {
            if (function.blocks[edge.from].terminator.kind == TerminatorKind::Invoke)
                failed = true;
            ValueID argument = definitionReaching(variable, edge.from, true);
            function.blocks[edge.from].terminator.targets[edge.target].arguments.append(argument);
        }
        return created;
    };
    auto definitionAtBlockStart = [&](uint32_t variable, unsigned b) -> ValueID {
        if (!isSlowBlock.get(b) && !isReachedFromSlowBlocks.get(b))
            return ValueID(variable);
        if (!isConstant(function.at(ValueID(variable)).opcode))
            return definitionReaching(variable, b, false);
        auto result = constantAtStart.add((static_cast<uint64_t>(variable) << 32 | b) + 1, ValueID());
        if (result.isNewEntry) {
            ValueID constant = add(Value(function.at(ValueID(variable))));
            constantsAtStart[b].append(constant);
            constantAtStart.set((static_cast<uint64_t>(variable) << 32 | b) + 1, constant);
            return constant;
        }
        return result.iterator->value;
    };
    for (unsigned b = 0; b < numberOfNewBlocks && !failed; ++b) {
        auto fix = [&](ValueID& child) {
            if (!child || child.index() >= slowCopyOriginal.size())
                return;
            uint32_t variable = child.index() < slowCopyOf.size() && slowCopyOf[child.index()] != none ? child.index() : slowCopyOriginal[child.index()];
            if (variable == none || definedIn[child.index()] == b)
                return;
            child = definitionAtBlockStart(variable, b);
        };
        unsigned numberOfValues = function.blocks[b].values.size();
        for (unsigned index = 0; index < numberOfValues; ++index) {
            ValueID id = function.blocks[b].values[index];
            Value value(function.at(id));
            value.forEachChild(fix);
            function.at(id) = WTF::move(value);
        }
        unsigned numberOfTargets = function.blocks[b].terminator.targets.size();
        Vector<unsigned, 2> numberOfArguments;
        for (auto& target : function.blocks[b].terminator.targets)
            numberOfArguments.append(target.arguments.size());
        if (ValueID value = function.blocks[b].terminator.value) {
            fix(value);
            function.blocks[b].terminator.value = value;
        }
        for (unsigned t = 0; t < numberOfTargets; ++t) {
            for (unsigned i = 0; i < numberOfArguments[t]; ++i) {
                ValueID argument = function.blocks[b].terminator.targets[t].arguments[i];
                fix(argument);
                function.blocks[b].terminator.targets[t].arguments[i] = argument;
            }
        }
    }
    if (failed)
        return giveUp(SplitOutcome::CouldNotJoinPaths);
    for (unsigned b = 0; b < numberOfNewBlocks; ++b) {
        if (!constantsAtStart[b].isEmpty())
            function.blocks[b].values.insertVector(0, constantsAtStart[b]);
    }
    Vector<String> findings;
    verify(program, functionID, function, findings);
    if (!findings.isEmpty())
        return giveUp(SplitOutcome::CouldNotJoinPaths);
    return SplitOutcome::Split;
}

static bool splitSubtypeFastPaths(const Program& program, FunctionID functionID, Function& function)
{
    if (program.typedOnlySupertypes.isEmpty() || function.isGenericEntrypoint() || !initialStoresOf(function).empty())
        return false;
    auto mayBeGuarded = [&](ValueID receiver) {
        auto& value = function.at(receiver);
        LayoutID supertype = value.type ? program.at(value.type).singleLayout() : LayoutID();
        return supertype && supertype.index() < program.typedOnlySupertypes.size() && !!program.typedOnlySupertypes[supertype.index()];
    };
    bool mayHaveSharedReceiver = false;
    BitVector receivers;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::HasShapeOf) && receivers.set(value.child(0).index()))
                mayHaveSharedReceiver |= mayBeGuarded(value.child(0));
        }
    }
    if (!mayHaveSharedReceiver)
        return false;
    for (bool allocationsInvalidateGuards : { false, true }) {
        BitVector skippedAccesses;
        SplitOutcome outcome = SplitOutcome::SkippedAccesses;
        for (unsigned attempt = 0; attempt < 4 && outcome == SplitOutcome::SkippedAccesses; ++attempt)
            outcome = trySplitSubtypeFastPaths(program, functionID, function, skippedAccesses, allocationsInvalidateGuards);
        if (outcome == SplitOutcome::Split)
            return true;
        if (outcome != SplitOutcome::CouldNotJoinPaths || allocationsInvalidateGuards)
            break;
    }
    return false;
}

namespace {

struct ForwardedParameter {
    uint32_t parameter;
    uint32_t callee;
    uint32_t calleeParameter;
};

enum class ParameterUse : uint8_t {
    Unused,
    ForwardedOnly,
    Used,
};

}

template<typename IsTracked>
static Vector<ParameterUse, 8> classifyParameterUses(const Function& function, const IsTracked& isTracked, Vector<ForwardedParameter>* forwarded)
{
    auto& parameters = function.blocks[0].parameters;
    Vector<ParameterUse, 8> uses(FillWith { }, parameters.size(), ParameterUse::Used);
    constexpr uint32_t untracked = UINT32_MAX;
    uint32_t end = 0;
    unsigned remaining = 0;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        if (!isTracked(i))
            continue;
        uses[i] = ParameterUse::Unused;
        end = std::max<uint32_t>(end, parameters[i].index() + 1);
        ++remaining;
    }
    if (!remaining)
        return uses;
    Vector<uint32_t, 16> parameterIndex(FillWith { }, end, untracked);
    for (unsigned i = 0; i < parameters.size(); ++i) {
        if (uses[i] == ParameterUse::Unused)
            parameterIndex[parameters[i].index()] = i;
    }
    auto use = [&](ValueID value) {
        if (value.index() >= end || parameterIndex[value.index()] == untracked)
            return;
        auto& state = uses[parameterIndex[value.index()]];
        remaining -= state != ParameterUse::Used;
        state = ParameterUse::Used;
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (!forwarded || value.opcode != Opcode::CallDirect || value.isJumpToContinuation()) {
                value.forEachChild(use);
                continue;
            }
            for (ValueID child : value.children) {
                if (child)
                    use(child);
            }
            for (unsigned i = 0; i < value.arguments.size(); ++i) {
                ValueID argument = value.arguments[i];
                if (argument.index() >= end || parameterIndex[argument.index()] == untracked || uses[parameterIndex[argument.index()]] == ParameterUse::Used)
                    continue;
                uses[parameterIndex[argument.index()]] = ParameterUse::ForwardedOnly;
                forwarded->append({ parameterIndex[argument.index()], value.function().index(), i });
            }
        }
        if (block.terminator.value)
            use(block.terminator.value);
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                use(argument);
        }
        if (!remaining)
            break;
    }
    return uses;
}

static void widenUnusedParameterTypes(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned index) {
        auto& function = std::as_const(program).functions[index];
        if (function.blocks.isEmpty() || function.parameterTypes.isEmpty())
            return;
        unsigned first = function.captureTypes.size() + function.has(Format::FunctionHasThis);
        unsigned count = function.parameterTypes.size() - function.has(Format::FunctionHasRestParameter);
        auto uses = classifyParameterUses(function, [&](unsigned position) {
            if (position < first || position - first >= count)
                return false;
            TypeID type = function.parameterTypes[position - first];
            return type && type != program.types.anything && !function.calledOnlyParameters.get(position - first);
        }, nullptr);
        if (!uses.contains(ParameterUse::Unused))
            return;
        auto& changed = program.functions[index];
        for (unsigned position = first; position < uses.size(); ++position) {
            if (uses[position] != ParameterUse::Unused)
                continue;
            unsigned i = position - first;
            changed.parameterTypes[i] = program.types.anything;
            changed.annotatedParameters.clear(i);
            if (i < changed.assertedParameterTypes.size())
                changed.assertedParameterTypes[i] = TypeID();
            auto& parameter = changed.at(changed.blocks[0].parameters[position]);
            parameter.type = program.types.anything;
            parameter.rep = Rep::JSValue;
            parameter.setPrediction({ });
            parameter.setPredictedClosure({ });
        }
    });
}

static void findUnusedParameters(Program& program)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return;
    unsigned count = program.functions.size();
    Vector<BitVector> unused(count);
    Vector<Vector<ForwardedParameter>> forwarded(count);
    forEachIndexInParallel(count, [&](unsigned index) {
        auto& function = std::as_const(program).functions[index];
        if (function.blocks.isEmpty() || function.blocks[0].parameters.isEmpty() || function.isGenericEntrypoint() || function.isValueEntry || function.isSharedOutlinedBody || function.has(FunctionUsesArgumentBuffer) || function.has(Format::FunctionHasRestParameter))
            return;
        auto uses = classifyParameterUses(function, [](unsigned) { return true; }, &forwarded[index]);
        for (unsigned i = 0; i < uses.size(); ++i) {
            if (uses[i] != ParameterUse::Used)
                unused[index].set(i);
        }
        forwarded[index].removeAllMatching([&](auto& forwardedParameter) {
            return uses[forwardedParameter.parameter] == ParameterUse::Used;
        });
    });
    Vector<unsigned> forwardingFunctions;
    for (unsigned index = 0; index < count; ++index) {
        if (!forwarded[index].isEmpty())
            forwardingFunctions.append(index);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned index : forwardingFunctions) {
            for (auto& forwardedParameter : forwarded[index]) {
                if (!unused[index].get(forwardedParameter.parameter) || unused[forwardedParameter.callee].get(forwardedParameter.calleeParameter))
                    continue;
                unused[index].clear(forwardedParameter.parameter);
                changed = true;
            }
        }
    }
    size_t unusedCount = 0;
    for (unsigned index = 0; index < count; ++index) {
        if (unused[index].isEmpty())
            continue;
        unusedCount += unused[index].bitCount();
        program.functions.atKeepingValueIndex(index).unusedParameters = WTF::move(unused[index]);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", unusedCount, " function parameters (including captures) are unused: direct callers do not pass them");
}

Vector<String> optimize(Program& program)
{
    MonotonicTime start = MonotonicTime::now();
    PhaseTimes phaseTimes("optimize"_s);
    auto didFinish = [&](ASCIILiteral what) {
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            program.functions.verifyValueIndex();
        program.functions.rebuildInvalidatedValueIndex();
        dataLogLnIf(shouldPrintTimings(), "AOT passes: ", what, " done at ", (MonotonicTime::now() - start).seconds(), " s");
        phaseTimes.didFinish(what);
    };
    didFinish("- buildValueIndex"_s);
    widenUnusedParameterTypes(program);
    didFinish("- widenUnusedParameterTypes"_s);
    findConstructedRecords(program);
    prepare(program);
    didFinish("- prepare"_s);
    widenUnprofitableLiteralStrings(program, false);
    didFinish("- widenUnprofitableLiteralStrings"_s);
    refuteIteratorsWithMutableNext(program);
    didFinish("- refuteIteratorsWithMutableNext"_s);
    Vector<String> malformed;
    convertAllocationAssertions(program, malformed);
    didFinish("- convertAllocationAssertions"_s);
    findInitialStructures(program, malformed);
    if (program.initialStructures.size() > Value::maxInitialStructureOfFirstStore)
        malformed.append("the program has more initial structures than a first store can encode"_s);
    if (!malformed.isEmpty())
        return malformed;
    didFinish("preparing"_s);
    verifyExceptionPropagation(program);
    verifyVariablesStoredOnce(program);
    didFinish("variables stored once"_s);
    findFunctionPairsInEnvironments(program);
    didFinish("- findFunctionPairsInEnvironments"_s);
    findBuiltinsInVariables(program);
    didFinish("- findBuiltinsInVariables"_s);
    removeUnreadExports(program);
    didFinish("- removeUnreadExports"_s);
    findInternalModuleExports(program);
    didFinish("- findInternalModuleExports"_s);
    for (unsigned round = 0; round < 3 && findFunctionsInModuleVariables(program); ++round) { }
    didFinish("- findFunctionsInModuleVariables"_s);
    findFunctionsWithClosureProperties(program);
    didFinish("- findFunctionsWithClosureProperties"_s);
    expandDisposableStacks(program);
    didFinish("- expandDisposableStacks"_s);
    auto generators = splitCoroutines(program);
    didFinish("- splitCoroutines"_s);
    internNamesForGeneratorLoopFusion(program);
    if (program.usesOperation(Operations::SuperGet))
        program.intern(asBytes("prototype"_s.span8()));
    didFinish("splitting coroutines"_s);
    passClosuresToGenerators(program);
    didFinish("- passClosuresToGenerators"_s);
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        program.layouts[i].indexFields();
    });
    if (!Options::auditAOTTypedAssertions())
        addPlaceholderFields(program);
    for (auto& table : program.prototypeChainLacksPropertyCache)
        table.answers.clear();
    for (auto& table : program.storeMayTransitionCache)
        table.answers.clear();
    didFinish("indexing fields"_s);
    createGenericEntrypoints(program);
    createValueEntrypoints(program);
    createResultAdapters(program);
    didFinish("- createGenericEntrypoints"_s);
    findDeclaredSuperclasses(program);
    didFinish("general entries"_s);
    createBoundFunctions(program);
    if (!Options::auditAOTTypedAssertions())
        createGuardedPreludeEntries(program);
    program.numberOfDirectCalls.fill(0, program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        if (!program.functions.mayContain(i, directCalls))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.isGenericEntrypoint() || (function.isValueEntry && !function.isValueEntryClone))
            return;
        std::as_const(program).functions.forEachValue(i, directCalls, [&](ValueID id) {
            WTF::atomicExchangeAdd(&program.numberOfDirectCalls[function.at(id).function().index()], 1u, std::memory_order_relaxed);
        });
    });
    didFinish("- createBoundFunctions and direct call counts"_s);
    preallocateConstructorFields(program);
    assignAllocations(program);
    didFinish("assigning allocations"_s);
    createTypedOnlySupertypes(program);
    findRetagTargets(program);
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.kind == FunctionKind::Plain && !(function.declaredAs & (functionIsAsync | functionIsGenerator)) && (function.thisType || function.assertedThisType) && program.genericEntrypoints.contains(i))
            program.constructorFunctionsWithTypedThis.append(FunctionID(i));
    }
    program.types.freeze();
    didFinish("what looks at the whole program"_s);

    program.mayEncounterMasqueradesAsUndefined = std::ranges::any_of(program.strings, [](std::span<const uint8_t> string) {
        return equalSpans(string, asBytes("makeMasquerader"_s.span8())) || equalSpans(string, asBytes("IsHTMLDDA"_s.span8()));
    });
    didFinish("- mayEncounterMasqueradesAsUndefined"_s);
    findStaticallyHotFunctions(program);
    didFinish("- findStaticallyHotFunctions"_s);
    findStaticallyRareFunctions(program);
    didFinish("- findStaticallyRareFunctions"_s);
    if (!isTooSmallForColdCode(program)) {
        auto vigintiles = computeHeatVigintiles(program);
        applyCallGraphHeat(program, vigintiles);
        didFinish("- applyCallGraphHeat"_s);
        findFunctionsWithInliningDisabled(program, vigintiles);
        didFinish("- findFunctionsWithInliningDisabled"_s);
        findCallGraphColdFunctions(program, vigintiles);
        didFinish("- findCallGraphColdFunctions"_s);
    }
    markCallsInTailPosition(program);
    didFinish("- markCallsInTailPosition"_s);
    if (!Options::auditAOTTypedAssertions()) {
        outlineDestructuredParameterReads(program);
        splitOffWorkers(program);
        outlineHostObjectReads(program);
        outlineGetStoreCalls(program);
        shareCallsOfFunctionFields(program);
    }
    didFinish("- splitOffWorkers"_s);
    if (!Options::auditAOTTypedAssertions()) {
        Vector<unsigned> largestFirst(generators.size());
        for (unsigned k = 0; k < largestFirst.size(); ++k)
            largestFirst[k] = k;
        std::ranges::sort(largestFirst, std::greater { }, [&](unsigned k) {
            return generators[k].second.values.size();
        });
        forEachIndexInParallel(generators.size(), [&](unsigned k) {
            auto& [ramp, function] = generators[largestFirst[k]];
            BitVector liveObjects = objectsLiveAcrossSuspensions(function);
            if (liveObjects.isEmpty()) {
                function = Function();
                return;
            }
            FunctionID id = std::as_const(program).at(ramp).resumeFunction;
            TypeID resumeModeType = std::as_const(program).at(id).parameterTypes[0];
            for (auto& value : function.values) {
                if (value.opcode != Opcode::GetResumeMode)
                    continue;
                value.type = resumeModeType;
                value.rep = repOf(program.at(resumeModeType));
            }
            assignAllocations(program, function);
            markCastsOfUnsharedLiterals(program, function);
            cleanUp(program, function);
            for (unsigned round = 0; round < 8; ++round) {
                unsigned changed = resolvePreludeCalls(program, function, CleanUpTiming::Deferred);
                changed += resolveCalls(program, function, program.isStaticallyWarm(id), CleanUpTiming::Deferred);
                changed += inlineCalls(program, id, function, CleanUpTiming::Deferred);
                changed += simplifyCasts(program, function, CleanUpTiming::Deferred);
                unsigned removed = removeRedundantCatchBlocks(program, function);
                if (!changed && !removed)
                    break;
                cleanUp(program, function);
                cleanUpIfFoldable(program, function);
            }
            for (unsigned round = 0; round < 8 && sinkObjectAllocations(program, function, AllocationSinkingPolicy::AllowMaterialization); ++round)
                cleanUp(program, function);
            BitVector objectsStillAlive = objectsLiveAcrossSuspensions(function);
            objectsStillAlive.filter(liveObjects);
            if (objectsStillAlive == liveObjects) {
                function = Function();
                return;
            }
            compactBlocks(function);
        });
        generators.removeAllMatching([](auto& generator) {
            return generator.second.blocks.isEmpty();
        });
        splitOptimizedGenerators(program, WTF::move(generators));
    }
    didFinish("- splitOptimizedGenerator"_s);
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        if (!Options::auditAOTTypedAssertions() && program.functions.mayContain(i, objectLiterals))
            markCastsOfUnsharedLiterals(program, function);
        function.values.shrinkToFit();
        function.blocks.shrinkToFit();
    });
    indexLayoutSupertypes(program);
    prepareForInlining(program);
    didFinish("- prepareForInlining"_s);
    unsigned count = program.functions.size();
    Vector<std::unique_ptr<Function>> optimized(count);
    Vector<Vector<String>> findings(count);
    Vector<uint64_t> sizesAndIndices(count);
    forEachIndexInParallel(count, [&](unsigned index) {
        sizesAndIndices[index] = static_cast<uint64_t>(UINT32_MAX - std::as_const(program).functions[index].values.size()) << 32 | index;
    });
    didFinish("- sorting functions by size"_s);
    Vector<uint8_t> evaluatesModule(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        evaluatesModule[i] = !!asModuleEvaluation(program, FunctionID(i));
    });
    std::atomic<unsigned> remainingModuleEvaluationCount { 0 };
    std::atomic<unsigned> removedModuleEvaluationCount { 0 };
    std::atomic<unsigned> numberOfReceiverGuardsEliminated { 0 };
    std::atomic<unsigned> guardedPreludeEntryCallCount { 0 };
    didFinish("- module evaluation functions"_s);
    auto optimizeFunction = [&](unsigned i) {
        FunctionID id(i);
        Function function = std::as_const(program).functions[i];
        verify(program, id, function, findings[i]);
        if (!findings[i].isEmpty())
            return;
        cleanUp(program, function);
        bool didSplitFastPaths = false;
        bool hasTriedToSplitFastPaths = false;
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions(); ++round) {
            unsigned changed = resolvePreludeCalls(program, function, CleanUpTiming::Deferred);
            changed += resolveCalls(program, function, program.isStaticallyWarm(id), CleanUpTiming::Deferred);
            changed += readJoinedResultsOnEdges(program, function);
            changed += inlineCalls(program, id, function, CleanUpTiming::Deferred);
            unsigned fused = fuseGeneratorLoops(program, function);
            changed += simplifyCasts(program, function, CleanUpTiming::Deferred);
            unsigned removed = removeRedundantCatchBlocks(program, function);
            if (unsigned redundant = evaluatesModule[i] ? 0 : removeRedundantModuleEvaluations(function, evaluatesModule)) {
                removed += redundant;
                removedModuleEvaluationCount.fetch_add(redundant, std::memory_order_relaxed);
            }
            if (!changed && !fused && !removed) {
                if (stepGeneratorsInPlace(program, function)) {
                    cleanUp(program, function);
                    round = 0;
                    continue;
                }
                if (std::exchange(hasTriedToSplitFastPaths, true) || !splitSubtypeFastPaths(program, id, function))
                    break;
                didSplitFastPaths = true;
                cleanUp(program, function);
                round = 0;
                continue;
            }
            if (changed || removed || function.cleanUpMovedStores)
                cleanUp(program, function);
            cleanUpIfFoldable(program, function);
        }
        if (!hasTriedToSplitFastPaths && !Options::auditAOTTypedAssertions() && splitSubtypeFastPaths(program, id, function)) {
            didSplitFastPaths = true;
            cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions() && convertCountersToIntegers(program, function))
            cleanUpIfFoldable(program, function);
        if (!Options::auditAOTTypedAssertions()) {
            speculateOnReceivers(program, id, function);
            markCastsOfUnsharedLiterals(program, function);
        }
        resolveBuiltins(program, function);
        if (!Options::auditAOTTypedAssertions() && sinkToBooleanIntoPredecessors(program, function))
            cleanUp(program, function);
        if (unsigned eliminated = Options::auditAOTTypedAssertions() ? 0 : eliminateGuardsOnUnescapedReceivers(program, function)) {
            numberOfReceiverGuardsEliminated += eliminated;
            cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions())
            guardedPreludeEntryCallCount += callGuardedPreludeEntries(program, id, function, UnprovenReceiverPolicy::Wait);
        if (!Options::auditAOTTypedAssertions())
            fuseOwnPropertyDescriptorCopies(program, function);
        if (!Options::auditAOTTypedAssertions())
            splitDefinePropertiesOverLiterals(program, function);
        if (!Options::auditAOTTypedAssertions())
            hoistDefinePropertyIntoPredecessors(function);
        if (!Options::auditAOTTypedAssertions() && flattenIteratedObjectEntries(program, function)) {
            cleanUp(program, function);
            for (unsigned round = 0; round < 4 && simplifyCasts(program, function); ++round)
                cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions() && threadForInOwnKeys(program, function))
            cleanUp(program, function);
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions() && sinkObjectAllocations(program, function, AllocationSinkingPolicy::AllowMaterialization); ++round) {
            cleanUp(program, function);
            function.didRemoveAllocation = true;
        }
        if (function.didRemoveAllocation && removeDeadCountedLoops(program, function))
            cleanUp(program, function);
        if (!Options::auditAOTTypedAssertions())
            hoistReadsOutOfLoops(program, function);
        while (!Options::auditAOTTypedAssertions() && eliminateRedundantReads(program, function)) { }
        while (didSplitFastPaths && foldRepeatedShapeGuards(program, function)) {
            cleanUp(program, function);
            while (eliminateRedundantReads(program, function)) { }
        }
        if (!Options::auditAOTTypedAssertions())
            hoistAllocationsIntoPredecessors(program, function);
        sinkEmptyArrayLiterals(function);
        if (!Options::auditAOTTypedAssertions())
            sinkFunctionReferences(function);
        recordCellsLiveAcrossAwaits(program, function);
        compactBlocks(function);
        verify(program, id, function, findings[i], VerifierScope::EverythingButDefinitions);
        finalizeAllocationNotes(program, function);
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            for (auto& block : function.blocks) {
                for (ValueID value : block.values)
                    remainingModuleEvaluationCount.fetch_add(requestsModuleEvaluation(function.at(value), evaluatesModule), std::memory_order_relaxed);
            }
        }
        function.values.shrinkToFit();
        function.blocks.shrinkToFit();
        optimized[i] = makeUniqueWithoutFastMallocCheck<Function>(WTF::move(function));
    };
    static constexpr unsigned minLargeFunctionSize = 256;
    static constexpr size_t maxBatchSize = 64;
    const unsigned numberOfWorkers = WTF::numberOfProcessorCores();
    Lock queueLock;
    Condition queueCondition;
    Vector<uint64_t> largeFunctions;
    Vector<unsigned> smallFunctions;
    BitVector isQueued = functionsWithOtherEntries(program);
    auto enqueue = [&](unsigned index) {
        if (UINT32_MAX - static_cast<uint32_t>(sizesAndIndices[index] >> 32) < minLargeFunctionSize) {
            smallFunctions.append(index);
            return;
        }
        largeFunctions.append(sizesAndIndices[index]);
        std::ranges::push_heap(largeFunctions, std::greater { });
    };
    for (size_t i : isQueued)
        enqueue(i);
    unsigned numberOfBusyWorkers = 0;
    unsigned numberOfSleepingWorkers = 0;
    Vector<Functions::ValueIndexRows> valueIndexRows(numberOfWorkers);
    forEachIndexInParallel(numberOfWorkers, [&](unsigned worker) {
        Vector<unsigned, maxBatchSize> batch;
        Vector<unsigned, 128> callees;
        for (;;) {
            {
                Locker locker { queueLock };
                for (unsigned callee : callees) {
                    if (!isQueued.set(callee))
                        enqueue(callee);
                }
                if (!batch.isEmpty())
                    --numberOfBusyWorkers;
                callees.shrink(0);
                batch.shrink(0);
                for (;;) {
                    if (!largeFunctions.isEmpty()) {
                        std::ranges::pop_heap(largeFunctions, std::greater { });
                        batch.append(static_cast<unsigned>(largeFunctions.takeLast()));
                        break;
                    }
                    if (!smallFunctions.isEmpty()) {
                        size_t batchSize = std::clamp<size_t>(smallFunctions.size() / (4 * numberOfWorkers), 1, maxBatchSize);
                        batch.append(smallFunctions.span().last(batchSize));
                        smallFunctions.shrink(smallFunctions.size() - batchSize);
                        break;
                    }
                    if (!numberOfBusyWorkers) {
                        queueCondition.notifyAll();
                        return;
                    }
                    ++numberOfSleepingWorkers;
                    queueCondition.wait(queueLock);
                    --numberOfSleepingWorkers;
                }
                ++numberOfBusyWorkers;
                if (numberOfSleepingWorkers && !(largeFunctions.isEmpty() && smallFunctions.isEmpty()))
                    queueCondition.notifyAll();
            }
            for (unsigned next : batch) {
                optimizeFunction(next);
                if (!optimized[next])
                    continue;
                valueIndexRows[worker].append(next, *optimized[next]);
                forEachCalleeOf(program, *optimized[next], [&](unsigned callee) {
                    callees.append(callee);
                });
                for (FunctionID entry : optimized[next]->pendingGuardedPreludeEntries)
                    callees.append(entry.index());
            }
        }
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isQueued.get(i))
            verify(program, FunctionID(i), std::as_const(program).functions[i], findings[i]);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", remainingModuleEvaluationCount.load() + removedModuleEvaluationCount.load(), " requests to evaluate a module, ", removedModuleEvaluationCount.load(), " behind another for the same module");
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        unsigned callsWithoutGenericFallbackCount = 0;
        for (auto& function : optimized) {
            if (!function)
                continue;
            for (auto& value : function->values)
                callsWithoutGenericFallbackCount += value.receiverHasOriginalStructure;
        }
        dataLogLn("AOT: ", guardedPreludeEntryCallCount.load(), " array method calls share a guarded entry that holds the guard and the generic call");
        dataLogLn("AOT: receivers that are allocated in the function and have not escaped: ", numberOfReceiverGuardsEliminated.load(), " guards and method reads eliminated, ", callsWithoutGenericFallbackCount, " calls of built-in methods without a generic fallback");
    }
    didFinish("- the loop over the functions"_s);
    program.sizesForInlining.clear();
    program.inliningFacts.clear();
    for (unsigned i = 0; i < program.noReturnBlocks.size(); ++i) {
        if (program.noReturnBlocks[i].get(0))
            program.noReturnFunctions.set(i);
    }
    program.noReturnBlocks.clear();
    didFinish("what looks at one function"_s);

    Vector<String> result;
    verifyRegExpLiterals(program, result);
    didFinish("- verifyRegExpLiterals"_s);
    for (auto& perFunction : findings)
        result.appendVector(WTF::move(perFunction));
    if (!result.isEmpty())
        return result;
    didFinish("- the findings"_s);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!optimized[i]) {
            removeBody(program.functions[i]);
            return;
        }
        std::swap(program.functions[i], *optimized[i]);
    });
    program.functions.adoptValueIndexRows(valueIndexRows.span());
    destroyOnHelperThread(std::tuple { WTF::move(optimized), WTF::move(valueIndexRows) });
    for (unsigned i = 0; i < count; ++i) {
        if (!isQueued.get(i))
            program.uncalledFunctions.set(i);
    }
    didFinish("freeing the unoptimized functions"_s);
    widenUnprofitableLiteralStrings(program, true);
    didFinish("- widenUnprofitableLiteralStrings"_s);
    if (!Options::auditAOTTypedAssertions())
        skipConversionsToLayoutsWithoutFieldAccess(program);
    didFinish("- skipConversionsToLayoutsWithoutFieldAccess"_s);
    if (!Options::auditAOTTypedAssertions())
        allocateLiteralsWithComputedKeysAsRecords(program);
    didFinish("- allocateLiteralsWithComputedKeysAsRecords"_s);
    if (!Options::auditAOTTypedAssertions())
        predictClonesByCastsInOtherBlocks(program);
    didFinish("- predictClonesByCastsInOtherBlocks"_s);
    if (!Options::auditAOTTypedAssertions()) {
        moveConstantTablesIntoImage(program);
        didFinish("- moveConstantTablesIntoImage"_s);
        eliminateGuardsOnFreshResults(program);
        didFinish("- eliminateGuardsOnFreshResults"_s);
        eliminateGuardsOnUnescapedModuleVariables(program);
        didFinish("- eliminateGuardsOnUnescapedModuleVariables"_s);
        askFreshTablesInImage(program);
        didFinish("- askFreshTablesInImage"_s);
        std::atomic<unsigned> lateGuardedPreludeEntryCallCount { 0 };
        forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
            if (!std::as_const(program).functions[i].pendingGuardedPreludeEntries.isEmpty())
                lateGuardedPreludeEntryCallCount += callGuardedPreludeEntries(program, FunctionID(i), program.functions[i], UnprovenReceiverPolicy::Speculate);
        });
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", lateGuardedPreludeEntryCallCount.load(), " more array method calls use such an entry after whole-program analysis");
        didFinish("- callGuardedPreludeEntries"_s);
        compareHeaderNamesWithPassedConstants(program);
        didFinish("- compareHeaderNamesWithPassedConstants"_s);
        computeFunctionsReturningMapKeys(program);
        didFinish("- computeFunctionsReturningMapKeys"_s);
        removeUnusedResults(program);
        didFinish("- removeUnusedResults"_s);
        forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
            fuseConcatenations(program, FunctionID(i));
        });
        didFinish("- fuseConcatenations"_s);
        planStringAppends(program, result);
        if (!result.isEmpty())
            return result;
        didFinish("- planStringAppends"_s);
        outlineConcatenations(program);
        didFinish("- outlineConcatenations"_s);
    }
    if (!Options::auditAOTTypedAssertions()) {
        awaitDirectCallsWithoutPromises(program);
        specializeConstantFormatters(program);
        createJSONSerializers(program);
        outlineBuiltinCalls(program);
        outlineConstructs(program);
        outlineCasts(program);
        resolveCallsBySignature(program);
    }
    didFinish("- outlineBuiltinCalls, outlineConstructs"_s);
    if (!Options::auditAOTTypedAssertions()) {
        returnResultsInRegisters(program);
        specializeResultsByCaller(program);
        removeUnusedResults(program);
    }
    didFinish("- returnResultsInRegisters"_s);
    if (!Options::auditAOTTypedAssertions())
        shareModuleEvaluation(program);
    didFinish("- shareModuleEvaluation"_s);
    removeUncalledFunctions(program);
    didFinish("- removeUncalledFunctions"_s);
    findComparedBuiltinCells(program);
    didFinish("- findComparedBuiltinCells"_s);
    clearLazyClosureSlots(program);
    didFinish("- clearLazyClosureSlots"_s);
    std::atomic<bool> functionLengthsAreBytes { true };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (std::as_const(program).functions[i].length > std::numeric_limits<uint8_t>::max())
            functionLengthsAreBytes.store(false, std::memory_order_relaxed);
    });
    program.functionLengthsAreBytes = functionLengthsAreBytes.load();
    reportAllocationAssertions(program, result);
    if (!result.isEmpty())
        return result;
    didFinish("functions without calls"_s);
    markMandatoryTailCalls(program);
    didFinish("- markMandatoryTailCalls"_s);
    findCalledOnlyOperands(program);
    didFinish("- findCalledOnlyOperands"_s);
    verifyCalledOnlyParameters(program, result);
    if (!result.isEmpty() && !program.recordsAllocations)
        return result;
    findStackAllocatedClosures(program);
    findStackAllocatedEnvironments(program, result);
    if (!result.isEmpty())
        return result;
    reuseEnvironments(program);
    didFinish("environments in frames"_s);
    linkAllocations(program);
    didFinish("allocations"_s);
    computeEffectSummaries(program);
    didFinish("effects"_s);
    computeFunctionsReturningMapKeys(program);
    didFinish("map keys"_s);
    removeModuleEvaluations(program);
    didFinish("- removeModuleEvaluations"_s);
    finalizeInitialStructures(program);
    didFinish("- finalizeInitialStructures"_s);
    findIntegralNumbers(program);
    didFinish("- findIntegralNumbers"_s);
    findConstructorsReturningObjects(program);
    didFinish("- findConstructorsReturningObjects"_s);
    boxResultsOfIndirectOnlyFunctions(program);
    didFinish("- boxResultsOfIndirectOnlyFunctions"_s);
    assignLiteralStringIndices(program);
    didFinish("- assignLiteralStringIndices"_s);
    prepareTailCalls(program);
    clearCallerCheckedResultTypes(program);
    didFinish("- prepareTailCalls"_s);
    findUnusedParameters(program);
    didFinish("- findUnusedParameters"_s);
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& ramp = std::as_const(program).functions[i];
        if (ramp.resumeFunction && (ramp.declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsGenerator && std::as_const(program).at(ramp.resumeFunction).generatorTemplate == UINT32_MAX)
            program.at(ramp.resumeFunction).generatorTemplate = program.numberOfGeneratorTemplates++;
    }
    return result;
}

static Vector<uint8_t> computeHeatVigintiles(const Program& program)
{
    unsigned count = program.functions.size();
    struct Edge {
        uint32_t to;
        uint8_t kind;
        bool isCold;
        int8_t shown;
    };
    Vector<Vector<Edge, 2>> edges(count);
    UncheckedKeyHashMap<uint64_t, uint32_t> hubs;
    auto hubOf = [&](uint64_t key, bool createsIfMissing) -> uint32_t {
        if (auto found = hubs.find(key); found != hubs.end())
            return found->value;
        if (!createsIfMissing)
            return UINT32_MAX;
        uint32_t hub = edges.size();
        hubs.add(key, hub);
        edges.append(Vector<Edge, 2>());
        return hub;
    };
    auto nameKey = [](StringID name) {
        return 1ull << 60 | name.index();
    };
    auto variableKey = [](VariableID variable) {
        return 2ull << 60 | variable.index();
    };
    auto fieldKey = [](LayoutID layout, uint32_t field) {
        return 3ull << 60 | static_cast<uint64_t>(layout.index()) << 24 | (field & 0xffffff);
    };
    auto locationKey = [&](LayoutID layout, uint32_t field) {
        auto& described = program.at(layout);
        if (isObjectLayout(described.kind) && field < described.fields.size() && described.fields[field].name)
            return nameKey(described.fields[field].name);
        return fieldKey(layout, field);
    };
    auto setterKey = [](StringID name) {
        return 4ull << 60 | name.index();
    };
    auto classKey = [](LayoutID layout) {
        return 5ull << 60 | layout.index();
    };
    auto getterKey = [](StringID name) {
        return 6ull << 60 | name.index();
    };
    auto keyKind = [](uint64_t key) -> uint8_t {
        unsigned tag = key >> 60;
        return tag == 5 ? 3 : tag == 1 || tag == 4 || tag == 6 || tag > 8 ? 2 : 1;
    };
    constexpr uint32_t noLayout = UINT32_MAX;
    auto keyScopedToLayout = [](uint64_t key, uint32_t layout) {
        return ((key >> 60) + 8) << 60 | static_cast<uint64_t>(layout) << 28 | (key & 0xfffffff);
    };
    Vector<uint32_t> subtypeLayoutCount(FillWith { }, program.layouts.size(), 0u);
    forEachIndexInParallel(program.layouts.size(), [&](unsigned index) {
        uint32_t number = 1;
        for (auto* subtypes : { &program.layouts[index].subtypes, &program.layouts[index].structuralSubtypes }) {
            for (LayoutID subtype : *subtypes)
                number += 1 + program.at(subtype).specializations.size();
        }
        subtypeLayoutCount[index] = number;
    });
    auto subtypeLayoutsOf = [&](const auto& objectLayouts) -> std::optional<Vector<uint32_t>> {
        constexpr size_t maxLayouts = 64;
        Vector<uint32_t> found;
        for (LayoutID supertype : objectLayouts) {
            auto& described = program.at(supertype);
            if (described.acceptsUntypedObjects)
                return std::nullopt;
            if (found.size() + subtypeLayoutCount[supertype.index()] > 16 * maxLayouts)
                return std::nullopt;
            found.append(supertype.index());
            for (auto* subtypes : { &described.subtypes, &described.structuralSubtypes }) {
                for (LayoutID subtype : *subtypes) {
                    found.append(subtype.index());
                    for (LayoutID specialization : program.at(subtype).specializations)
                        found.append(specialization.index());
                }
            }
            if (found.size() > 16 * maxLayouts)
                return std::nullopt;
        }
        std::ranges::sort(found);
        found.shrink(std::unique(found.begin(), found.end()) - found.begin());
        if (found.size() > maxLayouts)
            return std::nullopt;
        return found;
    };
    auto receiverLayouts = [&](const Function& function, ValueID receiver) -> std::optional<Vector<uint32_t>> {
        if (!receiver)
            return std::nullopt;
        auto& type = program.at(function.at(receiver).type);
        if (type.anyObject || type.anyFunction)
            return std::nullopt;
        return subtypeLayoutsOf(type.objectLayouts);
    };
    auto enteredByValue = [&](FunctionID function) -> uint32_t {
        auto entry = program.genericEntrypoints.find(function.index());
        return entry != program.genericEntrypoints.end() ? entry->value.index() : function.index();
    };
    static constexpr OpcodeSet functionValues { Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::FunctionReference };
    auto functionOfClosureValue = [](const Function& function, ValueID id) -> FunctionID {
        while (id && function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        if (!id)
            return { };
        auto& value = function.at(id);
        if (functionValues.contains(value.opcode))
            return value.function();
        return { };
    };
    auto storedLocation = [&](const Function& function, const Value& value) -> std::pair<uint64_t, ValueID> {
        switch (value.opcode) {
        case Opcode::PutById:
            return { nameKey(value.string()), value.child(1) };
        case Opcode::PutField:
            return { locationKey(value.layout(), value.field()), value.child(1) };
        case Opcode::PutModuleVariable:
            return { variableKey(value.variable()), value.child(0) };
        case Opcode::CallBuiltin:
            if (languageOperationOf(program, value) == LanguageOperation::DefineField && value.arguments.size() == 3 && function.at(value.arguments[1]).opcode == Opcode::String)
                return { nameKey(function.at(value.arguments[1]).string()), value.arguments[2] };
            if (auto operation = languageOperationOf(program, value); (operation == LanguageOperation::DefineGetter || operation == LanguageOperation::DefineSetter) && value.arguments.size() >= 3 && function.at(value.arguments[1]).opcode == Opcode::String)
                return { operation == LanguageOperation::DefineGetter ? getterKey(function.at(value.arguments[1]).string()) : setterKey(function.at(value.arguments[1]).string()), value.arguments[2] };
            return { 0, ValueID() };
        default:
            return { 0, ValueID() };
        }
    };

    for (auto& layout : program.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                if (!method.function)
                    continue;
                uint32_t entered = enteredByValue(method.function);
                if (method.name)
                    edges[hubOf(method.kind == MethodKind::Setter ? setterKey(method.name) : method.kind == MethodKind::Getter ? getterKey(method.name) : nameKey(method.name), true)].append(Edge { entered, 2, false, 0 });
                if (layout.constructor)
                    edges[layout.constructor.index()].append(Edge { entered, 3, false, 0 });
            }
        }
    }
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].kind != LayoutKind::Instance)
            continue;
        for (LayoutID at = LayoutID(i); at; at = program.at(at).base) {
            for (auto& method : program.at(at).methods) {
                if (method.function)
                    edges[hubOf(classKey(LayoutID(i)), true)].append(Edge { enteredByValue(method.function), 3, false, 0 });
            }
        }
    }
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        bool isReferencedByTypes = program.layouts[i].kind != LayoutKind::Instance || program.layouts[i].hasFixedLayout;
        for (LayoutID at = LayoutID(i); at; at = isReferencedByTypes ? program.at(at).base : LayoutID()) {
            for (auto& method : program.at(at).methods) {
                if (!method.function || !method.name)
                    continue;
                uint64_t key = method.kind == MethodKind::Setter ? setterKey(method.name) : method.kind == MethodKind::Getter ? getterKey(method.name) : nameKey(method.name);
                edges[hubOf(keyScopedToLayout(key, isReferencedByTypes ? i : noLayout), true)].append(Edge { enteredByValue(method.function), 2, false, 0 });
            }
        }
    }
    auto layoutsStoredThrough = [&](const Function& function, const Value& value) -> std::optional<Vector<uint32_t>> {
        switch (value.opcode) {
        case Opcode::PutById:
            return receiverLayouts(function, value.child(0));
        case Opcode::PutField:
            return subtypeLayoutsOf(std::array { value.layout() });
        case Opcode::CallBuiltin:
            return receiverLayouts(function, value.arguments[0]);
        default:
            return std::nullopt;
        }
    };
    struct LocationStore {
        uint64_t key;
        uint32_t entered;
        Vector<uint32_t> layouts;
    };
    Vector<Vector<LocationStore>> storedIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayContain(i, functionValues))
            return;
        auto& function = program.functions[i];
        static constexpr OpcodeSet stores { Opcode::PutById, Opcode::PutField, Opcode::PutModuleVariable, Opcode::CallBuiltin };
        program.functions.forEachIndexedValue(i, stores, [&](ValueID id) {
            auto [key, stored] = storedLocation(function, function.at(id));
            if (!key)
                return;
            FunctionID created = functionOfClosureValue(function, stored);
            if (!created)
                return;
            Vector<uint32_t> layouts;
            if (keyKind(key) == 2) {
                if (auto found = layoutsStoredThrough(function, function.at(id)))
                    layouts = WTF::move(*found);
            }
            storedIn[i].append(LocationStore { key, enteredByValue(created), WTF::move(layouts) });
        });
    });
    for (auto& storedInFunction : storedIn) {
        for (auto& [key, entered, layouts] : storedInFunction) {
            edges[hubOf(key, true)].append(Edge { entered, keyKind(key), false, 0 });
            if (keyKind(key) != 2)
                continue;
            if (layouts.isEmpty()) {
                edges[hubOf(keyScopedToLayout(key, noLayout), true)].append(Edge { entered, 2, false, 0 });
                continue;
            }
            for (uint32_t layout : layouts)
                edges[hubOf(keyScopedToLayout(key, layout), true)].append(Edge { entered, 2, false, 0 });
        }
    }
    storedIn.clear();

    auto isIteratingName = [](auto name) {
        static constexpr ASCIILiteral names[] = { "forEach"_s, "map"_s, "filter"_s, "reduce"_s, "reduceRight"_s, "some"_s, "every"_s, "find"_s, "findIndex"_s, "findLast"_s, "findLastIndex"_s, "flatMap"_s, "sort"_s, "toSorted"_s, "replace"_s, "replaceAll"_s, "from"_s };
        for (auto candidate : names) {
            if (equalSpans(name, candidate.span8()))
                return true;
        }
        return false;
    };
    BitVector iteratingNames(program.strings.size());
    auto stringsNamingIteration = findIndicesInParallel(program.strings.size(), [&](unsigned i) {
        return isIteratingName(program.strings[i]);
    });
    for (unsigned i : stringsNamingIteration)
        iteratingNames.set(i);
    UncheckedKeyHashMap<uint64_t, int> parameterCallWeight;
    auto parameterKey = [](uint32_t function, uint32_t index) {
        return (static_cast<uint64_t>(function) + 1) << 20 | (index & 0xfffff);
    };
    auto recordParameterCall = [&](uint32_t function, uint32_t index, int weight) {
        auto result = parameterCallWeight.add(parameterKey(function, index), weight);
        if (result.isNewEntry || result.iterator->value >= weight)
            return result.isNewEntry;
        result.iterator->value = weight;
        return true;
    };
    struct ForwardedArguments {
        uint32_t function;
        uint32_t index;
        uint32_t callee;
        uint32_t argument;
        int weight;
    };
    Vector<ForwardedArguments> forwardedParameters;
    struct Handed {
        uint32_t function;
        uint32_t edge;
        uint32_t callee;
        uint32_t argument;
        int weight;
    };
    Vector<Handed> passedFunctions;
    Vector<float> massOf(FillWith { }, count, 0.0f);
    Vector<uint32_t> sizes(FillWith { }, count, 0u);
    Vector<char> coroutineOfResumeFunction(FillWith { }, count, '-');
    auto ramps = findIndicesInParallel(count, [&](unsigned i) {
        return !!program.functions[i].resumeFunction;
    });
    for (unsigned i : ramps) {
        auto& ramp = program.functions[i];
        bool isAsync = ramp.declaredAs & functionIsAsync;
        bool isGenerator = ramp.declaredAs & functionIsGenerator;
        coroutineOfResumeFunction[ramp.resumeFunction.index()] = isAsync && isGenerator ? 'x' : isAsync ? 'a' : isGenerator ? 'g' : '-';
    }
    struct ParameterCall {
        uint32_t index;
        int weight;
    };
    struct FoundInFunction {
        Vector<ParameterCall> parameterCalls;
        Vector<ForwardedArguments> forwardedParameters;
        Vector<Handed> passedFunctions;
    };
    Vector<FoundInFunction> foundIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        unsigned numberOfBlocks = function.blocks.size();
        if (!numberOfBlocks)
            return;
        Vector<unsigned> resumedAt;
        if (coroutineOfResumeFunction[i] == 'g' || coroutineOfResumeFunction[i] == 'x')
            resumedAt = resumeBlocks(function);
        auto isSuspendedIn = [&](unsigned b) {
            return !resumedAt.isEmpty() && resumedAt[b] != UINT_MAX;
        };
        BitVector isResumedFromHere(numberOfBlocks);
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (auto& target : function.blocks[b].terminator.targets)
                predecessors[target.block.index()].append(b);
            if (isSuspendedIn(b)) {
                predecessors[resumedAt[b]].append(b);
                isResumedFromHere.set(resumedAt[b]);
            }
        }
        Vector<unsigned, 8> entrySuccessors;
        for (bool isResumed : { false, true }) {
            auto& targets = function.blocks[0].terminator.targets;
            for (unsigned way = 0; way < targets.size(); ++way) {
                if (isResumedFromHere.get(targets[way].block.index()) == isResumed)
                    entrySuccessors.append(way);
            }
        }
        Vector<Vector<unsigned, 1>> latchesOf(numberOfBlocks);
        Vector<unsigned> postOrder;
        {
            Vector<uint8_t> state(FillWith { }, numberOfBlocks, static_cast<uint8_t>(0));
            Vector<std::pair<unsigned, unsigned>, 32> stack;
            stack.append({ 0u, 0u });
            state[0] = 1;
            while (!stack.isEmpty()) {
                unsigned block = stack.last().first;
                auto& targets = function.blocks[block].terminator.targets;
                if (stack.last().second == targets.size() + isSuspendedIn(block)) {
                    state[block] = 2;
                    postOrder.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned way = stack.last().second++;
                unsigned to = way < targets.size() ? targets[block ? way : entrySuccessors[way]].block.index() : resumedAt[block];
                if (!state[to]) {
                    state[to] = 1;
                    stack.append({ to, 0u });
                } else if (state[to] == 1)
                    latchesOf[to].append(block);
            }
        }
        Vector<uint8_t> loops(FillWith { }, numberOfBlocks, static_cast<uint8_t>(0));
        Vector<std::pair<unsigned, unsigned>> hints;
        for (unsigned b = 0; b < numberOfBlocks && !resumedAt.isEmpty(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                if (function.at(id).opcode == Opcode::LoopHint)
                    hints.append({ id.index(), b });
            }
        }
        std::ranges::sort(hints);
        BitVector containsEarlierHint(numberOfBlocks);
        for (auto [number, hinted] : hints) {
            if (containsEarlierHint.get(hinted))
                continue;
            BitVector isReached(numberOfBlocks);
            BitVector body(numberOfBlocks);
            Vector<unsigned, 16> worklist { hinted };
            auto reach = [&](unsigned to) {
                if (!containsEarlierHint.get(to) && !isReached.set(to))
                    worklist.append(to);
            };
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                for (auto& target : function.blocks[block].terminator.targets)
                    reach(target.block.index());
                if (isSuspendedIn(block))
                    reach(resumedAt[block]);
            }
            containsEarlierHint.set(hinted);
            if (!isReached.get(hinted))
                continue;
            body.set(hinted);
            worklist.append(hinted);
            while (!worklist.isEmpty()) {
                for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                    if (isReached.get(predecessor) && !body.set(predecessor))
                        worklist.append(predecessor);
                }
            }
            for (unsigned b : body)
                loops[b] = std::min<unsigned>(3, loops[b] + 1);
        }
        for (unsigned header = 0; header < numberOfBlocks && resumedAt.isEmpty(); ++header) {
            if (latchesOf[header].isEmpty())
                continue;
            BitVector body(numberOfBlocks);
            body.set(header);
            Vector<unsigned, 16> worklist;
            for (unsigned latch : latchesOf[header]) {
                if (!body.set(latch))
                    worklist.append(latch);
            }
            while (!worklist.isEmpty()) {
                for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                    if (!body.set(predecessor))
                        worklist.append(predecessor);
                }
            }
            for (unsigned b : body)
                loops[b] = std::min<unsigned>(3, loops[b] + 1);
        }
        Vector<uint8_t> isCold(FillWith { }, numberOfBlocks, static_cast<uint8_t>(0));
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& terminator = function.blocks[b].terminator;
            if (terminator.kind == TerminatorKind::Throw || terminator.kind == TerminatorKind::Unreachable)
                isCold[b] = 1;
            if (terminator.kind == TerminatorKind::Invoke && terminator.targets.size() > 1)
                isCold[terminator.targets[1].block.index()] = 1;
        }
        for (unsigned round = 0; round < 64; ++round) {
            bool changed = false;
            for (unsigned b = numberOfBlocks; b--;) {
                auto& terminator = function.blocks[b].terminator;
                unsigned ways = terminator.kind == TerminatorKind::Invoke ? std::min<unsigned>(1, terminator.targets.size()) : terminator.targets.size();
                if (isCold[b] || !ways)
                    continue;
                bool allAreCold = true;
                for (unsigned k = 0; k < ways; ++k)
                    allAreCold &= !!isCold[terminator.targets[k].block.index()];
                if (allAreCold) {
                    isCold[b] = 1;
                    changed = true;
                }
            }
            for (unsigned b = 1; b < numberOfBlocks; ++b) {
                if (isCold[b] || predecessors[b].isEmpty())
                    continue;
                bool allAreCold = true;
                for (unsigned predecessor : predecessors[b])
                    allAreCold &= !!isCold[predecessor];
                if (allAreCold) {
                    isCold[b] = 1;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }

        bool hasOwnEntryState = false;
        for (auto& target : function.blocks[0].terminator.targets)
            hasOwnEntryState |= !isResumedFromHere.get(target.block.index()) && !isCold[target.block.index()];
        hasOwnEntryState &= !resumedAt.isEmpty();
        Vector<float> frequency(FillWith { }, numberOfBlocks, 0.0f);
        frequency[0] = 1;
        for (unsigned k = postOrder.size(); k--;) {
            unsigned b = postOrder[k];
            if (!latchesOf[b].isEmpty() && resumedAt.isEmpty())
                frequency[b] *= 8;
            auto& terminator = function.blocks[b].terminator;
            if (isSuspendedIn(b) && !latchesOf[resumedAt[b]].contains(b))
                frequency[resumedAt[b]] += frequency[b];
            float all = 0;
            auto shareOf = [&](unsigned way) -> float {
                unsigned to = terminator.targets[way].block.index();
                if (latchesOf[to].contains(b))
                    return 0;
                if (!b && hasOwnEntryState && isResumedFromHere.get(to))
                    return 0;
                if (terminator.kind == TerminatorKind::Invoke && way)
                    return 1.0f / 1024;
                return isCold[to] ? 1.0f / 32 : 1.0f;
            };
            for (unsigned way = 0; way < terminator.targets.size(); ++way)
                all += shareOf(way);
            if (!all)
                continue;
            if (!b && terminator.kind == TerminatorKind::Switch && program.isResumeFunction(function))
                all = hasOwnEntryState ? 1 : 2;
            for (unsigned way = 0; way < terminator.targets.size(); ++way) {
                unsigned to = terminator.targets[way].block.index();
                float left = loops[to] < loops[b] && resumedAt.isEmpty() ? static_cast<float>(1u << (3 * (loops[b] - loops[to]))) : 1.0f;
                frequency[to] += frequency[b] * shareOf(way) / all / left;
            }
        }
        for (unsigned b = 0; b < numberOfBlocks && !resumedAt.isEmpty(); ++b)
            frequency[b] *= static_cast<float>(1u << (3 * loops[b]));
        auto weightOf = [&](unsigned b) -> int {
            if (!(frequency[b] > 0))
                return -30;
            return std::clamp(static_cast<int>(std::lround(std::log2(frequency[b]))), -30, 12);
        };
        auto add = [&](uint32_t to, unsigned b, uint8_t kind, unsigned bonusShown = 0) {
            bool cold = isCold[b];
            int shown = std::min(15, weightOf(b) + 3 * static_cast<int>(bonusShown));
            edges[i].append(Edge { to, kind, cold, static_cast<int8_t>(shown) });
        };
        auto addNamedEdge = [&](uint64_t key, const std::optional<Vector<uint32_t>>& layouts, unsigned b) {
            if (!layouts) {
                if (uint32_t hub = hubOf(key, false); hub != UINT32_MAX)
                    add(hub, b, 2);
                return;
            }
            if (uint32_t hub = hubOf(keyScopedToLayout(key, noLayout), false); hub != UINT32_MAX)
                add(hub, b, 2);
            for (uint32_t layout : *layouts) {
                if (uint32_t hub = hubOf(keyScopedToLayout(key, layout), false); hub != UINT32_MAX)
                    add(hub, b, 2);
            }
        };
        auto parameterIndexOf = [&](ValueID id) -> std::optional<uint32_t> {
            while (id && function.at(id).opcode == Opcode::Cast)
                id = function.at(id).child(0);
            if (!id || function.at(id).opcode != Opcode::Parameter || function.at(id).parameterBlock().index())
                return std::nullopt;
            return function.at(id).parameterIndex();
        };
        for (FunctionID other : { function.resumeFunction, function.startFunction, function.rampForClosures, function.worker }) {
            if (other)
                add(other.index(), 0, 0);
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = function.blocks[b];
            sizes[i] += block.values.size() + 1;
            massOf[i] += frequency[b] * static_cast<float>(block.values.size() + 1);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                uint64_t read = 0;
                switch (value.opcode) {
                case Opcode::CallDirect:
                    add(value.function().index(), b, 0);
                    if (program.moduleEvaluatorIfNeeded && program.moduleEvaluationIndexByFunction.contains(value.function().index()))
                        add(program.moduleEvaluatorIfNeeded.index(), b, 0);
                    break;
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                case Opcode::FunctionReference:
                    add(enteredByValue(value.function()), b, 1);
                    break;
                case Opcode::GetById:
                case Opcode::CallMethod: {
                    bool mayLeadToGetter = hubOf(getterKey(value.string()), false) != UINT32_MAX;
                    bool mayLeadToFunction = (value.opcode == Opcode::CallMethod || program.at(value.type).anyFunction) && hubOf(nameKey(value.string()), false) != UINT32_MAX;
                    if (!mayLeadToGetter && !mayLeadToFunction)
                        break;
                    auto layouts = receiverLayouts(function, value.child(0));
                    if (mayLeadToGetter)
                        addNamedEdge(getterKey(value.string()), layouts, b);
                    if (mayLeadToFunction)
                        addNamedEdge(nameKey(value.string()), layouts, b);
                    break;
                }
                case Opcode::GetField:
                    if (program.at(value.type).anyFunction) {
                        read = locationKey(value.layout(), value.field());
                        if (keyKind(read) == 2) {
                            if (hubOf(read, false) != UINT32_MAX)
                                addNamedEdge(read, subtypeLayoutsOf(std::array { value.layout() }), b);
                            read = 0;
                        }
                    }
                    break;
                case Opcode::GetModuleVariable:
                    if (program.at(value.type).anyFunction)
                        read = variableKey(value.variable());
                    break;
                case Opcode::PutById:
                    if (hubOf(setterKey(value.string()), false) != UINT32_MAX)
                        addNamedEdge(setterKey(value.string()), receiverLayouts(function, value.child(0)), b);
                    break;
                case Opcode::NewObjectWithShape:
                    read = classKey(value.layout());
                    break;
                default:
                    break;
                }
                if (read) {
                    if (uint32_t hub = hubOf(read, false); hub != UINT32_MAX)
                        add(hub, b, keyKind(read));
                }
                bool isCall = value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect || value.opcode == Opcode::Construct
                    || (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).callsUserCode());
                if (!isCall)
                    continue;
                if (value.opcode == Opcode::Call && !isCold[b]) {
                    if (auto index = parameterIndexOf(value.child(0)))
                        foundIn[i].parameterCalls.append(ParameterCall { *index, weightOf(b) });
                }
                bool iterates = (value.opcode == Opcode::CallMethod && iteratingNames.get(value.string().index()))
                    || (value.opcode == Opcode::CallBuiltin && isIteratingName(Builtins::singleton().at(value.builtin()).name.span8()));
                bool isKnown = value.opcode == Opcode::CallDirect && !program.at(value.function()).blocks.isEmpty() && program.at(value.function()).blocks[0].parameters.size() == value.arguments.size();
                for (unsigned argument = 0; argument < value.arguments.size(); ++argument) {
                    if (FunctionID handed = functionOfClosureValue(function, value.arguments[argument])) {
                        add(enteredByValue(handed), b, 1, iterates);
                        if (isKnown)
                            foundIn[i].passedFunctions.append(Handed { i, static_cast<uint32_t>(edges[i].size() - 1), value.function().index(), argument, weightOf(b) });
                    } else if (isKnown && !isCold[b]) {
                        if (auto index = parameterIndexOf(value.arguments[argument]))
                            foundIn[i].forwardedParameters.append(ForwardedArguments { i, *index, value.function().index(), argument, weightOf(b) });
                    }
                }
            }
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto& call : foundIn[i].parameterCalls)
            recordParameterCall(i, call.index, call.weight);
        forwardedParameters.appendVector(foundIn[i].forwardedParameters);
        passedFunctions.appendVector(foundIn[i].passedFunctions);
    }
    destroyOnHelperThread(std::exchange(foundIn, { }));
    for (auto& evaluation : program.moduleEvaluations) {
        for (FunctionID callee : evaluation.callees)
            edges[evaluation.function.index()].append(Edge { callee.index(), 0, false, 0 });
    }

    for (unsigned round = 0; round < 6; ++round) {
        bool changed = false;
        for (auto& passed : forwardedParameters) {
            auto further = parameterCallWeight.find(parameterKey(passed.callee, passed.argument));
            if (further == parameterCallWeight.end())
                continue;
            int weight = std::clamp(passed.weight + further->value, -30, 15);
            changed |= recordParameterCall(passed.function, passed.index, weight);
        }
        if (!changed)
            break;
    }
    for (auto& handed : passedFunctions) {
        auto called = parameterCallWeight.find(parameterKey(handed.callee, handed.argument));
        if (called != parameterCallWeight.end())
            edges[handed.function][handed.edge].shown = static_cast<int8_t>(std::clamp(handed.weight + called->value, -30, 15));
    }

    unsigned nodes = edges.size();
    Vector<int8_t> hubShare(FillWith { }, nodes, static_cast<int8_t>(0));
    for (uint32_t hub = count; hub < nodes; ++hub) {
        for (size_t candidates = edges[hub].size(); candidates > 1; candidates >>= 1)
            ++hubShare[hub];
    }
    Vector<uint32_t> roots;
    for (FunctionID entry : program.entries)
        roots.append(entry.index());
    for (auto& module : program.builtinModules) {
        if (module.second)
            roots.append(module.second.index());
    }
    for (auto& builtin : program.builtinFunctions) {
        if (!builtin.function)
            continue;
        roots.append(builtin.function.index());
        roots.append(enteredByValue(builtin.function));
    }
    for (FunctionID prelude : program.preludeFunctions) {
        if (prelude)
            roots.append(prelude.index());
    }
    for (FunctionID evaluator : { program.moduleEvaluator, program.moduleEvaluatorIfNeeded, program.externalModuleEvaluator }) {
        if (evaluator)
            roots.append(evaluator.index());
    }
    Vector<uint8_t> isReachedNode(FillWith { }, nodes, static_cast<uint8_t>(0));
    Vector<uint32_t> frontier;
    for (uint32_t root : roots) {
        if (!std::exchange(isReachedNode[root], 1))
            frontier.append(root);
    }
    while (!frontier.isEmpty()) {
        frontier = collectInParallel<uint32_t>(frontier.size(), [&](unsigned k, Vector<uint32_t>& reached) {
            for (auto& edge : edges[frontier[k]]) {
                if (!WTF::atomicLoad(&isReachedNode[edge.to], std::memory_order_relaxed) && !WTF::atomicExchange(&isReachedNode[edge.to], static_cast<uint8_t>(1), std::memory_order_relaxed))
                    reached.append(edge.to);
            }
        });
    }
    BitVector isReached(nodes);
    for (uint32_t node = 0; node < nodes; ++node) {
        if (isReachedNode[node])
            isReached.quickSet(node);
    }
    uint64_t totalSize = 0;
    for (unsigned i = 0; i < count; ++i)
        totalSize += isReached.get(i) ? sizes[i] : 0;
    struct Term {
        uint32_t from;
        uint32_t to;
        double factor;
    };
    constexpr uint32_t nodesPerSlice = 1024;
    unsigned slices = (nodes + nodesPerSlice - 1) / nodesPerSlice;
    auto forEachTermFromSlice = [&](unsigned slice, auto&& functor) {
        uint32_t sliceEnd = std::min<uint32_t>(nodes, (slice + 1) * nodesPerSlice);
        for (uint32_t from = slice * nodesPerSlice; from < sliceEnd; ++from) {
            for (auto& edge : edges[from]) {
                if (!edge.isCold && edge.kind <= 2)
                    functor(from, edge);
            }
        }
    };
    constexpr unsigned targetRangeCount = 64;
    uint32_t nodesPerRange = std::max(1u, (nodes + targetRangeCount - 1) / targetRangeCount);
    Vector<std::array<uint32_t, targetRangeCount>> offsetBySliceAndRange(slices);
    forEachIndexInParallel(slices, [&](unsigned slice) {
        std::array<uint32_t, targetRangeCount> numbers { };
        forEachTermFromSlice(slice, [&](uint32_t, const Edge& edge) {
            ++numbers[edge.to / nodesPerRange];
        });
        offsetBySliceAndRange[slice] = numbers;
    });
    std::array<uint32_t, targetRangeCount + 1> rangeFirstTerm;
    uint32_t numberOfTerms = 0;
    for (unsigned range = 0; range < targetRangeCount; ++range) {
        rangeFirstTerm[range] = numberOfTerms;
        for (unsigned slice = 0; slice < slices; ++slice)
            numberOfTerms += std::exchange(offsetBySliceAndRange[slice][range], numberOfTerms);
    }
    rangeFirstTerm[targetRangeCount] = numberOfTerms;
    auto termCapacityByRange = MallocSpan<Term>::malloc(numberOfTerms * sizeof(Term));
    auto termCapacityByTarget = MallocSpan<Term>::malloc(numberOfTerms * sizeof(Term));
    std::span<Term> termsByRange = termCapacityByRange.mutableSpan();
    std::span<Term> termsByTarget = termCapacityByTarget.mutableSpan();
    forEachIndexInParallel(slices, [&](unsigned slice) {
        auto locations = offsetBySliceAndRange[slice];
        forEachTermFromSlice(slice, [&](uint32_t from, const Edge& edge) {
            double gain = edge.shown;
            if (gain < 0)
                gain *= 0.5;
            if (edge.kind == 2 && from >= count)
                gain -= hubShare[from];
            termsByRange[locations[edge.to / nodesPerRange]++] = Term { from, edge.to, std::exp2(gain) };
        });
    });
    Vector<uint32_t> firstTermTo(FillWith { }, nodes + 1, 0u);
    firstTermTo[nodes] = numberOfTerms;
    Vector<uint32_t> nextTermOffset(FillWith { }, nodes, 0u);
    forEachIndexInParallel(targetRangeCount, [&](unsigned range) {
        auto rangeTerms = termsByRange.subspan(rangeFirstTerm[range], rangeFirstTerm[range + 1] - rangeFirstTerm[range]);
        for (auto& term : rangeTerms)
            ++firstTermTo[term.to];
        uint32_t next = rangeFirstTerm[range];
        uint32_t rangeEnd = static_cast<uint32_t>(std::min<uint64_t>(nodes, static_cast<uint64_t>(range + 1) * nodesPerRange));
        for (uint32_t node = std::min(nodes, range * nodesPerRange); node < rangeEnd; ++node) {
            nextTermOffset[node] = next;
            next += std::exchange(firstTermTo[node], next);
        }
        for (auto& term : rangeTerms)
            termsByTarget[nextTermOffset[term.to]++] = term;
    });
    termCapacityByRange = { };
    Vector<double> atRoots(FillWith { }, nodes, 0.0);
    for (uint32_t root : roots)
        atRoots[root] = 1;
    constexpr std::array<double, 2> scales { 1.0, 0x1p-4 };
    Vector<std::array<double, 2>> flows(nodes, [&](size_t node) {
        return std::array<double, 2> { atRoots[node], atRoots[node] };
    });
    Vector<std::array<double, 2>> next(FillWith { }, nodes, std::array<double, 2> { 0.0, 0.0 });
    constexpr unsigned rounds = 80;
    static_assert(!(rounds % 2));
    std::atomic<uint32_t> nextSlice { 0 };
    std::array<std::atomic<uint32_t>, rounds> completedSlices { };
    forEachIndexInParallel(std::min<unsigned>(WTF::numberOfProcessorCores(), slices), [&](unsigned) {
        for (;;) {
            uint32_t taken = nextSlice.fetch_add(1, std::memory_order_relaxed);
            if (taken >= rounds * slices)
                return;
            unsigned round = taken / slices;
            unsigned slice = taken % slices;
            while (round && completedSlices[round - 1].load(std::memory_order_acquire) != slices)
                Thread::yield();
            auto& from = round % 2 ? next : flows;
            auto& to = round % 2 ? flows : next;
            uint32_t sliceEnd = std::min<uint32_t>(nodes, (slice + 1) * nodesPerSlice);
            for (uint32_t node = slice * nodesPerSlice; node < sliceEnd; ++node) {
                double sum = 0.0;
                double sumByNearness = 0.0;
                for (uint32_t k = firstTermTo[node]; k < firstTermTo[node + 1]; ++k) {
                    auto& flow = from[termsByTarget[k].from];
                    sum += flow[0] * termsByTarget[k].factor;
                    sumByNearness += flow[1] * termsByTarget[k].factor;
                }
                to[node] = { std::min(atRoots[node] + scales[0] * sum, 1e250), std::min(atRoots[node] + scales[1] * sumByNearness, 1e250) };
            }
            completedSlices[round].fetch_add(1, std::memory_order_release);
        }
    });
    std::array<Vector<uint8_t>, 2> vigintiles;
    for (unsigned which = 0; which < 2; ++which) {
        Vector<std::pair<double, uint32_t>> order(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            double flow = flows[i][which];
            if (sizes[i] < 256) {
                double shrinkFactor = 256.0 / std::max(1u, sizes[i]);
                flow *= shrinkFactor * shrinkFactor;
            }
            double mass = massOf[i] < 1e30f ? massOf[i] : 1e30;
            order[i] = std::pair { flow * mass / std::max(1u, sizes[i]), i };
        });
        sortOnPool(order.mutableSpan(), std::less { });
        Vector<uint8_t> result(FillWith { }, count, static_cast<uint8_t>(0));
        uint64_t before = 0;
        for (auto [key, function] : order) {
            result[function] = static_cast<uint8_t>(totalSize ? std::min<uint64_t>(19, before * 20 / totalSize) : 0);
            before += isReached.get(function) ? sizes[function] : 0;
        }
        vigintiles[which] = WTF::move(result);
    }
    destroyOnHelperThread(std::tuple { WTF::move(edges), WTF::move(hubs), WTF::move(parameterCallWeight) });
    static constexpr uint8_t minHeatNearRoots = 18;
    return Vector<uint8_t>(count, [&](size_t i) {
        return vigintiles[1][i] >= minHeatNearRoots ? vigintileNearRoots : vigintiles[0][i];
    });
}

Vector<String> describeForMap(const Program& program)
{
    unsigned count = program.functions.size();
    Vector<FunctionID> entered;
    entered.fill(FunctionID(), count);
    for (auto& entry : program.genericEntrypoints)
        entered[entry.value.index()] = FunctionID(entry.key);
    Vector<LayoutID> classes;
    classes.fill(LayoutID(), count);
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Instance)
            continue;
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                classes[method.function.index()] = LayoutID(i);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (FunctionID resume = program.functions[i].resumeFunction; resume && !program.functions[i].isRampForClosures)
            classes[resume.index()] = classes[i];
        if (FunctionID start = program.functions[i].startFunction)
            classes[start.index()] = classes[i];
    }
    auto text = [&](StringID id) {
        if (!id)
            return String();
        String result = String::fromUTF8(program.string(id));
        for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
            result = makeStringByReplacingAll(result, character, written);
        return result;
    };
    auto entrypointOf = [&](FunctionID id) -> ASCIILiteral {
        auto& function = program.at(id);
        if (program.isEntry(id))
            return "entry"_s;
        if (function.kind == FunctionKind::Module)
            return "module"_s;
        if (function.resumeFunction)
            return "ramp"_s;
        if (program.isResumeFunction(function))
            return "resume"_s;
        return function.isTyped() ? "typed"_s : "untyped"_s;
    };
    Vector<String> result;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        FunctionID described = entered[i] ? entered[i] : FunctionID(i);
        LayoutID ownClass = classes[described.index()];
        String className = ownClass ? text(program.at(ownClass).name) : String();
        String path = function.module ? text(program.modules[function.module.index()].path) : String();
        result.append(makeString(className, className.isEmpty() ? ""_s : "."_s, text(function.name), '\t', entered[i] ? "generic-"_s : ""_s, entrypointOf(described), '\t', i, '\t', path, '\t', function.position, '\t', program.isStaticallyHot(FunctionID(i)) ? 'h' : program.isStaticallyRare(FunctionID(i)) ? 'r' : 'c'));
    }
    return result;
}

bool dumpAtStage(std::span<const uint8_t> bytes, const String& stage, PrintStream& out)
{
    if (stage == "builtins"_s) {
        static constexpr ASCIILiteral kinds[] = { "Function"_s, "Constructor"_s, "StaticFunction"_s, "Constant"_s, "Method"_s, "Getter"_s, "Setter"_s, "Operation"_s };
        auto& builtins = Builtins::singleton();
        for (unsigned i = 0; i < builtins.size(); ++i) {
            auto& builtin = builtins.at(BuiltinID(i));
            static constexpr ASCIILiteral implementations[] = { "Generic"_s, "Call"_s, "Inline"_s, "Number"_s, "Cell"_s, "Operation"_s, "Prelude"_s };
            static constexpr ASCIILiteral callees[] = { "None"_s, "Function"_s, "HostFunction"_s, "InternalFunction"_s };
            out.print(kinds[static_cast<unsigned>(builtin.kind)], " ", builtin.holder.isEmpty() ? "-"_s : builtin.holder, " ", builtin.name, " ", builtin.numberOfRequiredParameters(), " ", builtin.callsUserCode() ? "unknown" : "known");
            out.print(" ", implementations[builtin.implementation.kind], " ", callees[static_cast<unsigned>(builtin.implementation.callee)]);
            if (builtin.implementation.kind == BuiltinImplementation::Number)
                out.printf(" %016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(builtins.numberOf(BuiltinID(i)))));
            out.print("\n");
        }
        for (auto& engineClass : builtins.classes()) {
            out.print("class ", engineClass.text, " listed as ", builtinNames[engineClass.listedAs.index()].text, " type ", static_cast<unsigned>(engineClass.type), " typed array ", static_cast<unsigned>(engineClass.typedArrayType));
            out.print(" structures ", engineClass.firstOriginalStructure, " ", engineClass.numberOfOriginalStructures, "\n");
        }
        for (unsigned i = 0; i < numberOfBuiltinNames; ++i) {
            BuiltinName name(i);
            auto* speculated = builtins.classSpeculatedFor(name);
            out.print("name ", builtinNames[i].text, builtins.isGlobal(name) ? " global" : "", builtins.isReplaceable(name) ? " replaceable" : "", builtins.mayBeExtended(name) ? " extended" : "");
            out.print(builtins.classNamed(name) ? " class" : "", speculated ? " speculated " : "", speculated ? speculated->text : ""_s, "\n");
        }
        return true;
    }
    String failureReason;
    auto program = Program::tryRead(bytes, failureReason);
    if (!program) {
        out.print(failureReason);
        return false;
    }
    if (stage == "read"_s) {
        program->dump(out);
        return true;
    }
    if (stage == "optimized"_s) {
        auto findings = optimize(*program);
        program->dump(out);
        for (size_t index : program->stringsCreatedOnce)
            out.print("made once: ", String::fromUTF8(program->strings[index]), "\n");
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage == "allocations"_s) {
        program->recordsAllocations = true;
        auto findings = optimize(*program);
        printAllocationRemarks(*program, out);
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage == "uses"_s) {
        auto findings = optimize(*program);
        printAllocationUses(*program, out);
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage == "string-builders"_s) {
        auto findings = optimize(*program);
        dumpStringBuilderRemarks(*program, out);
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage.startsWith("functions:"_s)) {
        auto findings = optimize(*program);
        for (auto part : StringView(stage).substring(10).split(',')) {
            uint32_t number = 0;
            for (unsigned i = 0; i < part.length(); ++i)
                number = number * 10 + (part[i] - '0');
            if (number < program->functions.size())
                program->dump(out, FunctionID(number));
        }
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    out.print("there is no such stage");
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
