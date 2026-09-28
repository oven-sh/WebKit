/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTGraph.h"
#include "AOTRuntime.h"
#include "B3AbstractHeapRepository.h"
#include "B3Procedure.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"
#include "ScopeOffset.h"

namespace JSC { namespace AOT {

using FTL::LBasicBlock;
using FTL::LType;
using FTL::LValue;
using FTL::TypedPointer;
using FTL::ValueFromBlock;
using FTL::rarely;
using FTL::unsure;
using FTL::usually;

// AOT IR -> B3. Implemented in AOTLowerCore.cpp (structure, values, calls into C++), AOTLowerArith.cpp, AOTLowerAccess.cpp
// (properties and scopes), AOTLowerObjects.cpp (allocation, conversions, the rarer kinds of property access),
// AOTLowerIteration.cpp (for-of and for-in), AOTLowerCalls.cpp and AOTLowerVarargs.cpp (every call that is not a plain one).
class Lowering {
    WTF_MAKE_NONCOPYABLE(Lowering);
public:
    Lowering(Graph&, B3::Procedure&);

    bool run();

    // Index of the B3 entrypoint of each catch block, in the order of Graph::catchEntrypoints; the first is 1.

    // Of a block that is there for what seldom happens, next to one that is not (see estimateFrequencies()).
    static constexpr double coldFrequency = 1.0 / 1024;

private:
    friend struct LoweringAccess;

    // AOTLowerCore.cpp
    void lowerBlock(BasicBlock*);
    void lowerNode(Node*);
    void lowerBytecode(Node*);
    void lowerTerminalOrFallThrough(BasicBlock*, Node* terminal);
    template<typename Conditional, typename IsUndefinedOrNull, typename EqualsNull>
    void lowerTerminal(BasicBlock*, Node*, const Conditional&, const IsUndefinedOrNull&, const EqualsNull&);
    LValue isUndefinedOrNull(Node*);
    LValue equalsNull(Node*, bool nullCounts);
    void emitUpsilons(BasicBlock*, BasicBlock* successor);
    LBasicBlock edgeTo(BasicBlock* successor);
    void lowerCatch(Node*);
    void lowerSwitch(Node*);
    void lowerMisc(Node*);
    bool tryLowerMisc(Node*);

    LBasicBlock blockFor(Node* branch, int relativeOffset);
    void unsupported(Node*);

    // Values.
    LValue lowRaw(Node*);
    LValue lowJSValue(Node*);
    LValue lowInt32(Node*);
    LValue lowInt64(Node*); // Of a node that is an integer of either size.
    LValue lowDouble(Node*);
    LValue lowBoolean(Node*);
    LValue lowCell(Node* node) { return lowJSValue(node); }
    LValue environmentAt(uint32_t distance) { return m_out.sub(m_instance, m_out.constIntPtr(distance)); } // ImageEnvironment::distance
    LValue callee() { return m_out.load64(m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot)); } // The object the function was called as.
    LValue lowConstantRegister(VirtualRegister); // For an operand that BytecodeUseDef does not count among the uses.
    LValue convert(LValue, Rep from, Type fromType, Rep to);
    void setJSValue(Node*, LValue);
    void setInt32(Node*, LValue);
    void setInt64(Node*, LValue);
    void setDouble(Node*, LValue);
    void setBoolean(Node*, LValue);
    void setResult(Node*, LValue, Rep);
    // For an instruction that defines several registers: the value of one of them.
    void setProj(Node*, VirtualRegister, LValue, Rep = Rep::JSValue);

    LValue isInt32(LValue v) { return m_out.aboveOrEqual(v, m_numberTag); }
    LValue isNotInt32(LValue v) { return m_out.below(v, m_numberTag); }
    LValue isNumber(LValue v) { return m_out.testNonZero64(v, m_numberTag); }
    LValue isNotNumber(LValue v) { return m_out.testIsZero64(v, m_numberTag); }
    LValue isCell(LValue v) { return m_out.testIsZero64(v, m_notCellMask); }
    LValue isNotCell(LValue v) { return m_out.testNonZero64(v, m_notCellMask); }
    LValue isBoolean(LValue v) { return m_out.testIsZero64(m_out.bitXor(v, m_out.constInt64(JSValue::ValueFalse)), m_out.constInt64(~1)); }
    LValue isOther(LValue v) { return m_out.equal(m_out.bitAnd(v, m_out.constInt64(~JSValue::UndefinedTag)), m_out.constInt64(JSValue::ValueNull)); }
    LValue unboxInt32(LValue v) { return m_out.castToInt32(v); }
    LValue boxInt32(LValue v) { return m_out.add(m_out.zeroExt(v, B3::Int64), m_numberTag); }
    LValue unboxDouble(LValue v) { return m_out.bitCast(m_out.add(v, m_numberTag), B3::Double); }
    LValue boxDouble(LValue v) { return m_out.sub(m_out.bitCast(v, B3::Int64), m_numberTag); }
    LValue unboxBoolean(LValue v) { return m_out.notZero64(m_out.bitAnd(v, m_out.constInt64(1))); }
    LValue boxBoolean(LValue v) { return m_out.select(v, m_out.constInt64(JSValue::ValueTrue), m_out.constInt64(JSValue::ValueFalse)); }
    LValue numberToDouble(LValue jsNumber); // A boxed value known to be a number.
    LValue doubleToInt32(LValue); // ToInt32.
    LValue toBoolean(Node*);
    LValue cellType(LValue cell) { return m_out.load8ZeroExt32(cell, m_heaps.JSCell_typeInfoType); }
    LValue isCellOfType(LValue cell, JSType type) { return m_out.equal(cellType(cell), m_out.constInt32(type)); }
    LValue isObjectCell(LValue cell) { return m_out.aboveOrEqual(cellType(cell), m_out.constInt32(ObjectType)); }

    TypedPointer addressFor(VirtualRegister, ptrdiff_t offset = 0);
    TypedPointer slotWord(unsigned slot, unsigned word); // Word 0: structureID and offset. Word 1: pointer.
    LValue slotAddress(unsigned slot);
    unsigned allocateSlot() { return m_graph.numICSlots++; }
    unsigned allocateSlots(unsigned count)
    {
        unsigned first = m_graph.numICSlots;
        m_graph.numICSlots += count;
        return first;
    }
    // Words in the frame that operations read lists of values from and write additional results to. Nothing is kept there.
    TypedPointer scratchWord(unsigned index);
    LValue scratchAddress() { return m_scratch; }
    LValue storeToScratch(Node*, VirtualRegister first, unsigned count); // The registers first, first - 1, ... in that order.
    LValue structureOf(LValue cell);
    LValue isSentinelCell(LValue cell) { return isCellOfType(cell, SentinelType); }
    template<typename Functor> LValue isCellAnd(Node*, LValue jsValue, const Functor&); // False for what is not a cell.

    // Calls into C++.
    LValue entry(Entry);
    void callPreflight(Node*);
    void checkException(LValue exception = nullptr);
    LType operationTuple(LType);
    // For an operation declared with JSC_DECLARE_JIT_OPERATION: hands back the result, having checked for an exception.
    template<typename... Args> LValue vmCall(Node*, LType, Entry, Args...);
    // For one declared NOEXCEPT.
    template<typename... Args> LValue plainCall(LType, Entry, Args...);
    void storeBarrier(LValue owner);

    // Calls to stubs (AOTStubs.h).
    struct StubArgument {
        LValue value;
        Reg reg;
    };
    struct StubImmediate {
        GPRReg reg;
        uint32_t value;
    };
    enum class StubClobbers : uint8_t { WhatCDoes, WhatJSDoes, Temporaries };
    B3::PatchpointValue* callStub(Stub, LType, const Vector<StubArgument, 8>&, const Vector<StubImmediate, 2>&, StubClobbers = StubClobbers::WhatCDoes);
    LValue callOperationThroughStub(Node*, LType, Entry, const Vector<LValue, 8>& arguments); // No node: it does not throw.
    // Code that is run over and over is worth its size. The rest, which is nearly all of it, is not: it calls a stub for what
    // it would otherwise do itself.
    LValue callBinaryStub(Node*, Stub, LType, LValue, LValue);
    bool isCompact() const { return usesStubs && ((!m_block->isInLoop && !m_graph.callsItself) || m_block->isGeneric); }
    // An op_resolve_scope that is only there for the op_get_from_scope that follows it: the two are one call.
    bool isFusedWithGetFromScope(Node*);
    // A slot that a stub can be told about.
    unsigned allocateSite(Node*, unsigned identifier, unsigned extra = 0);
    // The one site of an instruction that is in both copies of a loop: what the generic copy finds out, the fast one goes by.
    unsigned sharedSite(Node*, unsigned identifier, unsigned extra = 0);
    // The two slots of a call of a known function: see lowerCallToKnownFunction().
    unsigned siteOfKnownCall(Node*, unsigned knownCallee, bool isConstruct);

    // AOTLowerArith.cpp
    bool tryLowerArith(Node*);
    void lowerBinaryArith(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerBitOp(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerUnaryArith(Node*, VirtualRegister operand);
    LValue lowerCompare(Node*, OpcodeID canonical, VirtualRegister lhs, VirtualRegister rhs); // A boolean.
    LValue lowerEquality(Node*, bool strict, VirtualRegister lhs, VirtualRegister rhs);
    LValue toInt32ForBitOp(Node* operand);

    // AOTLowerAccess.cpp
    bool tryLowerAccess(Node*);
    void lowerGetById(Node*);
    void lowerPutById(Node*);
    void lowerGetByVal(Node*);
    void lowerPutByVal(Node*);
    void lowerResolveScope(Node*);
    void lowerGetFromScope(Node*);
    void lowerPutToScope(Node*);
    LValue loadProperty(LValue object, LValue offset);
    using StaticVariable = Graph::StaticVariable;
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType type) { return m_graph.resolveStatically(identifierIndex, localScopeDepth, type); }

    TypedPointer cachedPropertyAddress(LValue object, LValue firstSlotWord, const B3::AbstractHeap* = nullptr);
    LValue getByIdCached(Node*, LValue base, Type baseType, Entry operation, unsigned identifier);
    LValue lowIndex(Node* property, LBasicBlock haveIndex, LBasicBlock notIndex);

    // AOTLowerGuards.cpp
    void lowerGuard(BasicBlock*, Node*);
    void emitGuard(Node*);
    unsigned slotOfPropertyGuard(Node*);
    LValue loadSlotWord(unsigned slot, unsigned word);
    void checkStructure(Node* baseNode, LValue base, LValue firstSlotWord);
    void checkCallee(Node*);
    void lowerGuarded(Node*);
    LBasicBlock newColdBlock();
    LValue trapBits();
    void exitUnless(LValue condition);
    void exitUnlessOfType(LValue jsValue, Type from, Type wanted);
    void guardReentry(BasicBlock*);
    void guardGetById(Node*);
    void guardPutById(Node*);
    void guardGetByVal(Node*);
    void guardPutByVal(Node*);
    // Where the element is, having seen to it that there is one.
    TypedPointer elementOfTypedArray(Node* guard, Node* base, Node* property, JSType);
    void guardGetLength(Node*);
    void guardCheckType(Node*);
    // False: there is nothing to it, or nothing to be done about it.
    bool guardResolveScope(Node*);
    bool guardGetFromScope(Node*);
    bool guardCall(Node*);
    // Goes to passed if the value is of a type that is certain to get past the check, and to the other block if not.
    void emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock notSettled);

    // AOTLowerObjects.cpp
    bool tryLowerObjects(Node*);
    bool tryLowerAllocation(Node*);
    bool tryLowerConversion(Node*);
    bool tryLowerPropertyVariant(Node*);
    // An object of the structure that the cache at `slot` (fillAllocationCache()) is for, with those values for its first properties.
    // Goes to `otherwise` if there is nothing in the cache, or no room where it says to allocate.
    LValue allocateObjectWithProperties(unsigned slot, const Vector<LValue, 8>& values, LBasicBlock otherwise);
    void lowerGetLength(Node*);
    void lowerToThis(Node*);
    void throwTDZError(Node*);
    void throwStaticError(Node*);

    // AOTLowerIteration.cpp
    bool tryLowerIteration(Node*);
    void lowerIteratorOpen(Node*, bool isAsync);
    void lowerIteratorNext(Node*);
    void lowerAsyncIteratorNext(Node*);
    void lowerIteratorCloseCheck(Node*);
    LValue iteratorCloseCheckCondition(Node*);
    void checkIsObjectOrThrowIteratorResultIsNotObject(Node*, LValue);
    LValue inlineWatchpointSetIsStillValid(LValue set);

    // AOTLowerVarargs.cpp
    using Arguments = Vector<LValue, 8>; // Including this.
    bool tryLowerCallVariant(Node*);
    Arguments lowerArguments(Node*, unsigned argc, unsigned argv);
    LValue emitCall(Node*, LValue callee, const Arguments&, bool isConstruct = false);
    void emitTailCall(Node*, LValue callee, const Arguments&); // Ends the block.
    LValue canTailCall(Node*, LValue callee, Type calleeType);
    void lowerTailCall(Node*);
    void lowerCallVarargs(Node*, VirtualRegister callee, VirtualRegister thisValue, VirtualRegister arguments, int firstVarArg, bool isConstruct, bool isTail);
    LValue emitCallVarargs(Node*, LValue callee, LValue thisValue, LValue arguments, LValue length, int firstVarArg, bool isConstruct, bool isTail);
    void lowerCallDirectEval(Node*);

    // AOTLowerCalls.cpp
    bool tryLowerCall(Node*);
    void lowerCall(Node*, VirtualRegister callee, unsigned argc, unsigned argv, bool isConstruct, bool hasResult);
    bool lowerCallToKnownFunction(Node*, LValue callee, unsigned argc, unsigned argv, bool isConstruct, bool hasResult);

    Graph& m_graph;
    B3::Procedure& m_proc;
    B3::AbstractHeapRepository m_heaps;
    FTL::Output m_out;

    LValue m_callFrame { nullptr };
    LValue m_instance { nullptr };
    LValue m_data { nullptr };
    LValue m_calleeSlot { nullptr };
    LValue m_vm { nullptr };
    LValue m_globalObject { nullptr };
    LValue m_table { nullptr };
    LValue m_numberTag { nullptr };
    LValue m_notCellMask { nullptr };
    LValue m_scratch { nullptr };
    LBasicBlock m_handleExceptions { nullptr };
    LBasicBlock m_returnBlock { nullptr };
    Vector<ValueFromBlock, 4> m_returnValues;
    LBasicBlock m_tailCallBlock { nullptr };
    Vector<ValueFromBlock, 4> m_tailCallCallees;
    Vector<ValueFromBlock, 4> m_tailCallTargets;
    LBasicBlock m_throwTDZBlock { nullptr };
    Vector<ValueFromBlock, 4> m_throwTDZSites;
    LBasicBlock m_exit { nullptr }; // While a guard is lowered: the way to the generic copy.
    Vector<std::pair<BasicBlock*, LBasicBlock>, 2> m_edges; // While a branch is lowered: the ways to successors that have phis.
    LBasicBlock m_afterSlotChecks { nullptr };
    LValue m_slotEpoch { nullptr };
    unsigned m_slotOfSlotChecks { 0 };
    UncheckedKeyHashMap<uint64_t, unsigned, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sharedSites; // By bytecode offset and identifier.
    BasicBlock* m_block { nullptr };
    unsigned m_nodeIndex { 0 }; // Of the node being lowered, in m_block.
    LType m_tuples[8] { };
};

template<typename... Args>
LValue Lowering::vmCall(Node* node, LType type, Entry function, Args... args)
{
    if constexpr (usesStubs)
        return callOperationThroughStub(node, type, function, { args... });
    callPreflight(node);
    if (type == B3::Double) {
        // Comes back in a floating point register, so there is no room for the exception next to it.
        LValue result = m_out.call(type, entry(function), args...);
        checkException();
        return result;
    }
    LValue result = m_out.call(operationTuple(type), entry(function), args...);
    if (type == B3::Void) {
        checkException(result);
        return nullptr;
    }
    checkException(m_out.extract(result, 1));
    return m_out.extract(result, 0);
}

template<typename Functor>
LValue Lowering::isCellAnd(Node* node, LValue value, const Functor& test)
{
    if (!mayBe(node->type, TCell))
        return m_out.booleanFalse;
    if (isSubtype(node->type, TCell))
        return test(value);
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock notCellResult = m_out.anchor(m_out.booleanFalse);
    m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
    m_out.appendTo(cellCase, continuation);
    ValueFromBlock cellResult = m_out.anchor(test(value));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(B3::Int32, notCellResult, cellResult);
}

template<typename... Args>
LValue Lowering::plainCall(LType type, Entry function, Args... args)
{
    if constexpr (usesStubs)
        return callOperationThroughStub(nullptr, type, function, { args... });
    return m_out.call(type, entry(function), args...);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
