/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinFamilyEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

static unsigned prefixLengthOf(const InlineCall& call)
{
    RELEASE_ASSERT(call.value());
    const ValueContext& value = *call.value();
    if (value.opcode() == Opcode::CallBuiltin)
        return *value->prefixLength();
    return call.parameterWord(0);
}

static StringID literalSeparatorOf(const InlineCall& call)
{
    RELEASE_ASSERT(call.value());
    const ValueContext& value = *call.value();
    return value.opcode() == Opcode::CallBuiltin ? value->literalSeparator() : StringID();
}

namespace {

class SplitPrefixEmitter final : public BuiltinFamilyEmitter {
public:
    explicit SplitPrefixEmitter(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    void generate(const InlineCall&);

private:
    struct Separator {
        Word32 codeUnitOrText;
        unsigned length;
    };

    using Results = Vector<Operand, 33>;

    void splitByOperation(const InlineCall&, CellPointer string, unsigned count);
    void splitAtLiteral(const InlineCall&, CellPointer string, StringID literal, unsigned count);
    Separator separatorOf(StringID literal);
    Integer findSeparators(const CharacterView&, const Separator&, Pointer positions, unsigned wanted);
    Integer countSeparators(const CharacterView&, const Separator&);
};

void SplitPrefixEmitter::generate(const InlineCall& call)
{
    CellPointer string = cellOperand(call.receiver());
    unsigned count = prefixLengthOf(call);
    if (StringID literal = literalSeparatorOf(call)) {
        splitAtLiteral(call, string, literal, count);
        return;
    }
    splitByOperation(call, string, count);
}

void SplitPrefixEmitter::splitByOperation(const InlineCall& call, CellPointer string, unsigned count)
{
    Pointer bounds = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(count);
    Results results;
    results.append(callOperation<Integer>(call.context(), RuntimeOperation::StringSplitPrefix, { string, cellOperand(call.argument(0)), word32(count), bounds }));
    for (unsigned i = 0; i < 2 * count; ++i)
        results.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, bounds.value(), i * sizeof(uint32_t)))));
    finishWithResults(call, results.span());
}

auto SplitPrefixEmitter::separatorOf(StringID literal) -> Separator
{
    String text = emitter<StringEmitter>().textOf(literal);
    RELEASE_ASSERT(!text.isEmpty());
    if (text.length() == 1)
        return { word32(text[0]), 1 };
    return { word32(literal.index()), text.length() };
}

Integer SplitPrefixEmitter::findSeparators(const CharacterView& view, const Separator& separator, Pointer positions, unsigned wanted)
{
    auto find = [&](bool is8Bit) {
        if (separator.length == 1)
            return callLeaf<Integer>(is8Bit ? RuntimeOperation::FindSeparators8 : RuntimeOperation::FindSeparators16, { view.characters, view.length, separator.codeUnitOrText, positions, word32(wanted) });
        return callWithoutSite<Integer>(is8Bit ? RuntimeOperation::FindLongSeparators8 : RuntimeOperation::FindLongSeparators16, { view.characters, view.length, separator.codeUnitOrText, positions, word32(wanted) });
    };
    Integer found = selectByBranch<Integer>(view.is8Bit, [&] {
        return find(true);
    }, [&] {
        return find(false);
    });
    ensureStillAliveHere(view.owner);
    return found;
}

Integer SplitPrefixEmitter::countSeparators(const CharacterView& view, const Separator& separator)
{
    auto count = [&](bool is8Bit) {
        if (separator.length == 1)
            return callLeaf<Integer>(is8Bit ? RuntimeOperation::CountSeparators8 : RuntimeOperation::CountSeparators16, { view.characters, view.length, separator.codeUnitOrText });
        return callWithoutSite<Integer>(is8Bit ? RuntimeOperation::CountLongSeparators8 : RuntimeOperation::CountLongSeparators16, { view.characters, view.length, separator.codeUnitOrText });
    };
    Integer counted = selectByBranch<Integer>(view.is8Bit, [&] {
        return count(true);
    }, [&] {
        return count(false);
    });
    ensureStillAliveHere(view.owner);
    return counted;
}

void SplitPrefixEmitter::splitAtLiteral(const InlineCall& call, CellPointer string, StringID literal, unsigned count)
{
    const OperationContext& context = call.context();
    StringEmitter& strings = emitter<StringEmitter>();
    Separator separator = separatorOf(literal);
    Word32 size = word32(separator.length);
    CharacterView view = strings.viewOfString(context, string);

    Results results;
    if (!count) {
        results.append(add(countSeparators(view, separator), integer(1)));
        finishWithResults(call, results.span());
        return;
    }

    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer((count + 1) / 2);
    Word32 found = low32(findSeparators(view, separator, buffer, count));
    Vector<Word32, 16> positions;
    for (unsigned i = 0; i < count; ++i)
        positions.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, buffer.value(), i * sizeof(uint32_t)))));

    Integer numberOfSeparators = selectByBranch<Integer>(below(found, word32(count)), [&] {
        return zeroExtended(found);
    }, [&] {
        CharacterView rest = strings.subview(view, { add(positions[count - 1], size), view.length });
        return add(integer(count), countSeparators(rest, separator));
    });

    results.append(add(numberOfSeparators, integer(1)));
    for (unsigned i = 0; i < count; ++i) {
        CodeUnitRange piece { i ? add(positions[i - 1], size) : word32(0), select(below(word32(i), found), positions[i], view.length) };
        Condition exists = belowOrEqual(word32(i), found);
        results.append(select(exists, piece.start, word32(absentBound)));
        results.append(select(exists, piece.end, word32(absentBound)));
    }
    finishWithResults(call, results.span());
}

} // anonymous namespace

void StringBuiltinEmitter::generateHasAt(const InlineCall& call, LiteralEnd end)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));
    Word32 length = strings().lengthOf(string);
    Integer otherLength = zeroExtended(strings().lengthOf(other));

    Integer at;
    Condition hasRoom;
    switch (end) {
    case LiteralEnd::Start:
        at = zeroExtended(positionArgument(call, 1, length, word32(0)));
        hasRoom = lessThanOrEqual(add(at, otherLength), zeroExtended(length));
        break;
    case LiteralEnd::End:
        at = sub(zeroExtended(positionArgument(call, 1, length, length)), otherLength);
        hasRoom = greaterThanOrEqual(at, integer(0));
        break;
    }

    finish(call, selectByBranch<Condition>(hasRoom, [&] {
        return hasCharactersAt(call, string, other, at);
    }, [&] {
        return boolean(false);
    }));
}

// There is room for other at that place in string.
// Two strings that are no ropes and have characters of 8 bits, the sought one of at most two words: its first and its last bytes are compared, which may overlap.
Condition StringBuiltinEmitter::hasCharactersAt(const InlineCall& call, CellPointer string, CellPointer other, Integer at)
{
    constexpr unsigned maxLengthComparedInLine = 2 * sizeof(uint64_t);
    const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock generic = newBlock();
    Join<Condition> join(m_out);
    auto continueIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), unlikelyEdge(generic));
        m_out.appendTo(next);
    };
    auto load = [&](unsigned width, Pointer pointer) -> LValue {
        TypedPointer address(characters, pointer.value());
        switch (width) {
        case 8:
            return m_out.load64(address);
        case 4:
            return m_out.load32(address);
        case 2:
            return m_out.load16ZeroExt32(address);
        case 1:
            return m_out.load8ZeroExt32(address);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    continueIf(logicalNot(isRopeString(string)));
    continueIf(logicalNot(isRopeString(other)));
    CharacterView subject = strings().viewOfFlatString(string);
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(logicalAnd(subject.is8Bit, sought.is8Bit));
    continueIf(belowOrEqual(sought.length, word32(maxLengthComparedInLine)));
    Pointer here = offsetBy(subject.characters, at, 0);
    Integer size = zeroExtended(sought.length);
    for (unsigned width : { 8u, 4u, 2u }) {
        LBasicBlock compare = newBlock();
        LBasicBlock shorter = newBlock();
        m_out.branch(aboveOrEqual(sought.length, word32(width)).value(), normalEdge(compare), normalEdge(shorter));

        m_out.appendTo(compare);
        Integer lastOffset = sub(size, integer(width));
        LValue firstDifference = m_out.bitXor(load(width, here), load(width, sought.characters));
        LValue lastDifference = m_out.bitXor(load(width, offsetBy(here, lastOffset, 0)), load(width, offsetBy(sought.characters, lastOffset, 0)));
        LValue difference = m_out.bitOr(firstDifference, lastDifference);
        join.add(Condition(width == 8 ? m_out.isZero64(difference) : m_out.isZero32(difference)));

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    LBasicBlock noCharacter = newBlock();
    m_out.branch(isNotZero(sought.length).value(), normalEdge(oneCharacter), normalEdge(noCharacter));

    m_out.appendTo(oneCharacter);
    join.add(Condition(m_out.equal(load(1, here), load(1, sought.characters))));

    m_out.appendTo(noCharacter);
    join.add(boolean(true));

    m_out.appendTo(generic);
    join.add(isNotZero(low32(callOperation<Integer>(call.context(), RuntimeOperation::StringHasAt, { string, other, low32(at) }))));
    return join.finish();
}

void StringBuiltinEmitter::generateSearch(const InlineCall& call, SearchDirection direction, bool resultIsBoolean)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));

    RuntimeOperation operationOnCells = direction == SearchDirection::Forward ? RuntimeOperation::StringIndexOfFrom : RuntimeOperation::StringLastIndexOf;

    Word32 position;
    switch (direction) {
    case SearchDirection::Forward:
        position = positionArgument(call, 1, strings().lengthOf(string), word32(0));
        break;
    case SearchDirection::Backward: {
        Word32 fromEnd = word32(INT32_MAX);
        position = fromEnd;
        if (std::optional<Number> argument = numberArgument(call, 1))
            position = select(equal(*argument, *argument), positionArgument(call, 1, fromEnd, fromEnd), fromEnd);
        break;
    }
    }

    LBasicBlock generic = newBlock();
    Join<Word32> join(m_out);
    auto continueIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), unlikelyEdge(generic));
        m_out.appendTo(next);
    };

    continueIf(logicalNot(isRopeString(string)));
    continueIf(logicalNot(isRopeString(other)));
    CharacterView subject = strings().viewOfFlatString(string);
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(equal(subject.is8Bit, sought.is8Bit));
    join.add(strings().find(subject, sought, position, direction));

    m_out.appendTo(generic);
    join.add(low32(callOperation<Integer>(call.context(), operationOnCells, { string, other, position })));

    Word32 found = join.finish();
    if (resultIsBoolean) {
        finish(call, notEqual(found, word32(-1)));
        return;
    }
    finish(call, found);
}

void StringBuiltinEmitter::generateStringStartsWith(const InlineCall& call)
{
    generateHasAt(call, LiteralEnd::Start);
}

void StringBuiltinEmitter::generateStringEndsWith(const InlineCall& call)
{
    generateHasAt(call, LiteralEnd::End);
}

void StringBuiltinEmitter::generateStringIncludes(const InlineCall& call)
{
    generateSearch(call, SearchDirection::Forward, true);
}

void StringBuiltinEmitter::generateStringIndexOf(const InlineCall& call)
{
    generateSearch(call, SearchDirection::Forward, false);
}

void StringBuiltinEmitter::generateStringLastIndexOf(const InlineCall& call)
{
    generateSearch(call, SearchDirection::Backward, false);
}

void StringBuiltinEmitter::generateStringSplitPrefix(const InlineCall& call)
{
    SplitPrefixEmitter(generator()).generate(call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
