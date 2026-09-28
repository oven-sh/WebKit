/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3SwitchValue.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "FTLSwitchCase.h"
#include "JSCInlines.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

void Lowering::lowerTerminalOrFallThrough(BasicBlock* block, Node* node)
{
    if (!node) {
        RELEASE_ASSERT(block->successors.size() == 1);
        m_out.jump(edgeTo(block->successors[0]));
        return;
    }

    auto conditional = [&](LValue condition) {
        RELEASE_ASSERT(block->successors.size() == 2);
        m_out.branch(condition, unsure(edgeTo(block->successors[0])), unsure(edgeTo(block->successors[1])));
    };
    auto isUndefinedOrNull = [&](Node* value) { return this->isUndefinedOrNull(value); };
    auto equalsNull = [&](Node* value) { return this->equalsNull(value, true); };
    lowerTerminal(block, node, conditional, isUndefinedOrNull, equalsNull);
}

LValue Lowering::isUndefinedOrNull(Node* value)
{
    if (isSubtype(value->type, TOther))
        return m_out.booleanTrue;
    if (!mayBe(value->type, TOther))
        return m_out.booleanFalse;
    return isOther(lowJSValue(value));
}

// == null, or typeof == "undefined" (which null is not): also true of objects that masquerade as undefined, which is a matter
// for the structure's flags and realm.
LValue Lowering::equalsNull(Node* value, bool nullCounts)
{
    auto notCellCase = [&](LValue jsValue) {
        return nullCounts ? isOther(jsValue) : m_out.equal(jsValue, m_out.constInt64(JSValue::encode(jsUndefined())));
    };
    {
        if (!mayBe(value->type, TOther | TAnyObject))
            return m_out.booleanFalse;
        if (!mayBe(value->type, TAnyObject))
            return notCellCase(lowJSValue(value));
        LValue jsValue = lowJSValue(value);
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock masquerades = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 3> results;
        results.append(m_out.anchor(notCellCase(jsValue)));
        m_out.branch(isCell(jsValue), unsure(cellCase), unsure(continuation));
        m_out.appendTo(cellCase, masquerades);
        results.append(m_out.anchor(m_out.booleanFalse));
        m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(jsValue, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(MasqueradesAsUndefined)), rarely(masquerades), usually(continuation));
        m_out.appendTo(masquerades, continuation);
        results.append(m_out.anchor(m_out.equal(m_out.loadPtr(structureOf(jsValue), m_heaps.Structure_realm), m_globalObject)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, results);
    }
}

template<typename Conditional, typename IsUndefinedOrNull, typename EqualsNull>
void Lowering::lowerTerminal(BasicBlock* block, Node* node, const Conditional& conditional, const IsUndefinedOrNull& isUndefinedOrNull, const EqualsNull& equalsNull)
{
    switch (node->opcode) {
    case op_jmp:
        m_out.jump(edgeTo(block->successors[0]));
        return;
    case op_ret:
        // There is one way out, however many returns there are: what it takes to leave is not worth having twice.
        if (!m_returnBlock)
            m_returnBlock = m_out.newBlock();
        m_returnValues.append(m_out.anchor(lowJSValue(node->use(node->as<OpRet>().m_value))));
        m_out.jump(m_returnBlock);
        return;
    case op_unreachable:
        m_out.unreachable();
        return;
    case op_throw:
        callPreflight(node);
        m_out.call(pointerType(), entry(Entry::operationAOTThrow), m_globalObject, lowJSValue(node->use(node->as<OpThrow>().m_value)));
        m_out.jump(m_handleExceptions);
        return;
    case op_throw_static_error:
        throwStaticError(node);
        return;
    case op_iterator_close_check:
        conditional(iteratorCloseCheckCondition(node));
        return;
    case op_jtrue:
        conditional(toBoolean(node->use(node->as<OpJtrue>().m_condition)));
        return;
    case op_jfalse:
        conditional(m_out.logicalNot(toBoolean(node->use(node->as<OpJfalse>().m_condition))));
        return;
    case op_jeq_null:
        conditional(equalsNull(node->use(node->as<OpJeqNull>().m_value)));
        return;
    case op_jneq_null:
        conditional(m_out.logicalNot(equalsNull(node->use(node->as<OpJneqNull>().m_value))));
        return;
    case op_jundefined_or_null:
        conditional(isUndefinedOrNull(node->use(node->as<OpJundefinedOrNull>().m_value)));
        return;
    case op_jnundefined_or_null:
        conditional(m_out.logicalNot(isUndefinedOrNull(node->use(node->as<OpJnundefinedOrNull>().m_value))));
        return;
    case op_jeq_ptr: {
        auto bytecode = node->as<OpJeqPtr>();
        conditional(m_out.equal(lowJSValue(node->use(bytecode.m_value)), lowJSValue(node->use(bytecode.m_specialPointer))));
        return;
    }
    case op_jneq_ptr: {
        auto bytecode = node->as<OpJneqPtr>();
        conditional(m_out.notEqual(lowJSValue(node->use(bytecode.m_value)), lowJSValue(node->use(bytecode.m_specialPointer))));
        return;
    }

#define AOT_COMPARE_JUMP(Struct, opcodeName, canonical, negate) \
    case opcodeName: { \
        auto bytecode = node->as<Struct>(); \
        LValue result = lowerCompare(node, canonical, bytecode.m_lhs, bytecode.m_rhs); \
        conditional(negate ? m_out.logicalNot(result) : result); \
        return; \
    }
    AOT_COMPARE_JUMP(OpJless, op_jless, op_less, false)
    AOT_COMPARE_JUMP(OpJlesseq, op_jlesseq, op_lesseq, false)
    AOT_COMPARE_JUMP(OpJgreater, op_jgreater, op_greater, false)
    AOT_COMPARE_JUMP(OpJgreatereq, op_jgreatereq, op_greatereq, false)
    AOT_COMPARE_JUMP(OpJnless, op_jnless, op_less, true)
    AOT_COMPARE_JUMP(OpJnlesseq, op_jnlesseq, op_lesseq, true)
    AOT_COMPARE_JUMP(OpJngreater, op_jngreater, op_greater, true)
    AOT_COMPARE_JUMP(OpJngreatereq, op_jngreatereq, op_greatereq, true)
    AOT_COMPARE_JUMP(OpJbelow, op_jbelow, op_below, false)
    AOT_COMPARE_JUMP(OpJbeloweq, op_jbeloweq, op_beloweq, false)
#undef AOT_COMPARE_JUMP

    case op_jeq: {
        auto bytecode = node->as<OpJeq>();
        conditional(lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs));
        return;
    }
    case op_jneq: {
        auto bytecode = node->as<OpJneq>();
        conditional(m_out.logicalNot(lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs)));
        return;
    }
    case op_jstricteq: {
        auto bytecode = node->as<OpJstricteq>();
        conditional(lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs));
        return;
    }
    case op_jnstricteq: {
        auto bytecode = node->as<OpJnstricteq>();
        conditional(m_out.logicalNot(lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs)));
        return;
    }
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
        lowerSwitch(node);
        return;
    default:
        unsupported(node);
        m_out.unreachable();
        return;
    }
}

void Lowering::lowerSwitch(Node* node)
{
    UnlinkedCodeBlock* codeBlock = m_graph.codeBlock();
    Vector<FTL::SwitchCase> cases;
    UncheckedKeyHashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> seen;
    auto addCase = [&](int32_t value, int32_t offset) {
        if (!offset || !seen.add(static_cast<uint32_t>(value)).isNewEntry)
            return;
        cases.append(FTL::SwitchCase(m_out.constInt32(value), blockFor(node, offset), FTL::Weight()));
    };

    if (node->opcode == op_switch_string) {
        auto bytecode = node->as<OpSwitchString>();
        const auto& table = codeBlock->unlinkedStringSwitchJumpTable(bytecode.m_tableIndex);
        callPreflight(node);
        LValue offset = plainCall(Int32, Entry::operationAOTSwitchString, m_globalObject, lowJSValue(node->use(bytecode.m_scrutinee)), m_out.constInt32(bytecode.m_tableIndex));
        LBasicBlock ok = m_out.newBlock();
        m_out.branch(m_out.equal(offset, m_out.constInt32(INT32_MIN)), rarely(m_handleExceptions), usually(ok));
        m_out.appendTo(ok);
        for (auto& entry : table.m_offsetTable)
            addCase(entry.value.m_branchOffset, entry.value.m_branchOffset);
        m_out.switchInstruction(offset, cases, blockFor(node, table.m_defaultOffset), FTL::Weight());
        return;
    }

    bool isChar = node->opcode == op_switch_char;
    unsigned tableIndex = isChar ? node->as<OpSwitchChar>().m_tableIndex : node->as<OpSwitchImm>().m_tableIndex;
    Node* scrutinee = node->use(isChar ? node->as<OpSwitchChar>().m_scrutinee : node->as<OpSwitchImm>().m_scrutinee);
    const auto& table = codeBlock->unlinkedSwitchJumpTable(tableIndex);
    LBasicBlock defaultBlock = blockFor(node, table.m_defaultOffset);

    LValue value;
    if (isChar) {
        callPreflight(node);
        value = plainCall(Int32, Entry::operationAOTSwitchChar, m_globalObject, lowJSValue(scrutinee));
        LBasicBlock ok = m_out.newBlock();
        m_out.branch(m_out.equal(value, m_out.constInt32(INT32_MIN)), rarely(m_handleExceptions), usually(ok));
        m_out.appendTo(ok);
    } else if (scrutinee->rep() == Rep::Int32)
        value = lowInt32(scrutinee);
    else {
        // An int32, or a double that is one; anything else goes to the default.
        LValue jsValue = lowJSValue(scrutinee);
        LBasicBlock notInt = m_out.newBlock();
        LBasicBlock isDouble = m_out.newBlock();
        LBasicBlock dispatch = m_out.newBlock();
        ValueFromBlock intResult = m_out.anchor(unboxInt32(jsValue));
        m_out.branch(isInt32(jsValue), usually(dispatch), rarely(notInt));
        m_out.appendTo(notInt, isDouble);
        m_out.branch(isNumber(jsValue), unsure(isDouble), unsure(defaultBlock));
        m_out.appendTo(isDouble, dispatch);
        LValue asDouble = unboxDouble(jsValue);
        LValue asInt = m_out.doubleToInt32(asDouble);
        ValueFromBlock doubleResult = m_out.anchor(asInt);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(asInt), asDouble), unsure(dispatch), unsure(defaultBlock));
        m_out.appendTo(dispatch);
        value = m_out.phi(Int32, intResult, doubleResult);
    }

    if (table.isList()) {
        for (unsigned i = 0; i < table.m_branchOffsets.size(); i += 2)
            addCase(table.m_branchOffsets[i], table.m_branchOffsets[i + 1]);
    } else {
        for (unsigned i = 0; i < table.m_branchOffsets.size(); ++i)
            addCase(table.m_min + i, table.m_branchOffsets[i]);
    }
    m_out.switchInstruction(value, cases, defaultBlock, FTL::Weight());
}

void Lowering::lowerCatch(Node* node)
{
    // The prologue of this entrypoint (AOTCompiler.cpp) has put the frame and the callee saves back.
    auto bytecode = node->as<OpCatch>();
    LValue exception = plainCall(pointerType(), Entry::operationAOTCatch, m_vm);
    LBasicBlock caught = m_out.newBlock();
    // Termination is not for catching: keep unwinding.
    m_out.store32(m_out.constInt32(CallSiteIndex(node->bytecodeIndex).bits()), addressFor(VirtualRegister(CallFrameSlot::argumentCountIncludingThis), HighWordOffset));
    m_out.branch(m_out.isNull(exception), rarely(m_handleExceptions), usually(caught));
    m_out.appendTo(caught);
    setProj(node, bytecode.m_exception, exception);
    setProj(node, bytecode.m_thrownValue, m_out.load64(m_out.address(m_heaps.root, exception, Exception::valueOffset())));
}

void Lowering::emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock notSettled)
{
    Type candidates = value->type & typeProvingMask(mask);
    auto passIf = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, unsure(passed), unsure(next));
        m_out.appendTo(next);
    };
    if (mayBe(candidates, TNumber))
        passIf(isNumber(jsValue));
    if (isSubtype(TOther, candidates))
        passIf(isOther(jsValue));
    else if (mayBe(candidates, TUndefined))
        passIf(m_out.equal(jsValue, m_out.constInt64(JSValue::ValueUndefined)));
    else if (mayBe(candidates, TNull))
        passIf(m_out.equal(jsValue, m_out.constInt64(JSValue::ValueNull)));
    if (mayBe(candidates, TBoolean))
        passIf(isBoolean(jsValue));
    if (mayBe(value->type & typeAdmittedByMask(mask), TCell)) {
        if (!isSubtype(value->type, TCell)) {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(jsValue), unsure(cellCase), rarely(notSettled));
            m_out.appendTo(cellCase);
        }
        LValue type = cellType(jsValue);
        auto isType = [&](JSType jsType) { return m_out.equal(type, m_out.constInt32(jsType)); };
        if (isSubtype(TAnyObject & value->type, candidates) && mayBe(candidates, TAnyObject))
            passIf(m_out.aboveOrEqual(type, m_out.constInt32(ObjectType)));
        else {
            if (mayBe(candidates, TObject))
                passIf(isType(FinalObjectType));
            else if ((mask & MaskOtherObject) && !soundTypeMaskNamesTypedArray(mask) && mayBe(value->type, TObject))
                passIf(isType(FinalObjectType)); // Never callable.
            if (isSubtype(TTypedArray, candidates))
                passIf(m_out.below(m_out.sub(type, m_out.constInt32(FirstTypedArrayType)), m_out.constInt32(NumberOfTypedArrayTypesExcludingDataView)));
            else {
                for (unsigned i = 0; i < NumberOfTypedArrayTypesExcludingDataView; ++i) {
                    JSType typedArrayType = static_cast<JSType>(FirstTypedArrayType + i);
                    if (mayBe(candidates, typeOfTypedArray(typedArrayType)))
                        passIf(isType(typedArrayType));
                }
            }
            if (mayBe(candidates, TArray))
                passIf(m_out.bitOr(isType(ArrayType), isType(DerivedArrayType)));
            if (mayBe(candidates, TFunction))
                passIf(m_out.bitOr(isType(JSFunctionType), isType(InternalFunctionType)));
        }
        if (mayBe(candidates, TString))
            passIf(isType(StringType));
        if (mayBe(candidates, TSymbol))
            passIf(isType(SymbolType));
        if (mayBe(candidates, TBigInt))
            passIf(isType(HeapBigIntType));
    }
    m_out.jump(notSettled);
}

bool Lowering::tryLowerMisc(Node* node)
{
    auto typeTest = [&](Node* value, Type type, auto&& test) {
        if (isSubtype(value->type, type))
            setBoolean(node, m_out.booleanTrue);
        else if (!mayBe(value->type, type))
            setBoolean(node, m_out.booleanFalse);
        else
            setBoolean(node, test(lowJSValue(value)));
    };
    auto cellTest = [&](LValue value, auto&& test) -> LValue {
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock notCellResult = m_out.anchor(m_out.booleanFalse);
        m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
        m_out.appendTo(cellCase, continuation);
        ValueFromBlock cellResult = m_out.anchor(test(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, notCellResult, cellResult);
    };

    switch (node->opcode) {
    case op_loop_hint:
        return true;
    case op_check_traps: {
        LBasicBlock slowPath = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.testNonZero32(trapBits(), m_out.constInt32(VMTraps::AsyncEvents)), rarely(slowPath), usually(continuation));
        m_out.appendTo(slowPath, continuation);
        vmCall(node, Void, Entry::operationAOTHandleTraps, m_globalObject);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return true;
    }
    case op_catch:
        lowerCatch(node);
        return true;
    case op_get_scope:
        setJSValue(node, m_out.loadPtr(callee(), m_heaps.JSCallee_scope));
        return true;
    case op_get_parent_scope:
        setJSValue(node, m_out.loadPtr(lowCell(node->use(node->as<OpGetParentScope>().m_scope)), m_heaps.JSScope_next));
        return true;
    case op_argument_count:
        setInt32(node, m_out.sub(m_out.load32(addressFor(VirtualRegister(CallFrameSlot::argumentCountIncludingThis), LowWordOffset)), m_out.int32One));
        return true;
    case op_get_argument: {
        auto bytecode = node->as<OpGetArgument>();
        LValue count = m_out.load32(addressFor(VirtualRegister(CallFrameSlot::argumentCountIncludingThis), LowWordOffset));
        LBasicBlock present = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock absent = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.branch(m_out.above(count, m_out.constInt32(bytecode.m_index)), unsure(present), unsure(continuation));
        m_out.appendTo(present, continuation);
        ValueFromBlock loaded = m_out.anchor(m_out.load64(addressFor(virtualRegisterForArgumentIncludingThis(bytecode.m_index))));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, absent, loaded));
        return true;
    }
    case op_check_tdz: {
        Node* value = node->use(node->as<OpCheckTdz>().m_targetVirtualRegister);
        if (!mayBe(value->type, TEmpty))
            return true;
        LBasicBlock slowPath = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.isZero64(lowJSValue(value)), rarely(slowPath), usually(continuation));
        m_out.appendTo(slowPath, continuation);
        if (usesStubs && node->as<OpCheckTdz>().m_targetVirtualRegister != m_graph.codeBlock()->thisRegister()) {
            // All that differs from one to the next is where it is.
            if (!m_throwTDZBlock)
                m_throwTDZBlock = m_out.newBlock();
            m_throwTDZSites.append(m_out.anchor(m_out.constInt32(CallSiteIndex(node->bytecodeIndex).bits())));
            m_out.jump(m_throwTDZBlock);
        } else
            throwTDZError(node);
        m_out.appendTo(continuation);
        return true;
    }
    case op_check_type: {
        auto bytecode = node->as<OpCheckType>();
        Node* value = node->use(bytecode.m_value);
        unsigned mask = bytecode.m_mask;
        if (value->isKnownToPass(mask)) {
            // Something earlier has seen to it.
            setResult(node, lowRaw(value), value->rep());
            return true;
        }

        // Only what the value can still be is tested for, the cheapest first. Whatever that does not settle (a callable object
        // that is not a function, say) and every failure is for the runtime, which either comes back or throws.
        LValue jsValue = lowJSValue(value);
        LBasicBlock slowPath = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        emitTypeTests(value, jsValue, mask, continuation, slowPath);

        m_out.appendTo(slowPath, continuation);
        vmCall(node, Void, Entry::operationAOTCheckType, m_globalObject, jsValue, m_out.constInt32(mask));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setJSValue(node, jsValue);
        return true;
    }
    case op_not: {
        setBoolean(node, m_out.logicalNot(toBoolean(node->use(node->as<OpNot>().m_operand))));
        return true;
    }
    case op_eq_null:
        setBoolean(node, equalsNull(node->use(node->as<OpEqNull>().m_operand), true));
        return true;
    case op_neq_null:
        setBoolean(node, m_out.logicalNot(equalsNull(node->use(node->as<OpNeqNull>().m_operand), true)));
        return true;
    case op_typeof_is_undefined:
        setBoolean(node, equalsNull(node->use(node->as<OpTypeofIsUndefined>().m_operand), false));
        return true;
    case op_is_empty:
        typeTest(node->use(node->as<OpIsEmpty>().m_operand), TEmpty, [&](LValue v) { return m_out.isZero64(v); });
        return true;
    case op_is_undefined_or_null:
        typeTest(node->use(node->as<OpIsUndefinedOrNull>().m_operand), TOther, [&](LValue v) { return isOther(v); });
        return true;
    case op_is_boolean:
        typeTest(node->use(node->as<OpIsBoolean>().m_operand), TBoolean, [&](LValue v) { return isBoolean(v); });
        return true;
    case op_is_number:
        typeTest(node->use(node->as<OpIsNumber>().m_operand), TNumber, [&](LValue v) { return isNumber(v); });
        return true;
    case op_is_object:
        typeTest(node->use(node->as<OpIsObject>().m_operand), TAnyObject, [&](LValue v) {
            return cellTest(v, [&](LValue cell) { return isObjectCell(cell); });
        });
        return true;
    case op_is_big_int:
        typeTest(node->use(node->as<OpIsBigInt>().m_operand), TBigInt, [&](LValue v) {
            return cellTest(v, [&](LValue cell) { return isCellOfType(cell, HeapBigIntType); });
        });
        return true;
    case op_is_cell_with_type: {
        auto bytecode = node->as<OpIsCellWithType>();
        setBoolean(node, cellTest(lowJSValue(node->use(bytecode.m_operand)), [&](LValue cell) { return isCellOfType(cell, bytecode.m_type); }));
        return true;
    }
    case op_get_internal_field: {
        auto bytecode = node->as<OpGetInternalField>();
        setJSValue(node, m_out.load64(lowCell(node->use(bytecode.m_base)), m_heaps.JSInternalFieldObjectImpl_internalFields[bytecode.m_index]));
        return true;
    }
    case op_put_internal_field: {
        auto bytecode = node->as<OpPutInternalField>();
        LValue base = lowCell(node->use(bytecode.m_base));
        m_out.store64(lowJSValue(node->use(bytecode.m_value)), base, m_heaps.JSInternalFieldObjectImpl_internalFields[bytecode.m_index]);
        if (mayBe(node->use(bytecode.m_value)->type, TCell))
            storeBarrier(base);
        return true;
    }
    case op_profile_control_flow:
    case op_profile_type:
    case op_debug:
    case op_log_shadow_chicken_prologue:
    case op_log_shadow_chicken_tail:
        // Only in code compiled for the debugger or the profilers, which is not what gets compiled ahead of time.
        unsupported(node);
        return false;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
