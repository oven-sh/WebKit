/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// The family aot/builtins/typedarray.def.

std::optional<Writes> writesOfInlineTypedArray(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::TypedArrayWithLength:
    // Where the length depends on that of the buffer, the engine is asked.
    case InlineBuiltin::TypedArrayLength:
    case InlineBuiltin::TypedArrayByteLength:
    case InlineBuiltin::TypedArrayByteOffset:
        return Writes::RuntimeState;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineTypedArray(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    if (which != InlineBuiltin::TypedArrayByteLength && which != InlineBuiltin::TypedArrayByteOffset)
        return lowerContainerBuiltin(which, result, value, inPlace);
    // Of a view that has been detached both are zero, as its length is.
    TypedArrayReceiver view = typedArrayReceiver(nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName)));
    Type sizeType = Type::integer({ 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
    bool isSizeOfElementsKnown = value.engineClass() != Format::none;
    Word32 isInPlace = which == InlineBuiltin::TypedArrayByteOffset || isSizeOfElementsKnown ? hasLengthOfItsOwn(view) : boolean(false);
    return ensure(either<Integer>(isInPlace, [&] {
        if (which == InlineBuiltin::TypedArrayByteOffset)
            return loadField<Integer>(view.view, m_heaps.JSArrayBufferView_byteOffset, view.flags);
        Integer length = loadField<Integer>(view.view, m_heaps.JSArrayBufferView_length, view.flags);
        return Integer(m_out.shl(length.value(), m_out.constInt32(isSizeOfElementsKnown ? logElementSize(m_builtins.classes()[value.engineClass()].typedArrayType) : 0)));
    }, [&] {
        return convertTo<Integer>(callBuiltinFunction(value, OperationEffects { inPlace, true }), sizeType);
    }), sizeType, typeOf(result));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
