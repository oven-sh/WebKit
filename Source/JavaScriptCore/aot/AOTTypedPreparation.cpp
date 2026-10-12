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
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
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
    if (from == to || at(to).admitsForeignObjects)
        return true;
    return !at(from).admitsForeignObjects && isObjectLayout(at(from).kind) && std::ranges::includes(at(to).members, at(from).members);
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
        return type.primitives & Primitive::String;
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
    auto iterator = m_stringIDs.find(keyOf(bytes));
    if (iterator == m_stringIDs.end())
        return std::nullopt;
    return StringID(iterator->value);
}

StringID Program::intern(std::span<const uint8_t> bytes)
{
    auto result = m_stringIDs.add(keyOf(bytes), static_cast<uint32_t>(strings.size()));
    if (result.isNewEntry) {
        m_ownStrings.append(Vector<uint8_t>(bytes));
        strings.append(m_ownStrings.last().span());
        noteString(StringID(result.iterator->value));
    }
    return StringID(result.iterator->value);
}

void Program::replaceString(std::span<const uint8_t> from, std::span<const uint8_t> to)
{
    RELEASE_ASSERT(m_stringIDs.isEmpty());
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

Vector<StringID> Program::genericAccessIdentifiers() const
{
    BitVector seen;
    Vector<StringID> result;
    auto note = [&](StringID name) {
        // `#name`, and a name with half a surrogate pair, are no UTF-8: such an access has no cache, and the run time makes out the name.
        if (name && !seen.set(name.index()) && !String::fromUTF8(string(name)).isNull())
            result.append(name);
    };
    Vector<Vector<StringID>> namesOfFunctions(functions.size());
    forEachIndexInParallel(functions.size(), [&](unsigned i) {
        auto& function = functions[i];
        for (auto& value : function.values) {
            StringID name;
            if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod || value.opcode == Opcode::InById || value.opcode == Opcode::DeleteById
                || value.opcode == Opcode::GetGlobal || value.opcode == Opcode::GetGlobalForTypeOf || value.opcode == Opcode::PutGlobal)
                name = value.string();
            // The generic fallback.
            else if (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).takesReceiver())
                name = stringOf(Builtins::singleton().at(value.builtin()).nameName);
            // A definition under a name: DefineField (object, name, value), Object.defineProperty(object, name, descriptor).
            else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 3 && function.at(value.arguments[1]).opcode == Opcode::String) {
                auto& builtin = Builtins::singleton().at(value.builtin());
                bool isDefineField = builtin.implementation.kind == BuiltinImplementation::Operation && builtin.implementation.operation == LanguageOperation::DefineField;
                bool isDefineProperty = builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
                if (isDefineField || isDefineProperty)
                    name = function.at(value.arguments[1]).string();
            }
            if (name)
                namesOfFunctions[i].append(name);
        }
    });
    for (auto& names : namesOfFunctions) {
        for (StringID name : names)
            note(name);
    }
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
    return result;
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

class Preparation {
public:
    explicit Preparation(Program& program)
        : m_program(program)
    {
    }

    void run()
    {
        Vector<String> keys(m_program.strings.size());
        forEachIndexInParallel(keys.size(), [&](unsigned i) {
            keys[i] = keyOf(m_program.strings[i]);
            keys[i].impl()->hash();
        });
        m_program.m_stringIDs.reserveInitialCapacity(keys.size());
        for (uint32_t i = 0; i < keys.size(); ++i)
            m_program.m_stringIDs.add(WTF::move(keys[i]), i);
        m_program.emptyString = m_program.intern({ });
        if (m_program.find("NewDisposableStack"_s.span8())) {
            m_program.intern("The value of a using declaration must be an object, null or undefined"_s.span8());
            m_program.intern("The value of a using declaration has no callable dispose method"_s.span8());
            for (ASCIILiteral name : { "Symbol"_s, "dispose"_s, "TypeError"_s, "SuppressedError"_s })
                m_program.intern(name.span8());
        }
        expandInitialize();
        linkForInKeys();
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
            auto& function = m_program.functions[i];
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
        auto initialize = m_program.find("Initialize"_s.span8());
        if (!initialize)
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
            for (auto& block : function.blocks) {
                for (size_t at = 0; at < block.values.size(); ++at) {
                    if (function.at(block.values[at]).opcode != Opcode::Operation || function.at(block.values[at]).string() != *initialize)
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
                    leftOver.assertedType = { };
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
            hasLongLiteral[i] = m_program.functions[i].values.containsIf([&](auto& value) {
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

                StringID newContiguousArray = m_program.intern("NewContiguousArray"_s.span8());
                StringID putContiguousIndex = m_program.intern("PutContiguousIndex"_s.span8());
                Vector<ValueID> values;
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
                    operation.immediates = { putContiguousIndex.index(), Format::none };
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
                    literal.immediates = { newContiguousArray.index(), Format::none };
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
        auto prototype = m_program.find("prototype"_s.span8());
        auto createMappedArguments = m_program.find("CreateMappedArguments"_s.span8());
        auto makeClass = m_program.find("MakeClass"_s.span8());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
            auto note = [&](ValueID id) {
                FunctionID predicted = function.at(id).predictedClosure;
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
                for (FunctionID each : { used, predicted, definition.predictedClosure }) {
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
                    if (createMappedArguments && value.string() == *createMappedArguments) {
                        isCalleeOfItsArguments = true;
                        note(value.arguments[1]);
                    } else if (makeClass && value.string() == *makeClass)
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
        auto forInKeys = m_program.find("ForInKeys"_s.span8());
        auto hasEnumerableProperty = m_program.find("HasEnumerableProperty"_s.span8());
        if (!forInKeys || !hasEnumerableProperty)
            return;
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned indexOfFunction) {
            auto& function = m_program.functions[indexOfFunction];
            auto originOf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            for (auto& value : function.values) {
                if (value.opcode != Opcode::Operation || value.string() != *hasEnumerableProperty || value.arguments.size() != 2)
                    continue;
                auto& key = function.at(originOf(value.arguments[1]));
                if (key.opcode != Opcode::GetByVal)
                    continue;
                ValueID keys = originOf(key.child(0));
                auto& listing = function.at(keys);
                if (listing.opcode == Opcode::Operation && listing.string() == *forInKeys && listing.arguments[0] == value.arguments[0])
                    value.arguments.append(keys);
            }
        });
    }

    Program& m_program;
};

void prepare(Program& program)
{
    Preparation(program).run();
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
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce || opcode == Opcode::NewObjectWithShape)
                    births[i].append(id);
            }
        }
    });
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
        for (ValueID id : births[i]) {
            auto& value = function.at(id);
            if (value.opcode != Opcode::NewObjectWithShape) {
                needClosureWith(value.arguments.size());
                continue;
            }
            auto& allocation = function.allocations[value.allocation()];
            program.at(allocation.layout).isAllocated = true;
            value.immediates[1] = numberOf(allocation);
        }
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        program.functions[i].allocations.clear();
    });
    // `new (F as any)()`, Reflect.construct(F, ..), a bound F: whoever constructs does not know the function, and leaves the object to the
    // run time. It is born with nothing, like the one `new` makes for a function it knows, and gets its prototype when it is made.
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
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

// The blocks that a block with a LoopHint reaches and that reach it.
BitVector blocksInLoops(const Function& function)
{
    unsigned numberOfBlocks = function.blocks.size();
    BitVector result(numberOfBlocks);
    Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    for (unsigned header = 0; header < numberOfBlocks; ++header) {
        bool isHeader = function.blocks[header].values.containsIf([&](ValueID id) {
            return function.at(id).opcode == Opcode::LoopHint;
        });
        if (!isHeader || result.get(header))
            continue;
        BitVector isReached(numberOfBlocks);
        Vector<unsigned, 16> worklist { header };
        while (!worklist.isEmpty()) {
            for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        if (!isReached.get(header))
            continue;
        BitVector reaches(numberOfBlocks);
        reaches.set(header);
        worklist.append(header);
        while (!worklist.isEmpty()) {
            for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                if (isReached.get(predecessor) && !reaches.set(predecessor))
                    worklist.append(predecessor);
            }
        }
        result.merge(reaches);
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
    for (auto& [reference, blocks] : blocksOfUsers) {
        if (blocks.size() < 2 || stays.get(reference))
            continue;
        BitVector isReached(function.blocks.size());
        Vector<unsigned, 16> worklist;
        for (unsigned b : blocks)
            worklist.append(b);
        while (!worklist.isEmpty()) {
            for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        if (blocks.containsIf([&](unsigned b) { return isReached.get(b); }))
            stays.set(reference);
    }
    unsigned numberOfValues = function.values.size();
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> copies;
        Vector<ValueID> values;
        for (ValueID id : function.blocks[b].values) {
            if (id.index() < numberOfValues && definingBlock[id.index()] == b && blocksOfUsers.contains(id.index()) && !stays.get(id.index()))
                continue;
            Vector<ValueID, 2> wanted;
            std::as_const(function.at(id)).forEachChild([&](ValueID child) {
                if (child.index() < numberOfValues && definingBlock[child.index()] != none && !stays.get(child.index()) && !copies.contains(child.index()) && !wanted.contains(child))
                    wanted.append(child);
            });
            for (ValueID original : wanted) {
                ValueID copy(function.values.size());
                function.values.append(Value(function.at(original)));
                copies.add(original.index(), copy);
                values.append(copy);
            }
            function.at(id).forEachChild([&](ValueID& child) {
                if (auto copy = copies.get(child.index()))
                    child = copy;
            });
            values.append(id);
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
        BitVector isInLoop = blocksInLoops(program.functions[i]);
        bool isInCycle = program.functionsInCycles.get(i);
        if (isInLoop.isEmpty() && !isInCycle)
            return;
        calledInLoops[i] = Vector<unsigned>();
        forEachFunctionNamed(program.functions[i], isInCycle ? nullptr : &isInLoop, false, [&](unsigned callee) {
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
    BitVector hasLoop = result;
    isCalled.merge(program.functionsInCycles);
    BitVector isCalledOrMade = isCalled;
    BitVector isCalledNearLoops = isCalled;
    for (unsigned i : frontier) {
        forEachFunctionNamed(program.functions[i], nullptr, true, [&](unsigned callee) {
            isCalledOrMade.set(callee);
        });
        forEachFunctionNamed(program.functions[i], nullptr, false, [&](unsigned callee) {
            isCalledNearLoops.set(callee);
        });
    }
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
    for (unsigned i : warm) {
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference)
                made.set(value.function().index());
        }
    }
    if (forced == 1) [[unlikely]]
        made.clearAll();
    warm.merge(made);
    hasLoop.exclude(isCalled);
    program.functionsHotOnlyInTheirLoops = WTF::move(hasLoop);
    for (auto& function : program.functions) {
        if (!function.has(Format::FunctionIsHiddenInStackTraces) || function.isGenericEntrypoint())
            continue;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Invoke)
                continue;
            for (ValueID id : function.at(block.terminator.targets[1].block).values) {
                if (function.at(id).opcode == Opcode::CallDirect) {
                    result.clear(function.at(id).function().index());
                    warm.clear(function.at(id).function().index());
                    isCalledNearLoops.clear(function.at(id).function().index());
                }
            }
        }
    }
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
            if (value.predictedClosure)
                WTF::atomicExchangeAdd(&timesMadeAValue[value.predictedClosure.index()], 1u, std::memory_order_relaxed);
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
        auto& function = program.functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        return isPlain && !isMadeAValue.get(i) && !function.isGenericEntrypoint() && !function.has(Format::FunctionIsBuiltin) && !program.isResumeFunction(function) && !program.isEntry(FunctionID(i)) && !function.resumeFunction && !function.startFunction;
    };
    Vector<uint32_t> callsInRareCode;
    callsInRareCode.fill(0, count);
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        if ((function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i))) {
            program.functionsRareByText.set(i);
            worklist.append(i);
        }
    }
    while (!worklist.isEmpty()) {
        auto& function = program.functions[worklist.takeLast()];
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (isInLoop.get(b))
                continue;
            for (ValueID id : function.blocks[b].values) {
                if (function.at(id).opcode != Opcode::CallDirect)
                    continue;
                unsigned callee = function.at(id).function().index();
                if (mayBeRare(callee) && ++callsInRareCode[callee] == program.numberOfDirectCalls[callee] && !program.functionsRareByText.set(callee))
                    worklist.append(callee);
            }
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.functionsRareByText.bitCount(), " of ", count, " functions are rare by the text");
}

// A concatenation makes a string: in place that is a few hundred bytes at every site. Where the text does not say hot there is one body for each signature, and the sites call it.
static bool copiesGoByWhereTheyStand()
{
    static const bool result = getenv("AOT_SWITCH_COPIES_BY_PLACE");
    return result;
}

static std::array<std::atomic<unsigned>, 3> s_sitesInCopiesOfHotOriginals;

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

static void giveConcatenationsOneBody(Program& program)
{
    constexpr unsigned maxOperands = 6;
    unsigned count = program.functions.size();
    struct Site {
        ValueID value;
        String signature;
    };
    Vector<Vector<Site>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        if ((program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i))) || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::Strcat || value.arguments.size() > maxOperands)
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                if (isInCopyOfHotOriginal(program, function, value, false)) {
                    ++s_sitesInCopiesOfHotOriginals[0];
                    if (!copiesGoByWhereTheyStand())
                        continue;
                }
                StringBuilder signature;
                signature.append(value.type.index());
                for (ValueID argument : value.arguments)
                    signature.append(' ', function.at(argument).type.index());
                sites[i].append({ id, signature.toString() });
            }
        }
    });
    enum : unsigned { CallsBody, IsHotByText, HasTooManyParts, IsInLoop, IsCopiedFromHot, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = program.functions[i];
        if (function.blocks.isEmpty())
            continue;
        bool isHot = program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i));
        BitVector isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Strcat)
                    ++numbers[isHot ? IsHotByText : value.arguments.size() > maxOperands ? HasTooManyParts : isInLoop.get(b) ? IsInLoop : isInCopyOfHotOriginal(program, function, value, false) && !copiesGoByWhereTheyStand() ? IsCopiedFromHot : CallsBody];
            }
        }
    }
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> bodyOfSignature;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        for (auto& site : sites[i]) {
            auto& value = function.at(site.value);
            uint32_t body = bodyOfSignature.ensure(site.signature, [&] {
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
                    auto& argument = function.at(value.arguments[k]);
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
                made.blocks[0].values.append(ValueID(made.values.size()));
                made.blocks[0].terminator.kind = TerminatorKind::Return;
                made.blocks[0].terminator.value = ValueID(made.values.size());
                made.values.append(WTF::move(concatenation));
                bodies.append(WTF::move(made));
                return count + static_cast<uint32_t>(bodies.size()) - 1;
            }).iterator->value;
            value.opcode = Opcode::CallDirect;
            value.immediates = { body, Format::none };
            value.takePredictionOf(Value { });
        }
    }
    if (Options::verboseAOTCompilation()) {
        uint64_t taken = 0;
        for (auto& ofFunction : sites)
            taken += ofFunction.size();
        RELEASE_ASSERT(numbers[CallsBody] == taken);
        printSitesOfPassForOneBody("concatenations"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "the function is hot by the text"_s, numbers[IsHotByText] }, { "more than six parts"_s, numbers[HasTooManyParts] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot"_s, numbers[IsCopiedFromHot] } });
    }
    program.functions.appendVector(WTF::move(bodies));
}

static std::optional<BitVector> functionsOfColdSet(Program&, const char* text);

// A call of a method or a static function of the engine is lowered in place, with its tests and its general way: a few hundred bytes at every site. Where the text does not say hot there is
// one body for each signature that two sites have, and the sites call it.
static void giveCallsOfBuiltinsOneBody(Program& program)
{
    constexpr unsigned maxOperands = 6;
    auto& builtins = Builtins::singleton();
    auto operation = [&](ASCIILiteral name) {
        return builtins.find(BuiltinKind::Operation, { }, builtins.nameOf(name.span8()));
    };
    std::array<BuiltinID, 3> spreads { operation("AppendSpread"_s), operation("Apply"_s), operation("CallWithSpread"_s) };
    static const char* const asked = getenv("BUN_AOT_SWITCH_ONE_BODY");
    bool admitsAccessors = false;
    bool admitsFunctions = false;
    bool admitsPush = false;
    bool takesFunctionsNearLoops = false;
    std::optional<BitVector> setOfWhatIsNew;
    BitVector isNamed(builtins.size());
    if (asked) {
        bool isUnderstood = true;
        const char* text = asked;
        for (; *text && *text != ','; ++text) {
            admitsAccessors |= *text == 'g';
            admitsFunctions |= *text == 'f';
            admitsPush |= *text == 'p';
            isUnderstood &= !!strchr("gfp-", *text);
        }
        auto nextField = [&](char separator) {
            const char* start = ++text;
            while (*text && *text != ',' && *text != separator)
                ++text;
            return String::fromUTF8(std::span { start, static_cast<size_t>(text - start) });
        };
        if (*text == ',') {
            String set = nextField(',');
            if (set == "n"_s)
                takesFunctionsNearLoops = true;
            else if (set != "l"_s) {
                CString bytes = set.utf8();
                setOfWhatIsNew = functionsOfColdSet(program, byteCast<char>(bytes.data()));
                isUnderstood &= !!setOfWhatIsNew;
            }
        }
        while (*text) {
            String name = nextField('|');
            bool isFound = false;
            for (unsigned i = 0; i < builtins.size(); ++i) {
                auto& builtin = builtins.at(BuiltinID(i));
                if (name != makeString(builtin.holder, builtin.holder.isEmpty() ? ""_s : "."_s, builtin.name))
                    continue;
                isNamed.set(i);
                isFound = true;
            }
            isUnderstood &= isFound;
        }
        RELEASE_ASSERT_WITH_MESSAGE(isUnderstood, "BUN_AOT_SWITCH_ONE_BODY is not <letters of gfp, or ->[,<l | n | a cold set as AOT_X_COLD_SET has it>[,<public name of a built-in>|..]]");
    }
    enum class Candidate : uint8_t { No, AsLanded, New };
    auto isCandidate = [&](const Value& value) {
        // `s + t` makes a string as a concatenation does.
        if (value.opcode == Opcode::Add)
            return program.at(value.type).isString() ? Candidate::AsLanded : Candidate::No;
        if (value.opcode != Opcode::CallBuiltin || value.child(1) || value.child(2) || value.arguments.size() + !!value.child(0) > maxOperands)
            return Candidate::No;
        // Its site is small: the usual way in line, all else in a stub.
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush)
            return admitsPush || isNamed.get(value.builtin().index()) ? Candidate::New : Candidate::No;
        BuiltinKind kind = builtins.at(value.builtin()).kind;
        if (kind == BuiltinKind::Method || kind == BuiltinKind::StaticFunction || std::ranges::find(spreads, value.builtin()) != spreads.end())
            return Candidate::AsLanded;
        if (isNamed.get(value.builtin().index()) || (admitsAccessors && (kind == BuiltinKind::Getter || kind == BuiltinKind::Setter)) || (admitsFunctions && (kind == BuiltinKind::Function || kind == BuiltinKind::Constructor)))
            return Candidate::New;
        return Candidate::No;
    };
    auto isDescribedByLiteral = [&](const Function& function, const Value& value) {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3 || function.at(value.arguments[2]).opcode != Opcode::NewObject)
            return false;
        auto& builtin = builtins.at(value.builtin());
        return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
    };
    auto staysInBody = [](const Value& operand, bool isNew) {
        return (isNew && operand.opcode == Opcode::String) || operand.opcode == Opcode::Integer || operand.opcode == Opcode::Double || operand.opcode == Opcode::Boolean || operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null;
    };
    auto forEachOperand = [](auto& value, auto&& functor) {
        for (auto& child : value.children) {
            if (child)
                functor(child);
        }
        for (auto& argument : value.arguments)
            functor(argument);
    };
    unsigned count = program.functions.size();
    struct Site {
        ValueID value;
        String signature;
    };
    Vector<Vector<Site>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        bool isRefusedForLoopsNearby = program.isCalledNearLoops(FunctionID(i)) && !takesFunctionsNearLoops;
        if ((isRefusedForLoopsNearby && isNamed.isEmpty()) || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        bool hasHandlers = function.blocks.containsIf([](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke;
        });
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                Candidate candidate = isCandidate(value);
                if (candidate == Candidate::No || isDescribedByLiteral(function, value))
                    continue;
                bool isTakenEverywhere = value.opcode == Opcode::CallBuiltin && isNamed.get(value.builtin().index());
                bool isNew = candidate == Candidate::New;
                if ((isRefusedForLoopsNearby && !isTakenEverywhere) || (isNew && !isTakenEverywhere && setOfWhatIsNew && !setOfWhatIsNew->get(i)))
                    continue;
                if (hasHandlers && (function.blocks[b].terminator.kind != TerminatorKind::Invoke || id != function.blocks[b].values.last()))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b) && !isTakenEverywhere)
                    continue;
                if (!isTakenEverywhere && isInCopyOfHotOriginal(program, function, value, !takesFunctionsNearLoops)) {
                    ++s_sitesInCopiesOfHotOriginals[1];
                    if (!copiesGoByWhereTheyStand())
                        continue;
                }
                if (isNew) {
                    bool hasOperandInNoRegister = false;
                    forEachOperand(value, [&](ValueID operandID) {
                        auto& operand = function.at(operandID);
                        hasOperandInNoRegister |= operand.rep == Rep::Void && operand.opcode != Opcode::Undefined && operand.opcode != Opcode::Null;
                    });
                    if (hasOperandInNoRegister)
                        continue;
                }
                StringBuilder signature;
                if (isNew)
                    signature.append('n', static_cast<unsigned>(value.hasStringKeysByType), static_cast<unsigned>(value.receiverIsExpectedOfOneRow), ' ', value.predictedByKey.index(), ' ', value.predictedClosure.index(), ' ', value.assertedType.index(), ' ');
                signature.append(static_cast<unsigned>(value.opcode), ' ', value.immediates[0], ' ', value.immediates[1], ' ', value.type.index(), ' ', value.prediction.index(), value.children[0] ? ' ' : '-');
                forEachOperand(value, [&](ValueID operandID) {
                    auto& operand = function.at(operandID);
                    signature.append(' ', operand.type.index(), '~', operand.prediction.index(), '~', operand.predictedClosure.index());
                    if (staysInBody(operand, isNew))
                        signature.append('=', static_cast<unsigned>(operand.opcode), ':', operand.immediates[0], ':', operand.immediates[1]);
                });
                sites[i].append({ id, signature.toString() });
            }
        }
    });
    auto forEachSite = [&](auto&& functor) {
        for (unsigned i = 0; i < count; ++i) {
            auto& function = program.functions[i];
            for (auto& site : sites[i])
                functor(function, function.at(site.value), site.signature);
        }
    };
    UncheckedKeyHashMap<String, uint32_t> sitesOfSignature;
    forEachSite([&](Function&, Value&, const String& signature) {
        ++sitesOfSignature.add(signature, 0).iterator->value;
    });
    enum : unsigned { CallsBody, OccursOnce, IsNearLoops, IsGetterOrSetter, IsOperation, IsOfOtherKind, IsPush, HasTooManyOperands, IsDescribedByLiteral, IsOutsideSet, IsBesideHandlers, IsInLoop, IsLeft, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = program.functions[i];
        if (function.blocks.isEmpty())
            continue;
        BitVector isSite;
        BitVector isSharedSite;
        for (auto& site : sites[i]) {
            isSite.set(site.value.index());
            if (sitesOfSignature.get(site.signature) >= 2)
                isSharedSite.set(site.value.index());
        }
        bool isNearLoops = program.isCalledNearLoops(FunctionID(i)) && !takesFunctionsNearLoops;
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
                bool isTakenEverywhere = isNamed.get(value.builtin().index());
                Candidate candidate = isCandidate(value);
                unsigned reason = IsLeft;
                if (isSite.get(id.index()))
                    reason = isSharedSite.get(id.index()) ? CallsBody : OccursOnce;
                else if (isNearLoops && !isTakenEverywhere)
                    reason = IsNearLoops;
                else if (candidate == Candidate::No) {
                    if (value.child(1) || value.child(2) || value.arguments.size() + !!value.child(0) > maxOperands)
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
                else if (candidate == Candidate::New && !isTakenEverywhere && setOfWhatIsNew && !setOfWhatIsNew->get(i))
                    reason = IsOutsideSet;
                else if (hasHandlers && (function.blocks[b].terminator.kind != TerminatorKind::Invoke || id != function.blocks[b].values.last()))
                    reason = IsBesideHandlers;
                else if (isInLoop.get(b) && !isTakenEverywhere)
                    reason = IsInLoop;
                ++numbers[reason];
            }
        }
    }
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> bodyOfSignature;
    uint64_t sitesTakenAsLanded = 0;
    uint64_t newSitesTaken = 0;
    forEachSite([&](Function& function, Value& value, const String& signature) {
        if (sitesOfSignature.get(signature) < 2)
            return;
        bool isNew = signature[0] == 'n';
        ++(isNew ? newSitesTaken : sitesTakenAsLanded);
        uint32_t body = bodyOfSignature.ensure(signature, [&] {
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
                if (staysInBody(operand, isNew))
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
            bodies.append(WTF::move(made));
            return count + static_cast<uint32_t>(bodies.size()) - 1;
        }).iterator->value;
        Arguments passed;
        forEachOperand(value, [&](ValueID operandID) {
            if (!staysInBody(function.at(operandID), isNew))
                passed.append(operandID);
        });
        value.opcode = Opcode::CallDirect;
        value.immediates = { body, Format::none };
        value.children = { };
        value.arguments = WTF::move(passed);
    });
    for (unsigned i = 0; newSitesTaken && i < count; ++i) {
        if (!sites[i].containsIf([](auto& site) { return site.signature[0] == 'n'; }))
            continue;
        auto& function = program.functions[i];
        BitVector isUsed(function.values.size());
        auto use = [&](ValueID id) {
            isUsed.set(id.index());
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values)
                function.at(id).forEachChild(use);
            block.terminator.forEachChild(use);
        }
        for (auto& block : function.blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                return function.at(id).opcode == Opcode::String && !isUsed.get(id.index());
            });
        }
    }
    if (Options::verboseAOTCompilation())
        printSitesOfPassForOneBody("calls of built-ins"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "a signature that occurs once"_s, numbers[OccursOnce] }, { "the function is called near loops"_s, numbers[IsNearLoops] }, { "a getter or a setter"_s, numbers[IsGetterOrSetter] }, { "an operation of the engine"_s, numbers[IsOperation] }, { "a function, a constructor, a constant"_s, numbers[IsOfOtherKind] }, { "push"_s, numbers[IsPush] }, { "more than six operands, or a second or third child"_s, numbers[HasTooManyOperands] }, { "defineProperty with a literal"_s, numbers[IsDescribedByLiteral] }, { "outside the cold set"_s, numbers[IsOutsideSet] }, { "beside handlers"_s, numbers[IsBesideHandlers] }, { "in a loop"_s, numbers[IsInLoop] }, { "copied from what is hot, or an operand in no register"_s, numbers[IsLeft] } });
    if (asked)
        dataLogLn("AOT: one body (", asked, "): ", sitesTakenAsLanded, " sites of the kinds that are landed and ", newSitesTaken, " of the kinds that are new call one of ", bodies.size(), " bodies");
    program.functions.appendVector(WTF::move(bodies));
}

bool mayLeaveConstructionToBody(const Program& program, FunctionID id, const Function& function, const Value& value)
{
    if (function.isBodyOfSeveralSites || program.isCalledNearLoops(id))
        return false;
    return copiesGoByWhereTheyStand() || !isInCopyOfHotOriginal(program, function, value, true);
}

static void giveConstructionsOneBody(Program& program)
{
    unsigned count = program.functions.size();
    Vector<Vector<uint8_t, 0>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
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
                if (isInCopyOfHotOriginal(program, function, value, true))
                    ++s_sitesInCopiesOfHotOriginals[2];
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
        made.module = program.functions[first[numberOfArguments]].module;
        made.position = program.functions[first[numberOfArguments]].position;
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
        auto& function = program.functions[i];
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
    static const char* const compactSet = getenv("BUN_AOT_SWITCH_COMPACT2");
    static const char* const compactLevers = getenv("BUN_AOT_SWITCH_COMPACT2_LEVERS");
    if (!compactSet || !compactLevers || !(static_cast<unsigned>(atoi(compactLevers)) >> Program::leverOfBodiesOfCasts & 1))
        return;
    unsigned count = program.functions.size();
    Vector<Vector<ValueID>> sites(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        FunctionID id(i);
        bool isInSet = !function.isBodyOfSeveralSites && (!strcmp(compactSet, "all") || (strchr(compactSet, 'g') && program.isColdByGraph(id)) || strchr(compactSet, program.isHotByText(id) ? 'h' : program.isRareByText(id) ? 'r' : 'c'));
        if (!isInSet || function.blocks.isEmpty())
            return;
        std::optional<BitVector> isInLoop;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID valueID : function.blocks[b].values) {
                auto& value = function.at(valueID);
                if (!Program::mayHaveBody(value) || value.rep == Rep::Void)
                    continue;
                Rep given = function.at(value.child(0)).rep;
                if ((given != Rep::JSValue && given != Rep::Cell) || program.isHeldAlready(program.typeOf(function, value.child(0)), program.at(value.type), value.castReason()))
                    continue;
                if (!isInLoop)
                    isInLoop = blocksInLoops(function);
                if (isInLoop->get(b))
                    break;
                sites[i].append(valueID);
            }
        }
    });
    UncheckedKeyHashMap<uint64_t, uint32_t> numberOfSites;
    for (unsigned i = 0; i < count; ++i) {
        for (ValueID valueID : sites[i])
            ++numberOfSites.add(Program::keyOfBodyOfCast(program.functions[i].at(valueID)), 0).iterator->value;
    }
    Vector<Function> bodies;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        for (ValueID valueID : sites[i]) {
            auto& value = function.at(valueID);
            uint64_t key = Program::keyOfBodyOfCast(value);
            if (numberOfSites.get(key) < minSites)
                continue;
            program.bodiesOfCasts.ensure(key, [&] {
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
                cast.position = function.position;
                cast.children[0] = given;
                cast.takePredictionOf(Value { });
                made.blocks[0].values.append(ValueID(made.values.size()));
                made.blocks[0].terminator.kind = TerminatorKind::Return;
                made.blocks[0].terminator.value = ValueID(made.values.size());
                made.values.append(WTF::move(cast));
                bodies.append(WTF::move(made));
                return count + static_cast<uint32_t>(bodies.size()) - 1;
            });
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", bodies.size(), " casts have a body that their sites in cold code call");
    enum : unsigned { CallsBody, HasTooFewSites, IsOutsideSet, MayHaveNoBody, IsGivenInNoRegisterOrHeld, IsInLoop, NumberOfReasons };
    std::array<uint64_t, NumberOfReasons> numbers { };
    for (unsigned i = 0; Options::verboseAOTCompilation() && i < count; ++i) {
        auto& function = program.functions[i];
        if (function.blocks.isEmpty())
            continue;
        FunctionID id(i);
        bool isInSet = !function.isBodyOfSeveralSites && (!strcmp(compactSet, "all") || (strchr(compactSet, 'g') && program.isColdByGraph(id)) || strchr(compactSet, program.isHotByText(id) ? 'h' : program.isRareByText(id) ? 'r' : 'c'));
        BitVector isSite;
        for (ValueID valueID : sites[i])
            isSite.set(valueID.index());
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
                else if ((given != Rep::JSValue && given != Rep::Cell) || program.isHeldAlready(program.typeOf(function, value.child(0)), program.at(value.type), value.castReason()))
                    reason = IsGivenInNoRegisterOrHeld;
                ++numbers[reason];
            }
        }
    }
    if (Options::verboseAOTCompilation())
        printSitesOfPassForOneBody("casts"_s, bodies.size(), { { "call a body"_s, numbers[CallsBody] }, { "fewer than four sites"_s, numbers[HasTooFewSites] }, { "outside the set"_s, numbers[IsOutsideSet] }, { "a cast that may have no body"_s, numbers[MayHaveNoBody] }, { "given in no register, or held already"_s, numbers[IsGivenInNoRegisterOrHeld] }, { "in a loop"_s, numbers[IsInLoop] } });
    program.functions.appendVector(WTF::move(bodies));
}

static std::optional<Program::ModuleEvaluation> asModuleEvaluation(const Program& program, FunctionID id)
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
    Program::ModuleEvaluation result;
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
    if (!has(first, { Opcode::Boolean, Opcode::PutModuleVariable, Opcode::CallDirect }, TerminatorKind::Invoke) || !at(first, 0).boolean() || at(first, 1).child(0) != first.values[0]
        || at(first, 1).variable() != result.isEvaluated || !isCall(at(first, 2)))
        return std::nullopt;
    BlockID handlerID = first.terminator.targets[1].block;
    auto& handler = function.blocks[handlerID.index()];
    if (forgets) {
        if (!has(handler, { Opcode::CatchValue, Opcode::Undefined, Opcode::PutModuleVariable }, TerminatorKind::Throw) || handler.terminator.value != handler.values[0]
            || at(handler, 2).child(0) != handler.values[1] || at(handler, 2).variable() != result.isEvaluated)
            return std::nullopt;
    } else if (!has(handler, { Opcode::CatchValue, Opcode::Boolean, Opcode::PutModuleVariable, Opcode::PutModuleVariable }, TerminatorKind::Throw) || handler.terminator.value != handler.values[0] || at(handler, 1).boolean()
        || at(handler, 2).child(0) != handler.values[1] || at(handler, 2).variable() != result.isEvaluated || at(handler, 3).child(0) != handler.values[0] || at(handler, 3).variable() != result.error)
        return std::nullopt;
    result.callees.append(at(first, 2).function());
    size_t numberOfBlocks = forgets ? 4 : 6;
    for (const Block* block = &blockAt(first, 0);; block = &blockAt(*block, 0)) {
        ++numberOfBlocks;
        if (numberOfBlocks > function.blocks.size())
            return std::nullopt;
        if (has(*block, { }, TerminatorKind::Return) && !block->terminator.value)
            break;
        if (!has(*block, { Opcode::CallDirect }, TerminatorKind::Invoke) || block->terminator.targets[1].block != handlerID || !isCall(at(*block, 0)))
            return std::nullopt;
        result.callees.append(at(*block, 0).function());
    }
    if (numberOfBlocks != function.blocks.size())
        return std::nullopt;
    return result;
}

static void shareModuleEvaluation(Program& program)
{
    unsigned count = program.functions.size();
    Vector<std::optional<Program::ModuleEvaluation>> found(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        found[i] = asModuleEvaluation(program, FunctionID(i));
    });
    Vector<uint8_t> isReachedOtherwise(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        for (auto& value : program.functions[i].values) {
            if (value.predictedClosure)
                WTF::atomicStore(&isReachedOtherwise[value.predictedClosure.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            bool namesFunction = value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::GetCapture;
            if (namesFunction || (value.opcode == Opcode::CallDirect && (!value.arguments.isEmpty() || value.isJumpToContinuation())))
                WTF::atomicStore(&isReachedOtherwise[value.function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        if (!found[i] || isReachedOtherwise[i])
            continue;
        program.moduleEvaluationOfFunction.add(i, program.moduleEvaluations.size());
        program.moduleEvaluations.append(WTF::move(*found[i]));
    }
    constexpr unsigned fewestWorthSharing = 4;
    if (program.moduleEvaluations.size() < fewestWorthSharing) {
        program.moduleEvaluations.clear();
        program.moduleEvaluationOfFunction.clear();
        return;
    }
    auto doesNothing = [&](FunctionID id) {
        auto& function = program.at(id);
        return function.blocks.size() == 1 && function.blocks[0].values.isEmpty() && function.blocks[0].terminator.kind == TerminatorKind::Return && !function.blocks[0].terminator.value;
    };
    for (auto& evaluation : program.moduleEvaluations) {
        evaluation.callees.removeAllMatching([&](FunctionID callee) {
            return doesNothing(callee);
        });
    }

    uint32_t flags = program.at(program.moduleEvaluations[0].function).flags;
    uint32_t numberOfRows = program.moduleEvaluations.size();
    auto& types = program.types;
    auto make = [&](unsigned numberOfBlocks, auto&& fill) {
        Function made;
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
            (opcode == Opcode::Parameter ? made.blocks[block].parameters : made.blocks[block].values).append(id);
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
    program.functionsHotByText.set(program.moduleEvaluator.index());
    program.functionsWarmByText.set(program.moduleEvaluator.index());
    make(14, [&](auto& add, auto& end) {
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
        ValueID flag = add(entry, Opcode::GetModuleVariableAt, types.anything, flagAt);
        end(entry, TerminatorKind::Branch, isSame(entry, flag, add(entry, Opcode::Undefined, types.undefined)), { { first, { } }, { otherwise, { } } });
        end(otherwise, TerminatorKind::Branch, isSame(otherwise, flag, boolean(otherwise, false)), { { hasThrown, { } }, { isDone, { } } });
        end(hasThrown, TerminatorKind::Throw, add(hasThrown, Opcode::GetModuleVariableAt, types.anything, word(hasThrown, word(hasThrown, row, numberOfRows), 0)));
        end(isDone, TerminatorKind::Return);
        add(first, Opcode::PutModuleVariableAt, types.undefined, flagAt, boolean(first, true));
        ValueID start = word(first, row, numberOfRows);
        ValueID last = word(first, start, 1);
        end(first, TerminatorKind::Jump, { }, { { head, add(first, Opcode::OffsetIndex, types.int32, start, { }, 2) } });
        ValueID cursor = add(head, Opcode::Parameter, types.int32);
        end(head, TerminatorKind::Branch, isSame(head, cursor, last), { { isDone, { } }, { pair, { } } });
        ValueID place = word(pair, cursor, 0);
        ValueID following = add(pair, Opcode::OffsetIndex, types.int32, cursor, { }, 2);
        ValueID none = add(pair, Opcode::Integer, types.int32);
        end(pair, TerminatorKind::Branch, isSame(pair, place, none), { { isFunction, { } }, { isRow, { } } });
        add(isFunction, Opcode::CallModuleFunction, types.anything, word(isFunction, cursor, 1));
        end(isFunction, TerminatorKind::Invoke, { }, { { next, { } }, { handler, { } } });
        end(isRow, TerminatorKind::Branch, isSame(isRow, add(isRow, Opcode::GetModuleVariableAt, types.anything, place), boolean(isRow, true)), { { next, { } }, { evaluate, { } } });
        word(evaluate, cursor, 1);
        add(evaluate, Opcode::CallDirect, types.anything, { }, { }, program.moduleEvaluator.index());
        end(evaluate, TerminatorKind::Invoke, { }, { { next, { } }, { handler, { } } });
        end(next, TerminatorKind::Jump, { }, { { head, following } });
        ValueID caught = add(handler, Opcode::CatchValue, types.anything);
        ValueID errorAt = word(handler, word(handler, row, numberOfRows), 0);
        end(handler, TerminatorKind::Branch, isSame(handler, errorAt, add(handler, Opcode::Integer, types.int32)), { { forgets, { } }, { keeps, { } } });
        add(forgets, Opcode::PutModuleVariableAt, types.undefined, flagAt, add(forgets, Opcode::Undefined, types.undefined));
        end(forgets, TerminatorKind::Throw, caught);
        add(keeps, Opcode::PutModuleVariableAt, types.undefined, flagAt, boolean(keeps, false));
        add(keeps, Opcode::PutModuleVariableAt, types.undefined, errorAt, caught);
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
    }
    make(3, [&](auto& add, auto& end) {
        ValueID row = add(0, Opcode::Parameter, types.int32);
        ValueID flag = add(0, Opcode::GetModuleVariableAt, types.anything, add(0, Opcode::ModuleEvaluationWord, types.int32, row, { }, 0));
        end(0, TerminatorKind::Branch, add(0, Opcode::StrictEq, types.boolean, flag, add(0, Opcode::Boolean, types.boolean, { }, { }, true)), { { 1, { } }, { 2, { } } });
        end(1, TerminatorKind::Return);
        add(2, Opcode::JumpToModuleEvaluator, types.undefined, row);
        end(2, TerminatorKind::Return);
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.moduleEvaluations.size(), " modules are evaluated by one function");
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
    return result;
}

// By block: the one that immediately dominates it. The entry, and what is not reached: itself.
static Vector<unsigned> immediateDominatorsOf(const Function& function)
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
    bool changed = true;
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

static BitVector functionsWithCallsThatDoMoreThanRead(const Program& program)
{
    unsigned count = program.functions.size();
    Vector<uint8_t> found(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        BitVector doesMore(function.values.size());
        Vector<std::pair<ValueID, ValueID>, 8> aliases;
        for (auto& block : function.blocks) {
            if (block.terminator.value)
                doesMore.set(block.terminator.value.index());
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    doesMore.set(argument.index());
            }
            for (ValueID user : block.values) {
                auto& value = function.at(user);
                bool merelyReads = value.opcode == Opcode::GetField || value.opcode == Opcode::IsUndefinedOrNull
                    || (value.opcode == Opcode::GetById && !value.readsMethodFor())
                    || (value.opcode == Opcode::GetByVal && function.at(value.child(1)).opcode == Opcode::Integer);
                if (merelyReads)
                    continue;
                if (value.opcode == Opcode::Cast && (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull)) {
                    aliases.append({ user, value.child(0) });
                    continue;
                }
                value.forEachChild([&](ValueID child) {
                    doesMore.set(child.index());
                });
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto [cast, operand] : aliases) {
                if (doesMore.get(cast.index()) && !doesMore.set(operand.index()))
                    changed = true;
            }
        }
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && doesMore.get(id.index()))
                    WTF::atomicStore(&found[value.function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        }
    });
    BitVector result(count);
    for (unsigned i = 0; i < count; ++i) {
        if (found[i])
            result.set(i);
    }
    return result;
}

// See Function::worker. Of the program as it was read.
static void splitOffWorkers(Program& program)
{
    unsigned count = program.functions.size();
    BitVector hasCallThatDoesMoreThanRead;
    if (const char* asked = getenv("AOT_X_FRESH_ONLY_WHERE_READ"); asked && *asked == '2')
        hasCallThatDoesMoreThanRead = functionsWithCallsThatDoMoreThanRead(program);
    program.types.thaw();
    Vector<std::optional<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        freshResults[i] = freshResultsOf(program, program.functions[i]);
    });
    for (unsigned i = 0; i < count; ++i) {
        auto fresh = WTF::move(freshResults[i]);
        if (!fresh || hasCallThatDoesMoreThanRead.get(i))
            continue;
        FunctionID workerID(program.functions.size());
        Function worker = program.functions[i];
        for (unsigned k = 0; k < fresh->types.size(); ++k) {
            Type type = program.at(worker.at(fresh->returns[0].second[k]).type);
            for (auto& entry : fresh->returns)
                type = unionOf(type, program.at(worker.at(entry.second[k]).type));
            TypeID joined = program.types.intern(type);
            if (repOf(program.at(joined)) != Rep::Void && program.at(joined).isWithin(program.at(fresh->types[k]), program))
                fresh->types[k] = joined;
        }
        if (!fitInRegisters(program, fresh->types, fresh->nilKind))
            continue;

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
        if (!program.heatLetters.isEmpty()) {
            RELEASE_ASSERT(program.heatLetters.size() == workerID.index());
            program.heatLetters.append(String(program.heatLetters[i]));
        }
        program.functions[i] = WTF::move(wrapper);
        program.functions.append(WTF::move(worker));
    }
    program.types.freeze();
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
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::CallDirect && !function.at(id).isJumpToContinuation())
                    WTF::atomicStore(&isCalled[function.at(id).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        }
    });
    Vector<std::optional<FreshResults>> freshResults(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (isCalled[i])
            freshResults[i] = freshResultsOf(program, program.functions[i]);
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
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && candidates.contains(value.function().index()))
                    callsOf[i].append(id);
            }
        }
    });
    Vector<Function> workers;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        auto& calls = callsOf[i];
        if (calls.isEmpty())
            continue;
        BitVector removed(function.values.size());
        for (ValueID call : calls) {
            auto& candidate = found[candidates.get(function.at(call).function().index())];
            auto looked = readsOfFreshResult(program, function, call, candidate);
            if (!looked)
                continue;
            auto& [aliases, reads, nilTests] = *looked;
            if (candidate.worker == Format::none) {
                uint32_t original = function.at(call).function().index();
                Function worker = program.functions[original];
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

// Where every call of a function has taken its body in, nothing runs the function itself, nor what only it called. And what runs once: see Program::functionsThatRunOnce.
static void dropFunctionsWithoutCalls(Program& program)
{
    unsigned count = program.functions.size();
    BitVector isReachedOtherwise(count);
    Vector<uint32_t> calls;
    calls.fill(0, count);
    // How many values make a closure of it or name it.
    Vector<uint32_t> timesMadeAValue;
    timesMadeAValue.fill(0, count);
    auto forEachFunctionMadeAValue = [&](const Function& function, auto&& functor) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = function.at(id).opcode;
                if (opcode == Opcode::FunctionReference || opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce)
                    functor(function.at(id).function().index());
            }
        }
    };
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
    for (auto [index, entry] : program.genericEntrypoints) {
        auto& function = program.functions[index];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || isMember.get(index) || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || function.resumeFunction || function.startFunction || program.isEntry(FunctionID(index)))
            continue;
        isEnteredOnlyByClosures.set(index);
        isEnteredOnlyByClosures.set(entry.index());
    }
    auto forEachCallee = [&](const Function& function, auto&& functor) {
        auto entryOf = [&](FunctionID made) {
            if (FunctionID entry = program.genericEntrypoints.get(made.index()))
                functor(entry.index());
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::CallDirect) {
                    functor(value.function().index());
                    // The lowering may hand a fresh rest array over in memory, to the general entry: Lowering::isPassedInMemory().
                    if (program.at(value.function()).has(Format::FunctionHasRestParameter))
                        entryOf(value.function());
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
                if (value.predictedClosure) {
                    functor(value.predictedClosure.index());
                    entryOf(value.predictedClosure);
                }
            }
            for (ValueID id : block.parameters) {
                if (FunctionID predicted = function.at(id).predictedClosure) {
                    functor(predicted.index());
                    entryOf(predicted);
                }
            }
        }
    };
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || ((function.isGenericEntrypoint() || program.genericEntrypoints.contains(i)) && !isEnteredOnlyByClosures.get(i)) || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)))
            isReachedOtherwise.set(i);
        // Only its ramp calls the start of an async function.
        if (function.resumeFunction && (function.declaredAs & (functionIsAsync | functionIsGenerator)) != functionIsGenerator) {
            isReachedOtherwise.set(i);
            isReachedOtherwise.set(function.resumeFunction.index());
        }
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions[i];
        forEachCallee(function, [&](unsigned callee) {
            WTF::atomicExchangeAdd(&calls[callee], 1u, std::memory_order_relaxed);
        });
        forEachFunctionMadeAValue(function, [&](unsigned made) {
            WTF::atomicExchangeAdd(&timesMadeAValue[made], 1u, std::memory_order_relaxed);
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
            auto& function = program.functions[worklist[k]];
            forEachCallee(function, [&](unsigned callee) {
                if (WTF::atomicExchangeSub(&calls[callee], 1u, std::memory_order_relaxed) == 1 && !isReachedOtherwise.get(callee))
                    leftWithoutCalls[k].append(callee);
            });
            forEachFunctionMadeAValue(function, [&](unsigned made) {
                WTF::atomicExchangeSub(&timesMadeAValue[made], 1u, std::memory_order_relaxed);
            });
        });
        worklist.shrink(0);
        for (auto& callees : leftWithoutCalls)
            worklist.appendVector(callees);
    }
    if (program.saysWhatTakesNoCopies && program.licencesTheyAreNotGiven) [[unlikely]] {
        Vector<uint32_t> declined;
        declined.fill(0, count);
        std::array<uint64_t, 16> byLicence { };
        uint64_t sites = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (program.functionsWithoutCalls.get(i))
                continue;
            auto& function = program.functions[i];
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
        auto& function = program.functions[i];
        function.blocks.shrink(1);
        function.blocks[0].values.clear();
        function.blocks[0].terminator = Terminator { };
        function.inlineFrames.clear();
        function.allocations.clear();
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
        auto& function = program.functions[i];
        bool isPlain = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow;
        if (!isPlain || isMadeAValue.get(i) || function.isGenericEntrypoint() || function.has(Format::FunctionIsBuiltin) || program.isResumeFunction(function) || program.isEntry(FunctionID(i)) || function.resumeFunction || function.startFunction)
            return false;
        uint32_t fromItsEntry = 0;
        if (auto entry = program.genericEntrypoints.find(i); entry != program.genericEntrypoints.end()) {
            forEachCallee(program.at(entry->value), [&](unsigned callee) {
                fromItsEntry += callee == i;
            });
        }
        return calls[i] - fromItsEntry == 1;
    };
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        if ((function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && program.modules[function.module.index()].initializer == FunctionID(i))) {
            program.functionsThatRunOnce.set(i);
            worklist.append(i);
        }
    }
    while (!worklist.isEmpty()) {
        Vector<Vector<unsigned>> calledOnce(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            auto& function = program.functions[worklist[k]];
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
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode != Opcode::String)
                    continue;
                uint8_t* times = &timesFetched[function.at(id).string().index()];
                for (uint8_t seen = WTF::atomicLoad(times, std::memory_order_relaxed); seen < 2 && !WTF::atomicCompareExchangeWeakRelaxed(times, seen, static_cast<uint8_t>(seen + 1)); seen = WTF::atomicLoad(times, std::memory_order_relaxed)) { }
            }
        }
    });
    Vector<unsigned> runOnce;
    for (size_t i : program.functionsThatRunOnce)
        runOnce.append(static_cast<unsigned>(i));
    Vector<Vector<uint32_t>> madeOnce(runOnce.size());
    forEachIndexInParallel(runOnce.size(), [&](unsigned k) {
        unsigned i = runOnce[k];
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = program.functions[i];
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

static void findRowsOfBirths(Program& program, Vector<String>& malformed)
{
    for (auto& function : program.functions) {
        if (function.blocks.isEmpty())
            continue;
        auto complain = [&](ASCIILiteral what) {
            malformed.append(makeString(what, String::fromUTF8(program.string(function.name))));
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
                    store.setBirthOfFirstStore(1 + object.index());
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
            for (auto& value : function.values) {
                if (value.isFirstStoreToThis())
                    value.setBirthOfFirstStore(1 + program.rowsOfBirths.size());
            }
            program.rowsOfBirths.append(WTF::move(row));
        }
        if (rows.isEmpty())
            continue;
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
            numbers.add(birth, program.rowsOfBirths.size());
            function.values[birth].immediates[0] = program.rowsOfBirths.size();
            program.rowsOfBirths.append(WTF::move(row));
        }
        for (auto& value : function.values) {
            if (value.isStoreToObjectBornWhole() && value.birthOfFirstStore())
                value.setBirthOfFirstStore(1 + numbers.get(value.birthOfFirstStore() - 1));
        }
    }
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
            isDefiningStore = !name.empty() && !isASCIIDigit(name[0]) && (hasNullPrototype || !equalSpans(name, "__proto__"_s.span8()));
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
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
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
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
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
        auto& function = program.functions[i];
        unsigned forgottenHere = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (!value.isAtKnownPlace() || !timesMayBeLazy[value.string().index()])
                    continue;
                value.immediates[1] = value.isStoreToObjectBornWhole() ? value.immediates[1] | Value::goesByName : Format::none;
                ++forgottenHere;
            }
        }
        numberForgotten += forgottenHere;
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberForgotten.load(), " of ", numberOfPlaces.load(), " accesses at a known place go by the name: a closure may lie there unmade");
}

// Each closure of a generator function has a `prototype` of its own. Where there is one closure the run time goes by the function.
static void passClosuresToGenerators(Program& program)
{
    auto newGenerator = program.find("NewGenerator"_s.span8());
    auto newAsyncGenerator = program.find("NewAsyncGenerator"_s.span8());
    BitVector hasManyClosures(program.functions.size());
    BitVector isCalledWithoutClosure(program.functions.size());
    BitVector handsOverWhatItMakes(program.functions.size());
    auto next = program.find("next"_s.span8());
    auto returnName = program.find("return"_s.span8());
    auto throwName = program.find("throw"_s.span8());
    for (auto& function : program.functions) {
        Vector<unsigned> numberOfOtherUses;
        for (auto& value : function.values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                hasManyClosures.set(value.function().index());
            else if (value.opcode == Opcode::CallDirect && program.at(value.function()).declaredAs & functionIsGenerator && numberOfOtherUses.isEmpty())
                numberOfOtherUses.fill(0, function.values.size());
        }
        if (numberOfOtherUses.isEmpty())
            continue;
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
            isCalledWithoutClosure.set(value.function().index());
            if (numberOfOtherUses[k])
                handsOverWhatItMakes.set(value.function().index());
        }
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
        if (!(program.functions[i].declaredAs & functionIsGenerator) || !program.functions[i].resumeFunction || program.functions[i].blocks.isEmpty() || handsOverWhatItMakes.get(i))
            continue;
        if (isCalledWithoutClosure.get(i)) {
            Function copy = program.functions[i];
            copy.flags &= Format::FunctionIsTyped | Format::FunctionHasThis | Format::FunctionHasRestParameter | Format::FunctionIsOfBuiltinModule;
            copy.isRampForClosures = true;
            copy.isUsedAsObject = false;
            program.functions[i].rampForClosures = FunctionID(program.functions.size());
            program.functions.append(WTF::move(copy));
        }
        auto& ramp = program.functions[i].rampForClosures ? program.at(program.functions[i].rampForClosures) : program.functions[i];
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
            if (value.opcode == Opcode::Operation && ((newGenerator && value.immediates[0] == newGenerator->index()) || (newAsyncGenerator && value.immediates[0] == newAsyncGenerator->index())))
                value.arguments.append(closure);
        }
        ramp.takesItsClosure = true;
    }
}

static void markCallsInTailPosition(Program& program)
{
    auto apply = program.find("Apply"_s.span8());
    auto resumeGenerator = program.find("ResumeGenerator"_s.span8());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        if (!program.makesTailCallsAsBuiltin(function))
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
            bool isOperationThatCalls = value.opcode == Opcode::Operation && ((apply && value.immediates[0] == apply->index()) || (resumeGenerator && value.immediates[0] == resumeGenerator->index()));
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
                if (numberOfUses[operand.index()] == 1 && isConcatenation(function.at(operand)) && (!keys.get(id.index()) || hasOnlyStringsAndNumbers(function.at(operand)))) {
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
    auto next = program.find("next"_s.span8());
    auto getIterator = program.find("GetIterator"_s.span8());
    if (!next || !getIterator)
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
                if (operand.opcode != Opcode::Operation || operand.string() != *getIterator)
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

static Vector<String> heatLettersForMap(const Program&);

static const Vector<String>& heatLettersOf(Program& program)
{
    if (program.heatLetters.isEmpty())
        program.heatLetters = heatLettersForMap(program);
    return program.heatLetters;
}

static int rankOfHeatLetter(char letter)
{
    return letter == '-' ? -2 : letter == 'x' ? -1 : letter;
}

static char s_smallestSizeThatMayBeCold;

static bool isTooSmallToBeCold(const String& letters)
{
    return s_smallestSizeThatMayBeCold && letters.length() >= 12 && static_cast<char>(letters[11]) <= s_smallestSizeThatMayBeCold;
}

static bool parsePositionsOfHeat(const char*& text, Vector<unsigned, 4>& positions, bool& keepsHotByText)
{
    s_smallestSizeThatMayBeCold = 0;
    for (;; ++text) {
        if (*text == 'h') {
            keepsHotByText = true;
            ++text;
        } else if (*text == 's' && text[1] && text[1] != ':' && text[1] != '+') {
            s_smallestSizeThatMayBeCold = text[1];
            text += 2;
        } else {
            unsigned position = 0;
            while (*text >= '0' && *text <= '9')
                position = position * 10 + (*text++ - '0');
            if (!position)
                return false;
            positions.append(position);
        }
        if (*text != '+')
            break;
    }
    return *text == ':' && !positions.isEmpty();
}

static std::optional<int> hottestRankOf(const String& letters, const Vector<unsigned, 4>& positions)
{
    int result = -3;
    for (unsigned position : positions) {
        if (position > letters.length())
            return std::nullopt;
        result = std::max(result, rankOfHeatLetter(static_cast<char>(letters[position - 1])));
    }
    return result;
}

static bool arePositionsOfHeat(const Vector<String>& letters, const Vector<unsigned, 4>& positions)
{
    return !letters.isEmpty() && std::ranges::all_of(positions, [&](unsigned position) {
        return position <= letters.first().length() && letters.first()[position - 1] != '|';
    });
}

static std::optional<BitVector> functionsOfColdSet(Program& program, const char* text)
{
    Vector<unsigned, 4> positions;
    bool keepsHotByText = false;
    if (!parsePositionsOfHeat(text, positions, keepsHotByText) || !text[1] || text[2])
        return std::nullopt;
    int coldest = rankOfHeatLetter(text[1]);
    auto& letters = heatLettersOf(program);
    if (!arePositionsOfHeat(letters, positions))
        return std::nullopt;
    BitVector result(letters.size());
    for (unsigned i = 0; i < letters.size(); ++i) {
        auto rank = hottestRankOf(letters[i], positions);
        if (rank && *rank <= coldest && !(keepsHotByText && program.functionsHotByText.get(i)) && !isTooSmallToBeCold(letters[i]))
            result.set(i);
    }
    return result;
}

static void findFunctionsColdByGraph(Program& program)
{
    if (getenv("AOT_X_HEAT_LETTERS") || getenv("AOT_X_HEAT_GRAPH"))
        heatLettersOf(program);
    const char* wanted = getenv("AOT_X_COLD_SET");
    if (!wanted)
        return;
    const char* asGiven = wanted;
    Vector<unsigned, 4> positions;
    bool keepsHotByText = false;
    if (!parsePositionsOfHeat(wanted, positions, keepsHotByText) || !wanted[1] || wanted[2]) {
        dataLogLn("AOT: AOT_X_COLD_SET=", asGiven, " is not understood: no function is cold by the graph");
        return;
    }
    int coldest = rankOfHeatLetter(wanted[1]);
    auto& letters = heatLettersOf(program);
    if (!arePositionsOfHeat(letters, positions)) {
        dataLogLn("AOT: AOT_X_COLD_SET=", asGiven, " names a position that the letters do not have: no function is cold by the graph");
        return;
    }
    BitVector result(letters.size());
    uint64_t functions = 0;
    for (unsigned i = 0; i < letters.size(); ++i) {
        auto rank = hottestRankOf(letters[i], positions);
        if (!rank || *rank > coldest || (keepsHotByText && program.functionsHotByText.get(i)) || isTooSmallToBeCold(letters[i]))
            continue;
        result.set(i);
        ++functions;
    }
    dataLogLn("AOT: cold by the graph (", asGiven, "): ", functions, " of ", letters.size(), " functions");
    program.functionsColdByGraph = WTF::move(result);
}

static void takeHeatFromGraph(Program& program)
{
    const char* wanted = getenv("AOT_X_HOT_BY_GRAPH");
    if (!wanted)
        return;
    const char* asGiven = wanted;
    Vector<unsigned, 4> positions;
    bool keepsHotByText = false;
    if (!parsePositionsOfHeat(wanted, positions, keepsHotByText) || !wanted[1] || !wanted[2] || wanted[3]) {
        dataLogLn("AOT: AOT_X_HOT_BY_GRAPH=", asGiven, " is not understood: the marks stay as the text gives them");
        return;
    }
    int coldest = wanted[1] == '.' ? -4 : rankOfHeatLetter(wanted[1]);
    int hottest = wanted[2] == '.' ? 256 : rankOfHeatLetter(wanted[2]);
    auto& letters = heatLettersOf(program);
    if (!arePositionsOfHeat(letters, positions)) {
        dataLogLn("AOT: AOT_X_HOT_BY_GRAPH=", asGiven, " names a position that the letters do not have: the marks stay as the text gives them");
        return;
    }
    uint64_t cooled = 0;
    uint64_t heated = 0;
    for (unsigned i = 0; i < letters.size(); ++i) {
        auto found = hottestRankOf(letters[i], positions);
        if (!found)
            continue;
        int rank = *found;
        if (rank <= coldest && !(keepsHotByText && program.functionsHotByText.get(i)) && !isTooSmallToBeCold(letters[i])) {
            cooled += program.functionsHotByText.get(i) || program.functionsWarmByText.get(i);
            program.functionsHotByText.clear(i);
            program.functionsWarmByText.clear(i);
            program.functionsCalledNearLoops.clear(i);
            program.functionsHotOnlyInTheirLoops.clear(i);
        } else if (rank >= hottest && !program.functionsRareByText.get(i)) {
            heated += !program.functionsHotByText.get(i);
            program.functionsHotByText.set(i);
            program.functionsWarmByText.set(i);
            program.functionsCalledNearLoops.set(i);
            program.functionsHotOnlyInTheirLoops.clear(i);
        }
    }
    for (auto [function, entry] : program.genericEntrypoints) {
        for (auto* marks : { &program.functionsHotByText, &program.functionsWarmByText, &program.functionsCalledNearLoops, &program.functionsHotOnlyInTheirLoops })
            marks->set(entry.index(), marks->get(function));
    }
    dataLogLn("AOT: heat by the graph (", asGiven, "): ", cooled, " functions lose the mark hot or warm, ", heated, " gain the mark hot, of ", letters.size());
}

static void findFunctionsThatTakeNoCopies(Program& program)
{
    const char* wanted = getenv("BUN_AOT_SWITCH_NO_COPIES_INTO_COLD");
    bool isByDefault = !wanted;
    program.saysWhatTakesNoCopies = wanted || Options::verboseAOTCompilation();
    if (isByDefault)
        wanted = "66:f";
    const char* asGiven = wanted;
    Vector<unsigned, 4> positions;
    bool keepsHotByText = false;
    int coldest = 0;
    bool isUnderstood = true;
    bool isAll = !strncmp(wanted, "all", 3);
    bool isByText = !isAll && *wanted == 'c';
    if (isAll || isByText)
        wanted += isAll ? 3 : 1;
    else if ((isUnderstood = parsePositionsOfHeat(wanted, positions, keepsHotByText) && wanted[1])) {
        coldest = rankOfHeatLetter(wanted[1]);
        wanted += 2;
    }
    constexpr uint16_t areNoDuplicates = 1 << licenceForMap('m') | 1 << licenceForMap('w') | 1 << licenceForMap('o');
    uint16_t licences = ((1 << licencesForMap.length()) - 2) & ~areNoDuplicates;
    if (isUnderstood && *wanted == ',') {
        licences = 0;
        for (++wanted; *wanted; ++wanted) {
            size_t licence = 1;
            while (licence < licencesForMap.length() && licencesForMap[licence] != *wanted)
                ++licence;
            if (licence == licencesForMap.length())
                break;
            licences |= 1 << licence;
        }
        isUnderstood = licences && !(licences & areNoDuplicates);
    }
    if (!isUnderstood || *wanted) {
        dataLogLn("AOT: BUN_AOT_SWITCH_NO_COPIES_INTO_COLD=", asGiven, " is not understood: every caller takes copies as ever");
        return;
    }
    unsigned count = program.functions.size();
    BitVector result(count);
    if (isAll || isByText) {
        for (unsigned i = 0; i < count; ++i) {
            if (isAll || !program.functionsHotByText.get(i))
                result.set(i);
        }
    } else {
        bool dropsLetters = isByDefault && program.heatLetters.isEmpty();
        auto& letters = heatLettersOf(program);
        if (!arePositionsOfHeat(letters, positions)) {
            dataLogLn("AOT: BUN_AOT_SWITCH_NO_COPIES_INTO_COLD=", asGiven, " names a position that the letters do not have: every caller takes copies as ever");
            if (dropsLetters)
                program.heatLetters.clear();
            return;
        }
        for (unsigned i = 0; i < letters.size(); ++i) {
            auto rank = hottestRankOf(letters[i], positions);
            if (rank && *rank <= coldest && !(keepsHotByText && program.functionsHotByText.get(i)) && !isTooSmallToBeCold(letters[i]))
                result.set(i);
        }
        if (dropsLetters)
            program.heatLetters.clear();
    }
    dataLogLnIf(program.saysWhatTakesNoCopies, "AOT: no copies into cold callers (", asGiven, "): ", result.bitCount(), " of ", count, " functions take none");
    program.functionsThatTakeNoCopies = WTF::move(result);
    program.licencesTheyAreNotGiven = licences;
}

Vector<String> optimize(Program& program)
{
    MonotonicTime start = MonotonicTime::now();
    auto didFinish = [&](ASCIILiteral what) {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: ", what, " done at ", (MonotonicTime::now() - start).seconds(), " s");
    };
    prepare(program);
    didFinish("- prepare"_s);
    refuteIteratorsWhoseNextMayChange(program);
    didFinish("- refuteIteratorsWhoseNextMayChange"_s);
    Vector<String> malformed;
    convertAllocationAssertions(program, malformed);
    didFinish("- convertAllocationAssertions"_s);
    findRowsOfBirths(program, malformed);
    if (!malformed.isEmpty())
        return malformed;
    didFinish("preparing"_s);
    verifyVariablesStoredOnce(program);
    didFinish("variables stored once"_s);
    expandDisposableStacks(program);
    didFinish("- expandDisposableStacks"_s);
    splitCoroutines(program);
    didFinish("- splitCoroutines"_s);
    internNamesForGeneratorLoopFusion(program);
    didFinish("splitting coroutines"_s);
    passClosuresToGenerators(program);
    didFinish("- passClosuresToGenerators"_s);
    // It has made the layouts of the frames. From here on no layout gets a field.
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        program.layouts[i].indexFields();
    });
    for (auto& table : program.whetherOnlyFieldsSupply)
        table.answers.clear();
    didFinish("indexing fields"_s);
    createGenericEntrypoints(program);
    didFinish("- createGenericEntrypoints"_s);
    findDeclaredParents(program);
    didFinish("general entries"_s);
    if (!getenv("BUN_AOT_SWITCH_ENGINE_BIND"))
        createBoundFunctions(program);
    program.numberOfDirectCalls.fill(0, program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        if (function.isGenericEntrypoint())
            return;
        for (auto& value : function.values) {
            if (value.opcode == Opcode::CallDirect)
                WTF::atomicExchangeAdd(&program.numberOfDirectCalls[value.function().index()], 1u, std::memory_order_relaxed);
        }
    });
    didFinish("- createBoundFunctions, and the numbers of direct calls"_s);
    assignAllocations(program);
    didFinish("assigning allocations"_s);
    findViewsRetaggedTo(program);
    program.types.freeze();
    didFinish("what looks at the whole program"_s);

    program.mayMeetMasquerader = std::ranges::any_of(program.strings, [](std::span<const uint8_t> string) {
        return equalSpans(string, "makeMasquerader"_s.span8()) || equalSpans(string, "IsHTMLDDA"_s.span8());
    });
    didFinish("- mayMeetMasquerader"_s);
    findFunctionsHotByText(program);
    didFinish("- findFunctionsHotByText"_s);
    findFunctionsRareByText(program);
    didFinish("- findFunctionsRareByText"_s);
    takeHeatFromGraph(program);
    didFinish("- takeHeatFromGraph"_s);
    findFunctionsThatTakeNoCopies(program);
    didFinish("- findFunctionsThatTakeNoCopies"_s);
    markCallsInTailPosition(program);
    didFinish("- markCallsInTailPosition"_s);
    if (!Options::auditAOTTypedAssertions())
        splitOffWorkers(program);
    didFinish("- splitOffWorkers"_s);
    // Each function is worked on as a copy. What it takes from others, to inline them, is as it was read, whoever gets there first.
    prepareForInlining(program);
    didFinish("- prepareForInlining"_s);
    unsigned count = program.functions.size();
    Vector<Function> optimized(count);
    Vector<Vector<String>> findings(count);
    Vector<uint64_t> sizesAndNumbers(count, [&](size_t index) {
        return static_cast<uint64_t>(UINT32_MAX - program.functions[index].values.size()) << 32 | index;
    });
    std::ranges::sort(sizesAndNumbers);
    Vector<unsigned> largestFirst(count, [&](size_t index) {
        return static_cast<unsigned>(sizesAndNumbers[index]);
    });
    didFinish("- the functions by size"_s);
    forEachIndexInParallel(count, [&](unsigned next) {
        unsigned i = largestFirst[next];
        FunctionID id(i);
        Function function = program.functions[i];
        verify(program, id, function, findings[i]);
        if (!findings[i].isEmpty())
            return;
        cleanUp(program, function);
        // Options::auditAOTTypedAssertions(): every test stays where it is, and every call of a function value goes by the general entry,
        // which has the tests for the parameters.
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions(); ++round) {
            unsigned changed = resolvePreludeCalls(program, function);
            changed += resolveCalls(program, function, program.isWarmByText(id));
            changed += inlineCalls(program, id, function);
            changed += fuseGeneratorLoops(program, function);
            changed += simplifyCasts(program, function);
            unsigned removed = removeRedundantCatchBlocks(program, function);
            if (!changed && !removed)
                break;
            if (removed || function.storesWereMovedByCleanUp)
                cleanUp(program, function);
            else
                cleanUpIfAnythingFolds(program, function);
        }
        if (!Options::auditAOTTypedAssertions())
            speculateOnReceivers(program, function);
        resolveBuiltins(program, function);
        if (!Options::auditAOTTypedAssertions() && threadOwnKeysOfForIn(program, function))
            cleanUp(program, function);
        for (unsigned round = 0; round < 8 && !Options::auditAOTTypedAssertions() && sinkObjectAllocations(program, function, AllocationSinkingPolicy::AllowMaterialization); ++round)
            cleanUp(program, function);
        while (!Options::auditAOTTypedAssertions() && eliminateRedundantReads(program, function)) { }
        if (!Options::auditAOTTypedAssertions())
            moveBirthsIntoArms(program, function);
        sinkEmptyArrayLiterals(function);
        if (!Options::auditAOTTypedAssertions())
            sinkFunctionReferences(function);
        verify(program, id, function, findings[i]);
        concludeAllocationNotes(program, function);
        optimized[i] = WTF::move(function);
    });
    didFinish("- the loop over the functions"_s);
    program.sizesForInlining.clear();
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
        program.functions[i] = WTF::move(optimized[i]);
    });
    didFinish("freeing the functions as they were read"_s);
    if (!Options::auditAOTTypedAssertions()) {
        computeFunctionsReturningMapKeys(program);
        didFinish("- computeFunctionsReturningMapKeys"_s);
        forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
            fuseConcatenations(program, FunctionID(i), program.functions[i]);
        });
        didFinish("- fuseConcatenations"_s);
        giveConcatenationsOneBody(program);
        didFinish("- giveConcatenationsOneBody"_s);
    }
    findFunctionsColdByGraph(program);
    didFinish("- findFunctionsColdByGraph"_s);
    if (!Options::auditAOTTypedAssertions()) {
        giveCallsOfBuiltinsOneBody(program);
        giveConstructionsOneBody(program);
        giveCastsOneBody(program);
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: sites outside loops in copies of originals that are hot by the text: concatenations ", s_sitesInCopiesOfHotOriginals[0].load(), ", calls of builtins and + of texts ", s_sitesInCopiesOfHotOriginals[1].load(), ", constructions ", s_sitesInCopiesOfHotOriginals[2].load(), copiesGoByWhereTheyStand() ? ": taken" : ": passed over");
    }
    didFinish("- giveCallsOfBuiltinsOneBody, giveConstructionsOneBody"_s);
    if (!Options::auditAOTTypedAssertions())
        returnResultsInRegisters(program);
    didFinish("- returnResultsInRegisters"_s);
    if (!Options::auditAOTTypedAssertions())
        shareModuleEvaluation(program);
    didFinish("- shareModuleEvaluation"_s);
    dropFunctionsWithoutCalls(program);
    didFinish("- dropFunctionsWithoutCalls"_s);
    forgetPlacesOfLazyClosures(program);
    didFinish("- forgetPlacesOfLazyClosures"_s);
    reportAllocationAssertions(program, result);
    if (!result.isEmpty())
        return result;
    didFinish("functions without calls"_s);
    findEnvironmentsInFrames(program);
    didFinish("environments in frames"_s);
    linkAllocations(program);
    didFinish("allocations"_s);
    computeEffectSummaries(program);
    didFinish("effects"_s);
    computeFunctionsReturningMapKeys(program);
    didFinish("map keys"_s);
    dropModuleEvaluations(program);
    didFinish("- dropModuleEvaluations"_s);
    settleRowsOfBirths(program);
    didFinish("- settleRowsOfBirths"_s);
    numberRowsOfBirths(program);
    didFinish("rows by number"_s);
    return result;
}

static Vector<String> heatLettersForMap(const Program& program)
{
    unsigned count = program.functions.size();
    struct Edge {
        uint32_t to;
        uint8_t loops;
        uint8_t kind;
        bool isCold;
        int8_t weight;
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
    auto layoutsUnder = [&](const auto& views) -> std::optional<Vector<uint32_t>> {
        constexpr size_t mostLayouts = 64;
        Vector<uint32_t> found;
        for (LayoutID view : views) {
            auto& described = program.at(view);
            if (described.admitsForeignObjects)
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
                    edges[hubOf(method.kind == MethodKind::Setter ? keyOfSetter(method.name) : method.kind == MethodKind::Getter ? keyOfGetter(method.name) : keyOfName(method.name), true)].append(Edge { entered, 0, 2, false, 0, 0 });
                if (layout.constructor)
                    edges[layout.constructor.index()].append(Edge { entered, 0, 3, false, 0, 0 });
            }
        }
    }
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].kind != LayoutKind::Instance)
            continue;
        for (LayoutID at = LayoutID(i); at; at = program.at(at).base) {
            for (auto& method : program.at(at).methods) {
                if (method.function)
                    edges[hubOf(keyOfClass(LayoutID(i)), true)].append(Edge { enteredByValue(method.function), 0, 3, false, 0, 0 });
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
                edges[hubOf(keyWithin(key, isNamedByTypes ? i : noLayout), true)].append(Edge { enteredByValue(method.function), 0, 2, false, 0, 0 });
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
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto [key, stored] = placeStoredTo(function, function.at(id));
                if (!key)
                    continue;
                FunctionID made = functionMadeBy(function, stored);
                if (!made)
                    continue;
                edges[hubOf(key, true)].append(Edge { enteredByValue(made), 0, kindOfKey(key), false, 0, 0 });
                if (kindOfKey(key) != 2)
                    continue;
                auto layouts = layoutsStoredThrough(function, function.at(id));
                if (!layouts || layouts->isEmpty()) {
                    edges[hubOf(keyWithin(key, noLayout), true)].append(Edge { enteredByValue(made), 0, 2, false, 0, 0 });
                    continue;
                }
                for (uint32_t layout : *layouts)
                    edges[hubOf(keyWithin(key, layout), true)].append(Edge { enteredByValue(made), 0, 2, false, 0, 0 });
            }
        }
    }

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
    Vector<uint8_t> deepestLoop(FillWith { }, count, static_cast<uint8_t>(0));
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
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        unsigned numberOfBlocks = function.blocks.size();
        if (!numberOfBlocks)
            continue;
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
        auto add = [&](uint32_t to, unsigned b, unsigned bonus, uint8_t kind, unsigned bonusShown = 0) {
            bool cold = isCold[b];
            int weight = std::min(15, weightOf(b) + 3 * static_cast<int>(bonus));
            int shown = std::min(15, weightOf(b) + 3 * static_cast<int>(bonusShown));
            edges[i].append(Edge { to, static_cast<uint8_t>(std::min<unsigned>(4, loops[b] + bonus)), kind, cold, static_cast<int8_t>(weight), static_cast<int8_t>(shown) });
        };
        auto leadByName = [&](uint64_t key, const std::optional<Vector<uint32_t>>& layouts, unsigned b) {
            if (!layouts) {
                if (uint32_t hub = hubOf(key, false); hub != UINT32_MAX)
                    add(hub, b, 0, 2);
                return;
            }
            if (uint32_t hub = hubOf(keyWithin(key, noLayout), false); hub != UINT32_MAX)
                add(hub, b, 0, 2);
            for (uint32_t layout : *layouts) {
                if (uint32_t hub = hubOf(keyWithin(key, layout), false); hub != UINT32_MAX)
                    add(hub, b, 0, 2);
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
                add(other.index(), 0, 0, 0);
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = function.blocks[b];
            sizes[i] += block.values.size() + 1;
            massOf[i] += frequency[b] * static_cast<float>(block.values.size() + 1);
            deepestLoop[i] = std::max(deepestLoop[i], loops[b]);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                uint64_t read = 0;
                switch (value.opcode) {
                case Opcode::CallDirect:
                    add(value.function().index(), b, 0, 0);
                    if (program.moduleEvaluatorIfNeeded && program.moduleEvaluationOfFunction.contains(value.function().index()))
                        add(program.moduleEvaluatorIfNeeded.index(), b, 0, 0);
                    break;
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                case Opcode::FunctionReference:
                    add(enteredByValue(value.function()), b, 0, 1);
                    break;
                case Opcode::GetById:
                case Opcode::CallMethod: {
                    auto layouts = layoutsOfReceiver(function, value.child(0));
                    leadByName(keyOfGetter(value.string()), layouts, b);
                    if (value.opcode == Opcode::CallMethod || program.at(value.type).anyFunction)
                        leadByName(keyOfName(value.string()), layouts, b);
                    break;
                }
                case Opcode::GetField:
                    if (program.at(value.type).anyFunction) {
                        read = keyOfPlace(value.layout(), value.field());
                        if (kindOfKey(read) == 2) {
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
                        add(hub, b, 0, kindOfKey(read));
                }
                bool isCall = value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect || value.opcode == Opcode::Construct
                    || (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).callsUserCode());
                if (!isCall)
                    continue;
                if (value.opcode == Opcode::Call && !isCold[b]) {
                    if (auto index = parameterThatIs(value.child(0)))
                        noteCallOfParameter(i, *index, weightOf(b));
                }
                bool iterates = (value.opcode == Opcode::CallMethod && iteratingNames.get(value.string().index()))
                    || (value.opcode == Opcode::CallBuiltin && isIteratingName(Builtins::singleton().at(value.builtin()).name.span8()));
                bool isKnown = value.opcode == Opcode::CallDirect && !program.at(value.function()).blocks.isEmpty() && program.at(value.function()).blocks[0].parameters.size() == value.arguments.size();
                for (unsigned argument = 0; argument < value.arguments.size(); ++argument) {
                    if (FunctionID handed = functionMadeBy(function, value.arguments[argument])) {
                        add(enteredByValue(handed), b, 1, 1, iterates);
                        if (isKnown)
                            functionsHanded.append(Handed { i, static_cast<uint32_t>(edges[i].size() - 1), value.function().index(), argument, weightOf(b) });
                    } else if (isKnown && !isCold[b]) {
                        if (auto index = parameterThatIs(value.arguments[argument]))
                            parametersPassedOn.append(PassedOn { i, *index, value.function().index(), argument, weightOf(b) });
                    }
                }
            }
        }
    }
    for (auto& evaluation : program.moduleEvaluations) {
        for (FunctionID callee : evaluation.callees)
            edges[evaluation.function.index()].append(Edge { callee.index(), 0, 0, false, 0, 0 });
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
    for (FunctionID evaluator : { program.moduleEvaluator, program.moduleEvaluatorIfNeeded }) {
        if (evaluator)
            roots.append(evaluator.index());
    }
    auto bucket = [](uint64_t number) -> char {
        unsigned bits = 0;
        while (number) {
            ++bits;
            number >>= 1;
        }
        return "0123456789abcdefghijklmnopqrstuvwxyz"[std::min(bits, 35u)];
    };
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
    if (const char* path = getenv("AOT_X_HEAT_GRAPH")) {
        if (FILE* file = fopen(path, "w")) {
            fprintf(file, "N\t%u\t%u\n", count, nodes);
            for (unsigned i = 0; i < count; ++i)
                fprintf(file, "F\t%u\t%u\t%g\t%u\t%c\n", i, sizes[i], static_cast<double>(massOf[i]), static_cast<unsigned>(deepestLoop[i]), coroutineResumedBy[i]);
            for (uint32_t root : roots)
                fprintf(file, "R\t%u\n", root);
            for (uint32_t from = 0; from < nodes; ++from) {
                for (auto& edge : edges[from])
                    fprintf(file, "E\t%u\t%u\t%u\t%u\t%u\t%d\t%d\n", from, edge.to, static_cast<unsigned>(edge.kind), static_cast<unsigned>(edge.isCold), static_cast<unsigned>(edge.loops), static_cast<int>(edge.weight), static_cast<int>(edge.shown));
            }
            fclose(file);
        }
    }
    struct Term {
        uint32_t from;
        uint32_t to;
        double factor;
    };
    Vector<Term> terms;
    for (uint32_t from = 0; from < nodes; ++from) {
        for (auto& edge : edges[from]) {
            if (edge.isCold || edge.kind > 2)
                continue;
            double gain = edge.shown;
            if (gain < 0)
                gain *= 0.5;
            if (edge.kind == 2 && from >= count)
                gain -= shareOfHub[from];
            terms.append(Term { from, edge.to, std::exp2(gain) });
        }
    }
    Vector<double> atRoots(FillWith { }, nodes, 0.0);
    for (uint32_t root : roots)
        atRoots[root] = 1;
    auto twentiethsAt = [&](double scale) {
        Vector<double> flow = atRoots;
        Vector<double> next(FillWith { }, nodes, 0.0);
        for (unsigned round = 0; round < 80; ++round) {
            next.fill(0.0);
            for (auto& term : terms)
                next[term.to] += flow[term.from] * term.factor;
            for (uint32_t node = 0; node < nodes; ++node)
                flow[node] = std::min(atRoots[node] + scale * next[node], 1e250);
        }
        Vector<double> keys(FillWith { }, count, 0.0);
        for (uint32_t i = 0; i < count; ++i) {
            if (sizes[i] < 256) {
                double timesSmaller = 256.0 / std::max(1u, sizes[i]);
                flow[i] *= timesSmaller * timesSmaller;
            }
            double mass = massOf[i] < 1e30f ? massOf[i] : 1e30;
            keys[i] = flow[i] * mass / std::max(1u, sizes[i]);
        }
        Vector<uint32_t> order(count, [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (keys[a] != keys[b])
                return keys[a] < keys[b];
            return a < b;
        });
        Vector<char> result(FillWith { }, count, 'a');
        uint64_t before = 0;
        for (uint32_t function : order) {
            result[function] = static_cast<char>('a' + (sizeOfAll ? std::min<uint64_t>(19, before * 20 / sizeOfAll) : 0));
            before += isReached.get(function) ? sizes[function] : 0;
        }
        return result;
    };
    auto ranks = twentiethsAt(1.0);
    auto ranksByNearness = twentiethsAt(0x1p-4);
    Vector<String> result;
    result.reserveInitialCapacity(count);
    for (unsigned i = 0; i < count; ++i)
        result.append(makeString("|||||||||||"_s, bucket(sizes[i]), "|||||||||||||||||||||||||||||||||||||||||||||||||||||"_s, ranksByNearness[i] >= 's' ? 'z' : ranks[i], ranksByNearness[i]));
    return result;
}

Vector<String> describeForMap(const Program& program)
{
    unsigned count = program.functions.size();
    auto& heatLetters = program.heatLetters;
    bool printsHeatLetters = getenv("AOT_X_HEAT_LETTERS") && !heatLetters.isEmpty();
    StringBuilder lettersOfWhatWasMadeLater;
    for (unsigned position = 0; printsHeatLetters && position < heatLetters.first().length(); ++position)
        lettersOfWhatWasMadeLater.append('~');
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
        if (printsHeatLetters)
            result.last() = makeString(result.last(), '\t', i < heatLetters.size() ? heatLetters[i] : lettersOfWhatWasMadeLater.toString(), ':', program.isColdByGraph(FunctionID(i)) ? 'c' : '-');
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
