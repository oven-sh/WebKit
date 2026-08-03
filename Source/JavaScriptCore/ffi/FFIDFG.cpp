/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

    if (node->op() != DFG::Call)
        return false;

    if (!function)
        return false;
    auto* ffiFunction = dynamicDowncast<JSFFIFunction>(function);
    if (!ffiFunction)
        return false;

    if (ffiFunction->isHostPathOnly())
        return false;

    Signature& signature = ffiFunction->signature();

    if (node->numChildren() - 2 != signature.argumentCount())
        return false;

    if (!signature.invokeThunk())
        return false;

    if (!node->origin.exitOK)
        return false;
    unsigned checkIndex = nodeIndex;

    graph.m_parameterSlots = std::max(graph.m_parameterSlots, DFG::Graph::parameterSlotsForArgCount(signature.slotCount() + 1));

    for (unsigned index = 0; index < signature.argumentCount(); ++index) {
        Type type = signature.argumentType(index);
        unsigned childIndex = 2 + index;
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
            if (argumentNode->shouldSpeculateInt32()) {
                insertionSet.insertCheck(checkIndex, node->origin, DFG::Edge(argumentNode, DFG::Int32Use));
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::KnownInt32Use);
            } else
                graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::UntypedUse);
            break;
        }
        case Type::Bool: {
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
        case Type::BufferLength:
        case Type::JSValue: {
            graph.varArgChild(node, childIndex) = DFG::Edge(argumentNode, DFG::UntypedUse);
            break;
        }
        case Type::RESERVED_WasNapiEnv:
        case Type::Void:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }

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
        return SpecBigInt;
    case Type::Int64Fast:
    case Type::Uint64Fast:
        return SpecBytecodeNumber | SpecHeapBigInt;
    case Type::CString:
        return SpecString | SpecOther;
    case Type::Pointer:
    case Type::Function:
    case Type::Buffer:
        return SpecBytecodeNumber | SpecOther | SpecBigInt;
    case Type::JSValue:
        return SpecBytecodeTop;
    case Type::RESERVED_WasNapiEnv:
    case Type::BufferLength:
        RELEASE_ASSERT_NOT_REACHED();
        return SpecBytecodeTop;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return SpecBytecodeTop;
}

} } // namespace JSC::FFI

#endif // ENABLE(DFG_JIT)

#endif // USE(BUN_JSC_ADDITIONS)
