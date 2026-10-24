/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCellFormat.h"

#if ENABLE(AOT)

#include "AOTClassToken.h"
#include "AOTClosureWord.h"
#include "AOTRuntimeValidation.h"
#include "ArrayConventions.h"
#include "CustomGetterSetter.h"
#include "GetterSetter.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPromiseCombinatorsContext.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseReaction.h"
#include "JSSentinel.h"
#include "JSWebAssemblyArray.h"
#include "JSWebAssemblyStruct.h"
#include "MarkedBlock.h"
#include "Symbol.h"
#include <limits>

#if !CPU(ADDRESS64) || !CPU(LITTLE_ENDIAN) || CPU(ARM64E)
#error "the word at byte 12 is read as the upper half of the 64-bit word at byte 8, and a pointer has no bits above its address"
#endif

namespace JSC { namespace AOT {

static_assert(sizeof(JSCell) == 8 && offsetOfShapeWord == 12);
static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);

enum class WordAtShapeOffset : uint8_t { Unlisted, ShapeWord, UpperHalfOfPointer, UpperHalfOfValue, UpperHalfOfClosureWord, BigIntHash, VectorLength, Zero, NotAValue };

static_assert(!canPassShapeTest(pointerUpperHalfBits));

static constexpr uint32_t valueUpperHalfBits = static_cast<uint32_t>(~JSValue::NumberTag >> 32);
static_assert(!canPassShapeTest(valueUpperHalfBits) && !mayBeUpperHalfOfSlimClosureWord(valueUpperHalfBits));

static_assert(!canPassShapeTest(closureWordUpperHalfBits));

static_assert(!canPassShapeTest(bigIntHashMask));

// A vector length can contain shapeBlockMarker. The only JSCellButterfly that is a value is a class token: a longer one is storage that compiled code reaches through its owner.
static constexpr uint32_t classTokenLengthBits = (1u << 17) - 1;
static_assert(ClassToken::firstNameSlot + std::numeric_limits<decltype(LayoutData::numberOfFields)>::max() <= classTokenLengthBits);
static_assert(!canPassShapeTest(classTokenLengthBits) && !mayBeUpperHalfOfSlimClosureWord(classTokenLengthBits));

#define FOR_EACH_JS_TYPE_WITH_BUTTERFLY(macro) \
    macro(ObjectType) \
    macro(FinalObjectType) \
    macro(JSCalleeType) \
    macro(JSFunctionType) \
    macro(CompiledClosureType) \
    macro(InternalFunctionType) \
    macro(NullSetterFunctionType) \
    macro(BooleanObjectType) \
    macro(NumberObjectType) \
    macro(ErrorInstanceType) \
    macro(GlobalProxyType) \
    macro(DirectArgumentsType) \
    macro(ScopedArgumentsType) \
    macro(ClonedArgumentsType) \
    macro(ArrayType) \
    macro(DerivedArrayType) \
    macro(ArrayBufferType) \
    macro(Int8ArrayType) \
    macro(Uint8ArrayType) \
    macro(Uint8ClampedArrayType) \
    macro(Int16ArrayType) \
    macro(Uint16ArrayType) \
    macro(Int32ArrayType) \
    macro(Uint32ArrayType) \
    macro(Float16ArrayType) \
    macro(Float32ArrayType) \
    macro(Float64ArrayType) \
    macro(BigInt64ArrayType) \
    macro(BigUint64ArrayType) \
    macro(DataViewType) \
    macro(GlobalObjectType) \
    macro(GlobalLexicalEnvironmentType) \
    macro(LexicalEnvironmentType) \
    macro(ModuleEnvironmentType) \
    macro(StrictEvalActivationType) \
    macro(WithScopeType) \
    macro(AsyncDisposableStackType) \
    macro(DisposableStackType) \
    macro(ModuleNamespaceObjectType) \
    macro(ShadowRealmType) \
    macro(RegExpObjectType) \
    macro(JSDateType) \
    macro(ProxyObjectType) \
    macro(JSGeneratorType) \
    macro(JSAsyncFunctionGeneratorType) \
    macro(JSAsyncGeneratorType) \
    macro(JSArrayIteratorType) \
    macro(JSIteratorType) \
    macro(JSIteratorHelperType) \
    macro(JSMapIteratorType) \
    macro(JSSetIteratorType) \
    macro(JSStringIteratorType) \
    macro(JSWrapForValidIteratorType) \
    macro(JSRegExpStringIteratorType) \
    macro(JSAsyncFromSyncIteratorType) \
    macro(JSPromiseType) \
    macro(JSMapType) \
    macro(JSSetType) \
    macro(JSWeakMapType) \
    macro(JSWeakSetType) \
    macro(WebAssemblyModuleType) \
    macro(WebAssemblyInstanceType) \
    macro(SlimClosureWithPropertiesType) \
    macro(StringObjectType) \
    macro(DerivedStringObjectType) \
    macro(InternalFieldTupleType) \

#define AOT_CHECK_BUTTERFLY_FIELD(type) static_assert(hasButterflyField(type));
FOR_EACH_JS_TYPE_WITH_BUTTERFLY(AOT_CHECK_BUTTERFLY_FIELD)
#undef AOT_CHECK_BUTTERFLY_FIELD
static_assert(!hasButterflyField(WebAssemblyGCObjectType) && !hasButterflyField(CompiledObjectType) && !hasButterflyField(SlimClosureType));

static constexpr WordAtShapeOffset wordAtShapeOffset(JSType type)
{
    switch (type) {
    case StringType:
        static_assert(JSString::offsetOfValue() == 8);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case SymbolType:
        static_assert(Symbol::offsetOfSymbolImpl() == 8);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case HeapBigIntType:
        static_assert(JSBigInt::offsetOfLength() == 8 && sizeof(JSBigInt) == 16);
        return WordAtShapeOffset::BigIntHash;
    case GetterSetterType:
        static_assert(GetterSetter::offsetOfGetter() == 8);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case CustomGetterSetterType:
        static_assert(sizeof(CustomGetterSetter) >= 16);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case JSCellButterflyType:
        static_assert(JSCellButterfly::offsetOfVectorLength() == 12 && MAX_STORAGE_VECTOR_LENGTH < compiledObjectShapeBit);
        return WordAtShapeOffset::VectorLength;
    case JSSlimPromiseReactionType:
    case JSFullPromiseReactionType:
    case JSPromiseCombinatorsGlobalContextType:
        static_assert(sizeof(JSSlimPromiseReaction) >= 16 && sizeof(JSFullPromiseReaction) >= 16 && sizeof(JSPromiseCombinatorsGlobalContext) >= 16);
        return WordAtShapeOffset::UpperHalfOfValue;
    case JSPromiseCombinatorsContextType:
        static_assert(sizeof(JSPromiseCombinatorsContext) >= 16);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case SentinelType:
        static_assert(sizeof(JSSentinel) == 16);
        return WordAtShapeOffset::Zero;
    case WebAssemblyGCObjectType:
#if ENABLE(WEBASSEMBLY)
        static_assert(JSWebAssemblyStruct::offsetOfData() <= offsetOfShapeWord && JSWebAssemblyStruct::offsetOfData() + Wasm::structPayloadOffsetPastShapeWord == offsetOfShapeWord + sizeof(uint32_t));
        static_assert(JSWebAssemblyArray::offsetOfSize() + sizeof(uint32_t) == offsetOfShapeWord && JSWebAssemblyArray::offsetPastShapeWord == offsetOfShapeWord + sizeof(uint32_t));
#endif
        return WordAtShapeOffset::Zero;
    case CompiledObjectType:
        static_assert(offsetOfOrderWord == 8);
        return WordAtShapeOffset::ShapeWord;
    case SlimClosureType:
        static_assert(offsetOfSlimClosureWord == 8);
        return WordAtShapeOffset::UpperHalfOfClosureWord;
#define AOT_CASE(type) case type:
    FOR_EACH_JS_TYPE_WITH_BUTTERFLY(AOT_CASE)
#undef AOT_CASE
    case MaxJSType:
        static_assert(JSObject::butterflyOffset() == 8);
        return WordAtShapeOffset::UpperHalfOfPointer;
    case CellType:
    case StructureType:
    case APIValueWrapperType:
    case NativeExecutableType:
    case ProgramExecutableType:
    case ModuleProgramExecutableType:
    case EvalExecutableType:
    case FunctionExecutableType:
    case UnlinkedFunctionExecutableType:
    case UnlinkedProgramCodeBlockType:
    case UnlinkedModuleProgramCodeBlockType:
    case UnlinkedEvalCodeBlockType:
    case UnlinkedFunctionCodeBlockType:
    case CodeBlockType:
    case JSSourceCodeType:
    case JSWebAssemblyStreamingContextType:
    case JSMicrotaskDispatcherType:
    case ModuleRegistryEntryType:
    case ModuleLoadingContextType:
    case ModuleLoaderPayloadType:
    case ModuleGraphLoadingStateType:
    case JSModuleLoaderType:
    case CompiledEnvironmentType:
        return WordAtShapeOffset::NotAValue;
    }
    return WordAtShapeOffset::Unlisted;
}

#undef FOR_EACH_JS_TYPE_WITH_BUTTERFLY

#define AOT_CHECK_JS_TYPE(type, speculatedType) static_assert(wordAtShapeOffset(type) != WordAtShapeOffset::Unlisted, #type " needs a line in AOTCellFormat.cpp");
FOR_EACH_JS_TYPE(AOT_CHECK_JS_TYPE)
#undef AOT_CHECK_JS_TYPE

static constexpr bool isValidWord(WordAtShapeOffset content, uint32_t word)
{
    switch (content) {
    case WordAtShapeOffset::ShapeWord:
        return word & compiledObjectShapeBit;
    case WordAtShapeOffset::UpperHalfOfPointer:
        return !(word & ~pointerUpperHalfBits);
    case WordAtShapeOffset::UpperHalfOfValue:
        return !(word & ~valueUpperHalfBits);
    case WordAtShapeOffset::UpperHalfOfClosureWord:
        return !word || (!(word & ~closureWordUpperHalfBits) && mayBeUpperHalfOfSlimClosureWord(word));
    case WordAtShapeOffset::BigIntHash:
        return !(word & ~bigIntHashMask);
    case WordAtShapeOffset::VectorLength:
        return word <= MAX_STORAGE_VECTOR_LENGTH;
    case WordAtShapeOffset::Zero:
        return !word;
    case WordAtShapeOffset::NotAValue:
        return true;
    case WordAtShapeOffset::Unlisted:
        break;
    }
    return false;
}

static_assert(isValidWord(WordAtShapeOffset::ShapeWord, shapeWordOf(1)) && !isValidWord(WordAtShapeOffset::ShapeWord, pointerUpperHalfBits));
static_assert(isValidWord(WordAtShapeOffset::UpperHalfOfPointer, 0) && isValidWord(WordAtShapeOffset::UpperHalfOfPointer, pointerUpperHalfBits) && !isValidWord(WordAtShapeOffset::UpperHalfOfPointer, 0u - 5));
static_assert(isValidWord(WordAtShapeOffset::UpperHalfOfValue, 0) && !isValidWord(WordAtShapeOffset::UpperHalfOfValue, static_cast<uint32_t>(JSValue::NumberTag >> 32)) && !isValidWord(WordAtShapeOffset::UpperHalfOfValue, 1u << (JSValue::DoubleEncodeOffsetBit - 32)));
static_assert(isValidWord(WordAtShapeOffset::UpperHalfOfClosureWord, closureWordMarker | (ClosureWord::firstReservedFunction - 1)) && !isValidWord(WordAtShapeOffset::UpperHalfOfClosureWord, ClosureWord::firstReservedFunction - 1) && !isValidWord(WordAtShapeOffset::UpperHalfOfClosureWord, compiledObjectShapeBit));
static_assert(isValidWord(WordAtShapeOffset::BigIntHash, 0) && isValidWord(WordAtShapeOffset::BigIntHash, bigIntHashMask) && !isValidWord(WordAtShapeOffset::BigIntHash, 0u - 5) && !isValidWord(WordAtShapeOffset::BigIntHash, shapeBlockMarker));
static_assert(isValidWord(WordAtShapeOffset::VectorLength, classTokenLengthBits) && isValidWord(WordAtShapeOffset::VectorLength, shapeBlockMarker) && !isValidWord(WordAtShapeOffset::VectorLength, 0u - 5));
static_assert(isValidWord(WordAtShapeOffset::Zero, 0) && !isValidWord(WordAtShapeOffset::Zero, 1));
static_assert(isValidWord(WordAtShapeOffset::NotAValue, 0u - 5) && !isValidWord(WordAtShapeOffset::Unlisted, 0));

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

void validateShapeWord(const JSCell* cell)
{
    JSType type = cell->type();
    if (type == CompiledEnvironmentType)
        return;

    uint32_t word = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(cell) + offsetOfShapeWord);
    bool isValid = isValidWord(type > LastJSCObjectType ? WordAtShapeOffset::UpperHalfOfPointer : wordAtShapeOffset(type), word);
    if (!isValid)
        crashOnValidationFailure(cell->vm(), { ValidationFailureKind::ShapeWordInvalid, { }, { }, cell });
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
