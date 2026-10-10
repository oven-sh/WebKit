/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "IntlCollator.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "ParseInt.h"
#include <wtf/text/AdaptiveStringSearcher.h>

namespace JSC { namespace AOT {

// What takes characters neither allocates nor throws. Whoever calls keeps the strings alive. The start is within the string.

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFindCharacter8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start))
{
    return WTF::find(unsafeMakeSpan(characters, length), static_cast<Latin1Character>(character), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFind8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, uint32_t start))
{
    if (!otherLength)
        return start;
    if (otherLength > length - start)
        return notFound;
    return static_cast<size_t>(static_cast<ptrdiff_t>(searchString(instance->vm->adaptiveStringSearcherTables(), unsafeMakeSpan(characters, length), unsafeMakeSpan(other, otherLength), start)));
}

// Where what is left begins without the white space in front, and where it ends without that behind, no earlier than at the start.

// A text of the program that is all ASCII, by its number: no string is made of it.
static ALWAYS_INLINE StringView textOfProgram(Instance* instance, uint32_t text)
{
    return StringView { byteCast<Latin1Character>(instance->typed->data.string(text)) };
}

// What Stub::StringStartsWithText and Stub::StringEndsWithText leave: a rope, which is made flat, and 16 bits.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringStartsWithText, size_t, (Instance* instance, JSCell* string, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    StringView other = textOfProgram(instance, text);
    if (uncheckedDowncast<JSString>(string)->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->startsWith(other));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringEndsWithText, size_t, (Instance* instance, JSCell* string, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    StringView other = textOfProgram(instance, text);
    if (uncheckedDowncast<JSString>(string)->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->endsWith(other));
}

// As operationAOTTypedStringIndexOf. The place is within the string.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringIndexOfText, size_t, (Instance* instance, JSCell* string, uint32_t text, int32_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = uncheckedDowncast<JSString>(string);
    StringView other = textOfProgram(instance, text);
    unsigned position = place;
    if (base->length() < static_cast<uint64_t>(other.length()) + position)
        OPERATION_RETURN(scope, none);
    if (other.length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, other[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), other, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

// The searches of operationAOTTypedStringFind8 and operationAOTTypedStringReverseFind8 for a text of the program, by its number.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFindText8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t start))
{
    auto other = instance->typed->data.string(text);
    return operationAOTTypedStringFind8(instance, characters, length, byteCast<Latin1Character>(other.data()), other.size(), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFindText8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, uint32_t text, int32_t position))
{
    auto other = instance->typed->data.string(text);
    return operationAOTTypedStringReverseFind8(instance, characters, length, byteCast<Latin1Character>(other.data()), other.size(), position);
}

// As operationAOTTypedStringLastIndexOf.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLastIndexOfText, size_t, (Instance* instance, JSCell* cell, uint32_t text, int32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* string = uncheckedDowncast<JSString>(cell);
    StringView other = textOfProgram(instance, text);
    unsigned length = string->length();
    unsigned otherLength = other.length();
    if (length < otherLength)
        OPERATION_RETURN(scope, none);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, other[0], start))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    size_t found;
    if (otherLength == 1)
        found = characters->reverseFind(other[0], start);
    else if (!start)
        found = characters->startsWith(other) ? 0 : notFound;
    else
        found = characters->reverseFind(other, start);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

// What Stub::StringIsText leaves: a rope, which stays one, and 16 bits. The text is all ASCII.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringIsText, size_t, (Instance* instance, JSCell* cell, uint32_t text))
{
    auto* string = uncheckedDowncast<JSString>(cell);
    auto expected = byteCast<Latin1Character>(instance->typed->data.string(text));
    if (string->length() != expected.size())
        return false;
    if (!string->isRope())
        return StringView { *string->tryGetValueImpl() } == StringView { expected };
    if (string->is8Bit()) {
        Vector<Latin1Character, 64> characters(expected.size());
        string->resolveToBuffer(characters.mutableSpan());
        return equalSpans(characters.span(), expected);
    }
    Vector<char16_t, 64> characters(expected.size());
    string->resolveToBuffer(characters.mutableSpan());
    return StringView { characters.span() } == StringView { expected };
}

// What Stub::StringsAreEqual leaves: ropes, which stay ropes, and 16 bits.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringsAreEqual, size_t, (Instance*, JSCell* first, JSCell* second))
{
    auto* a = uncheckedDowncast<JSString>(first);
    auto* b = uncheckedDowncast<JSString>(second);
    unsigned length = a->length();
    if (length != b->length())
        return false;
    Vector<char16_t, 64> buffers[2];
    auto viewOf = [&](JSString* string, Vector<char16_t, 64>& buffer) -> StringView {
        if (!string->isRope())
            return StringView { *string->tryGetValueImpl() };
        buffer.grow(length);
        string->resolveToBuffer(buffer.mutableSpan());
        return StringView { buffer.span() };
    };
    return viewOf(a, buffers[0]) == viewOf(b, buffers[1]);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringTrimmedStart16, size_t, (Instance*, const char16_t* characters, uint32_t length))
{
    uint32_t start = 0;
    while (start < length && isStrWhiteSpace(characters[start]))
        ++start;
    return start;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringTrimmedEnd16, size_t, (Instance*, const char16_t* characters, uint32_t start, uint32_t length))
{
    while (length > start && isStrWhiteSpace(characters[length - 1]))
        --length;
    return length;
}

// The position may be any int32: the search starts at the last place with room, or at the position if that comes first, and no earlier than at the first place.

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFindCharacter8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position))
{
    if (!length)
        return notFound;
    for (size_t i = std::min<size_t>(std::max(position, 0), length - 1);; --i) {
        if (characters[i] == character)
            return i;
        if (!i)
            return notFound;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFind8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, int32_t position))
{
    if (otherLength > length)
        return notFound;
    size_t start = std::min<size_t>(std::max(position, 0), length - otherLength);
    if (!otherLength)
        return start;
    Latin1Character first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    for (size_t i = start;; --i) {
        if (characters[i] == first && equal(characters + i + 1, rest))
            return i;
        if (!i)
            return notFound;
    }
}

// As stringProtoFuncLastIndexOf(). Without a position: the greatest.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLastIndexOf, size_t, (Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* string = uncheckedDowncast<JSString>(cell);
    JSString* other = uncheckedDowncast<JSString>(otherCell);
    unsigned length = string->length();
    unsigned otherLength = other->length();
    if (length < otherLength)
        OPERATION_RETURN(scope, none);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    auto otherCharacters = other->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, otherCharacters[0], start))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    size_t found;
    if (otherLength == 1)
        found = characters->reverseFind(otherCharacters[0], start);
    else if (!start)
        found = characters->startsWith(otherCharacters) ? 0 : notFound;
    else
        found = characters->reverseFind(otherCharacters, start);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

enum class LetterCase : uint8_t { Lower, Upper };

// ASCII text is converted here, in two loops without a branch. Anything else is left to WTF.
template<LetterCase letterCase>
static ALWAYS_INLINE JSString* convertCase(JSGlobalObject* globalObject, VM& vm, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (view->is8Bit()) {
            const Latin1Character* characters = view->span8().data();
            size_t length = view->length();
            unsigned isNotASCII = 0;
            unsigned changes = 0;
            for (size_t i = 0; i < length; ++i) {
                isNotASCII |= characters[i] & 0x80;
                changes |= letterCase == LetterCase::Lower ? isASCIIUpper(characters[i]) : isASCIILower(characters[i]);
            }
            if (!isNotASCII) {
                if (!changes)
                    return string;
                std::span<Latin1Character> buffer;
                Ref impl = StringImpl::createUninitialized(length, buffer);
                Latin1Character* converted = buffer.data();
                for (size_t i = 0; i < length; ++i)
                    converted[i] = letterCase == LetterCase::Lower ? toASCIILower(characters[i]) : toASCIIUpper(characters[i]);
                return jsString(vm, String { WTF::move(impl) });
            }
        }
    }
    auto characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    String converted = letterCase == LetterCase::Lower ? characters->convertToLowercaseWithoutLocale() : characters->convertToUppercaseWithoutLocale();
    if (converted.impl() == characters->impl())
        return string;
    return jsString(vm, WTF::move(converted));
}

template<typename CharacterType>
static void fillWithPattern(std::span<CharacterType> buffer, StringView pattern)
{
    if (pattern.length() == 1) {
        std::ranges::fill(buffer, static_cast<CharacterType>(pattern[0]));
        return;
    }
    size_t filled = std::min<size_t>(pattern.length(), buffer.size());
    pattern.left(filled).getCharacters(buffer.first(filled));
    while (filled < buffer.size()) {
        size_t more = std::min(filled, buffer.size() - filled);
        memcpySpan(buffer.subspan(filled, more), buffer.first(more));
        filled += more;
    }
}

// The string, if any, with the pattern over and over in front of it or behind it, up to the length. In one piece.
template<typename CharacterType>
static JSString* tryMakePadded(VM& vm, StringView string, StringView pattern, unsigned length, bool isAtStart)
{
    std::span<CharacterType> buffer;
    auto impl = StringImpl::tryCreateUninitialized(length, buffer);
    if (!impl) [[unlikely]]
        return nullptr;
    if (!string.isEmpty())
        string.getCharacters(isAtStart ? buffer.last(string.length()) : buffer.first(string.length()));
    fillWithPattern(isAtStart ? buffer.first(length - string.length()) : buffer.last(length - string.length()), pattern);
    return jsString(vm, impl.releaseNonNull());
}

// Of stringProtoFuncRepeat(): the length times the count is at most Typed::maxLengthOfStringMadeInOnePiece.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringRepeat, JSCell*, (Instance* instance, JSCell* cell, uint32_t count))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(cell);
    unsigned length = string->length() * count;
    if (!length)
        OPERATION_RETURN(scope, jsEmptyString(vm));
    if (count == 1)
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSString* result = view->is8Bit() ? tryMakePadded<Latin1Character>(vm, { }, view, length, false) : tryMakePadded<char16_t>(vm, { }, view, length, false);
    if (!result) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, result);
}

// Of padString(): the length is above the string's and at most Typed::maxLengthOfStringMadeInOnePiece.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringPad, JSCell*, (Instance* instance, JSCell* cell, uint32_t length, JSCell* filler, uint32_t isAtStart))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(cell);
    if (!uncheckedDowncast<JSString>(filler)->length())
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto pattern = uncheckedDowncast<JSString>(filler)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSString* result = view->is8Bit() && pattern->is8Bit() ? tryMakePadded<Latin1Character>(vm, view, pattern, length, isAtStart) : tryMakePadded<char16_t>(vm, view, pattern, length, isAtStart);
    if (!result) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, result);
}

// As operationStringLocaleCompare(). -1, 0 or 1.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLocaleCompare, size_t, (Instance* instance, JSCell* cell, JSCell* other))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto string = uncheckedDowncast<JSString>(cell)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto that = uncheckedDowncast<JSString>(other)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (globalObject->canDoASCIIUCADUCETLocaleCompare() && string->is8Bit() && that->is8Bit()) {
        if (auto result = compareASCIIWithUCADUCET(string->span8(), that->span8()))
            OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(*result)));
    }
    auto* collator = globalObject->defaultCollator();
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(collator->compareStrings(globalObject, string, that))));
}

// As stringProtoFuncToLowerCase().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringToLowerCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, convertCase<LetterCase::Lower>(globalObject, vm, uncheckedDowncast<JSString>(cell)));
}

// As stringProtoFuncToUpperCase().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringToUpperCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, convertCase<LetterCase::Upper>(globalObject, vm, uncheckedDowncast<JSString>(cell)));
}

namespace Typed {

using namespace B3;

template<> struct ParameterOfOperation<const Latin1Character*> { using Type = Pointer; };
template<> struct ParameterOfOperation<const char16_t*> { using Type = Pointer; };

// The family aot/builtins/string.def.

// What repeat(), padStart() and padEnd() make longer than this the engine makes, as a rope: its maxResultLengthForFlatRepeat.
static constexpr unsigned maxLengthOfStringMadeInOnePiece = 1024;

static bool areStrings(std::span<const Type> types)
{
    for (auto& type : types) {
        if (!type.isString())
            return false;
    }
    return true;
}

std::optional<Writes> writesOfInlineString(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::StringLength:
        return Writes::Nothing;
    // Looking at the characters flattens a rope.
    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
        return writesIf(arguments.size() == 1 && areNumbers(arguments), Writes::RuntimeState);
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return writesIf(!arguments.empty() && areNumbers(arguments), Writes::RuntimeState);
    case InlineBuiltin::StringRepeat:
        return writesIf(arguments.size() == 1 && arguments[0].isNumber(), Writes::RuntimeState);
    case InlineBuiltin::StringPadStart:
    case InlineBuiltin::StringPadEnd:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && arguments[0].isNumber() && (arguments.size() == 1 || arguments[1].isString()), Writes::RuntimeState);
    case InlineBuiltin::StringLocaleCompare:
        return writesIf(arguments.size() == 1 && arguments[0].isString(), Writes::RuntimeState);
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
        return writesIf(!arguments.empty() && arguments[0].isString() && (arguments.size() == 1 || arguments[1].isNumber()), Writes::RuntimeState);
    // What is not a number stands for the end there, not for the start.
    case InlineBuiltin::StringLastIndexOf:
        return writesIf(!arguments.empty() && arguments[0].isString() && (arguments.size() == 1 || (arguments[1].isInteger() && arguments[1].integers.isWithin(Range::int32()))), Writes::RuntimeState);
    case InlineBuiltin::StringToLowerCase:
    case InlineBuiltin::StringToUpperCase:
    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd:
    case InlineBuiltin::StringIsWellFormed:
    case InlineBuiltin::StringToWellFormed:
    case InlineBuiltin::StringNormalize:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    // A rope has room for three.
    case InlineBuiltin::StringConcat:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && areStrings(arguments), Writes::RuntimeState);
    case InlineBuiltin::StringFromCharCode:
        return writesIf(arguments.size() == 1 && arguments[0].isNumber(), Writes::Nothing);
    case InlineBuiltin::StringFromCodePoint:
        return writesIf(arguments.size() == 1 && arguments[0].isInteger(), Writes::RuntimeState);
    // A symbol is described, and an object has a say in what becomes of it.
    case InlineBuiltin::CallStringConstructor:
        return writesIf(arguments.size() == 1 && !arguments[0].hasObjects() && !(arguments[0].primitives & Primitive::Symbol), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// Ends the block. WhiteSpace and LineTerminator among the first 256 code points.
void Lowering::branchOnLatin1WhiteSpace(Word32 character, LBasicBlock isWhiteSpace, LBasicBlock isNot)
{
    LBasicBlock isAtMostSpace = m_out.newBlock();
    LBasicBlock isAboveSpace = m_out.newBlock();
    LBasicBlock isBelowSpace = m_out.newBlock();
    m_out.branch(belowOrEqual(character, word32(' ')).value(), unsure(isAtMostSpace), unsure(isAboveSpace));
    m_out.appendTo(isAboveSpace);
    m_out.branch(equal(character, word32(0xa0)).value(), rarely(isWhiteSpace), usually(isNot));
    m_out.appendTo(isAtMostSpace);
    m_out.branch(equal(character, word32(' ')).value(), usually(isWhiteSpace), rarely(isBelowSpace));
    m_out.appendTo(isBelowSpace);
    m_out.branch(belowOrEqual(add(character, word32(-'\t')), word32('\r' - '\t')).value(), unsure(isWhiteSpace), unsure(isNot));
}

// What is left without the white space at the ends that are asked for. 8-bit characters are looked at in place.
std::pair<Integer, Integer> Lowering::trimmedRange(const Characters& characters, bool trimsStart, bool trimsEnd)
{
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), unsure(narrow), unsure(wide));
    m_out.appendTo(wide);
    Integer wideFrom = trimsStart ? zeroExtended(AOT_TYPED_OPERATION(StringTrimmedStart16, characters.data, low32(characters.length))) : integer(0);
    Integer wideTo = trimsEnd ? zeroExtended(AOT_TYPED_OPERATION(StringTrimmedEnd16, characters.data, low32(wideFrom), low32(characters.length))) : characters.length;
    ValueFromBlock fromOfWide = m_out.anchor(wideFrom.value());
    ValueFromBlock toOfWide = m_out.anchor(wideTo.value());
    m_out.jump(continuation);
    m_out.appendTo(narrow);
    auto characterAt = [&](LValue index) {
        return Word32(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(characters.data.value(), index))), IsFixed));
    };
    Integer from = integer(0);
    if (trimsStart) {
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock look = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        ValueFromBlock first = m_out.anchor(m_out.int64Zero);
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue index = m_out.phi(Int64, first);
        m_out.branch(m_out.below(index, characters.length.value()), usually(look), rarely(done));
        m_out.appendTo(look);
        branchOnLatin1WhiteSpace(characterAt(index), next, done);
        m_out.appendTo(next);
        m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.constInt64(1))));
        m_out.jump(loop);
        m_out.appendTo(done);
        from = Integer(index);
    }
    Integer to = characters.length;
    if (trimsEnd) {
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock look = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        ValueFromBlock last = m_out.anchor(characters.length.value());
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue end = m_out.phi(Int64, last);
        m_out.branch(m_out.above(end, from.value()), usually(look), rarely(done));
        m_out.appendTo(look);
        LValue before = m_out.sub(end, m_out.constInt64(1));
        branchOnLatin1WhiteSpace(characterAt(before), next, done);
        m_out.appendTo(next);
        m_out.addIncomingToPhi(end, m_out.anchor(before));
        m_out.jump(loop);
        m_out.appendTo(done);
        to = Integer(end);
    }
    ValueFromBlock fromOfNarrow = m_out.anchor(from.value());
    ValueFromBlock toOfNarrow = m_out.anchor(to.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    from = Integer(m_out.phi(Int64, fromOfNarrow, fromOfWide));
    to = Integer(m_out.phi(Int64, toOfNarrow, toOfWide));
    ensureStillAliveHere(characters.owner);
    return { from, to };
}

// Whether the characters of the string from the place on are those of the other, which has room there.
Word32 Lowering::hasStringAt(const Characters& characters, Integer place, const Characters& other)
{
    return either<Word32>(hasBits(bitAnd(characters.flags, other.flags), StringImpl::flagIs8Bit()), [&] {
        return areBytesEqual(offsetBy(characters.data, place, 0), other.data, other.length);
    }, [&] {
        return AOT_TYPED_OPERATION(StringHasAt, characters.owner, other.owner, low32(place));
    });
}

// Whether the other string is at the place: by default at the start, or at the end.
Word32 Lowering::lowerStartsOrEndsWith(bool isAtEnd, const StringReceiver& receiver, ValueID other, std::optional<ValueID> position)
{
    auto text = constantASCIIText(other);
    if (text && text->empty())
        return boolean(true);
    // The way that is taken stands at the site, as in isTextOfLengthByStub(). No cell for the text: see Stub::StringStartsWithText.
    if (text && !position && !m_isInLoop) {
        bool comparesHere = text->size() <= maxLengthOfTextComparedAtEverySite;
        LBasicBlock isTooShort = m_out.newBlock();
        LBasicBlock isLongEnough = m_out.newBlock();
        LBasicBlock byStub = comparesHere ? newColdBlock() : m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        if (comparesHere) {
            Characters characters = charactersOf(receiver, byStub);
            orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), byStub);
            m_out.branch(below(characters.length, integer(text->size())).value(), unsure(isTooShort), unsure(isLongEnough));
            m_out.appendTo(isLongEnough);
            Word32 isSame = hasText(isAtEnd ? offsetBy(characters.data, sub(characters.length, integer(text->size())), 0) : characters.data, *text);
            ensureStillAliveHere(characters.owner);
            join.add(isSame);
        } else {
            m_out.branch(below(stringLength(receiver), integer(text->size())).value(), unsure(isTooShort), unsure(isLongEnough));
            m_out.appendTo(isLongEnough);
            m_out.jump(byStub);
        }
        m_out.appendTo(isTooShort);
        join.add(boolean(false));
        m_out.appendTo(byStub);
        LValue number = m_out.constInt64(m_function.at(originOf(other)).string().index());
        auto effects = isAtEnd ? effectsOfOperation<operationAOTTypedStringEndsWithText>() : effectsOfOperation<operationAOTTypedStringStartsWithText>();
        join.add(Word32(m_out.castToInt32(callAllocationStub(isAtEnd ? Stub::StringEndsWithText : Stub::StringStartsWithText, { { number, stubTemporaryGPRs[0] }, { receiver.string.value(), stubTemporaryGPRs[2] } }, effects, true))));
        Word32 has = join.finish().as<Word32>();
        ensureStillAliveHere(receiver.string);
        return has;
    }
    LBasicBlock hasRoom = m_out.newBlock();
    LBasicBlock hasNoRoom = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    // Goes on where the other string has room at the place, which it yields.
    auto findPlace = [&](Integer length, Integer otherLength) {
        Integer place = isAtEnd ? length : integer(0);
        if (position)
            place = placeInString(*position, length, false);
        if (isAtEnd)
            place = sub(place, otherLength);
        m_out.branch((isAtEnd ? greaterThanOrEqual(place, integer(0)) : lessThanOrEqual(add(place, otherLength), length)).value(), unsure(hasRoom), unsure(hasNoRoom));
        m_out.appendTo(hasNoRoom);
        join.add(boolean(false));
        m_out.appendTo(hasRoom);
        return place;
    };
    if (text) {
        Integer place = findPlace(stringLength(receiver), integer(text->size()));
        join.add(hasTextAt(receiver, place, other, *text));
        return join.finish().as<Word32>();
    }
    Characters characters = charactersOf(receiver);
    Characters otherCharacters = charactersOf(stringReceiver(other));
    Integer place = findPlace(characters.length, otherCharacters.length);
    join.add(hasStringAt(characters, place, otherCharacters));
    Word32 has = join.finish().as<Word32>();
    ensureStillAliveHere(characters.owner);
    ensureStillAliveHere(otherCharacters.owner);
    return has;
}

// Where it is found, or -1.
Word32 Lowering::lowerStringSearch(InlineBuiltin which, const StringReceiver& receiver, const Arguments& arguments)
{
    bool isReverse = which == InlineBuiltin::StringLastIndexOf;
    ValueID other = arguments[0];
    // Backwards from the start there is one place to look at.
    if (isReverse && arguments.size() > 1 && typeOf(arguments[1]).integers.most <= 0)
        return select(lowerStartsOrEndsWith(false, receiver, other, std::nullopt), word32(0), word32(-1));
    Word32 position = word32(isReverse ? INT32_MAX : 0);
    if (arguments.size() > 1)
        position = low32(isReverse ? in<Integer>(arguments[1]) : placeInString(arguments[1], stringLength(receiver), false));

    // As below, but for the text: it goes by its number, to the search and to the operation for all else: no cell for it is fetched or opened.
    if (auto text = constantASCIIText(other); text && !text->empty() && !m_isInLoop) {
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        Word32 number = word32(m_function.at(originOf(other)).string().index());
        Characters characters = charactersOf(receiver, otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        Pointer data = characters.data;
        Word32 length = low32(characters.length);
        if (text->size() == 1) {
            Word32 character = word32((*text)[0]);
            join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindCharacter8, data, length, character, position) : AOT_TYPED_OPERATION(StringFindCharacter8, data, length, character, position));
        } else
            join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindText8, data, length, number, position) : AOT_TYPED_OPERATION(StringFindText8, data, length, number, position));
        m_out.appendTo(otherwise);
        join.add(isReverse ? AOT_TYPED_OPERATION(StringLastIndexOfText, receiver.string, number, position) : AOT_TYPED_OPERATION(StringIndexOfText, receiver.string, number, position));
        Word32 found = join.finish().as<Word32>();
        ensureStillAliveHere(receiver.string);
        return found;
    }
    // A long rope is walked for one character, and not made flat.
    LBasicBlock generally = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    Characters characters = charactersOf(receiver, generally);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), generally);
    Word32 length = low32(characters.length);
    if (auto text = constantASCIIText(other); text && text->size() == 1) {
        Word32 character = word32((*text)[0]);
        join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindCharacter8, characters.data, length, character, position) : AOT_TYPED_OPERATION(StringFindCharacter8, characters.data, length, character, position));
    } else {
        Characters otherCharacters = charactersOf(stringReceiver(other), generally);
        orElse(hasBits(otherCharacters.flags, StringImpl::flagIs8Bit()), generally);
        Word32 otherLength = low32(otherCharacters.length);
        // So is there where no more is left than the other string is long.
        if (!isReverse && arguments.size() > 1) {
            LBasicBlock hasOnePlaceAtMost = m_out.newBlock();
            LBasicBlock hasOnePlace = m_out.newBlock();
            LBasicBlock hasNoPlace = m_out.newBlock();
            LBasicBlock hasMorePlaces = m_out.newBlock();
            Word32 left(m_out.sub(length.value(), position.value()));
            m_out.branch(belowOrEqual(left, otherLength).value(), unsure(hasOnePlaceAtMost), unsure(hasMorePlaces));
            m_out.appendTo(hasOnePlaceAtMost);
            m_out.branch(equal(left, otherLength).value(), unsure(hasOnePlace), unsure(hasNoPlace));
            m_out.appendTo(hasOnePlace);
            Word32 isThere = areBytesEqual(offsetBy(characters.data, zeroExtended(position), 0), otherCharacters.data, otherCharacters.length);
            ensureStillAliveHere(otherCharacters.owner);
            join.add(select(isThere, position, word32(-1)));
            m_out.appendTo(hasNoPlace);
            join.add(word32(-1));
            m_out.appendTo(hasMorePlaces);
        }
        Word32 found = isReverse ? AOT_TYPED_OPERATION(StringReverseFind8, characters.data, length, otherCharacters.data, otherLength, position) : AOT_TYPED_OPERATION(StringFind8, characters.data, length, otherCharacters.data, otherLength, position);
        ensureStillAliveHere(otherCharacters.owner);
        join.add(found);
    }
    m_out.appendTo(generally);
    CellPointer otherString = in<CellPointer>(other);
    join.add(isReverse ? AOT_TYPED_OPERATION(StringLastIndexOf, receiver.string, otherString, position) : AOT_TYPED_OPERATION(StringIndexOf, receiver.string, otherString, position));
    Word32 found = join.finish().as<Word32>();
    ensureStillAliveHere(receiver.string);
    return found;
}

std::optional<Operand> Lowering::lowerInlineString(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    Operand yielded = lowerStringBuiltin(which, result, value);
    // What the table says is a string is neither `undefined` nor `null`, whatever the type of the value admits. Not where this is one arm of a test on what is expected.
    if (&value == &m_function.at(result) && m_builtins.at(value.builtin()).result.kinds == BuiltinResult::String && !m_builtins.at(value.builtin()).result.arguments)
        m_passedNilTestIn.add(result.index(), Vector<BlockID, 1> { }).iterator->value.append(m_currentBlockID);
    return yielded;
}

ValueID Lowering::stringDefinedBy(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() != CastReason::Coerce)
        id = m_function.at(id).child(0);
    return id;
}

std::optional<Integer> Lowering::lengthReadBefore(ValueID string) const
{
    auto found = m_lengthsOfStrings.find(stringDefinedBy(string).index());
    if (found == m_lengthsOfStrings.end())
        return std::nullopt;
    for (auto& read : found->value) {
        if (dominates(read.block, m_currentBlockID))
            return Integer(read.length);
    }
    return std::nullopt;
}

void Lowering::didReadLength(ValueID string, Integer length)
{
    // What is defined before the entry of a handler is not there behind it.
    if (!m_catchSpills.isEmpty())
        return;
    m_lengthsOfStrings.add(stringDefinedBy(string).index(), Vector<LengthOfString, 1> { }).iterator->value.append(LengthOfString { m_currentBlockID, length.value() });
}

void Lowering::didMakeStringOfCodeUnit(ValueID id, Operand string, const Vector<ValueFromBlock, 2>& codeUnits)
{
    m_codeUnitsOfStrings.set(id.index(), CodeUnitOfString { string.value, m_out.phi(Int64, codeUnits) });
}

std::optional<Integer> Lowering::codeUnitOfString(ValueID id)
{
    if (m_codeUnitsOfStrings.isEmpty())
        return std::nullopt;
    id = stringDefinedBy(id);
    auto found = m_codeUnitsOfStrings.find(id.index());
    // Where a test has shown more of the value it stands for something else.
    if (found == m_codeUnitsOfStrings.end() || m_values[id.index()].value != found->value.string)
        return std::nullopt;
    return Integer(found->value.codeUnit);
}

Operand Lowering::lowerStringBuiltin(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& stringType = m_program.at(m_program.types.string);
    // Not in an arm of a test on what is expected: what is defined there is not there behind it.
    bool isOnTrunk = &value == &m_function.at(result);
    auto generally = [&] {
        return asCell(callBuiltinFunction(value, effectsOfBuiltinCall(value)));
    };
    auto isIntegerWithin = [&](ValueID id, Range range) {
        return typeOf(id).isInteger() && typeOf(id).integers.isWithin(range);
    };

    switch (which) {
    case InlineBuiltin::StringFromCharCode:
        if (isIntegerWithin(arguments[0], { 0, maxSingleCharacterString }))
            return ensure(CellPointer(singleCharacterString(toInt32(arguments[0]).value())), stringType, type);
        return ensure(stringOfCodeUnit(bitAnd(toInt32(arguments[0]), word32(UINT16_MAX))), stringType, type);
    case InlineBuiltin::StringFromCodePoint: {
        Integer codePoint = in<Integer>(arguments[0]);
        if (isIntegerWithin(arguments[0], { 0, maxSingleCharacterString }))
            return ensure(CellPointer(singleCharacterString(low32(codePoint).value())), stringType, type);
        return ensure(either<CellPointer>(belowOrEqual(codePoint, integer(maxSingleCharacterString)), [&] {
            return CellPointer(singleCharacterString(low32(codePoint).value()));
        }, generally), stringType, type);
    }
    case InlineBuiltin::CallStringConstructor:
        return ensure(toStringCell(arguments[0]), stringType, type);
    default:
        break;
    }

    StringReceiver receiver = stringReceiver(nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName)));
    receiver.id = value.child(0);
    switch (which) {
    case InlineBuiltin::StringLength: {
        if (auto length = lengthReadBefore(value.child(0)))
            return ensure(*length, Type::integer(Range::length()), type);
        Integer length = stringLength(receiver);
        if (isOnTrunk)
            didReadLength(value.child(0), length);
        return ensure(length, Type::integer(Range::length()), type);
    }

    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt: {
        Rep rep = repOf(type);
        LBasicBlock outside = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        Characters characters = charactersOf(receiver);
        if (auto length = lengthReadBefore(value.child(0)))
            characters.length = *length;
        else if (isOnTrunk)
            didReadLength(value.child(0), characters.length);
        Integer index = positionOf(arguments[0]);
        if (which == InlineBuiltin::StringAt && !isIntegerWithin(arguments[0], Range::length()))
            index = select(lessThan(index, integer(0)), add(index, characters.length), index);
        orElse(below(index, characters.length), outside);
        bool keepsCodeUnit = ((which == InlineBuiltin::StringCharCodeAt && rep == Rep::Double) || (which == InlineBuiltin::StringCodePointAt && rep == Rep::OptionalInt64)) && isOnTrunk && m_catchSpills.isEmpty();
        bool keepsCodeUnitOfString = (which == InlineBuiltin::StringCharAt || which == InlineBuiltin::StringAt) && isOnTrunk && keepsCodeUnitsOfStrings();
        Vector<ValueFromBlock, 2> codeUnits;
        if (which == InlineBuiltin::StringCharCodeAt) {
            Integer codeUnit = zeroExtended(codeUnitAt(characters, index));
            if (keepsCodeUnit)
                codeUnits.append(m_out.anchor(codeUnit.value()));
            join.add(ensure(codeUnit, Type::integer({ 0, UINT16_MAX }), type));
        } else if (which == InlineBuiltin::StringCodePointAt) {
            Integer codePoint = zeroExtended(codePointAt(characters, index));
            if (keepsCodeUnit)
                codeUnits.append(m_out.anchor(codePoint.value()));
            join.add(ensure(codePoint, Type::integer({ 0, 0x10ffff }), type));
        } else {
            Word32 codeUnit;
            CellPointer string = singleCharacterStringAt(characters, index, keepsCodeUnitOfString ? &codeUnit : nullptr);
            if (keepsCodeUnitOfString)
                codeUnits.append(m_out.anchor(zeroExtended(codeUnit).value()));
            join.add(ensure(string, stringType, type));
        }

        m_out.appendTo(outside);
        if (which == InlineBuiltin::StringCharCodeAt) {
            if (keepsCodeUnit)
                codeUnits.append(m_out.anchor(m_out.constInt64(INT64_MIN)));
            join.add(ensure(number(PNaN), m_program.at(m_program.types.number), type));
        } else if (which == InlineBuiltin::StringCharAt) {
            if (keepsCodeUnitOfString)
                codeUnits.append(m_out.anchor(m_out.constInt64(codeUnitOfEmptyString)));
            join.add(ensure(CellPointer(emptyString()), stringType, type));
        } else {
            static_assert(codeUnitOfUndefined == INT64_MIN);
            if (keepsCodeUnitOfString || keepsCodeUnit)
                codeUnits.append(m_out.anchor(m_out.constInt64(codeUnitOfUndefined)));
            join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        }
        Operand joined = join.finish();
        if (keepsCodeUnitOfString)
            didMakeStringOfCodeUnit(result, joined, codeUnits);
        // See didDefineNumber(), which finds this there.
        if (keepsCodeUnit) {
            m_integersOfNumbers.add(result.index(), IntegerOfNumber { joined.value, m_out.phi(Int64, codeUnits), 0 });
        }
        return joined;
    }

    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr: {
        Integer length;
        if (auto known = lengthReadBefore(value.child(0)))
            length = *known;
        else {
            length = stringLength(receiver);
            if (isOnTrunk)
                didReadLength(value.child(0), length);
        }
        Integer from = placeInString(arguments[0], length, which != InlineBuiltin::StringSubstring);
        Integer to = length;
        if (arguments.size() > 1) {
            to = placeInString(arguments[1], length, which == InlineBuiltin::StringSlice);
            if (which == InlineBuiltin::StringSubstr) {
                Integer end = add(from, to);
                to = select(greaterThan(end, length), length, end);
            } else if (which == InlineBuiltin::StringSubstring) {
                Word32 isReversed = greaterThan(from, to);
                Integer least = select(isReversed, to, from);
                to = select(isReversed, from, to);
                from = least;
            }
        }
        return ensure(partOfString(receiver, length, from, to), stringType, type);
    }

    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return yieldBoolean(lowerStartsOrEndsWith(which == InlineBuiltin::StringEndsWith, receiver, arguments[0], arguments.size() > 1 ? std::optional { arguments[1] } : std::nullopt), type);

    case InlineBuiltin::StringIncludes:
        return yieldBoolean(notEqual(lowerStringSearch(which, receiver, arguments), word32(-1)), type);
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
        return ensure(signExtended(lowerStringSearch(which, receiver, arguments)), Type::integer({ -1, INT32_MAX }), type);

    // What is negative, infinite or too large is not within the limit as an unsigned integer, whichever integer the machine makes of it; what is no number is 0.
    case InlineBuiltin::StringRepeat: {
        Integer count = positionOf(arguments[0]);
        Integer limit = integer(maxLengthOfStringMadeInOnePiece);
        return ensure(either<CellPointer>(bitAnd(belowOrEqual(count, limit), belowOrEqual(mul(count, stringLength(receiver)), limit)), [&] {
            return AOT_TYPED_OPERATION(StringRepeat, receiver.string, low32(count));
        }, generally), stringType, type);
    }
    case InlineBuiltin::StringPadStart:
    case InlineBuiltin::StringPadEnd: {
        Integer length = positionOf(arguments[0]);
        CellPointer filler = arguments.size() > 1 ? in<CellPointer>(arguments[1]) : CellPointer(singleCharacterString(m_out.constInt32(' ')));
        return ensure(either<CellPointer>(belowOrEqual(length, integer(maxLengthOfStringMadeInOnePiece)), [&] {
            return either<CellPointer>(lessThanOrEqual(length, stringLength(receiver)), [&] {
                return receiver.string;
            }, [&] {
                return AOT_TYPED_OPERATION(StringPad, receiver.string, low32(length), filler, word32(which == InlineBuiltin::StringPadStart));
            });
        }, generally), stringType, type);
    }

    case InlineBuiltin::StringLocaleCompare:
        return ensure(signExtended(AOT_TYPED_OPERATION(StringLocaleCompare, receiver.string, in<CellPointer>(arguments[0]))), Type::integer({ -1, 1 }), type);

    case InlineBuiltin::StringToLowerCase:
        return ensure(AOT_TYPED_OPERATION(StringToLowerCase, receiver.string), stringType, type);
    case InlineBuiltin::StringToUpperCase:
        return ensure(AOT_TYPED_OPERATION(StringToUpperCase, receiver.string), stringType, type);

    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd: {
        Characters characters = charactersOf(receiver);
        auto [from, to] = trimmedRange(characters, which != InlineBuiltin::StringTrimEnd, which != InlineBuiltin::StringTrimStart);
        return ensure(partOfString(receiver, characters.length, from, to), stringType, type);
    }

    // No surrogate has 8 bits, and every text of the first 256 code points is in Normalization Form C.
    case InlineBuiltin::StringIsWellFormed:
        return yieldBoolean(either<Word32>(hasBits(stringPartOf(receiver.string).flags, StringImpl::flagIs8Bit()), [&] {
            return boolean(true);
        }, [&] {
            return unboxedBoolean(callBuiltinFunction(value, effectsOfBuiltinCall(value)));
        }), type);
    case InlineBuiltin::StringToWellFormed:
    case InlineBuiltin::StringNormalize:
        return ensure(either<CellPointer>(hasBits(stringPartOf(receiver.string).flags, StringImpl::flagIs8Bit()), [&] {
            return receiver.string;
        }, generally), stringType, type);

    case InlineBuiltin::StringConcat: {
        StringPart first = stringPartOf(receiver.string);
        StringPart second = stringPartOf(arguments[0]);
        if (arguments.size() == 1)
            return ensure(concatenate(first, second).string, stringType, type);
        StringPart third = stringPartOf(arguments[1]);
        return ensure(concatenate(first, second, &third).string, stringType, type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
