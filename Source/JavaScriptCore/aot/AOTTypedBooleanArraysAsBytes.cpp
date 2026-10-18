/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BooleanArray {
    ValueID construction;
    ValueID fill;
    bool isBornTrue { false };
    bool lengthIsKnownToBeValid { false };
    Vector<ValueID, 4> casts;
    Vector<ValueID, 8> reads;
    Vector<ValueID, 8> stores;
    Vector<ValueID, 4> lengths;
};

bool isInline(const Value& value, InlineBuiltin which)
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == which;
}

class BooleanArrayFinder {
public:
    BooleanArrayFinder(const Program& program, const Function& function)
        : m_program(program)
        , m_function(function)
        , m_fill(Builtins::singleton().find(BuiltinKind::Method, Builtins::singleton().arrayClass().name, Builtins::singleton().nameOf(asBytes("fill"_s.span8()))))
    {
    }

    Vector<BooleanArray> run()
    {
        Vector<BooleanArray> found;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& values = m_function.blocks[b].values;
            for (unsigned index = 0; index < values.size(); ++index) {
                if (!isInline(m_function.at(values[index]), InlineBuiltin::ArrayWithLength))
                    continue;
                if (auto array = arrayConstructedAt(b, index))
                    found.append(WTF::move(*array));
            }
        }
        return found;
    }

private:
    struct Access {
        ValueID value;
        unsigned block;
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    static bool keepsItsOperand(const Value& value)
    {
        if (value.opcode != Opcode::Cast)
            return false;
        CastReason reason = value.castReason();
        return reason == CastReason::Upcast || reason == CastReason::Claim || reason == CastReason::NonNull || reason == CastReason::Flow;
    }

    ValueID withoutConversions(ValueID id) const
    {
        for (;;) {
            auto& value = m_function.at(id);
            bool isSameNumber = value.opcode == Opcode::ToNumeric && typeOf(value.child(0)).isNumber();
            if (!isSameNumber && !keepsItsOperand(value))
                return id;
            id = value.child(0);
        }
    }

    std::optional<int64_t> constantOf(ValueID id) const
    {
        auto& type = typeOf(id);
        if (!type.isInteger() || type.integers.minimum != type.integers.maximum)
            return std::nullopt;
        return type.integers.minimum;
    }

    std::optional<BooleanArray> arrayConstructedAt(unsigned block, unsigned index)
    {
        auto& values = m_function.blocks[block].values;
        auto& terminator = m_function.blocks[block].terminator;
        BooleanArray array;
        array.construction = values[index];
        auto& construction = m_function.at(array.construction);
        if (construction.arguments.size() != 1 || !typeOf(construction.arguments[0]).isNumber())
            return std::nullopt;
        m_length = construction.arguments[0];

        BitVector isUnfilled;
        isUnfilled.set(array.construction.index());
        for (unsigned next = index + 1; next < values.size() && !array.fill; ++next) {
            auto& value = m_function.at(values[next]);
            if (keepsItsOperand(value) && isUnfilled.get(value.child(0).index())) {
                isUnfilled.set(values[next].index());
                array.casts.append(values[next]);
                continue;
            }
            if (value.opcode == Opcode::CallBuiltin && value.builtin() == m_fill && value.child(0) && isUnfilled.get(value.child(0).index())) {
                if (value.arguments.size() != 1 || m_function.at(value.arguments[0]).opcode != Opcode::Boolean)
                    return std::nullopt;
                array.fill = values[next];
                array.isBornTrue = m_function.at(value.arguments[0]).immediates[0];
            }
        }
        if (!array.fill || (terminator.kind == TerminatorKind::Invoke && (values.last() == array.fill || values.last() == array.construction)))
            return std::nullopt;

        BitVector isFilled;
        isFilled.set(array.fill.index());
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& other : m_function.blocks) {
                for (ValueID id : other.values) {
                    auto& value = m_function.at(id);
                    if (keepsItsOperand(value) && isFilled.get(value.child(0).index()) && !isFilled.set(id.index())) {
                        array.casts.append(id);
                        changed = true;
                    }
                }
            }
        }

        Vector<Access, 16> accesses;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& other = m_function.blocks[b];
            for (ValueID id : other.values) {
                auto& value = m_function.at(id);
                unsigned numberOfUses = 0;
                bool usesUnfilled = false;
                value.forEachChild([&](ValueID child) {
                    numberOfUses += isFilled.get(child.index());
                    usesUnfilled |= isUnfilled.get(child.index());
                });
                if (usesUnfilled && id != array.fill && !isUnfilled.get(id.index()))
                    return std::nullopt;
                if (!numberOfUses || isFilled.get(id.index()))
                    continue;
                if (numberOfUses != 1 || !value.child(0) || !isFilled.get(value.child(0).index()))
                    return std::nullopt;
                if (value.opcode == Opcode::GetByVal && !value.readsForInKey() && !value.readsForInKeys()) {
                    array.reads.append(id);
                    accesses.append({ id, b });
                } else if (value.opcode == Opcode::PutByVal && typeOf(value.child(2)).isBoolean()) {
                    array.stores.append(id);
                    accesses.append({ id, b });
                } else if (isInline(value, InlineBuiltin::ArrayLength))
                    array.lengths.append(id);
                else
                    return std::nullopt;
            }
            bool escapes = false;
            Terminator terminatorOfOther = other.terminator;
            terminatorOfOther.forEachChild([&](ValueID& child) {
                escapes |= isFilled.get(child.index()) || isUnfilled.get(child.index());
            });
            if (escapes)
                return std::nullopt;
        }

        m_lengths = array.lengths.span();
        for (auto& access : accesses) {
            ValueID key = m_function.at(access.value).child(1);
            Vector<ValueID, 8> assumed;
            bool isIndex = isWholeAndNotNegative(key, assumed, 0) || (typeOf(key).isInteger() && isTestedNotToBeNegative(key, access.block));
            if (!isIndex || !isBelowLength(key, access.block))
                return std::nullopt;
        }
        auto& lengthType = typeOf(m_length);
        array.lengthIsKnownToBeValid = lengthType.isInteger() && lengthType.integers.minimum >= 0 && lengthType.integers.maximum <= UINT32_MAX;
        return array;
    }

    bool isWholeAndNotNegative(ValueID id, Vector<ValueID, 8>& assumed, unsigned depth)
    {
        auto& type = typeOf(id);
        if (type.isInteger())
            return type.integers.minimum >= 0;
        if (!type.isNumber() || depth > 8)
            return false;
        auto& value = m_function.at(id);
        switch (value.opcode) {
        case Opcode::ToNumeric:
            return isWholeAndNotNegative(value.child(0), assumed, depth + 1);
        case Opcode::Cast:
            return keepsItsOperand(value) && isWholeAndNotNegative(value.child(0), assumed, depth + 1);
        case Opcode::Add:
        case Opcode::Mul:
            return isWholeAndNotNegative(value.child(0), assumed, depth + 1) && isWholeAndNotNegative(value.child(1), assumed, depth + 1);
        case Opcode::Parameter: {
            if (assumed.contains(id))
                return true;
            BlockID block = value.parameterBlock();
            if (!block.index())
                return false;
            assumed.append(id);
            for (auto& other : m_function.blocks) {
                for (auto& target : other.terminator.targets) {
                    if (target.block == block && !isWholeAndNotNegative(target.arguments[value.parameterIndex()], assumed, depth + 1))
                        return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    bool isTheLength(ValueID id) const
    {
        id = withoutConversions(id);
        return id == withoutConversions(m_length) || std::ranges::find(m_lengths, id) != m_lengths.end();
    }

    bool isTheLengthLessOne(ValueID id) const
    {
        auto& length = m_function.at(withoutConversions(m_length));
        if (length.opcode != Opcode::Add)
            return false;
        for (unsigned side = 0; side < 2; ++side) {
            if (constantOf(length.child(side)) == 1 && withoutConversions(length.child(1 - side)) == withoutConversions(id))
                return true;
        }
        return false;
    }

    bool isAtMostTheLength(ValueID id, int64_t margin) const
    {
        auto bound = constantOf(id);
        auto length = constantOf(m_length);
        return bound && length && *bound + margin <= *length;
    }

    bool saysKeyIsBelowLength(const Value& test, ValueID key) const
    {
        bool keyIsFirst = test.opcode == Opcode::Less || test.opcode == Opcode::LessEq;
        if (!keyIsFirst && test.opcode != Opcode::Greater && test.opcode != Opcode::GreaterEq)
            return false;
        if (withoutConversions(test.child(keyIsFirst ? 0 : 1)) != withoutConversions(key))
            return false;
        ValueID bound = test.child(keyIsFirst ? 1 : 0);
        if (test.opcode == Opcode::Less || test.opcode == Opcode::Greater)
            return isTheLength(bound) || isTheLengthLessOne(bound) || isAtMostTheLength(bound, 0);
        return isTheLengthLessOne(bound) || isAtMostTheLength(bound, 1);
    }

    bool saysKeyIsNotNegative(const Value& test, ValueID key) const
    {
        bool keyIsFirst = test.opcode == Opcode::Greater || test.opcode == Opcode::GreaterEq;
        if (!keyIsFirst && test.opcode != Opcode::Less && test.opcode != Opcode::LessEq)
            return false;
        if (withoutConversions(test.child(keyIsFirst ? 0 : 1)) != withoutConversions(key))
            return false;
        auto& bound = typeOf(test.child(keyIsFirst ? 1 : 0));
        bool mayBeEqual = test.opcode == Opcode::GreaterEq || test.opcode == Opcode::LessEq;
        return bound.isInteger() && bound.integers.minimum >= (mayBeEqual ? 0 : -1);
    }

    bool holdsByTestPassedOnTheWayTo(unsigned block, const Invocable<bool(const Value&)> auto& says)
    {
        findDominators();
        for (unsigned current = block; current && m_dominators[current] != UINT_MAX; current = m_dominators[current]) {
            auto& terminator = m_function.blocks[m_dominators[current]].terminator;
            if (m_numberOfPredecessors[current] != 1 || terminator.kind != TerminatorKind::Branch || terminator.targets[0].block.index() != current)
                continue;
            if (says(m_function.at(terminator.value)))
                return true;
        }
        return false;
    }

    bool isTestedNotToBeNegative(ValueID key, unsigned block)
    {
        return holdsByTestPassedOnTheWayTo(block, [&](const Value& test) {
            return saysKeyIsNotNegative(test, key);
        });
    }

    bool isBelowLength(ValueID key, unsigned block)
    {
        if (auto length = constantOf(m_length); length && typeOf(key).isInteger() && typeOf(key).integers.maximum < *length)
            return true;
        return holdsByTestPassedOnTheWayTo(block, [&](const Value& test) {
            return saysKeyIsBelowLength(test, key);
        });
    }

    void findDominators()
    {
        if (!m_dominators.isEmpty())
            return;
        m_dominators = immediateDominatorsOf(m_function);
        m_numberOfPredecessors.fill(0, m_function.blocks.size());
        for (auto& other : m_function.blocks) {
            for (auto& target : other.terminator.targets)
                ++m_numberOfPredecessors[target.block.index()];
        }
    }

    const Program& m_program;
    const Function& m_function;
    BuiltinID m_fill;
    ValueID m_length;
    std::span<const ValueID> m_lengths;
    Vector<unsigned> m_dominators;
    Vector<unsigned> m_numberOfPredecessors;
};

struct ByteTypes {
    TypeID bytes;
    TypeID bit;
    TypeID zero;
    TypeID one;
    TypeID length;
    BuiltinID construct;
    uint32_t engineClass { 0 };
};

class BooleanArrayConverter {
public:
    BooleanArrayConverter(const Program& program, Function& function, const ByteTypes& types)
        : m_program(program)
        , m_function(function)
        , m_types(types)
    {
    }

    void convert(const BooleanArray& array)
    {
        Value original = m_function.at(array.construction);
        m_position = original.position;
        m_inlineFrame = original.inlineFrame;
        ValueID length = original.arguments[0];

        if (!array.lengthIsKnownToBeValid) {
            unsigned site = m_function.blocks.findIf([&](auto& block) {
                return block.values.contains(array.construction);
            });
            BlockValues values = std::exchange(m_function.blocks[site].values, { });
            Terminator terminator = std::exchange(m_function.blocks[site].terminator, { });
            size_t index = values.find(array.construction);
            m_function.blocks[site].values.append(values.subspan(0, index));
            Value shifted = make(Opcode::URShift, m_types.length);
            shifted.children[0] = length;
            shifted.children[1] = append(site, integer(0));
            ValueID validLength = append(site, WTF::move(shifted));
            Value test = make(Opcode::StrictEq, m_program.types.boolean);
            test.children[0] = validLength;
            test.children[1] = length;
            ValueID isValid = append(site, WTF::move(test));
            length = validLength;

            unsigned refused = addBlock();
            unsigned accepted = addBlock();
            append(refused, make(Opcode::UnlikelyHint, m_program.types.undefined));
            append(refused, WTF::move(original));
            m_function.blocks[refused].terminator.kind = TerminatorKind::Unreachable;
            m_function.blocks[accepted].values.append(values.subspan(index));
            m_function.blocks[accepted].terminator = WTF::move(terminator);
            branch(site, isValid, accepted, refused);
        }

        Value bytes = make(Opcode::CallBuiltin, m_types.bytes);
        bytes.immediates = { m_types.construct.index(), m_types.engineClass };
        bytes.arguments.append(length);
        m_function.at(array.construction) = WTF::move(bytes);

        BitVector isDropped;
        isDropped.set(array.fill.index());
        for (ValueID cast : array.casts)
            isDropped.set(cast.index());
        BitVector isRead;
        for (ValueID read : array.reads)
            isRead.set(read.index());
        BitVector isStore;
        for (ValueID store : array.stores)
            isStore.set(store.index());
        for (ValueID id : array.lengths) {
            Value same = make(Opcode::ToNumeric, m_function.at(id).type);
            same.position = m_function.at(id).position;
            same.inlineFrame = m_function.at(id).inlineFrame;
            same.children[0] = length;
            m_function.at(id) = WTF::move(same);
        }

        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            BlockValues values = std::exchange(m_function.blocks[b].values, { });
            for (ValueID id : values) {
                if (isDropped.get(id.index()))
                    continue;
                m_position = m_function.at(id).position;
                m_inlineFrame = m_function.at(id).inlineFrame;
                if (isRead.get(id.index())) {
                    Value byte = m_function.at(id);
                    byte.children[0] = array.construction;
                    byte.type = m_types.bit;
                    byte.rep = repOf(m_program.at(m_types.bit));
                    byte.copyPredictionFrom(Value { });
                    Value flag = make(Opcode::ToBoolean, m_program.types.boolean);
                    flag.children[0] = append(b, WTF::move(byte));
                    if (array.isBornTrue) {
                        Value inverted = make(Opcode::LogicalNot, m_program.types.boolean);
                        inverted.children[0] = append(b, WTF::move(flag));
                        flag = WTF::move(inverted);
                    }
                    m_function.at(id) = WTF::move(flag);
                } else if (isStore.get(id.index())) {
                    ValueID stored = m_function.at(id).child(2);
                    ValueID byte;
                    if (m_function.at(stored).opcode == Opcode::Boolean)
                        byte = append(b, integer(!!m_function.at(stored).immediates[0] != array.isBornTrue));
                    else {
                        if (array.isBornTrue) {
                            Value inverted = make(Opcode::LogicalNot, m_program.types.boolean);
                            inverted.children[0] = stored;
                            stored = append(b, WTF::move(inverted));
                        }
                        Value number = make(Opcode::ToNumber, m_types.bit);
                        number.children[0] = stored;
                        byte = append(b, WTF::move(number));
                    }
                    m_function.at(id).children[0] = array.construction;
                    m_function.at(id).children[2] = byte;
                }
                m_function.blocks[b].values.append(id);
            }
        }
    }

private:
    unsigned addBlock()
    {
        m_function.blocks.append(Block { });
        return m_function.blocks.size() - 1;
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    void branch(unsigned from, ValueID condition, unsigned taken, unsigned notTaken)
    {
        auto& terminator = m_function.blocks[from].terminator;
        terminator.kind = TerminatorKind::Branch;
        terminator.value = condition;
        terminator.targets.append(Target { BlockID(taken), { } });
        terminator.targets.append(Target { BlockID(notTaken), { } });
    }

    Value integer(int64_t number) const
    {
        Value value = make(Opcode::Integer, number ? m_types.one : m_types.zero);
        value.setBits(static_cast<uint64_t>(number));
        return value;
    }

    ValueID append(unsigned block, Value&& value)
    {
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(value));
        m_function.blocks[block].values.append(id);
        return id;
    }

    const Program& m_program;
    Function& m_function;
    const ByteTypes& m_types;
    decltype(Value::position) m_position { };
    decltype(Value::inlineFrame) m_inlineFrame { };
};

} // anonymous namespace

void keepLocalBooleanArraysAsBytes(Program& program)
{
    static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
    auto& builtins = Builtins::singleton();
    ByteTypes types;
    for (auto& engineClass : builtins.classes()) {
        if (engineClass.typedArrayType == TypeUint8) {
            types.bytes = program.engineClassTypes[engineClass.index];
            types.engineClass = engineClass.index;
        }
    }
    for (unsigned index = 0; index < builtins.size(); ++index) {
        auto& builtin = builtins.at(BuiltinID(index));
        if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::TypedArrayWithLength)
            types.construct = BuiltinID(index);
    }
    if (!types.bytes || !types.construct)
        return;
    struct Found {
        unsigned function;
        Vector<BooleanArray> arrays;
    };
    auto found = collectInParallel<Found>(program.functions.size(), [&](unsigned i, Vector<Found>& collected) {
        if (!program.functions.mayContain(i, builtinCalls))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty() || function.has(Format::FunctionIsBuiltin) || function.isGenericEntrypoint())
            return;
        bool constructsArray = false;
        std::as_const(program).functions.forEachIndexedValue(i, builtinCalls, [&](ValueID id) {
            constructsArray |= isInline(function.at(id), InlineBuiltin::ArrayWithLength);
        });
        if (!constructsArray)
            return;
        auto arrays = BooleanArrayFinder(program, function).run();
        if (!arrays.isEmpty())
            collected.append(Found { i, WTF::move(arrays) });
    });
    if (found.isEmpty())
        return;
    program.types.thaw();
    types.bit = program.types.intern(Type::integer({ 0, 1 }));
    types.zero = program.types.intern(Type::integer(Range::one(0)));
    types.one = program.types.intern(Type::integer(Range::one(1)));
    types.length = program.types.intern(Type::integer(Range::uint32()));
    program.types.freeze();
    forEachIndexInParallel(found.size(), [&](unsigned k) {
        auto& function = program.functions[found[k].function];
        BooleanArrayConverter converter(program, function, types);
        for (auto& array : found[k].arrays)
            converter.convert(array);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
