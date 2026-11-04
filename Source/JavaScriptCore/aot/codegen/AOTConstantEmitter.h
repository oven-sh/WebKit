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

struct BoundFunctionPlan;

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
    void generateNewClosure(const ValueContext&);
    void generateNewBoundClosure(const ValueContext&);
    void generateKeyConstant(const ValueContext&);
    void generateUnspecified(const ValueContext& value) { define(value, zeroOf(value.representation())); }
    void generateEmptyArgumentList(const ValueContext& value) { defineCell(value, CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.emptyArgumentList)).value())); }

    CellPointer stringConstant(const OperationContext&, StringID);
    CellPointer keyConstant(const OperationContext&, PropertyKeyID);
    CellPointer bigIntConstant(const OperationContext&, StringID digits);
    CellPointer builtinCell(const OperationContext& context, BuiltinID id) { return builtinCell(context, BuiltinCellKey::object(id)); }
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    CellPointer singletonClosure(const OperationContext&, FunctionID);
    // Makes the closure that a slot of the object deferred and stores it there. With several functions the shape of the object says which: each comes with the layouts that defer it.
    // The stub for closures without a function object makes what it can make. The run time makes the rest.
    struct DeferredFunction {
        FunctionID function;
        Vector<LayoutID, 2> layouts;
    };
    // offsetOfEnvironment: of the hidden field that keeps the environment, for closures over it.
    CellPointer makeClosureOfField(const OperationContext&, std::span<const DeferredFunction>, CellPointer object, uint32_t offsetOfSlot, bool capturesObject, std::optional<uint32_t> offsetOfEnvironment, NOESCAPE const ScopedLambda<CellPointer()>& makeInRunTime);
    // For a type test, which has no context.
    CellPointer keyConstant(PropertyKeyID);
    CellPointer builtinCell(BuiltinID);
    void branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID, BranchTargets);
    Operand constantOf(JSValue, Representation);
    Operand zeroOf(Representation);
    // The cell in a word that, once filled, stays as it is.
    CellPointer permanentCell(TypedPointer cellWord, bool isNarrow, NOESCAPE const Invocable<CellPointer()> auto& create);
    // The same for a cell that is a root for good from then on. The cell behind the join is numbered under the function: a later request in the scope takes it, and has no load, no test and no arm.
    CellPointer numberedPermanentCell(const Machine::PureFunction&, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord, NOESCAPE const Invocable<CellPointer(TypedPointer)> auto& create);
    // For every other cell that is a root for good once it is made. `make` ends in the join of the arm that has read the cell and the arm that has had it made.
    CellPointer cellNumberedBehindJoin(const Machine::PureFunction&, NOESCAPE const Invocable<CellPointer()> auto& make);

    struct ConstantMadeAtSite {
        ConstantAtSite constant;
        // What lies behind the call: an index into the mentions of the function, or the bits themselves.
        uint32_t number { 0 };
        bool numberIsMention { false };
        // CallOptions::runsCodeOfHost.
        bool runsCodeOfHost { false };
    };
    // The rare arm of permanentCell(), as one call: the stub fills the word, hands the cell back where the other arm has it and keeps every other register. The call says that it writes the heap of the word.
    // A type test has no context. `literalStringWord` is the operand of ConstantAtSite::LiteralStringInRegister.
    CellPointer makeConstantAtSite(const OperationContext*, const ConstantMadeAtSite&, const Machine::AbstractHeap& heapOfWord, LValue literalStringWord = nullptr);

private:
    struct NewClosureRequest {
        FunctionID function;
        // What the word of the closure names: CallingConventions::closureEntryOf(function), or the generic entry of a bound function of it.
        CodeUnit entry;
        FunctionKindData kind;
        MentionedAllocation allocation;
        AllocationStorage storage;
        ClosureForm form;
        std::span<const ValueID> captures;
        // Of a bound function: what lays the closure out. Null: the record of the function does.
        const BoundFunctionPlan* bound { nullptr };
        // How many of `captures` the closure holds. The record lists an operand for a capture that is removed too.
        unsigned numberOfWords { 0 };
    };

    void defineCell(const ValueContext&, CellPointer);
    LiteralStringWord spelledLiteralString(StringID);
    void failAndDefineZero(const ValueContext&, InternalErrorKind);
    CellPointer failWithoutCell(const OperationContext&, InternalErrorKind);

    TypedPointer builtinCellWord(BuiltinCellKey);
    CellPointer cellMadeByStub(const OperationContext&, StubKey, CallOptions, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord);
    // What a site of the stub loses whichever register its result takes.
    const RegisterSet& registersLostAtSiteOf(ConstantAtSite);
    CellPointer newCopyOnWriteArray(const ValueContext&);
    CellPointer newCopyOnWriteArrayByStub(const ValueContext&);

    CellPointer closureCreatedBy(const ValueContext&);
    CellPointer boundClosureCreatedBy(const ValueContext&);
    bool canUseClosureStub(const NewClosureRequest&) const;
    CellPointer newFullClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newSlimClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newClosureByStub(const ValueContext&, const NewClosureRequest&);
    CellPointer callClosureStub(const ValueContext&, const NewClosureRequest&, uint64_t requestWord);
    Operand captureOf(const NewClosureRequest&, unsigned index) const;
    Vector<LValue, 4> captureWordsBySlot(const NewClosureRequest&);
    void storeCaptures(CellPointer closure, const NewClosureRequest&, const Machine::IndexedAbstractHeap&);
    // The mention of the closure number of NewClosureRequest::entry.
    uint32_t closureNumberMentionedBy(const NewClosureRequest&, int32_t addend = 0);
    Integer closureWordOf(const NewClosureRequest&);

    std::array<std::optional<RegisterSet>, static_cast<size_t>(ConstantAtSite::LiteralStringInRegister) + 1> m_registersLostAtSites;
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
