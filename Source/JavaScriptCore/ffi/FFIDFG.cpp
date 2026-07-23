/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FFIDFG.h"

#if USE(BUN_JSC_ADDITIONS)

#if ENABLE(DFG_JIT)

#include "DFGGraph.h"
#include "DFGInsertionSet.h"
#include "DFGNode.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "JSCellInlines.h"
#include "JSFFIFunction.h"
#include "Options.h"

namespace JSC { namespace FFI {

bool tryConvertCallToCallFFI(DFG::Graph& graph, DFG::InsertionSet& insertionSet, unsigned nodeIndex, DFG::Node* node, JSFunction* function)
{
    if (!Options::useFFICallInDFG())
        return false;

    // Only plain calls become CallFFI - never Construct / TailCall / TailCallInlinedCaller.
    if (node->op() != DFG::Call)
        return false;

    // bun:ffi is 64-bit only (compiled out on 32-bit).
    if (!is64Bit())
        return false;

    // The callee child is the constant JSFFIFunction (the ByteCodeParser feed
    // guarantees it); anything else is not an FFI call site.
    if (!function)
        return false;
    auto* ffiFunction = dynamicDowncast<JSFFIFunction>(function);
    if (!ffiFunction)
        return false;

    Signature& signature = ffiFunction->signature();

    // Exact arity only: children are |callee|, |this|, then the JS arguments.
    if (node->numChildren() - 2 != signature.jsArgumentCount())
        return false;

    // The DFG/FTL CallFFI codegen calls the signature's invoke thunk; make
    // its existence an invariant of the conversion by generating it here.
    // The thunk is process-shared and immortal once generated (SPEC 7.2),
    // and generation is a plain LinkBuffer emit under the Signature's own
    // lock, so it is safe on the compiler thread. SPEC 7.2 lets generation
    // fail on executable-memory exhaustion; then keep the plain Call so this
    // function stays on its host path instead of degrading the whole node to
    // a runtime OutOfMemoryError.
    if (!signature.invokeThunk())
        return false;

    // The inserted argument checks and DoubleRep nodes exit, so they can only
    // go where exiting is valid. Exactly like CallWasm's indexForChecks
    // (DFGStrengthReductionPhase.cpp), the only candidate index is the Call
    // node itself: it defines-after every argument child (no use-before-def)
    // and shares the origin the inserted nodes carry, so its origin must be
    // exitOK - otherwise refuse the conversion.
    if (!node->origin.exitOK)
        return false;
    unsigned checkIndex = nodeIndex;

    // Reserve outgoing frame space for the canonical slot buffer
    // (argumentCount() + 1 slots, SPEC section 4).
    graph.m_parameterSlots = std::max(graph.m_parameterSlots, DFG::Graph::parameterSlotsForArgCount(signature.slotCount() + 1));

    // Establish argument checks and edge use kinds here and nowhere else:
    // PredictionPropagation and FixupPhase run before strength reduction.
    unsigned jsArgumentIndex = 0;
    for (unsigned index = 0; index < signature.argumentCount(); ++index) {
        Type type = signature.argumentType(index);
        // Synthetic arguments (napi_env) are supplied by the engine and have
        // no JS argument child.
        if (isSyntheticArgument(type))
            continue;

        unsigned childIndex = 2 + jsArgumentIndex++;
        DFG::Edge argument = graph.varArgChild(node, childIndex);
        DFG::Node* argumentNode = argument.node();
        switch (type) {
        case Type::Char:
        case Type::Int8:
        case Type::Uint8:
        case Type::Int16:
        case Type::Uint16:
        case Type::Int32:
        case Type::Uint32: {
            insertionSet.insertCheck(checkIndex, node->origin, DFG::Edge(argumentNode, DFG::Int32Use));
            graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::KnownInt32Use);
            break;
        }
        case Type::Bool: {
            // Codegen converts a KnownInt32Use bool with a `!= 0` compare
            // (toBoolean), never `and32(1)`.
            if (argumentNode->shouldSpeculateBoolean()) {
                insertionSet.insertCheck(checkIndex, node->origin, DFG::Edge(argumentNode, DFG::BooleanUse));
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::KnownBooleanUse);
            } else if (argumentNode->shouldSpeculateInt32()) {
                insertionSet.insertCheck(checkIndex, node->origin, DFG::Edge(argumentNode, DFG::Int32Use));
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::KnownInt32Use);
            } else
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::UntypedUse);
            break;
        }
        case Type::Float:
        case Type::Double: {
            // FFI-SPEC-GAP: SPEC 10.2 lists NotCellNorBigIntUse as the last
            // resort here (copying the CallWasm F32/F64 recipe), but that use kind
            // makes DoubleRep silently coerce null -> +0.0, booleans -> 0/1 and
            // (for f32) undefined -> NaN with no exception, contradicting the
            // SPEC 5 conversion table every other tier implements (f64 maps
            // null/undefined -> NaN; booleans and any non-number f32 argument
            // throw TypeError). SPEC 5 states all tiers are behaviorally
            // identical and 11.4 tests that, so only the OSR-exiting number use
            // kinds get a DoubleRep; anything else keeps the boxed value and
            // flows through operationFFIWriteSlot (the SPEC 5 rules) in codegen.
            DFG::UseKind useKind;
            if (argumentNode->shouldSpeculateDoubleReal())
                useKind = DFG::RealNumberUse;
            else if (argumentNode->shouldSpeculateNumber())
                useKind = DFG::NumberUse;
            else {
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::UntypedUse);
                break;
            }
            DFG::Node* result = insertionSet.insertNode(checkIndex, SpecBytecodeDouble, DFG::DoubleRep, node->origin, DFG::Edge(argumentNode, useKind));
            graph.varArgChild(node, childIndex) = DFG::Edge(result, DFG::DoubleRepUse);
            break;
        }
        case Type::Int64:
        case Type::Uint64:
        case Type::Int64Fast:
        case Type::Uint64Fast:
        case Type::Pointer:
        case Type::CString:
        case Type::Function:
        case Type::Buffer:
        case Type::NapiValue: {
            // Converted at runtime through operationFFIWriteSlot; keep the value boxed.
            graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::UntypedUse);
            break;
        }
        case Type::NapiEnv:
        case Type::Void:
            // NapiEnv is synthetic (skipped above); Void is never a valid argument type.
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }
    ASSERT(jsArgumentIndex == signature.jsArgumentCount());

    // Child 0 is the callee, a cell constant (dynamicCastConstant above), so it
    // trivially satisfies KnownCellUse (SPEC section 10.1). Child 1 (|this|)
    // stays UntypedUse and is ignored by codegen.
    graph.varArgChild(node, 0) = DFG::Edge(graph.varArgChild(node, 0).node(), DFG::KnownCellUse);

    node->convertToCallFFI(graph.freeze(ffiFunction));
    return true;
}

SpeculatedType speculatedResultTypeForCallFFI(DFG::Node* node)
{
    switch (node->ffiSignature().returnType()) {
    case Type::Char:
    case Type::Int8:
    case Type::Uint8:
    case Type::Int16:
    case Type::Uint16:
    case Type::Int32:
        return SpecInt32Only;
    case Type::Uint32:
        return SpecBytecodeNumber;
    case Type::Bool:
        return SpecBoolean;
    case Type::Float:
    case Type::Double:
        return SpecBytecodeDouble;
    case Type::Void:
        return SpecOther;
    case Type::Int64:
    case Type::Uint64:
        // FFI-SPEC-GAP: SPEC section 10.3 spells this as SpecHeapBigInt | SpecBigInt32;
        // that is exactly SpecBigInt under USE(BIGINT32), and SpecBigInt drops
        // SpecBigInt32 when !USE(BIGINT32) (which SpeculatedType.h requires and
        // which matches the CallWasm I64 precedent), so use SpecBigInt.
        return SpecBigInt;
    case Type::Int64Fast:
    case Type::Uint64Fast:
        return SpecBytecodeNumber | SpecHeapBigInt;
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer:
        // A null pointer boxes to jsNull(), everything else to a number (SPEC section 5).
        return SpecBytecodeNumber | SpecOther;
    case Type::NapiValue:
        return SpecBytecodeTop;
    case Type::NapiEnv:
        // NapiEnv is never a valid return type (Signature::tryCreate rejects it).
        RELEASE_ASSERT_NOT_REACHED();
        return SpecBytecodeTop;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return SpecBytecodeTop;
}

} } // namespace JSC::FFI

#endif // ENABLE(DFG_JIT)

#endif // USE(BUN_JSC_ADDITIONS)
