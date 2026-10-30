// Writes AOTVector.h from wtf/Vector.h: the same class in JSC::AOT for the compiler's own lists. Nobody borrows them, so there is no
// borrowed bit to test and to mask, and a destructor frees the buffer and is done. Every edit names the text it expects and how often,
// so a change of wtf/Vector.h that one of them does not fit stops the build here.
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { parseArgs } from "node:util";

const outputFile = "AOTVector.h";

function fail(message: string): never {
  console.error(`error: aot/support/generator/vector.ts: ${message}`);
  process.exit(1);
}

function indexOfRequired(text: string, wanted: string, from = 0): number {
  const index = text.indexOf(wanted, from);
  if (index < 0) fail(`wtf/Vector.h no longer has ${JSON.stringify(wanted.slice(0, 80))}`);
  return index;
}

function between(text: string, first: string, end: string): string {
  const start = indexOfRequired(text, first);
  return text.slice(start, indexOfRequired(text, end, start));
}

function replaceExactly(text: string, old: string, replacement: string | readonly string[]): string {
  const replacements = typeof replacement === "string" ? [replacement] : replacement;
  const pieces = text.split(old);
  if (pieces.length - 1 !== replacements.length) {
    fail(`${JSON.stringify(old.slice(0, 80))} is there ${pieces.length - 1} times, not ${replacements.length}`);
  }
  return pieces.reduce((result, piece, index) => result + replacements[index - 1] + piece);
}

const freeForDestruction = `    void deallocateBufferForDestruction(T* bufferToDeallocate)
    {
        if (bufferToDeallocate)
            Malloc::free(bufferToDeallocate);
    }
`;

const wordsGetter = `    VectorWords& words()
    {
        static_assert(sizeof(VectorBufferBase) == sizeof(VectorWords) && !OBJECT_OFFSETOF(VectorBufferBase, m_buffer) && sizeof(m_buffer) == sizeof(VectorWords::buffer));
        static_assert(OBJECT_OFFSETOF(VectorBufferBase, m_capacity) == OBJECT_OFFSETOF(VectorWords, capacity) && OBJECT_OFFSETOF(VectorBufferBase, m_size) == OBJECT_OFFSETOF(VectorWords, size));
        return *reinterpret_cast<VectorWords*>(this);
    }
`;

const growthDeclarations = `    template<FailureAction> bool reserveCapacitySlowCase(size_t newCapacity);
    static constexpr bool growsAsBytes()
    {
        return movesAsBytes<T> && sizeof(T) <= std::numeric_limits<uint16_t>::max() && minCapacity <= std::numeric_limits<uint8_t>::max() && Base::inlineBufferOffset() <= std::numeric_limits<uint8_t>::max();
    }
    // The first buffer of a list has room for minCapacity elements or 32 bytes, whichever is less: most lists stay short, and a build keeps millions of them.
    static constexpr size_t smallestCapacity() { return std::min<size_t>(minCapacity, std::max<size_t>(32 / sizeof(T), 1)); }
    static constexpr VectorLayout layout() { return { static_cast<uint16_t>(sizeof(T)), static_cast<uint8_t>(smallestCapacity()), static_cast<uint8_t>(Base::inlineBufferOffset()) }; }
`;

const conversions = `
    // The two classes lay out the same words, and a vector of WTF's that nobody has borrowed holds the same values in them.
    using VectorOfWTF = WTF::Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>;
    Vector(VectorOfWTF&& other)
        : Vector(reinterpret_cast<Vector&&>(other))
    {
    }
    Vector(const VectorOfWTF& other)
        : Vector(reinterpret_cast<const Vector&>(other))
    {
    }
    template<typename Other> requires std::same_as<Other, VectorOfWTF>
    Vector& operator=(Other&& other) { return *this = reinterpret_cast<Vector&&>(other); }
    template<typename Other> requires std::same_as<Other, VectorOfWTF>
    Vector& operator=(const Other& other) { return *this = reinterpret_cast<const Vector&>(other); }
    template<typename U, size_t otherCapacity, typename OtherOverflowHandler, size_t otherMinCapacity, typename OtherMalloc>
    void appendVector(const WTF::Vector<U, otherCapacity, OtherOverflowHandler, otherMinCapacity, OtherMalloc>& other) { append(other.span()); }
    operator VectorOfWTF&() & { return reinterpret_cast<VectorOfWTF&>(*this); }
    operator const VectorOfWTF&() const & { return reinterpret_cast<const VectorOfWTF&>(*this); }
    operator VectorOfWTF&&() && { return reinterpret_cast<VectorOfWTF&&>(*this); }
`;

const head = `#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>
#include <wtf/HashTraits.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/VectorTraits.h>

// wtf/Vector.h for the compiler's own lists. Nobody borrows them, so there is no borrowed bit to test and to mask, and a destructor
// frees the buffer and is done.

namespace JSC { namespace AOT {

using WTF::FailureAction;
using WTF::GetPtrHelper;
using WTF::CopyOrMoveToVectorResult;
using WTF::ValueCheck;
using WTF::VectorBufferMalloc;
using WTF::VectorCopier;
using WTF::VectorMalloc;
using WTF::VectorTypeOperations;
using WTF::addressOfArgument;
using WTF::containerSize;
using WTF::isValidCapacityForVector;
using WTF::typelessPointersAreEqual;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename T, size_t inlineCapacity = 0, typename OverflowHandler = CrashOnOverflow, size_t minCapacity = 16, typename Malloc = VectorBufferMalloc> class Vector;

template<typename T>
inline constexpr bool movesAsBytes = (std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>) || VectorTraits<T>::canMoveWithMemcpy;

// FreeBSD's libc++ gives std::pair a copy constructor of its own.
template<typename First, typename Second>
inline constexpr bool movesAsBytes<std::pair<First, Second>> = movesAsBytes<First> && movesAsBytes<Second>;

struct VectorWords {
    void* buffer;
    unsigned capacity;
    unsigned size;
};

struct VectorLayout {
    uint16_t elementSize;
    uint8_t minCapacity;
    uint8_t inlineBufferOffset; // Zero without an inline buffer: no vector is its own buffer.
};

// AOTSystemResources.h
[[noreturn]] void exitProgramTooLarge();

template<typename Malloc>
ALWAYS_INLINE void allocateVectorBufferInline(VectorWords& vector, size_t newCapacity, size_t elementSize)
{
    if (newCapacity > std::numeric_limits<unsigned>::max()) [[unlikely]]
        exitProgramTooLarge();
    vector.buffer = Malloc::malloc(newCapacity * elementSize);
    vector.capacity = newCapacity;
}

template<typename Malloc>
NEVER_INLINE void allocateVectorBuffer(VectorWords& vector, size_t newCapacity, size_t elementSize)
{
    allocateVectorBufferInline<Malloc>(vector, newCapacity, elementSize);
}

template<typename Malloc>
NEVER_INLINE void reserveVectorCapacity(VectorWords& vector, size_t newCapacity, VectorLayout layout)
{
    void* oldBuffer = vector.buffer;
    allocateVectorBufferInline<Malloc>(vector, newCapacity, layout.elementSize);
    if (vector.size)
        memcpy(vector.buffer, oldBuffer, static_cast<size_t>(vector.size) * layout.elementSize);
    if (oldBuffer && oldBuffer != reinterpret_cast<char*>(&vector) + layout.inlineBufferOffset)
        Malloc::free(oldBuffer);
}

// A short list doubles.
template<typename Malloc>
constexpr size_t nextVectorCapacity(size_t capacity)
{
    return capacity < 16 ? 2 * capacity : Malloc::nextCapacity(capacity);
}

template<typename Malloc>
NEVER_INLINE void expandVectorCapacity(VectorWords& vector, size_t newMinCapacity, VectorLayout layout)
{
    reserveVectorCapacity<Malloc>(vector, std::max(newMinCapacity, std::max(static_cast<size_t>(layout.minCapacity), nextVectorCapacity<Malloc>(vector.capacity))), layout);
}

template<typename Malloc>
NEVER_INLINE void* expandVectorCapacity(VectorWords& vector, size_t newMinCapacity, VectorLayout layout, void* pointer)
{
    size_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(vector.buffer);
    bool isElement = offset < static_cast<size_t>(vector.size) * layout.elementSize;
    expandVectorCapacity<Malloc>(vector, newMinCapacity, layout);
    return isElement ? static_cast<char*>(vector.buffer) + offset : pointer;
}

`;

const tail = `

template<typename T, size_t inlineCapacityA, typename OverflowHandlerA, size_t minCapacityA, typename MallocA, size_t inlineCapacityB, typename OverflowHandlerB, size_t minCapacityB, typename MallocB>
bool operator==(const Vector<T, inlineCapacityA, OverflowHandlerA, minCapacityA, MallocA>& a, const WTF::Vector<T, inlineCapacityB, OverflowHandlerB, minCapacityB, MallocB>& b)
{
    return a.size() == b.size() && VectorTypeOperations<T>::compare(a.span().data(), b.span().data(), a.size());
}

template<typename T, size_t inlineCapacity>
bool insertInUniquedSortedVector(Vector<T, inlineCapacity>& vector, const T& value)
{
    return WTF::insertInUniquedSortedVector<T, inlineCapacity>(vector, value);
}

} } // namespace JSC::AOT

namespace WTF {

template<typename T, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
struct VectorTraits<JSC::AOT::Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>> : VectorTraits<Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>> { };

template<typename T, size_t inlineCapacity>
struct HashTraits<JSC::AOT::Vector<T, inlineCapacity>> : GenericHashTraits<JSC::AOT::Vector<T, inlineCapacity>> {
    static constexpr bool emptyValueIsZero = !inlineCapacity;

    static void constructDeletedValue(JSC::AOT::Vector<T, inlineCapacity>& slot) { new (NotNull, std::addressof(slot)) JSC::AOT::Vector<T, inlineCapacity>(WTF::HashTableDeletedValue); }
    static bool isDeletedValue(const JSC::AOT::Vector<T, inlineCapacity>& value) { return value.isHashTableDeletedValue(); }
};

} // namespace WTF

#endif // ENABLE(AOT)
`;

export function deriveVector(source: string): string {
  const license = source.slice(0, indexOfRequired(source, "#pragma once"));
  let text =
    between(source, "template<typename T, typename Malloc>\nclass VectorBufferBase {", "struct UnsafeVectorOverflow {") +
    between(
      source,
      "// Template default values are in Forward.h.\n",
      "#if ENABLE(SECURITY_ASSERTIONS)\ntemplate<typename T> struct ValueCheck<Vector<T>> {",
    ) +
    between(
      source,
      "template<typename T, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>\nsize_t removeRepeatedElements(Vector<",
      "template<typename SourceType>\nstruct CollectionInspector",
    );

  text = replaceExactly(
    text,
    "    unsigned m_capacity : 31;\n    mutable unsigned m_isBorrowed : 1;\n",
    "    unsigned m_capacity;\n",
  );
  text = replaceExactly(text, "        , m_isBorrowed(false)\n", ["", ""]);
  const firstBorrowedMember = indexOfRequired(text, "    bool isBorrowed() const { return m_isBorrowed; }");
  const lastBorrowedMember = indexOfRequired(text, "    void crashIfBorrowed() const\n");
  const behindBorrowedMembers = indexOfRequired(text, "    }\n", lastBorrowedMember) + 6;
  text = text.slice(0, firstBorrowedMember) + text.slice(behindBorrowedMembers).replace(/^\n+/, "");
  text = text.replace(/^[ \t]*(?:other\.|Base::)?crashIfBorrowed\(\);\n/gm, "");
  text = text.replace(/^[ \t]*using Base::(?:setIsBorrowed|crashIfBorrowed);\n/gm, "");
  text = text.replaceAll("    template<typename> friend class Borrow;\n", "");

  text = replaceExactly(text, "// Template default values are in Forward.h.\n", "");
  text = replaceExactly(
    text,
    "inline bool typelessPointersAreEqual(const void* a, const void* b) { return a == b; }\n\n",
    "",
  );
  text = replaceExactly(
    text,
    "template <typename ContainerType>\nsize_t containerSize(const ContainerType& container) { return std::size(container); }\n\n",
    "",
  );

  const behindDeallocateBuffer = "        Malloc::free(bufferToDeallocate);\n    }\n\n";
  const bufferGetter = "    T* buffer() LIFETIME_BOUND { return m_buffer; }";
  text = replaceExactly(
    text,
    behindDeallocateBuffer + bufferGetter,
    `${behindDeallocateBuffer}${freeForDestruction}\n${bufferGetter}`,
  );
  const destructor = (body: string): string => `    ~VectorBuffer()\n    {\n${body}    }\n`;
  text = replaceExactly(text, destructor("        deallocateBuffer(buffer());\n"), [
    destructor("        Base::deallocateBufferForDestruction(buffer());\n"),
    destructor(
      "        if (buffer() != inlineBuffer())\n            Base::deallocateBufferForDestruction(buffer());\n",
    ),
  ]);
  const lastIteratorType = "    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;\n";
  text = replaceExactly(text, lastIteratorType, lastIteratorType + conversions);
  text = text.replace(/(?<![:\w])SmartPtr\b/g, "WTF::SmartPtr");

  const bufferOfAnyElements = "        ASSERT(newCapacity);\n        if (!isValidCapacityForVector<T>(newCapacity)) {\n";
  text = replaceExactly(
    text,
    bufferOfAnyElements,
    `        ASSERT(newCapacity);
        if constexpr (action == FailureAction::Crash) {
            allocateVectorBuffer<Malloc>(words(), newCapacity, sizeof(T));
            return true;
        }
        if (!isValidCapacityForVector<T>(newCapacity)) {
`,
  );
  text = replaceExactly(text, bufferGetter, `${wordsGetter}\n${bufferGetter}`);
  const withoutInlineBuffer = "    void restoreInlineBufferIfNeeded() { }\n";
  text = replaceExactly(
    text,
    withoutInlineBuffer,
    `${withoutInlineBuffer}    static constexpr size_t inlineBufferOffset() { return 0; }\n`,
  );
  const withInlineBuffer = "    void restoreInlineBufferIfNeeded()\n    {\n";
  text = replaceExactly(
    text,
    withInlineBuffer,
    `    static constexpr size_t inlineBufferOffset() { return OBJECT_OFFSETOF(VectorBuffer, m_inlineBuffer); }\n\n${withInlineBuffer}`,
  );
  text = replaceExactly(text, "    using Base::capacitySpan;\n", [
    "    using Base::capacitySpan;\n    using Base::words;\n",
    "    using Base::capacitySpan;\n    using Base::words;\n",
  ]);

  const reserveCapacityDeclaration = "    template<FailureAction> bool reserveCapacity(size_t newCapacity);\n";
  text = replaceExactly(text, reserveCapacityDeclaration, reserveCapacityDeclaration + growthDeclarations);

  const member = (specifiers: string, signature: string): string =>
    `template<typename T, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
template<FailureAction action>
${specifiers}Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>::${signature}
{
`;
  const checkedAction = "    static_assert(action == FailureAction::Crash || action == FailureAction::Report);\n";
  const asBytes = (call: string, result: string): string =>
    `    if constexpr (action == FailureAction::Crash && growsAsBytes()) {
        asanSetBufferSizeToFullCapacity();
        ${call};
        asanSetInitialBufferSizeTo(size());
        return ${result};
    }
`;
  text = replaceExactly(
    text,
    member("bool ", "expandCapacity(size_t newMinCapacity)"),
    member("NEVER_INLINE bool ", "expandCapacity(size_t newMinCapacity)") +
      asBytes("expandVectorCapacity<Malloc>(Base::words(), newMinCapacity, layout())", "true"),
  );
  text = replaceExactly(
    text,
    "std::max(static_cast<size_t>(minCapacity), Malloc::nextCapacity(capacity()))",
    "std::max(smallestCapacity(), nextVectorCapacity<Malloc>(capacity()))",
  );
  const expandCapacityAroundPointer = member("NEVER_INLINE T* ", "expandCapacity(size_t newMinCapacity, T* ptr)") + checkedAction;
  text = replaceExactly(
    text,
    expandCapacityAroundPointer,
    expandCapacityAroundPointer +
      asBytes("ptr = static_cast<T*>(expandVectorCapacity<Malloc>(Base::words(), newMinCapacity, layout(), ptr))", "ptr"),
  );
  const appendSlowCase = (specifiers: string): string =>
    `template<FailureAction action, typename U>\n${specifiers}Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>::appendSlowCase(U&& value)\n`;
  text = replaceExactly(text, appendSlowCase("bool "), appendSlowCase("NEVER_INLINE bool "));
  const enoughCapacity = "    if (newCapacity <= capacity())\n        return true;\n";
  const reserveCapacity = member("bool ", "reserveCapacity(size_t newCapacity)") + checkedAction + enoughCapacity;
  text = replaceExactly(
    text,
    reserveCapacity,
    `${reserveCapacity}    return reserveCapacitySlowCase<action>(newCapacity);
}

${member("NEVER_INLINE bool ", "reserveCapacitySlowCase(size_t newCapacity)")}${asBytes("reserveVectorCapacity<Malloc>(Base::words(), newCapacity, layout())", "true")}`,
  );

  return license + head + text.replace(/\n+$/, "") + tail;
}

const { values } = parseArgs({ strict: true, options: { "wtf": { type: "string" }, "out": { type: "string" } } });
if (values.wtf === undefined || values.out === undefined) {
  fail("usage: <bun or node> aot/support/generator/vector.ts --wtf <Source/WTF> --out <dir>");
}
const written = deriveVector(readFileSync(join(values.wtf, "wtf", "Vector.h"), "utf8"));
const path = join(values.out, outputFile);
mkdirSync(values.out, { recursive: true });
if (!existsSync(path) || readFileSync(path, "utf8") !== written) writeFileSync(path, written);
