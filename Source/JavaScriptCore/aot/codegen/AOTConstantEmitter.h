/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTFunctionData.h"
#include "AOTOperationEmitter.h"
#include "AOTStubsConstants.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <array>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class ConstantEmitter final : public OperationEmitter {
public:
    explicit ConstantEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateUndefined(const ValueContext& value) { define(value, constantOf(jsUndefined(), value.representation())); }
    void generateNull(const ValueContext& value) { define(value, constantOf(jsNull(), value.representation())); }
    void generateAbsent(const ValueContext& value) { define(value, constantOf(JSValue(), value.representation())); }
    void generateBoolean(const ValueContext& value) { define(value, constantOf(jsBoolean(value->boolean()), value.representation())); }
    void generateInteger(const ValueContext&);
    void generateDouble(const ValueContext& value) { define(value, constantOf(jsNumber(withoutPayload(value->number())), value.representation())); }
    void generateString(const ValueContext&);
    void generateBigInt(const ValueContext& value) { defineCell(value, bigIntConstant(value, value->digits())); }
    void generateBuiltin(const ValueContext&);
    void generateModuleBuiltin(const ValueContext&);
    void generateData(const ValueContext&);
    LValue wordKeptOnHeap(const ValueContext&, ValueID);
    void generateNewClosure(const ValueContext&);
    bool closureLentLiesInFrame(ValueID closure);
    void generateKeyConstant(const ValueContext&);
    void generateUnspecified(const ValueContext& value) { define(value, zeroOf(value.representation())); }
    void generateEmptyArgumentList(const ValueContext& value) { defineCell(value, CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.emptyArgumentList)).value())); }

    CellPointer stringConstant(const OperationContext&, StringID);
    bool loadsStringConstantByStub(const OperationContext&, StringID);
    bool tryStoreStringConstantByStub(const OperationContext&, ValueID string, CellPointer object, uint64_t offset, const TypedPointer& word);
    CellPointer keyConstant(const OperationContext&, PropertyKeyID);
    CellPointer bigIntConstant(const OperationContext&, StringID digits);
    CellPointer builtinCell(const OperationContext& context, BuiltinID id) { return builtinCell(context, BuiltinCellKey::object(id)); }
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    CellPointer singletonClosure(const OperationContext&, FunctionID);
    struct DeferredFunction {
        FunctionID function;
        Vector<LayoutID, 2> layouts;
    };
    CellPointer makeClosureOfField(const OperationContext&, std::span<const DeferredFunction>, CellPointer object, uint32_t offsetOfSlot, bool capturesObject, std::optional<uint32_t> offsetOfEnvironment, NOESCAPE const ScopedLambda<CellPointer()>& makeInRunTime);
    CellPointer keyConstant(PropertyKeyID);
    CellPointer builtinCell(BuiltinID);
    void branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID, BranchTargets);
    Operand constantOf(JSValue, Representation);
    Operand zeroOf(Representation);
    CellPointer permanentCell(TypedPointer cellWord, bool isNarrow, NOESCAPE const Invocable<CellPointer()> auto& create);
    CellPointer numberedPermanentCell(const Machine::PureFunction&, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord, NOESCAPE const Invocable<CellPointer(TypedPointer)> auto& create);
    CellPointer cellNumberedBehindJoin(const Machine::PureFunction&, NOESCAPE const Invocable<CellPointer()> auto& make);

    struct ConstantMadeAtSite {
        ConstantAtSite constant;
        uint32_t number { 0 };
        bool runsCodeOfHost { false };
    };
    CellPointer makeConstantAtSite(const OperationContext*, const ConstantMadeAtSite&, const Machine::AbstractHeap& heapOfWord);

private:
    struct NewClosureRequest {
        FunctionID function;
        FunctionKindData kind;
        MentionedAllocation allocation;
        AllocationStorage storage;
        ClosureForm form;
        std::span<const ValueID> captures;
        unsigned numberOfWords { 0 };
    };

    void defineCell(const ValueContext&, CellPointer);
    void failAndDefineZero(const ValueContext&, InternalErrorKind);
    CellPointer failWithoutCell(const OperationContext&, InternalErrorKind);

    TypedPointer builtinCellWord(BuiltinCellKey);
    FormAtPlace formOfConstantsWithCellsAt(const OperationContext&);
    CellPointer cellMadeByStub(const OperationContext&, StubKey, const CallOptions&, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord);
    const RegisterSet& registersLostAtSiteOf(ConstantAtSite);
    CellPointer sharedObjectOfData(const ValueContext&);
    CellPointer newCopyOnWriteArray(const ValueContext&);
    CellPointer newCopyOnWriteArrayByStub(const ValueContext&);

    CellPointer closureCreatedBy(const ValueContext&);
    bool canUseClosureStub(const NewClosureRequest&) const;
    CellPointer newFullClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newSlimClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newClosureByStub(const ValueContext&, const NewClosureRequest&);
    CellPointer callClosureStub(const ValueContext&, const NewClosureRequest&, uint64_t requestWord);
    Operand captureOf(const NewClosureRequest&, unsigned index) const;
    bool mayBeClosureOfFrame(ValueID captured);
    Operand broughtToHeap(const ValueContext&, ValueID captured, Operand);
    Vector<LValue, 4> captureWordsBySlot(const ValueContext&, const NewClosureRequest&);
    uint32_t closureNumberMentionedBy(const NewClosureRequest&, int32_t addend = 0);
    Integer closureWordOf(const NewClosureRequest&);

    std::array<std::optional<RegisterSet>, static_cast<size_t>(ConstantAtSite::BuiltinCell) + 1> m_registersLostAtSites;
    std::optional<bool> m_mayCaptureClosuresOfFrames;
};

CellPointer ConstantEmitter::permanentCell(TypedPointer cellWord, bool isNarrow, NOESCAPE const Invocable<CellPointer()> auto& create)
{
    if (isNarrow) {
        NarrowReference reference(m_out.load32(cellWord));
        CellPointer cell = decode(reference);
        return either<CellPointer>(Condition(m_out.notZero32(reference.value())), [&] {
            return cell;
        }, create);
    }
    CellPointer cell(m_out.load64(cellWord));
    return either<CellPointer>(Condition(m_out.notZero64(cell.value())), [&] {
        return cell;
    }, create);
}

CellPointer ConstantEmitter::numberedPermanentCell(const Machine::PureFunction& function, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord, NOESCAPE const Invocable<CellPointer(TypedPointer)> auto& create)
{
    if (LValue known = m_out.findPurePatch(function, Machine::Int64, { }))
        return CellPointer(known);
    TypedPointer word = cellWord();
    CellPointer cell = permanentCell(word, isNarrow, [&] {
        return create(word);
    });
    m_out.recordInPlaceOfPurePatch(function, cell.value());
    return cell;
}

CellPointer ConstantEmitter::cellNumberedBehindJoin(const Machine::PureFunction& function, NOESCAPE const Invocable<CellPointer()> auto& make)
{
    if (LValue known = m_out.findPurePatch(function, Machine::Int64, { }))
        return CellPointer(known);
    CellPointer cell = make();
    m_out.recordInPlaceOfPurePatch(function, cell.value());
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
