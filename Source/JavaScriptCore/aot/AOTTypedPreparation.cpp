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
    static constexpr unsigned minFieldsToIndex = 32;
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
    for (auto& function : functions) {
        for (auto& value : function.values) {
            StringID name;
            if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod)
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
            if (!name)
                continue;
            // `#name`, and a name with half a surrogate pair, are no UTF-8: such an access has no cache, and the run time makes out the name.
            if (!seen.set(name.index()) && !String::fromUTF8(string(name)).isNull())
                result.append(name);
        }
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
        for (uint32_t i = 0; i < m_program.strings.size(); ++i)
            m_program.m_stringIDs.add(keyOf(m_program.strings[i]), i);
        m_program.emptyString = m_program.intern({ });
        expandInitialize();
        linkForInKeys();
        fillLongLiterals();

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
        for (auto& function : m_program.functions) {
            for (auto& block : function.blocks) {
                for (size_t at = 0; at < block.values.size(); ++at) {
                    Value operation = function.at(block.values[at]);
                    if (operation.opcode != Opcode::Operation || operation.string() != *initialize)
                        continue;
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
        }
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
        for (auto& function : m_program.functions) {
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
    void linkForInKeys()
    {
        auto forInKeys = m_program.find("ForInKeys"_s.span8());
        auto hasEnumerableProperty = m_program.find("HasEnumerableProperty"_s.span8());
        if (!forInKeys || !hasEnumerableProperty)
            return;
        for (auto& function : m_program.functions) {
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
        }
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
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                    needClosureWith(value.arguments.size());
                if (value.opcode != Opcode::NewObjectWithShape)
                    continue;
                auto& allocation = function.allocations[value.allocation()];
                program.at(allocation.layout).isAllocated = true;
                value.immediates[1] = numberOf(allocation);
            }
        }
        function.allocations.clear();
    }
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

// The blocks that a block with a LoopHint reaches and that reach it.
static BitVector blocksInLoops(const Function& function)
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

// Where a program spends its time, as far as its text says: in a function that has a loop, that a loop calls by name, or that such a function calls by name or makes, two levels
// down; and in what any of these makes. Of the program as it was read.
static void findFunctionsHotByText(Program& program)
{
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
        if (isInLoop.isEmpty())
            return;
        calledInLoops[i] = Vector<unsigned>();
        forEachFunctionNamed(program.functions[i], &isInLoop, false, [&](unsigned callee) {
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
    for (unsigned level = 0; level < 2; ++level) {
        Vector<unsigned> next;
        for (unsigned i : frontier) {
            forEachFunctionNamed(program.functions[i], nullptr, true, [&](unsigned callee) {
                if (!isCalled.set(callee))
                    next.append(callee);
            });
        }
        frontier = WTF::move(next);
    }
    BitVector hasLoop = result;
    result.merge(isCalled);
    BitVector made(count);
    for (unsigned i : result) {
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference)
                made.set(value.function().index());
        }
    }
    result.merge(made);
    hasLoop.exclude(isCalled);
    hasLoop.exclude(made);
    program.functionsHotOnlyInTheirLoops = WTF::move(hasLoop);
    // A general entry does what its function does.
    for (auto [function, entry] : program.genericEntrypoints) {
        if (result.get(function))
            result.set(entry.index());
    }
    program.functionsHotByText = WTF::move(result);
}

// A concatenation makes a string: in place that is a few hundred bytes at every site. Where the text does not say hot there is one body for each signature, and the sites call it.
static void giveConcatenationsOneBody(Program& program)
{
    constexpr unsigned maxOperands = 6;
    unsigned count = program.functions.size();
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> bodyOfSignature;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        if ((program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i))) || function.blocks.isEmpty())
            continue;
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
                if (value.inlineFrame && program.isHotByText(function.inlineFrames[value.inlineFrame].callee))
                    continue;
                StringBuilder signature;
                signature.append(value.type.index());
                for (ValueID argument : value.arguments)
                    signature.append(' ', function.at(argument).type.index());
                uint32_t body = bodyOfSignature.ensure(signature.toString(), [&] {
                    Function made;
                    made.name = program.emptyString;
                    made.module = function.module;
                    made.position = function.position;
                    made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
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
    }
    program.functions.appendVector(WTF::move(bodies));
}

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
    auto isCandidate = [&](const Value& value) {
        // `s + t` makes a string as a concatenation does.
        if (value.opcode == Opcode::Add)
            return program.at(value.type).isString();
        if (value.opcode != Opcode::CallBuiltin || value.child(1) || value.child(2) || value.arguments.size() + !!value.child(0) > maxOperands)
            return false;
        // Its site is small: the usual way in line, all else in a stub.
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayPush)
            return false;
        BuiltinKind kind = builtins.at(value.builtin()).kind;
        return kind == BuiltinKind::Method || kind == BuiltinKind::StaticFunction || std::ranges::find(spreads, value.builtin()) != spreads.end();
    };
    auto staysInBody = [](const Value& operand) {
        return operand.opcode == Opcode::Integer || operand.opcode == Opcode::Double || operand.opcode == Opcode::Boolean || operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null;
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
    auto forEachSite = [&](auto&& functor) {
        for (unsigned i = 0; i < count; ++i) {
            auto& function = program.functions[i];
            if ((program.isHotByText(FunctionID(i)) && !program.isHotOnlyInItsLoops(FunctionID(i))) || function.blocks.isEmpty())
                continue;
            std::optional<BitVector> isInLoop;
            // A call may throw, if only for want of stack. Where a handler may watch, what may throw ends its block.
            bool hasHandlers = function.blocks.containsIf([](auto& block) {
                return block.terminator.kind == TerminatorKind::Invoke;
            });
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                for (ValueID id : function.blocks[b].values) {
                    auto& value = function.at(id);
                    if (!isCandidate(value))
                        continue;
                    if (hasHandlers && (function.blocks[b].terminator.kind != TerminatorKind::Invoke || id != function.blocks[b].values.last()))
                        continue;
                    if (!isInLoop)
                        isInLoop = blocksInLoops(function);
                    if (isInLoop->get(b))
                        break;
                    if (value.inlineFrame && program.isHotByText(function.inlineFrames[value.inlineFrame].callee))
                        continue;
                    StringBuilder signature;
                    signature.append(static_cast<unsigned>(value.opcode), ' ', value.immediates[0], ' ', value.immediates[1], ' ', value.type.index(), ' ', value.prediction.index(), value.children[0] ? ' ' : '-');
                    forEachOperand(value, [&](ValueID operandID) {
                        auto& operand = function.at(operandID);
                        signature.append(' ', operand.type.index(), '~', operand.prediction.index(), '~', operand.predictedClosure.index());
                        if (staysInBody(operand))
                            signature.append('=', static_cast<unsigned>(operand.opcode), ':', operand.immediates[0], ':', operand.immediates[1]);
                    });
                    functor(function, value, signature.toString());
                }
            }
        }
    };
    UncheckedKeyHashMap<String, uint32_t> sitesOfSignature;
    forEachSite([&](Function&, Value&, const String& signature) {
        ++sitesOfSignature.add(signature, 0).iterator->value;
    });
    Vector<Function> bodies;
    UncheckedKeyHashMap<String, uint32_t> bodyOfSignature;
    forEachSite([&](Function& function, Value& value, const String& signature) {
        if (sitesOfSignature.get(signature) < 2)
            return;
        uint32_t body = bodyOfSignature.ensure(signature, [&] {
            Function made;
            made.name = program.emptyString;
            made.module = function.module;
            made.position = function.position;
            made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
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
            bodies.append(WTF::move(made));
            return count + static_cast<uint32_t>(bodies.size()) - 1;
        }).iterator->value;
        Arguments passed;
        forEachOperand(value, [&](ValueID operandID) {
            if (!staysInBody(function.at(operandID)))
                passed.append(operandID);
        });
        value.opcode = Opcode::CallDirect;
        value.immediates = { body, Format::none };
        value.children = { };
        value.arguments = WTF::move(passed);
    });
    program.functions.appendVector(WTF::move(bodies));
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

// See Function::worker. Of the program as it was read.
static void splitOffWorkers(Program& program)
{
    unsigned count = program.functions.size();
    program.types.thaw();
    for (unsigned i = 0; i < count; ++i) {
        auto fresh = freshResultsOf(program, program.functions[i]);
        if (!fresh)
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
        if (program.functionsHotOnlyInTheirLoops.get(i))
            program.functionsHotOnlyInTheirLoops.set(workerID.index());
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
    BitVector isCalled(count);
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::CallDirect && !function.at(id).isJumpToContinuation())
                    isCalled.set(function.at(id).function().index());
            }
        }
    }
    for (unsigned i : isCalled) {
        if (auto fresh = freshResultsOf(program, program.functions[i]); fresh && !fresh->nilKind) {
            candidates.add(i, found.size());
            found.append(Candidate { WTF::move(*fresh) });
        }
    }
    if (candidates.isEmpty())
        return;

    Vector<Function> workers;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        Vector<ValueID, 4> calls;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && candidates.contains(value.function().index()))
                    calls.append(id);
            }
        }
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
        if (function.resumeFunction) {
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
        unsigned i = worklist.takeLast();
        auto& function = program.functions[i];
        program.functionsWithoutCalls.set(i);
        forEachCallee(function, [&](unsigned callee) {
            if (!--calls[callee] && !isReachedOtherwise.get(callee))
                worklist.append(callee);
        });
        forEachFunctionMadeAValue(function, [&](unsigned made) {
            --timesMadeAValue[made];
        });
        function.blocks.shrink(1);
        function.blocks[0].values.clear();
        function.blocks[0].terminator = Terminator { };
        function.inlineFrames.clear();
        function.allocations.clear();
    }

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
        if (function.kind == FunctionKind::Module || (function.module && program.modules[function.module.index()].initializer == FunctionID(i))) {
            program.functionsThatRunOnce.set(i);
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
                if (hasOneCall(callee) && !program.functionsThatRunOnce.set(callee))
                    worklist.append(callee);
            }
        }
    }
}

// Loads of a variable that is said to be stored once are taken not to change. Of the program as it was written: where a coroutine is split, what is
// behind a join may come to stand in two of its parts.
static void verifyVariablesStoredOnce(const Program& program)
{
    Vector<unsigned> numberOfStores;
    numberOfStores.fill(0, program.variables.size());
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::PutModuleVariable)
                    ++numberOfStores[function.at(id).variable().index()];
            }
        }
    }
    for (unsigned i = 0; i < program.variables.size(); ++i)
        RELEASE_ASSERT(!program.variables[i].isStoredOnce || numberOfStores[i] <= 1);
}

// Each closure of a generator function has a `prototype` of its own. Where there is one closure the run time goes by the function.
static void passClosuresToGenerators(Program& program)
{
    auto newGenerator = program.find("NewGenerator"_s.span8());
    auto newAsyncGenerator = program.find("NewAsyncGenerator"_s.span8());
    BitVector hasManyClosures(program.functions.size());
    BitVector isCalledWithoutClosure(program.functions.size());
    for (auto& function : program.functions) {
        for (auto& value : function.values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                hasManyClosures.set(value.function().index());
            else if (value.opcode == Opcode::CallDirect)
                isCalledWithoutClosure.set(value.function().index());
        }
    }
    for (auto& layout : program.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isCalledWithoutClosure.set(method.function.index());
        }
    }
    for (unsigned i : hasManyClosures) {
        auto& ramp = program.functions[i];
        if (!(ramp.declaredAs & functionIsGenerator) || !ramp.resumeFunction || ramp.blocks.isEmpty() || isCalledWithoutClosure.get(i))
            continue;
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

Vector<String> optimize(Program& program)
{
    MonotonicTime start = MonotonicTime::now();
    auto didFinish = [&](ASCIILiteral what) {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: ", what, " done at ", (MonotonicTime::now() - start).seconds(), " s");
    };
    prepare(program);
    Vector<String> malformed;
    convertAllocationAssertions(program, malformed);
    if (!malformed.isEmpty())
        return malformed;
    didFinish("preparing"_s);
    verifyVariablesStoredOnce(program);
    didFinish("variables stored once"_s);
    splitCoroutines(program);
    didFinish("splitting coroutines"_s);
    passClosuresToGenerators(program);
    // It has made the layouts of the frames. From here on no layout gets a field.
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        program.layouts[i].indexFields();
    });
    for (auto& table : program.whetherOnlyFieldsSupply)
        table.answers.clear();
    didFinish("indexing fields"_s);
    createGenericEntrypoints(program);
    findDeclaredParents(program);
    didFinish("general entries"_s);
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
    assignAllocations(program);
    didFinish("assigning allocations"_s);
    findViewsRetaggedTo(program);
    program.types.freeze();
    didFinish("what looks at the whole program"_s);

    findFunctionsHotByText(program);
    if (!Options::auditAOTTypedAssertions())
        splitOffWorkers(program);
    // Each function is worked on as a copy. What it takes from others, to inline them, is as it was read, whoever gets there first.
    unsigned count = program.functions.size();
    Vector<Function> optimized(count);
    Vector<Vector<String>> findings(count);
    forEachIndexInParallel(count, [&](unsigned i) {
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
            changed += resolveCalls(program, function, program.isHotByText(id));
            changed += inlineCalls(program, id, function);
            changed += simplifyCasts(program, function);
            changed += removeRedundantCatchBlocks(program, function);
            if (!changed)
                break;
            cleanUp(program, function);
        }
        if (!Options::auditAOTTypedAssertions())
            speculateOnReceivers(program, function);
        resolveBuiltins(program, function);
        while (!Options::auditAOTTypedAssertions() && eliminateRedundantReads(program, function)) { }
        if (!Options::auditAOTTypedAssertions())
            moveBirthsIntoArms(program, function);
        verify(program, id, function, findings[i]);
        concludeAllocationNotes(program, function);
        optimized[i] = WTF::move(function);
    });
    didFinish("what looks at one function"_s);

    Vector<String> result;
    for (auto& ofFunction : findings)
        result.appendVector(WTF::move(ofFunction));
    if (!result.isEmpty())
        return result;
    program.functions = WTF::move(optimized);
    didFinish("freeing the functions as they were read"_s);
    if (!Options::auditAOTTypedAssertions())
        giveConcatenationsOneBody(program);
    if (!Options::auditAOTTypedAssertions())
        giveCallsOfBuiltinsOneBody(program);
    if (!Options::auditAOTTypedAssertions())
        returnResultsInRegisters(program);
    dropFunctionsWithoutCalls(program);
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
    return result;
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
        if (FunctionID resume = program.functions[i].resumeFunction)
            classes[resume.index()] = classes[i];
        if (FunctionID start = program.functions[i].startFunction)
            classes[start.index()] = classes[i];
    }
    auto text = [&](StringID id) {
        return id ? String::fromUTF8(program.string(id)) : String();
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
        result.append(makeString(className, className.isEmpty() ? ""_s : "."_s, text(function.name), '\t', entered[i] ? "generic-"_s : ""_s, entrypointOf(described), '\t', i, '\t', path, '\t', function.position, '\t', program.isHotByText(FunctionID(i)) ? 'h' : 'c'));
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
