/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectCreationEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maskWidth = 32;

static uint32_t descriptorBitsOf(OptionSet<PropertyDescriptorFlags> flags)
{
    PropertyDescriptorBits bits { };
    bits.hasValue = flags.contains(PropertyDescriptorFlags::HasValue);
    bits.hasWritable = flags.contains(PropertyDescriptorFlags::HasWritable);
    bits.isWritable = flags.contains(PropertyDescriptorFlags::IsWritable);
    bits.hasEnumerable = flags.contains(PropertyDescriptorFlags::HasEnumerable);
    bits.isEnumerable = flags.contains(PropertyDescriptorFlags::IsEnumerable);
    bits.hasConfigurable = flags.contains(PropertyDescriptorFlags::HasConfigurable);
    bits.isConfigurable = flags.contains(PropertyDescriptorFlags::IsConfigurable);
    bits.hasGet = flags.contains(PropertyDescriptorFlags::HasGet);
    bits.hasSet = flags.contains(PropertyDescriptorFlags::HasSet);
    return std::bit_cast<uint32_t>(bits);
}

ObjectCreationEmitter::ObjectCreationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ObjectCreationEmitter::generateDefineOwnProperty(const ValueContext& value)
{
    ValueID objectID = value->objectOperand();
    ValueID definedValueID = value->valueOperand();
    ValueID getterID = value->getterOperand();
    ValueID setterID = value->setterOperand();
    OptionSet<PropertyDescriptorFlags> flags = value->descriptorFlags();
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasValue) == !!definedValueID);
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasGet) == !!getterID);
    RELEASE_ASSERT(flags.contains(PropertyDescriptorFlags::HasSet) == !!setterID);
    RELEASE_ASSERT(!definedValueID || !getterID);

    Boxed object = boxCell(in<CellPointer>(objectID));
    Boxed key = in<Boxed>(value->computedKeyOperand());
    Boxed valueOrGetter = inOptional<Boxed>(definedValueID ? definedValueID : getterID).value_or(undefined());
    Boxed setter = inOptional<Boxed>(setterID).value_or(undefined());
    callVoidOperation(value, RuntimeOperation::DefineOwnProperty, { object, key, valueOrGetter, setter, word32(static_cast<int32_t>(descriptorBitsOf(flags))) });
    define(value, get(objectID));
}

void ObjectCreationEmitter::generateDefineAccessors(const ValueContext& value)
{
    static_assert(!OBJECT_OFFSETOF(DefinedAccessor, key) && OBJECT_OFFSETOF(DefinedAccessor, function) == sizeof(EncodedJSValue) && sizeof(DefinedAccessor) == 2 * sizeof(EncodedJSValue));
    std::span<const PropertyKeyID> keys = value->keys();
    std::span<const ValueID> functions = value->functionsOperands();
    size_t count = keys.size();
    RELEASE_ASSERT(functions.size() == count && count <= maskWidth);

    CellPointer object = in<CellPointer>(value->objectOperand());
    ConstantEmitter& constants = emitter<ConstantEmitter>();
    Vector<Boxed, 16> keysAndFunctions;
    for (size_t i = 0; i < count; ++i) {
        keysAndFunctions.append(boxCell(constants.keyConstant(value, keys[i])));
        keysAndFunctions.append(boxCell(in<CellPointer>(functions[i])));
    }
    Pointer buffer = argumentBuffer(keysAndFunctions.span());
    Word32 getters = word32(static_cast<int32_t>(value->getterMask()));
    Word32 enumerable = word32(static_cast<int32_t>(value->enumerableMask()));
    callVoidOperation(value, RuntimeOperation::DefineAccessors, { object, word32(static_cast<int32_t>(count)), getters, enumerable, buffer });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
