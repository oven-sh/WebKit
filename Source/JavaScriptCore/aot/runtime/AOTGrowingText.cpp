/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGrowingText.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "LocalAllocatorInlines.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A buffer is a flat string that no program gets to see, and that a Structure of its own tells from every other string.
// Its first characters are no text: they hold the index at which the text in the buffer ends. From there to the end of the string is room, which nothing has written yet.
using EndOfText = uint32_t;
static_assert(!offsetOfEndOfTextInCharactersOfBuffer);

static ALWAYS_INLINE uint32_t lengthOfBufferHeader(bool is8Bit)
{
    return is8Bit ? sizeof(EndOfText) / sizeof(Latin1Character) : sizeof(EndOfText) / sizeof(char16_t);
}

static ALWAYS_INLINE EndOfText& endOfTextIn(StringImpl& characters)
{
    const void* first = characters.is8Bit() ? static_cast<const void*>(characters.span8().data()) : characters.span16().data();
    return *static_cast<EndOfText*>(const_cast<void*>(first));
}

// The words of a rope behind its header, as the stubs read and write them: AOTStubsStringHelpers.cpp.
using CompactFibers = JSRopeString::CompactFibers;
static_assert(sizeof(JSRopeString) == 4 * sizeof(uint64_t) && JSRopeString::offsetOfFiber0() == sizeof(uint64_t) && JSRopeString::offsetOfLength() == 2 * sizeof(uint64_t) && JSRopeString::offsetOfFiber2() == 3 * sizeof(uint64_t));
static_assert(CompactFibers::shiftOfFiber1LowerInLengthWord == 32 && CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16);

static ALWAYS_INLINE uint64_t wordOfRope(const JSString* rope, ptrdiff_t offset)
{
    return WTF::unalignedLoad<uint64_t>(std::bit_cast<const uint8_t*>(rope) + offset);
}

static ALWAYS_INLINE JSString* firstPartOf(const JSString* rope) { return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfFiber0()) & JSRopeString::stringMask); }
static ALWAYS_INLINE JSString* secondPartOf(const JSString* rope)
{
    constexpr uint64_t upperBitsOfSecondPart = (1ull << CompactFibers::shiftOfFiber2InUpperWord) - 1;
    return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfLength()) >> CompactFibers::shiftOfFiber1LowerInLengthWord | (wordOfRope(rope, JSRopeString::offsetOfFiber2()) & upperBitsOfSecondPart) << CompactFibers::widthOfFiber1LowerInLengthWord);
}
static ALWAYS_INLINE JSString* thirdPartOf(const JSString* rope) { return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfFiber2()) >> CompactFibers::shiftOfFiber2InUpperWord); }

static ALWAYS_INLINE JSString* createSubstringOfBuffer(Instance& instance, VM& vm, JSString* buffer, uint32_t offset, uint32_t length, bool is8Bit)
{
    auto* allocator = static_cast<LocalAllocator*>(instance.prefix().ropeStringAllocator);
    auto* words = static_cast<uint64_t*>(allocator->allocate(vm.heap, sizeof(JSRopeString), nullptr, AllocationFailureMode::Assert));
    uint64_t base = std::bit_cast<uint64_t>(buffer);
    words[0] = instance.prefix().stringHeader;
    words[1] = JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | (is8Bit ? JSRopeString::is8BitInPointer : 0);
    words[2] = length | base << CompactFibers::shiftOfFiber1LowerInLengthWord;
    words[3] = base >> CompactFibers::widthOfFiber1LowerInLengthWord | static_cast<uint64_t>(offset) << CompactFibers::shiftOfFiber2InUpperWord;
    vm.mutatorFence();
    return std::bit_cast<JSString*>(words);
}

// Most pieces are a few characters, and a call of memcpy costs more than they do.
static ALWAYS_INLINE void copyCharacters(Latin1Character* destination, const Latin1Character* source, uint32_t length)
{
    auto copy = [&]<typename Word>(uint32_t at) {
        WTF::unalignedStore<Word>(destination + at, WTF::unalignedLoad<Word>(source + at));
    };
    if (length > 2 * sizeof(uint64_t)) {
        memcpy(destination, source, length);
        return;
    }
    if (length >= sizeof(uint64_t)) {
        copy.template operator()<uint64_t>(0);
        copy.template operator()<uint64_t>(length - sizeof(uint64_t));
    } else if (length >= sizeof(uint32_t)) {
        copy.template operator()<uint32_t>(0);
        copy.template operator()<uint32_t>(length - sizeof(uint32_t));
    } else if (length) {
        destination[0] = source[0];
        destination[length / 2] = source[length / 2];
        destination[length - 1] = source[length - 1];
    }
}

static ALWAYS_INLINE void copyCharacters(char16_t* destination, const StringImpl& source)
{
    StringView(source).getCharacters(std::span { destination, source.length() });
}

static ALWAYS_INLINE void copyCharacters(Latin1Character* destination, const StringImpl& source)
{
    // A string that becomes a key takes the characters of the name, whatever their width: JSRopeString::tryResolveFlatLatin1Parts().
    if (source.is8Bit()) [[likely]] {
        copyCharacters(destination, source.span8().data(), source.length());
        return;
    }
    StringView(source).getCharacters(std::span { destination, source.length() });
}

// `text += a + b + c`: the piece is a rope of ropes, and its leaves are a few flat strings.
template<typename CharacterType>
static ALWAYS_INLINE void writeString(CharacterType* destination, JSString* string)
{
    if (const StringImpl* flat = string->tryGetValueImpl()) {
        copyCharacters(destination, *flat);
        return;
    }
    constexpr unsigned maxNumberOfPendingParts = 16;
    JSString* pending[maxNumberOfPendingParts];
    unsigned numberOfPendingParts = 0;
    for (JSString* current = string;; current = pending[--numberOfPendingParts]) {
        for (;;) {
            if (const StringImpl* flat = current->tryGetValueImpl()) [[likely]] {
                copyCharacters(destination, *flat);
                destination += flat->length();
                break;
            }
            if (current->isSubstring() || numberOfPendingParts + 2 > maxNumberOfPendingParts) [[unlikely]] {
                uint32_t length = current->length();
                current->resolveToBuffer(std::span { destination, length });
                destination += length;
                break;
            }
            if (JSString* third = thirdPartOf(current))
                pending[numberOfPendingParts++] = third;
            if (JSString* second = secondPartOf(current))
                pending[numberOfPendingParts++] = second;
            current = firstPartOf(current);
        }
        if (!numberOfPendingParts)
            return;
    }
}

// `text` is `bottom` with pieces added to it, each time as a rope that begins with what was there. What was added is written so that it ends at `end`, from the last piece to the first.
template<typename CharacterType>
static ALWAYS_INLINE void writeWhatWasAddedTo(JSString* bottom, JSString* text, CharacterType* end)
{
    for (JSString* current = text; current != bottom; current = firstPartOf(current)) {
        for (JSString* part : { thirdPartOf(current), secondPartOf(current) }) {
            if (!part)
                continue;
            end -= part->length();
            writeString(end, part);
        }
    }
}

template<typename CharacterType>
static ALWAYS_INLINE CharacterType* charactersOf(StringImpl& buffer)
{
    if constexpr (std::is_same_v<CharacterType, Latin1Character>)
        return const_cast<Latin1Character*>(buffer.span8().data());
    else
        return const_cast<char16_t*>(buffer.span16().data());
}

static JSString* tryCreateTextBuffer(VM& vm, bool is8Bit, uint32_t length)
{
    RefPtr<StringImpl> characters;
    if (is8Bit) {
        std::span<Latin1Character> room;
        characters = StringImpl::tryCreateUninitialized(length, room);
    } else {
        std::span<char16_t> room;
        characters = StringImpl::tryCreateUninitialized(length, room);
    }
    if (!characters)
        return nullptr;
    JSString* buffer = jsNontrivialString(vm, String(characters.releaseNonNull()));
    buffer->setStructure(vm, vm.aotState().runtimeTable->textBufferStructure());
    return buffer;
}

// Null: the rope is no substring of a buffer, or the buffer holds more behind it by now.
static ALWAYS_INLINE JSString* bufferThatEndsWith(Instance& instance, JSString* text)
{
    if (!text->isSubstring())
        return nullptr;
    JSString* buffer = secondPartOf(text);
    if (buffer->structureID().bits() != instance.allocation.textBufferStructureID)
        return nullptr;
    uint32_t offset = static_cast<uint32_t>(std::bit_cast<uintptr_t>(thirdPartOf(text)));
    return offset + text->length() == endOfTextIn(*buffer->tryGetValueImpl()) ? buffer : nullptr;
}

// A buffer of 8-bit characters takes 8-bit strings alone.
template<typename CharacterType>
static ALWAYS_INLINE void writePieces(StringImpl& buffer, uint32_t position, std::span<JSString* const> pieces)
{
    for (JSString* piece : pieces) {
        writeString(charactersOf<CharacterType>(buffer) + position, piece);
        position += piece->length();
    }
    endOfTextIn(buffer) = position;
}

JSString* tryAppendToGrowingText(Instance& instance, JSString* text, std::span<JSString* const> pieces)
{
    ASSERT(text->isRope() && text->length() >= minLengthOfGrowingText);
    VM& vm = *instance.vm;
    uint32_t length = text->length();
    uint64_t lengthOfPieces = 0;
    bool is8Bit = text->is8Bit();
    for (JSString* piece : pieces) {
        lengthOfPieces += piece->length();
        is8Bit &= piece->is8Bit();
    }
    if (!lengthOfPieces)
        return text;

    // The stubs add what they cannot write by themselves as a rope, and come here when they are out of cells. So the text in the buffer may lie below a row of ropes.
    uint32_t numberOfAdditions = 0;
    JSString* bottom = text;
    for (; bottom->isNonSubstringRope(); bottom = firstPartOf(bottom))
        ++numberOfAdditions;

    if (JSString* buffer = bottom->isRope() ? bufferThatEndsWith(instance, bottom) : nullptr) {
        StringImpl& characters = *buffer->tryGetValueImpl();
        uint32_t end = endOfTextIn(characters);
        uint32_t lengthAddedAsRopes = length - bottom->length();
        if (lengthAddedAsRopes + lengthOfPieces <= characters.length() - end && (is8Bit || !characters.is8Bit())) {
            if (characters.is8Bit()) [[likely]] {
                writeWhatWasAddedTo(bottom, text, charactersOf<Latin1Character>(characters) + end + lengthAddedAsRopes);
                writePieces<Latin1Character>(characters, end + lengthAddedAsRopes, pieces);
            } else {
                writeWhatWasAddedTo(bottom, text, charactersOf<char16_t>(characters) + end + lengthAddedAsRopes);
                writePieces<char16_t>(characters, end + lengthAddedAsRopes, pieces);
            }
            return createSubstringOfBuffer(instance, vm, buffer, end - bottom->length(), length + static_cast<uint32_t>(lengthOfPieces), characters.is8Bit());
        }
    } else if (numberOfAdditions < minAdditionsToGrowingText || lengthOfPieces > length)
        return nullptr;

    uint32_t start = lengthOfBufferHeader(is8Bit);
    uint64_t lengthOfAll = length + lengthOfPieces;
    if (start + lengthOfAll > JSString::MaxLength)
        return nullptr;
    JSString* buffer = tryCreateTextBuffer(vm, is8Bit, static_cast<uint32_t>(std::min<uint64_t>(start + 2 * lengthOfAll, JSString::MaxLength)));
    if (!buffer)
        return nullptr;
    StringImpl& characters = *buffer->tryGetValueImpl();
    if (is8Bit) {
        text->resolveToBuffer(std::span { charactersOf<Latin1Character>(characters) + start, length });
        writePieces<Latin1Character>(characters, start + length, pieces);
    } else {
        text->resolveToBuffer(std::span { charactersOf<char16_t>(characters) + start, length });
        writePieces<char16_t>(characters, start + length, pieces);
    }
    return createSubstringOfBuffer(instance, vm, buffer, start, static_cast<uint32_t>(lengthOfAll), is8Bit);
}

bool canGrowInPlace(Instance& instance, JSString* text)
{
    return text->isRope() && bufferThatEndsWith(instance, text);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
