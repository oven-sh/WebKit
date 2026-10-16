/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
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
    // A few are searched faster one by one.
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
    // The last non-ASCII character of this line that ends at or before the position.
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
    if (from == to || at(to).admitsUncheckedObjects())
        return true;
    bool foreignObjectsPass = !at(from).admitsForeignObjects || (!at(from).admitsUncheckedObjects() && at(to).admitsOnlyOtherLayouts);
    return foreignObjectsPass && isObjectLayout(at(from).kind) && std::ranges::includes(at(to).members, at(from).members);
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
        if (number == std::trunc(number) && std::abs(number) <= static_cast<double>(maxKeptInteger) && !(!number && std::signbit(number)))
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
        bool isLeftUnchanged = !((reason == CastReason::As || cast.coercesFreshData()) && (from.anyObject || from.admitsForeignObjects(*this)));
        if (isLeftUnchanged && reason == CastReason::Coerce && at(cast.coercedType()).primitives & (Primitive::Number | Primitive::String | Primitive::Boolean | Primitive::BigInt)) {
            if (isConstantOfType(function.at(id), at(types.anything)))
                isLeftUnchanged = isConstantOfType(function.at(id), to);
            else
                isLeftUnchanged = !(from.primitives & ~to.primitives) && (!(from.primitives & Primitive::Int) || from.integers.isWithin(to.integers)) && (!from.hasObjects() || to.hasObjects());
        }
        if (!isLeftUnchanged)
            id = castID;
    }
    return id;
}

static BuiltinPath memberOfBuiltin(const Program& program, const BuiltinPath& base, StringID name)
{
    auto& builtins = Builtins::singleton();
    BuiltinName member = program.builtinName(name);
    if (!base || base.member)
        return { };
    if (base.isGlobalObject) {
        if (program.isNameOfGlobalThis(name))
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

struct ExpectedBuiltinsQuery {
    const Program& program;
    const Function& function;
    bool joinsParameters;
    Vector<uint32_t>* entryParametersAsked { nullptr };
    Vector<Vector<const Target*, 2>> incomingTargets { };
    Vector<ValueID, 8> parametersBeingJoined { };
    unsigned joinsLeft { 0 };

    ExpectedBuiltins ask(ValueID id)
    {
        joinsLeft = joinsParameters ? 32 : 0;
        return of(id);
    }

    ExpectedBuiltins ofHolder(ValueID id)
    {
        auto& value = function.at(program.originOf(function, id));
        if ((value.opcode != Opcode::NewObject && value.opcode != Opcode::NewArray) || !value.arguments.isEmpty())
            return of(id);
        auto global = program.find(asBytes((value.opcode == Opcode::NewObject ? "Object"_s : "Array"_s).span8()));
        return global ? ExpectedBuiltins { BuiltinPath { false, true, *global, { } }, { }, false } : ExpectedBuiltins { };
    }

    ExpectedBuiltins of(ValueID id)
    {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        auto& value = function.at(id);
        switch (value.opcode) {
        case Opcode::GetGlobal:
            if (program.isNameOfGlobalThis(value.string()))
                return { BuiltinPath { true, false, { }, { } }, { }, false };
            if (Builtins::singleton().isGlobal(program.builtinName(value.string())))
                return { BuiltinPath { false, false, value.string(), { } }, { }, false };
            return { };
        case Opcode::GetModuleVariable:
            return program.variables[value.variable().index()].expectedBuiltins;
        case Opcode::GetField:
            return program.at(value.layout()).fields[value.field()].expectedBuiltins;
        case Opcode::GetById: {
            ExpectedBuiltins base = ofHolder(value.child(0));
            ExpectedBuiltins members;
            members.add(memberOfBuiltin(program, base.first, value.string()));
            members.add(memberOfBuiltin(program, base.second, value.string()));
            return members;
        }
        case Opcode::Parameter:
            if (value.parameterBlock().index())
                return join(id, value);
            if (entryParametersAsked)
                entryParametersAsked->append(value.parameterIndex());
            if (value.parameterIndex() < function.builtinsExpectedOfEntryParameters.size())
                return function.builtinsExpectedOfEntryParameters[value.parameterIndex()];
            return { };
        case Opcode::GetCapture: {
            auto& expected = program.functions[value.function().index()].builtinsExpectedOfEntryParameters;
            return value.captureIndex() < expected.size() ? expected[value.captureIndex()] : ExpectedBuiltins { };
        }
        default:
            return { };
        }
    }

    ExpectedBuiltins join(ValueID id, const Value& parameter)
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
        ExpectedBuiltins joined;
        for (const Target* target : incomingTargets[parameter.parameterBlock().index()]) {
            if (parameter.parameterIndex() < target->arguments.size())
                joined.add(of(target->arguments[parameter.parameterIndex()]));
        }
        parametersBeingJoined.removeLast();
        return joined.areTooMany ? ExpectedBuiltins { } : joined;
    }
};

ExpectedBuiltins Program::expectedBuiltins(const Function& function, ValueID id) const
{
    return ExpectedBuiltinsQuery { *this, function, false }.ask(id);
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
    StringView view(byteCast<Latin1Character>(bytes));
    auto& table = m_stringIDs[tableOfStringWithHash(bytes.empty() ? WTF::emptyString().impl()->hash() : view.hash())];
    auto iterator = bytes.empty() ? table.find(keyOf(bytes)) : table.find<StringViewHashTranslator>(view);
    if (iterator == table.end())
        return std::nullopt;
    return StringID(iterator->value);
}

StringID Program::intern(std::span<const uint8_t> bytes)
{
    String key = keyOf(bytes);
    auto& table = m_stringIDs[tableOfStringWithHash(key.impl()->hash())];
    auto result = table.add(WTF::move(key), static_cast<uint32_t>(strings.size()));
    if (result.isNewEntry) {
        m_ownStrings.append(Vector<uint8_t>(bytes));
        strings.append(m_ownStrings.last().span());
        noteString(StringID(result.iterator->value));
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

bool Program::mayMakeSlimClosures(const Function& function) const
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
    if (function.isUsedAsObject || function.declaredAs & (functionIsGenerator | functionReadsNewTarget | functionIsOfBuiltinModule))
        return false;
    return !function.has(Format::FunctionIsBuiltin) && !function.has(Format::FunctionIsOfBuiltinModule) && !function.has(Format::FunctionIsGenerator) && !function.isGenericEntrypoint() && !function.takesItsClosure && !isResumeFunction(function);
}

std::pair<unsigned, unsigned> Program::referencesAndCapturesOfClosures(const Function& function) const
{
    RELEASE_ASSERT(!function.isGenericEntrypoint());
    // A class object captures the environment of its class, whether its constructor takes that or not.
    if (function.kind == FunctionKind::Constructor)
        return at(function.constructedClass).isEvaluatedOnce ? std::pair(0u, 0u) : std::pair(1u, 1u);
    unsigned references = 0;
    for (TypeID type : function.captureTypes)
        references += holdsCells(repOf(at(type)));
    return { references, static_cast<unsigned>(function.captureTypes.size()) };
}

void Program::readNamesAndStringsOffValues()
{
    Vector<Vector<StringID>> namesOfFunctions(functions.size());
    Vector<UsesOfName> usesOfStrings(strings.size());
    Vector<uint32_t> usesOfConstants(FillWith { }, strings.size(), 0);
    auto noteNamesDefinedAsAccessors = [&](unsigned i, const Function& function) {
        Vector<ValueID, 2> literalsOfDescriptors;
        auto definesField = [](const Value& value) {
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
        };
        for (auto& value : function.values) {
            if (value.opcode != Opcode::CallBuiltin || value.arguments.size() < 2)
                continue;
            auto& builtin = Builtins::singleton().at(value.builtin());
            bool definesAccessor = builtin.implementation.kind == BuiltinImplementation::Operation && (builtin.implementation.operation == LanguageOperation::DefineGetter || builtin.implementation.operation == LanguageOperation::DefineSetter);
            bool isStaticFunction = builtin.kind == BuiltinKind::StaticFunction;
            bool takesDescriptor = isStaticFunction && (builtin.holder == "Object"_s || builtin.holder == "Reflect"_s) && builtin.name == "defineProperty"_s;
            if ((definesAccessor || takesDescriptor) && function.at(value.arguments[1]).opcode == Opcode::String)
                namesOfFunctions[i].append(function.at(value.arguments[1]).string());
            if (isStaticFunction && builtin.holder == "Object"_s && (builtin.name == "defineProperties"_s || builtin.name == "create"_s))
                literalsOfDescriptors.append(value.arguments[1]);
        }
        if (literalsOfDescriptors.isEmpty())
            return;
        for (auto& value : function.values) {
            if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 3 && definesField(value) && literalsOfDescriptors.contains(value.arguments[0]) && function.at(value.arguments[1]).opcode == Opcode::String)
                namesOfFunctions[i].append(function.at(value.arguments[1]).string());
        }
    };
    auto countUsesOfNames = [&](unsigned i, const Function& function) {
        BitVector valuesThatRunOnce;
        if (runsOnce(FunctionID(i))) {
            BitVector isInLoop = blocksInLoops(function);
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (isInLoop.get(b))
                    continue;
                for (ValueID id : function.blocks[b].values)
                    valuesThatRunOnce.set(id.index());
            }
        }
        unsigned next = 0;
        for (auto& value : function.values) {
            unsigned index = next++;
            StringID name;
            AccessByName access = value.opcode == Opcode::GetById ? AccessByName::Read : value.opcode == Opcode::PutById ? AccessByName::Store : value.opcode == Opcode::CallMethod ? AccessByName::Call : AccessByName::Other;
            if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod || value.opcode == Opcode::InById || value.opcode == Opcode::DeleteById
                || value.opcode == Opcode::GetGlobal || value.opcode == Opcode::GetGlobalForTypeOf || value.opcode == Opcode::PutGlobal)
                name = value.string();
            // The generic fallback.
            else if (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).takesReceiver()) {
                name = stringOf(Builtins::singleton().at(value.builtin()).nameName);
                access = AccessByName::Call;
            }
            // A definition under a name: DefineField (object, name, value), Object.defineProperty(object, name, descriptor).
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
                        auto& ofPrototype = usesOfStrings[prototype->index()];
                        WTF::atomicExchangeAdd(&ofPrototype.uses, 1u, std::memory_order_relaxed);
                        WTF::atomicExchangeAdd(&ofPrototype.usesByAccess[static_cast<unsigned>(AccessByName::Read)], 1u, std::memory_order_relaxed);
                    }
                }
            }
            if (!name)
                continue;
            auto& ofName = usesOfStrings[name.index()];
            WTF::atomicExchangeAdd(&ofName.uses, 1u, std::memory_order_relaxed);
            if (!valuesThatRunOnce.get(index))
                WTF::atomicExchangeAdd(&ofName.usesThatAskTheTable, 1u, std::memory_order_relaxed);
            if (access != AccessByName::Other)
                WTF::atomicExchangeAdd(&ofName.usesByAccess[static_cast<unsigned>(access)], 1u, std::memory_order_relaxed);
        }
    };
    forEachIndexInParallel(functions.size(), [&](unsigned i) {
        auto& function = std::as_const(functions)[i];
        noteNamesDefinedAsAccessors(i, function);
        countUsesOfNames(i, function);
        for (auto& value : function.values) {
            if (value.opcode == Opcode::String)
                WTF::atomicExchangeAdd(&usesOfConstants[value.string().index()], 1u, std::memory_order_relaxed);
        }
    });
    m_namesAndStringsInValues = NamesAndStringsInValues { WTF::move(namesOfFunctions), WTF::move(usesOfStrings), WTF::move(usesOfConstants) };
}

void Program::findNamesThatMayBeAccessors()
{
    if (!m_namesAndStringsInValues)
        readNamesAndStringsOffValues();
    namesThatMayBeAccessors.clearAll();
    for (auto& names : m_namesAndStringsInValues->namesDefinedAsAccessors) {
        for (StringID name : names)
            namesThatMayBeAccessors.set(name.index());
    }
    for (auto& layout : layouts) {
        if (!isObjectLayout(layout.kind))
            continue;
        for (auto& method : layout.methods) {
            if (method.name && method.kind != MethodKind::Method)
                namesThatMayBeAccessors.set(method.name.index());
        }
    }
}

auto Program::genericAccessIdentifiersAndNumberOfUses() const -> Vector<UsesOfName>
{
    RELEASE_ASSERT(m_namesAndStringsInValues && m_namesAndStringsInValues->usesOfNames.size() == strings.size());
    Vector<UsesOfName> usesOfStrings = m_namesAndStringsInValues->usesOfNames;
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
    Vector<UsesOfName> result;
    for (uint32_t index = 0; index < strings.size(); ++index) {
        auto& ofName = usesOfStrings[index];
        if (!ofName.uses && !isNameInLayout.quickGet(index))
            continue;
        StringID name(index);
        // `#name`, and a name with half a surrogate pair, are no UTF-8: such an access has no cache, and the run time makes out the name.
        if (!isConstantSymbol(string(name)) && !isEnginePrivateName(string(name)) && String::fromUTF8(string(name)).isNull())
            continue;
        ofName.name = name;
        result.append(ofName);
    }
    return result;
}

Vector<uint32_t> Program::keysOfStringsAskedForMost(unsigned limit) const
{
    static constexpr uint32_t usesThatPayForStubOfOneString = isARM64() ? 20 : 7;
    RELEASE_ASSERT(m_namesAndStringsInValues && m_namesAndStringsInValues->usesOfStringConstants.size() == strings.size());
    auto& uses = m_namesAndStringsInValues->usesOfStringConstants;
    Vector<uint32_t> most;
    for (uint32_t index = 0; index < uses.size(); ++index) {
        if (uses[index] >= usesThatPayForStubOfOneString && string(StringID(index)).size() > 1)
            most.append(index);
    }
    std::ranges::sort(most, [&](uint32_t a, uint32_t b) {
        return uses[a] != uses[b] ? uses[a] > uses[b] : a < b;
    });
    if (most.size() > limit)
        most.shrink(limit);
    for (uint32_t& index : most)
        ++index;
    return most;
}

void Program::noteString(StringID id)
{
    BuiltinName name = Builtins::singleton().nameOf(string(id));
    m_builtinNames.append(name);
    if (!name)
        return;
    if (m_stringsOfBuiltinNames.size() <= name.index())
        m_stringsOfBuiltinNames.grow(name.index() + 1);
    m_stringsOfBuiltinNames[name.index()] = id;
}

namespace {

class ConstantSymbolNaming {
public:
    explicit ConstantSymbolNaming(Program& program)
        : m_program(program)
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
        if (!nameSymbols())
            return;
        m_names.fill(StringID(), m_program.variables.size());
        m_errorsThatRefuteNames.fill(VariableID(), m_program.variables.size());
        collectExports();
        followLiteralsThatEscapeLate();
        while (nameVariables()) { }
        m_readsOfLiteralsThatAreFacts = m_readsOfLiterals;
        m_takesAFailureForFinal = true;
        m_mayRunAgain.clear();
        m_valuesOfLiterals.clear();
        m_readsOfLiterals.clear();
        followLiteralsThatEscapeLate();
        while (nameVariables()) { }
        nameKeys();
    }

private:
    struct Place {
        FunctionID function;
        unsigned block { 0 };
        unsigned index { 0 };
    };

    struct FactsOfFunction {
        BitVector blocksInLoops;
        BitVector blocksBehindFunctionValues;
        Vector<unsigned> firstFunctionValue;
    };

    struct Export {
        FunctionID function;
        ValueID value;
    };

    enum class ChangeKind : uint8_t { TestAheadOfAccessByName, SymbolOfFirstEvaluation, MarkOfFailure };

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

    static std::optional<Place> placeOf(FunctionID id, const Function& function, ValueID value)
    {
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& values = function.blocks[b].values;
            for (unsigned k = 0; k < values.size(); ++k) {
                if (values[k] == value)
                    return Place { id, b, k };
            }
        }
        return std::nullopt;
    }

    static uint64_t keyOfExport(uint32_t module, StringID name) { return static_cast<uint64_t>(module) << 32 | name.index(); }

    std::optional<uint32_t> builtinModuleRequiredBy(const Function& function, const Value& value) const
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

    bool isCallOfSymbol(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::Call || value.arguments.size() > 1 || (!value.arguments.isEmpty() && function.at(value.arguments[0]).opcode != Opcode::String))
            return false;
        auto& callee = function.at(value.child(0));
        return callee.opcode == Opcode::GetGlobal && callee.string() == m_symbol;
    }

    bool isCallOfSymbolFor(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::CallMethod || !m_for || value.string() != m_for || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::String)
            return false;
        auto& receiver = function.at(value.child(0));
        return receiver.opcode == Opcode::GetGlobal && receiver.string() == m_symbol;
    }

    void collect()
    {
        struct Found {
            Vector<std::pair<uint32_t, Place>> stores;
            Vector<std::pair<uint32_t, Place>> clearsInHandlers;
            Vector<uint32_t> escapedExports;
            Vector<Place> calls;
            Vector<std::pair<uint32_t, uint32_t>> callsOfModuleCode;
            Vector<std::pair<uint32_t, Place>> referencesToHiddenCode;
        };
        unsigned count = m_program.functions.size();
        Vector<Found> foundIn(count);
        m_isReadFromOutside.fill(0, m_program.variables.size());
        m_isEnteredFromOutside.fill(0, m_program.modules.size());
        m_numberOfReads.fill(0, m_program.variables.size());
        m_isClearedInHandler.fill(0, m_program.variables.size());
        m_isClearedElsewhere.fill(0, m_program.variables.size());
        auto note = [](uint8_t& flag) {
            WTF::atomicStore(&flag, static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = m_program.functions[i];
            auto& found = foundIn[i];
            Vector<std::pair<ValueID, uint32_t>, 8> required;
            bool isRunOnce = runsOnce(FunctionID(i));
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    auto& value = function.at(values[k]);
                    switch (value.opcode) {
                    case Opcode::PutModuleVariable:
                        found.stores.append({ value.variable().index(), Place { FunctionID(i), b, k } });
                        if (function.at(value.child(0)).opcode == Opcode::Undefined) {
                            bool isInHandler = function.at(values[0]).opcode == Opcode::CatchValue;
                            note((isInHandler ? m_isClearedInHandler : m_isClearedElsewhere)[value.variable().index()]);
                            if (isInHandler)
                                found.clearsInHandlers.append({ value.variable().index(), Place { FunctionID(i), b, k } });
                        }
                        break;
                    case Opcode::GetModuleVariable:
                        WTF::atomicExchangeAdd(&m_numberOfReads[value.variable().index()], 1u, std::memory_order_relaxed);
                        if (m_program.variables[value.variable().index()].module != function.module)
                            note(m_isReadFromOutside[value.variable().index()]);
                        break;
                    case Opcode::Call:
                        if (isRunOnce && isCallOfSymbol(function, value))
                            found.calls.append(Place { FunctionID(i), b, k });
                        break;
                    case Opcode::CallMethod:
                        if (isCallOfSymbolFor(function, value))
                            found.calls.append(Place { FunctionID(i), b, k });
                        break;
                    case Opcode::FunctionReference:
                    case Opcode::NewClosure:
                    case Opcode::NewClosureOnce:
                    case Opcode::CallDirect:
                        if (ModuleID module = m_program.at(value.function()).module; module && module != function.module && !runsOnce(value.function()))
                            note(m_isEnteredFromOutside[module.index()]);
                        if (m_program.at(value.function()).kind == FunctionKind::Module)
                            found.callsOfModuleCode.append({ value.function().index(), value.opcode == Opcode::CallDirect ? i : UINT32_MAX });
                        if (auto& callee = m_program.at(value.function()); callee.module && (callee.kind == FunctionKind::Module || callee.has(Format::FunctionIsHiddenInStackTraces)))
                            found.referencesToHiddenCode.append({ value.function().index(), Place { value.opcode == Opcode::CallDirect ? FunctionID(i) : FunctionID(), b, k } });
                        break;
                    case Opcode::Operation:
                        if (auto module = builtinModuleRequiredBy(function, value))
                            required.append({ values[k], *module });
                        break;
                    default:
                        break;
                    }
                }
            }
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
                    auto& value = std::as_const(function).at(id);
                    if (value.opcode != Opcode::GetById && value.opcode != Opcode::IsUndefinedOrNull)
                        value.forEachChild(noteUse);
                }
                block.terminator.forEachChild([&](ValueID& used) {
                    noteUse(used);
                });
            }
        });
        m_numberOfStores.fill(0, m_program.variables.size());
        m_stores.fill(Place { }, m_program.variables.size());
        for (auto& found : foundIn) {
            for (auto& [variable, place] : found.stores) {
                m_numberOfStores[variable] = static_cast<uint8_t>(std::min(m_numberOfStores[variable] + 1, 3));
                m_stores[variable] = place;
            }
            m_clearsInHandlers.appendVector(found.clearsInHandlers);
            for (uint32_t module : found.escapedExports)
                m_escapedExports.add(module);
            m_calls.appendVector(found.calls);
            m_referencesToHiddenCode.appendVector(found.referencesToHiddenCode);
            for (auto [callee, caller] : found.callsOfModuleCode) {
                if (!m_callersOfModuleCode.add(callee, caller).isNewEntry)
                    m_callersOfModuleCode.set(callee, UINT32_MAX);
            }
        }
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
                    if (m_program.variables[value.variable().index()].mayBeSeenUninitialized() && !value.findsVariableInitialized())
                        return true;
                    break;
                case Opcode::Call:
                    if (!isCallOfSymbol(function, value))
                        return true;
                    break;
                case Opcode::CallMethod:
                    if (!isCallOfSymbolFor(function, value))
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
            auto caller = m_callersOfModuleCode.find(id.index());
            if (!initializer || caller == m_callersOfModuleCode.end() || caller->value != initializer.index())
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
            return !m_takesAFailureForFinal || !errorKeptFor(function.module);
        }).iterator->value;
    }

    VariableID errorKeptFor(ModuleID module) const
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

    static bool keepsFunctionsOutOfReach(const Value& value)
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

    const FactsOfFunction& factsOf(FunctionID id)
    {
        return *m_facts.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            auto facts = makeUniqueWithoutFastMallocCheck<FactsOfFunction>();
            unsigned count = function.blocks.size();
            facts->blocksInLoops = blocksInLoops(function);
            facts->blocksBehindFunctionValues.ensureSize(count);
            facts->firstFunctionValue.fill(UINT_MAX, count);
            Vector<unsigned> worklist;
            for (unsigned b = 0; b < count; ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    if (keepsFunctionsOutOfReach(function.at(values[k])))
                        continue;
                    facts->firstFunctionValue[b] = k;
                    worklist.append(b);
                    break;
                }
            }
            while (!worklist.isEmpty()) {
                for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                    if (!facts->blocksBehindFunctionValues.set(target.block.index()))
                        worklist.append(target.block.index());
                }
            }
            return facts;
        }).iterator->value;
    }

    bool comesBehind(const Place& first, unsigned block, unsigned index) const
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

    bool startsWithAValue(unsigned variable) const
    {
        return m_program.variables[variable].startsUndefined || m_program.variables[variable].startsZero;
    }

    bool isStoredOnceAndBeforeEveryRead(unsigned variable)
    {
        const Place& store = m_stores[variable];
        if (m_numberOfStores[variable] != 1 || !runsOnce(store.function))
            return false;
        auto& facts = factsOf(store.function);
        if (facts.blocksInLoops.get(store.block))
            return false;
        if (!startsWithAValue(variable))
            return true;
        ModuleID module = m_program.at(store.function).module;
        if (!module || m_program.variables[variable].module != module || m_isReadFromOutside[variable] || m_isEnteredFromOutside[module.index()])
            return false;
        return !facts.blocksBehindFunctionValues.get(store.block) && facts.firstFunctionValue[store.block] > store.index;
    }

    bool holdsItsSymbolWhenRead(unsigned variable, FunctionID reader, const Function& function, ValueID read) const
    {
        if (!startsWithAValue(variable) || m_stores[variable].function != reader)
            return true;
        auto place = placeOf(reader, function, read);
        return place && comesBehind(m_stores[variable], place->block, place->index);
    }

    bool nameSymbols()
    {
        bool hasAny = false;
        for (const Place& place : m_calls) {
            auto& function = m_program.functions[place.function.index()];
            auto& block = function.blocks[place.block];
            auto& call = function.at(block.values[place.index]);
            if (block.terminator.kind == TerminatorKind::Invoke && place.index + 1 == block.values.size())
                continue;
            Vector<uint8_t, 64> name;
            VariableID error;
            name.append(static_cast<uint8_t>(0xfc));
            if (call.opcode == Opcode::CallMethod)
                name.append(static_cast<uint8_t>('r'));
            else {
                if (factsOf(place.function).blocksInLoops.get(place.block))
                    continue;
                if (mayRunAgain(place.function)) {
                    m_takesAFailureForFinal = true;
                    m_mayRunAgain.remove(place.function.index());
                    bool mayRunAgainOtherwise = mayRunAgain(place.function);
                    m_mayRunAgain.remove(place.function.index());
                    m_takesAFailureForFinal = false;
                    if (mayRunAgainOtherwise)
                        continue;
                    error = errorKeptFor(function.module);
                }
                CString numbers = makeString(place.function.index(), '.', block.values[place.index].index()).ascii();
                name.append(byteCast<uint8_t>(numbers.span()));
            }
            name.append(static_cast<uint8_t>(call.arguments.isEmpty() ? ';' : ':'));
            if (!call.arguments.isEmpty())
                name.append(m_program.string(function.at(call.arguments[0]).string()));
            if (error) {
                StringID ofFirstEvaluation = m_program.intern(name.span());
                m_symbolsOfFirstEvaluations.add(static_cast<uint64_t>(place.function.index()) << 32 | block.values[place.index].index(), ofFirstEvaluation);
                m_changes.add(place.function.index(), Vector<Change>()).iterator->value.append(Change { ChangeKind::SymbolOfFirstEvaluation, place.block, place.index, ofFirstEvaluation, error });
                m_errorsThatMarkFailures.add(error.index());
                hasAny = true;
                continue;
            }
            call.opcode = Opcode::ConstantSymbol;
            call.children = { };
            call.arguments.clear();
            call.immediates = { m_program.intern(name.span()).index(), Format::none };
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
            unsigned blockOfReturn = 0;
            unsigned numberOfReturns = 0;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                if (function.blocks[b].terminator.kind != TerminatorKind::Return)
                    continue;
                ++numberOfReturns;
                blockOfReturn = b;
                exports = function.blocks[b].terminator.value;
            }
            if (numberOfReturns != 1 || !exports)
                continue;
            ValueID storeOfExports;
            if (auto& returned = function.at(exports); returned.opcode == Opcode::GetModuleVariable) {
                unsigned variable = returned.variable().index();
                const Place& store = m_stores[variable];
                auto read = placeOf(id, function, exports);
                if (m_numberOfStores[variable] != 1 || m_numberOfReads[variable] != 1 || store.function != id || !read || !comesBehind(store, read->block, read->index))
                    continue;
                storeOfExports = function.blocks[store.block].values[store.index];
                exports = function.at(storeOfExports).child(0);
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
                    if (!uses || block.values[k] == storeOfExports)
                        continue;
                    bool isDefinition = value.opcode == Opcode::Operation && m_program.operationOf(value) == Operations::DefineField && value.arguments.size() == 3 && value.arguments[0] == exports && value.arguments[1] != exports && value.arguments[2] != exports
                        && function.at(value.arguments[1]).opcode == Opcode::String && comesBehind(Place { id, b, k }, blockOfReturn, UINT_MAX);
                    if (!isDefinition) {
                        isOnlyDefinedAndReturned = false;
                        break;
                    }
                    defined.append({ function.at(value.arguments[1]).string(), value.arguments[2] });
                }
                for (auto& target : block.terminator.targets)
                    isOnlyDefinedAndReturned &= !target.arguments.contains(exports);
                if (block.terminator.value == exports && (block.terminator.kind != TerminatorKind::Return || storeOfExports))
                    isOnlyDefinedAndReturned = false;
            }
            if (!isOnlyDefinedAndReturned)
                continue;
            for (auto& [name, value] : defined) {
                if (!m_exports.add(keyOfExport(module, name), Export { id, value }).isNewEntry)
                    m_exports.set(keyOfExport(module, name), Export { });
            }
        }
    }

    using EdgesIntoBlocks = Vector<Vector<const Target*, 2>>;

    struct YieldedSymbol {
        StringID name;
        VariableID errorThatRefutes;

        explicit operator bool() const { return !!name; }
    };

    YieldedSymbol symbolYieldedBy(FunctionID id, ValueID yielded, EdgesIntoBlocks& edges, unsigned depth = 0) const
    {
        auto& function = m_program.at(id);
        yielded = withoutUpcasts(function, yielded);
        auto& value = function.at(yielded);
        switch (value.opcode) {
        case Opcode::ConstantSymbol:
            return { value.string(), { } };
        case Opcode::Call: {
            if (!m_takesAFailureForFinal)
                return { };
            auto found = m_symbolsOfFirstEvaluations.find(static_cast<uint64_t>(id.index()) << 32 | yielded.index());
            if (found == m_symbolsOfFirstEvaluations.end())
                return { };
            return { found->value, errorKeptFor(function.module) };
        }
        case Opcode::GetModuleVariable: {
            unsigned variable = value.variable().index();
            if (!m_names[variable] || !holdsItsSymbolWhenRead(variable, id, function, yielded))
                return { };
            return { m_names[variable], m_errorsThatRefuteNames[variable] };
        }
        case Opcode::GetById: {
            uint64_t read = static_cast<uint64_t>(id.index()) << 32 | yielded.index();
            if (!depth && m_readsOfLiterals.contains(read)) {
                auto found = m_valuesOfLiterals.find(keyOfExport(function.at(withoutUpcasts(function, value.child(0))).variable().index(), value.string()));
                if (found == m_valuesOfLiterals.end() || !found->value.function)
                    return { };
                YieldedSymbol made = symbolYieldedBy(found->value.function, found->value.value, edges, depth + 1);
                if (!made || m_readsOfLiteralsThatAreFacts.contains(read))
                    return made;
                if (made.errorThatRefutes)
                    return { };
                return { made.name, errorKeptFor(function.module) };
            }
            auto module = builtinModuleRequiredBy(function, function.at(value.child(0)));
            if (!module || depth)
                return { };
            auto found = m_exports.find(keyOfExport(*module, value.string()));
            return found != m_exports.end() && found->value.function ? symbolYieldedBy(found->value.function, found->value.value, edges, depth + 1) : YieldedSymbol { };
        }
        case Opcode::ToPropertyKey:
            return symbolYieldedBy(id, value.child(0), edges, depth + 1);
        case Opcode::Parameter: {
            unsigned b = value.immediates[0];
            if (depth || !b || b >= function.blocks.size())
                return { };
            size_t index = function.blocks[b].parameters.find(yielded);
            if (index == notFound)
                return { };
            if (edges.isEmpty()) {
                edges.grow(function.blocks.size());
                for (auto& block : function.blocks) {
                    for (auto& target : block.terminator.targets)
                        edges[target.block.index()].append(&target);
                }
            }
            YieldedSymbol symbol;
            for (const Target* target : edges[b]) {
                YieldedSymbol given = index < target->arguments.size() ? symbolYieldedBy(id, target->arguments[index], edges, 1) : YieldedSymbol { };
                if (!given || (symbol && (given.name != symbol.name || given.errorThatRefutes != symbol.errorThatRefutes)))
                    return { };
                symbol = given;
            }
            return symbol;
        }
        default:
            return { };
        }
    }

    std::optional<unsigned> moduleEvaluatedBy(FunctionID id) const
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

    bool isPassedOnEveryWayOut(const Place& place) const
    {
        auto& function = m_program.at(place.function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (function.blocks[b].terminator.kind == TerminatorKind::Return && !comesBehind(place, b, UINT_MAX))
                return false;
        }
        return true;
    }

    bool isEvaluatedAgainAfterAFailure(const Function& function) const
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

    void followLiteralsThatEscapeLate()
    {
        if (!m_program.usesOperation(Operations::DefineField))
            return;
        struct Literal {
            unsigned variable;
            Vector<Place, 2> escapes;
            Vector<std::pair<Place, ValueID>, 8> reads;
        };
        Vector<Literal> literals;
        Vector<uint32_t> literalOf;
        literalOf.fill(UINT32_MAX, m_program.variables.size());
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.kind != FunctionKind::Module || !function.module || function.blocks.size() != 1 || !runsOnce(FunctionID(i)) || mayThrow(function) || mayRunAgain(FunctionID(i)) || !isEvaluatedAgainAfterAFailure(function))
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
                    if (!m_valuesOfLiterals.add(keyOfExport(variable, name), Export { FunctionID(i), value }).isNewEntry)
                        m_valuesOfLiterals.set(keyOfExport(variable, name), Export { });
                }
            }
        }
        if (literals.isEmpty())
            return;

        struct UseOfLiteral {
            uint32_t literal;
            Place place;
            ValueID read;
        };
        Vector<Vector<UseOfLiteral>> usesIn(m_program.functions.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = std::as_const(m_program).functions[i];
            Vector<std::pair<ValueID, uint32_t>, 4> held;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::GetModuleVariable && literalOf[value.variable().index()] != UINT32_MAX)
                        held.append({ id, literalOf[value.variable().index()] });
                }
            }
            if (held.isEmpty())
                return;
            auto literalHeldBy = [&](ValueID id) {
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
                        uint32_t literal = literalHeldBy(child);
                        if (literal == UINT32_MAX || value.opcode == Opcode::IsUndefinedOrNull)
                            return;
                        usesIn[i].append({ literal, Place { FunctionID(i), b, k }, value.opcode == Opcode::GetById ? block.values[k] : ValueID() });
                    });
                }
                auto noteEscape = [&](ValueID child) {
                    if (uint32_t literal = literalHeldBy(child); literal != UINT32_MAX)
                        usesIn[i].append({ literal, Place { FunctionID(i), b, UINT_MAX }, ValueID() });
                };
                if (block.terminator.value)
                    noteEscape(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        noteEscape(argument);
                }
            }
        });
        for (auto& uses : usesIn) {
            for (auto& use : uses) {
                if (use.read)
                    literals[use.literal].reads.append({ use.place, use.read });
                else
                    literals[use.literal].escapes.append(use.place);
            }
        }

        unsigned numberOfModules = m_program.modules.size();
        Vector<Vector<Place, 2>> callersOfEvaluation(numberOfModules);
        UncheckedKeyHashMap<uint32_t, Vector<std::pair<Place, unsigned>, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> evaluationsCalledIn;
        BitVector isEvaluatedFromAnywhere(numberOfModules);
        for (auto& [callee, place] : m_referencesToHiddenCode) {
            auto module = moduleEvaluatedBy(FunctionID(callee));
            if (!module)
                continue;
            if (!place.function || !m_program.at(place.function).module) {
                isEvaluatedFromAnywhere.set(*module);
                continue;
            }
            if (m_program.at(place.function).module.index() == *module)
                continue;
            callersOfEvaluation[*module].append(place);
            evaluationsCalledIn.add(place.function.index(), Vector<std::pair<Place, unsigned>, 4> { }).iterator->value.append({ place, *module });
        }
        Vector<Vector<FunctionID, 2>> codeOf(numberOfModules);
        BitVector waits(numberOfModules);
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.kind != FunctionKind::Module || !function.module)
                continue;
            if (function.has(Format::FunctionIsAsync) || function.has(Format::FunctionIsGenerator))
                waits.set(function.module.index());
            if (runsOnce(FunctionID(i)))
                codeOf[function.module.index()].append(FunctionID(i));
        }

        auto modulesThatHaveEndedAt = [&](const Place& escape) -> std::optional<BitVector> {
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
                for (auto& [place, module] : called->value) {
                    if (code == escape.function ? comesBehind(place, escape.block, escape.index) : code == m_program.modules[own].initializer)
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
                    for (auto& [place, evaluated] : called->value) {
                        if (isInitializer || isPassedOnEveryWayOut(place))
                            take(evaluated);
                    }
                }
            }
            if (ended.get(own))
                return std::nullopt;
            for (bool changed = true; changed;) {
                changed = false;
                for (unsigned module : ended) {
                    bool isBegunWithin = !isEvaluatedFromAnywhere.get(module) && std::ranges::all_of(callersOfEvaluation[module], [&](const Place& caller) {
                        unsigned calling = m_program.at(caller.function).module.index();
                        return (calling == own || ended.get(calling)) && (runsOnce(caller.function) || !m_isEnteredFromOutside[calling]);
                    });
                    if (!isBegunWithin) {
                        ended.clear(module);
                        changed = true;
                    }
                }
            }
            return ended;
        };

        for (auto& literal : literals) {
            Vector<std::pair<Place, BitVector>, 2> escapes;
            bool isFollowed = true;
            for (auto& escape : literal.escapes) {
                auto ended = modulesThatHaveEndedAt(escape);
                if (!ended) {
                    isFollowed = false;
                    break;
                }
                escapes.append({ escape, WTF::move(*ended) });
            }
            if (!isFollowed)
                continue;
            for (auto& taken : literal.reads) {
                const Place& place = taken.first;
                auto& reader = m_program.at(place.function);
                bool isAheadOfEveryEscape = escapes.isEmpty() || (reader.kind == FunctionKind::Module && reader.module && runsOnce(place.function) && !mayRunAgain(place.function) && std::ranges::all_of(escapes, [&](auto& escape) {
                    return escape.first.function == place.function ? comesBehind(place, escape.first.block, escape.first.index) : escape.second.get(reader.module.index());
                }));
                if (isAheadOfEveryEscape)
                    m_readsOfLiterals.add(static_cast<uint64_t>(place.function.index()) << 32 | taken.second.index());
            }
        }
    }

    bool nameVariables()
    {
        bool changed = false;
        UncheckedKeyHashMap<uint32_t, EdgesIntoBlocks, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> edgesByFunction;
        for (unsigned variable = 0; variable < m_program.variables.size(); ++variable) {
            if (m_names[variable] || m_numberOfStores[variable] != 1)
                continue;
            const Place& store = m_stores[variable];
            auto& function = m_program.at(store.function);
            EdgesIntoBlocks& edges = edgesByFunction.add(store.function.index(), EdgesIntoBlocks()).iterator->value;
            YieldedSymbol symbol = symbolYieldedBy(store.function, function.at(function.blocks[store.block].values[store.index]).child(0), edges);
            if (!symbol || !isStoredOnceAndBeforeEveryRead(variable))
                continue;
            if (symbol.errorThatRefutes && (symbol.errorThatRefutes != errorKeptFor(m_program.variables[variable].module) || function.module != m_program.variables[variable].module))
                continue;
            m_names[variable] = symbol.name;
            m_errorsThatRefuteNames[variable] = symbol.errorThatRefutes;
            changed = true;
        }
        return changed;
    }

    static void accessByName(Value& value, StringID name)
    {
        ValueID stored = value.opcode == Opcode::PutByVal ? value.child(2) : ValueID();
        value.opcode = value.opcode == Opcode::PutByVal ? Opcode::PutById : Opcode::GetById;
        value.children = { value.child(0), stored, ValueID() };
        value.immediates = { name.index(), Format::none };
        value.setPredictedByKey(StringID());
        value.hasStringKeysByType = false;
    }

    void nameKeys()
    {
        for (VariableID error : m_errorsThatRefuteNames) {
            if (error)
                m_errorsThatMarkFailures.add(error.index());
        }
        for (auto& [mark, place] : m_clearsInHandlers) {
            VariableID error = errorKeptFor(m_program.variables[mark].module);
            if (!error || !m_errorsThatMarkFailures.contains(error.index()))
                continue;
            m_program.variables[error.index()].isStoredOnce = false;
            m_changes.add(place.function.index(), Vector<Change>()).iterator->value.append(Change { ChangeKind::MarkOfFailure, place.block, place.index, { }, error });
        }
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = m_program.functions[i];
            EdgesIntoBlocks edges;
            Vector<Change> changes;
            if (auto found = m_changes.find(i); found != m_changes.end())
                changes = found->value;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    auto& value = function.at(values[k]);
                    if (value.opcode != Opcode::GetByVal && value.opcode != Opcode::PutByVal)
                        continue;
                    if ((value.opcode == Opcode::GetByVal && value.immediates[0] != Format::none) || !m_program.typeOf(function, value.child(0)).isAnything())
                        continue;
                    ValueID key = withoutUpcasts(function, value.child(1));
                    Opcode keyOpcode = function.at(key).opcode;
                    if (keyOpcode != Opcode::GetModuleVariable && keyOpcode != Opcode::ConstantSymbol && keyOpcode != Opcode::Parameter && keyOpcode != Opcode::ToPropertyKey)
                        continue;
                    YieldedSymbol symbol = symbolYieldedBy(FunctionID(i), key, edges);
                    if (!symbol)
                        continue;
                    if (!symbol.errorThatRefutes)
                        accessByName(value, symbol.name);
                    else if (m_errorsThatMarkFailures.contains(symbol.errorThatRefutes.index()))
                        changes.append(Change { ChangeKind::TestAheadOfAccessByName, b, k, symbol.name, symbol.errorThatRefutes });
                }
            }
            std::ranges::sort(changes, [](const Change& a, const Change& b) {
                return a.block != b.block ? a.block > b.block : a.index > b.index;
            });
            for (auto& change : changes)
                apply(function, change);
        });
    }

    Value valueLike(const Value& other, Opcode opcode, TypeID type) const
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
        if (change.kind == ChangeKind::MarkOfFailure) {
            Value yes = valueLike(replaced, Opcode::Boolean, m_program.types.boolean);
            yes.immediates[0] = 1;
            Value store = replaced;
            store.children[0] = add(function, WTF::move(yes));
            store.immediates[0] = change.error.index();
            ValueID marks[] = { store.children[0], add(function, WTF::move(store)) };
            function.blocks[change.block].values.insertSpan(change.index + 1, std::span<const ValueID> { marks });
            return;
        }
        bool yieldsNothing = replaced.rep == Rep::Void;
        if (yieldsNothing) {
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
        Value error = valueLike(replaced, Opcode::GetModuleVariable, m_program.types.anything);
        error.immediates[0] = change.error.index();
        Value hasNotFailed = valueLike(replaced, Opcode::StrictEq, m_program.types.boolean);
        hasNotFailed.children = { add(function, WTF::move(error)), add(function, valueLike(replaced, Opcode::Undefined, m_program.types.undefined)), ValueID() };
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
        Value asExpected = replaced;
        if (change.kind == ChangeKind::SymbolOfFirstEvaluation) {
            asExpected.opcode = Opcode::ConstantSymbol;
            asExpected.children = { };
            asExpected.arguments.clear();
            asExpected.immediates = { change.name.index(), Format::none };
        } else
            accessByName(asExpected, change.name);
        auto finish = [&](BlockID block, ValueID value) {
            auto& finished = function.blocks[block.index()];
            finished.values.append(value);
            finished.terminator.kind = isWatched ? TerminatorKind::Invoke : TerminatorKind::Jump;
            finished.terminator.targets.append(Target { after, yieldsNothing ? Arguments { } : Arguments { value } });
            if (isWatched)
                finished.terminator.targets.append(handler);
        };
        finish(expected, add(function, WTF::move(asExpected)));
        if (yieldsNothing) {
            finish(otherwise, id);
            return;
        }
        finish(otherwise, add(function, Value(replaced)));
        Value parameter = valueLike(replaced, Opcode::Parameter, replaced.type);
        parameter.rep = replaced.rep;
        parameter.takePredictionOf(replaced);
        parameter.immediates = { after.index(), 0 };
        function.at(id) = WTF::move(parameter);
        function.blocks[after.index()].parameters.append(id);
    }

    Program& m_program;
    StringID m_symbol;
    StringID m_for;
    Vector<Place> m_calls;
    Vector<uint32_t> m_numberOfReads;
    Vector<uint8_t> m_numberOfStores;
    Vector<Place> m_stores;
    Vector<uint8_t> m_isReadFromOutside;
    Vector<uint8_t> m_isEnteredFromOutside;
    Vector<uint8_t> m_isClearedInHandler;
    Vector<uint8_t> m_isClearedElsewhere;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callersOfModuleCode;
    UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_mayRunAgain;
    Vector<StringID> m_names;
    Vector<VariableID> m_errorsThatRefuteNames;
    bool m_takesAFailureForFinal { false };
    Vector<std::pair<uint32_t, Place>> m_clearsInHandlers;
    UncheckedKeyHashMap<uint64_t, StringID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_symbolsOfFirstEvaluations;
    UncheckedKeyHashMap<uint32_t, Vector<Change>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_changes;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_errorsThatMarkFailures;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_readsOfLiteralsThatAreFacts;
    Vector<std::pair<uint32_t, Place>> m_referencesToHiddenCode;
    UncheckedKeyHashMap<uint64_t, Export, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_valuesOfLiterals;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_readsOfLiterals;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_escapedExports;
    UncheckedKeyHashMap<uint64_t, Export, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_exports;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<FactsOfFunction>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_facts;
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
            tableOf[i] = Program::tableOfStringWithHash(keys[i].impl()->hash());
        });
        std::array<Vector<uint32_t>, Program::numberOfTablesOfStringIDs> numbersIn;
        for (uint32_t i = 0; i < keys.size(); ++i)
            numbersIn[tableOf[i]].append(i);
        forEachIndexInParallel(numbersIn.size(), [&](unsigned number) {
            auto& table = m_program.m_stringIDs[number];
            table.reserveInitialCapacity(numbersIn[number].size());
            for (uint32_t i : numbersIn[number])
                table.add(WTF::move(keys[i]), i);
        });
        m_program.emptyString = m_program.intern({ });
        ConstantSymbolNaming(m_program).run();
        if (m_program.usesOperation(Operations::NewDisposableStack)) {
            m_program.intern(asBytes("The value of a using declaration must be an object, null or undefined"_s.span8()));
            m_program.intern(asBytes("The value of a using declaration has no callable dispose method"_s.span8()));
            for (ASCIILiteral name : { "Symbol"_s, "dispose"_s, "TypeError"_s, "SuppressedError"_s })
                m_program.intern(asBytes(name.span8()));
        }
        expandInitialize();
        linkForInKeys();
        makeTextsOfLargeLiterals();
        fillLongLiterals();
        markFunctionsUsedAsObjects();

        m_program.closureLayout = LayoutID(m_program.layouts.size());
        Layout closure;
        closure.kind = LayoutKind::Closure;
        closure.isAllocated = true;
        m_program.layouts.append(WTF::move(closure));

        auto& builtins = Builtins::singleton();
        m_program.preludeFunctions.fill(FunctionID(), numberOfPreludeFunctions);
        for (uint32_t i = 0; i < m_program.functions.size(); ++i) {
            auto& function = std::as_const(m_program).functions[i];
            if (!function.has(Format::FunctionIsBuiltin) || !function.name || function.blocks.isEmpty())
                continue;
            auto which = builtins.preludeFunctionNamed(m_program.string(function.name));
            if (!which)
                continue;
            RELEASE_ASSERT(function.kind == FunctionKind::Plain && function.captureTypes.isEmpty() && !function.parameterTypes.isEmpty());
            RELEASE_ASSERT(!function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis));
            m_program.preludeFunctions[static_cast<unsigned>(*which)] = FunctionID(i);
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
                m_program.integersYieldedByBuiltins.append(TypeID());
                continue;
            }
            Type yielded = Type::integer({ result.least, result.most });
            for (auto [kind, primitive] : primitives) {
                if (result.kinds & kind)
                    yielded.primitives |= primitive;
            }
            m_program.integersYieldedByBuiltins.append(m_program.types.intern(yielded));
        }
    }

private:
    // `Operation "Initialize" this, "name", value, ..` stands for `this.name = value; ..`. From here on it is that: stores by name, which every pass
    // knows, each with its place in the list. See initialStoresOf().
    void expandInitialize()
    {
        if (!m_program.usesOperation(Operations::Initialize))
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
            for (auto& block : function.blocks) {
                for (size_t at = 0; at < block.values.size(); ++at) {
                    if (function.at(block.values[at]).opcode != Opcode::Operation || m_program.operationOf(function.at(block.values[at])) != Operations::Initialize)
                        continue;
                    Value operation = function.at(block.values[at]);
                    RELEASE_ASSERT(operation.arguments.size() % 2);
                    // Where nothing goes by a type there is nothing to lower together. Nor where a name is no field of the type of `this`, or may be inherited: then these
                    // are stores like any other, all of them. The front end counts on none of them there.
                    auto& receiver = m_program.typeOf(function, operation.arguments[0]);
                    bool areInitialStores = !Options::auditAOTTypedAssertions();
                    for (size_t i = 1; i < operation.arguments.size(); i += 2) {
                        RELEASE_ASSERT(function.at(operation.arguments[i]).opcode == Opcode::String);
                        StringID name = function.at(operation.arguments[i]).string();
                        areInitialStores = areInitialStores && fieldOf(m_program, receiver, name) && !inheritsByItsLayout(m_program, receiver.onlyView(), name);
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
                    // Nothing uses what it yields.
                    auto& leftOver = function.at(block.values[at]);
                    leftOver.opcode = Opcode::Undefined;
                    leftOver.arguments = { };
                    leftOver.immediates = { Format::none, Format::none };
                    leftOver.setAssertedType({ });
                    block.values.removeAt(at);
                    block.values.insertVector(at, stores);
                    at += stores.size() - 1;
                }
            }
        });
    }

    // `[e1, .., en]` is `NewArray v1, .., vn`, which keeps every element until the last is made: of a table of n records n values are alive at once: n slots of the frame that all interfere, which the
    // allocation of the stack is quadratic in, and n roots. From here on a long one is made first, at its full length and with nothing but holes, which is an array like any other to the collector at
    // every point, and each element is stored as soon as it is whole. Nobody can tell: the expression of an element cannot name the array, and that something is allocated earlier is not observable.
    // Within the block of the literal: what is allocated there is allocated whenever the literal is reached. A literal of constants stays: it is a buffer that is shared until it is written to, see
    // Placement::placeArraySites(). So does one of numbers: it keeps them as numbers from its birth, which a store without a look cannot see to: what is not a number would read as a hole. So does one that a
    // handler waits for: it has to end its block. That a NewArray which does not end its block stands under no handler, nor anything in front of it there, holds of the blocks as the front end writes them: this
    // comes ahead of whatever takes an Invoke away or merges blocks.
    void makeTextsOfLargeLiterals()
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
        for (unsigned indexOfFunction = 0; indexOfFunction < m_program.functions.size(); ++indexOfFunction) {
            if (!hasLargeBlock[indexOfFunction])
                continue;
            auto& function = m_program.functions[indexOfFunction];
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
                auto& valuesOfBlock = function.blocks[b].values;
                bool isWatchedAtEnd = function.blocks[b].terminator.kind == TerminatorKind::Invoke;
                for (uint32_t at = 0; at < valuesOfBlock.size(); ++at) {
                    ValueID id = valuesOfBlock[at];
                    auto& value = std::as_const(function).at(id);
                    auto& node = nodes[id.index()];
                    bool isWatched = isWatchedAtEnd && at + 1 == valuesOfBlock.size();
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
                for (ValueID id : valuesOfBlock)
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
                UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> textOfRoot;
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
                    textOfRoot.add(id.index(), textID);
                    auto& root = function.at(id);
                    root.opcode = Opcode::Operation;
                    root.children = { };
                    root.immediates = { Operations::ParseJSON.index(), Format::none };
                    root.arguments = Arguments { textID };
                }
                if (textOfRoot.isEmpty())
                    continue;
                ValuesOfBlock kept;
                for (ValueID id : function.blocks[b].values) {
                    if (isRemoved.quickGet(id.index()) || (nodes[id.index()].kind == Kind::Leaf && !uses[id.index()]))
                        continue;
                    if (auto found = textOfRoot.find(id.index()); found != textOfRoot.end())
                        kept.append(found->value);
                    kept.append(id);
                }
                function.blocks[b].values = WTF::move(kept);
            }
        }
    }

    void fillLongLiterals()
    {
        constexpr unsigned maxElementsMadeAtOnce = 32;
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
            hasLongLiteral[i] = std::as_const(m_program).functions[i].values.containsIf([&](auto& value) {
                return value.opcode == Opcode::NewArray && value.arguments.size() > maxElementsMadeAtOnce;
            });
        });
        for (unsigned indexOfFunction = 0; indexOfFunction < m_program.functions.size(); ++indexOfFunction) {
            if (!hasLongLiteral[indexOfFunction])
                continue;
            auto& function = m_program.functions[indexOfFunction];
            // By value: where it stands in the block that is looked at.
            Vector<uint32_t> indexInBlock;
            for (auto& block : function.blocks) {
                Vector<uint32_t, 4> literals;
                for (uint32_t at = 0; at < block.values.size(); ++at) {
                    auto& value = function.at(block.values[at]);
                    if (value.opcode != Opcode::NewArray || value.arguments.size() <= maxElementsMadeAtOnce)
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
                // In front of which value of the block each is made: the first of its elements that is made in the block. An element that is such a literal itself is made where that is made.
                UncheckedKeyHashMap<uint32_t, Vector<ValueID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> madeInFrontOf;
                // By element: the literals it is an element of, and its places there.
                UncheckedKeyHashMap<uint32_t, Vector<std::pair<ValueID, uint32_t>, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> places;
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
                    madeInFrontOf.add(first, Vector<ValueID, 1>()).iterator->value.append(id);
                    for (uint32_t k = 0; k < elements.size(); ++k)
                        places.add(elements[k].index(), Vector<std::pair<ValueID, uint32_t>, 1>()).iterator->value.append({ id, k });
                }
                if (madeInFrontOf.isEmpty())
                    continue;

                ValuesOfBlock values;
                // What is made in another block was made before.
                BitVector isMade(numberOfValuesBefore);
                for (uint32_t i = 0; i < numberOfValuesBefore; ++i) {
                    if (indexInBlock[i] == nowhere)
                        isMade.set(i);
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
                auto store = [&](ValueID literal, uint32_t place, ValueID element) {
                    ValueID index = addInteger(place, literal);
                    Value operation;
                    operation.opcode = Opcode::Operation;
                    operation.type = m_program.types.undefined;
                    operation.immediates = { Operations::PutContiguousIndex.index(), Format::none };
                    operation.arguments = Arguments { literal, index, element };
                    add(WTF::move(operation), literal);
                };
                // What makes an element may go on behind the value that stands for it: `NewObject`, then its members, which are carried out as one as long as nothing between them uses the object. It is whole
                // where the next element is made, and the last one where the literal stood. By literal: the elements that wait for that.
                UncheckedKeyHashMap<uint32_t, Vector<std::pair<uint32_t, ValueID>, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> waiting;
                auto storeWaiting = [&](ValueID literal) {
                    auto found = waiting.find(literal.index());
                    if (found == waiting.end())
                        return;
                    for (auto [place, element] : std::exchange(found->value, { }))
                        store(literal, place, element);
                };
                auto willMake = [&](ValueID id) {
                    auto found = places.find(id.index());
                    if (found == places.end())
                        return;
                    for (auto [literal, place] : found->value) {
                        if (isMade.get(literal.index()))
                            storeWaiting(literal);
                    }
                };
                // Whichever of a literal and its element is made second has the element wait.
                auto didMake = [&](ValueID id) {
                    isMade.set(id.index());
                    auto found = places.find(id.index());
                    if (found == places.end())
                        return;
                    for (auto [literal, place] : found->value) {
                        if (isMade.get(literal.index()))
                            waiting.add(literal.index(), Vector<std::pair<uint32_t, ValueID>, 2>()).iterator->value.append({ place, id });
                    }
                };
                auto make = [&](ValueID id) {
                    willMake(id);
                    Arguments elements = std::exchange(function.at(id).arguments, { });
                    ValueID length = addInteger(elements.size(), id);
                    auto& literal = function.at(id);
                    literal.opcode = Opcode::Operation;
                    literal.immediates = { Operations::NewContiguousArray.index(), Format::none };
                    literal.arguments = Arguments { length };
                    values.append(id);
                    auto& early = waiting.add(id.index(), Vector<std::pair<uint32_t, ValueID>, 2>()).iterator->value;
                    for (uint32_t k = 0; k < elements.size(); ++k) {
                        if (isMade.get(elements[k].index()))
                            early.append({ k, elements[k] });
                    }
                    didMake(id);
                };
                for (uint32_t at = 0; at < block.values.size(); ++at) {
                    // The outer ones first: they come later in the block.
                    if (auto found = madeInFrontOf.find(at); found != madeInFrontOf.end()) {
                        for (ValueID id : found->value | std::views::reverse)
                            make(id);
                    }
                    ValueID id = block.values[at];
                    if (isMoved.get(id.index())) {
                        storeWaiting(id);
                        Value operation;
                        operation.opcode = Opcode::Operation;
                        operation.type = function.at(id).type;
                        operation.immediates = { Operations::TruncateNewArray.index(), Format::none };
                        operation.arguments = Arguments { id, function.at(id).arguments[0] };
                        add(WTF::move(operation), id);
                        continue;
                    }
                    willMake(id);
                    values.append(id);
                    didMake(id);
                }
                block.values = WTF::move(values);
            }
        }
    }

    // `for (key in o)` is `keys = Operation "ForInKeys" o`, and at the turn of each key, which is read from that list, `Operation
    // "HasEnumerableProperty" o, key`. From here on the second names the list, as a third operand: whatever the passes make of the values, the
    // lowering knows which listing a test comes after. See Lowering::lowerForInKeys().
    void markFunctionsUsedAsObjects()
    {
        auto prototype = m_program.find(asBytes("prototype"_s.span8()));
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
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
                    used = m_program.variables[definition.variable().index()].onlyFunction;
                    break;
                case Opcode::GetField:
                    used = m_program.at(definition.layout()).fields[definition.field()].onlyFunction;
                    break;
                default:
                    break;
                }
                for (FunctionID each : { used, predicted, definition.predictedClosure() }) {
                    if (each)
                        WTF::atomicStore(&m_program.at(each).isUsedAsObject, true, std::memory_order_relaxed);
                }
            };
            bool isCalleeOfItsArguments = false;
            for (auto& value : function.values) {
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
                        isCalleeOfItsArguments = true;
                        note(value.arguments[1]);
                    } else if (m_program.operationOf(value) == Operations::MakeClass)
                        note(value.arguments[1]);
                    break;
                default:
                    break;
                }
            }
            if (isCalleeOfItsArguments)
                WTF::atomicStore(&function.isUsedAsObject, true, std::memory_order_relaxed);
        });
    }

    void linkForInKeys()
    {
        if (!m_program.usesOperation(Operations::ForInKeys) || !m_program.usesOperation(Operations::HasEnumerableProperty))
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
            auto originOf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            for (auto& value : function.values) {
                if (value.opcode != Opcode::Operation || m_program.operationOf(value) != Operations::HasEnumerableProperty || value.arguments.size() != 2)
                    continue;
                auto& key = function.at(originOf(value.arguments[1]));
                if (key.opcode != Opcode::GetByVal)
                    continue;
                ValueID keys = originOf(key.child(0));
                auto& listing = function.at(keys);
                if (listing.opcode == Opcode::Operation && m_program.operationOf(listing) == Operations::ForInKeys && listing.arguments[0] == value.arguments[0])
                    value.arguments.append(keys);
            }
        });
    }

    Program& m_program;
};

static void bearFieldsThatConstructorsStoreFirst(Program& program)
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
        if (layout.kind != LayoutKind::Instance || layout.base || isExtended.get(number) || !layout.isLaidOut || layout.admitsForeignObjects || layout.mayGiveUp || !layout.constructor)
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
            if (!(layout.fields[index].flags & (fieldIsAbsentAtBirth | fieldIsNeverStored)))
                last = index + 1;
        }
        BitVector holdsSelf;
        Vector<uint32_t, 16> born;
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
                if (!(field.flags & fieldIsAbsentAtBirth) || born.contains(*named))
                    continue;
                if (*named < last || program.prototypePropertyNames.get(value.string().index()) || !field.type || !(program.at(field.type).primitives & Primitive::Undefined))
                    break;
                last = *named + 1;
                born.append(*named);
                continue;
            }
            bool mayLetSelfOut = false;
            value.forEachChild([&](ValueID child) {
                mayLetSelfOut |= child && (isSelf(child) || holdsSelf.get(child.index()));
            });
            if (mayLetSelfOut)
                break;
        }
        for (uint32_t index : born)
            layout.fields[index].flags &= ~fieldIsAbsentAtBirth;
        numberOfFields += born.size();
        numberOfClasses += !born.isEmpty();
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfFields, " fields of ", numberOfClasses, " classes are there from birth because the constructor stores to them before anybody can look");
}

static void findNamesOfClosuresMadeWhenRead(Program& program)
{
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
        for (auto& value : function.values) {
            if ((value.opcode != Opcode::PutById && value.opcode != Opcode::PutField) || function.at(value.child(1)).opcode != Opcode::NewClosure || function.at(value.child(1)).arguments.size() > 1)
                continue;
            ValueID holder = withoutUpcasts(value.child(0));
            if (holder != self && function.at(holder).opcode != Opcode::NewObjectWithShape)
                continue;
            if (auto& captured = function.at(value.child(1)).arguments; !captured.isEmpty() && withoutUpcasts(captured[0]) != holder)
                continue;
            found[number].append((value.opcode == Opcode::PutById ? value.string() : program.at(value.layout()).fields[value.field()].name).index());
        }
    });
    for (auto& names : found) {
        for (uint32_t name : names)
            program.namesOfClosuresMadeWhenRead.set(name);
    }
}

void prepare(Program& program)
{
    Preparation(program).run();
    findNamesOfClosuresMadeWhenRead(program);
}

static LayoutID layoutExpectedOfCopy(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin || !value.prediction())
        return { };
    auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Operation || (implementation.operation != LanguageOperation::CloneObject && implementation.operation != LanguageOperation::CloneObjectWith && implementation.operation != LanguageOperation::CloneObjectAfter && implementation.operation != LanguageOperation::CopyDataPropertiesExcluding))
        return { };
    LayoutID view = program.at(value.prediction()).onlyView();
    return view && program.at(view).kind == LayoutKind::Record ? view : LayoutID();
}

static bool holdsFreshObjectToRecords(const Function& function, const Value& value)
{
    return value.opcode == Opcode::Cast && value.castReason() == CastReason::As && function.at(value.child(0)).opcode == Opcode::NewObject;
}

void linkAllocations(Program& program)
{
    UncheckedKeyHashMap<unsigned, Vector<uint32_t, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> byHash;
    auto numberOf = [&](const Allocation& allocation) -> uint32_t {
        Hasher hasher;
        add(hasher, allocation.layout, allocation.constructedBy, allocation.born, allocation.numberOfCaptures);
        for (uint16_t field : allocation.order)
            add(hasher, field);
        auto& candidates = byHash.add(hasher.hash(), Vector<uint32_t, 1>()).iterator->value;
        for (uint32_t candidate : candidates) {
            if (program.allocations[candidate] == allocation)
                return candidate;
        }
        candidates.append(program.allocations.size());
        program.allocations.append(allocation);
        return candidates.last();
    };
    // Closures, which no value names the layout of. Number 0: with room for one capture.
    auto needClosureWith = [&](unsigned numberOfCaptures) {
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
    needClosureWith(0);
    Vector<Vector<ValueID>> births(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet mayBeBirths { Opcode::NewClosure, Opcode::NewClosureOnce, Opcode::NewObjectWithShape, Opcode::CallBuiltin, Opcode::Cast };
        std::as_const(program).functions.forEachValueInOrder(i, mayBeBirths, [&](ValueID id) {
            Opcode opcode = function.at(id).opcode;
            if (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::NewObjectWithShape || layoutExpectedOfCopy(program, function.at(id)) || holdsFreshObjectToRecords(function, function.at(id)))
                births[i].append(id);
        });
    });
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (births[i].isEmpty())
            continue;
        auto& function = program.functions.withSameValues(i);
        for (ValueID id : births[i]) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::CallBuiltin || value.opcode == Opcode::Cast)
                continue;
            if (value.opcode != Opcode::NewObjectWithShape) {
                needClosureWith(value.arguments.size());
                continue;
            }
            auto& allocation = function.allocations[value.allocation()];
            program.at(allocation.layout).isAllocated = true;
            value.immediates[1] = numberOf(allocation);
        }
    }
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        for (ValueID id : births[i]) {
            if (LayoutID view = layoutExpectedOfCopy(program, std::as_const(program).functions[i].at(id)); view && program.at(view).isAllocated)
                numberOf(Allocation { view, { }, { }, 0 });
            if (auto& value = std::as_const(program).functions[i].at(id); value.opcode == Opcode::Cast) {
                for (LayoutID view : program.at(value.type).views) {
                    if (program.at(view).kind == LayoutKind::Record && program.at(view).isAllocated)
                        numberOf(Allocation { view, { }, { }, 0 });
                }
            }
        }
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        program.functions.withSameValues(i).allocations.clear();
    });
    // `new (F as any)()`, Reflect.construct(F, ..), a bound F: whoever constructs does not know the function, and leaves the object to the
    // run time. It is born with nothing, like the one `new` makes for a function it knows, and gets its prototype when it is made.
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions.withSameValues(i);
        TypeID thisType = function.assertedThisType ? function.assertedThisType : function.thisType;
        if (function.kind != FunctionKind::Plain || function.declaredAs & (functionIsAsync | functionIsGenerator) || !thisType || !program.genericEntrypoints.contains(i))
            continue;
        LayoutID view = program.at(thisType).onlyView();
        if (view && program.at(view).kind == LayoutKind::Record && program.at(view).isAllocated)
            function.allocationOfThis = numberOf(Allocation { view, { }, { }, 0 });
    }
    for (uint32_t i = 0; i < program.allocations.size(); ++i) {
        auto& allocation = program.allocations[i];
        auto& layout = program.at(allocation.layout);
        if (layout.kind == LayoutKind::Record && !allocation.constructedBy && allocation.discriminant == UINT16_MAX && layout.allocationOfClone == Format::none)
            layout.allocationOfClone = i;
    }
}

static Vector<unsigned> blocksResumedAt(const Function& function)
{
    Vector<unsigned> result;
    if (function.suspensionPositions.isEmpty() || function.blocks.isEmpty())
        return result;
    auto& dispatch = function.blocks[0].terminator;
    if (dispatch.kind != TerminatorKind::Switch || function.blocks[0].parameters.isEmpty())
        return result;
    ValueID frame = function.blocks[0].parameters[0];
    auto isStateOfFrame = [&](const Value& value) {
        return value.child(0) == frame && value.field() == CoroutineFrame::stateField;
    };
    if (function.at(dispatch.value).opcode != Opcode::GetField || !isStateOfFrame(function.at(dispatch.value)))
        return result;
    result.fill(UINT_MAX, function.blocks.size());
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        for (ValueID id : function.blocks[b].values) {
            auto& store = function.at(id);
            if (store.opcode != Opcode::PutField || !isStateOfFrame(store) || function.at(store.child(1)).opcode != Opcode::Integer)
                continue;
            size_t index = dispatch.cases.find(function.at(store.child(1)).integer());
            if (index != notFound)
                result[b] = dispatch.targets[index].block.index();
        }
    }
    return result;
}

Vector<unsigned> cycleOfEachBlock(const Function& function, Vector<unsigned>* rankOfComponent)
{
    constexpr unsigned notVisited = UINT_MAX;
    unsigned numberOfBlocks = function.blocks.size();
    Vector<unsigned> result;
    result.fill(isInNoCycle, numberOfBlocks);
    unsigned numberOfComponents = 0;
    if (rankOfComponent)
        rankOfComponent->fill(0, numberOfBlocks);
    BitVector hasEdgeToItself(numberOfBlocks);
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
                    hasEdgeToItself.set(block);
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
            bool isCycle = componentStack.last() != block || hasEdgeToItself.get(block);
            for (unsigned member = notVisited; member != block;) {
                member = componentStack.takeLast();
                isInComponentStack.clear(member);
                if (isCycle)
                    result[member] = block;
                if (rankOfComponent)
                    rankOfComponent->at(member) = numberOfComponents;
            }
            ++numberOfComponents;
        }
    }
    return result;
}

Vector<unsigned> cycleOfEachBlock(const Function& function)
{
    return cycleOfEachBlock(function, nullptr);
}

// The blocks that a block with a LoopHint reaches and that reach it.
BitVector blocksInLoops(const Function& function)
{
    unsigned numberOfBlocks = function.blocks.size();
    BitVector result(numberOfBlocks);
    Vector<unsigned, 8> headers;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        bool isHeader = function.blocks[b].values.containsIf([&](ValueID id) {
            return function.at(id).opcode == Opcode::LoopHint;
        });
        if (isHeader)
            headers.append(b);
    }
    if (headers.isEmpty())
        return result;
    Vector<unsigned> cycles = cycleOfEachBlock(function);
    BitVector holdsHeader(numberOfBlocks);
    for (unsigned header : headers) {
        if (cycles[header] != isInNoCycle)
            holdsHeader.set(cycles[header]);
    }
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        if (cycles[b] != isInNoCycle && holdsHeader.get(cycles[b]))
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
    UncheckedKeyHashMap<uint32_t, Vector<unsigned, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> blocksOfUsers;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        for (ValueID id : block.values) {
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                if (definingBlock[child.index()] == none)
                    return;
                auto& blocks = blocksOfUsers.add(child.index(), Vector<unsigned, 2>()).iterator->value;
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
    Vector<unsigned> rankOfComponent;
    Vector<uint32_t> isReachedFor;
    for (auto& [reference, blocks] : blocksOfUsers) {
        if (blocks.size() < 2 || stays.get(reference))
            continue;
        if (rankOfComponent.isEmpty()) {
            cycleOfEachBlock(function, &rankOfComponent);
            isReachedFor.fill(none, function.blocks.size());
        }
        unsigned leastRank = UINT_MAX;
        Vector<unsigned, 16> worklist;
        for (unsigned b : blocks) {
            leastRank = std::min(leastRank, rankOfComponent[b]);
            worklist.append(b);
        }
        while (!worklist.isEmpty()) {
            for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned to = target.block.index();
                if (rankOfComponent[to] < leastRank || isReachedFor[to] == reference)
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
        ValuesOfBlock values;
        size_t birth = notFound;
        for (ValueID id : function.blocks[b].values) {
            if (id.index() < numberOfValues && definingBlock[id.index()] == b && blocksOfUsers.contains(id.index()) && !stays.get(id.index()))
                continue;
            Vector<ValueID, 2> wanted;
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                if (child.index() < numberOfValues && definingBlock[child.index()] != none && !stays.get(child.index()) && !copies.contains(child.index()) && !wanted.contains(child))
                    wanted.append(child);
            });
            Opcode opcode = function.at(id).opcode;
            bool storesToNewObject = birth != notFound && (opcode == Opcode::PutField || opcode == Opcode::PutById) && function.at(id).child(0) == values[birth] && function.at(id).child(1) != values[birth];
            for (ValueID original : wanted) {
                ValueID copy(function.values.size());
                function.values.append(Value(function.at(original)));
                copies.add(original.index(), copy);
                if (storesToNewObject)
                    values.insert(birth++, copy);
                else
                    values.append(copy);
            }
            function.at(id).forEachChild([&](ValueID& child) {
                if (auto copy = copies.get(child.index()))
                    child = copy;
            });
            values.append(id);
            if (opcode == Opcode::NewObjectWithShape)
                birth = values.size() - 1;
            else if (!storesToNewObject)
                birth = notFound;
        }
        function.blocks[b].values = WTF::move(values);
    }
}

static void findFunctionsInCycles(Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<unsigned>> callees(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::CallDirect)
                callees[i].append(value.function().index());
        }
    });
    struct Frame {
        unsigned function;
        unsigned next;
    };
    BitVector result(count);
    BitVector isOnStack(count);
    Vector<unsigned> index;
    index.fill(0, count);
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
            bool isAlone = stack.last() == function;
            for (;;) {
                unsigned member = stack.takeLast();
                isOnStack.clear(member);
                if (!isAlone)
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
}

// Where a program spends its time, as far as its text says: in a function that has a loop, that a loop calls by name, or that such a function calls by name or makes, two levels
// down; and in what any of these makes. Of the program as it was read.
static void findFunctionsHotByText(Program& program)
{
    findFunctionsInCycles(program);
    unsigned count = program.functions.size();
    auto forEachFunctionNamed = [&](const Function& function, const BitVector* blocks, bool alsoMade, auto&& functor) {
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (blocks && !blocks->get(b))
                continue;
            for (ValueID id : function.blocks[b].values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::CallDirect || (alsoMade && (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::FunctionReference)))
                    functor(function.at(id).function().index());
            }
        }
    };
    BitVector result(count);
    BitVector isCalled(count);
    Vector<unsigned> frontier;
    // By function with a loop: what its loops call by name.
    Vector<std::optional<Vector<unsigned>>> calledInLoops(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        BitVector isInLoop = blocksInLoops(std::as_const(program).functions[i]);
        bool isInCycle = program.functionsInCycles.get(i);
        if (isInLoop.isEmpty() && !isInCycle)
            return;
        calledInLoops[i] = Vector<unsigned>();
        forEachFunctionNamed(std::as_const(program).functions[i], isInCycle ? nullptr : &isInLoop, false, [&](unsigned callee) {
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
    static constexpr ASCIILiteral runtimeModulesThatRunForEveryRequest[] = {
        "node:events"_s, "node:_http_common"_s, "node:_http_incoming"_s, "node:_http_outgoing"_s, "node:_http_server"_s, "internal:http"_s,
        "internal:streams/readable"_s, "internal:streams/state"_s, "internal:streams/writable"_s, "internal:process/next_tick_queue"_s, "internal:validators"_s,
    };
    BitVector runsForEveryRequest(program.modules.size());
    for (auto& [number, code] : program.builtinModules) {
        ModuleID module = code ? std::as_const(program).at(code).module : ModuleID();
        if (!module)
            continue;
        auto path = program.string(program.modules[module.index()].path);
        if (std::ranges::any_of(runtimeModulesThatRunForEveryRequest, [&](ASCIILiteral name) { return equalSpans(path, name.span8()); }))
            runsForEveryRequest.set(module.index());
    }
    if (!runsForEveryRequest.isEmpty()) {
        BitVector isCodeOfModule(count);
        for (auto& [number, code] : program.builtinModules) {
            if (code)
                isCodeOfModule.set(code.index());
        }
        for (unsigned i = 0; i < count; ++i) {
            auto& function = std::as_const(program).functions[i];
            if (function.module && runsForEveryRequest.get(function.module.index()) && !function.isGenericEntrypoint() && !isCodeOfModule.get(i) && !isCalled.set(i))
                frontier.append(i);
        }
    }
    BitVector hasLoop = result;
    isCalled.merge(program.functionsInCycles);
    for (unsigned i = 0; i < count; ++i) {
        if (std::as_const(program).functions[i].savesItsFrame)
            isCalled.clear(i);
    }
    BitVector isCalledOrMade = isCalled;
    BitVector isCalledNearLoops = isCalled;
    auto say = [](Vector<uint8_t>& said, unsigned index) {
        WTF::atomicStore(&said[index], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    auto forEachSaid = [&](const Vector<uint8_t>& said, auto&& functor) {
        for (unsigned i = 0; i < count; ++i) {
            if (said[i])
                functor(i);
        }
    };
    Vector<uint8_t> isNamedByFrontier(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> isCalledByFrontier(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(frontier.size(), [&](unsigned k) {
        auto& function = std::as_const(program).functions[frontier[k]];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::CallDirect)
                    say(isCalledByFrontier, function.at(id).function().index());
                else if (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::FunctionReference)
                    say(isNamedByFrontier, function.at(id).function().index());
            }
        }
    });
    forEachSaid(isNamedByFrontier, [&](unsigned callee) {
        isCalledOrMade.set(callee);
    });
    forEachSaid(isCalledByFrontier, [&](unsigned callee) {
        isCalledOrMade.set(callee);
        isCalledNearLoops.set(callee);
    });
    unsigned forced = Options::aotHotByTextForTesting();
    if (forced == 1) [[unlikely]] {
        isCalled.clearAll();
        isCalledOrMade.clearAll();
        isCalledNearLoops.clearAll();
    }
    if (forced == 2) [[unlikely]] {
        for (unsigned i = 0; i < count; ++i) {
            isCalled.set(i);
            isCalledOrMade.set(i);
            isCalledNearLoops.set(i);
        }
    }
    BitVector warm = result;
    warm.merge(isCalledOrMade);
    result.merge(isCalled);
    BitVector made(count);
    Vector<uint8_t> isMadeByWarm(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!warm.get(i))
            return;
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference)
                say(isMadeByWarm, value.function().index());
        }
    });
    forEachSaid(isMadeByWarm, [&](unsigned callee) {
        made.set(callee);
    });
    if (forced == 1) [[unlikely]]
        made.clearAll();
    warm.merge(made);
    hasLoop.exclude(isCalled);
    program.functionsHotOnlyInTheirLoops = WTF::move(hasLoop);
    Vector<uint8_t> isCalledWhereHiddenCodeCatches(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (!function.has(Format::FunctionIsHiddenInStackTraces) || function.isGenericEntrypoint())
            return;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Invoke)
                continue;
            for (ValueID id : function.at(block.terminator.targets[1].block).values) {
                if (function.at(id).opcode == Opcode::CallDirect)
                    say(isCalledWhereHiddenCodeCatches, function.at(id).function().index());
            }
        }
    });
    forEachSaid(isCalledWhereHiddenCodeCatches, [&](unsigned callee) {
        result.clear(callee);
        warm.clear(callee);
        isCalledNearLoops.clear(callee);
    });
    // A general entry does what its function does.
    for (auto [function, entry] : program.genericEntrypoints) {
        if (result.get(function))
            result.set(entry.index());
        if (warm.get(function))
            warm.set(entry.index());
        if (isCalledNearLoops.get(function))
            isCalledNearLoops.set(entry.index());
    }
    program.functionsHotByText = WTF::move(result);
    program.functionsWarmByText = WTF::move(warm);
    program.functionsCalledNearLoops = WTF::move(isCalledNearLoops);
}

static void findFunctionsRareByText(Program& program)
{
    unsigned count = program.functions.size();
    Vector<unsigned> timesMadeAValue;
    timesMadeAValue.fill(0, count);
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : program.functions[i].values) {
            if (value.predictedClosure())
                WTF::atomicExchangeAdd(&timesMadeAValue[value.predictedClosure().index()], 1u, std::memory_order_relaxed);
            if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                WTF::atomicExchangeAdd(&timesMadeAValue[value.function().index()], 1u, std::memory_order_relaxed);
        }
    });
    BitVector isMadeAValue(count);
    for (unsigned i = 0; i < count; ++i) {
        if (timesMadeAValue[i])
            isMadeAValue.set(i);
    }
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isMadeAValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isMadeAValue.set(method.function.index());
        }
    }
    for (auto& module : program.builtinModules)
        isMadeAValue.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isMadeAValue.set(builtin.function.index());
    for (auto [index, entry] : program.genericEntrypoints) {
        isMadeAValue.set(index);
        isMadeAValue.set(entry.index());
    }
    auto mayBeRare = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        return isPlain && !isMadeAValue.get(i) && !function.isGenericEntrypoint() && !function.has(Format::FunctionIsBuiltin) && !program.isResumeFunction(function) && !program.isEntry(FunctionID(i)) && !function.resumeFunction && !function.startFunction;
    };
    Vector<uint32_t> callsInRareCode;
    callsInRareCode.fill(0, count);
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if ((function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i))) {
            program.functionsRareByText.set(i);
            worklist.append(i);
        }
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
                if (++callsInRareCode[callee] == program.numberOfDirectCalls[callee] && !program.functionsRareByText.set(callee))
                    worklist.append(callee);
            }
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.functionsRareByText.bitCount(), " of ", count, " functions are rare by the text");
}

// A concatenation makes a string: in place that is a few hundred bytes at every site. Where the text does not say hot there is one body for each signature, and the sites call it.
static bool isInCopyOfHotOriginal(const Program& program, const Function& function, const Value& value, bool asksForLoopsNearby)
{
    if (!value.inlineFrame)
        return false;
    FunctionID original = function.inlineFrames[value.inlineFrame].callee;
    return program.isHotByText(original) || (asksForLoopsNearby && program.isCalledNearLoops(original));
}

static void printSitesOfPassForOneBody(ASCIILiteral what, size_t bodies, std::initializer_list<std::pair<ASCIILiteral, uint64_t>> rows)
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

static bool isMadeOfNumbersAndConstants(const Program& program, const Function& function, ValueID id, unsigned& valuesLeftToLookAt)
{
    if (!valuesLeftToLookAt)
        return false;
    --valuesLeftToLookAt;
    auto& value = function.at(id);
    switch (value.opcode) {
    case Opcode::String:
        return true;
    case Opcode::ToString:
        return isMadeOfNumbersAndConstants(program, function, value.child(0), valuesLeftToLookAt);
    case Opcode::Add:
        if (program.at(value.type).isString())
            return isMadeOfNumbersAndConstants(program, function, value.child(0), valuesLeftToLookAt) && isMadeOfNumbersAndConstants(program, function, value.child(1), valuesLeftToLookAt);
        break;
    case Opcode::Strcat:
        return std::ranges::all_of(value.arguments, [&](ValueID part) {
            return isMadeOfNumbersAndConstants(program, function, part, valuesLeftToLookAt);
        });
    default:
        break;
    }
    auto& type = program.typeOf(function, id);
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
}

static bool isMadeOfNumbersAndConstants(const Program& program, const Function& function, ValueID id)
{
    unsigned valuesLeftToLookAt = 64;
    return isMadeOfNumbersAndConstants(program, function, id, valuesLeftToLookAt);
}

static void giveConcatenationsOneBody(Program& program)
{
    constexpr unsigned maxOperands = 6;
    unsigned count = program.functions.size();
    struct Site {
        ValueID value;
        String signature;
        BitVector isMadeStringInBody;
        String signatureOfStrings;
        uint32_t body { 0 };
    };
    Vector<Vector<Site>> sites(count);
    static constexpr OpcodeSet concatenations { Opcode::Strcat };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayHold(i, concatenations))
            return;
        auto& function = std::as_const(program).functions[i];
        if ((program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i))) || function.blocks.isEmpty())
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
                if (isInCopyOfHotOriginal(program, function, value, false))
                    continue;
                if (!builders)
                    builders = analyzeStringBuilders(program, FunctionID(i), function);
                if (builders->isState(id))
                    continue;
                if (isMadeOfNumbersAndConstants(program, function, id)) {
                    if (!keys)
                        keys = computeMapKeyValues(program, FunctionID(i), function);
                    if (keys->get(id.index()))
                        continue;
                }
                StringBuilder signature;
                StringBuilder signatureOfStrings;
                signature.append(value.type.index());
                signatureOfStrings.append(value.type.index());
                BitVector isMadeStringInBody;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    auto& argument = function.at(value.arguments[k]);
                    signatureOfStrings.append(' ', argument.type.index());
                    if (argument.opcode == Opcode::ToString && isListedIn(b, value.arguments[k]) && isUsedOnce(value.arguments[k])) {
                        auto& given = function.at(argument.child(0));
                        if (auto& type = program.at(given.type); !type.hasObjects() && !(type.primitives & Primitive::Symbol)) {
                            isMadeStringInBody.set(k);
                            signature.append(" s"_s, given.type.index());
                            continue;
                        }
                    }
                    signature.append(' ', argument.type.index());
                }
                sites[i].append({ id, signature.toString(), WTF::move(isMadeStringInBody), signatureOfStrings.toString() });
            }
        }
    });
    enum : unsigned { CallsBody, IsHotByText, HasTooManyParts, IsInLoop, IsCopiedFromHot, IsKey, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        bool isHot = program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i));
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Strcat)
                    ++numbers[isHot ? IsHotByText : value.arguments.size() > maxOperands ? HasTooManyParts : isInLoop.get(b) ? IsInLoop : isInCopyOfHotOriginal(program, function, value, false) ? IsCopiedFromHot : isMadeOfNumbersAndConstants(program, function, id) && computeMapKeyValues(program, FunctionID(i), function).get(id.index()) ? IsKey : CallsBody];
            }
        }
    }
    constexpr unsigned minSitesOfBodyThatMakesStrings = 3;
    UncheckedKeyHashMap<String, unsigned> sitesOfSignature;
    for (auto& ofFunction : sites) {
        for (auto& site : ofFunction) {
            if (!site.isMadeStringInBody.isEmpty())
                ++sitesOfSignature.add(site.signature, 0).iterator->value;
        }
    }
    for (auto& ofFunction : sites) {
        for (auto& site : ofFunction) {
            if (site.isMadeStringInBody.isEmpty() || sitesOfSignature.get(site.signature) >= minSitesOfBodyThatMakesStrings)
                continue;
            site.isMadeStringInBody.clearAll();
            site.signature = site.signatureOfStrings;
        }
    }
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> bodyOfSignature;
    for (unsigned i = 0; i < count; ++i) {
        if (sites[i].isEmpty())
            continue;
        auto& function = std::as_const(program).functions[i];
        for (auto& site : sites[i]) {
            auto& value = function.at(site.value);
            site.body = bodyOfSignature.ensure(site.signature, [&] {
                Function made;
                made.name = program.emptyString;
                made.module = function.module;
                made.position = function.position;
                made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
                made.isBodyOfSeveralSites = true;
                made.length = value.arguments.size();
                made.resultType = value.type;
                made.blocks.append(Block { });
                Value concatenation(value);
                concatenation.inlineFrame = 0;
                concatenation.position = function.position;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    bool isMadeString = site.isMadeStringInBody.get(k);
                    auto& argument = function.at(isMadeString ? function.at(value.arguments[k]).child(0) : value.arguments[k]);
                    Value parameter;
                    parameter.opcode = Opcode::Parameter;
                    parameter.position = function.position;
                    parameter.type = argument.type;
                    parameter.rep = argument.rep;
                    parameter.immediates = { 0, k };
                    made.parameterTypes.append(argument.type);
                    made.blocks[0].parameters.append(ValueID(made.values.size()));
                    concatenation.arguments[k] = ValueID(made.values.size());
                    made.values.append(WTF::move(parameter));
                }
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    if (!site.isMadeStringInBody.get(k))
                        continue;
                    Value conversion(function.at(value.arguments[k]));
                    conversion.inlineFrame = 0;
                    conversion.position = function.position;
                    conversion.children[0] = concatenation.arguments[k];
                    concatenation.arguments[k] = ValueID(made.values.size());
                    made.blocks[0].values.append(ValueID(made.values.size()));
                    made.values.append(WTF::move(conversion));
                }
                made.blocks[0].values.append(ValueID(made.values.size()));
                made.blocks[0].terminator.kind = TerminatorKind::Return;
                made.blocks[0].terminator.value = ValueID(made.values.size());
                made.values.append(WTF::move(concatenation));
                bodies.append(WTF::move(made));
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
            value.takePredictionOf(Value { });
            for (size_t k : site.isMadeStringInBody) {
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
        for (auto& ofFunction : sites)
            taken += ofFunction.size();
        RELEASE_ASSERT(numbers[CallsBody] == taken);
        printSitesOfPassForOneBody("concatenations"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "the function is hot by the text"_s, numbers[IsHotByText] }, { "more than six parts"_s, numbers[HasTooManyParts] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsCopiedFromHot] }, { "a key"_s, numbers[IsKey] } });
    }
    program.functions.appendVector(WTF::move(bodies));
}

// A call of a method or a static function of the engine is lowered in place, with its tests and its general way: a few hundred bytes at every site. Where the text does not say hot there is
// one body for each signature that two sites have, and the sites call it.
static void giveCallsOfBuiltinsOneBody(Program& program)
{
    constexpr unsigned maxOperands = 6;
    auto& builtins = Builtins::singleton();
    std::array<BuiltinID, 3> spreads { Operations::AppendSpread, Operations::Apply, Operations::CallWithSpread };
    auto isCandidate = [&](const Value& value) {
        // `s + t` makes a string as a concatenation does.
        if (value.opcode == Opcode::Add)
            return program.at(value.type).isString();
        if (value.opcode == Opcode::CallMethod && value.classExpectedOfReceiverPlusOne)
            return !value.isInTailPosition && value.arguments.size() + 1 + !!value.child(1) <= maxOperands;
        if (value.opcode != Opcode::CallBuiltin || value.child(2) || value.arguments.size() + !!value.child(0) + !!value.child(1) > maxOperands)
            return false;
        // Its site is small: the usual way in line, all else in a stub.
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush)
            return false;
        BuiltinKind kind = builtins.at(value.builtin()).kind;
        return kind == BuiltinKind::Method || kind == BuiltinKind::StaticFunction || (!value.isInTailPosition && std::ranges::find(spreads, value.builtin()) != spreads.end());
    };
    auto isReadOfMethodOfGuessedClass = [&](const Function& function, const Value& value) {
        return value.opcode == Opcode::GetById && value.readsMethodFor() && value.classExpectedOfReceiverPlusOne && !program.typeOf(function, value.child(0)).onlyEngineClass();
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
    auto makesObjectWithoutPrototype = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::Null)
            return false;
        auto& builtin = builtins.at(value.builtin());
        return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
    };
    auto makesItsFirstArgument = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() < 2 || function.at(value.arguments[0]).opcode != Opcode::NewObject)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign;
    };
    auto writesLiteralAsJSON = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || function.at(program.originOf(function, value.arguments[0])).opcode != Opcode::NewObject)
            return false;
        auto& implementation = builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::JSONStringify || implementation.inlined == InlineBuiltin::ResponseJSON);
    };
    auto isArithmetic = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& builtin = builtins.at(value.builtin());
        if (builtin.effects != Effect::Pure || builtin.implementation.kind != BuiltinImplementation::Inline)
            return false;
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
        uint32_t numberOfSignature { 0 };
    };
    Vector<Vector<Site>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (program.isCalledNearLoops(FunctionID(i)) || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        std::optional<BitVector> keys;
        std::optional<StringBuilderAnalysis> builders;
        auto isBuiltInOneBuffer = [&](ValueID id) {
            if (!builders)
                builders = analyzeStringBuilders(program, FunctionID(i), function);
            return builders->isState(id);
        };
        auto isKeyMadeHere = [&](ValueID key) {
            auto& made = function.at(key);
            if (made.opcode != Opcode::Add && (made.opcode != Opcode::Strcat || !isMadeOfNumbersAndConstants(program, function, key)))
                return false;
            if (!keys)
                keys = computeMapKeyValues(program, FunctionID(i), function);
            return keys->get(key.index());
        };
        bool hasHandlers = function.blocks.containsIf([](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke;
        });
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if ((!isCandidate(value) && !isReadOfMethodOfGuessedClass(function, value)) || isDescribedByLiteral(function, value))
                    continue;
                if (value.opcode == Opcode::Add ? isKeyMadeHere(id) || isBuiltInOneBuffer(id) : value.opcode == Opcode::CallBuiltin && !value.arguments.isEmpty() && isKeyMadeHere(value.arguments[0]))
                    continue;
                if (isArithmetic(function, value) || makesObjectWithoutPrototype(function, value) || makesItsFirstArgument(function, value) || writesLiteralAsJSON(function, value))
                    continue;
                if (hasHandlers && (function.blocks[b].terminator.kind != TerminatorKind::Invoke || id != function.blocks[b].values.last()))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    continue;
                if (isInCopyOfHotOriginal(program, function, value, true))
                    continue;
                StringBuilder signature;
                signature.append(static_cast<unsigned>(value.opcode), ' ', value.immediates[0], ' ', value.immediates[1], ' ', value.type.index(), ' ', value.prediction().index(), value.children[0] ? ' ' : '-');
                if (value.classExpectedOfReceiverPlusOne)
                    signature.append('e', value.classExpectedOfReceiverPlusOne);
                if (value.receiverHasOriginalStructure)
                    signature.append('m');
                if (value.child(1))
                    signature.append('r');
                forEachOperand(value, [&](ValueID operandID) {
                    auto& operand = function.at(operandID);
                    signature.append(' ', operand.type.index(), '~', operand.prediction().index(), '~', operand.predictedClosure().index());
                    if (staysInBody(operand))
                        signature.append('=', static_cast<unsigned>(operand.opcode), ':', operand.immediates[0], ':', operand.immediates[1]);
                });
                sites[i].append({ id, signature.toString() });
                sites[i].last().signature.impl()->hash();
            }
        }
    });
    struct Signature {
        uint32_t numberOfSites;
        uint32_t functionOfFirstSite;
        uint32_t firstSite;
        uint32_t body;
    };
    Vector<Signature> signatures;
    {
        UncheckedKeyHashMap<String, uint32_t> numberOfSignature;
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned k = 0; k < sites[i].size(); ++k) {
                auto result = numberOfSignature.add(sites[i][k].signature, static_cast<uint32_t>(signatures.size()));
                if (result.isNewEntry)
                    signatures.append(Signature { 0, i, k, 0 });
                sites[i][k].numberOfSignature = result.iterator->value;
                ++signatures[result.iterator->value].numberOfSites;
            }
        }
    }
    enum : unsigned { CallsBody, OccursOnce, IsNearLoops, IsGetterOrSetter, IsOperation, IsOfOtherKind, IsPush, HasTooManyOperands, IsDescribedByLiteral, IsArithmetic, IsBirth, IsBesideHandlers, IsInLoop, IsLeft, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        BitVector isSite;
        BitVector isSharedSite;
        for (auto& site : sites[i]) {
            isSite.set(site.value.index());
            if (signatures[site.numberOfSignature].numberOfSites >= 2)
                isSharedSite.set(site.value.index());
        }
        bool isNearLoops = program.isCalledNearLoops(FunctionID(i));
        bool hasHandlers = function.blocks.containsIf([](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke;
        });
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::CallBuiltin)
                    continue;
                BuiltinKind kind = builtins.at(value.builtin()).kind;
                auto& implementation = builtins.at(value.builtin()).implementation;
                unsigned reason = IsLeft;
                if (isSite.get(id.index()))
                    reason = isSharedSite.get(id.index()) ? CallsBody : OccursOnce;
                else if (isNearLoops)
                    reason = IsNearLoops;
                else if (!isCandidate(value)) {
                    if (value.child(2) || value.arguments.size() + !!value.child(0) + !!value.child(1) > maxOperands)
                        reason = HasTooManyOperands;
                    else if (kind == BuiltinKind::Getter || kind == BuiltinKind::Setter)
                        reason = IsGetterOrSetter;
                    else if (kind == BuiltinKind::Operation)
                        reason = IsOperation;
                    else if (kind != BuiltinKind::Method && kind != BuiltinKind::StaticFunction)
                        reason = IsOfOtherKind;
                    else {
                        RELEASE_ASSERT(implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush);
                        reason = IsPush;
                    }
                } else if (isDescribedByLiteral(function, value))
                    reason = IsDescribedByLiteral;
                else if (isArithmetic(function, value))
                    reason = IsArithmetic;
                else if (makesObjectWithoutPrototype(function, value) || makesItsFirstArgument(function, value))
                    reason = IsBirth;
                else if (hasHandlers && (function.blocks[b].terminator.kind != TerminatorKind::Invoke || id != function.blocks[b].values.last()))
                    reason = IsBesideHandlers;
                else if (isInLoop.get(b))
                    reason = IsInLoop;
                ++numbers[reason];
            }
        }
    }
    Vector<uint32_t> signaturesWithBody;
    for (uint32_t number = 0; number < signatures.size(); ++number) {
        auto& signature = signatures[number];
        if (signature.numberOfSites < 2)
            continue;
        signature.body = count + static_cast<uint32_t>(signaturesWithBody.size());
        signaturesWithBody.append(number);
    }
    Vector<Function> bodies(signaturesWithBody.size());
    forEachIndexInParallel(signaturesWithBody.size(), [&](unsigned k) {
        auto& signature = signatures[signaturesWithBody[k]];
        auto& function = std::as_const(program).functions[signature.functionOfFirstSite];
        auto& site = sites[signature.functionOfFirstSite][signature.firstSite];
        auto& value = function.at(site.value);
        Function made;
        made.name = program.emptyString;
        made.module = function.module;
        made.position = function.position;
        made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        made.isBodyOfSeveralSites = true;
        made.resultType = value.type;
        made.blocks.append(Block { });
        Value call(value);
        call.inlineFrame = 0;
        call.position = function.position;
        forEachOperand(call, [&](ValueID& operandID) {
            Value operand(function.at(operandID));
            operand.inlineFrame = 0;
            operand.position = function.position;
            operandID = ValueID(made.values.size());
            if (staysInBody(operand))
                made.blocks[0].values.append(operandID);
            else {
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = function.position;
                parameter.type = operand.type;
                parameter.rep = operand.rep;
                parameter.takePredictionOf(operand);
                parameter.immediates = { 0, static_cast<uint32_t>(made.parameterTypes.size()) };
                made.parameterTypes.append(operand.type);
                made.blocks[0].parameters.append(operandID);
                operand = WTF::move(parameter);
            }
            made.values.append(WTF::move(operand));
        });
        made.length = made.parameterTypes.size();
        made.blocks[0].values.append(ValueID(made.values.size()));
        made.blocks[0].terminator.kind = TerminatorKind::Return;
        made.blocks[0].terminator.value = ValueID(made.values.size());
        made.values.append(WTF::move(call));
        bodies[k] = WTF::move(made);
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        for (auto& site : sites[i]) {
            auto& signature = signatures[site.numberOfSignature];
            if (signature.numberOfSites < 2)
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
    if (Options::verboseAOTCompilation())
        printSitesOfPassForOneBody("calls of built-ins"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "a signature that occurs once"_s, numbers[OccursOnce] }, { "the function is called near loops"_s, numbers[IsNearLoops] }, { "a getter or a setter"_s, numbers[IsGetterOrSetter] }, { "an operation of the engine"_s, numbers[IsOperation] }, { "a function, a constructor, a constant"_s, numbers[IsOfOtherKind] }, { "push"_s, numbers[IsPush] }, { "more than six operands, or a second or third child"_s, numbers[HasTooManyOperands] }, { "defineProperty with a literal"_s, numbers[IsDescribedByLiteral] }, { "arithmetic"_s, numbers[IsArithmetic] }, { "an object born without a prototype"_s, numbers[IsBirth] }, { "beside handlers"_s, numbers[IsBesideHandlers] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsLeft] } });
    program.functions.appendVector(WTF::move(bodies));
}

bool mayLeaveConstructionToBody(const Program& program, FunctionID id, const Function& function, const Value& value)
{
    if (function.isBodyOfSeveralSites || program.isCalledNearLoops(id))
        return false;
    return !isInCopyOfHotOriginal(program, function, value, true);
}

static void giveConstructionsOneBody(Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<uint8_t, 0>> sites(count);
    static constexpr OpcodeSet constructions { Opcode::Construct };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functions.mayHold(i, constructions))
            return;
        auto& function = std::as_const(program).functions[i];
        std::optional<BitVector> isInLoop;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::Construct || value.arguments.size() > numberOfArgumentGPRs || function.isBodyOfSeveralSites || program.isCalledNearLoops(FunctionID(i)))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                if (!mayLeaveConstructionToBody(program, FunctionID(i), function, value))
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
        Function made;
        made.name = program.emptyString;
        made.module = std::as_const(program).functions[first[numberOfArguments]].module;
        made.position = std::as_const(program).functions[first[numberOfArguments]].position;
        made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        made.isBodyOfSeveralSites = true;
        made.resultType = program.types.object;
        made.blocks.append(Block { });
        auto addParameter = [&](TypeID type) {
            ValueID id(made.values.size());
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = made.position;
            parameter.type = type;
            parameter.rep = repOf(program.at(type));
            parameter.immediates = { 0, static_cast<uint32_t>(made.parameterTypes.size()) };
            made.parameterTypes.append(type);
            made.blocks[0].parameters.append(id);
            made.values.append(WTF::move(parameter));
            return id;
        };
        Value construct;
        construct.opcode = Opcode::Construct;
        construct.position = made.position;
        construct.type = program.types.object;
        construct.rep = repOf(program.at(program.types.object));
        construct.immediates = { Format::none, Format::none };
        construct.children[0] = addParameter(program.types.anything);
        for (unsigned k = 0; k < numberOfArguments; ++k)
            construct.arguments.append(addParameter(program.types.anything));
        construct.children[1] = addParameter(program.types.integer);
        made.length = made.parameterTypes.size();
        made.blocks[0].values.append(ValueID(made.values.size()));
        made.blocks[0].terminator.kind = TerminatorKind::Return;
        made.blocks[0].terminator.value = ValueID(made.values.size());
        made.values.append(WTF::move(construct));
        program.bodiesOfConstructions[numberOfArguments] = FunctionID(program.functions.size());
        program.functions.append(WTF::move(made));
    }
    if (!Options::verboseAOTCompilation())
        return;
    enum : unsigned { CallsBody, OccursOnce, HasTooManyArguments, IsInBody, IsNearLoops, IsInLoop, IsCopiedFromHot, NumberOfReasons };
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
                ++numbers[value.arguments.size() > numberOfArgumentGPRs ? HasTooManyArguments : function.isBodyOfSeveralSites ? IsInBody : program.isCalledNearLoops(FunctionID(i)) ? IsNearLoops : isInLoop.get(b) ? IsInLoop
                    : !mayLeaveConstructionToBody(program, FunctionID(i), function, value) ? IsCopiedFromHot : numberOfSites[value.arguments.size()] < 2 ? OccursOnce : CallsBody];
            }
        }
    }
    uint64_t taken = 0;
    for (auto& ofFunction : sites)
        taken += ofFunction.size();
    RELEASE_ASSERT(numbers[CallsBody] + numbers[OccursOnce] == taken);
    printSitesOfPassForOneBody("constructions"_s, std::ranges::count_if(numberOfSites, [](unsigned sites) { return sites >= 2; }), { { "call a body"_s, numbers[CallsBody] }, { "a number of arguments that occurs once"_s, numbers[OccursOnce] }, { "more arguments than registers"_s, numbers[HasTooManyArguments] }, { "in a body of several sites"_s, numbers[IsInBody] }, { "the function is called near loops"_s, numbers[IsNearLoops] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsCopiedFromHot] } });
}

static void giveCastsOneBody(Program& program)
{
    constexpr unsigned minSites = 4;
    unsigned count = program.functions.size();
    Vector<Vector<std::pair<ValueID, uint64_t>>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        FunctionID id(i);
        bool isInSet = !function.isBodyOfSeveralSites && program.isColdByGraph(id);
        if (!isInSet || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID valueID : function.blocks[b].values) {
                auto& value = function.at(valueID);
                if (!Program::mayHaveBody(value) || value.rep == Rep::Void || value.operandClassIsProven)
                    continue;
                Rep given = function.at(value.child(0)).rep;
                if ((given != Rep::JSValue && given != Rep::Cell) || program.isHeldAlready(program.typeOf(function, value.child(0)), program.at(value.type), value))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                sites[i].append({ valueID, Program::keyOfBodyOfCast(value) });
            }
        }
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
        Function made;
        made.name = program.emptyString;
        made.module = function.module;
        made.position = function.position;
        made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
        made.isBodyOfSeveralSites = true;
        made.length = 1;
        made.resultType = value.type;
        made.blocks.append(Block { });
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.position = function.position;
        parameter.type = program.types.anything;
        parameter.rep = repOf(program.at(program.types.anything));
        parameter.immediates = { 0, 0 };
        made.parameterTypes.append(program.types.anything);
        ValueID given(made.values.size());
        made.blocks[0].parameters.append(given);
        made.values.append(WTF::move(parameter));
        Value cast(value);
        cast.inlineFrame = 0;
        cast.setParameterCheckCallee(FunctionID());
        cast.position = function.position;
        cast.children[0] = given;
        cast.takePredictionOf(Value { });
        made.blocks[0].values.append(ValueID(made.values.size()));
        made.blocks[0].terminator.kind = TerminatorKind::Return;
        made.blocks[0].terminator.value = ValueID(made.values.size());
        made.values.append(WTF::move(cast));
        bodies.append(WTF::move(made));
        program.bodiesOfCasts.add(first.key, count + static_cast<uint32_t>(bodies.size()) - 1);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", bodies.size(), " casts have a body that their sites in cold code call");
    enum : unsigned { CallsBody, HasTooFewSites, IsOutsideSet, MayHaveNoBody, IsGivenInNoRegisterOrHeld, IsInLoop, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty())
            continue;
        FunctionID id(i);
        bool isInSet = !function.isBodyOfSeveralSites && program.isColdByGraph(id);
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
                    reason = numberOfSites.get(Program::keyOfBodyOfCast(value)) < minSites ? HasTooFewSites : CallsBody;
                else if (!isInSet)
                    reason = IsOutsideSet;
                else if (!Program::mayHaveBody(value) || value.rep == Rep::Void)
                    reason = MayHaveNoBody;
                else if ((given != Rep::JSValue && given != Rep::Cell) || program.isHeldAlready(program.typeOf(function, value.child(0)), program.at(value.type), value))
                    reason = IsGivenInNoRegisterOrHeld;
                ++numbers[reason];
            }
        }
    }
    if (Options::verboseAOTCompilation())
        printSitesOfPassForOneBody("casts"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "fewer than four sites"_s, numbers[HasTooFewSites] }, { "outside the set"_s, numbers[IsOutsideSet] }, { "a cast that may have no body"_s, numbers[MayHaveNoBody] }, { "given in no register, or held already"_s, numbers[IsGivenInNoRegisterOrHeld] }, { "in a loop"_s, numbers[IsInLoop] } });
    program.functions.appendVector(WTF::move(bodies));
}

struct FetchOfExternalModule {
    uint32_t numberOfCalleesAhead { 0 };
    ValueID read;
    ValueID undefined;
    ValueID comparison;
    ValueID fetch;
    ValueID store;
};

struct FoundModuleEvaluation {
    Program::ModuleEvaluation evaluation;
    Vector<FetchOfExternalModule, 2> fetches;
};

static bool doesNothing(const Function& function)
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
        return record.startsUndefined && !record.type && record.store == Store::JSValue && !record.onlyFunction;
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
            if (FunctionID callee = at(*block, start).function(); !doesNothing(program.at(callee)))
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
    auto forEachOtherRowCalledBy = [&](uint32_t row, auto&& functor) {
        for (FunctionID callee : program.moduleEvaluations[row].callees) {
            auto other = program.moduleEvaluationOfFunction.find(callee.index());
            if (other != program.moduleEvaluationOfFunction.end() && other->value != row)
                functor(other->value);
        }
    };
    Vector<uint32_t> numberOfCalls(FillWith { }, numberOfRows, 0u);
    for (uint32_t row = 0; row < numberOfRows; ++row) {
        forEachOtherRowCalledBy(row, [&](uint32_t callee) {
            ++numberOfCalls[callee];
        });
    }
    Vector<uint32_t> worklist;
    for (uint32_t row = 0; row < numberOfRows; ++row) {
        if (!numberOfCalls[row])
            worklist.append(row);
    }
    uint32_t numberAheadOfEveryCycle = 0;
    while (!worklist.isEmpty()) {
        ++numberAheadOfEveryCycle;
        forEachOtherRowCalledBy(worklist.takeLast(), [&](uint32_t callee) {
            if (!--numberOfCalls[callee])
                worklist.append(callee);
        });
    }
    return numberAheadOfEveryCycle != numberOfRows;
}

static bool asksForModuleEvaluation(const Value& value, const Vector<uint8_t>& evaluatesModule)
{
    return value.opcode == Opcode::CallDirect && value.arguments.isEmpty() && !value.isJumpToContinuation() && evaluatesModule[value.function().index()];
}

static unsigned removeRedundantModuleEvaluations(Function& function, const Vector<uint8_t>& evaluatesModule)
{
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfModule;
    unsigned numberOfRequests = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (!asksForModuleEvaluation(value, evaluatesModule))
                continue;
            ++numberOfRequests;
            unsigned next = numberOfModule.size();
            numberOfModule.add(value.function().index(), next);
        }
    }
    if (numberOfRequests == numberOfModule.size())
        return 0;
    auto suspends = [](const Value& value) {
        return value.opcode == Opcode::Await || value.opcode == Opcode::Yield;
    };
    Vector<BitVector> evaluatedAtHead(function.blocks.size());
    BitVector isReached(function.blocks.size());
    isReached.set(0);
    Vector<unsigned, 16> worklist { 0u };
    auto flowInto = [&](unsigned block, const BitVector& evaluated) {
        if (!isReached.set(block)) {
            evaluatedAtHead[block] = evaluated;
            worklist.append(block);
            return;
        }
        BitVector onEveryWay = evaluatedAtHead[block];
        onEveryWay.filter(evaluated);
        if (onEveryWay == evaluatedAtHead[block])
            return;
        evaluatedAtHead[block] = WTF::move(onEveryWay);
        worklist.append(block);
    };
    while (!worklist.isEmpty()) {
        unsigned b = worklist.takeLast();
        auto& block = function.blocks[b];
        BitVector evaluated = evaluatedAtHead[b];
        BitVector evaluatedAheadOfLast = evaluated;
        for (unsigned i = 0; i < block.values.size(); ++i) {
            auto& value = function.at(block.values[i]);
            if (suspends(value))
                evaluated.clearAll();
            if (i + 1 == block.values.size())
                evaluatedAheadOfLast = evaluated;
            if (asksForModuleEvaluation(value, evaluatesModule))
                evaluated.set(numberOfModule.get(value.function().index()));
        }
        bool isWatched = block.terminator.kind == TerminatorKind::Invoke;
        for (unsigned target = 0; target < block.terminator.targets.size(); ++target)
            flowInto(block.terminator.targets[target].block.index(), isWatched && target == 1 ? evaluatedAheadOfLast : evaluated);
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
        BitVector evaluated = evaluatedAtHead[b];
        unsigned removedHere = 0;
        bool removedLast = false;
        for (unsigned i = 0; i < block.values.size(); ++i) {
            ValueID id = block.values[i];
            auto& value = function.at(id);
            if (suspends(value))
                evaluated.clearAll();
            if (!asksForModuleEvaluation(value, evaluatesModule))
                continue;
            if (!evaluated.set(numberOfModule.get(value.function().index())) || isUsed.get(id.index()))
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
    Vector<std::optional<FoundModuleEvaluation>> found(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        found[i] = asModuleEvaluation(program, FunctionID(i));
    });
    Vector<uint8_t> isReachedOtherwise(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.predictedClosure())
                WTF::atomicStore(&isReachedOtherwise[value.predictedClosure().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            bool namesFunction = value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::GetCapture;
            if (namesFunction || (value.opcode == Opcode::CallDirect && (!value.arguments.isEmpty() || value.isJumpToContinuation())))
                WTF::atomicStore(&isReachedOtherwise[value.function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!found[i] || isReachedOtherwise[i])
            continue;
        program.moduleEvaluationOfFunction.add(i, program.moduleEvaluations.size());
        program.moduleEvaluations.append(WTF::move(found[i]->evaluation));
    }
    bool hasCycle = moduleEvaluationsHaveCycle(program);
    constexpr unsigned fewestWorthSharing = 4;
    if (program.moduleEvaluations.size() < fewestWorthSharing && !hasCycle) {
        program.moduleEvaluations.clear();
        program.moduleEvaluationOfFunction.clear();
        return;
    }
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numberOfExternalModule;
    Vector<std::pair<FunctionID, FetchOfExternalModule>> externalModules;
    for (auto& evaluation : program.moduleEvaluations) {
        auto& function = std::as_const(program).at(evaluation.function);
        for (auto& fetch : found[evaluation.function.index()]->fetches) {
            uint64_t key = static_cast<uint64_t>(function.at(fetch.read).variable().index()) << 32 | function.at(fetch.fetch).string().index();
            auto number = numberOfExternalModule.add(key, static_cast<uint32_t>(externalModules.size()));
            if (number.isNewEntry)
                externalModules.append({ evaluation.function, fetch });
            evaluation.fetches.append({ fetch.numberOfCalleesAhead, number.iterator->value });
        }
    }

    uint32_t flags = std::as_const(program).at(program.moduleEvaluations[0].function).flags;
    uint32_t numberOfRows = program.moduleEvaluations.size();
    auto& types = program.types;
    Function* beingMade = nullptr;
    ValueID markOfFetch;
    auto make = [&](unsigned numberOfBlocks, auto&& fill) {
        Function made;
        beingMade = &made;
        made.name = program.emptyString;
        made.kind = FunctionKind::Module;
        made.flags = flags;
        made.parameterTypes.append(types.int32);
        made.length = 1;
        made.blocks.grow(numberOfBlocks);
        auto add = [&](unsigned block, Opcode opcode, TypeID type, ValueID child = { }, ValueID second = { }, uint32_t immediate = Format::none) {
            Value value;
            value.opcode = opcode;
            value.type = type;
            value.rep = repOf(program.at(type));
            value.children[0] = child;
            value.children[1] = second;
            value.immediates[0] = immediate;
            ValueID id(made.values.size());
            if (opcode == Opcode::Parameter)
                value.immediates = { block, static_cast<uint32_t>(made.blocks[block].parameters.size()) };
            made.values.append(WTF::move(value));
            if (opcode == Opcode::Parameter)
                made.blocks[block].parameters.append(id);
            else
                made.blocks[block].values.append(id);
            return id;
        };
        auto end = [&](unsigned block, TerminatorKind kind, ValueID value = { }, std::initializer_list<std::pair<unsigned, ValueID>> targets = { }) {
            auto& terminator = made.blocks[block].terminator;
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
        program.functions.append(WTF::move(made));
    };
    program.moduleEvaluator = FunctionID(program.functions.size());
    program.moduleEvaluatorIfNeeded = FunctionID(program.functions.size() + 1);
    if (hasCycle)
        program.moduleEvaluatorFromOutside = FunctionID(program.functions.size() + 2);
    program.functionsHotByText.set(program.moduleEvaluator.index());
    program.functionsWarmByText.set(program.moduleEvaluator.index());
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
        ValueID place = word(pair, cursor, 0);
        ValueID following = add(pair, Opcode::OffsetIndex, types.int32, cursor, { }, 2);
        ValueID none = add(pair, Opcode::Integer, types.int32);
        constexpr unsigned isNoFunction = 14, isFetch = 15, firstOfExternalModules = 16;
        end(pair, TerminatorKind::Branch, isSame(pair, place, none), { { isFunction, { } }, { externalModules.isEmpty() ? isRow : isNoFunction, { } } });
        if (!externalModules.isEmpty()) {
            markOfFetch = add(isNoFunction, Opcode::Integer, types.int32);
            end(isNoFunction, TerminatorKind::Branch, isSame(isNoFunction, place, markOfFetch), { { isFetch, { } }, { isRow, { } } });
            end(isFetch, TerminatorKind::Switch, word(isFetch, cursor, 1));
            auto& dispatch = beingMade->blocks[isFetch].terminator;
            for (unsigned number = 0; number < externalModules.size(); ++number) {
                auto& fetch = externalModules[number].second;
                auto& function = std::as_const(program).at(externalModules[number].first);
                unsigned asks = firstOfExternalModules + 3 * number;
                unsigned fetches = asks + 1;
                unsigned stores = asks + 2;
                auto copy = [&](unsigned block, ValueID copied, ValueID child = { }, ValueID second = { }) {
                    Value value = function.at(copied);
                    value.children[0] = child;
                    value.children[1] = second;
                    ValueID id(beingMade->values.size());
                    beingMade->values.append(WTF::move(value));
                    beingMade->blocks[block].values.append(id);
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
        end(isRow, TerminatorKind::Branch, isSame(isRow, add(isRow, Opcode::GetModuleVariableAt, types.anything, place, { }, isFlag), boolean(isRow, true)), { { next, { } }, { evaluate, { } } });
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
        auto& made = program.at(program.moduleEvaluator);
        for (auto& value : made.values) {
            if (value.opcode == Opcode::Integer)
                value.setBits(static_cast<uint64_t>(-1));
            if (value.opcode == Opcode::CallDirect)
                value.arguments.append(made.blocks[9].values[0]);
        }
        if (markOfFetch)
            made.values[markOfFetch.index()].setBits(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Program::ModuleEvaluation::markOfFetch))));
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
        auto& made = program.at(program.moduleEvaluatorFromOutside);
        for (auto& value : made.values) {
            if (value.opcode == Opcode::CallDirect)
                value.arguments.append(made.blocks[0].parameters[0]);
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.moduleEvaluations.size(), " modules are evaluated by one function, which fetches ", externalModules.size(), " external modules");
}

static void dropModuleEvaluations(Program& program)
{
    for (auto& evaluation : program.moduleEvaluations) {
        auto& function = program.at(evaluation.function);
        program.functionsWithoutCalls.set(evaluation.function.index());
        function.blocks.shrink(1);
        function.blocks[0].values.clear();
        function.blocks[0].terminator = Terminator { };
    }
    if (program.moduleEvaluations.isEmpty())
        return;
    Vector<uint8_t> isNamed(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (program.moduleEvaluationOfFunction.contains(i))
            return;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::PutModuleVariable)
                WTF::atomicStore(&isNamed[value.variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
    });
    for (auto& evaluation : program.moduleEvaluations) {
        for (VariableID variable : { evaluation.isEvaluated, evaluation.error }) {
            if (variable && !isNamed[variable.index()])
                program.variables[variable.index()].startsUndefined = false;
        }
    }
}

// As arguments are passed: see Lowering::registersOfResults().
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

std::optional<FreshResults> freshResultsOf(const Program& program, const Function& function)
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
            uint8_t nilKind = returned == Opcode::Undefined ? 1 : 2;
            if (result.nilKind && result.nilKind != nilKind)
                return std::nullopt;
            result.nilKind = nilKind;
            result.nilReturns.append(b);
            continue;
        }
        if ((returned != Opcode::NewArray && returned != Opcode::NewObjectWithShape) || !function.blocks[b].values.contains(terminator.value))
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
        ValueID made = block.terminator.value;
        auto& value = function.at(made);
        bool isFirst = &held == &result.returns[0].second;
        if (value.opcode == Opcode::NewArray) {
            if ((!isFirst && !result.isArray) || uses[made.index()] != 1 || !value.arguments.size() || value.arguments.size() > maxResults)
                return std::nullopt;
            result.isArray = true;
            held.appendRange(value.arguments.begin(), value.arguments.end());
        } else {
            auto& layout = program.at(value.layout());
            unsigned size = layout.fields.size();
            if ((!isFirst && (result.isArray || result.layout != value.layout())) || value.child(0) || !isObjectLayout(layout.kind) || !size || size > maxResults || uses[made.index()] != size + 1)
                return std::nullopt;
            auto& ofFirst = function.allocations[function.at(function.blocks[result.returns[0].first].terminator.value).allocation()];
            auto& ofThis = function.allocations[value.allocation()];
            if (ofFirst.order != ofThis.order || ofFirst.born != ofThis.born || ofFirst.constructedBy != ofThis.constructedBy)
                return std::nullopt;
            result.layout = value.layout();
            held.fill(ValueID(), size);
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (store.opcode != Opcode::PutField || store.child(0) != made)
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
    // A type that the table has: that of all, or the narrowest of the plain ones that takes all. The types are frozen: none is made.
    for (unsigned k = 0; k < result.returns[0].second.size(); ++k) {
        TypeID type = function.at(result.returns[0].second[k]).type;
        for (auto& entry : result.returns) {
            TypeID other = function.at(entry.second[k]).type;
            if (other == type)
                continue;
            TypeID joined = program.types.anything;
            for (TypeID wider : { program.types.boolean, program.types.int32, program.types.number, program.types.string }) {
                if (program.at(type).isWithin(program.at(wider), program) && program.at(other).isWithin(program.at(wider), program)) {
                    joined = wider;
                    break;
                }
            }
            type = joined;
        }
        // What is stored to a field is of the type of the field.
        if (TypeID declared = result.isArray ? TypeID() : program.at(result.layout).fields[k].type; declared && !program.at(type).isWithin(program.at(declared), program))
            type = declared;
        if (repOf(program.at(type)) == Rep::Void)
            type = program.types.anything;
        result.types.append(type);
    }
    if (!fitInRegisters(program, result.types, result.nilKind))
        return std::nullopt;
    return result;
}

std::optional<ReadsOfFreshResult> readsOfFreshResult(const Program& program, const Function& function, ValueID call, const FreshResults& fresh)
{
    ReadsOfFreshResult result;
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
                    if (!fresh.isArray && isObjectLayout(program.at(value.layout()).kind))
                        index = program.at(fresh.layout).fieldNamed(program.at(value.layout()).fields[value.field()].name);
                    break;
                case Opcode::GetById:
                    if (!fresh.isArray && !value.readsMethodFor())
                        index = program.at(fresh.layout).fieldNamed(value.string());
                    break;
                case Opcode::IsUndefinedOrNull:
                    result.nilTests.append(user);
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
                // The read is held to its type by what it reads; a result that is of the type needs no test.
                if (!index || !program.at(fresh.types[*index]).isWithin(program.at(value.type), program))
                    return std::nullopt;
                result.reads.append({ user, *index });
            }
        }
    }
    return result;
}

static Type unionOf(const Type& a, const Type& b)
{
    Type result = a;
    result.primitives |= b.primitives;
    result.integers = a.integers.hull(b.integers);
    result.anyObject |= b.anyObject;
    result.anyFunction |= b.anyFunction;
    for (LayoutID view : b.views) {
        if (!result.views.contains(view))
            result.views.append(view);
    }
    std::ranges::sort(result.views, { }, [](LayoutID view) {
        return view.index();
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

// By block: the one that immediately dominates it. The entry, and what is not reached: itself.
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

bool takeApartResultOrNil(const Program& program, Function& function, ValueID call)
{
    auto& wrapper = program.at(function.at(call).function());
    auto& types = program.at(wrapper.worker).resultTypes;
    const Value* made = nullptr;
    for (auto& value : wrapper.values) {
        if (value.opcode == Opcode::NewArray || value.opcode == Opcode::NewObjectWithShape)
            made = &value;
    }
    RELEASE_ASSERT(made && wrapper.nilOfResults);
    bool isArray = made->opcode == Opcode::NewArray;
    Opcode nilOpcode = wrapper.nilOfResults == 1 ? Opcode::Undefined : Opcode::Null;

    enum class Test : uint8_t { IsThere, IsNil, Never };
    Vector<ValueID, 4> aliases { call };
    Vector<std::pair<ValueID, uint32_t>, 8> reads;
    Vector<std::pair<ValueID, Test>, 4> tests;
    // The blocks in which there has to be an object.
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
                // It is no use, and it holds: there is no object.
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
                    tests.append({ user, Test::IsThere });
                    continue;
                case Opcode::IsUndefinedOrNull:
                    tests.append({ user, Test::IsNil });
                    continue;
                case Opcode::StrictEq:
                case Opcode::Eq: {
                    ValueID other = value.child(0) == object ? value.child(1) : value.child(0);
                    Opcode constant = function.at(other).opcode;
                    if (other == object || (constant != Opcode::Undefined && constant != Opcode::Null))
                        return false;
                    tests.append({ user, value.opcode == Opcode::Eq || constant == nilOpcode ? Test::IsNil : Test::Never });
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
                        index = program.at(made->layout()).fieldNamed(program.at(value.layout()).fields[value.field()].name);
                    break;
                case Opcode::GetById:
                    if (!isArray && !value.readsMethodFor())
                        index = program.at(made->layout()).fieldNamed(value.string());
                    break;
                default:
                    break;
                }
                if (!index || !program.at(types[*index + 1]).isWithin(program.at(value.type), program))
                    return false;
                reads.append({ user, *index + 1 });
                needsObject.set(b);
            }
        }
    }

    if (!needsObject.isEmpty()) {
        // By value that says whether there is an object: whether it is true then.
        UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isTrueIfThere;
        for (auto [test, kind] : tests) {
            if (kind != Test::Never)
                isTrueIfThere.add(test.index(), kind == Test::IsThere);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::LogicalNot || isTrueIfThere.contains(id.index()))
                        continue;
                    if (auto found = isTrueIfThere.find(value.child(0).index()); found != isTrueIfThere.end()) {
                        bool flipped = !found->value;
                        isTrueIfThere.add(id.index(), flipped);
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
        BitVector isArmWithObject(function.blocks.size());
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto found = isTrueIfThere.find(block.terminator.value.index());
            if (found == isTrueIfThere.end())
                continue;
            unsigned arm = block.terminator.targets[found->value ? 0 : 1].block.index();
            if (numberOfPredecessors[arm] == 1)
                isArmWithObject.set(arm);
        }
        auto dominators = immediateDominatorsOf(function);
        for (unsigned b : needsObject) {
            bool isGuarded = false;
            for (unsigned at = b;; at = dominators[at]) {
                if (isArmWithObject.get(at)) {
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
        value.takePredictionOf(Value { });
    }
    for (auto [read, index] : reads)
        function.at(read) = resultAt(function.at(read), index);
    for (auto [test, kind] : tests) {
        if (kind == Test::IsThere) {
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
        ValueID isThere(function.values.size());
        function.values.append(resultAt(function.at(test), 0));
        for (auto& block : function.blocks) {
            if (size_t at = block.values.find(test); at != notFound)
                block.values.insert(at, isThere);
        }
        auto& negated = function.at(test);
        negated.opcode = Opcode::LogicalNot;
        negated.children = { isThere, ValueID(), ValueID() };
        negated.immediates = { Format::none, Format::none };
    }
    for (auto& block : function.blocks) {
        block.values.removeAllMatching([&](ValueID id) {
            return id != call && aliases.contains(id);
        });
    }
    return true;
}

static void splitOffReadsOfDestructuredParameters(Program& program)
{
    static constexpr unsigned maxParametersOfBody = 6;
    unsigned count = program.functions.size();
    Vector<uint32_t> parametersHandedNewObjects(FillWith { }, count, 0u);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& call = function.at(id);
                if (call.opcode != Opcode::CallDirect)
                    continue;
                for (unsigned k = 0; k < call.arguments.size() && k < 32; ++k) {
                    ValueID argument = call.arguments[k];
                    while (function.at(argument).opcode == Opcode::Cast)
                        argument = function.at(argument).child(0);
                    if (function.at(argument).opcode == Opcode::NewObjectWithShape)
                        WTF::atomicExchangeOr(&parametersHandedNewObjects[call.function().index()], 1u << k, std::memory_order_relaxed);
                }
            }
        }
    });
    Vector<Function> bodies;
    for (unsigned i = 0; i < count; ++i) {
        if (!parametersHandedNewObjects[i])
            continue;
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty() || !function.isTyped() || (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method))
            continue;
        if (function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionIsBuiltin | Format::FunctionIsSloppy | Format::FunctionHasRestParameter | Format::FunctionIsHiddenInStackTraces | FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer | FunctionLogsMisfits))
            continue;
        if (function.declaredAs || function.resumeFunction || function.startFunction || function.savesItsFrame || function.worker || !function.resultTypes.isEmpty() || function.constructedClass || function.isValueEntry || function.takesItsClosure || function.rampForClosures || function.isRampForClosures || function.secondBody || function.writtenEntry || function.isBodyOfSeveralSites || function.isGuardedPreludeEntry || program.isResumeFunction(function))
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
        if (index == notFound || index < first || index >= 32 || !(parametersHandedNewObjects[i] & 1u << index))
            continue;
        Vector<ValueID, 8> reads;
        for (ValueID id : reading.values) {
            auto& read = function.at(id);
            if (read.opcode != Opcode::GetById || read.child(0) != parameter || read.immediates[1] != Format::none || !fieldOf(program, program.typeOf(function, parameter), read.string()))
                break;
            reads.append(id);
        }
        if (reads.isEmpty() || entry.parameters.size() - 1 + reads.size() > maxParametersOfBody)
            continue;

        unsigned uses = 0;
        bool isEnteredAgain = false;
        BitVector isOfThrower(function.values.size());
        if (test) {
            for (ValueID id : function.blocks[thrower].values)
                isOfThrower.set(id.index());
        }
        bool throwerStandsAlone = true;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            auto use = [&](ValueID child) {
                uses += child == parameter;
                if (test && b == thrower)
                    throwerStandsAlone &= isOfThrower.get(child.index());
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
        if (uses != reads.size() + !!test || isEnteredAgain || !throwerStandsAlone || !function.blocks[reader].parameters.isEmpty() || (test && !function.blocks[thrower].parameters.isEmpty()))
            continue;

        FunctionID bodyID(count + bodies.size());
        unsigned declared = index - first;
        Function body = function;
        body.flags &= ~(FunctionHasUnknownCallers | FunctionIsEnteredByValue);
        body.hasManyClosures = false;
        body.closuresAreGivenProperties = false;
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
                    taken.takePredictionOf(value);
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
                ValueID made = add(WTF::move(thrown));
                copies.add(id.index(), made);
                wrapper.blocks[1].values.append(made);
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
        if (program.functionsHotByText.get(i))
            program.functionsHotByText.set(bodyID.index());
        if (program.functionsWarmByText.get(i))
            program.functionsWarmByText.set(bodyID.index());
        if (program.functionsCalledNearLoops.get(i))
            program.functionsCalledNearLoops.set(bodyID.index());
        if (program.functionsHotOnlyInTheirLoops.get(i))
            program.functionsHotOnlyInTheirLoops.set(bodyID.index());
        if (program.functionsInCycles.get(i))
            program.functionsInCycles.set(bodyID.index());
        if (program.functionsThatTakeNoCopies.get(i))
            program.functionsThatTakeNoCopies.set(bodyID.index());
        if (program.functionsColdByGraph.get(i))
            program.functionsColdByGraph.set(bodyID.index());
        program.functions[i] = WTF::move(wrapper);
        bodies.append(WTF::move(body));
    }
    program.functions.appendVector(WTF::move(bodies));
}

// See Function::worker. Of the program as it was read.
static void splitOffWorkers(Program& program)
{
    unsigned count = program.functions.size();
    program.types.thaw();
    Vector<std::unique_ptr<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (auto fresh = freshResultsOf(program, std::as_const(program).functions[i]))
            freshResults[i] = makeUniqueWithoutFastMallocCheck<FreshResults>(WTF::move(*fresh));
    });
    Vector<unsigned> functionsSplit;
    for (unsigned i = 0; i < count; ++i) {
        auto& fresh = freshResults[i];
        if (!fresh)
            continue;
        auto& function = std::as_const(program).functions[i];
        for (unsigned k = 0; k < fresh->types.size(); ++k) {
            Type type = program.at(function.at(fresh->returns[0].second[k]).type);
            for (auto& entry : fresh->returns)
                type = unionOf(type, program.at(function.at(entry.second[k]).type));
            TypeID joined = program.types.intern(type);
            if (repOf(program.at(joined)) != Rep::Void && program.at(joined).isWithin(program.at(fresh->types[k]), program))
                fresh->types[k] = joined;
        }
        if (fitInRegisters(program, fresh->types, fresh->nilKind))
            functionsSplit.append(i);
    }
    Vector<Function> wrappers(functionsSplit.size());
    Vector<Function> workers(functionsSplit.size());
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
        // The block that makes the object.
        unsigned building = 0;
        auto append = [&](Value&& value) {
            ValueID id = add(WTF::move(value));
            wrapper.blocks[building].values.append(id);
            return id;
        };
        auto& first = worker.blocks[fresh->returns[0].first];
        Value made(worker.at(first.terminator.value));
        Value call;
        call.opcode = Opcode::CallDirect;
        call.position = made.position;
        call.type = program.types.undefined;
        call.rep = repOf(program.at(call.type));
        call.immediates = { workerID.index(), Format::none };
        for (ValueID parameter : worker.blocks[0].parameters) {
            call.arguments.append(add(Value(worker.at(parameter))));
            wrapper.blocks[0].parameters.append(call.arguments.last());
        }
        ValueID callID = append(WTF::move(call));
        if (fresh->nilKind) {
            Value isThere;
            isThere.opcode = Opcode::ResultAt;
            isThere.position = made.position;
            isThere.type = program.types.boolean;
            isThere.rep = repOf(program.at(isThere.type));
            isThere.children = { callID, ValueID(), ValueID() };
            isThere.immediates = { 0, Format::none };
            wrapper.blocks[0].terminator.kind = TerminatorKind::Branch;
            wrapper.blocks[0].terminator.value = append(WTF::move(isThere));
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(1u), { } });
            wrapper.blocks[0].terminator.targets.append(Target { BlockID(2u), { } });
            wrapper.blocks.append(Block { });
            wrapper.blocks.append(Block { });
            Value nothing;
            nothing.opcode = fresh->nilKind == 1 ? Opcode::Undefined : Opcode::Null;
            nothing.position = made.position;
            ValueID returned = worker.blocks[fresh->nilReturns[0]].terminator.value;
            nothing.type = returned ? worker.at(returned).type : program.types.undefined;
            nothing.rep = repOf(program.at(nothing.type));
            wrapper.blocks[2].terminator.kind = TerminatorKind::Return;
            wrapper.blocks[2].terminator.value = add(WTF::move(nothing));
            wrapper.blocks[2].values.append(wrapper.blocks[2].terminator.value);
            wrapper.nilOfResults = fresh->nilKind;
            building = 1;
        }
        Arguments held;
        for (unsigned k = 0; k < fresh->types.size(); ++k) {
            const Value& text = worker.at(fresh->returns[0].second[k]);
            bool isOneText = !fresh->isArray && text.opcode == Opcode::String;
            for (auto& entry : fresh->returns)
                isOneText = isOneText && worker.at(entry.second[k]).opcode == Opcode::String && worker.at(entry.second[k]).string() == text.string();
            if (isOneText) {
                Value constant(text);
                constant.position = made.position;
                held.append(append(WTF::move(constant)));
                continue;
            }
            Value result;
            result.opcode = Opcode::ResultAt;
            result.position = made.position;
            result.type = fresh->types[k];
            result.rep = repOf(program.at(result.type));
            result.children = { callID, ValueID(), ValueID() };
            result.immediates = { k + !!fresh->nilKind, Format::none };
            held.append(append(WTF::move(result)));
        }
        if (fresh->isArray)
            made.arguments = held;
        else {
            wrapper.allocations.append(Allocation(worker.allocations[made.allocation()]));
            made.immediates[1] = 0;
        }
        ValueID object = append(WTF::move(made));
        if (!fresh->isArray) {
            for (ValueID id : first.values) {
                if (worker.at(id).opcode != Opcode::PutField || worker.at(id).child(0) != first.terminator.value)
                    continue;
                Value store(worker.at(id));
                store.children[0] = object;
                store.children[1] = held[store.field()];
                append(WTF::move(store));
            }
        }
        wrapper.blocks[building].terminator.kind = TerminatorKind::Return;
        wrapper.blocks[building].terminator.value = object;

        for (auto& [b, operands] : fresh->returns) {
            auto& block = worker.blocks[b];
            ValueID literal = block.terminator.value;
            block.values.removeAllMatching([&](ValueID id) {
                return id == literal || (worker.at(id).opcode == Opcode::PutField && worker.at(id).child(0) == literal);
            });
            Value results;
            results.opcode = Opcode::Results;
            results.position = worker.at(literal).position;
            results.type = program.types.undefined;
            results.rep = repOf(program.at(results.type));
            if (fresh->nilKind) {
                Value isThere;
                isThere.opcode = Opcode::Boolean;
                isThere.position = results.position;
                isThere.type = program.types.boolean;
                isThere.rep = repOf(program.at(isThere.type));
                isThere.immediates = { 1, Format::none };
                results.arguments.append(ValueID(worker.values.size()));
                block.values.append(results.arguments.last());
                worker.values.append(WTF::move(isThere));
            }
            results.arguments.appendRange(operands.begin(), operands.end());
            block.terminator.value = ValueID(worker.values.size());
            block.values.append(block.terminator.value);
            worker.values.append(WTF::move(results));
        }
        for (unsigned b : fresh->nilReturns) {
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
        if (fresh->nilKind)
            worker.resultTypes.insert(0, program.types.boolean);

        wrappers[numberOfSplit] = WTF::move(wrapper);
        workers[numberOfSplit] = WTF::move(worker);
    });
    for (unsigned numberOfSplit = 0; numberOfSplit < functionsSplit.size(); ++numberOfSplit) {
        unsigned i = functionsSplit[numberOfSplit];
        FunctionID workerID(count + numberOfSplit);
        program.numberOfDirectCalls.append(program.numberOfDirectCalls[i] + 1);
        if (program.functionsHotByText.get(i))
            program.functionsHotByText.set(workerID.index());
        if (program.functionsWarmByText.get(i))
            program.functionsWarmByText.set(workerID.index());
        if (program.functionsCalledNearLoops.get(i))
            program.functionsCalledNearLoops.set(workerID.index());
        if (program.functionsHotOnlyInTheirLoops.get(i))
            program.functionsHotOnlyInTheirLoops.set(workerID.index());
        if (program.functionsInCycles.get(i))
            program.functionsInCycles.set(workerID.index());
        if (program.functionsThatTakeNoCopies.get(i))
            program.functionsThatTakeNoCopies.set(workerID.index());
        if (program.functionsColdByGraph.get(i))
            program.functionsColdByGraph.set(workerID.index());
        program.functions[i] = WTF::move(wrappers[numberOfSplit]);
        program.functions.append(WTF::move(workers[numberOfSplit]));
    }
    program.types.freeze();
}

static void splitOffReadsOfHostObjects(Program& program)
{
    struct Reader {
        TypeID typeOfHostObject;
        TypeID assertedTypeOfHostObject;
        uint32_t flags;
        unsigned made;
    };
    constexpr uint32_t flagsOfText = Format::FunctionIsTyped | Format::FunctionIsOfBuiltinModule | Format::FunctionIsSloppy;
    unsigned count = program.functions.size();
    UncheckedKeyHashMap<uint64_t, Vector<Reader, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> readers;
    Vector<Function> made;
    Vector<uint32_t> numbersOfCalls;
    Vector<uint32_t> numbersOfUses;
    Vector<uint8_t> readsHostObject(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin))
            return;
        for (auto& block : function.blocks) {
            for (unsigned k = 1; k < block.values.size(); ++k) {
                auto& read = function.at(block.values[k]);
                auto& global = function.at(block.values[k - 1]);
                if (read.opcode == Opcode::GetById && global.opcode == Opcode::GetGlobal && program.isDataMemberOfHostObject(global.string(), read.string()))
                    readsHostObject[i] = 1;
            }
        }
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
                if (!program.isDataMemberOfHostObject(global.string(), read.string()))
                    continue;
                if (!std::exchange(hasCountedUses, true)) {
                    numbersOfUses.fill(0, function.values.size());
                    auto use = [&](ValueID child) { ++numbersOfUses[child.index()]; };
                    for (auto& value : function.values)
                        std::as_const(value).forEachChild(use);
                    for (auto& other : function.blocks)
                        other.terminator.forEachChild(use);
                }
                if (numbersOfUses[base.index()] != 1)
                    continue;
                uint32_t flags = function.flags & flagsOfText;
                auto& ofPair = readers.add(static_cast<uint64_t>(global.string().index()) << 32 | read.string().index(), Vector<Reader, 2> { }).iterator->value;
                size_t found = ofPair.findIf([&](auto& reader) { return reader.typeOfHostObject == global.type && reader.assertedTypeOfHostObject == global.assertedType() && reader.flags == flags; });
                if (found == notFound) {
                    found = ofPair.size();
                    ofPair.append(Reader { global.type, global.assertedType(), flags, static_cast<unsigned>(made.size()) });
                    Function reader;
                    reader.name = read.string();
                    reader.module = function.module;
                    reader.position = read.position;
                    reader.flags = flags | Format::FunctionIsHiddenInStackTraces;
                    reader.readsMemberOfHostObject = true;
                    reader.isBodyOfSeveralSites = true;
                    reader.resultType = program.types.anything;
                    reader.blocks.append(Block { });
                    reader.blocks[0].terminator.kind = TerminatorKind::Return;
                    Function general = reader;
                    Value yielded(read);
                    yielded.type = program.types.anything;
                    yielded.rep = repOf(program.at(yielded.type));
                    yielded.setAssertedType(TypeID());
                    yielded.takePredictionOf(Value { });
                    general.values.append(Value(global));
                    general.values.append(Value(yielded));
                    general.values[1].children[0] = ValueID(0u);
                    general.blocks[0].values.append(ValueID(0u));
                    general.blocks[0].values.append(ValueID(1u));
                    general.blocks[0].terminator.value = ValueID(1u);
                    yielded.opcode = Opcode::CallDirect;
                    yielded.children = { };
                    yielded.immediates = { count + static_cast<unsigned>(made.size()) + 1, Format::none };
                    reader.hostObjectReadFromCache = global.string();
                    reader.generalReaderOfHostObject = FunctionID(count + static_cast<unsigned>(made.size()) + 1);
                    reader.values.append(WTF::move(yielded));
                    reader.blocks[0].values.append(ValueID(0u));
                    reader.blocks[0].terminator.value = ValueID(0u);
                    made.append(WTF::move(reader));
                    made.append(WTF::move(general));
                    numbersOfCalls.append(0);
                    numbersOfCalls.append(1);
                }
                unsigned number = ofPair[found].made;
                ++numbersOfCalls[number];
                read.opcode = Opcode::CallDirect;
                read.children = { };
                read.immediates = { count + number, Format::none };
                block.values.removeAt(k - 1);
                --k;
            }
        }
    }
    for (unsigned number = 0; number < made.size(); ++number) {
        program.numberOfDirectCalls.append(numbersOfCalls[number]);
        program.functionsHotByText.set(count + number);
        program.functionsWarmByText.set(count + number);
        program.functions.append(WTF::move(made[number]));
    }
}

static void splitOffCallsOfGetStore(Program& program)
{
    auto getStore = program.find(asBytes("getStore"_s.span8()));
    if (!getStore)
        return;
    constexpr uint32_t flagsOfText = Format::FunctionIsTyped | Format::FunctionIsOfBuiltinModule | Format::FunctionIsSloppy;
    static constexpr OpcodeSet methodCalls { Opcode::CallMethod };
    unsigned count = program.functions.size();
    auto& functions = std::as_const(program).functions;
    Vector<Vector<ValueID, 0>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        if (function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin))
            return;
        functions.forEachValueInOrder(i, methodCalls, [&](ValueID id) {
            auto& call = function.at(id);
            if (call.string() == *getStore && call.arguments.isEmpty() && !call.goesBySignature && !program.at(function.at(call.child(0)).type).onlyView())
                sites[i].append(id);
        });
    });
    struct Reader {
        uint32_t flags;
        uint32_t numberOfCalls;
    };
    Vector<Reader, 4> readers;
    Vector<Function> made;
    Vector<uint32_t> readerCalledBy(FillWith { }, count, static_cast<uint32_t>(Format::none));
    for (unsigned i = 0; i < count; ++i) {
        if (sites[i].isEmpty())
            continue;
        auto& function = functions[i];
        uint32_t flags = function.flags & flagsOfText;
        size_t found = readers.findIf([&](auto& reader) { return reader.flags == flags; });
        if (found == notFound) {
            found = readers.size();
            readers.append(Reader { flags, 0 });
            auto& site = function.at(sites[i][0]);
            uint32_t numberOfGeneralReader = count + 2 * static_cast<uint32_t>(found) + 1;
            Function reader;
            reader.name = *getStore;
            reader.module = function.module;
            reader.position = site.position;
            reader.flags = flags | Format::FunctionIsHiddenInStackTraces;
            reader.readsAsyncLocalStorage = true;
            reader.isBodyOfSeveralSites = true;
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
            call.immediates = { numberOfGeneralReader, Format::none };
            call.arguments.append(ValueID(0u));
            reader.generalReaderOfAsyncLocalStorage = FunctionID(numberOfGeneralReader);
            reader.values.append(WTF::move(call));
            made.append(WTF::move(reader));
            made.append(WTF::move(general));
        }
        readers[found].numberOfCalls += sites[i].size();
        readerCalledBy[i] = count + 2 * static_cast<uint32_t>(found);
    }
    if (made.isEmpty())
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
            call.immediates = { readerCalledBy[i], Format::none };
            call.arguments = WTF::move(passed);
        }
    });
    for (unsigned number = 0; number < made.size(); ++number) {
        program.numberOfDirectCalls.append(number & 1 ? 1 : readers[number / 2].numberOfCalls);
        program.functionsHotByText.set(count + number);
        program.functionsWarmByText.set(count + number);
        program.functions.append(WTF::move(made[number]));
    }
}

// A function whose every `return` yields an object it has just made, of one kind, hands what the object would hold to the calls that only read that: see Function::resultTypes.
static void returnResultsInRegisters(Program& program)
{
    unsigned count = program.functions.size();
    struct Candidate : FreshResults {
        uint32_t worker { Format::none };
    };
    Vector<Candidate> found;
    // By function: where in `found`.
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidates;
    Vector<uint8_t> isCalled(FillWith { }, count, static_cast<uint8_t>(0));
    auto& functions = std::as_const(program.functions);
    static constexpr OpcodeSet directCalls { Opcode::CallDirect };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachValueInOrder(i, directCalls, [&](ValueID id) {
            if (!function.at(id).isJumpToContinuation())
                WTF::atomicStore(&isCalled[function.at(id).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    Vector<std::unique_ptr<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isCalled[i])
            return;
        if (auto fresh = freshResultsOf(program, functions[i]))
            freshResults[i] = makeUniqueWithoutFastMallocCheck<FreshResults>(WTF::move(*fresh));
    });
    for (unsigned i = 0; i < count; ++i) {
        if (auto& fresh = freshResults[i]; fresh && !fresh->nilKind) {
            candidates.add(i, found.size());
            found.append(Candidate { WTF::move(*fresh) });
        }
    }
    if (candidates.isEmpty())
        return;

    Vector<Vector<ValueID, 4>> callsOf(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        functions.forEachValueInOrder(i, directCalls, [&](ValueID id) {
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
            auto looked = readsOfFreshResult(program, function, call, candidate);
            if (!looked)
                continue;
            auto& [aliases, reads, nilTests] = *looked;
            if (candidate.worker == Format::none) {
                uint32_t original = function.at(call).function().index();
                Function worker = functions[original];
                for (auto& [b, held] : candidate.returns) {
                    auto& block = worker.blocks[b];
                    ValueID made = block.terminator.value;
                    block.values.removeAllMatching([&](ValueID id) {
                        return id == made || (worker.at(id).opcode == Opcode::PutField && worker.at(id).child(0) == made);
                    });
                    Value results;
                    results.opcode = Opcode::Results;
                    results.position = worker.at(made).position;
                    results.inlineFrame = worker.at(made).inlineFrame;
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
                concludeAllocationNotes(program, worker);
                candidate.worker = count + workers.size();
                if (program.isHotByText(FunctionID(original)))
                    program.functionsHotByText.set(candidate.worker);
                if (program.isWarmByText(FunctionID(original)))
                    program.functionsWarmByText.set(candidate.worker);
                if (program.isCalledNearLoops(FunctionID(original)))
                    program.functionsCalledNearLoops.set(candidate.worker);
                if (program.functionsInCycles.get(original))
                    program.functionsInCycles.set(candidate.worker);
                workers.append(WTF::move(worker));
            }
            auto& value = function.at(call);
            value.immediates[0] = candidate.worker;
            value.type = program.types.undefined;
            value.rep = repOf(program.at(value.type));
            value.takePredictionOf(Value { });
            for (auto [read, index] : reads) {
                auto& result = function.at(read);
                result.opcode = Opcode::ResultAt;
                result.children = { call, ValueID(), ValueID() };
                result.immediates = { index, Format::none };
                result.arguments.clear();
                // What is made is of that type, which says no less than the type of the read: `undefined|number` of an element that is there.
                result.type = candidate.types[index];
                result.rep = repOf(program.at(result.type));
                result.takePredictionOf(Value { });
            }
            for (ValueID test : nilTests) {
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

template<typename ForEachCallee>
static void findFunctionsEnteredWithRoomOnStack(Program& program, const BitVector& mayBeEnteredByWhoHasNotAsked, const ForEachCallee& forEachCallee)
{
    unsigned count = program.functions.size();
    BitVector isCandidate(count);
    for (unsigned i = 0; i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if (mayBeEnteredByWhoHasNotAsked.get(i) || program.functionsWithoutCalls.get(i) || function.blocks.isEmpty())
            continue;
        if (function.resumeFunction || function.startFunction || function.declaredAs & (functionIsAsync | functionIsGenerator) || function.has(FunctionIsHalfOfHiddenCoroutine) || function.has(FunctionHasUnknownCallers))
            continue;
        if (function.takesItsClosure || function.rampForClosures || function.isRampForClosures)
            continue;
        isCandidate.set(i);
    }
    for (FunctionID function : { program.moduleEvaluator, program.moduleEvaluatorIfNeeded, program.moduleEvaluatorFromOutside }) {
        if (function)
            isCandidate.clear(function.index());
    }
    for (FunctionID function : program.preludeFunctions) {
        if (function)
            isCandidate.clear(function.index());
    }
    Vector<Vector<uint32_t, 4>> callees(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isCandidate.get(i))
            return;
        static constexpr size_t mostCalleesToCompare = 16;
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> setOfCallees;
        forEachCallee(std::as_const(program).functions[i], [&](unsigned callee) {
            if (!isCandidate.get(callee))
                return;
            if (callees[i].size() < mostCalleesToCompare) {
                if (callees[i].contains(callee))
                    return;
            } else {
                if (setOfCallees.isEmpty())
                    setOfCallees.addAll(callees[i]);
                if (!setOfCallees.add(callee).isNewEntry)
                    return;
            }
            callees[i].append(callee);
        });
    });
    enum : uint8_t { NotSeen, UnderWay, Done };
    Vector<uint8_t> state(FillWith { }, count, static_cast<uint8_t>(NotSeen));
    Vector<uint8_t> height(FillWith { }, count, static_cast<uint8_t>(0));
    static_assert(maxFunctionsWithoutStackCheckOnEachOther < std::numeric_limits<uint8_t>::max());
    Vector<std::pair<uint32_t, uint32_t>> underWay;
    for (unsigned root = 0; root < count; ++root) {
        if (!isCandidate.get(root) || state[root] != NotSeen)
            continue;
        state[root] = UnderWay;
        underWay.append({ root, 0 });
        while (!underWay.isEmpty()) {
            auto [function, next] = underWay.last();
            if (next < callees[function].size()) {
                ++underWay.last().second;
                uint32_t callee = callees[function][next];
                if (state[callee] == UnderWay)
                    isCandidate.clear(callee);
                else if (state[callee] == NotSeen) {
                    state[callee] = UnderWay;
                    underWay.append({ callee, 0 });
                }
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : callees[function]) {
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, height[callee]);
            }
            if (below >= maxFunctionsWithoutStackCheckOnEachOther)
                isCandidate.clear(function);
            height[function] = isCandidate.get(function) ? below + 1 : 0;
            state[function] = Done;
            underWay.removeLast();
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", isCandidate.bitCount(), " functions are entered with room on the stack: they do not ask");
    program.generalEntriesWhoseFunctionAsks = BitVector(count);
    for (auto [index, entry] : program.genericEntrypoints) {
        if (!isCandidate.get(index))
            program.generalEntriesWhoseFunctionAsks.set(entry.index());
    }
    program.functionsEnteredWithRoomOnStack = WTF::move(isCandidate);
}

// Where every call of a function has taken its body in, nothing runs the function itself, nor what only it called. And what runs once: see Program::functionsThatRunOnce.
template<typename Functor, typename AlsoLookAt>
static void forEachCalleeOf(const Program& program, const Function& function, const Functor& functor, const AlsoLookAt& alsoLookAt)
{
    auto entryOf = [&](FunctionID made) {
        FunctionID entry = program.genericEntrypoints.get(made.index());
        if (!entry)
            return;
        functor(entry.index());
        if (FunctionID valueEntry = program.at(entry).valueEntry; valueEntry && program.isAskedForBySomeCall(valueEntry))
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
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            alsoLookAt(value);
            if (value.opcode == Opcode::CallDirect) {
                functor(value.function().index());
                // The lowering may hand a fresh rest array over in memory, to the general entry: Lowering::isPassedInMemory().
                if (program.at(value.function()).has(Format::FunctionHasRestParameter))
                    entryOf(value.function());
            } else if (FunctionID adapter = value.opcode != Opcode::Call ? FunctionID() : value.goesBySignature ? FunctionID(value.immediates[1]) : program.callsBySignatureAreChosen ? FunctionID() : program.adapterOfCall(value)) {
                functor(adapter.index());
            } else if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                entryOf(value.function());
            // `new F(..)` of a plain function: the run time makes the closure itself, for `F.prototype`, the first time the allocation is filled: State::fill().
            else if (value.opcode == Opcode::NewObjectWithShape) {
                if (FunctionID constructor = function.allocations[value.allocation()].constructedBy)
                    entryOf(constructor);
            } else if (value.opcode == Opcode::Construct && !function.isBodyOfSeveralSites) {
                if (FunctionID body = program.bodyOfConstruction(value))
                    functor(body.index());
            } else if (value.opcode == Opcode::Cast && !function.isBodyOfSeveralSites) {
                if (FunctionID body = program.bodyOfCast(value))
                    functor(body.index());
            }
            // The lowering may test for the closure and call either.
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
    forEachCalleeOf(program, function, functor, [](const Value&) { });
}

static BitVector functionsReachedOtherwise(const Program& program)
{
    unsigned count = program.functions.size();
    BitVector isReachedOtherwise(count);
    // The run time makes a closure of the constructor of every class and of every method. Of any other function only the program does, and whoever has the closure enters by the general entry.
    BitVector isMember(count);
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isMember.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isMember.set(method.function.index());
        }
    }
    // And of what the host or the engine asks for by a number of its own: evaluateBuiltinModule().
    for (auto& module : program.builtinModules)
        isMember.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isMember.set(builtin.function.index());
    BitVector isEnteredOnlyByClosures(count);
    Vector<std::pair<uint32_t, uint32_t>> functionsAndEntries;
    functionsAndEntries.reserveInitialCapacity(program.genericEntrypoints.size());
    for (auto [index, entry] : program.genericEntrypoints)
        functionsAndEntries.append({ index, entry.index() });
    Vector<uint8_t> isSoEntered(FillWith { }, functionsAndEntries.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(functionsAndEntries.size(), [&](unsigned k) {
        uint32_t index = functionsAndEntries[k].first;
        auto& function = program.functions[index];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        isSoEntered[k] = !(!isPlain || isMember.get(index) || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || function.resumeFunction || function.startFunction || program.isEntry(FunctionID(index)));
    });
    for (unsigned k = 0; k < functionsAndEntries.size(); ++k) {
        if (!isSoEntered[k])
            continue;
        isEnteredOnlyByClosures.set(functionsAndEntries[k].first);
        isEnteredOnlyByClosures.set(functionsAndEntries[k].second);
    }
    BitVector isReachedByCallsAlone(count);
    for (unsigned which = 0; which < program.preludeFunctions.size(); ++which) {
        if (program.preludeFunctions[which] && isChosenByCallbackHandedOver(static_cast<PreludeFunction>(which)))
            isReachedByCallsAlone.set(program.preludeFunctions[which].index());
    }
    Vector<uint8_t> isReached(FillWith { }, count, static_cast<uint8_t>(0));
    auto reach = [&](unsigned index) {
        WTF::atomicStore(&isReached[index], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || ((function.isGenericEntrypoint() || program.genericEntrypoints.contains(i)) && !isEnteredOnlyByClosures.get(i)) || (function.has(Format::FunctionIsBuiltin) && !isReachedByCallsAlone.get(i)) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)))
            reach(i);
        // Only its ramp calls the start of an async function.
        if (function.resumeFunction && (function.declaredAs & (functionIsAsync | functionIsGenerator)) != functionIsGenerator) {
            reach(i);
            reach(function.resumeFunction.index());
        }
        if (function.resultOfAdapter != Format::none && !program.callsBySignatureAreChosen)
            reach(i);
    });
    for (unsigned i = 0; i < count; ++i) {
        if (isReached[i])
            isReachedOtherwise.set(i);
    }
    for (auto [layout, serializer] : program.jsonSerializers)
        isReachedOtherwise.set(serializer.index());
    if (program.jsonDispatcher)
        isReachedOtherwise.set(program.jsonDispatcher.index());
    return isReachedOtherwise;
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

static void dropFunctionsWithoutCalls(Program& program)
{
    unsigned count = program.functions.size();
    BitVector isReachedOtherwise = functionsReachedOtherwise(program);
    Vector<uint32_t> calls;
    calls.fill(0, count);
    // How many values make a closure of it or name it.
    Vector<uint32_t> timesMadeAValue;
    timesMadeAValue.fill(0, count);
    Vector<Vector<uint32_t>> calleesOf(count);
    Vector<Vector<uint32_t>> functionsMadeValuesBy(count);
    Vector<Vector<uint32_t>> namesOfMethodsCalledBy(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        forEachCalleeOf(program, std::as_const(program).functions[i], [&](unsigned callee) {
            calleesOf[i].append(callee);
            WTF::atomicExchangeAdd(&calls[callee], 1u, std::memory_order_relaxed);
        }, [&](const Value& value) {
            if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce) {
                functionsMadeValuesBy[i].append(value.function().index());
                WTF::atomicExchangeAdd(&timesMadeAValue[value.function().index()], 1u, std::memory_order_relaxed);
            } else if (value.opcode == Opcode::CallMethod)
                namesOfMethodsCalledBy[i].append(value.string().index());
        });
    });
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        if (!calls[i] && !isReachedOtherwise.get(i))
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        for (unsigned i : worklist)
            program.functionsWithoutCalls.set(i);
        Vector<Vector<unsigned>> leftWithoutCalls(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            for (uint32_t callee : calleesOf[worklist[k]]) {
                if (WTF::atomicExchangeSub(&calls[callee], 1u, std::memory_order_relaxed) == 1 && !isReachedOtherwise.get(callee))
                    leftWithoutCalls[k].append(callee);
            }
            for (uint32_t made : functionsMadeValuesBy[worklist[k]])
                WTF::atomicExchangeSub(&timesMadeAValue[made], 1u, std::memory_order_relaxed);
        });
        worklist.shrink(0);
        for (auto& callees : leftWithoutCalls)
            worklist.appendVector(callees);
    }
    if (Options::verboseAOTCompilation() && program.licencesTheyAreNotGiven) [[unlikely]] {
        Vector<uint32_t> declined;
        declined.fill(0, count);
        std::array<uint64_t, 16> byLicence { };
        uint64_t sites = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (program.functionsWithoutCalls.get(i))
                continue;
            auto& function = std::as_const(program).functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (!function.at(id).isCallWhoseCopyWasDeclined())
                        continue;
                    ++declined[function.at(id).function().index()];
                    ++byLicence[function.at(id).licenceOfDeclinedCopy()];
                    ++sites;
                }
            }
        }
        uint64_t bodies = 0;
        for (unsigned i = 0; i < count; ++i)
            bodies += declined[i] && declined[i] == calls[i] && !isReachedOtherwise.get(i) && !program.functionsWithoutCalls.get(i);
        dataLog("AOT: no copies into cold callers: ", sites, " calls stay calls:");
        for (unsigned licence = 0; licence < licencesForMap.length(); ++licence) {
            if (byLicence[licence])
                dataLog(" ", licencesForMap[licence], " ", byLicence[licence]);
        }
        dataLogLn("; ", bodies, " functions are compiled only for such calls");
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.functionsWithoutCalls.get(i))
            return;
        removeBody(program.functions[i]);
        calleesOf[i].shrink(0);
        namesOfMethodsCalledBy[i].shrink(0);
        forEachCalleeOf(program, std::as_const(program).functions[i], [&](unsigned callee) {
            calleesOf[i].append(callee);
        });
    });
    BitVector mayBeEnteredByWhoHasNotAsked = isReachedOtherwise;
    BitVector isAskedForByNumber(count);
    for (auto& module : program.builtinModules)
        isAskedForByNumber.set(module.second.index());
    for (auto& builtin : program.builtinFunctions)
        isAskedForByNumber.set(builtin.function.index());
    auto isEnteredByCallsAndClosuresAlone = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method && function.kind != FunctionKind::Constructor)
            return false;
        return !isAskedForByNumber.get(i) && !function.has(Format::FunctionIsBuiltin) && !program.isResumeFunction(function) && !function.resumeFunction && !function.startFunction && !program.isEntry(FunctionID(i));
    };
    enum : uint8_t { IsEnteredByCallsAndClosuresAlone = 1, IsGeneralEntry = 2 };
    Vector<uint8_t> howEntered(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        howEntered[i] = (isEnteredByCallsAndClosuresAlone(i) ? IsEnteredByCallsAndClosuresAlone : 0) | (std::as_const(program).functions[i].isGenericEntrypoint() ? IsGeneralEntry : 0);
    });
    for (unsigned i = 0; i < count; ++i) {
        if (howEntered[i] == IsEnteredByCallsAndClosuresAlone)
            mayBeEnteredByWhoHasNotAsked.clear(i);
    }
    for (auto [index, entry] : program.genericEntrypoints) {
        if (howEntered[index] & IsEnteredByCallsAndClosuresAlone)
            mayBeEnteredByWhoHasNotAsked.clear(entry.index());
    }
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> methodsByName;
    for (auto& layout : program.layouts) {
        for (auto& method : layout.methods) {
            if (method.name && method.kind == MethodKind::Method)
                methodsByName.add(method.name.index(), Vector<uint32_t, 2> { }).iterator->value.append(method.function.index());
        }
    }
    findFunctionsEnteredWithRoomOnStack(program, mayBeEnteredByWhoHasNotAsked, [&](const Function& function, auto&& functor) {
        size_t i = &function - std::as_const(program).functions.begin();
        for (uint32_t callee : calleesOf[i])
            functor(callee);
        for (uint32_t name : namesOfMethodsCalledBy[i]) {
            if (auto found = methodsByName.find(name); found != methodsByName.end()) {
                for (uint32_t method : found->value)
                    functor(method);
            }
        }
    });

    // By now: a function expression that is called where it stands was a value when the general entries were made.
    BitVector isMadeAValue(count);
    for (unsigned i = 0; i < count; ++i) {
        if (timesMadeAValue[i])
            isMadeAValue.set(i);
    }
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isMadeAValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isMadeAValue.set(method.function.index());
        }
    }
    // How many CallDirects name it, but for that of a general entry nobody can enter.
    auto hasOneCall = [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || isMadeAValue.get(i) || function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)) || function.resumeFunction || function.startFunction)
            return false;
        uint32_t fromItsEntry = 0;
        if (auto entry = program.genericEntrypoints.find(i); entry != program.genericEntrypoints.end()) {
            for (uint32_t callee : calleesOf[entry->value.index()])
                fromItsEntry += callee == i;
        }
        return calls[i] - fromItsEntry == 1;
    };
    for (unsigned i = 0; i < count; ++i) {
        auto& function = std::as_const(program).functions[i];
        if ((function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i))) {
            program.functionsThatRunOnce.set(i);
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
                if (!program.functionsThatRunOnce.set(callee))
                    worklist.append(callee);
            }
        }
    }
    Vector<uint8_t> timesFetched;
    timesFetched.fill(0, program.strings.size());
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = std::as_const(program).functions[i];
        static constexpr OpcodeSet strings { Opcode::String };
        std::as_const(program).functions.forEachValueInOrder(i, strings, [&](ValueID id) {
            uint8_t* times = &timesFetched[function.at(id).string().index()];
            for (uint8_t seen = WTF::atomicLoad(times, std::memory_order_relaxed); seen < 2 && !WTF::atomicCompareExchangeWeakRelaxed(times, seen, static_cast<uint8_t>(seen + 1)); seen = WTF::atomicLoad(times, std::memory_order_relaxed)) { }
        });
    });
    Vector<unsigned> runOnce;
    for (size_t i : program.functionsThatRunOnce)
        runOnce.append(static_cast<unsigned>(i));
    Vector<Vector<uint32_t>> madeOnce(runOnce.size());
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
                if (function.at(id).opcode == Opcode::String && timesFetched[function.at(id).string().index()] == 1)
                    madeOnce[k].append(function.at(id).string().index());
            }
        }
    });
    for (auto& strings : madeOnce) {
        for (uint32_t string : strings)
            program.stringsMadeOnce.set(string);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.stringsMadeOnce.bitCount(), " of ", program.strings.size(), " strings are made once and not kept");
    destroyBeside(std::tuple { WTF::move(calleesOf), WTF::move(functionsMadeValuesBy), WTF::move(namesOfMethodsCalledBy) });
}

static void verifyExceptionsPassedOn(Program& program)
{
    std::atomic<bool> passesExceptionsOn { false };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        BitVector isPassedOn;
        for (unsigned index = 0; index < function.values.size(); ++index) {
            if (!function.values[index].yieldsExceptionItself())
                continue;
            RELEASE_ASSERT(function.values[index].type == program.types.anything);
            isPassedOn.set(index);
        }
        if (isPassedOn.isEmpty())
            return;
        passesExceptionsOn.store(true, std::memory_order_relaxed);
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
            unsigned numberThatCatch = 0;
            bool catchesExceptionItself = false;
            for (ValueID id : block.values) {
                function.at(id).forEachChild([&](ValueID child) {
                    RELEASE_ASSERT(!isPassedOn.get(child.index()));
                });
                if (function.at(id).opcode != Opcode::CatchValue)
                    continue;
                if (function.at(id).yieldsExceptionItself()) {
                    RELEASE_ASSERT(!numberThatCatch);
                    catchesExceptionItself = true;
                }
                ++numberThatCatch;
            }
            RELEASE_ASSERT(numberThatCatch <= 1u + catchesExceptionItself);
            auto& terminator = block.terminator;
            RELEASE_ASSERT(terminator.kind == TerminatorKind::Throw || !terminator.value || !isPassedOn.get(terminator.value.index()));
        }
    });
    program.passesExceptionsOn = passesExceptionsOn.load(std::memory_order_relaxed);
}

// Loads of a variable that is said to be stored once are taken not to change. Of the program as it was written: where a coroutine is split, what is
// behind a join may come to stand in two of its parts.
static void verifyVariablesStoredOnce(const Program& program)
{
    Vector<unsigned> numberOfStores;
    numberOfStores.fill(0, program.variables.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::PutModuleVariable)
                    WTF::atomicExchangeAdd(&numberOfStores[function.at(id).variable().index()], 1u, std::memory_order_relaxed);
            }
        }
    });
    for (unsigned i = 0; i < program.variables.size(); ++i)
        RELEASE_ASSERT(!program.variables[i].isStoredOnce || numberOfStores[i] <= 1);
}

static ValueID originThroughTrivialParameters(const Function& function, ValueID id)
{
    auto beforeCasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast && function.at(value).castReason() != CastReason::As && function.at(value).castReason() != CastReason::Coerce)
            value = function.at(value).child(0);
        return value;
    };
    for (unsigned steps = 0; steps < 8; ++steps) {
        id = beforeCasts(id);
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
                ValueID given = beforeCasts(target.arguments[value.immediates[1]]);
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

static void findPairsOfFunctionsHeldByEnvironments(Program& program)
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
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::PutField || value.layout().index() >= held.size() || value.field() >= held[value.layout().index()].size())
                    continue;
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
            }
        }
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
            if (ofField.isUnknown || !ofField.second || field.onlyFunction || field.flags & fieldHoldsClosureMadeOnce)
                continue;
            field.onlyFunction = ofField.first;
            field.otherFunction = ofField.second;
            ++numberOfPairs;
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPairs, " variables of environments hold closures of one of two functions");
}

static void dropExportsNobodyReads(Program& program)
{
    if (!program.usesOperation(Operations::MakeModuleNamespace))
        return;
    auto& asRead = std::as_const(program);
    unsigned count = asRead.functions.size();
    struct Namespace {
        FunctionID maker;
        FunctionID importer;
        FunctionID reader;
        VariableID variable;
        Vector<StringID> names;
        Vector<StringID> namesRead;
        Vector<StringID> methodsCalled;
        bool getsAway { false };
    };
    Vector<Namespace> namespaces;
    using NumberOfNamespace = UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    NumberOfNamespace byMaker;
    NumberOfNamespace byImporter;
    NumberOfNamespace byVariable;
    Vector<uint8_t> makesNamespace(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : asRead.functions[i].values) {
            if (value.opcode == Opcode::Operation && program.operationOf(value) == Operations::MakeModuleNamespace)
                makesNamespace[i] = 1;
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!makesNamespace[i])
            continue;
        auto& function = asRead.functions[i];
        Namespace made;
        made.maker = FunctionID(i);
        unsigned numberMade = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutModuleVariable && function.at(value.child(0)).opcode == Opcode::Operation && program.operationOf(function.at(value.child(0))) == Operations::MakeModuleNamespace)
                    made.variable = value.variable();
                if (value.opcode != Opcode::Operation || program.operationOf(value) != Operations::MakeModuleNamespace)
                    continue;
                ++numberMade;
                for (unsigned k = 0; k < value.arguments.size(); ++k) {
                    auto& operand = function.at(value.arguments[k]);
                    if (!k && operand.opcode == Opcode::FunctionReference)
                        made.reader = operand.function();
                    else if (k && operand.opcode == Opcode::String)
                        made.names.append(operand.string());
                    else
                        made.getsAway = true;
                }
            }
        }
        if (numberMade != 1 || !made.reader || !made.variable || function.kind != FunctionKind::Plain || !function.blocks[0].parameters.isEmpty())
            made.getsAway = true;
        byMaker.add(i, namespaces.size());
        if (made.variable)
            byVariable.add(made.variable.index(), namespaces.size());
        namespaces.append(WTF::move(made));
    }
    struct Uses {
        unsigned number { 0 };
        FunctionID user;
        Vector<std::pair<unsigned, StringID>, 4> namesRead;
        Vector<std::pair<unsigned, StringID>, 4> methodsCalled;
        Vector<VariableID, 1> keptIn;
        bool isAwaited { false };
        bool isReturned { false };
        bool getsAway { false };
    };
    auto promise = program.find(asBytes("Promise"_s.span8()));
    auto all = program.find(asBytes("all"_s.span8()));
    auto length = program.find(asBytes("length"_s.span8()));
    auto esModule = program.find(asBytes("__esModule"_s.span8()));
    auto nameOfThen = program.find(asBytes("then"_s.span8()));
    auto nameOfCatch = program.find(asBytes("catch"_s.span8()));
    auto nameOfFinally = program.find(asBytes("finally"_s.span8()));
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
        auto& function = asRead.at(id);
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
            Followed promiseMade;
        };
        Vector<Handler, 2> handlers;
        Vector<Followed, 2> returnedByHandlers;
        Vector<Followed, 4> worklist { Followed { startsIn, start, 0, Held::Itself, 0 } };
        Vector<Followed, 8> seen;
        auto resolve = [&](const Followed& promiseMade, const Followed& returned) {
            if (returned.as == Held::Itself)
                worklist.append(Followed { promiseMade.in, promiseMade.value, 0, Held::Itself, 0 });
            else if (returned.as == Held::List || returned.as == Held::PromiseOfList)
                worklist.append(Followed { promiseMade.in, promiseMade.value, returned.stage, Held::PromiseOfList, returned.index });
            else
                uses.getsAway = true;
        };
        while (!worklist.isEmpty() && !uses.getsAway) {
            Followed next = worklist.takeLast();
            ValueID held = next.value;
            unsigned stage = next.stage;
            if (seen.containsIf([&](auto& other) { return other.in == next.in && other.value == held && other.stage == stage && other.as == next.as && other.index == next.index; }))
                continue;
            seen.append(next);
            auto& function = asRead.at(next.in);
            auto withoutCasts = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() != CastReason::Coerce)
                    id = function.at(id).child(0);
                return id;
            };
            auto isThePromiseConstructor = [&](ValueID id) {
                return promise && function.at(id).opcode == Opcode::GetGlobal && function.at(id).string() == *promise;
            };
            bool isPromiseNow = next.as == Held::PromiseOfList || (next.as == Held::Itself && isPromise && !stage);
            auto isReaction = [&](StringID name) {
                return (nameOfThen && name == *nameOfThen) || (nameOfCatch && name == *nameOfCatch) || (nameOfFinally && name == *nameOfFinally);
            };
            auto& usesInFunction = usesIn(listed, next.in);
            auto isOnlyCalledOnWhatIsHeld = [&](ValueID member) {
                auto usesOfMember = usesInFunction.of(member);
                for (size_t at = 0; at < usesOfMember.size(); ++at) {
                    if (usesOfMember[at].user == usedByTerminator || (at + 1 < usesOfMember.size() && usesOfMember[at + 1].user == usesOfMember[at].user))
                        return false;
                    auto& user = function.at(ValueID(usesOfMember[at].user));
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
                Followed promiseMade { next.in, id, stage, next.as, next.index };
                if ((nameOfCatch && name == *nameOfCatch) || (nameOfFinally && name == *nameOfFinally)) {
                    worklist.append(promiseMade);
                    return true;
                }
                if (!nameOfThen || name != *nameOfThen || user.arguments.isEmpty())
                    return false;
                auto& given = function.at(withoutCasts(user.arguments[0]));
                if (given.opcode == Opcode::Undefined || given.opcode == Opcode::Null) {
                    worklist.append(promiseMade);
                    return true;
                }
                if (given.opcode != Opcode::FunctionReference && given.opcode != Opcode::NewClosure)
                    return false;
                auto& handler = asRead.at(given.function());
                if ((handler.kind != FunctionKind::Plain && handler.kind != FunctionKind::Arrow) || handler.has(Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsSloppy | Format::FunctionIsBuiltin) || handler.blocks.isEmpty())
                    return false;
                if (!handler.parameterTypes.isEmpty() && handler.parameterTypes[0] && !asRead.at(handler.parameterTypes[0]).views.isEmpty())
                    return false;
                if (handler.blocks[0].parameters.size() > handler.captureTypes.size())
                    worklist.append(Followed { given.function(), handler.blocks[0].parameters[handler.captureTypes.size()], next.as == Held::Itself ? 1 : stage, next.as == Held::Itself ? Held::Itself : Held::List, next.index });
                handlers.append(Handler { given.function(), promiseMade });
                for (auto& returned : returnedByHandlers) {
                    if (returned.in == given.function())
                        resolve(promiseMade, returned);
                }
                return true;
            };
            auto usesOfHeld = usesInFunction.of(held);
            for (size_t at = 0; at < usesOfHeld.size();) {
                unsigned numberOfBlock = usesOfHeld[at].block;
                auto& block = function.blocks[numberOfBlock];
                while (at < usesOfHeld.size() && usesOfHeld[at].block == numberOfBlock && usesOfHeld[at].user != usedByTerminator) {
                    ValueID id(usesOfHeld[at].user);
                    auto& user = function.at(id);
                    unsigned timesUsed = 0;
                    for (; at < usesOfHeld.size() && usesOfHeld[at].user == id.index(); ++at)
                        ++timesUsed;
                    switch (user.opcode) {
                    case Opcode::Cast: {
                        CastReason reason = user.castReason();
                        bool looksAtKindAlone = (reason == CastReason::As || reason == CastReason::Store) && asRead.at(user.type).views.isEmpty() && asRead.at(user.type).engineClasses.isEmpty();
                        if (reason == CastReason::Claim || reason == CastReason::Upcast || reason == CastReason::Flow || reason == CastReason::NonNull || looksAtKindAlone)
                            worklist.append(Followed { next.in, id, stage, next.as, next.index });
                        else
                            uses.getsAway = true;
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
                    if (isPromiseNow && user.opcode == Opcode::GetById && isReaction(user.string()) && isOnlyCalledOnWhatIsHeld(id))
                        continue;
                    switch (next.as) {
                    case Held::Itself:
                        break;
                    case Held::ListOfPromises:
                        if (all && user.opcode == Opcode::Call && timesUsed == 1 && user.arguments.size() == 1 && user.arguments[0] == held && isThePromiseConstructor(user.child(1))
                            && function.at(user.child(0)).opcode == Opcode::GetById && function.at(user.child(0)).string() == *all && isThePromiseConstructor(function.at(user.child(0)).child(0)))
                            worklist.append(Followed { next.in, id, stage, Held::PromiseOfList, next.index });
                        else
                            uses.getsAway = true;
                        continue;
                    case Held::PromiseOfList:
                        if (user.opcode == Opcode::Await)
                            worklist.append(Followed { next.in, id, stage, Held::List, next.index });
                        else
                            uses.getsAway = true;
                        continue;
                    case Held::List:
                        if (user.opcode == Opcode::GetByVal && timesUsed == 1 && user.child(0) == held && function.at(user.child(1)).opcode == Opcode::Integer) {
                            if (function.at(user.child(1)).integer() == static_cast<int64_t>(next.index))
                                worklist.append(Followed { next.in, id, 1, Held::Itself, 0 });
                        } else if (user.opcode == Opcode::IteratorOpen)
                            worklist.append(Followed { next.in, id, stage, Held::IteratorOfList, next.index });
                        else if (user.opcode != Opcode::GetById || !length || user.string() != *length)
                            uses.getsAway = true;
                        continue;
                    case Held::IteratorOfList:
                        if (user.opcode == Opcode::IteratorValue && timesUsed == 1 && user.child(0) == held && function.at(user.child(1)).opcode == Opcode::Integer) {
                            if (function.at(user.child(1)).integer() == static_cast<int64_t>(next.index))
                                worklist.append(Followed { next.in, id, 1, Held::Itself, 0 });
                        } else if (user.opcode != Opcode::IteratorHasNext && user.opcode != Opcode::IteratorClose)
                            uses.getsAway = true;
                        continue;
                    }
                    switch (user.opcode) {
                    case Opcode::Await:
                        uses.isAwaited = true;
                        if (stage > 1)
                            uses.getsAway = true;
                        else
                            worklist.append(Followed { next.in, id, stage + 1, Held::Itself, 0 });
                        break;
                    case Opcode::GetById:
                        uses.namesRead.append({ stage, user.string() });
                        break;
                    case Opcode::CallMethod:
                        if (timesUsed == 1 && user.child(0) == held)
                            uses.methodsCalled.append({ stage, user.string() });
                        else
                            uses.getsAway = true;
                        break;
                    case Opcode::Call: {
                        ValueID callee = withoutCasts(user.child(0));
                        if (timesUsed == 1 && user.child(1) == held && function.at(callee).opcode == Opcode::GetById && function.at(callee).child(0) == held)
                            uses.methodsCalled.append({ stage, function.at(callee).string() });
                        else
                            uses.getsAway = true;
                        break;
                    }
                    case Opcode::Operation:
                        if (esModule && program.operationOf(user) == Operations::DefineField && timesUsed == 1 && user.arguments.size() == 3 && user.arguments[0] == held
                            && function.at(user.arguments[1]).opcode == Opcode::String && function.at(user.arguments[1]).string() == *esModule && function.at(user.arguments[2]).opcode == Opcode::Boolean)
                            worklist.append(Followed { next.in, id, stage, Held::Itself, 0 });
                        else
                            uses.getsAway = true;
                        break;
                    case Opcode::PutModuleVariable:
                        if (stage == (isPromise ? 1u : 0u))
                            uses.keptIn.append(user.variable());
                        else
                            uses.getsAway = true;
                        break;
                    case Opcode::NewArray:
                        if (!isPromise || stage) {
                            uses.getsAway = true;
                            break;
                        }
                        for (unsigned k = 0; k < user.arguments.size(); ++k) {
                            if (user.arguments[k] == held)
                                worklist.append(Followed { next.in, id, stage, Held::ListOfPromises, k });
                        }
                        break;
                    default:
                        uses.getsAway = true;
                        break;
                    }
                }
                while (at < usesOfHeld.size() && usesOfHeld[at].block == numberOfBlock)
                    ++at;
                auto& terminator = block.terminator;
                if (terminator.value == held) {
                    if (terminator.kind == TerminatorKind::Return && next.in != startsIn) {
                        returnedByHandlers.append(next);
                        for (auto& handler : handlers) {
                            if (handler.function == next.in)
                                resolve(handler.promiseMade, next);
                        }
                    } else if (terminator.kind == TerminatorKind::Return && next.as == Held::Itself)
                        uses.isReturned = true;
                    else if (terminator.kind != TerminatorKind::Branch)
                        uses.getsAway = true;
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
    auto forEachUse = [&](Opcode yields, const NumberOfNamespace& byWhatYieldsIt, bool isPromise, auto&& functor) {
        Vector<Vector<Uses>> usesIn(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = asRead.functions[i];
            UsesByFunction listed;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != yields)
                        continue;
                    auto found = byWhatYieldsIt.find(value.immediates[0]);
                    if (found == byWhatYieldsIt.end())
                        continue;
                    Uses uses;
                    uses.number = found->value;
                    uses.user = FunctionID(i);
                    usesOf(FunctionID(i), id, uses, isPromise, listed);
                    usesIn[i].append(WTF::move(uses));
                }
            }
        });
        for (auto& usesInFunction : usesIn) {
            for (auto& uses : usesInFunction)
                functor(uses);
        }
    };
    auto note = [&](Namespace& made, const Uses& uses, unsigned stageOfObject) {
        made.getsAway |= uses.getsAway || uses.isReturned;
        for (auto* list : { &uses.namesRead, &uses.methodsCalled }) {
            for (auto [stage, name] : *list) {
                if (stage != stageOfObject)
                    made.getsAway = true;
                else
                    (list == &uses.namesRead ? made.namesRead : made.methodsCalled).append(name);
            }
        }
    };
    NumberOfNamespace byVariableItIsKeptIn;
    NumberOfNamespace newlyKept;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> storesFollowed;
    auto noteWhereItIsKept = [&](const Uses& uses) {
        for (VariableID variable : uses.keptIn) {
            auto result = byVariableItIsKeptIn.add(variable.index(), uses.number);
            if (result.isNewEntry)
                newlyKept.add(variable.index(), uses.number);
            else if (result.iterator->value != uses.number) {
                namespaces[uses.number].getsAway = true;
                namespaces[result.iterator->value].getsAway = true;
            }
            ++storesFollowed.add(variable.index(), 0).iterator->value;
        }
    };
    forEachUse(Opcode::CallDirect, byMaker, false, [&](const Uses& uses) {
        auto& made = namespaces[uses.number];
        auto& user = asRead.at(uses.user);
        bool isOnlyReturned = uses.isReturned && !uses.getsAway && !uses.isAwaited && uses.namesRead.isEmpty() && uses.methodsCalled.isEmpty() && uses.keptIn.isEmpty();
        if (isOnlyReturned && !made.importer && user.kind == FunctionKind::Plain && user.has(Format::FunctionIsAsync) && user.has(Format::FunctionIsHiddenInStackTraces) && !user.has(Format::FunctionIsUsedAsValue) && user.blocks[0].parameters.isEmpty() && !asRead.isEntry(uses.user)) {
            made.importer = uses.user;
            byImporter.add(uses.user.index(), uses.number);
            return;
        }
        made.getsAway |= uses.isAwaited;
        note(made, uses, 0);
        noteWhereItIsKept(uses);
    });
    forEachUse(Opcode::CallDirect, byImporter, true, [&](const Uses& uses) {
        note(namespaces[uses.number], uses, 1);
        noteWhereItIsKept(uses);
    });
    forEachUse(Opcode::GetModuleVariable, byVariable, false, [&](const Uses& uses) {
        auto& made = namespaces[uses.number];
        if (uses.user == made.maker)
            return;
        made.getsAway |= uses.isAwaited;
        note(made, uses, 0);
        noteWhereItIsKept(uses);
    });
    for (unsigned round = 0; round < 3 && !newlyKept.isEmpty(); ++round) {
        forEachUse(Opcode::GetModuleVariable, std::exchange(newlyKept, NumberOfNamespace()), false, [&](const Uses& uses) {
            namespaces[uses.number].getsAway |= uses.isAwaited;
            note(namespaces[uses.number], uses, 0);
            noteWhereItIsKept(uses);
        });
    }
    for (auto& entry : newlyKept)
        namespaces[entry.value].getsAway = true;
    if (!byVariableItIsKeptIn.isEmpty()) {
        Vector<uint32_t> numberOfStores(FillWith { }, asRead.variables.size(), 0u);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = asRead.functions[i];
            for (auto& value : function.values) {
                if (value.opcode != Opcode::PutModuleVariable || !byVariableItIsKeptIn.contains(value.variable().index()))
                    continue;
                Opcode stored = function.at(value.child(0)).opcode;
                if (stored != Opcode::Undefined && stored != Opcode::Null)
                    WTF::atomicExchangeAdd(&numberOfStores[value.variable().index()], 1u, std::memory_order_relaxed);
            }
        });
        for (auto& entry : byVariableItIsKeptIn) {
            if (numberOfStores[entry.key] != storesFollowed.get(entry.key))
                namespaces[entry.value].getsAway = true;
        }
    }
    unsigned numberThatStay = 0;
    unsigned numberOfExports = 0;
    unsigned numberNobodyReads = 0;
    unsigned numberOfFunctions = 0;
    unsigned numberOfVariables = 0;
    unsigned numberOfClasses = 0;
    auto report = [&] {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", namespaces.size(), " namespace objects of modules are made; ", numberThatStay, " of them are only read by constant names: of their ", numberOfExports, " exports nobody reads ", numberNobodyReads, "; ", numberOfFunctions, " functions that nothing else names are dropped, those of ", numberOfClasses, " classes among them; ", numberOfVariables, " module variables that nothing reads any more are not stored to");
    };
    if (!namespaces.containsIf([](auto& made) { return !made.getsAway; })) {
        report();
        return;
    }
    Vector<uint8_t> isNamedOtherwise(FillWith { }, namespaces.size(), static_cast<uint8_t>(0));
    BitVector makesOrImports(count);
    for (auto* table : { &byMaker, &byImporter }) {
        for (auto& entry : *table)
            makesOrImports.set(entry.key);
    }
    BitVector holdsNamespace(asRead.variables.size());
    for (auto& entry : byVariable)
        holdsNamespace.set(entry.key);
    auto armOf = [&](const Namespace& made, const Function& reader, StringID name) -> std::optional<unsigned> {
        auto& terminator = reader.blocks[0].terminator;
        size_t index = made.names.find(name);
        size_t which = index == notFound ? notFound : terminator.cases.find(static_cast<int64_t>(index));
        return which == notFound ? std::nullopt : std::optional<unsigned>(terminator.targets[which].block.index());
    };
    auto variableYieldedBy = [&](const Function& reader, unsigned arm) -> VariableID {
        VariableID yielded;
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
                    if (yielded && yielded != value.variable())
                        return { };
                    yielded = value.variable();
                    break;
                case Opcode::CallDirect:
                    if (asRead.at(value.function()).kind != FunctionKind::Module || !value.arguments.isEmpty())
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
            auto& maker = asRead.at(id);
            for (auto& block : maker.blocks) {
                if (block.terminator.kind != TerminatorKind::Return)
                    continue;
                ValueID returned = block.terminator.value;
                while (returned && maker.at(returned).opcode == Opcode::Cast && maker.at(returned).castReason() != CastReason::Coerce)
                    returned = maker.at(returned).child(0);
                if (!returned || maker.at(returned).opcode != Opcode::GetModuleVariable || maker.at(returned).variable() != yielded)
                    return { };
            }
        }
        return yielded;
    };
    BitVector isCalledOnNamespace(asRead.variables.size());
    for (auto& made : namespaces) {
        if (made.getsAway || !made.reader || asRead.at(made.reader).blocks.isEmpty() || asRead.at(made.reader).blocks[0].terminator.kind != TerminatorKind::Switch)
            continue;
        for (StringID name : made.methodsCalled) {
            if (auto arm = armOf(made, asRead.at(made.reader), name)) {
                if (VariableID yielded = variableYieldedBy(asRead.at(made.reader), *arm))
                    isCalledOnNamespace.set(yielded.index());
            }
        }
    }
    Vector<uint8_t> mayHoldWhatReadsThis(FillWith { }, asRead.variables.size(), static_cast<uint8_t>(0));
    auto countNamesIn = [&](unsigned i, Vector<uint32_t>& timesNamed) {
        auto& function = asRead.functions[i];
        auto name = [&](FunctionID named, bool isValue) {
            if (!named)
                return;
            ++timesNamed[named.index()];
            if (!isValue || !makesOrImports.get(named.index()))
                return;
            for (auto* table : { &byMaker, &byImporter }) {
                if (auto found = table->find(named.index()); found != table->end())
                    WTF::atomicStore(&isNamedOtherwise[found->value], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.parameters)
                name(function.at(id).predictedClosure(), true);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                name(value.predictedClosure(), true);
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
                        if (isClosure ? asRead.at(function.at(stored).function()).has(Format::FunctionHasThis) : opcode != Opcode::Undefined && opcode != Opcode::Null)
                            WTF::atomicStore(&mayHoldWhatReadsThis[value.variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                    }
                    if (!holdsNamespace.get(value.variable().index()))
                        break;
                    if (auto found = byVariable.find(value.variable().index()); value.opcode == Opcode::PutModuleVariable && namespaces[found->value].maker != FunctionID(i))
                        WTF::atomicStore(&isNamedOtherwise[found->value], static_cast<uint8_t>(1), std::memory_order_relaxed);
                    break;
                default:
                    break;
                }
            }
        }
    };
    constexpr unsigned numberOfParts = 16;
    Vector<Vector<uint32_t>> timesNamedInPart(numberOfParts);
    forEachIndexInParallel(numberOfParts, [&](unsigned part) {
        timesNamedInPart[part].fill(0, count);
        for (unsigned i = part; i < count; i += numberOfParts)
            countNamesIn(i, timesNamedInPart[part]);
    });
    Vector<uint32_t> timesNamed = WTF::move(timesNamedInPart[0]);
    for (unsigned part = 1; part < numberOfParts; ++part) {
        for (unsigned i = 0; i < count; ++i)
            timesNamed[i] += timesNamedInPart[part][i];
    }
    BitVector isEnteredOtherwise(count);
    for (auto& layout : asRead.layouts) {
        if (layout.constructor)
            isEnteredOtherwise.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isEnteredOtherwise.set(method.function.index());
        }
    }
    for (auto& module : asRead.builtinModules)
        isEnteredOtherwise.set(module.second.index());
    for (auto& builtin : asRead.builtinFunctions)
        isEnteredOtherwise.set(builtin.function.index());
    for (FunctionID function : asRead.preludeFunctions) {
        if (function)
            isEnteredOtherwise.set(function.index());
    }
    for (auto& [property, key, function] : asRead.functionsByKey)
        isEnteredOtherwise.set(function.index());
    for (auto& module : asRead.modules) {
        if (module.initializer)
            isEnteredOtherwise.set(module.initializer.index());
    }
    auto then = program.find(asBytes("then"_s.span8()));
    Vector<unsigned> maybeDead;
    BitVector mayBeReadNoMore(asRead.variables.size());
    for (unsigned n = 0; n < namespaces.size(); ++n) {
        auto& made = namespaces[n];
        if (made.getsAway || isNamedOtherwise[n] || (then && made.names.contains(*then)))
            continue;
        bool isReachedFromOutside = false;
        for (FunctionID function : { made.maker, made.importer, made.reader }) {
            if (function)
                isReachedFromOutside |= asRead.isEntry(function) || isEnteredOtherwise.get(function.index()) || asRead.at(function).has(FunctionHasUnknownCallers | FunctionIsEnteredByValue);
        }
        if (isReachedFromOutside)
            continue;
        auto& reader = program.at(made.reader);
        if (timesNamed[made.reader.index()] != 1 || reader.blocks.isEmpty() || reader.blocks[0].terminator.kind != TerminatorKind::Switch)
            continue;
        unsigned refusal = reader.blocks.size();
        reader.blocks.append(Block { });
        auto& terminator = reader.blocks[0].terminator;
        auto blockOf = [&](StringID name) {
            return armOf(made, reader, name);
        };
        bool handsItselfOver = false;
        for (StringID name : made.methodsCalled) {
            auto block = blockOf(name);
            if (!block)
                continue;
            auto& values = reader.blocks[*block].values;
            if (values.size() == 1 && reader.at(values[0]).opcode == Opcode::FunctionReference) {
                handsItselfOver |= asRead.at(reader.at(values[0]).function()).has(Format::FunctionHasThis);
                continue;
            }
            VariableID yielded = variableYieldedBy(reader, *block);
            handsItselfOver |= !yielded || mayHoldWhatReadsThis[yielded.index()];
        }
        if (handsItselfOver) {
            reader.blocks.removeLast();
            continue;
        }
        ++numberThatStay;
        numberOfExports += made.names.size();
        BitVector isRead(reader.blocks.size());
        isRead.set(0);
        isRead.set(terminator.targets.last().block.index());
        for (auto* list : { &made.namesRead, &made.methodsCalled }) {
            for (StringID name : *list) {
                if (auto block = blockOf(name))
                    isRead.set(*block);
            }
        }
        for (unsigned which = 0; which < terminator.cases.size(); ++which) {
            auto& target = terminator.targets[which];
            if (isRead.get(target.block.index()) || !target.arguments.isEmpty())
                continue;
            ++numberNobodyReads;
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
        auto nameNoMore = [&](FunctionID named) {
            if (!named)
                return;
            --timesNamed[named.index()];
            maybeDead.append(named.index());
        };
        for (unsigned block = 0; block < refusal; ++block) {
            if (isReached.get(block))
                continue;
            for (ValueID id : reader.blocks[block].parameters)
                nameNoMore(reader.at(id).predictedClosure());
            for (ValueID id : reader.blocks[block].values) {
                auto& value = reader.at(id);
                nameNoMore(value.predictedClosure());
                if (value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::CallDirect || value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::GetCapture)
                    nameNoMore(value.function());
                else if (value.opcode == Opcode::NewObjectWithShape && value.allocation() < reader.allocations.size())
                    nameNoMore(reader.allocations[value.allocation()].constructedBy);
                else if (value.opcode == Opcode::GetModuleVariable)
                    mayBeReadNoMore.set(value.variable().index());
            }
            reader.blocks[block] = Block { };
        }
        bool isRefusalReached = false;
        for (auto& target : terminator.targets)
            isRefusalReached |= target.block.index() == refusal;
        if (!isRefusalReached) {
            reader.blocks.removeLast();
            continue;
        }
        StringID nameOfError = program.intern(asBytes("Error"_s.span8()));
        StringID refusalText = program.intern(asBytes("An export that nothing in the program reads was read from the namespace object of its module"_s.span8()));
        auto append = [&](Opcode opcode, TypeID type) -> Value& {
            Value value;
            value.opcode = opcode;
            value.position = reader.position;
            value.type = type;
            value.rep = repOf(asRead.at(type));
            reader.blocks[refusal].values.append(ValueID(reader.values.size()));
            reader.values.append(WTF::move(value));
            return reader.values.last();
        };
        append(Opcode::GetGlobal, asRead.types.anything).immediates[0] = nameOfError.index();
        append(Opcode::String, asRead.types.string).immediates[0] = refusalText.index();
        auto& construct = append(Opcode::Construct, asRead.types.anything);
        construct.immediates = { Format::none, Format::none };
        construct.children[0] = ValueID(reader.values.size() - 3);
        construct.arguments.append(ValueID(reader.values.size() - 2));
        reader.blocks[refusal].terminator.kind = TerminatorKind::Throw;
        reader.blocks[refusal].terminator.value = ValueID(reader.values.size() - 1);
    }
    auto forEachFunctionNamedIn = [&](const Function& function, auto&& functor) {
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
    auto isOfTheHost = [&](unsigned i) {
        return asRead.isEntry(FunctionID(i)) || asRead.functions[i].has(Format::FunctionIsBuiltin | Format::FunctionIsCodeOfModule | Format::FunctionIsGeneralEntry | FunctionHasUnknownCallers);
    };
    auto drop = [&](unsigned i) {
        auto& function = program.functions[i];
        ++numberOfFunctions;
        forEachFunctionNamedIn(function, [&](FunctionID other) {
            --timesNamed[other.index()];
            maybeDead.append(other.index());
        });
        removeBody(function);
        function.flags &= ~static_cast<uint32_t>(Format::FunctionIsUsedAsValue);
    };
    Vector<unsigned> constructorsNamedLess;
    auto dropWhatNothingNames = [&] {
        while (!maybeDead.isEmpty()) {
            unsigned i = maybeDead.takeLast();
            auto& function = asRead.functions[i];
            if (function.kind == FunctionKind::Constructor)
                constructorsNamedLess.append(i);
            bool isEnteredByNameAlone = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Module;
            if (!timesNamed[i] && isEnteredByNameAlone && hasBody(function) && !isEnteredOtherwise.get(i) && !isOfTheHost(i))
                drop(i);
        }
    };
    dropWhatNothingNames();
    if (!constructorsNamedLess.isEmpty()) {
        NumberOfNamespace layoutOfConstructor;
        BitVector isExtended(asRead.layouts.size());
        BitVector isConstructorOfParent(count);
        for (unsigned k = 0; k < asRead.layouts.size(); ++k) {
            auto& layout = asRead.layouts[k];
            if (layout.constructor && !layoutOfConstructor.add(layout.constructor.index(), k).isNewEntry)
                isConstructorOfParent.set(layout.constructor.index());
            if (layout.base)
                isExtended.set(layout.base.index());
            if (layout.declaredParentConstructor)
                isConstructorOfParent.set(layout.declaredParentConstructor.index());
        }
        while (!constructorsNamedLess.isEmpty()) {
            unsigned constructor = constructorsNamedLess.takeLast();
            auto found = layoutOfConstructor.find(constructor);
            if (found == layoutOfConstructor.end() || isExtended.get(found->value) || isConstructorOfParent.get(constructor) || !hasBody(asRead.functions[constructor]))
                continue;
            auto& layout = asRead.layouts[found->value];
            Vector<unsigned, 16> members { constructor };
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (!members.contains(method.function.index()))
                        members.append(method.function.index());
                }
            }
            Vector<uint32_t, 16> timesNamedFromWithin(FillWith { }, members.size(), 0u);
            for (unsigned member : members) {
                forEachFunctionNamedIn(asRead.functions[member], [&](FunctionID other) {
                    if (size_t which = members.find(other.index()); which != notFound)
                        ++timesNamedFromWithin[which];
                });
            }
            bool isNamedFromOutside = false;
            for (unsigned which = 0; which < members.size(); ++which)
                isNamedFromOutside |= timesNamed[members[which]] != timesNamedFromWithin[which] || isOfTheHost(members[which]);
            if (isNamedFromOutside)
                continue;
            ++numberOfClasses;
            for (unsigned member : members) {
                if (hasBody(asRead.functions[member]))
                    drop(member);
            }
            dropWhatNothingNames();
        }
    }
    if (!mayBeReadNoMore.isEmpty()) {
        Vector<uint8_t> isRead(FillWith { }, asRead.variables.size(), static_cast<uint8_t>(0));
        Vector<uint8_t> storesToOne(FillWith { }, count, static_cast<uint8_t>(0));
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = asRead.functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if ((value.opcode != Opcode::GetModuleVariable && value.opcode != Opcode::PutModuleVariable) || !mayBeReadNoMore.get(value.variable().index()))
                        continue;
                    if (value.opcode == Opcode::GetModuleVariable)
                        isRead[value.variable().index()] = 1;
                    else
                        storesToOne[i] = 1;
                }
            }
        });
        for (unsigned variable : mayBeReadNoMore)
            numberOfVariables += !isRead[variable];
        forEachIndexInParallel(count, [&](unsigned i) {
            if (!storesToOne[i])
                return;
            auto& function = program.functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::PutModuleVariable || !mayBeReadNoMore.get(value.variable().index()) || isRead[value.variable().index()])
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

static void findFunctionsExportedByInternalModules(Program& program)
{
    auto& asRead = std::as_const(program);
    if (!asRead.usesOperation(Operations::RequireBuiltinModule) || asRead.builtinModules.isEmpty())
        return;
    unsigned count = asRead.functions.size();
    struct Place {
        unsigned block { 0 };
        unsigned index { 0 };
    };
    struct Exports {
        FunctionID code;
        Place placeOfReturned;
        ValueID returned;
        VariableID holder;
        bool isInternal { false };
        bool getsAway { false };
    };
    using ByNumber = UncheckedKeyHashMap<uint32_t, Exports, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByNumber exportsOf;
    BitVector isHolder(asRead.variables.size());
    for (auto& [module, id] : asRead.builtinModules) {
        auto& function = asRead.at(id);
        if (!function.module)
            continue;
        Exports exports;
        exports.code = id;
        exports.isInternal = spanHasPrefix(asRead.string(asRead.modules[function.module.index()].path), asBytes("internal:"_s.span8()));
        unsigned numberOfReturns = 0;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (function.blocks[b].terminator.kind != TerminatorKind::Return)
                continue;
            ++numberOfReturns;
            exports.returned = function.blocks[b].terminator.value;
            exports.placeOfReturned = { b, UINT_MAX };
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

    auto moduleRequiredBy = [&](const Function& function, const Value& value) -> std::optional<uint32_t> {
        if (value.opcode != Opcode::Operation || asRead.operationOf(value) != Operations::RequireBuiltinModule || value.arguments.size() != 1)
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
        Vector<uint32_t> modulesThatGetAway;
        Vector<std::pair<ValueID, uint32_t>> required;
    };
    Vector<Found> foundIn(count);
    Vector<uint32_t> numberOfStores(FillWith { }, asRead.variables.size(), 0u);
    Vector<uint32_t> numberOfReads(FillWith { }, asRead.variables.size(), 0u);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        Vector<std::pair<ValueID, uint32_t>, 8> required;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutModuleVariable && isHolder.get(value.variable().index()))
                    WTF::atomicExchangeAdd(&numberOfStores[value.variable().index()], 1u, std::memory_order_relaxed);
                else if (value.opcode == Opcode::GetModuleVariable && isHolder.get(value.variable().index()))
                    WTF::atomicExchangeAdd(&numberOfReads[value.variable().index()], 1u, std::memory_order_relaxed);
                else if (auto module = moduleRequiredBy(function, value))
                    required.append({ id, *module });
            }
        }
        if (required.isEmpty())
            return;
        auto& found = foundIn[i];
        found.required.appendVector(required);
        auto moduleYielding = [&](ValueID used) -> std::optional<uint32_t> {
            for (auto [exports, module] : required) {
                if (exports == used)
                    return module;
            }
            return std::nullopt;
        };
        auto getsAway = [&](ValueID used) {
            if (auto module = moduleYielding(used))
                found.modulesThatGetAway.append(*module);
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
                    auto moduleOfReceiver = receiver ? moduleYielding(receiver) : std::nullopt;
                    if (moduleOfReceiver && !value.arguments.contains(receiver) && (isByName || (function.at(named).opcode == Opcode::GetById && function.at(named).child(0) == receiver))) {
                        found.methodsCalled.append(Read { named, *moduleOfReceiver });
                        for (ValueID argument : value.arguments)
                            getsAway(argument);
                        continue;
                    }
                }
                if (value.opcode != Opcode::GetById) {
                    value.forEachChild(getsAway);
                    continue;
                }
                auto module = moduleYielding(value.child(0));
                if (module && (block.terminator.kind != TerminatorKind::Invoke || k + 1 < block.values.size()))
                    found.reads.append(Read { block.values[k], *module });
            }
            if (block.terminator.value)
                getsAway(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    getsAway(argument);
            }
        }
    });
    for (auto& found : foundIn) {
        for (uint32_t module : found.modulesThatGetAway) {
            if (auto exports = exportsOf.find(module); exports != exportsOf.end())
                exports->value.getsAway = true;
        }
    }

    auto isOnEveryWayTo = [](const Function& function, Place first, Place second) {
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
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsYielded;
    auto withoutUpcasts = [](const Function& function, ValueID id) {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
            id = function.at(id).child(0);
        return id;
    };
    unsigned numberOfModules = 0;
    for (auto& [module, exports] : exportsOf) {
        auto& function = asRead.at(exports.code);
        ValueID object = exports.returned;
        ValueID storeOfObject;
        if (exports.holder) {
            if (numberOfStores[exports.holder.index()] != 1)
                continue;
            Place placeOfStore;
            Place placeOfRead;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& values = function.blocks[b].values;
                for (unsigned k = 0; k < values.size(); ++k) {
                    if (values[k] == exports.returned)
                        placeOfRead = { b, k };
                    else if (function.at(values[k]).opcode == Opcode::PutModuleVariable && function.at(values[k]).variable() == exports.holder) {
                        storeOfObject = values[k];
                        placeOfStore = { b, k };
                    }
                }
            }
            if (!storeOfObject || !isOnEveryWayTo(function, placeOfStore, placeOfRead))
                continue;
            object = function.at(storeOfObject).child(0);
        }
        if (auto& yielded = function.at(withoutUpcasts(function, object)); yielded.opcode == Opcode::FunctionReference) {
            functionsYielded.add(module, yielded.function());
            continue;
        }
        if (!exports.isInternal || exports.getsAway || (exports.holder && numberOfReads[exports.holder.index()] != 1))
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
                if (!uses || block.values[k] == storeOfObject)
                    continue;
                StringID name;
                ValueID stored;
                if (value.opcode == Opcode::Operation && asRead.operationOf(value) == Operations::DefineField && value.arguments.size() == 3 && value.arguments[0] == object && function.at(value.arguments[1]).opcode == Opcode::String) {
                    name = function.at(value.arguments[1]).string();
                    stored = value.arguments[2];
                } else if (value.opcode == Opcode::PutField && value.child(0) == object && value.layout().index() < asRead.layouts.size() && value.field() < asRead.at(value.layout()).fields.size()) {
                    name = asRead.at(value.layout()).fields[value.field()].name;
                    stored = value.child(1);
                }
                if (!name || !stored || stored == object || !isOnEveryWayTo(function, Place { b, k }, exports.placeOfReturned))
                    isOnlyFilledAndReturned = false;
                else
                    filled.append({ name, stored });
            }
            for (auto& target : block.terminator.targets)
                isOnlyFilledAndReturned &= !target.arguments.contains(object);
            if (block.terminator.value == object && (block.terminator.kind != TerminatorKind::Return || storeOfObject))
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
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> modulesHandedOver;
    for (unsigned i = 0; i < count; ++i) {
        for (auto& call : foundIn[i].methodsCalled) {
            FunctionID callee = functionsExported.get(keyOf(call.module, asRead.functions[i].at(call.value).string()));
            if (!callee || asRead.at(callee).has(Format::FunctionHasThis))
                modulesHandedOver.add(call.module);
        }
    }
    unsigned numberOfReadsKnown = 0;
    for (unsigned i = 0; i < count && !functionsExported.isEmpty(); ++i) {
        for (auto& read : foundIn[i].reads) {
            FunctionID exported = functionsExported.get(keyOf(read.module, asRead.functions[i].at(read.value).string()));
            if (!exported || modulesHandedOver.contains(read.module))
                continue;
            auto& value = program.functions[i].at(read.value);
            value.opcode = Opcode::FunctionReference;
            value.children = { };
            value.immediates = { exported.index(), Format::none };
            value.setPrediction({ });
            value.setPredictedClosure(exported);
            value.setPredictedByKey({ });
            value.receiverIsExpectedOfOneRow = false;
            value.classExpectedOfReceiverPlusOne = 0;
            ++numberOfReadsKnown;
        }
    }
    unsigned numberOfRequiresKnown = 0;
    for (unsigned i = 0; i < count && !functionsYielded.isEmpty(); ++i) {
        for (auto [required, module] : foundIn[i].required) {
            FunctionID yielded = functionsYielded.get(module);
            if (!yielded)
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
                closure.immediates = { yielded.index(), Format::none };
                closure.setPredictedClosure(yielded);
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
                ++numberOfRequiresKnown;
                break;
            }
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfModules, " internal modules yield an object that is only read by name: ", numberOfReadsKnown, " reads of it are known to yield a function of the program; ", functionsYielded.size(), " modules of the runtime yield a function: ", numberOfRequiresKnown, " places that ask for one of them know it");
}

static void findFunctionsHeldByModuleVariables(Program& program)
{
    auto& asRead = std::as_const(program);
    unsigned count = asRead.functions.size();
    struct Source {
        enum Kind : uint8_t { Same, Closure, Parameter, Nil, Unknown };
        Kind kind { Unknown };
        FunctionID function;
        uint32_t parameter { 0 };
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
            case Opcode::GetModuleVariable: {
                if (origin.variable() == variable) {
                    functor(Source { Source::Same, { }, 0 });
                    continue;
                }
                auto& other = asRead.variables[origin.variable().index()];
                if (!other.onlyFunctionIsFact)
                    break;
                functor(Source { Source::Closure, other.onlyFunction, 0 });
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
        bool mayHoldAnythingElse { false };
        bool mayHoldNil { false };
        void note(const Source& source)
        {
            if (source.kind == Source::Same)
                return;
            if (source.kind == Source::Nil) {
                mayHoldNil = true;
                return;
            }
            if (source.kind != Source::Closure || (function && function != source.function))
                mayHoldAnythingElse = true;
            else
                function = source.function;
        }
    };
    Vector<Held> held(asRead.variables.size());
    Vector<Vector<std::pair<VariableID, Source>>> storesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        Incoming incoming;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::PutModuleVariable)
                    continue;
                auto& variable = asRead.variables[value.variable().index()];
                if (variable.onlyFunction || (variable.type && !asRead.at(variable.type).anyFunction))
                    continue;
                forEachSource(function, incoming, value.child(0), value.variable(), [&](Source source) {
                    storesIn[i].append(std::pair { value.variable(), source });
                });
            }
        }
    });
    BitVector isEnteredOtherwise(count);
    for (auto& layout : asRead.layouts) {
        if (layout.constructor)
            isEnteredOtherwise.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isEnteredOtherwise.set(method.function.index());
        }
    }
    for (auto& module : asRead.builtinModules)
        isEnteredOtherwise.set(module.second.index());
    for (auto& builtin : asRead.builtinFunctions)
        isEnteredOtherwise.set(builtin.function.index());
    for (FunctionID function : asRead.preludeFunctions) {
        if (function)
            isEnteredOtherwise.set(function.index());
    }
    auto takesArgumentsOfDirectCallsAsParameters = [&](unsigned i) {
        auto& function = asRead.functions[i];
        constexpr uint32_t flagsThatRuleItOut = Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsBuiltin | Format::FunctionIsCodeOfModule | Format::FunctionIsSloppy | Format::FunctionIsGeneralEntry;
        return (function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow) && !function.has(flagsThatRuleItOut) && function.captureTypes.isEmpty();
    };
    auto mayBeEnteredOtherwise = [&](unsigned i) {
        return asRead.functions[i].has(Format::FunctionIsUsedAsValue | FunctionHasUnknownCallers | FunctionIsEnteredByValue) || isEnteredOtherwise.get(i) || asRead.isEntry(FunctionID(i));
    };
    struct Passed {
        VariableID variable;
        uint32_t parameter;
    };
    using PassedOn = UncheckedKeyHashMap<uint32_t, Vector<Passed, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    PassedOn followed;
    PassedOn toFollow;
    auto follow = [&](unsigned function, uint32_t parameter, VariableID variable) {
        if (held[variable.index()].mayHoldAnythingElse)
            return;
        if (!takesArgumentsOfDirectCallsAsParameters(function) || mayBeEnteredOtherwise(function)) {
            held[variable.index()].mayHoldAnythingElse = true;
            return;
        }
        auto& known = followed.add(function, Vector<Passed, 1>()).iterator->value;
        if (known.containsIf([&](auto& passed) { return passed.variable == variable && passed.parameter == parameter; }))
            return;
        known.append(Passed { variable, parameter });
        toFollow.add(function, Vector<Passed, 1>()).iterator->value.append(Passed { variable, parameter });
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
    for (unsigned round = 0; round < 3 && !toFollow.isEmpty(); ++round) {
        PassedOn setters = std::exchange(toFollow, PassedOn());
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
            auto& function = asRead.functions[i];
            Incoming incoming;
            auto isValue = [&](FunctionID named) {
                if (named && isSetter.get(named.index()))
                    seenIn[i].append(Seen { named, { }, { } });
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.parameters)
                    isValue(function.at(id).predictedClosure());
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    isValue(value.predictedClosure());
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
                }
            }
        });
        for (unsigned i = 0; i < count; ++i) {
            for (auto& seen : seenIn[i]) {
                if (!seen.variable) {
                    for (auto& passed : setters.find(seen.setter.index())->value)
                        held[passed.variable.index()].mayHoldAnythingElse = true;
                } else if (seen.source.kind == Source::Parameter)
                    follow(i, seen.source.parameter, seen.variable);
                else
                    held[seen.variable.index()].note(seen.source);
            }
        }
    }
    for (auto& entry : toFollow) {
        for (auto& passed : entry.value)
            held[passed.variable.index()].mayHoldAnythingElse = true;
    }
    unsigned numberWithOne = 0;
    for (unsigned i = 0; i < held.size(); ++i) {
        auto& variable = program.variables[i];
        if (variable.onlyFunction || !held[i].function || held[i].mayHoldAnythingElse || variable.startsZero)
            continue;
        variable.onlyFunction = held[i].function;
        if (variable.startsUndefined || held[i].mayHoldNil)
            variable.onlyFunctionOrNil = true;
        else
            variable.onlyFunctionIsFact = true;
        ++numberWithOne;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: by every store in the program ", numberWithOne, " more module variables hold closures of one function and nothing else");
}

static void findBuiltinsStoredInVariables(Program& program)
{
    enum class Place : uint8_t { Field, Variable, EntryParameter };
    struct Stored {
        Place place;
        uint32_t owner;
        uint32_t index;
        ValueID value;
    };
    auto expectedOf = [&](const Stored& stored) -> ExpectedBuiltins& {
        switch (stored.place) {
        case Place::Field:
            return program.layouts[stored.owner].fields[stored.index].expectedBuiltins;
        case Place::Variable:
            return program.variables[stored.index].expectedBuiltins;
        case Place::EntryParameter:
            break;
        }
        return program.functions[stored.owner].builtinsExpectedOfEntryParameters[stored.index];
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
    constexpr ExpectedBuiltins holdsSomethingElse { { }, { }, true };
    auto takeFreshCandidates = [&] {
        activeFunctions.shrink(0);
        for (unsigned i = 0; i < count; ++i) {
            if (isActive[i])
                activeFunctions.append(i);
        }
        for (unsigned i : activeFunctions) {
            for (auto& stored : others[i])
                expectedOf(stored) = holdsSomethingElse;
        }
        for (unsigned i : activeFunctions) {
            for (auto& stored : fresh[i]) {
                if (expectedOf(stored) != holdsSomethingElse)
                    candidates[i].append(stored);
            }
            fresh[i].shrink(0);
        }
        for (unsigned i : activeFunctions) {
            for (auto& stored : others[i])
                expectedOf(stored) = { };
            others[i].shrink(0);
        }
    };
    Vector<Vector<uint32_t>> entryParametersAsked(count);
    struct Site {
        uint32_t value;
        uint32_t callee;
    };
    Vector<Vector<Site>> sitesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (!function.isGenericEntrypoint()) {
            for (uint32_t k = 0; k < function.values.size(); ++k) {
                auto& value = function.values[k];
                if (value.opcode == Opcode::CallDirect || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                    sitesIn[i].append(Site { k, value.function().index() });
            }
        }
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutField) {
                    auto& layout = std::as_const(program).at(value.layout());
                    if (layout.kind == LayoutKind::Environment && !layout.isCoroutineFrame)
                        sort(i, { Place::Field, value.layout().index(), value.field(), value.child(1) });
                } else if (value.opcode == Opcode::PutModuleVariable)
                    sort(i, { Place::Variable, 0, value.variable().index(), value.child(0) });
                else if (value.opcode == Opcode::Call || value.opcode == Opcode::Construct || (value.opcode == Opcode::CallMethod && program.builtinName(value.string()))) {
                    ValueID called = value.child(0);
                    while (function.at(called).opcode == Opcode::Cast)
                        called = function.at(called).child(0);
                    if (auto& parameter = function.at(called); parameter.opcode == Opcode::Parameter && !parameter.parameterBlock().index()) {
                        isActive[i] = true;
                        entryParametersAsked[i].append(parameter.parameterIndex());
                    }
                }
            }
        }
    });
    takeFreshCandidates();

    Vector<Vector<ExpectedBuiltins>> found(count);
    Vector<Vector<ExpectedBuiltins*>> places(count);
    struct Candidate {
        ExpectedBuiltins* place;
        uint32_t function;
        uint32_t index;
    };
    constexpr unsigned numberOfShares = 64;
    std::array<Vector<Candidate>, numberOfShares> shares;
    Vector<BitVector> entryParametersFollowed(count);
    BitVector functionsToFollow(count);
    unsigned numberOfPlaces = 0;
    for (unsigned round = 0; round < 8; ++round) {
        forEachIndexInParallel(activeFunctions.size(), [&](unsigned k) {
            unsigned i = activeFunctions[k];
            places[i].shrink(0);
            if (candidates[i].isEmpty())
                return;
            ExpectedBuiltinsQuery query { program, std::as_const(program).functions[i], true, &entryParametersAsked[i] };
            found[i].shrink(0);
            for (auto& stored : candidates[i]) {
                found[i].append(query.ask(stored.value));
                places[i].append(&expectedOf(stored));
            }
        });
        for (auto& share : shares)
            share.shrink(0);
        for (unsigned i : activeFunctions) {
            for (uint32_t k = 0; k < places[i].size(); ++k)
                shares[PtrHash<ExpectedBuiltins*>::hash(places[i][k]) % numberOfShares].append(Candidate { places[i][k], i, k });
        }
        std::array<unsigned, numberOfShares> numbersOfPlaces { };
        std::array<bool, numberOfShares> hasChanged { };
        forEachIndexInParallel(numberOfShares, [&](unsigned number) {
            auto& share = shares[number];
            Vector<ExpectedBuiltins> before(share.size(), [&](size_t k) {
                return *share[k].place;
            });
            for (auto& candidate : share)
                *candidate.place = { };
            for (auto& candidate : share) {
                auto& one = found[candidate.function][candidate.index];
                candidate.place->add(one ? one : holdsSomethingElse);
            }
            for (auto& candidate : share) {
                if (candidate.place->areTooMany)
                    *candidate.place = { };
            }
            for (size_t k = 0; k < share.size(); ++k) {
                hasChanged[number] |= *share[k].place != before[k];
                numbersOfPlaces[number] += !!*share[k].place;
            }
        });
        bool changed = false;
        numberOfPlaces = 0;
        for (unsigned number = 0; number < numberOfShares; ++number) {
            changed |= hasChanged[number];
            numberOfPlaces += numbersOfPlaces[number];
        }

        functionsToFollow.clearAll();
        for (unsigned i : activeFunctions) {
            for (uint32_t parameter : entryParametersAsked[i]) {
                if (entryParametersFollowed[i].set(parameter))
                    continue;
                functionsToFollow.set(i);
                auto& expected = program.functions[i].builtinsExpectedOfEntryParameters;
                if (expected.size() <= parameter)
                    expected.grow(parameter + 1);
            }
            entryParametersAsked[i].shrink(0);
        }
        if (!functionsToFollow.isEmpty()) {
            changed = true;
            for (unsigned i : activeFunctions) {
                candidates[i].removeAllMatching([&](auto& stored) {
                    return stored.place == Place::EntryParameter && functionsToFollow.get(stored.owner);
                });
            }
            forEachIndexInParallel(count, [&](unsigned i) {
                auto& function = std::as_const(program).functions[i];
                for (auto [index, callee] : sitesIn[i]) {
                    if (!functionsToFollow.get(callee))
                        continue;
                    auto& value = function.values[index];
                    for (size_t parameter : entryParametersFollowed[callee]) {
                        if (parameter < value.arguments.size())
                            sort(i, { Place::EntryParameter, callee, static_cast<uint32_t>(parameter), value.arguments[parameter] });
                    }
                }
            });
            takeFreshCandidates();
        }
        if (!changed)
            break;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPlaces, " stores to variables and parameters are expected to store a built-in");
    destroyBeside(WTF::move(sitesIn));
}

static void findBuiltinCellsComparedWith(Program& program)
{
    auto& builtins = Builtins::singleton();
    static constexpr OpcodeSet guards { Opcode::IsBuiltinFunction };
    Lock lock;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.functions.mayHold(i, guards))
            return;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode != Opcode::IsBuiltinFunction)
                continue;
            BuiltinName global = value.globalComparedWith();
            Locker locker { lock };
            program.builtinCellsComparedWith.set(global ? builtins.cellOf(global) : builtins.cellOf(value.builtin()));
        }
    });
}

static void findFunctionsWhoseClosuresAreGivenProperties(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::PutById && value.opcode != Opcode::PutByVal)
                    continue;
                auto& object = function.at(originThroughTrivialParameters(function, value.child(0)));
                FunctionID given;
                switch (object.opcode) {
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                    given = object.function();
                    break;
                case Opcode::GetField:
                    given = std::as_const(program).at(object.layout()).fields[object.field()].onlyFunction;
                    break;
                case Opcode::GetModuleVariable:
                    given = program.variables[object.variable().index()].onlyFunction;
                    break;
                default:
                    break;
                }
                if (given)
                    WTF::atomicStore(&program.at(given).closuresAreGivenProperties, true, std::memory_order_relaxed);
            }
        }
    });
}

static void findRowsOfBirths(Program& program, Vector<String>& malformed)
{
    struct Found {
        Vector<Program::RowOfBirth> rows;
        Vector<uint32_t> births;
        Vector<uint32_t> storesThatNameRows;
        Vector<String> complaints;
    };
    unsigned count = program.functions.size();
    Vector<Found> foundIn(count);
    forEachIndexInParallel(count, [&](unsigned index) {
        auto& function = program.functions[index];
        auto& found = foundIn[index];
        if (function.blocks.isEmpty())
            return;
        auto complain = [&](ASCIILiteral what) {
            found.complaints.append(makeString(what, String::fromUTF8(program.string(function.name))));
        };
        UncheckedKeyHashMap<uint32_t, Vector<StringID, 8>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rows;
        auto note = [&](auto& row, const Value& store) {
            if (row.size() <= store.knownPlace())
                row.grow(store.knownPlace() + 1);
            StringID& name = row[store.knownPlace()];
            if (name && name != store.string())
                complain("two names are said to lie at one place of an object born whole in "_s);
            name = store.string();
        };
        Vector<StringID, 8> rowOfThis;
        Vector<std::pair<uint32_t, uint32_t>, 8> storesAndBirths;
        ValueID self = function.has(Format::FunctionHasThis) && function.captureTypes.size() < function.blocks[0].parameters.size() ? function.blocks[0].parameters[function.captureTypes.size()] : ValueID();
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (!store.isStoreToObjectBornWhole() && !store.isFirstStoreToThis())
                    continue;
                if (store.birthOfFirstStore() || store.attributesOfFirstStore())
                    complain("a first store says more than its place in "_s);
                ValueID object = store.child(0);
                while (object.index() < function.values.size()) {
                    auto& origin = function.at(object);
                    if (origin.opcode != Opcode::Cast || origin.castReason() == CastReason::As || origin.castReason() == CastReason::Flow || origin.castReason() == CastReason::Coerce)
                        break;
                    object = origin.child(0);
                }
                if (store.isFirstStoreToThis()) {
                    if (!self || object != self)
                        complain("a store of the row of this goes to something else in "_s);
                    note(rowOfThis, store);
                    continue;
                }
                if (object.index() < function.values.size() && function.at(object).opcode == Opcode::NewObject) {
                    note(rows.ensure(object.index(), [] { return Vector<StringID, 8>(); }).iterator->value, store);
                    storesAndBirths.append({ id.index(), object.index() });
                } else
                    complain("a first store names no birth in "_s);
            }
        }
        if (!rowOfThis.isEmpty()) {
            Program::RowOfBirth row;
            row.isOfThis = true;
            row.names = rowOfThis;
            if (row.names.contains(StringID()))
                complain("the row has a gap: this of "_s);
            for (size_t i = 0; i < row.names.size(); ++i) {
                if (row.names.find(row.names[i]) != i)
                    complain("one name is said to lie at two places of this in "_s);
            }
            for (unsigned k = 0; k < function.values.size(); ++k) {
                if (!function.values[k].isFirstStoreToThis())
                    continue;
                function.values[k].setBirthOfFirstStore(1 + found.rows.size());
                found.storesThatNameRows.append(k);
            }
            found.rows.append(WTF::move(row));
        }
        if (rows.isEmpty())
            return;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbers;
        for (auto& [birth, names] : rows) {
            Program::RowOfBirth row;
            while (row.firstPlace < names.size() && !names[row.firstPlace])
                ++row.firstPlace;
            row.names.append(names.subspan(row.firstPlace));
            if (row.names.contains(StringID()))
                complain("the row has a gap: a literal of "_s);
            for (size_t i = 0; i < row.names.size(); ++i) {
                if (row.names.find(row.names[i]) != i)
                    complain("one name is said to lie at two places of an object born whole in "_s);
            }
            numbers.add(birth, found.rows.size());
            function.values[birth].immediates[0] = found.rows.size();
            found.births.append(birth);
            found.rows.append(WTF::move(row));
        }
        for (auto [store, birth] : storesAndBirths) {
            function.values[store].setBirthOfFirstStore(1 + numbers.get(birth));
            found.storesThatNameRows.append(store);
        }
    });
    Vector<uint32_t> firstRowOf(count);
    for (unsigned index = 0; index < count; ++index) {
        firstRowOf[index] = program.rowsOfBirths.size();
        program.rowsOfBirths.appendVector(WTF::move(foundIn[index].rows));
        malformed.appendVector(WTF::move(foundIn[index].complaints));
    }
    forEachIndexInParallel(count, [&](unsigned index) {
        auto& found = foundIn[index];
        uint32_t first = firstRowOf[index];
        if (!first || (found.births.isEmpty() && found.storesThatNameRows.isEmpty()))
            return;
        auto& function = program.functions[index];
        for (uint32_t birth : found.births)
            function.values[birth].immediates[0] += first;
        for (uint32_t store : found.storesThatNameRows)
            function.values[store].setBirthOfFirstStore(function.values[store].birthOfFirstStore() + first);
    });
}

Vector<size_t, 6> fieldsMadeWithAllocation(const Program& program, const Function& function, const Block& block, size_t allocation, bool isCreateThis, bool hasNullPrototype)
{
    static constexpr unsigned maxFields = 64;
    static constexpr unsigned maxFieldsOfLiteral = 1024;
    auto isDefineField = [](const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
    };
    auto& values = block.values;
    Vector<size_t, 6> fields;
    for (size_t k = allocation + 1; k < values.size() && fields.size() < (isCreateThis ? maxFields : maxFieldsOfLiteral); ++k) {
        auto& value = function.at(values[k]);
        auto nameOfMember = [&](const Value& member) {
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
            bool isThereAlready = std::ranges::any_of(fields, [&](size_t earlier) {
                return nameOfMember(function.at(values[earlier])) == nameOfMember(value);
            });
            if (isThereAlready)
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

bool isBornByItsRow(const Program& program, const Function& function, const Block& block, size_t allocation)
{
    auto& row = program.rowsOfBirths[function.at(block.values[allocation]).rowOfBirth()];
    Vector<StringID, 6> names;
    for (size_t index : fieldsMadeWithAllocation(program, function, block, allocation, false, false)) {
        auto& field = function.at(block.values[index]);
        names.append(field.opcode == Opcode::PutById ? field.string() : function.at(field.arguments[1]).string());
    }
    bool isGapless = names.size() >= row.firstPlace;
    for (size_t i = 0; isGapless && i < row.names.size(); ++i)
        isGapless = row.firstPlace + i < names.size() ? names[row.firstPlace + i] == row.names[i] : !names.contains(row.names[i]);
    return isGapless;
}

static void settleRowsOfBirths(Program& program)
{
    if (Options::validateAOTTypedFacts())
        return;
    Vector<unsigned> timesNotBornByRow;
    timesNotBornByRow.fill(0, program.rowsOfBirths.size());
    Vector<unsigned> timesWithoutSlots;
    timesWithoutSlots.fill(0, program.rowsOfBirths.size());
    static constexpr OpcodeSet births { Opcode::NewObject };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.functions.mayHold(i, births))
            return;
        auto& function = std::as_const(program).functions[i];
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (!value.hasRowOfBirth())
                    continue;
                if (Options::auditAOTTypedAssertions() || !isBornByItsRow(program, function, block, index))
                    WTF::atomicExchangeAdd(&timesNotBornByRow[value.rowOfBirth()], 1u, std::memory_order_relaxed);
            }
        }
        size_t births = std::ranges::count_if(function.values, [](const Value& value) { return value.hasRowOfBirth(); });
        if (2 * births <= maxSlotsOfFunction())
            return;
        for (auto& value : function.values) {
            if (value.hasRowOfBirth())
                WTF::atomicExchangeAdd(&timesWithoutSlots[value.rowOfBirth()], 1u, std::memory_order_relaxed);
        }
    });
    unsigned notBornByTheirRows = 0;
    unsigned withoutSlots = 0;
    for (unsigned row = 0; row < program.rowsOfBirths.size(); ++row) {
        if (!program.rowsOfBirths[row].isWhole || (!timesNotBornByRow[row] && !timesWithoutSlots[row]))
            continue;
        ++(timesNotBornByRow[row] ? notBornByTheirRows : withoutSlots);
        program.rowsOfBirths[row].isWhole = false;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: rows of births: ", program.rowsOfBirths.size(), "; not whole: ", notBornByTheirRows, " not born by their rows, ", withoutSlots, " without slots");
}

static void forgetPlacesOfLazyClosures(Program& program)
{
    Vector<unsigned> timesMayBeLazy;
    timesMayBeLazy.fill(0, program.strings.size());
    std::atomic<unsigned> numberOfPlaces { 0 };
    static constexpr OpcodeSet closures { Opcode::NewClosure };
    static constexpr OpcodeSet accessesByName { Opcode::GetById, Opcode::PutById };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        unsigned placesHere = 0;
        auto withoutUpcasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                id = function.at(id).child(0);
            return id;
        };
        auto note = [&](const Value& store, ValueID stored) {
            auto& closure = function.at(stored);
            if (closure.opcode == Opcode::NewClosure && closure.arguments.size() == 1 && withoutUpcasts(closure.arguments[0]) == withoutUpcasts(store.child(0)))
                WTF::atomicExchangeAdd(&timesMayBeLazy[store.string().index()], 1u, std::memory_order_relaxed);
        };
        if (!program.functions.mayHold(i, closures)) {
            std::as_const(program).functions.forEachValueInOrder(i, accessesByName, [&](ValueID id) {
                placesHere += function.at(id).isAtKnownPlace();
            });
            numberOfPlaces += placesHere;
            return;
        }
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                placesHere += value.isAtKnownPlace();
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
        numberOfPlaces += placesHere;
    });
    if (!numberOfPlaces)
        return;
    std::atomic<unsigned> numberForgotten { 0 };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        Vector<ValueID, 8> toForget;
        std::as_const(program).functions.forEachValueInOrder(i, accessesByName, [&](ValueID id) {
            auto& value = std::as_const(program).functions[i].at(id);
            if (value.isAtKnownPlace() && timesMayBeLazy[value.string().index()])
                toForget.append(id);
        });
        if (toForget.isEmpty())
            return;
        auto& function = program.functions.withSameValues(i);
        for (ValueID id : toForget) {
            auto& value = function.at(id);
            value.immediates[1] = value.isStoreToObjectBornWhole() ? value.immediates[1] | Value::goesByName : Format::none;
        }
        numberForgotten += toForget.size();
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberForgotten.load(), " of ", numberOfPlaces.load(), " accesses at a known place go by the name: a closure may lie there unmade");
}

// Each closure of a generator function has a `prototype` of its own. Where there is one closure the run time goes by the function.
static void passClosuresToGenerators(Program& program)
{
    BitVector hasManyClosures(program.functions.size());
    BitVector isCalledWithoutClosure(program.functions.size());
    BitVector handsOverWhatItMakes(program.functions.size());
    auto next = program.find(asBytes("next"_s.span8()));
    auto returnName = program.find(asBytes("return"_s.span8()));
    auto throwName = program.find(asBytes("throw"_s.span8()));
    unsigned count = program.functions.size();
    constexpr uint8_t saidToHaveManyClosures = 1;
    constexpr uint8_t saidToBeCalledWithoutClosure = 2;
    constexpr uint8_t saidToHandOverWhatItMakes = 4;
    Vector<uint8_t> said(FillWith { }, count, static_cast<uint8_t>(0));
    auto say = [&](FunctionID function, uint8_t what) {
        WTF::atomicExchangeOr(&said[function.index()], what, std::memory_order_relaxed);
    };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        Vector<unsigned> numberOfOtherUses;
        for (auto& value : function.values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                say(value.function(), saidToHaveManyClosures);
            else if (value.opcode == Opcode::CallDirect && std::as_const(program).at(value.function()).declaredAs & functionIsGenerator && numberOfOtherUses.isEmpty())
                numberOfOtherUses.fill(0, function.values.size());
        }
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
            say(value.function(), numberOfOtherUses[k] ? saidToBeCalledWithoutClosure | saidToHandOverWhatItMakes : saidToBeCalledWithoutClosure);
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        if (said[i] & saidToHaveManyClosures)
            hasManyClosures.set(i);
        if (said[i] & saidToBeCalledWithoutClosure)
            isCalledWithoutClosure.set(i);
        if (said[i] & saidToHandOverWhatItMakes)
            handsOverWhatItMakes.set(i);
    }
    for (auto& layout : program.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                if (layout.isEvaluatedOnce) {
                    isCalledWithoutClosure.set(method.function.index());
                    handsOverWhatItMakes.set(method.function.index());
                } else
                    hasManyClosures.set(method.function.index());
            }
        }
    }
    for (unsigned i : hasManyClosures) {
        if (!(std::as_const(program).functions[i].declaredAs & functionIsGenerator) || !std::as_const(program).functions[i].resumeFunction || std::as_const(program).functions[i].blocks.isEmpty() || handsOverWhatItMakes.get(i))
            continue;
        if (isCalledWithoutClosure.get(i)) {
            Function copy = std::as_const(program).functions[i];
            copy.flags &= Format::FunctionIsTyped | Format::FunctionHasThis | Format::FunctionHasRestParameter | Format::FunctionIsOfBuiltinModule;
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
        ramp.takesItsClosure = true;
    }
}

static void createGuardedPreludeEntries(Program& program)
{
    auto& builtins = Builtins::singleton();
    unsigned count = program.functions.size();
    Vector<Function> entries;
    for (unsigned i = 0; i < builtins.size(); ++i) {
        auto& builtin = builtins.at(BuiltinID(i));
        if (builtin.kind != BuiltinKind::Method || builtin.implementation.kind != BuiltinImplementation::Prelude || isChosenByCallbackHandedOver(builtin.implementation.prelude))
            continue;
        FunctionID prelude = program.preludeFunctions[static_cast<unsigned>(builtin.implementation.prelude)];
        StringID name = program.stringOf(builtin.nameName);
        auto* engineClass = builtins.classNamed(builtin.holderName);
        if (!prelude || !name || !engineClass || !engineClass->canCheckOriginalStructure() || !program.engineClassTypes[engineClass->index])
            continue;
        unsigned numberOfParameters = program.at(prelude).parameterTypes.size();
        constexpr unsigned numberOfKindsOfFirstArgument = ClosureRequest::maxNumberOfCapturesOfSlimClosure + 2;
        for (unsigned variant = 0; variant < 2 * numberOfKindsOfFirstArgument * std::min(numberOfParameters, Program::maxArgumentsOfGuardedPreludeEntry + 1); ++variant) {
            unsigned numberOfArguments = variant / (2 * numberOfKindsOfFirstArgument);
            unsigned kindOfFirstArgument = variant / 2 % numberOfKindsOfFirstArgument;
            std::optional<unsigned> numberOfCapturesOfCopiedArgument = kindOfFirstArgument ? std::optional(kindOfFirstArgument - 1) : std::nullopt;
            bool holdsResultToType = variant & 1;
            if ((numberOfCapturesOfCopiedArgument && !numberOfArguments) || (holdsResultToType && !program.at(prelude).resultType) || program.guardedPreludeEntries.contains(Program::keyOfGuardedPreludeEntry(prelude, numberOfArguments, numberOfCapturesOfCopiedArgument, holdsResultToType)))
                continue;
            Function entry;
            entry.name = program.emptyString;
            entry.module = program.at(prelude).module;
            entry.position = program.at(prelude).position;
            entry.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
            entry.isGuardedPreludeEntry = true;
            entry.copiesFirstArgumentOutOfFrame = !!numberOfCapturesOfCopiedArgument;
            entry.numberOfCapturesOfCopiedArgument = numberOfCapturesOfCopiedArgument.value_or(0);
            entry.resultType = holdsResultToType ? program.at(prelude).resultType : program.types.anything;
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
            program.guardedPreludeEntries.add(Program::keyOfGuardedPreludeEntry(prelude, numberOfArguments, numberOfCapturesOfCopiedArgument, holdsResultToType), FunctionID(count + entries.size()));
            entries.append(WTF::move(entry));
        }
    }
    program.functions.appendVector(WTF::move(entries));
}

static void markCallsInTailPosition(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        if (program.standsForBuiltin(function) || program.isOfFileOfBuiltinFunctions(function)) {
            for (auto& value : function.values)
                value.isInTailPosition = false;
        }
        if (!program.makesTailCallsAsBuiltin(function) || function.isValueEntry)
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
            bool isOperationThatCalls = value.opcode == Opcode::Operation && (program.operationOf(value) == Operations::Apply || program.operationOf(value) == Operations::ResumeGenerator);
            if (isOperationThatCalls || value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect)
                value.isInTailPosition = true;
        }
    });
}

static void fuseConcatenations(const Program& program, FunctionID functionID, Function& function)
{
    auto isStringOrNumber = [&](ValueID id) {
        auto& type = program.typeOf(function, id);
        return type.isString() || type.isNumber();
    };
    auto isNoObject = [&](ValueID id) {
        return !program.typeOf(function, id).hasObjects();
    };
    auto isConcatenation = [&](const Value& value) {
        if (value.rep != Rep::Cell || !program.at(value.type).isString())
            return false;
        if (value.opcode == Opcode::Add)
            return isNoObject(value.child(0)) && isNoObject(value.child(1)) && (program.typeOf(function, value.child(0)).isString() || program.typeOf(function, value.child(1)).isString());
        return value.opcode == Opcode::Strcat && std::ranges::all_of(value.arguments, isNoObject);
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
                constexpr size_t maxPartsTakenIn = 256;
                if (numberOfUses[operand.index()] == 1 && function.at(operand).arguments.size() < maxPartsTakenIn && isConcatenation(function.at(operand)) && (!keys.get(id.index()) || hasOnlyStringsAndNumbers(function.at(operand)))) {
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

static void refuteIteratorsWhoseNextMayChange(Program& program)
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
        bool findsIt = false;
        for (LayoutID at = LayoutID(i); at && !findsIt; at = program.at(at).base)
            findsIt = mayChange.get(at.index());
        for (LayoutID at = LayoutID(i); at && findsIt; at = program.at(at).base)
            isRefuted.set(at.index());
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::IsInstanceOfClass || !isRefuted.get(value.layout().index()))
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

static constexpr uint8_t twentiethOfWhatIsNearTheRoots = 25;

static Vector<uint8_t> twentiethsByHeat(const Program&);

static void findFunctionsColdByGraph(Program& program, const Vector<uint8_t>& twentieths)
{
    static constexpr uint8_t hottestThatIsCold = 4;
    BitVector result(twentieths.size());
    for (unsigned i = 0; i < twentieths.size(); ++i) {
        if (twentieths[i] <= hottestThatIsCold && !program.functions[i].isValueEntry)
            result.set(i);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: cold by the graph: ", result.bitCount(), " of ", twentieths.size(), " functions");
    program.functionsColdByGraph = WTF::move(result);
}

static bool isTooSmallToHaveColdCode(const Program& program)
{
    static constexpr uint64_t leastSizeWithColdCode = 65536;
    uint64_t size = 0;
    for (auto& function : program.functions) {
        if (!function.isGenericEntrypoint() && !function.has(Format::FunctionIsBuiltin | Format::FunctionIsOfBuiltinModule))
            size += function.values.size();
    }
    return size < leastSizeWithColdCode;
}

static void takeHeatFromGraph(Program& program, const Vector<uint8_t>& twentieths)
{
    static constexpr uint8_t hottestThatIsCooled = 5;
    uint64_t cooled = 0;
    Vector<uint8_t> isWarmAndHasLoops(FillWith { }, twentieths.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(twentieths.size(), [&](unsigned i) {
        isWarmAndHasLoops[i] = twentieths[i] <= hottestThatIsCooled && program.functionsWarmByText.get(i) && !blocksInLoops(std::as_const(program).functions[i]).isEmpty();
    });
    for (unsigned i = 0; i < twentieths.size(); ++i) {
        if (twentieths[i] > hottestThatIsCooled)
            continue;
        if (isWarmAndHasLoops[i]) {
            program.functionsCalledNearLoops.clear(i);
            program.functionsHotOnlyInTheirLoops.clear(i);
            program.functionsHotByText.clear(i);
            continue;
        }
        cooled += program.functionsHotByText.get(i) || program.functionsWarmByText.get(i);
        program.functionsHotByText.clear(i);
        program.functionsWarmByText.clear(i);
        program.functionsCalledNearLoops.clear(i);
        program.functionsHotOnlyInTheirLoops.clear(i);
    }
    for (auto [function, entry] : program.genericEntrypoints) {
        for (auto* marks : { &program.functionsHotByText, &program.functionsWarmByText, &program.functionsCalledNearLoops, &program.functionsHotOnlyInTheirLoops })
            marks->set(entry.index(), marks->get(function));
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: heat by the graph: ", cooled, " functions lose the mark hot or warm, of ", twentieths.size());
}

static void findFunctionsThatTakeNoCopies(Program& program, const Vector<uint8_t>& twentieths)
{
    static constexpr uint8_t hottestThatTakesNoCopies = 5;
    constexpr uint16_t areNoDuplicates = 1 << licenceForMap('m') | 1 << licenceForMap('w') | 1 << licenceForMap('o');
    BitVector result(twentieths.size());
    for (unsigned i = 0; i < twentieths.size(); ++i) {
        if (twentieths[i] <= hottestThatTakesNoCopies)
            result.set(i);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: no copies into cold callers: ", result.bitCount(), " of ", twentieths.size(), " functions take none");
    program.functionsThatTakeNoCopies = WTF::move(result);
    program.licencesTheyAreNotGiven = ((1 << licencesForMap.length()) - 2) & ~areNoDuplicates;
}

bool WholeNumbers::contains(ValueID id, unsigned depth)
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
        return m_program.at(value.layout()).fields[value.field()].holdsWholeNumbers;
    case Opcode::CallDirect:
        return !value.isJumpToContinuation() && m_program.at(value.function()).returnsWholeNumbers;
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

static void findWholeNumbers(Program& program)
{
    auto mayHoldFractions = [&](TypeID id) {
        if (!id)
            return false;
        auto& type = program.at(id);
        return type.primitives & Primitive::Double && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    };
    for (auto& layout : program.layouts) {
        if (layout.kind != LayoutKind::Environment || layout.isCoroutineFrame)
            continue;
        for (auto& field : layout.fields)
            field.holdsWholeNumbers = mayHoldFractions(field.type);
    }
    struct Site {
        unsigned function;
        ValueID stored;
        LayoutID layout;
        uint32_t field;
    };
    Vector<Vector<Site>> sitesOfFunctions(program.functions.size());
    Vector<Vector<LayoutID>> layoutsMadeWithArguments(program.functions.size());
    static constexpr OpcodeSet birthsAndStores { Opcode::NewObjectWithShape, Opcode::PutField };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.withSameValues(i);
        if (function.blocks.isEmpty())
            return;
        function.returnsWholeNumbers = function.resultTypes.isEmpty() && mayHoldFractions(function.resultType);
        if (!function.returnsWholeNumbers && !program.functions.mayHold(i, birthsAndStores))
            return;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::NewObjectWithShape && !value.arguments.isEmpty())
                    layoutsMadeWithArguments[i].append(value.layout());
                if (value.opcode == Opcode::PutField && std::as_const(program).at(value.layout()).fields[value.field()].holdsWholeNumbers)
                    sitesOfFunctions[i].append({ i, value.child(1), value.layout(), value.field() });
            }
            if (block.terminator.kind == TerminatorKind::Return && function.returnsWholeNumbers) {
                if (block.terminator.value)
                    sitesOfFunctions[i].append({ i, block.terminator.value, LayoutID(), 0 });
                else
                    function.returnsWholeNumbers = false;
            }
        }
    });
    Vector<Site> sites;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        for (LayoutID layout : layoutsMadeWithArguments[i]) {
            for (auto& field : program.at(layout).fields)
                field.holdsWholeNumbers = false;
        }
        sites.appendVector(sitesOfFunctions[i]);
    }
    Vector<unsigned> firstSiteOfFunction;
    for (unsigned at = 0; at < sites.size(); ++at) {
        if (!at || sites[at].function != sites[at - 1].function)
            firstSiteOfFunction.append(at);
    }
    firstSiteOfFunction.append(sites.size());
    Vector<uint8_t> holdsNoLonger(FillWith { }, sites.size(), static_cast<uint8_t>(0));
    for (bool changed = true; changed;) {
        changed = false;
        forEachIndexInParallel(firstSiteOfFunction.size() - 1, [&](unsigned k) {
            auto& function = std::as_const(program).functions[sites[firstSiteOfFunction[k]].function];
            WholeNumbers wholeNumbers(program, function);
            for (unsigned at = firstSiteOfFunction[k]; at < firstSiteOfFunction[k + 1]; ++at) {
                auto& site = sites[at];
                bool holds = site.layout ? std::as_const(program).at(site.layout).fields[site.field].holdsWholeNumbers : function.returnsWholeNumbers;
                holdsNoLonger[at] = holds && !wholeNumbers.contains(site.stored);
            }
        });
        for (unsigned at = 0; at < sites.size(); ++at) {
            if (!holdsNoLonger[at])
                continue;
            auto& site = sites[at];
            (site.layout ? program.at(site.layout).fields[site.field].holdsWholeNumbers : program.functions.withSameValues(site.function).returnsWholeNumbers) = false;
            changed = true;
        }
    }
}

static void boxResultsOfFunctionsOnlyCalledByValue(Program& program)
{
    Vector<FunctionID> enteredBy(program.functions.size());
    for (auto [function, entry] : program.genericEntrypoints)
        enteredBy[entry.index()] = FunctionID(function);
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (FunctionID function = std::as_const(program).functions[i].functionOfValueEntry)
            enteredBy[i] = function;
    }
    Vector<uint8_t> isCalledInPlace(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& caller = std::as_const(program).functions[i];
        auto note = [&](FunctionID callee) {
            WTF::atomicStore(&isCalledInPlace[callee.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        if (!program.functions.mayHoldPredictedClosure(i)) {
            std::as_const(program).functions.forEachValueInOrder(i, directCalls, [&](ValueID id) {
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
                    LayoutID view = program.typeOf(entry, argument).onlyView();
                    if (view && program.at(view).mayBeInFrame)
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
        auto& function = program.functions.withSameValues(index);
        function.isCalledInPlace = isCalledInPlace[index];
        function.takesCapturesFromClosure = !isCalledInPlace[index] && !function.captureTypes.isEmpty() && program.valueEntryWithCodeOf(entry) && function.blocks[0].parameters.size() - function.captureTypes.size() <= numberOfArgumentGPRs;
        if (isCalledInPlace[index] || (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow) || !function.resultTypes.isEmpty() || function.blocks.isEmpty())
            return;
        Rep rep = resultRepOf(program, FunctionID(index));
        function.returnsBoxedValue = rep != Rep::JSValue && rep != Rep::Cell && mayJumpFrom(std::as_const(program).at(entry), FunctionID(index));
    });
}

namespace {

struct TailCallSite {
    BlockID block;
    ValueID call;
    unsigned numberOfCasts { 0 };
    FunctionID callee;
    FunctionID functionOfClosureCalled;
    bool returnsArgument { false };
    Vector<std::pair<BlockID, unsigned>, 2> forwardedThrough;
};

}

static void collectTailCallSites(const Program& program, FunctionID id, const Function& function, Vector<TailCallSite>& sites)
{
    constexpr unsigned maxBlocks = 8;
    if (program.isEntry(id) || function.isBodyOfSeveralSites || !function.resultTypes.isEmpty() || function.worker || function.kind == FunctionKind::Module || function.kind == FunctionKind::Constructor)
        return;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        if (block.values.isEmpty() || block.terminator.kind == TerminatorKind::Invoke)
            continue;
        TailCallSite site;
        site.block = BlockID(b);
        size_t at = block.values.size();
        while (at > 1 && function.at(block.values[at - 1]).opcode == Opcode::Cast && function.at(block.values[at - 1]).castReason() != CastReason::Coerce && function.at(block.values[at - 1]).child(0) == block.values[at - 2])
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
            if (callee.blocks.isEmpty() || !callee.resultTypes.isEmpty() || callee.kind == FunctionKind::Module || program.moduleEvaluationOfFunction.contains(site.callee.index()))
                continue;
        } else if (call.opcode != Opcode::Call && call.opcode != Opcode::CallMethod && languageOperationOf(program, call) != LanguageOperation::Apply && languageOperationOf(program, call) != LanguageOperation::CallWithSpread)
            continue;
        if (!site.callee && call.opcode != Opcode::CallMethod) {
            auto& called = function.at(program.originOf(function, call.opcode == Opcode::Call ? call.child(0) : call.arguments[0]));
            if (called.opcode == Opcode::BuiltinFunction)
                continue;
            if (called.opcode == Opcode::FunctionReference || called.opcode == Opcode::NewClosure)
                site.functionOfClosureCalled = called.function();
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
    Vector<unsigned> callersOfKnownClosures;
    for (unsigned i = 0; i < count; ++i) {
        if (sites[i].containsIf([](auto& site) { return !!site.functionOfClosureCalled; }))
            callersOfKnownClosures.append(i);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i : callersOfKnownClosures) {
            bool removed = sites[i].removeAllMatching([&](auto& site) {
                if (!site.functionOfClosureCalled || !sites[site.functionOfClosureCalled.index()].isEmpty())
                    return false;
                auto entry = program.genericEntrypoints.find(site.functionOfClosureCalled.index());
                return entry == program.genericEntrypoints.end() || sites[entry->value.index()].isEmpty();
            });
            changed |= removed;
        }
    }
    unsigned indirectCalleeNode = count;
    Vector<unsigned> indirectlyCallable;
    for (auto [function, entry] : program.genericEntrypoints) {
        if (!sites[function].isEmpty())
            indirectlyCallable.append(function);
        if (!sites[entry.index()].isEmpty())
            indirectlyCallable.append(entry.index());
    }
    std::ranges::sort(indirectlyCallable);
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
                program.functions.withSameValues(i).at(site.call).mustBeProperTailCall = true;
        }
    });
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
        Rep rep = repOf(program.typePassedAs(function, i));
        ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
    }
    return numberOfGPRs - std::min<unsigned>(numberOfGPRs, numberOfDirectArgumentGPRs) + numberOfFPRs - std::min<unsigned>(numberOfFPRs, FPRInfo::numberOfArgumentRegisters);
}

static void reserveStackArgumentsForTailCalls(Program& program, const Vector<Vector<TailCallSite>>& sites)
{
    unsigned count = program.functions.size();
    Vector<Vector<unsigned, 2>> callers(count);
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        for (auto& site : sites[i]) {
            if (site.callee)
                callers[site.callee.index()].append(i);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (callers[i].isEmpty() && sites[i].isEmpty())
            continue;
        program.functions.withSameValues(i).reservedStackArgumentCount = numberOfStackArguments(program, std::as_const(program).functions[i]);
        if (std::as_const(program).functions[i].reservedStackArgumentCount)
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        unsigned callee = worklist.takeLast();
        for (unsigned caller : callers[callee]) {
            if (std::as_const(program).functions[caller].isGenericEntrypoint() || std::as_const(program).functions[caller].reservedStackArgumentCount >= std::as_const(program).functions[callee].reservedStackArgumentCount)
                continue;
            program.functions.withSameValues(caller).reservedStackArgumentCount = std::as_const(program).functions[callee].reservedStackArgumentCount;
            worklist.append(caller);
        }
    }
}

static bool yieldsResultsOfItsOwn(const Function& function, const Vector<TailCallSite>& sites)
{
    unsigned count = function.blocks.size();
    Vector<Vector<unsigned, 2>> predecessors(count);
    for (unsigned b = 0; b < count; ++b) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    BitVector yieldsOnlyWhatIsCalled(count);
    for (auto& site : sites) {
        if (!site.callee)
            yieldsOnlyWhatIsCalled.set(site.block.index());
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 1; b < count; ++b) {
            if (yieldsOnlyWhatIsCalled.get(b) || !function.blocks[b].values.isEmpty() || predecessors[b].isEmpty())
                continue;
            if (std::ranges::all_of(predecessors[b], [&](unsigned predecessor) { return yieldsOnlyWhatIsCalled.get(predecessor); })) {
                yieldsOnlyWhatIsCalled.set(b);
                changed = true;
            }
        }
    }
    for (unsigned b = 0; b < count; ++b) {
        if (function.blocks[b].terminator.kind == TerminatorKind::Return && !yieldsOnlyWhatIsCalled.get(b) && (!b || !predecessors[b].isEmpty()))
            return true;
    }
    return false;
}

static BitVector findFunctionsThatKeepTheirResultType(const Program& program, const Vector<Vector<TailCallSite>>& sites, const BitVector& isInIndirectCycle)
{
    BitVector keeps(program.functions.size());
    auto mayKeep = [&](unsigned i) {
        auto& function = program.functions[i];
        if (function.isGenericEntrypoint() || function.isValueEntry || function.returnsBoxedValue || !function.resultType || program.at(function.resultType).isAnything())
            return false;
        if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method)
            return false;
        if (function.takesItsClosure || function.takesCapturesFromClosure || function.rampForClosures || function.isRampForClosures)
            return false;
        if (auto entry = program.genericEntrypoints.find(i); entry != program.genericEntrypoints.end() && (!function.isCalledInPlace || function.has(Format::FunctionHasRestParameter)))
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
        return yieldsResultsOfItsOwn(function, sites[i]);
    };
    Vector<unsigned> members;
    for (unsigned i : isInIndirectCycle)
        members.append(i);
    Vector<uint8_t> mayKeepByMember(FillWith { }, members.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(members.size(), [&](unsigned k) {
        mayKeepByMember[k] = mayKeep(members[k]);
    });
    for (unsigned k = 0; k < members.size(); ++k) {
        if (mayKeepByMember[k])
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
                bool staysKeeper;
                if (keeps.get(callee)) {
                    Rep own = resultRepOf(program, FunctionID(member));
                    Rep yielded = resultRepOf(program, site.callee);
                    staysKeeper = !site.returnsArgument && !site.numberOfCasts && (own == yielded || (yielded == Rep::Cell && own == Rep::JSValue)) && program.at(program.functions[callee].resultType).isWithin(program.at(program.functions[member].at(site.call).type), program);
                } else
                    staysKeeper = !site.returnsArgument && !program.functions[callee].reservedStackArgumentCount && !numberOfStackArguments(program, program.functions[callee]);
                if (staysKeeper)
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
    BitVector keepsResultType = findFunctionsThatKeepTheirResultType(program, sites, isInIndirectCycle);
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
                needsCheckAfterCall |= site.numberOfCasts || !resultTypeOf(site.callee.index()).isWithin(program.at(std::as_const(program).functions[member].at(site.call).type), program);
                Rep own = resultRepOf(program, FunctionID(member));
                Rep yielded = resultRepOf(program, site.callee);
                representationsDiffer |= own != yielded && !(yielded == Rep::Cell && own == Rep::JSValue);
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
    std::atomic<unsigned> numberWidened { 0 };
    std::atomic<unsigned> numberOfCastsMovedToCallers { 0 };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (sites[i].isEmpty())
            return;
        auto& function = program.functions[i];
        function.hasTailCallSites = true;
        function.isInCycleOfIndirectTailCalls = isInIndirectCycle.get(i);
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
        bool isCoerced = false;
        for (auto& site : sites[i]) {
            if (site.returnsArgument || (site.callee ? cycleOf[i] != cycleOf[site.callee.index()] : !isInIndirectCycle.get(i)))
                continue;
            auto& block = function.at(site.block);
            ValueID returned = block.values.last();
            for (unsigned k = 0; k < site.numberOfCasts; ++k)
                isCoerced |= function.at(block.values[block.values.size() - 1 - k]).castReason() == CastReason::As;
            numberOfCastsMovedToCallers.fetch_add(site.numberOfCasts, std::memory_order_relaxed);
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
        if (function.isGenericEntrypoint() || !function.resultType || program.at(function.resultType).isAnything())
            return;
        function.resultTypeCheckedByCallers = function.resultType;
        function.resultIsCoercedByCallers = isCoerced;
        function.resultType = TypeID();
        numberWidened.fetch_add(1, std::memory_order_relaxed);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: tail calls: ", isInIndirectCycle.bitCount(), " functions are in the cycle of indirect tail calls, ", cycleMembers.size() - 1, " cycles in total; callers check the result type of ", numberWidened.load(), " functions, ", numberOfCastsMovedToCallers.load(), " casts moved to callers, ", isBoxed.bitCount(), " functions return a boxed value to match their cycle, ", keepsResultType.bitCount(), " functions keep their result type and are called from droppable frames");
}

static void findConstructorsThatYieldObjects(Program& program)
{
    if (Options::auditAOTTypedAssertions())
        return;
    const Program& asRead = program;
    unsigned count = program.functions.size();
    auto& object = asRead.at(program.types.object);
    Vector<uint8_t> yieldsObject(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        if (function.kind != FunctionKind::Constructor || function.isGenericEntrypoint() || function.resultType || !function.resultTypes.isEmpty() || function.blocks.isEmpty() || function.worker
            || function.has(FunctionHasUnknownCallers | FunctionIsEnteredByValue | FunctionUsesArgumentBuffer))
            return;
        bool returns = false;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            if (!block.terminator.value || !asRead.typeOf(function, block.terminator.value).isWithin(object, asRead))
                return;
            returns = true;
        }
        yieldsObject[i] = returns;
    });
    auto shouldBeTypedObject = [&](const Value& value) {
        return value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && yieldsObject[value.function().index()] && asRead.at(value.type).isAnything();
    };
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!yieldsObject[i] && !asRead.functions[i].values.containsIf(shouldBeTypedObject))
            return;
        auto& function = program.functions[i];
        if (yieldsObject[i])
            function.resultType = program.types.object;
        for (auto& value : function.values) {
            if (!shouldBeTypedObject(value))
                continue;
            value.type = program.types.object;
            value.rep = repOf(object);
        }
    });
}

static void findRecordsMadeByNew(Program& program)
{
    Vector<uint8_t> isMade(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    uint8_t isAnyRecord = 0;
    auto note = [&](uint8_t& flag) {
        WTF::atomicStore(&flag, static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    const Program& asRead = program;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = asRead.functions[i];
        for (TypeID thisType : { function.thisType, function.assertedThisType }) {
            if (LayoutID view = thisType ? asRead.at(thisType).onlyView() : LayoutID(); view && asRead.at(view).kind == LayoutKind::Record)
                note(isMade[view.index()]);
        }
        for (auto& value : function.values) {
            if (value.opcode != Opcode::Operation || value.arguments.size() < 2 || !(program.operationOf(value) == Operations::ConstructWith || program.operationOf(value) == Operations::ConstructWithSpread))
                continue;
            auto& object = function.at(value.arguments[1]);
            note(object.opcode == Opcode::NewObjectWithShape ? isMade[object.layout().index()] : isAnyRecord);
        }
    });
    Vector<uint32_t> worklist;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (isMade[i] || isAnyRecord)
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        auto& layout = program.layouts[worklist.takeLast()];
        if (std::exchange(layout.mayBeMadeByNew, true))
            continue;
        for (LayoutID specialization : layout.specializations)
            worklist.append(specialization.index());
    }
    BitVector mayBeBornUnderAnyPrototype;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].mayBeMadeByNew)
            continue;
        mayBeBornUnderAnyPrototype.set(i);
        for (LayoutID member : program.layouts[i].members)
            mayBeBornUnderAnyPrototype.set(member.index());
    }
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].kind == LayoutKind::Record && program.layouts[i].mayGiveUp && !mayBeBornUnderAnyPrototype.get(i))
            program.layouts[i].mayHaveOtherPrototype = false;
    }
}

static void markCastsOfLiteralsNobodyElseHolds(const Program& program, Function& function)
{
    Vector<unsigned> numberOfUses;
    auto standsFor = [&](const Value& value) {
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
            while (ValueID behind = numberOfUses[literal.index()] == 1 ? standsFor(function.at(literal)) : ValueID())
                literal = behind;
            if (function.at(literal).opcode != Opcode::NewObject)
                continue;
            size_t birth = block.values.find(literal);
            if (birth == notFound || birth > i)
                continue;
            unsigned numberSeen = 0;
            bool isHeldByNobodyElse = true;
            for (size_t k = birth + 1; k <= i && isHeldByNobodyElse; ++k) {
                auto& user = function.at(block.values[k]);
                unsigned times = 0;
                user.forEachChild([&](ValueID child) {
                    times += child == literal;
                });
                if (!times)
                    continue;
                numberSeen += times;
                bool storesIntoIt = user.opcode == Opcode::Operation && (program.operationOf(user) == Operations::DefineField || program.operationOf(user) == Operations::CopyDataProperties) && !user.arguments.isEmpty() && user.arguments[0] == literal;
                isHeldByNobodyElse = times == 1 && (storesIntoIt || k == i || standsFor(user) == literal);
            }
            if (isHeldByNobodyElse && numberSeen == numberOfUses[literal.index()])
                cast.immediates[1] |= static_cast<uint32_t>(Format::Freshness::TopLevel) << Format::freshnessShift;
        }
    }
}

static void addPlaceholderFields(Program& program)
{
    const Program& asRead = program;
    unsigned count = program.functions.size();
    Vector<Vector<uint64_t>> readsOfFunctions(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        for (auto& value : function.values) {
            if (value.opcode != Opcode::GetById)
                continue;
            LayoutID view = asRead.typeOf(function, value.child(0)).onlyView();
            if (!view || asRead.at(view).kind != LayoutKind::Record || asRead.at(view).admitsForeignObjects || !asRead.at(view).fieldNamed(value.string()))
                continue;
            readsOfFunctions[i].append(static_cast<uint64_t>(view.index()) << 32 | value.string().index());
        }
    });
    Vector<uint64_t> reads;
    for (auto& readsOfFunction : readsOfFunctions)
        reads.appendVector(readsOfFunction);
    std::ranges::sort(reads);
    Vector<uint8_t> isBornAsItIs(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> isBornByDiscriminant(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : asRead.functions[i].values) {
            if (value.opcode == Opcode::NewObjectWithShape)
                WTF::atomicStore(&(value.child(0) ? isBornByDiscriminant : isBornAsItIs)[value.layout().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
    });
    BitVector isSizedAtBirth(program.layouts.size());
    BitVector isSpecialization(program.layouts.size());
    Vector<uint32_t> worklist;
    for (uint32_t i = 0; i < program.layouts.size(); ++i) {
        if (isBornByDiscriminant[i])
            worklist.append(i);
        for (LayoutID specialization : asRead.layouts[i].specializations)
            isSpecialization.set(specialization.index());
    }
    while (!worklist.isEmpty()) {
        uint32_t layout = worklist.takeLast();
        if (isSizedAtBirth.set(layout))
            continue;
        for (LayoutID specialization : asRead.layouts[layout].specializations)
            worklist.append(specialization.index());
    }
    auto isBornWithRoomToBecomeAnother = [&](LayoutID member) {
        bool hasSpecializations = !asRead.at(member).specializations.isEmpty();
        return !isSizedAtBirth.get(member.index()) && (hasSpecializations || (isSpecialization.get(member.index()) && !isBornAsItIs[member.index()]));
    };
    struct Candidate {
        StringID name;
        TypeID type;
        Vector<LayoutID, 16> membersWithoutField;
        size_t numberOfReads;
    };
    Vector<Candidate> candidates;
    for (size_t next = 0; next < reads.size();) {
        uint64_t read = reads[next];
        size_t numberOfReads = 0;
        for (; next < reads.size() && reads[next] == read; ++next)
            ++numberOfReads;
        LayoutID view(static_cast<uint32_t>(read >> 32));
        StringID name(static_cast<uint32_t>(read));
        Vector<LayoutID, 16> membersWithoutField;
        unsigned numberOfMembersWithField = 0;
        bool areRecords = true;
        TypeID type;
        for (LayoutID member : asRead.at(view).members) {
            auto& layout = asRead.at(member);
            if (auto field = layout.fieldNamed(name)) {
                if (!numberOfMembersWithField++)
                    type = layout.fields[*field].type;
                continue;
            }
            areRecords &= layout.kind == LayoutKind::Record && isBornWithRoomToBecomeAnother(member);
            membersWithoutField.append(member);
        }
        if (!areRecords || membersWithoutField.isEmpty() || numberOfMembersWithField < membersWithoutField.size())
            continue;
        candidates.append(Candidate { name, type, WTF::move(membersWithoutField), numberOfReads });
    }
    auto getTheSame = [](const Candidate& a, const Candidate& b) {
        return a.name == b.name && a.membersWithoutField == b.membersWithoutField;
    };
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.name != b.name)
            return a.name.index() < b.name.index();
        return std::ranges::lexicographical_compare(a.membersWithoutField, b.membersWithoutField, { }, &LayoutID::index, &LayoutID::index);
    });
    BitVector hasNewField(program.layouts.size());
    unsigned numberOfPlaceholders = 0;
    constexpr size_t readsForEachPlaceholder = 1;
    for (size_t next = 0; next < candidates.size();) {
        auto& candidate = candidates[next];
        size_t numberOfReads = 0;
        for (; next < candidates.size() && getTheSame(candidates[next], candidate); ++next)
            numberOfReads += candidates[next].numberOfReads;
        if (numberOfReads < readsForEachPlaceholder * candidate.membersWithoutField.size())
            continue;
        Vector<uint8_t> text { 0xfe };
        text.append(asBytes("placeholder for "_s.span8()));
        text.append(asRead.string(candidate.name));
        StringID nameOfPlaceholder = program.intern(text.span());
        for (LayoutID member : candidate.membersWithoutField) {
            auto& layout = program.at(member);
            bool hasPlaceholder = layout.fields.containsIf([&](auto& field) {
                return field.name == nameOfPlaceholder;
            });
            if (hasPlaceholder)
                continue;
            Field field;
            field.name = nameOfPlaceholder;
            field.type = candidate.type;
            field.flags = fieldIsOptional | fieldIsPlaceholder;
            field.placeholderFor = candidate.name;
            layout.fields.append(WTF::move(field));
            hasNewField.set(member.index());
            ++numberOfPlaceholders;
        }
    }
    for (size_t index : hasNewField)
        program.layouts[index].indexFields();
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfPlaceholders, " placeholder fields in ", hasNewField.bitCount(), " layouts");
}

static void widenUnprofitableLiteralStrings(const Program& program, Function& function, const Vector<TypeID>& widened)
{
    auto hasLiteralString = [&](ValueID id) {
        return program.typeOf(function, id).hasLiteralString();
    };
    BitVector candidates;
    BitVector castsIn;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        for (ValueID parameter : block.parameters) {
            if (b && hasLiteralString(parameter))
                candidates.set(parameter.index());
        }
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::Cast && hasLiteralString(id) && !hasLiteralString(value.child(0))) {
                candidates.set(id.index());
                castsIn.set(id.index());
            }
        }
    }
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
        widenUnprofitableLiteralStrings(program, program.functions[i], widened);
    });
}

static void assignLiteralStringIndices(Program& program)
{
    auto number = [&](StringID string) {
        if (program.literalStrings.size() < maxLiteralStrings && program.literalStringIndices.add(string.index(), program.literalStrings.size()).isNewEntry)
            program.literalStrings.append(string);
    };
    number(program.emptyString);
    auto& asRead = std::as_const(program);
    Vector<Vector<StringID>> stringsIn(asRead.functions.size());
    forEachIndexInParallel(asRead.functions.size(), [&](unsigned i) {
        if (!asRead.hasCode(FunctionID(i)))
            return;
        auto& function = asRead.functions[i];
        static constexpr OpcodeSet stringsAndComparisons { Opcode::String, Opcode::StrictEq };
        asRead.functions.forEachValueInOrder(i, stringsAndComparisons, [&](ValueID id) {
            auto& value = function.at(id);
            if (value.opcode == Opcode::String) {
                if (asRead.at(value.type).hasLiteralString())
                    stringsIn[i].append(value.string());
                return;
            }
            for (unsigned side = 0; side < 2; ++side) {
                ValueID constant = asRead.constantOf(function, value.child(side));
                ValueID other = value.child(1 - side);
                while (function.at(other).opcode == Opcode::Cast && function.at(other).castReason() == CastReason::Upcast)
                    other = function.at(other).child(0);
                if (constant && function.at(constant).opcode == Opcode::String && asRead.typeOf(function, other).hasLiteralString())
                    stringsIn[i].append(function.at(constant).string());
            }
        });
    });
    for (auto& strings : stringsIn) {
        for (StringID string : strings)
            number(string);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.literalStrings.size(), " literal strings have numbers");
}

static void createMemberOnlyViews(Program& program)
{
    if (program.mayRunInSeveralInstances || Options::auditAOTTypedAssertions())
        return;
    unsigned numberOfLayouts = program.layouts.size();
    program.memberOnlyViews.fill(LayoutID(), numberOfLayouts);
    BitVector isMember(numberOfLayouts);
    auto membersStayMembers = [&](const Layout& view) {
        for (LayoutID member : view.members)
            isMember.set(member.index());
        bool stay = std::ranges::all_of(view.members, [&](LayoutID member) {
            return std::ranges::all_of(program.at(member).specializations, [&](LayoutID specialization) {
                return isMember.get(specialization.index());
            });
        });
        for (LayoutID member : view.members)
            isMember.clear(member.index());
        return stay;
    };
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (isObjectLayout(program.layouts[i].kind) && !mayGiveUp(program, LayoutID(i)) && membersStayMembers(program.layouts[i]))
            program.viewsThatKeepTheirMembers.set(i);
    }
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (program.layouts[i].kind != LayoutKind::Record || !program.layouts[i].admitsForeignObjects || program.layouts[i].isOpenedForLog || program.layouts[i].members.isEmpty() || !program.viewsThatKeepTheirMembers.get(i))
            continue;
        Layout view = program.layouts[i];
        view.admitsForeignObjects = false;
        view.admitsOnlyOtherLayouts = false;
        view.isMemberOnlyView = true;
        view.isAllocated = false;
        view.mayGiveUp = false;
        view.specializations.clear();
        view.orders.clear();
        view.allocationOfClone = Format::none;
        LayoutID id(program.layouts.size());
        view.type = program.types.intern(Type::view(id));
        program.memberOnlyViews[i] = id;
        program.layouts.append(WTF::move(view));
    }
}

static TypeID findWidestTypeStoredInField(const Program& program, LayoutID view, StringID name)
{
    TypeID widest;
    auto include = [&](const Layout& layout, bool isMember) {
        auto field = layout.fieldNamed(name);
        if (!field)
            return !isMember;
        TypeID type = layout.fields[*field].type;
        if (!type)
            return false;
        if (isMember && !(layout.fields[*field].flags & fieldIsNeverSeenAbsentAsWritten) && !(program.at(type).primitives & Primitive::Undefined))
            return false;
        if (!widest || program.at(widest).isWithin(program.at(type), program, false)) {
            widest = type;
            return true;
        }
        return program.at(type).isWithin(program.at(widest), program, false);
    };
    for (LayoutID member : program.at(view).members) {
        if (!include(program.at(member), true))
            return { };
        for (uint32_t i = program.firstViewOfLayout[member.index()]; i < program.firstViewOfLayout[member.index() + 1]; ++i) {
            LayoutID other = program.viewsOfLayouts[i];
            if (program.fieldsStoredToThroughLayouts.contains(static_cast<uint64_t>(other.index()) << 32 | name.index()) && !include(program.at(other), false))
                return { };
        }
    }
    return widest;
}

static TypeID widestTypeStoredInField(const Program& program, LayoutID view, StringID name)
{
    if (program.firstViewOfLayout.isEmpty())
        return { };
    uint64_t key = static_cast<uint64_t>(view.index()) << 32 | name.index();
    auto& table = program.widestTypesStoredInFields[(view.index() * 31 + name.index()) % program.widestTypesStoredInFields.size()];
    {
        Locker locker { table.lock };
        if (auto found = table.types.find(key); found != table.types.end())
            return found->value;
    }
    TypeID type = findWidestTypeStoredInField(program, view, name);
    Locker locker { table.lock };
    table.types.add(key, type);
    return type;
}

static constexpr int bytesSavedByAccess = 32;
static constexpr int mostBytesSavedUp = 512;

static unsigned estimatedSizeOfCopy(const Value& value)
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

static bool mayBeCarriedOutTwice(const Value& value, Writes writes)
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
        return test.opcode == Opcode::HasShapeOf && (!test.immediates[1] || test.immediates[1] == shapeTestGuardsFastPath) && program.viewsThatKeepTheirMembers.get(test.layout().index()) ? &test : nullptr;
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
    Vector<std::optional<Passed>> passedAtEnd(numberOfBlocks);
    auto passedAtEndOf = [&](unsigned b) {
        std::optional<Passed> passed;
        if (!b || suspends.get(b))
            return std::optional<Passed>(Passed());
        for (unsigned predecessor : predecessors[b]) {
            if (!passedAtEnd[predecessor])
                continue;
            Passed onEdge = *passedAtEnd[predecessor];
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
            auto passed = passedAtEndOf(b);
            if (passed && (!passedAtEnd[b] || *passedAtEnd[b] != *passed)) {
                passedAtEnd[b] = WTF::move(passed);
                changed = true;
            }
        }
    }
    bool folded = false;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        auto* test = shapeTestOf(block);
        if (!test || test->immediates[1] != shapeTestGuardsFastPath || !passedAtEnd[b] || !passedAtEnd[b]->contains(keyOf(*test)))
            continue;
        block.terminator.kind = TerminatorKind::Jump;
        block.terminator.value = ValueID();
        block.terminator.targets.shrink(1);
        folded = true;
    }
    return folded;
}

enum class SplitOutcome : uint8_t { Split, FoundNothingToSplit, LeftAccessesOut, CouldNotJoinPaths };

static SplitOutcome trySplittingFastPathsForMembers(const Program& program, FunctionID functionID, Function& function, BitVector& accessesLeftAsTheyAre, bool birthsEndEveryGuard)
{
    static constexpr uint32_t none = UINT32_MAX;
    struct GuardedView {
        LayoutID tested;
        LayoutID onFastPath;
    };
    auto guardedViewOf = [&](ValueID receiver) {
        auto& value = function.at(receiver);
        LayoutID view = value.type ? program.at(value.type).onlyView() : LayoutID();
        if (!view || view.index() >= program.memberOnlyViews.size())
            return GuardedView { };
        return GuardedView { view, program.memberOnlyViews[view.index()] };
    };
    auto isAccessToGuard = [&](unsigned b, size_t index) {
        auto& block = function.blocks[b];
        auto& value = function.at(block.values[index]);
        if (value.opcode != Opcode::GetById && value.opcode != Opcode::PutById)
            return false;
        if (accessesLeftAsTheyAre.get(block.values[index].index()))
            return false;
        if (value.hasWordOfKnownPlace() || value.immediates[1] != Format::none)
            return false;
        if (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size())
            return false;
        GuardedView view = guardedViewOf(value.child(0));
        if (!view.onFastPath)
            return false;
        if (!program.at(view.onFastPath).fieldNamed(value.string()) || !onlyFieldsSupply(program, view.onFastPath, value.string()))
            return false;
        return !isLocalAllocation(function, value.child(0));
    };
    bool hasReceiverOfTwoAccesses = false;
    BitVector receiversOfAccesses;
    for (unsigned b = 0; b < function.blocks.size() && !hasReceiverOfTwoAccesses; ++b) {
        for (size_t index = 0; index < function.blocks[b].values.size() && !hasReceiverOfTwoAccesses; ++index)
            hasReceiverOfTwoAccesses = isAccessToGuard(b, index) && receiversOfAccesses.set(function.at(function.blocks[b].values[index]).child(0).index());
    }
    for (unsigned b = 0; b < function.blocks.size() && !hasReceiverOfTwoAccesses && !receiversOfAccesses.isEmpty(); ++b) {
        auto& terminator = function.blocks[b].terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        auto& test = function.at(terminator.value);
        hasReceiverOfTwoAccesses = test.opcode == Opcode::HasShapeOf && !test.immediates[1] && receiversOfAccesses.get(test.child(0).index());
    }
    if (!hasReceiverOfTwoAccesses)
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

    auto addGuard = [&](ValueID receiver, LayoutID view, uint32_t position, uint32_t inlineFrame) {
        Value cast;
        cast.opcode = Opcode::Cast;
        cast.position = position;
        cast.inlineFrame = inlineFrame;
        cast.type = program.at(view).type;
        cast.rep = repOf(program.at(cast.type));
        cast.children = { receiver };
        cast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Member) };
        return add(WTF::move(cast));
    };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> viewTestedBy;
    BitVector isGuardedReceiver;
    BitVector isReadWithNarrowedType;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        for (size_t index = 0; index < block.values.size(); ++index) {
            if (!isAccessToGuard(b, index))
                continue;
            ValueID access = block.values[index];
            ValueID receiver = function.at(access).child(0);
            GuardedView view = guardedViewOf(receiver);
            ValueID guard = addGuard(receiver, view.onFastPath, function.at(access).position, function.at(access).inlineFrame);
            viewTestedBy.add(guard.index(), view.tested.index());
            isGuardedReceiver.set(receiver.index());
            block.values.insert(index++, guard);
            auto& value = function.at(access);
            value.children[0] = guard;
            if (value.opcode != Opcode::GetById || !value.type)
                continue;
            if (!program.at(value.type).isAnything())
                continue;
            TypeID type = widestTypeStoredInField(program, view.onFastPath, value.string());
            if (!type || type == value.type)
                continue;
            readsOnSlowPath.add(access.index(), Value(value));
            isReadWithNarrowedType.set(access.index());
            value.type = type;
            value.rep = repOf(program.at(type));
            value.takePredictionOf(Value { });
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
        GuardedView view = guardedViewOf(tested);
        if (view.tested != function.at(terminator.value).layout())
            continue;
        ValueID guard = addGuard(tested, view.onFastPath, function.at(terminator.value).position, function.at(terminator.value).inlineFrame);
        isAlreadyTested.set(guard.index());
        function.blocks[b].values.insert(0, guard);
    }
    BitVector endsEveryGuard(function.values.size());
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (!isGuard(id) && (suspendsFunction(function.at(id)) || (birthsEndEveryGuard && function.at(id).opcode == Opcode::NewObjectWithShape)))
                endsEveryGuard.set(id.index());
        }
    }
    Vector<uint32_t> numberOfReceiver(FillWith { }, numberOfValuesAtStart, none);
    Vector<uint32_t> numberOfNarrowedRead(FillWith { }, isReadWithNarrowedType.isEmpty() ? 0 : numberOfValuesAtStart, none);
    unsigned numberOfFacts = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isGuard(id)) {
                if (uint32_t& number = numberOfReceiver[function.at(id).child(0).index()]; number == none)
                    number = numberOfFacts++;
            } else if (isReadWithNarrowedType.get(id.index()))
                numberOfNarrowedRead[id.index()] = numberOfFacts++;
        }
    }
    auto numberOfReceiverOf = [&](ValueID guard) {
        return numberOfReceiver[function.at(guard).child(0).index()];
    };
    Vector<std::optional<BitVector>> testedAtEnd(numberOfBlocks);
    auto testedAtStartOf = [&](unsigned b) {
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
    Vector<int> bytesSavedUpAtEnd(FillWith { }, numberOfBlocks, 0);
    auto bytesSavedUpAtStartOf = [&](unsigned b) {
        int bytesSavedUp = mostBytesSavedUp;
        for (unsigned predecessor : predecessors[b]) {
            if (testedAtEnd[predecessor])
                bytesSavedUp = std::min(bytesSavedUp, bytesSavedUpAtEnd[predecessor]);
        }
        return bytesSavedUp;
    };
    auto walkForward = [&](unsigned b, BitVector& tested, const auto& atGuard, const auto& whereSavingsAreSpent) {
        int bytesSavedUp = bytesSavedUpAtStartOf(b);
        auto& block = function.blocks[b];
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            if (isGuard(id)) {
                bool beginsStretch = tested.isEmpty();
                bool wasTested = tested.set(numberOfReceiverOf(id));
                bytesSavedUp = std::min(mostBytesSavedUp, (beginsStretch ? 0 : std::max(bytesSavedUp, 0)) + bytesSavedByAccess);
                atGuard(id, wasTested);
                continue;
            }
            if (endsEveryGuard.get(id.index())) {
                tested.clearAll();
                continue;
            }
            if (tested.isEmpty() || isGuard(function.at(id).child(0)))
                continue;
            bytesSavedUp -= estimatedSizeOfCopy(function.at(id));
            if (bytesSavedUp < 0) {
                tested.clearAll();
                whereSavingsAreSpent(id);
            }
        }
        return tested.isEmpty() ? 0 : std::max(bytesSavedUp - (function.blocks[b].terminator.kind == TerminatorKind::Jump ? 0 : 8), -1);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto tested = testedAtStartOf(b);
            if (!tested)
                continue;
            int bytesSavedUp = walkForward(b, *tested, [](ValueID, bool) { }, [](ValueID) { });
            if (!testedAtEnd[b] || *testedAtEnd[b] != *tested || bytesSavedUpAtEnd[b] != bytesSavedUp) {
                testedAtEnd[b] = WTF::move(tested);
                bytesSavedUpAtEnd[b] = bytesSavedUp;
                changed = true;
            }
        }
    }
    BitVector isCoveredByEarlierTests;
    BitVector spendsSavings;
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto tested = testedAtStartOf(b);
        if (!tested)
            continue;
        walkForward(b, *tested, [&](ValueID id, bool wasTested) {
            if (wasTested && !isAlreadyTested.get(id.index()))
                isCoveredByEarlierTests.set(id.index());
        }, [&](ValueID id) {
            spendsSavings.set(id.index());
        });
    }
    endsEveryGuard.merge(spendsSavings);
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
                live.set(numberOfReceiverOf(child));
            else if (isReadWithNarrowedType.get(child.index()))
                live.set(numberOfNarrowedRead[child.index()]);
        };
        block.terminator.forEachChild([&](ValueID child) {
            use(child);
            if (function.at(child).opcode == Opcode::Results)
                std::as_const(function).at(child).forEachChild(use);
        });
        atPoint(block.values.size(), live);
        for (size_t index = block.values.size(); index--;) {
            ValueID id = block.values[index];
            if (endsEveryGuard.get(id.index()))
                live.clearAll();
            else if (isGuard(id)) {
                if (!isCoveredByEarlierTests.get(id.index()))
                    live.clear(numberOfReceiverOf(id));
            } else {
                if (isReadWithNarrowedType.get(id.index()))
                    live.clear(numberOfNarrowedRead[id.index()]);
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
        ValuesOfBlock values;
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
                accessesLeftAsTheyAre.set(access.index());
                leavesAccesses = true;
            }
        });
        if (leavesAccesses)
            return giveUp(SplitOutcome::LeftAccessesOut);
    }
    BitVector isCarriedOutOnce(function.values.size());
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        size_t index = 0;
        for (auto& segment : segments[b]) {
            for (ValueID id : segment.values) {
                RELEASE_ASSERT(block.values[index] == id);
                if (segment.isGuarded && !isGuard(id) && (endsEveryGuard.get(id.index()) || function.at(id).opcode == Opcode::CallDirect || !mayBeCarriedOutTwice(function.at(id), writesOf(program, function, block, index))))
                    isCarriedOutOnce.set(id.index());
                ++index;
            }
        }
    }
    static constexpr uint32_t unknown = UINT32_MAX - 1;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> slowBlockBehind;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> guardToStartOverAt;
    Vector<uint32_t> startOverAtStart(FillWith { }, numberOfBlocks, unknown);
    Vector<uint32_t> startOverAtEnd(FillWith { }, numberOfBlocks, unknown);
    auto findWhereToStartOver = [&](unsigned b, bool records) {
        uint32_t guard = b ? unknown : none;
        for (unsigned predecessor : predecessors[b]) {
            uint32_t other = startOverAtEnd[predecessor];
            if (other == unknown)
                continue;
            guard = guard == unknown || guard == other ? other : none;
        }
        if (guard == unknown)
            return unknown;
        if (startOverAtStart[b] != unknown && startOverAtStart[b] != guard)
            guard = none;
        startOverAtStart[b] = guard;
        auto& list = segments[b];
        for (unsigned s = 0; s < list.size(); ++s) {
            if (!list[s].isGuarded)
                guard = none;
            for (ValueID id : list[s].values) {
                if (isCarriedOutOnce.get(id.index()))
                    guard = none;
            }
            if (!list[s].guardAtEnd)
                continue;
            if (guard == none)
                guard = list[s].guardAtEnd.index();
            if (records) {
                slowBlockBehind.set(list[s].guardAtEnd.index(), list[s + 1].slowBlock);
                guardToStartOverAt.set(list[s].guardAtEnd.index(), guard);
            }
        }
        return guard;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            uint32_t guard = findWhereToStartOver(b, false);
            if (guard != startOverAtEnd[b]) {
                startOverAtEnd[b] = guard;
                changed = true;
            }
        }
    }
    for (unsigned b = 0; b < numberOfBlocks; ++b)
        findWhereToStartOver(b, true);

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
    Vector<uint32_t> originalOfSlowCopy(FillWith { }, function.values.size(), none);
    for (unsigned i = 0; i < slowCopyOf.size(); ++i) {
        if (slowCopyOf[i] != none)
            originalOfSlowCopy[slowCopyOf[i]] = i;
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
        test.immediates = { viewTestedBy.get(guard.index()), shapeTestGuardsFastPath };
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
                auto startOverAt = guardToStartOverAt.find(segment.guardAtEnd.index());
                block.terminator.targets.append(Target { BlockID(startOverAt == guardToStartOverAt.end() ? list[s + 1].slowBlock : slowBlockBehind.get(startOverAt->value)), { } });
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
    Vector<uint32_t> numberInOrder(FillWith { }, numberOfNewBlocks, none);
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
            numberInOrder[order[i]] = i;
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
                    while (numberInOrder[other] > numberInOrder[found])
                        other = dominator[other];
                    while (numberInOrder[found] > numberInOrder[other])
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

    Vector<ValuesOfBlock> constantsAtStart(numberOfNewBlocks);
    UncheckedKeyHashMap<uint64_t, ValueID> constantAtStart;
    bool failed = false;
    struct Joins {
        Vector<unsigned, 4> blocks;
        Vector<ValueID, 4> parameters;
    };
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Joins>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> joinsOfVariable;
    auto joinsOf = [&](uint32_t variable) -> Joins& {
        auto result = joinsOfVariable.add(variable, nullptr);
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
        if (function.at(ValueID(variable)).opcode == Opcode::NewObjectWithShape || function.at(ValueID(variable)).opcode == Opcode::Results) {
            failed = true;
            return ValueID(variable);
        }
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.type = function.at(ValueID(slowCopyOf[variable])).type;
        parameter.rep = function.at(ValueID(slowCopyOf[variable])).rep;
        parameter.takePredictionOf(function.at(ValueID(slowCopyOf[variable])));
        parameter.position = function.at(ValueID(variable)).position;
        parameter.immediates = { nearest, static_cast<uint32_t>(function.blocks[nearest].parameters.size()) };
        ValueID made = add(WTF::move(parameter));
        function.blocks[nearest].parameters.append(made);
        joins.parameters[join] = made;
        for (auto& edge : incoming[nearest]) {
            if (function.blocks[edge.from].terminator.kind == TerminatorKind::Invoke)
                failed = true;
            ValueID argument = definitionReaching(variable, edge.from, true);
            function.blocks[edge.from].terminator.targets[edge.target].arguments.append(argument);
        }
        return made;
    };
    auto definitionAtStartOf = [&](uint32_t variable, unsigned b) -> ValueID {
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
            if (!child || child.index() >= originalOfSlowCopy.size())
                return;
            uint32_t variable = child.index() < slowCopyOf.size() && slowCopyOf[child.index()] != none ? child.index() : originalOfSlowCopy[child.index()];
            if (variable == none || definedIn[child.index()] == b)
                return;
            child = definitionAtStartOf(variable, b);
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

static bool splitFastPathsForMembers(const Program& program, FunctionID functionID, Function& function)
{
    if (program.memberOnlyViews.isEmpty() || function.isGenericEntrypoint() || !initialStoresOf(function).empty())
        return false;
    auto mayBeGuarded = [&](ValueID receiver) {
        auto& value = function.at(receiver);
        LayoutID view = value.type ? program.at(value.type).onlyView() : LayoutID();
        return view && view.index() < program.memberOnlyViews.size() && !!program.memberOnlyViews[view.index()];
    };
    bool mayHaveReceiverOfTwoAccesses = false;
    BitVector receivers;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::HasShapeOf) && receivers.set(value.child(0).index()))
                mayHaveReceiverOfTwoAccesses |= mayBeGuarded(value.child(0));
        }
    }
    if (!mayHaveReceiverOfTwoAccesses)
        return false;
    for (bool birthsEndEveryGuard : { false, true }) {
        BitVector accessesLeftAsTheyAre;
        SplitOutcome outcome = SplitOutcome::LeftAccessesOut;
        for (unsigned attempt = 0; attempt < 4 && outcome == SplitOutcome::LeftAccessesOut; ++attempt)
            outcome = trySplittingFastPathsForMembers(program, functionID, function, accessesLeftAsTheyAre, birthsEndEveryGuard);
        if (outcome == SplitOutcome::Split)
            return true;
        if (outcome != SplitOutcome::CouldNotJoinPaths || birthsEndEveryGuard)
            break;
    }
    return false;
}

Vector<String> optimize(Program& program)
{
    MonotonicTime start = MonotonicTime::now();
    auto didFinish = [&](ASCIILiteral what) {
        dataLogLnIf(saysClocks(), "AOT passes: ", what, " done at ", (MonotonicTime::now() - start).seconds(), " s");
    };
    findRecordsMadeByNew(program);
    prepare(program);
    didFinish("- prepare"_s);
    widenUnprofitableLiteralStrings(program, false);
    didFinish("- widenUnprofitableLiteralStrings"_s);
    refuteIteratorsWhoseNextMayChange(program);
    didFinish("- refuteIteratorsWhoseNextMayChange"_s);
    Vector<String> malformed;
    convertAllocationAssertions(program, malformed);
    didFinish("- convertAllocationAssertions"_s);
    findRowsOfBirths(program, malformed);
    if (program.rowsOfBirths.size() > Value::greatestBirthOfFirstStore)
        malformed.append("the program has more rows of births than a first store can name"_s);
    if (!malformed.isEmpty())
        return malformed;
    didFinish("preparing"_s);
    verifyExceptionsPassedOn(program);
    verifyVariablesStoredOnce(program);
    didFinish("variables stored once"_s);
    findPairsOfFunctionsHeldByEnvironments(program);
    didFinish("- findPairsOfFunctionsHeldByEnvironments"_s);
    findBuiltinsStoredInVariables(program);
    didFinish("- findBuiltinsStoredInVariables"_s);
    dropExportsNobodyReads(program);
    didFinish("- dropExportsNobodyReads"_s);
    findFunctionsExportedByInternalModules(program);
    didFinish("- findFunctionsExportedByInternalModules"_s);
    findFunctionsHeldByModuleVariables(program);
    didFinish("- findFunctionsHeldByModuleVariables"_s);
    findFunctionsWhoseClosuresAreGivenProperties(program);
    didFinish("- findFunctionsWhoseClosuresAreGivenProperties"_s);
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
    // It has made the layouts of the frames. From here on no layout gets a field.
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        program.layouts[i].indexFields();
    });
    if (!Options::auditAOTTypedAssertions())
        addPlaceholderFields(program);
    for (auto& table : program.whetherOnlyFieldsSupply)
        table.answers.clear();
    for (auto& table : program.whetherStoreMayTransition)
        table.answers.clear();
    didFinish("indexing fields"_s);
    createGenericEntrypoints(program);
    createValueEntrypoints(program);
    createAdaptersOfResults(program);
    didFinish("- createGenericEntrypoints"_s);
    findDeclaredParents(program);
    didFinish("general entries"_s);
    createBoundFunctions(program);
    if (!Options::auditAOTTypedAssertions())
        createGuardedPreludeEntries(program);
    program.numberOfDirectCalls.fill(0, program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if (function.isGenericEntrypoint() || (function.isValueEntry && !function.isTwinOfItsFunction))
            return;
        for (auto& value : function.values) {
            if (value.opcode == Opcode::CallDirect)
                WTF::atomicExchangeAdd(&program.numberOfDirectCalls[value.function().index()], 1u, std::memory_order_relaxed);
        }
    });
    didFinish("- createBoundFunctions, and the numbers of direct calls"_s);
    bearFieldsThatConstructorsStoreFirst(program);
    assignAllocations(program);
    didFinish("assigning allocations"_s);
    createMemberOnlyViews(program);
    findViewsRetaggedTo(program);
    program.types.freeze();
    didFinish("what looks at the whole program"_s);

    program.mayMeetMasquerader = std::ranges::any_of(program.strings, [](std::span<const uint8_t> string) {
        return equalSpans(string, asBytes("makeMasquerader"_s.span8())) || equalSpans(string, asBytes("IsHTMLDDA"_s.span8()));
    });
    didFinish("- mayMeetMasquerader"_s);
    findFunctionsHotByText(program);
    didFinish("- findFunctionsHotByText"_s);
    findFunctionsRareByText(program);
    didFinish("- findFunctionsRareByText"_s);
    if (!isTooSmallToHaveColdCode(program)) {
        auto twentieths = twentiethsByHeat(program);
        takeHeatFromGraph(program, twentieths);
        didFinish("- takeHeatFromGraph"_s);
        findFunctionsThatTakeNoCopies(program, twentieths);
        didFinish("- findFunctionsThatTakeNoCopies"_s);
        findFunctionsColdByGraph(program, twentieths);
        didFinish("- findFunctionsColdByGraph"_s);
    }
    markCallsInTailPosition(program);
    didFinish("- markCallsInTailPosition"_s);
    if (!Options::auditAOTTypedAssertions()) {
        splitOffReadsOfDestructuredParameters(program);
        splitOffWorkers(program);
        splitOffReadsOfHostObjects(program);
        splitOffCallsOfGetStore(program);
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
            BitVector objectsKeptAlive = objectsLiveAcrossSuspensions(function);
            if (objectsKeptAlive.isEmpty()) {
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
            markCastsOfLiteralsNobodyElseHolds(program, function);
            cleanUp(program, function);
            for (unsigned round = 0; round < 8; ++round) {
                unsigned changed = resolvePreludeCalls(program, function, CleanUpTiming::Deferred);
                changed += resolveCalls(program, function, program.isWarmByText(id), CleanUpTiming::Deferred);
                changed += inlineCalls(program, id, function, CleanUpTiming::Deferred);
                changed += simplifyCasts(program, function, CleanUpTiming::Deferred);
                unsigned removed = removeRedundantCatchBlocks(program, function);
                if (!changed && !removed)
                    break;
                cleanUp(program, function);
                cleanUpIfAnythingFolds(program, function);
            }
            for (unsigned round = 0; round < 8 && sinkObjectAllocations(program, function, AllocationSinkingPolicy::AllowMaterialization); ++round)
                cleanUp(program, function);
            BitVector objectsStillKeptAlive = objectsLiveAcrossSuspensions(function);
            objectsStillKeptAlive.filter(objectsKeptAlive);
            if (objectsStillKeptAlive == objectsKeptAlive) {
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
        auto& function = program.functions[i];
        if (!Options::auditAOTTypedAssertions())
            markCastsOfLiteralsNobodyElseHolds(program, function);
        function.values.shrinkToFit();
        function.blocks.shrinkToFit();
    });
    // Each function is worked on as a copy. What it takes from others, to inline them, is as it was read, whoever gets there first.
    indexViewsOfLayouts(program);
    prepareForInlining(program);
    didFinish("- prepareForInlining"_s);
    unsigned count = program.functions.size();
    Vector<std::unique_ptr<Function>> optimized(count);
    Vector<Vector<String>> findings(count);
    Vector<uint64_t> sizesAndNumbers(count);
    forEachIndexInParallel(count, [&](unsigned index) {
        sizesAndNumbers[index] = static_cast<uint64_t>(UINT32_MAX - std::as_const(program).functions[index].values.size()) << 32 | index;
    });
    didFinish("- the functions by size"_s);
    Vector<uint8_t> evaluatesModule(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        evaluatesModule[i] = !!asModuleEvaluation(program, FunctionID(i));
    });
    std::atomic<unsigned> numberOfModuleEvaluationsLeft { 0 };
    std::atomic<unsigned> numberOfModuleEvaluationsRemoved { 0 };
    std::atomic<unsigned> numberOfReceiverGuardsEliminated { 0 };
    std::atomic<unsigned> numberOfCallsOfGuardedPreludeEntries { 0 };
    didFinish("- the functions that evaluate a module"_s);
    auto optimizeFunction = [&](unsigned i) {
        FunctionID id(i);
        Function function = std::as_const(program).functions[i];
        verify(program, id, function, findings[i]);
        if (!findings[i].isEmpty())
            return;
        cleanUp(program, function);
        // Options::auditAOTTypedAssertions(): every test stays where it is, and every call of a function value goes by the general entry,
        // which has the tests for the parameters.
        bool hasSplitFastPaths = false;
        bool hasTriedToSplitFastPaths = false;
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions(); ++round) {
            unsigned changed = resolvePreludeCalls(program, function, CleanUpTiming::Deferred);
            changed += resolveCalls(program, function, program.isWarmByText(id), CleanUpTiming::Deferred);
            changed += inlineCalls(program, id, function, CleanUpTiming::Deferred);
            unsigned fused = fuseGeneratorLoops(program, function);
            changed += simplifyCasts(program, function, CleanUpTiming::Deferred);
            unsigned removed = removeRedundantCatchBlocks(program, function);
            if (unsigned redundant = evaluatesModule[i] ? 0 : removeRedundantModuleEvaluations(function, evaluatesModule)) {
                removed += redundant;
                numberOfModuleEvaluationsRemoved.fetch_add(redundant, std::memory_order_relaxed);
            }
            if (!changed && !fused && !removed) {
                if (std::exchange(hasTriedToSplitFastPaths, true) || !splitFastPathsForMembers(program, id, function))
                    break;
                hasSplitFastPaths = true;
                cleanUp(program, function);
                round = 0;
                continue;
            }
            if (changed || removed || function.storesWereMovedByCleanUp)
                cleanUp(program, function);
            cleanUpIfAnythingFolds(program, function);
        }
        if (!hasTriedToSplitFastPaths && !Options::auditAOTTypedAssertions() && splitFastPathsForMembers(program, id, function)) {
            hasSplitFastPaths = true;
            cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions() && countInIntegers(program, function))
            cleanUpIfAnythingFolds(program, function);
        if (!Options::auditAOTTypedAssertions()) {
            speculateOnReceivers(program, id, function);
            markCastsOfLiteralsNobodyElseHolds(program, function);
        }
        resolveBuiltins(program, function);
        if (!Options::auditAOTTypedAssertions() && askForTruthOnTheWaysIn(program, function))
            cleanUp(program, function);
        if (unsigned eliminated = Options::auditAOTTypedAssertions() ? 0 : eliminateGuardsOnUnescapedReceivers(program, function)) {
            numberOfReceiverGuardsEliminated += eliminated;
            cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions())
            numberOfCallsOfGuardedPreludeEntries += callGuardedPreludeEntries(program, id, function, ReceiversThatMayYetBeProven::Wait);
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
        if (!Options::auditAOTTypedAssertions() && threadOwnKeysOfForIn(program, function))
            cleanUp(program, function);
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions() && sinkObjectAllocations(program, function, AllocationSinkingPolicy::AllowMaterialization); ++round)
            cleanUp(program, function);
        if (!Options::auditAOTTypedAssertions())
            hoistReadsOutOfLoops(program, function);
        while (!Options::auditAOTTypedAssertions() && eliminateRedundantReads(program, function)) { }
        while (hasSplitFastPaths && foldRepeatedShapeGuards(program, function)) {
            cleanUp(program, function);
            while (eliminateRedundantReads(program, function)) { }
        }
        if (!Options::auditAOTTypedAssertions())
            moveBirthsIntoArms(program, function);
        sinkEmptyArrayLiterals(function);
        if (!Options::auditAOTTypedAssertions())
            sinkFunctionReferences(function);
        recordCellsLiveAcrossAwaits(program, function);
        compactBlocks(function);
        verify(program, id, function, findings[i], VerifierScope::EverythingButDefinitions);
        concludeAllocationNotes(program, function);
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            for (auto& block : function.blocks) {
                for (ValueID value : block.values)
                    numberOfModuleEvaluationsLeft.fetch_add(asksForModuleEvaluation(function.at(value), evaluatesModule), std::memory_order_relaxed);
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
    BitVector isQueued = functionsReachedOtherwise(program);
    auto enqueue = [&](unsigned index) {
        if (std::as_const(program).functions[index].values.size() < minLargeFunctionSize) {
            smallFunctions.append(index);
            return;
        }
        largeFunctions.append(sizesAndNumbers[index]);
        std::ranges::push_heap(largeFunctions, std::greater { });
    };
    for (size_t i : isQueued)
        enqueue(i);
    unsigned numberOfBusyWorkers = 0;
    unsigned numberOfSleepingWorkers = 0;
    forEachIndexInParallel(numberOfWorkers, [&](unsigned) {
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
                forEachCalleeOf(program, *optimized[next], [&](unsigned callee) {
                    callees.append(callee);
                });
                for (FunctionID entry : optimized[next]->guardedPreludeEntriesWantedLater)
                    callees.append(entry.index());
            }
        }
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!isQueued.get(i))
            verify(program, FunctionID(i), std::as_const(program).functions[i], findings[i]);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfModuleEvaluationsLeft.load() + numberOfModuleEvaluationsRemoved.load(), " requests to evaluate a module, ", numberOfModuleEvaluationsRemoved.load(), " behind another for the same module");
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        unsigned numberOfCallsWithoutGenericFallback = 0;
        for (auto& function : optimized) {
            if (!function)
                continue;
            for (auto& value : function->values)
                numberOfCallsWithoutGenericFallback += value.receiverHasOriginalStructure;
        }
        dataLogLn("AOT: ", numberOfCallsOfGuardedPreludeEntries.load(), " calls of array methods leave the guard and the call by name to an entry that all such sites share");
        dataLogLn("AOT: receivers that are allocated in the function and have not escaped: ", numberOfReceiverGuardsEliminated.load(), " guards and method reads eliminated, ", numberOfCallsWithoutGenericFallback, " calls of built-in methods without a generic fallback");
    }
    didFinish("- the loop over the functions"_s);
    program.sizesForInlining.clear();
    program.firstViewOfLayout.clear();
    program.viewsOfLayouts.clear();
    program.fieldsStoredToThroughLayouts.clear();
    program.factsForInlining.clear();
    for (unsigned i = 0; i < program.blocksThatNeverReturn.size(); ++i) {
        if (program.blocksThatNeverReturn[i].get(0))
            program.functionsThatNeverReturn.set(i);
    }
    program.blocksThatNeverReturn.clear();
    didFinish("what looks at one function"_s);

    Vector<String> result;
    verifyRegExpLiterals(program, result);
    didFinish("- verifyRegExpLiterals"_s);
    for (auto& ofFunction : findings)
        result.appendVector(WTF::move(ofFunction));
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
    destroyBeside(WTF::move(optimized));
    for (unsigned i = 0; i < count; ++i) {
        if (!isQueued.get(i))
            program.functionsWithoutCalls.set(i);
    }
    didFinish("freeing the functions as they were read"_s);
    widenUnprofitableLiteralStrings(program, true);
    didFinish("- widenUnprofitableLiteralStrings"_s);
    if (!Options::auditAOTTypedAssertions()) {
        eliminateGuardsOnFreshResults(program);
        didFinish("- eliminateGuardsOnFreshResults"_s);
        eliminateGuardsOnUnescapedModuleVariables(program);
        didFinish("- eliminateGuardsOnUnescapedModuleVariables"_s);
        std::atomic<unsigned> numberOfLateCallsOfGuardedPreludeEntries { 0 };
        forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
            if (!std::as_const(program).functions[i].guardedPreludeEntriesWantedLater.isEmpty())
                numberOfLateCallsOfGuardedPreludeEntries += callGuardedPreludeEntries(program, FunctionID(i), program.functions[i], ReceiversThatMayYetBeProven::AreTaken);
        });
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfLateCallsOfGuardedPreludeEntries.load(), " more calls of array methods go to such an entry once the whole program has been looked at");
        didFinish("- callGuardedPreludeEntries"_s);
        computeFunctionsReturningMapKeys(program);
        didFinish("- computeFunctionsReturningMapKeys"_s);
        static constexpr OpcodeSet concatenations { Opcode::Add, Opcode::Strcat };
        forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
            if (program.functions.mayHold(i, concatenations))
                fuseConcatenations(program, FunctionID(i), program.functions[i]);
        });
        didFinish("- fuseConcatenations"_s);
        giveConcatenationsOneBody(program);
        didFinish("- giveConcatenationsOneBody"_s);
    }
    if (!Options::auditAOTTypedAssertions()) {
        createJSONSerializers(program);
        giveCallsOfBuiltinsOneBody(program);
        giveConstructionsOneBody(program);
        giveCastsOneBody(program);
        callValuesBySignature(program);
    }
    didFinish("- giveCallsOfBuiltinsOneBody, giveConstructionsOneBody"_s);
    if (!Options::auditAOTTypedAssertions()) {
        program.functions.listValuesInOrder();
        didFinish("- listValuesInOrder"_s);
        returnResultsInRegisters(program);
    }
    didFinish("- returnResultsInRegisters"_s);
    if (!Options::auditAOTTypedAssertions())
        shareModuleEvaluation(program);
    didFinish("- shareModuleEvaluation"_s);
    dropFunctionsWithoutCalls(program);
    didFinish("- dropFunctionsWithoutCalls"_s);
    findBuiltinCellsComparedWith(program);
    didFinish("- findBuiltinCellsComparedWith"_s);
    forgetPlacesOfLazyClosures(program);
    didFinish("- forgetPlacesOfLazyClosures"_s);
    program.functionLengthsAreBytes = std::ranges::all_of(std::as_const(program).functions, [](auto& function) {
        return function.length <= std::numeric_limits<uint8_t>::max();
    });
    reportAllocationAssertions(program, result);
    if (!result.isEmpty())
        return result;
    didFinish("functions without calls"_s);
    markMandatoryTailCalls(program);
    didFinish("- markMandatoryTailCalls"_s);
    findOperandsOnlyCalled(program);
    didFinish("- findOperandsOnlyCalled"_s);
    verifyParametersOnlyCalled(program, result);
    if (!result.isEmpty() && !program.notesAllocations)
        return result;
    findClosuresInFrames(program);
    findEnvironmentsInFrames(program, result);
    if (!result.isEmpty())
        return result;
    reuseEnvironments(program);
    didFinish("environments in frames"_s);
    linkAllocations(program);
    didFinish("allocations"_s);
    computeEffectSummaries(program);
    didFinish("effects"_s);
    if (!Options::auditAOTTypedAssertions())
        program.functions.listForgottenValuesInOrder();
    computeFunctionsReturningMapKeys(program);
    didFinish("map keys"_s);
    dropModuleEvaluations(program);
    didFinish("- dropModuleEvaluations"_s);
    settleRowsOfBirths(program);
    didFinish("- settleRowsOfBirths"_s);
    findWholeNumbers(program);
    didFinish("- findWholeNumbers"_s);
    findConstructorsThatYieldObjects(program);
    didFinish("- findConstructorsThatYieldObjects"_s);
    boxResultsOfFunctionsOnlyCalledByValue(program);
    didFinish("- boxResultsOfFunctionsOnlyCalledByValue"_s);
    assignLiteralStringIndices(program);
    didFinish("- assignLiteralStringIndices"_s);
    prepareTailCalls(program);
    forgetResultsThatCallersCheck(program);
    didFinish("- prepareTailCalls"_s);
    return result;
}

static Vector<uint8_t> twentiethsByHeat(const Program& program)
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
    auto hubOf = [&](uint64_t key, bool makesIt) -> uint32_t {
        if (auto found = hubs.find(key); found != hubs.end())
            return found->value;
        if (!makesIt)
            return UINT32_MAX;
        uint32_t hub = edges.size();
        hubs.add(key, hub);
        edges.append(Vector<Edge, 2>());
        return hub;
    };
    auto keyOfName = [](StringID name) {
        return 1ull << 60 | name.index();
    };
    auto keyOfVariable = [](VariableID variable) {
        return 2ull << 60 | variable.index();
    };
    auto keyOfField = [](LayoutID layout, uint32_t field) {
        return 3ull << 60 | static_cast<uint64_t>(layout.index()) << 24 | (field & 0xffffff);
    };
    auto keyOfPlace = [&](LayoutID layout, uint32_t field) {
        auto& described = program.at(layout);
        if (isObjectLayout(described.kind) && field < described.fields.size() && described.fields[field].name)
            return keyOfName(described.fields[field].name);
        return keyOfField(layout, field);
    };
    auto keyOfSetter = [](StringID name) {
        return 4ull << 60 | name.index();
    };
    auto keyOfClass = [](LayoutID layout) {
        return 5ull << 60 | layout.index();
    };
    auto keyOfGetter = [](StringID name) {
        return 6ull << 60 | name.index();
    };
    auto kindOfKey = [](uint64_t key) -> uint8_t {
        unsigned tag = key >> 60;
        return tag == 5 ? 3 : tag == 1 || tag == 4 || tag == 6 || tag > 8 ? 2 : 1;
    };
    constexpr uint32_t noLayout = UINT32_MAX;
    auto keyWithin = [](uint64_t key, uint32_t layout) {
        return ((key >> 60) + 8) << 60 | static_cast<uint64_t>(layout) << 28 | (key & 0xfffffff);
    };
    Vector<uint32_t> numberOfLayoutsUnder(FillWith { }, program.layouts.size(), 0u);
    forEachIndexInParallel(program.layouts.size(), [&](unsigned index) {
        uint32_t number = 1;
        for (auto* members : { &program.layouts[index].members, &program.layouts[index].membersByFit }) {
            for (LayoutID member : *members)
                number += 1 + program.at(member).specializations.size();
        }
        numberOfLayoutsUnder[index] = number;
    });
    auto layoutsUnder = [&](const auto& views) -> std::optional<Vector<uint32_t>> {
        constexpr size_t mostLayouts = 64;
        Vector<uint32_t> found;
        for (LayoutID view : views) {
            auto& described = program.at(view);
            if (described.admitsForeignObjects)
                return std::nullopt;
            if (found.size() + numberOfLayoutsUnder[view.index()] > 16 * mostLayouts)
                return std::nullopt;
            found.append(view.index());
            for (auto* members : { &described.members, &described.membersByFit }) {
                for (LayoutID member : *members) {
                    found.append(member.index());
                    for (LayoutID specialization : program.at(member).specializations)
                        found.append(specialization.index());
                }
            }
            if (found.size() > 16 * mostLayouts)
                return std::nullopt;
        }
        std::ranges::sort(found);
        found.shrink(std::unique(found.begin(), found.end()) - found.begin());
        if (found.size() > mostLayouts)
            return std::nullopt;
        return found;
    };
    auto layoutsOfReceiver = [&](const Function& function, ValueID receiver) -> std::optional<Vector<uint32_t>> {
        if (!receiver)
            return std::nullopt;
        auto& type = program.at(function.at(receiver).type);
        if (type.anyObject || type.anyFunction)
            return std::nullopt;
        return layoutsUnder(type.views);
    };
    auto enteredByValue = [&](FunctionID function) -> uint32_t {
        auto entry = program.genericEntrypoints.find(function.index());
        return entry != program.genericEntrypoints.end() ? entry->value.index() : function.index();
    };
    auto functionMadeBy = [](const Function& function, ValueID id) -> FunctionID {
        while (id && function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        if (!id)
            return { };
        auto& value = function.at(id);
        if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference)
            return value.function();
        return { };
    };
    auto placeStoredTo = [&](const Function& function, const Value& value) -> std::pair<uint64_t, ValueID> {
        switch (value.opcode) {
        case Opcode::PutById:
            return { keyOfName(value.string()), value.child(1) };
        case Opcode::PutField:
            return { keyOfPlace(value.layout(), value.field()), value.child(1) };
        case Opcode::PutModuleVariable:
            return { keyOfVariable(value.variable()), value.child(0) };
        case Opcode::CallBuiltin:
            if (languageOperationOf(program, value) == LanguageOperation::DefineField && value.arguments.size() == 3 && function.at(value.arguments[1]).opcode == Opcode::String)
                return { keyOfName(function.at(value.arguments[1]).string()), value.arguments[2] };
            if (auto operation = languageOperationOf(program, value); (operation == LanguageOperation::DefineGetter || operation == LanguageOperation::DefineSetter) && value.arguments.size() >= 3 && function.at(value.arguments[1]).opcode == Opcode::String)
                return { operation == LanguageOperation::DefineGetter ? keyOfGetter(function.at(value.arguments[1]).string()) : keyOfSetter(function.at(value.arguments[1]).string()), value.arguments[2] };
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
                    edges[hubOf(method.kind == MethodKind::Setter ? keyOfSetter(method.name) : method.kind == MethodKind::Getter ? keyOfGetter(method.name) : keyOfName(method.name), true)].append(Edge { entered, 2, false, 0 });
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
                    edges[hubOf(keyOfClass(LayoutID(i)), true)].append(Edge { enteredByValue(method.function), 3, false, 0 });
            }
        }
    }
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        bool isNamedByTypes = program.layouts[i].kind != LayoutKind::Instance || program.layouts[i].isLaidOut;
        for (LayoutID at = LayoutID(i); at; at = isNamedByTypes ? program.at(at).base : LayoutID()) {
            for (auto& method : program.at(at).methods) {
                if (!method.function || !method.name)
                    continue;
                uint64_t key = method.kind == MethodKind::Setter ? keyOfSetter(method.name) : method.kind == MethodKind::Getter ? keyOfGetter(method.name) : keyOfName(method.name);
                edges[hubOf(keyWithin(key, isNamedByTypes ? i : noLayout), true)].append(Edge { enteredByValue(method.function), 2, false, 0 });
            }
        }
    }
    auto layoutsStoredThrough = [&](const Function& function, const Value& value) -> std::optional<Vector<uint32_t>> {
        switch (value.opcode) {
        case Opcode::PutById:
            return layoutsOfReceiver(function, value.child(0));
        case Opcode::PutField:
            return layoutsUnder(std::array { value.layout() });
        case Opcode::CallBuiltin:
            return layoutsOfReceiver(function, value.arguments[0]);
        default:
            return std::nullopt;
        }
    };
    struct StoredToPlace {
        uint64_t key;
        uint32_t entered;
        Vector<uint32_t> layouts;
    };
    Vector<Vector<StoredToPlace>> storedIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto [key, stored] = placeStoredTo(function, function.at(id));
                if (!key)
                    continue;
                FunctionID made = functionMadeBy(function, stored);
                if (!made)
                    continue;
                Vector<uint32_t> layouts;
                if (kindOfKey(key) == 2) {
                    if (auto found = layoutsStoredThrough(function, function.at(id)))
                        layouts = WTF::move(*found);
                }
                storedIn[i].append(StoredToPlace { key, enteredByValue(made), WTF::move(layouts) });
            }
        }
    });
    for (auto& storedInFunction : storedIn) {
        for (auto& [key, entered, layouts] : storedInFunction) {
            edges[hubOf(key, true)].append(Edge { entered, kindOfKey(key), false, 0 });
            if (kindOfKey(key) != 2)
                continue;
            if (layouts.isEmpty()) {
                edges[hubOf(keyWithin(key, noLayout), true)].append(Edge { entered, 2, false, 0 });
                continue;
            }
            for (uint32_t layout : layouts)
                edges[hubOf(keyWithin(key, layout), true)].append(Edge { entered, 2, false, 0 });
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
    for (unsigned i = 0; i < program.strings.size(); ++i) {
        if (isIteratingName(program.strings[i]))
            iteratingNames.set(i);
    }
    UncheckedKeyHashMap<uint64_t, int> weightOfCallsOfParameter;
    auto keyOfParameter = [](uint32_t function, uint32_t index) {
        return (static_cast<uint64_t>(function) + 1) << 20 | (index & 0xfffff);
    };
    auto noteCallOfParameter = [&](uint32_t function, uint32_t index, int weight) {
        auto result = weightOfCallsOfParameter.add(keyOfParameter(function, index), weight);
        if (result.isNewEntry || result.iterator->value >= weight)
            return result.isNewEntry;
        result.iterator->value = weight;
        return true;
    };
    struct PassedOn {
        uint32_t function;
        uint32_t index;
        uint32_t callee;
        uint32_t argument;
        int weight;
    };
    Vector<PassedOn> parametersPassedOn;
    struct Handed {
        uint32_t function;
        uint32_t edge;
        uint32_t callee;
        uint32_t argument;
        int weight;
    };
    Vector<Handed> functionsHanded;
    Vector<float> massOf(FillWith { }, count, 0.0f);
    Vector<uint32_t> sizes(FillWith { }, count, 0u);
    Vector<char> coroutineResumedBy(FillWith { }, count, '-');
    for (unsigned i = 0; i < count; ++i) {
        auto& ramp = program.functions[i];
        if (!ramp.resumeFunction)
            continue;
        bool isAsync = ramp.declaredAs & functionIsAsync;
        bool isGenerator = ramp.declaredAs & functionIsGenerator;
        coroutineResumedBy[ramp.resumeFunction.index()] = isAsync && isGenerator ? 'x' : isAsync ? 'a' : isGenerator ? 'g' : '-';
    }
    struct CallOfParameter {
        uint32_t index;
        int weight;
    };
    struct FoundInFunction {
        Vector<CallOfParameter> callsOfParameters;
        Vector<PassedOn> parametersPassedOn;
        Vector<Handed> functionsHanded;
    };
    Vector<FoundInFunction> foundIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        unsigned numberOfBlocks = function.blocks.size();
        if (!numberOfBlocks)
            return;
        Vector<unsigned> resumedAt;
        if (coroutineResumedBy[i] == 'g' || coroutineResumedBy[i] == 'x')
            resumedAt = blocksResumedAt(function);
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
        Vector<unsigned, 8> waysOutOfTheEntry;
        for (bool isResumed : { false, true }) {
            auto& targets = function.blocks[0].terminator.targets;
            for (unsigned way = 0; way < targets.size(); ++way) {
                if (isResumedFromHere.get(targets[way].block.index()) == isResumed)
                    waysOutOfTheEntry.append(way);
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
                unsigned to = way < targets.size() ? targets[block ? way : waysOutOfTheEntry[way]].block.index() : resumedAt[block];
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
        BitVector holdsAnEarlierHint(numberOfBlocks);
        for (auto [number, hinted] : hints) {
            if (holdsAnEarlierHint.get(hinted))
                continue;
            BitVector isReached(numberOfBlocks);
            BitVector body(numberOfBlocks);
            Vector<unsigned, 16> worklist { hinted };
            auto reach = [&](unsigned to) {
                if (!holdsAnEarlierHint.get(to) && !isReached.set(to))
                    worklist.append(to);
            };
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                for (auto& target : function.blocks[block].terminator.targets)
                    reach(target.block.index());
                if (isSuspendedIn(block))
                    reach(resumedAt[block]);
            }
            holdsAnEarlierHint.set(hinted);
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

        bool isEnteredAtAStateOfItsOwn = false;
        for (auto& target : function.blocks[0].terminator.targets)
            isEnteredAtAStateOfItsOwn |= !isResumedFromHere.get(target.block.index()) && !isCold[target.block.index()];
        isEnteredAtAStateOfItsOwn &= !resumedAt.isEmpty();
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
                if (!b && isEnteredAtAStateOfItsOwn && isResumedFromHere.get(to))
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
                all = isEnteredAtAStateOfItsOwn ? 1 : 2;
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
        auto leadByName = [&](uint64_t key, const std::optional<Vector<uint32_t>>& layouts, unsigned b) {
            if (!layouts) {
                if (uint32_t hub = hubOf(key, false); hub != UINT32_MAX)
                    add(hub, b, 2);
                return;
            }
            if (uint32_t hub = hubOf(keyWithin(key, noLayout), false); hub != UINT32_MAX)
                add(hub, b, 2);
            for (uint32_t layout : *layouts) {
                if (uint32_t hub = hubOf(keyWithin(key, layout), false); hub != UINT32_MAX)
                    add(hub, b, 2);
            }
        };
        auto parameterThatIs = [&](ValueID id) -> std::optional<uint32_t> {
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
                    if (program.moduleEvaluatorIfNeeded && program.moduleEvaluationOfFunction.contains(value.function().index()))
                        add(program.moduleEvaluatorIfNeeded.index(), b, 0);
                    break;
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                case Opcode::FunctionReference:
                    add(enteredByValue(value.function()), b, 1);
                    break;
                case Opcode::GetById:
                case Opcode::CallMethod: {
                    bool mayLeadToGetter = hubOf(keyOfGetter(value.string()), false) != UINT32_MAX;
                    bool mayLeadToFunction = (value.opcode == Opcode::CallMethod || program.at(value.type).anyFunction) && hubOf(keyOfName(value.string()), false) != UINT32_MAX;
                    if (!mayLeadToGetter && !mayLeadToFunction)
                        break;
                    auto layouts = layoutsOfReceiver(function, value.child(0));
                    if (mayLeadToGetter)
                        leadByName(keyOfGetter(value.string()), layouts, b);
                    if (mayLeadToFunction)
                        leadByName(keyOfName(value.string()), layouts, b);
                    break;
                }
                case Opcode::GetField:
                    if (program.at(value.type).anyFunction) {
                        read = keyOfPlace(value.layout(), value.field());
                        if (kindOfKey(read) == 2) {
                            if (hubOf(read, false) != UINT32_MAX)
                                leadByName(read, layoutsUnder(std::array { value.layout() }), b);
                            read = 0;
                        }
                    }
                    break;
                case Opcode::GetModuleVariable:
                    if (program.at(value.type).anyFunction)
                        read = keyOfVariable(value.variable());
                    break;
                case Opcode::PutById:
                    if (hubOf(keyOfSetter(value.string()), false) != UINT32_MAX)
                        leadByName(keyOfSetter(value.string()), layoutsOfReceiver(function, value.child(0)), b);
                    break;
                case Opcode::NewObjectWithShape:
                    read = keyOfClass(value.layout());
                    break;
                default:
                    break;
                }
                if (read) {
                    if (uint32_t hub = hubOf(read, false); hub != UINT32_MAX)
                        add(hub, b, kindOfKey(read));
                }
                bool isCall = value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect || value.opcode == Opcode::Construct
                    || (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).callsUserCode());
                if (!isCall)
                    continue;
                if (value.opcode == Opcode::Call && !isCold[b]) {
                    if (auto index = parameterThatIs(value.child(0)))
                        foundIn[i].callsOfParameters.append(CallOfParameter { *index, weightOf(b) });
                }
                bool iterates = (value.opcode == Opcode::CallMethod && iteratingNames.get(value.string().index()))
                    || (value.opcode == Opcode::CallBuiltin && isIteratingName(Builtins::singleton().at(value.builtin()).name.span8()));
                bool isKnown = value.opcode == Opcode::CallDirect && !program.at(value.function()).blocks.isEmpty() && program.at(value.function()).blocks[0].parameters.size() == value.arguments.size();
                for (unsigned argument = 0; argument < value.arguments.size(); ++argument) {
                    if (FunctionID handed = functionMadeBy(function, value.arguments[argument])) {
                        add(enteredByValue(handed), b, 1, iterates);
                        if (isKnown)
                            foundIn[i].functionsHanded.append(Handed { i, static_cast<uint32_t>(edges[i].size() - 1), value.function().index(), argument, weightOf(b) });
                    } else if (isKnown && !isCold[b]) {
                        if (auto index = parameterThatIs(value.arguments[argument]))
                            foundIn[i].parametersPassedOn.append(PassedOn { i, *index, value.function().index(), argument, weightOf(b) });
                    }
                }
            }
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto& call : foundIn[i].callsOfParameters)
            noteCallOfParameter(i, call.index, call.weight);
        parametersPassedOn.appendVector(foundIn[i].parametersPassedOn);
        functionsHanded.appendVector(foundIn[i].functionsHanded);
    }
    destroyBeside(std::exchange(foundIn, { }));
    for (auto& evaluation : program.moduleEvaluations) {
        for (FunctionID callee : evaluation.callees)
            edges[evaluation.function.index()].append(Edge { callee.index(), 0, false, 0 });
    }

    for (unsigned round = 0; round < 6; ++round) {
        bool changed = false;
        for (auto& passed : parametersPassedOn) {
            auto further = weightOfCallsOfParameter.find(keyOfParameter(passed.callee, passed.argument));
            if (further == weightOfCallsOfParameter.end())
                continue;
            int weight = std::clamp(passed.weight + further->value, -30, 15);
            changed |= noteCallOfParameter(passed.function, passed.index, weight);
        }
        if (!changed)
            break;
    }
    for (auto& handed : functionsHanded) {
        auto called = weightOfCallsOfParameter.find(keyOfParameter(handed.callee, handed.argument));
        if (called != weightOfCallsOfParameter.end())
            edges[handed.function][handed.edge].shown = static_cast<int8_t>(std::clamp(handed.weight + called->value, -30, 15));
    }

    unsigned nodes = edges.size();
    Vector<int8_t> shareOfHub(FillWith { }, nodes, static_cast<int8_t>(0));
    for (uint32_t hub = count; hub < nodes; ++hub) {
        for (size_t candidates = edges[hub].size(); candidates > 1; candidates >>= 1)
            ++shareOfHub[hub];
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
    for (FunctionID evaluator : { program.moduleEvaluator, program.moduleEvaluatorIfNeeded, program.moduleEvaluatorFromOutside }) {
        if (evaluator)
            roots.append(evaluator.index());
    }
    BitVector isReached(nodes);
    Vector<uint32_t> reachedAndNotLeft;
    for (uint32_t root : roots) {
        if (!isReached.set(root))
            reachedAndNotLeft.append(root);
    }
    while (!reachedAndNotLeft.isEmpty()) {
        for (auto& edge : edges[reachedAndNotLeft.takeLast()]) {
            if (!isReached.set(edge.to))
                reachedAndNotLeft.append(edge.to);
        }
    }
    uint64_t sizeOfAll = 0;
    for (unsigned i = 0; i < count; ++i)
        sizeOfAll += isReached.get(i) ? sizes[i] : 0;
    struct Term {
        uint32_t from;
        uint32_t to;
        double factor;
    };
    constexpr uint32_t nodesOfSlice = 1024;
    unsigned slices = (nodes + nodesOfSlice - 1) / nodesOfSlice;
    auto forEachTermFromSlice = [&](unsigned slice, auto&& functor) {
        uint32_t behindTheSlice = std::min<uint32_t>(nodes, (slice + 1) * nodesOfSlice);
        for (uint32_t from = slice * nodesOfSlice; from < behindTheSlice; ++from) {
            for (auto& edge : edges[from]) {
                if (!edge.isCold && edge.kind <= 2)
                    functor(from, edge);
            }
        }
    };
    constexpr unsigned rangesOfTargets = 64;
    uint32_t nodesOfRange = std::max(1u, (nodes + rangesOfTargets - 1) / rangesOfTargets);
    Vector<std::array<uint32_t, rangesOfTargets>> placeBySliceAndRange(slices);
    forEachIndexInParallel(slices, [&](unsigned slice) {
        std::array<uint32_t, rangesOfTargets> numbers { };
        forEachTermFromSlice(slice, [&](uint32_t, const Edge& edge) {
            ++numbers[edge.to / nodesOfRange];
        });
        placeBySliceAndRange[slice] = numbers;
    });
    std::array<uint32_t, rangesOfTargets + 1> firstTermOfRange;
    uint32_t numberOfTerms = 0;
    for (unsigned range = 0; range < rangesOfTargets; ++range) {
        firstTermOfRange[range] = numberOfTerms;
        for (unsigned slice = 0; slice < slices; ++slice)
            numberOfTerms += std::exchange(placeBySliceAndRange[slice][range], numberOfTerms);
    }
    firstTermOfRange[rangesOfTargets] = numberOfTerms;
    auto roomForTermsByRange = MallocSpan<Term>::malloc(numberOfTerms * sizeof(Term));
    auto roomForTermsByTarget = MallocSpan<Term>::malloc(numberOfTerms * sizeof(Term));
    std::span<Term> termsByRange = roomForTermsByRange.mutableSpan();
    std::span<Term> termsByTarget = roomForTermsByTarget.mutableSpan();
    forEachIndexInParallel(slices, [&](unsigned slice) {
        auto places = placeBySliceAndRange[slice];
        forEachTermFromSlice(slice, [&](uint32_t from, const Edge& edge) {
            double gain = edge.shown;
            if (gain < 0)
                gain *= 0.5;
            if (edge.kind == 2 && from >= count)
                gain -= shareOfHub[from];
            termsByRange[places[edge.to / nodesOfRange]++] = Term { from, edge.to, std::exp2(gain) };
        });
    });
    Vector<uint32_t> firstTermTo(FillWith { }, nodes + 1, 0u);
    firstTermTo[nodes] = numberOfTerms;
    Vector<uint32_t> placeOfNextTermTo(FillWith { }, nodes, 0u);
    forEachIndexInParallel(rangesOfTargets, [&](unsigned range) {
        auto termsOfRange = termsByRange.subspan(firstTermOfRange[range], firstTermOfRange[range + 1] - firstTermOfRange[range]);
        for (auto& term : termsOfRange)
            ++firstTermTo[term.to];
        uint32_t next = firstTermOfRange[range];
        uint32_t behindTheRange = static_cast<uint32_t>(std::min<uint64_t>(nodes, static_cast<uint64_t>(range + 1) * nodesOfRange));
        for (uint32_t node = std::min(nodes, range * nodesOfRange); node < behindTheRange; ++node) {
            placeOfNextTermTo[node] = next;
            next += std::exchange(firstTermTo[node], next);
        }
        for (auto& term : termsOfRange)
            termsByTarget[placeOfNextTermTo[term.to]++] = term;
    });
    roomForTermsByRange = { };
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
    std::atomic<uint32_t> nextSliceOfSomeRound { 0 };
    std::array<std::atomic<uint32_t>, rounds> slicesDoneOfRound { };
    forEachIndexInParallel(std::min<unsigned>(WTF::numberOfProcessorCores(), slices), [&](unsigned) {
        for (;;) {
            uint32_t taken = nextSliceOfSomeRound.fetch_add(1, std::memory_order_relaxed);
            if (taken >= rounds * slices)
                return;
            unsigned round = taken / slices;
            unsigned slice = taken % slices;
            while (round && slicesDoneOfRound[round - 1].load(std::memory_order_acquire) != slices)
                Thread::yield();
            auto& from = round % 2 ? next : flows;
            auto& to = round % 2 ? flows : next;
            uint32_t behindTheSlice = std::min<uint32_t>(nodes, (slice + 1) * nodesOfSlice);
            for (uint32_t node = slice * nodesOfSlice; node < behindTheSlice; ++node) {
                double sum = 0.0;
                double sumByNearness = 0.0;
                for (uint32_t k = firstTermTo[node]; k < firstTermTo[node + 1]; ++k) {
                    auto& flow = from[termsByTarget[k].from];
                    sum += flow[0] * termsByTarget[k].factor;
                    sumByNearness += flow[1] * termsByTarget[k].factor;
                }
                to[node] = { std::min(atRoots[node] + scales[0] * sum, 1e250), std::min(atRoots[node] + scales[1] * sumByNearness, 1e250) };
            }
            slicesDoneOfRound[round].fetch_add(1, std::memory_order_release);
        }
    });
    std::array<Vector<uint8_t>, 2> twentieths;
    for (unsigned which = 0; which < 2; ++which) {
        Vector<std::pair<double, uint32_t>> order(count, [&](size_t i) {
            double flow = flows[i][which];
            if (sizes[i] < 256) {
                double timesSmaller = 256.0 / std::max(1u, sizes[i]);
                flow *= timesSmaller * timesSmaller;
            }
            double mass = massOf[i] < 1e30f ? massOf[i] : 1e30;
            return std::pair { flow * mass / std::max(1u, sizes[i]), static_cast<uint32_t>(i) };
        });
        sortOnPool(order.mutableSpan(), std::less { });
        Vector<uint8_t> result(FillWith { }, count, static_cast<uint8_t>(0));
        uint64_t before = 0;
        for (auto [key, function] : order) {
            result[function] = static_cast<uint8_t>(sizeOfAll ? std::min<uint64_t>(19, before * 20 / sizeOfAll) : 0);
            before += isReached.get(function) ? sizes[function] : 0;
        }
        twentieths[which] = WTF::move(result);
    }
    static constexpr uint8_t leastThatIsNearTheRoots = 18;
    return Vector<uint8_t>(count, [&](size_t i) {
        return twentieths[1][i] >= leastThatIsNearTheRoots ? twentiethOfWhatIsNearTheRoots : twentieths[0][i];
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
        LayoutID itsClass = classes[described.index()];
        String className = itsClass ? text(program.at(itsClass).name) : String();
        String path = function.module ? text(program.modules[function.module.index()].path) : String();
        result.append(makeString(className, className.isEmpty() ? ""_s : "."_s, text(function.name), '\t', entered[i] ? "generic-"_s : ""_s, entrypointOf(described), '\t', i, '\t', path, '\t', function.position, '\t', program.isHotByText(FunctionID(i)) ? 'h' : program.isRareByText(FunctionID(i)) ? 'r' : 'c'));
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
    ASCIILiteral whyFailed;
    auto program = Program::tryRead(bytes, whyFailed);
    if (!program) {
        out.print(whyFailed);
        return false;
    }
    if (stage == "read"_s) {
        program->dump(out);
        return true;
    }
    if (stage == "optimized"_s) {
        auto findings = optimize(*program);
        program->dump(out);
        for (size_t index : program->stringsMadeOnce)
            out.print("made once: ", String::fromUTF8(program->strings[index]), "\n");
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage == "allocations"_s) {
        program->notesAllocations = true;
        auto findings = optimize(*program);
        printAllocationRemarks(*program, out);
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    if (stage == "uses"_s) {
        auto findings = optimize(*program);
        printUsesOfAllocations(*program, out);
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
    // "functions:12,34": those functions, once the passes have run.
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
