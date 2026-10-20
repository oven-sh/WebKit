/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsRegExp.h"
#include "RegExp.h"
#include <algorithm>
#include <wtf/FixedVector.h>

namespace JSC { namespace AOT { namespace Typed {

uint32_t RegExpBuiltinEmitter::prefixLengthOf(const InlineCall& call) const
{
    RELEASE_ASSERT(call.value());
    const ValueContext& value = *call.value();
    if (value.opcode() == Opcode::CallBuiltin)
        return *value->prefixLength();
    return call.parameterWord(numberOfParametersOf(call.builtin()));
}

template<typename First, typename Second>
RegExpBuiltinEmitter::Results RegExpBuiltinEmitter::eitherResults(Condition condition, SecondPath secondPath, const First& first, const Second& second)
{
    LBasicBlock firstBlock = newBlock();
    LBasicBlock secondBlock = newBlock();
    LBasicBlock continuation = newBlock();
    switch (secondPath) {
    case SecondPath::Unsure:
        m_out.branch(condition.value(), normalEdge(firstBlock), normalEdge(secondBlock));
        break;
    case SecondPath::Rare:
        m_out.branch(condition.value(), normalEdge(firstBlock), rareEdge(secondBlock));
        break;
    }

    auto anchorAndLeave = [&](const Results& results) {
        Vector<ValueFromBlock, 36> anchors;
        for (const Operand& result : results)
            anchors.append(m_out.anchor(result.value));
        m_out.jump(continuation);
        return anchors;
    };

    m_out.appendTo(firstBlock);
    Results firstResults = first();
    auto firstAnchors = anchorAndLeave(firstResults);
    m_out.appendTo(secondBlock);
    Results secondResults = second();
    auto secondAnchors = anchorAndLeave(secondResults);
    RELEASE_ASSERT(firstAnchors.size() == secondAnchors.size());

    m_out.appendTo(continuation);
    Results joined;
    for (unsigned i = 0; i < firstAnchors.size(); ++i) {
        Representation representation = firstResults[i].representation;
        RELEASE_ASSERT(representation == secondResults[i].representation);
        joined.append(Operand(representation, m_out.phi(b3TypeOf(representation), firstAnchors[i], secondAnchors[i])));
    }
    return joined;
}

Integer RegExpBuiltinEmitter::numberOfGroupsOf(CellPointer regExpObject)
{
    return zeroExtended(Word32(m_out.load32(fieldAt(regExpOf(regExpObject).value(), RegExp::offsetOfNumSubpatterns()))));
}

Vector<CodeUnitRange, 16> RegExpBuiltinEmitter::loadMatchedRanges(CellPointer regExpObject, Integer numberOfGroups, unsigned count)
{
    LValue offsets = m_out.loadPtr(fieldAt(regExpOf(regExpObject).value(), RegExp::offsetOfOvector() + FixedVector<int>::offsetOfStorage()));
    auto loadRange = [&](Integer group) {
        LValue pair = m_out.add(offsets, m_out.shl(group.value(), m_out.constInt32(3)));
        Word32 start(m_out.load32(fieldAt(pair, FixedVector<int>::Storage::offsetOfData())));
        Word32 end(m_out.load32(fieldAt(pair, FixedVector<int>::Storage::offsetOfData() + sizeof(int))));
        return CodeUnitRange { start, end };
    };

    Vector<CodeUnitRange, 16> ranges;
    ranges.append(loadRange(integer(0)));
    for (unsigned group = 1; group < count; ++group) {
        Condition exists = lessThanOrEqual(integer(group), numberOfGroups);
        CodeUnitRange range = loadRange(select(exists, integer(group), integer(0)));
        ranges.append(CodeUnitRange { select(exists, range.start, word32(absentBound)), select(exists, range.end, word32(absentBound)) });
    }
    return ranges;
}

RegExpBuiltinEmitter::Results RegExpBuiltinEmitter::execBoundsByOperation(const InlineCall& call, CellPointer regExpObject, CellPointer string, unsigned count)
{
    unsigned numberOfRanges = std::max(count, 1u);
    Pointer bounds = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(numberOfRanges);
    Word32 length = low32(callOperation<Integer>(call.context(), RuntimeOperation::RegExpExecPrefix, { regExpObject, string, word32(numberOfRanges), bounds }));
    auto boundAt = [&](unsigned index) {
        return Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, bounds.value(), index * sizeof(uint32_t))));
    };

    Condition isPresent = notEqual(length, word32(static_cast<int32_t>(noMatchPrefixLength)));
    Results results { isPresent, zeroExtended(select(isPresent, length, word32(0))), select(isPresent, boundAt(0), word32(0)), string };
    for (unsigned i = 0; i < 2 * count; ++i)
        results.append(boundAt(i));
    return results;
}

RegExpBuiltinEmitter::Results RegExpBuiltinEmitter::execBoundsInline(const InlineCall& call, CellPointer regExpObject, CellPointer string, unsigned count)
{
    Condition isFound(m_out.castToInt32(callStub(call.context(), regExpMatch(RegExpMatchResult::Found), { regExpObject.value(), string.value() }).first));
    return eitherResults(isFound, SecondPath::Unsure, [&] {
        Integer numberOfGroups = numberOfGroupsOf(regExpObject);
        Vector<CodeUnitRange, 16> ranges = loadMatchedRanges(regExpObject, numberOfGroups, count);
        Results results { boolean(true), add(numberOfGroups, integer(1)), ranges[0].start, string };
        for (const CodeUnitRange& range : ranges.span().first(count)) {
            results.append(range.start);
            results.append(range.end);
        }
        return results;
    }, [&] {
        Results results { boolean(false), integer(0), word32(0), string };
        for (unsigned i = 0; i < 2 * count; ++i)
            results.append(word32(absentBound));
        return results;
    });
}

RegExpBuiltinEmitter::Results RegExpBuiltinEmitter::matchPrefixByOperation(const InlineCall& call, CellPointer regExpObject, CellPointer string, unsigned count, const CallOptions& options)
{
    Pointer elements = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(std::max(count, 1u));
    Word32 length = low32(callOperation<Integer>(call.context(), RuntimeOperation::StringMatchPrefix, { string, regExpObject, word32(count), elements }, options));
    Condition isPresent = notEqual(length, word32(static_cast<int32_t>(noMatchPrefixLength)));
    Results results { isPresent, zeroExtended(select(isPresent, length, word32(0))) };
    for (unsigned i = 0; i < count; ++i)
        results.append(Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, elements.value(), i * sizeof(EncodedJSValue)))));
    return results;
}

RegExpBuiltinEmitter::Results RegExpBuiltinEmitter::matchPrefixInline(const InlineCall& call, CellPointer regExpObject, CellPointer string, unsigned count)
{
    Condition isFound(m_out.castToInt32(callStub(call.context(), regExpMatch(RegExpMatchResult::Found), { regExpObject.value(), string.value() }).first));
    return eitherResults(isFound, SecondPath::Unsure, [&] {
        StringEmitter& strings = emitter<StringEmitter>();
        Integer numberOfGroups = numberOfGroupsOf(regExpObject);
        Vector<CodeUnitRange, 16> ranges = loadMatchedRanges(regExpObject, numberOfGroups, count);
        Results results { boolean(true), add(numberOfGroups, integer(1)) };
        for (unsigned group = 0; group < count; ++group) {
            if (!group) {
                results.append(boxCell(strings.substring(call.context(), string, ranges[0])));
                continue;
            }
            results.append(either<Boxed>(greaterThanOrEqual(ranges[group].start, word32(0)), [&] {
                return boxCell(strings.substring(call.context(), string, ranges[group]));
            }, [&] {
                return undefined();
            }));
        }
        return results;
    }, [&] {
        Results results { boolean(false), integer(0) };
        for (unsigned i = 0; i < count; ++i)
            results.append(undefined());
        return results;
    });
}

void RegExpBuiltinEmitter::generateRegExpExecPrefix(const InlineCall& call)
{
    unsigned count = prefixLengthOf(call);
    CellPointer regExpObject = cellOperand(call.receiver());
    CellPointer string = cellOperand(call.argument(0));
    bool isCompact = call.context().shape() == CodeShape::Compact;
    Results results = isCompact ? execBoundsByOperation(call, regExpObject, string, count) : execBoundsInline(call, regExpObject, string, count);
    finishWithResults(call, results.span());
}

void RegExpBuiltinEmitter::generateStringMatchPrefix(const InlineCall& call)
{
    unsigned count = prefixLengthOf(call);
    CellPointer string = cellOperand(call.receiver());
    CellPointer regExpObject = cellOperand(call.argument(0));
    if (call.context().shape() == CodeShape::Compact) {
        finishWithResults(call, matchPrefixByOperation(call, regExpObject, string, count).span());
        return;
    }
    Condition isNotGlobal = hasNoBits(flagsOf(regExpObject), static_cast<uint32_t>(Yarr::Flags::Global));
    finishWithResults(call, eitherResults(isNotGlobal, SecondPath::Rare, [&] {
        return matchPrefixInline(call, regExpObject, string, count);
    }, [&] {
        return matchPrefixByOperation(call, regExpObject, string, count, { .isOnRarePath = true });
    }).span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
