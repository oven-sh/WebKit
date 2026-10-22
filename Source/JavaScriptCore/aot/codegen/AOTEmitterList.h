/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#define FOR_EACH_AOT_OPERATION_EMITTER(v) \
    v(ConstantEmitter) \
    v(BindingEmitter) \
    v(AllocationEmitter) \
    v(AccessEmitter) \
    v(OperatorEmitter) \
    v(CastEmitter) \
    v(CallEmitter) \
    v(ExceptionEmitter) \
    v(HintEmitter) \
    v(ControlEmitter) \
    v(ClassEmitter) \
    v(PrivateAccessEmitter) \
    v(ForInEmitter) \
    v(ModuleEmitter) \
    v(EvalEmitter) \
    v(EngineIntrinsicEmitter) \
    v(RuntimeIntrinsicEmitter) \
    v(PromiseIntrinsicEmitter) \
    v(ArrayIntrinsicEmitter) \
    v(CollectionIntrinsicEmitter) \
    v(GuardEmitter) \
    v(GuardValueEmitter) \
    v(SlotAccessEmitter) \
    v(OwnAccessEmitter) \
    v(ElementAccessEmitter) \
    v(ArgumentsEmitter) \
    v(EnvironmentEmitter) \
    v(BuiltinCallEmitter) \
    v(ExpandedIterationEmitter) \
    v(SavedFrameEmitter) \
    v(StringBuilderEmitter) \
    v(JSONParseEmitter) \
    v(ValidationEmitter) \
    v(JSONWriteEmitter) \
    v(FusedAccessEmitter) \
    v(SlotRunEmitter) \
    v(ObjectCreationEmitter) \
    v(SelectedOperatorEmitter) \
    v(StringEmitter) \
    v(StringRangeReadEmitter) \
    v(StringBuilderPartEmitter) \
    v(KeyPartsLookupEmitter) \
    v(ResolvedCallEmitter) \
    v(InlineBuiltinEmitter) \
    v(RepresentationEmitter)

#define FOR_EACH_AOT_INLINE_BUILTIN_EMITTER(v) \
    v(ArrayBuiltinEmitter) \
    v(ArrayStorageBuiltinEmitter) \
    v(ArrayBufferViewBuiltinEmitter) \
    v(OwnPropertyBuiltinEmitter) \
    v(ObjectBuiltinEmitter) \
    v(HashTableBuiltinEmitter) \
    v(MapSetBuiltinEmitter) \
    v(PromiseBuiltinEmitter) \
    v(BufferBuiltinEmitter) \
    v(MathBuiltinEmitter) \
    v(DateBuiltinEmitter) \
    v(FunctionBuiltinEmitter) \
    v(StringBuiltinEmitter) \
    v(RegExpBuiltinEmitter) \
    v(ErrorBuiltinEmitter) \
    v(IntlBuiltinEmitter)

#define FOR_EACH_AOT_SHARED_EMITTER(v) \
    v(EntryEmitter) \
    v(RuntimeCallEmitter) \
    v(ThrowEmitter) \
    v(TypeTestEmitter) \
    v(ShapeTestEmitter) \
    v(ClosureTestEmitter) \
    v(TruthinessEmitter) \
    v(ConversionEmitter) \
    v(SlotEmitter) \
    v(StaticsEmitter) \
    v(BarrierEmitter) \
    v(CellAllocationEmitter) \
    v(FrameMapEmitter) \
    v(RuntimeValidationEmitter)

#define FOR_EACH_AOT_EMITTER(v) \
    FOR_EACH_AOT_OPERATION_EMITTER(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_EMITTER(v) \
    FOR_EACH_AOT_SHARED_EMITTER(v)

namespace JSC { namespace AOT { namespace Typed {

#define AOT_DECLARE_EMITTER(Class) class Class;
FOR_EACH_AOT_EMITTER(AOT_DECLARE_EMITTER)
#undef AOT_DECLARE_EMITTER

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
