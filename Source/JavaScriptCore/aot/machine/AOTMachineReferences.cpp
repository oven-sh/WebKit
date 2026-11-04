/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include "AOTStubRegisters.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

enum class CollectorRuleForm : uint8_t {
    // The base of its address is its last child.
    Load,
    Sum,
    // The bits of its first child, widened, cut or shifted.
    BitsOfFirstChild,
    Other,
};

static constexpr CollectorRuleForm collectorRuleFormOf(Opcode opcode)
{
    switch (opcode) {
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return CollectorRuleForm::Load;
    case Add:
    case Sub:
        return CollectorRuleForm::Sum;
    case Opaque:
    case Shl:
    case SExt32:
    case ZExt32:
    case Trunc:
        return CollectorRuleForm::BitsOfFirstChild;
    case Nop:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        return CollectorRuleForm::Other;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// The collector scans the registers and the frame. A word there keeps a cell alive only if it is the address of that cell.
// A pointer at what a cell owns, a narrow reference and an offset into the cage keep nothing alive, so whatever does is read once more where they are last read.
static void keepAliveForCollector(BackwardScan& scan, Operation* value, Position until)
{
    // What an address has absorbed has no range of its own. Its leaves stand in for it.
    if (scan.m_liveness.isFused(value)) {
        forEachLeaf(scan.m_liveness, value, [&](const Operation*, unsigned, Operation* leaf) {
            keepAliveForCollector(scan, leaf, until);
        });
        return;
    }
    if (value->hasNoPosition() || value->has(OperationFlag::NamesPermanentCell))
        return;
    scan.noteValueKeptForCollector(value, until);
    scan.set(value, ValueFlag::IsKeptForCollector);
    Position lastUse = scan.facts(value).lastUse;
    if (lastUse == noPosition || lastUse < until)
        scan.addUse(value, until);
}

// With a constant added to the cage base the sum points into the cell, which keeps nothing alive.
static bool decodesNarrowReferenceForCollector(const Operation* sum)
{
    return sum->opcode() == Add && (sum->child(0)->isArgumentReg(cellCageBaseGPR) || sum->child(1)->isArgumentReg(cellCageBaseGPR));
}

// A decode is an extension, a shift and a sum with the cage base, and a constant may be added ahead of each of them.
static constexpr unsigned maxStepsFromWordToNarrowReference = 5;

// A decode that is shared with other code, or whose end has sunk into a rare region, leaves its extension or its shift alive across a safepoint that the reference does not reach.
// The reference then lives as long as they do, and gets its pointer when the scan comes to it.
static void keepNarrowReferencesBehindWord(BackwardScan& scan, Operation* word, Position until, unsigned stepsLeft)
{
    if (word->isNarrowReference()) {
        keepAliveForCollector(scan, word, until);
        return;
    }
    if (!stepsLeft || !word->type().isInt())
        return;

    switch (collectorRuleFormOf(word->opcode())) {
    case CollectorRuleForm::BitsOfFirstChild:
        keepNarrowReferencesBehindWord(scan, word->child(0), until, stepsLeft - 1);
        return;
    case CollectorRuleForm::Sum:
        if (decodesNarrowReferenceForCollector(word))
            return;
        for (Operation* child : word->children())
            keepNarrowReferencesBehindWord(scan, child, until, stepsLeft - 1);
        return;
    case CollectorRuleForm::Load:
    case CollectorRuleForm::Other:
        return;
    }
}

static NEVER_INLINE void applyCollectorRulesToMarkedValue(BackwardScan& scan, Operation* operation)
{
    const ValueFacts& own = scan.facts(operation);
    Position lastUse = own.lastUse;
    if (lastUse == noPosition || operation->hasNoPosition())
        return;

    if (operation->has(OperationFlag::IsInnerPointer)) {
        switch (collectorRuleFormOf(operation->opcode())) {
        case CollectorRuleForm::Load:
            keepAliveForCollector(scan, operation->lastChild(), lastUse);
            break;
        case CollectorRuleForm::Sum:
            keepAliveForCollector(scan, operation->child(0), lastUse);
            break;
        case CollectorRuleForm::BitsOfFirstChild:
        case CollectorRuleForm::Other:
            scan.m_proc.fail(FailureKind::InnerPointerWithoutOwner, operation);
            break;
        }
    }

    if (!scan.m_proc.traits().usesNarrowReferences || !own.has(ValueFlag::LivesAcrossSafepoint))
        return;
    if (!operation->isNarrowReference()) {
        keepNarrowReferencesBehindWord(scan, operation, lastUse, maxStepsFromWordToNarrowReference);
        return;
    }
    if (!operation->has(OperationFlag::NamesPermanentCell))
        scan.set(operation, ValueFlag::NeedsPointerBesideIt);
}

// By the opcode: whether keepNarrowReferencesBehindWord() looks below a word that is no narrow reference itself.
static constexpr std::array<bool, numberOfOpcodes> collectorRulesLookBelowOpcode = [] {
    std::array<bool, numberOfOpcodes> looksBelow { };
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        CollectorRuleForm form = collectorRuleFormOf(static_cast<Opcode>(index));
        looksBelow[index] = form == CollectorRuleForm::BitsOfFirstChild || form == CollectorRuleForm::Sum;
    }
    return looksBelow;
}();

void BackwardScan::applyCollectorRules(Operation* operation)
{
    // The rules speak of an inner pointer, and of what lives across a safepoint and is a narrow reference or may be made of one. They do nothing for any other value.
    // Whoever calls has the operation at hand and has often asked about the safepoint already, so the operation is asked first: it answers for most values.
    if (!operation->has(OperationFlag::IsInnerPointer)) [[likely]] {
        if (!operation->isNarrowReference() && !collectorRulesLookBelowOpcode.data()[operation->opcode()])
            return;
        if (!facts(operation).has(ValueFlag::LivesAcrossSafepoint))
            return;
    }
    applyCollectorRulesToMarkedValue(*this, operation);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
