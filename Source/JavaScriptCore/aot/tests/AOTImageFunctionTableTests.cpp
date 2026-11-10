/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTCodeLayout.h"
#include "AOTCompiledFrame.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImage.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestSupport.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTPlan.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRepresentation.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCalls.h"
#include "AOTStubsExceptions.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

static CodeSketch sketchWithSite(CodeUnit unit, unsigned frameSize)
{
    CodeSketch sketch(unit);
    sketch.withFrame(frameSize);
    sketch.plain(0x1000 + unit.function.index());
    sketch.callStub(writeBarrier(), sketch.addSite());
    return sketch;
}

static CodeSketch sketchWithSite(uint32_t function, unsigned frameSize)
{
    return sketchWithSite(bodyUnitOf(function), frameSize);
}

static CompiledCode plainBody(uint32_t function, unsigned frameSize = 16)
{
    CodeSketch sketch = sketchWithSite(function, frameSize);
    sketch.end();
    return sketch.take();
}

static CompiledCode plainCodeOf(CodeUnit unit)
{
    CodeSketch sketch = sketchWithSite(unit, 16);
    sketch.end();
    return sketch.take();
}

static CompiledCode jumpToStubBody(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.plain(7);
    sketch.jumpToStub(catchException());
    return sketch.take();
}

static void testImageFunctionOf()
{
    TestBuild build(8);
    TestImage image = build.build(bodies(jumpToStubBody(1), jumpToStubBody(2), plainBody(4), plainBody(6)));
    const Image& loaded = *image.image;
    auto hasOwnRecord = [&](uint32_t function) {
        std::optional<uint32_t> index = loaded.imageFunctionOf(FunctionID(function));
        return index && loaded.function(*index).function() == function;
    };

    for (uint32_t function : { 1u, 2u, 4u, 6u })
        AOT_TEST_CHECK(hasOwnRecord(function));
    AOT_TEST_CHECK(loaded.imageFunctionOf(FunctionID(1u)) != loaded.imageFunctionOf(FunctionID(2u)));
    for (uint32_t function : { 0u, 3u, 5u, 7u })
        AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(function)));
    AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(build.program().functions.size() + 100)));
}

static Vector<CompiledCode> recordBodies()
{
    CodeSketch withHandler = sketchWithSite(3, 48);
    uint32_t handlerOffset = withHandler.plain(0x2000);
    withHandler.code().sites.rareLists.ensure().handlers.append(SiteHandler { 1, handlerOffset, HandlerType::Catch });
    withHandler.end();
    return bodies(plainBody(1, 32), plainBody(2, 48), withHandler.take(), jumpToStubBody(4));
}

static void testRecords()
{
    using enum ImageFunctionFlag;
    TestBuild build(5);
    TestImage image = build.build(recordBodies());
    const Image& loaded = *image.image;
    uint32_t first = image.indexOf(1);
    uint32_t second = image.indexOf(2);
    uint32_t third = image.indexOf(3);
    uint32_t frameless = image.indexOf(4);

    AOT_TEST_CHECK(loaded.frameOf(first).frameSizeInBytes() == 32);
    AOT_TEST_CHECK(loaded.frameOf(second).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(loaded.frameOf(third).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(!loaded.frameOf(frameless).frameSizeInBytes());
    AOT_TEST_CHECK(&loaded.frameOf(second) == &loaded.frameOf(third));
    AOT_TEST_CHECK(&loaded.frameOf(first) != &loaded.frameOf(second));
    AOT_TEST_CHECK(loaded.section<ImageFunction>(ImageSection::Functions).size() == 4);

    AOT_TEST_CHECK(loaded.function(first).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(second).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(third).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions, HasHandlers }));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasHandlers));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasFrameMaps));
    for (uint32_t function : { 1u, 2u, 3u, 4u })
        AOT_TEST_CHECK(loaded.function(image.indexOf(function)).function() == function);
}

static SUPPRESS_ASAN void testSampleInNativeCode()
{
    TestBuild build(4);
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callFunction(1, outer.addSite());
    uint32_t afterIndirectCall = outer.callStub(callValue(1), outer.addSite()) + sizeOfNearCall;
    uint32_t afterCallToOtherFunction = outer.callFunction(3, outer.addSite()) + sizeOfNearCall;
    uint32_t afterCallToNativeCode = outer.callStub(writeBarrier(), outer.addSite()) + sizeOfNearCall;
    outer.end();
    CodeSketch larger = sketchWithSite(3, 64);
    uint32_t afterCallInLargerFrame = larger.callFunction(1, larger.addSite()) + sizeOfNearCall;
    larger.end();
    TestImage image = build.build(bodies(plainBody(1, 32), outer.take(), larger.take()));
    const Image& loaded = *image.image;
    const StackBounds& bounds = Thread::currentSingleton().stack();
    auto returnAddressIn = [&](uint32_t function) {
        return const_cast<uint8_t*>(image.address(function, sizeof(uint32_t) + sizeOfNearCall));
    };
    void* nativeCode = reinterpret_cast<void*>(&testSampleInNativeCode);

    alignas(16) std::array<void*, 48> stack { };
    constexpr size_t recordOfCaller = 32;
    constexpr size_t recordOfOuter = recordOfCaller + 2 + 48 / sizeof(void*);
    constexpr size_t endOfNativeFrame = recordOfCaller - 32 / sizeof(void*);
    stack[recordOfOuter] = &stack[recordOfOuter + 4];
    stack[recordOfOuter + 1] = nativeCode;
    stack[recordOfCaller] = &stack[recordOfOuter];
    stack[recordOfCaller + 1] = returnAddressIn(2);
    auto isTop = [&](const SampledRegisters& registers, size_t record, const void* pc) {
        std::optional<SampledTop> top = findSampledTop(loaded, registers, nullptr, bounds);
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[record]) && top->pc == pc && !top->framelessTopCallerPC && !top->framePointer;
    };

    SampledRegisters withoutFrameRecord { &stack[recordOfCaller], nativeCode, nullptr, &stack[endOfNativeFrame - 8] };
    AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
    if constexpr (isX86_64()) {
        stack[endOfNativeFrame - 6] = returnAddressIn(3);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
        stack[endOfNativeFrame - 1] = const_cast<uint8_t*>(image.address(1, sizeof(uint32_t)));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
        stack[endOfNativeFrame - 1] = returnAddressIn(1);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        constexpr size_t endOfLargerFrame = recordOfCaller - 64 / sizeof(void*);
        stack[endOfLargerFrame - 1] = returnAddressIn(3);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        stack[recordOfCaller + 1] = const_cast<uint8_t*>(image.address(2, afterIndirectCall));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, image.address(2, afterIndirectCall)));
        stack[endOfLargerFrame - 1] = const_cast<uint8_t*>(image.address(3, afterCallInLargerFrame));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        stack[recordOfCaller + 1] = const_cast<uint8_t*>(image.address(2, afterCallToOtherFunction));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
    }

    void* afterNativeCall = const_cast<uint8_t*>(image.address(2, afterCallToNativeCode));
    stack[endOfNativeFrame - 1] = returnAddressIn(1);
    stack[recordOfCaller + 1] = afterNativeCall;
    for (void* linkRegister : { static_cast<void*>(nullptr), afterNativeCall, nativeCode }) {
        SampledRegisters withOwnFrameRecord { &stack[recordOfCaller], nativeCode, linkRegister, &stack[endOfNativeFrame - 8] };
        AOT_TEST_CHECK(isTop(withOwnFrameRecord, recordOfOuter, afterNativeCall));
    }
    stack[recordOfCaller + 1] = returnAddressIn(2);

    if constexpr (isARM64()) {
        auto check = [&](void* returnAddress) {
            SampledRegisters inLeaf { &stack[recordOfCaller], nativeCode, returnAddress, &stack[endOfNativeFrame] };
            AOT_TEST_CHECK(isTop(inLeaf, recordOfCaller, returnAddressIn(1)));
            stack[endOfNativeFrame - 2] = &stack[recordOfCaller];
            stack[endOfNativeFrame - 1] = returnAddress;
            SampledRegisters withFrameRecord { &stack[endOfNativeFrame - 2], nativeCode, returnAddress, &stack[endOfNativeFrame - 8] };
            AOT_TEST_CHECK(isTop(withFrameRecord, recordOfCaller, returnAddressIn(1)));
            withFrameRecord.linkRegister = nativeCode;
            AOT_TEST_CHECK(isTop(withFrameRecord, recordOfCaller, returnAddressIn(1)));
        };
        check(returnAddressIn(1));
#if CPU(ARM64) && !OS(DARWIN)
        uintptr_t signedReturnAddress = std::bit_cast<uintptr_t>(returnAddressIn(1));
        asm("mov x30, %0\n"
            "hint #24\n"
            "mov %0, x30"
            : "+r"(signedReturnAddress) : : "x30");
        check(std::bit_cast<void*>(signedReturnAddress));
#endif
    }
}

static CompiledCode bodyWithFrameMap(uint32_t function, const Vector<uint32_t>& frameMapWords)
{
    CodeSketch sketch = sketchWithSite(function, 16);
    sketch.code().sites.rareLists.ensure().frameMapWords = frameMapWords;
    sketch.end();
    return sketch.take();
}

static void testFrameMaps()
{
    Vector<uint32_t> firstWords { 1, 2, 3 };
    Vector<uint32_t> secondWords { 4, 5 };

    TestBuild build(4);
    TestImage image = build.build(bodies(bodyWithFrameMap(1, firstWords), bodyWithFrameMap(2, secondWords), bodyWithFrameMap(3, { })));
    const Image& loaded = *image.image;

    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(1)), firstWords.span()));
    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(2)), secondWords.span()));
    AOT_TEST_CHECK(loaded.frameMapWords(image.indexOf(3)).empty());
}

namespace {

struct ReturnOffsetsAroundFrame {
    uint32_t beforeFrame { 0 };
    uint32_t firstInFrame { 0 };
    uint32_t lastInFrame { 0 };
    uint32_t behindFrame { 0 };
    uint32_t inSecondFrame { 0 };
};

} // anonymous namespace

static uint32_t callAndGetReturnOffset(CodeSketch& sketch)
{
    sketch.callStub(writeBarrier(), sketch.addSite());
    return sketch.size();
}

static CompiledCode bodyWithFramesInTheMiddle(uint32_t function, ReturnOffsetsAroundFrame& offsets)
{
    CodeSketch sketch(function);
    CodeRange first { };
    CodeRange second { };
    sketch.pinned(1, 2);
    offsets.beforeFrame = callAndGetReturnOffset(sketch);
    first.begin = sketch.pinned(2, 3);
    offsets.firstInFrame = callAndGetReturnOffset(sketch);
    sketch.pinned(3, 40);
    offsets.lastInFrame = callAndGetReturnOffset(sketch);
    first.end = sketch.pinned(4, 2);
    offsets.behindFrame = callAndGetReturnOffset(sketch);
    second.begin = sketch.pinned(5, 2);
    offsets.inSecondFrame = callAndGetReturnOffset(sketch);
    second.end = sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.appendList({ first, second });
    return sketch.take();
}

static void testHasFrameAtReturnOffsets()
{
    TestBuild build(4);
    ReturnOffsetsAroundFrame offsets;
    TestImage image = build.build(bodies(bodyWithFramesInTheMiddle(1, offsets), plainBody(2), jumpToStubBody(3)));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);

    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, 0));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, offsets.beforeFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.firstInFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.lastInFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, offsets.behindFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.inSecondFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(image.indexOf(3), codeOffsetUnit));
}

static CompiledCode bodyWithEpilogues(uint32_t function, Vector<uint32_t>& returnOffsets)
{
    constexpr unsigned numberOfEpilogues = 6;
    CodeSketch sketch(function);
    Vector<CodeRange, 1> ranges;
    uint32_t begin = sketch.pinned(1, 2);
    for (unsigned epilogue = 0; epilogue < numberOfEpilogues; ++epilogue) {
        sketch.pinned(2, 50);
        returnOffsets.append(callAndGetReturnOffset(sketch));
        ranges.append(CodeRange { begin, sketch.pinned(3, 2) });
        begin = sketch.size();
    }
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges = WTF::move(ranges);
    return sketch.take();
}

#if CPU(X86_64)
static void checkFrameAtFaultingAccesses(unsigned sizeOfAccess)
{
    static_assert(sizeOfNearCall == 5);
    CodeSketch sketch(1);
    uint32_t accessBeforeFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t begin = sketch.pinnedBytes(8);
    uint32_t lastAccessInFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t end = sketch.pinnedBytes(8);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.append(CodeRange { begin, end });
    for (uint32_t access : { accessBeforeFrame, lastAccessInFrame })
        sketch.code().sites.faultingAccesses.append(FaultingAccess { access, sketch.addSite(), { 1 } });

    TestBuild build(2);
    TestImage image = build.build(bodies(sketch.take()));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);
    uint32_t returnOffsetBeforeFrame = accessBeforeFrame + sizeOfNearCall;
    uint32_t returnOffsetInFrame = lastAccessInFrame + sizeOfNearCall;

    AOT_TEST_CHECK(!loaded.hasFrameAt(index, accessBeforeFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, returnOffsetBeforeFrame));
    AOT_TEST_CHECK(loaded.hasFrameAt(index, lastAccessInFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, returnOffsetInFrame));
}

static void checkFrameOfLoneFaultingAccess(unsigned sizeOfAccess)
{
    Vector<uint32_t> returnOffsets;
    CompiledCode other = bodyWithEpilogues(1, returnOffsets);
    other.frame.frameSizeInBytes = 48;

    CodeSketch sketch(2);
    uint32_t begin = sketch.pinnedBytes(1);
    uint32_t access = sketch.pinnedBytes(sizeOfAccess);
    uint32_t end = sketch.pinnedBytes(1);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.hasFrameRecord = false;
    sketch.code().frame.frameRanges.append(CodeRange { begin + 1, end });
    sketch.code().sites.faultingAccesses.append(FaultingAccess { access, sketch.addSite(), { 1 } });

    TestBuild build(3);
    TestImage image = build.build(bodies(WTF::move(other), sketch.take()));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(2);
    uint32_t returnOffset = access + sizeOfNearCall;

    AOT_TEST_CHECK(loaded.frameOf(image.indexOf(1)).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(loaded.hasFrameAt(index, access));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, returnOffset));
    for (const ImageFrame& frame : { loaded.frameOf(index), loaded.frameAtReturnOffset(index, returnOffset), loaded.returnSiteInfoAt(index, returnOffset).frame }) {
        AOT_TEST_CHECK(frame.frameSizeInBytes() == 16);
        AOT_TEST_CHECK(!frame.hasFrameRecord());
    }
}

static void testFrameAtFaultingAccesses()
{
    for (unsigned sizeOfAccess : { 2u, 3u, 4u }) {
        checkFrameAtFaultingAccesses(sizeOfAccess);
        checkFrameOfLoneFaultingAccess(sizeOfAccess);
    }
}
#endif

static String textOfProgramReadingNames(unsigned numberOfNames)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
        "    v2 = NewClosure function=f1\n"
        "    v3 = Call v1, -, v2\n"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append("    v"_s, name + 4, " = PropGet v1 key=\"n"_s, name, "\"\n"_s);
    text.append("    Return\n}\n"
        "function f1 name=\"f1\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(2));
    return text.toString();
}

namespace {

struct NameNumberInCode {
    PropertyKeyID key;
    uint32_t offset { 0 };
};

} // anonymous namespace

static void testNameNumberPastFifteenBits()
{
    constexpr uint32_t firstNumberPastFifteenBits = 1u << 15;
    constexpr unsigned numberOfNames = firstNumberPastFifteenBits + 1000;

    TestBuild build(textOfProgramReadingNames(numberOfNames), { });
    const Typed::Program& program = build.program();
    const Typed::SlotPlan& slots = build.plan().slots();

    CodeSketch sketch(1);
    MentionList mentions;
    Vector<NameNumberInCode> words;
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        if (program.at(key).kind != PropertyKeyKind::Text)
            continue;
        uint32_t offset = sketch.pinned(0);
        sketch.code().body.relocations.append(Relocation { offset, RelocationKind::NumberWord, SymbolSpace::Number, primaryEntry, mentions.add(NumberSpace::Name, key) });
        words.append(NameNumberInCode { key, offset });
    }
    AOT_TEST_CHECK(words.size() >= numberOfNames);
    mentions.moveTo(sketch.code().body);
    sketch.end();
    TestImage image = build.build(bodies(sketch.take()));
    AOT_TEST_CHECK(image.image);

    const ImageText& text = image.image->text();
    const IdentifierNumbering& names = image.built.numbers->identifiers();
    const uint8_t* code = image.address(1, 0);

    unsigned numberOfNamesPastFifteenBits = 0;
    for (const NameNumberInCode& word : words) {
        std::optional<uint32_t> number = names.numberOf(word.key);
        AOT_TEST_CHECK(number);
        uint32_t numberInCode = 0;
        memcpy(&numberInCode, code + word.offset, sizeof(numberInCode));
        AOT_TEST_CHECK(numberInCode == *number);
        if (slots.keysWithColumn().get(word.key.index()))
            AOT_TEST_CHECK(*number < names.countWithLookupTableColumn() && RowDisplacementTable::hasColumn(*number));
        if (*number < firstNumberPastFifteenBits)
            continue;

        ++numberOfNamesPastFifteenBits;
        AOT_TEST_CHECK(names.at(*number).key == word.key);
        AOT_TEST_CHECK(*number < text.numberOfIdentifiers());
        AOT_TEST_CHECK(text.identifier(*number).text() == program.at(word.key).text);
    }
    AOT_TEST_CHECK(numberOfNamesPastFifteenBits >= numberOfNames - firstNumberPastFifteenBits);
}

static constexpr uint32_t numberOfVariedFunctions = 9000;
static constexpr uint32_t closuresHandedToOneCall = 50;
static constexpr std::array<unsigned, 3> threadCountsOfFunctionTables { 1, 2, 16 };
static constexpr std::array flagsOfVariedFunctions { "IsSloppy"_s, "IsHiddenInStackTraces"_s, "HasExplicitName"_s, "IsNamedByComputedKey"_s };
static constexpr std::array syntaxOfVariedFunctions { "IsEvaluatedScript"_s, "IsShownAsNativeCode"_s, "IsShownInHelperModule"_s };
static constexpr uint32_t numberOfWrittenForms = (1u << flagsOfVariedFunctions.size()) * 2 * 2 * 3;
static constexpr uint32_t lengthThatFitsNoByte = 300;
static constexpr unsigned numberOfCapturesThatFitNoHalfByte = 16;
static_assert(numberOfVariedFunctions > 2 * indicesInRangeOfLoop);
static_assert(numberOfWrittenForms / 2 * 3 > unsharedFunctionRecord);

static bool variedFunctionHasName(uint32_t function) { return function % 3; }
static bool variedFunctionHasCode(uint32_t function) { return function % 11 != 3; }
static bool variedFunctionHasEntry(uint32_t function) { return variedFunctionHasCode(function) && function % 13 != 5; }
static bool variedFunctionHasCoroutineBody(uint32_t function) { return variedFunctionHasCode(function) && function % 17 == 7; }

static uint32_t lengthOfVariedFunction(uint32_t function)
{
    if (!(function % 89))
        return lengthThatFitsNoByte;
    if (function % 97 == 1)
        return countsAreListed;
    return function % 7;
}

static unsigned numberOfCapturesOfVariedFunction(uint32_t function)
{
    if (!(function % 83))
        return numberOfCapturesThatFitNoHalfByte;
    if (!(function % 7))
        return 2;
    return function % 5 ? 0 : 1;
}

static void appendNamesInBrackets(StringBuilder& text, ASCIILiteral attribute, std::span<const ASCIILiteral> names, uint32_t namesToAppend)
{
    if (!namesToAppend)
        return;
    text.append(' ', attribute, "=["_s);
    bool isFirst = true;
    for (size_t index = 0; index < names.size(); ++index) {
        if (!(namesToAppend >> index & 1))
            continue;
        text.append(isFirst ? ""_s : ", "_s, names[index]);
        isFirst = false;
    }
    text.append(']');
}

static void appendVariedFunction(StringBuilder& text, uint32_t function)
{
    uint32_t form = function % numberOfWrittenForms;
    uint32_t flags = form % (1u << flagsOfVariedFunctions.size());
    form >>= flagsOfVariedFunctions.size();
    uint32_t syntax = form & 1;
    bool isArrow = form & 2;
    if (uint32_t shownAs = form >> 2)
        syntax |= 1u << shownAs;

    text.append("function f"_s, function);
    if (variedFunctionHasName(function))
        text.append(" name=\"f"_s, function, '"');
    text.append(" module=m0"_s, isArrow ? " kind=Arrow"_s : ""_s);
    appendNamesInBrackets(text, "flags"_s, flagsOfVariedFunctions, flags);
    if (uint32_t length = lengthOfVariedFunction(function))
        text.append(" length="_s, length);
    appendNamesInBrackets(text, "syntax"_s, syntaxOfVariedFunctions, syntax);
    unsigned numberOfCaptures = numberOfCapturesOfVariedFunction(function);
    if (numberOfCaptures) {
        text.append(" captures=["_s);
        for (unsigned capture = 0; capture < numberOfCaptures; ++capture) {
            text.append(capture ? ", "_s : ""_s, "{type=anything"_s);
            if (!(capture % 2))
                text.append(" variableName=\"kept"_s, capture, '"');
            text.append('}');
        }
        text.append(']');
    }
    text.append(" results=[{type=anything}] {\n  b0("_s);
    for (unsigned capture = 0; capture < numberOfCaptures; ++capture)
        text.append(capture ? ", v"_s : "v"_s, capture, ": anything"_s);
    text.append("):\n    Return\n}\n"_s);
}

static String textOfVariedFunctions()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry module=m0\n"
        "function f0 name=\"main\" module=m0 kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function) {
        text.append("    v"_s, function + 1, " = NewClosure"_s);
        for (unsigned capture = 0; capture < numberOfCapturesOfVariedFunction(function); ++capture)
            text.append(capture ? ", v1"_s : " v1"_s);
        text.append(" function=f"_s, function, '\n');
    }
    uint32_t nextValue = numberOfVariedFunctions + 2;
    for (uint32_t first = 1; first <= numberOfVariedFunctions; first += closuresHandedToOneCall) {
        text.append("    v"_s, nextValue++, " = Call v1, -"_s);
        for (uint32_t function = first; function < first + closuresHandedToOneCall && function <= numberOfVariedFunctions; ++function)
            text.append(", v"_s, function + 1);
        text.append('\n');
    }
    text.append("    Return\n}\n"_s);
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function)
        appendVariedFunction(text, function);
    text.append(runtimeFunctionsText(numberOfVariedFunctions + 1));
    return text.toString();
}

static Vector<CompiledCode> codeOfVariedFunctions(const TestBuild& build)
{
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function) {
        if (variedFunctionHasCode(function))
            code.append(plainBody(function));
        if (variedFunctionHasEntry(function))
            code.append(plainCodeOf(build.genericEntrypointOf(function)));
        if (variedFunctionHasCoroutineBody(function))
            code.append(plainCodeOf({ FunctionID(function), Typed::CodeRole::CoroutineBody }));
    }
    return code;
}

static bool lieInSeveralRangesOfLoop(uint32_t firstFunction, uint32_t lastFunction)
{
    return firstFunction / indicesInRangeOfLoop != lastFunction / indicesInRangeOfLoop;
}

static void checkTablesOfVariedFunctions(const TestBuild& build, const TestImage& image, const BodyFactsForTables& bodyFacts)
{
    const Typed::Program& program = build.program();
    const Typed::CallingConventions& conventions = build.plan().conventions();
    const ImageNumbers& numbers = *image.built.numbers;
    const FunctionNumbers& functions = numbers.functions();
    const ProgramTables& tables = image.image->programTables();
    const ClosureFunctionRangeData& closureFunctions = tables.closureFunctions;
    AOT_TEST_CHECK(tables.numberOfFunctions() == functions.numberOfFunctions());
    AOT_TEST_CHECK(closureFunctions.first == functions.firstClosureFunction().index() && closureFunctions.end == functions.endOfClosureFunctions().index());
    AOT_TEST_CHECK(closureFunctions.end < functions.numberOfFunctions());

    size_t numberOfNames = 0;
    size_t numberWithLargeCounts = 0;
    size_t numberThatOnlyReturn = 0;
    Vector<NamedCaptureData> namedCaptures;
    for (uint32_t index = 0; index < functions.numberOfFunctions(); ++index) {
        CodeUnit unit = index < functions.firstDerived() ? bodyUnitOf(index) : conventions.unitAt(functions.unitOf(FunctionID(index)));
        uint32_t source = unit.function.index();
        const Typed::Function& function = program.at(unit.function);
        FunctionTraceRecord trace = Typed::functionTraceOf(program, conventions, unit);
        FunctionRecordData record { build.plan().functionKind(program, Typed::isGenericEntry(unit.role) ? unit : conventions.closureEntryOf(unit.function)), trace };
        AOT_TEST_CHECK(std::bit_cast<uint32_t>(tables.recordOfFunction(index)) == std::bit_cast<uint32_t>(record));
        AOT_TEST_CHECK(tables.sourceFunctionOf(index) == source);
        AOT_TEST_CHECK(tables.builtinImplementedBy(index).has_value() == trace.implements().has_value());

        bool isShownByName = source && source <= numberOfVariedFunctions && variedFunctionHasCode(source);
        std::optional<uint32_t> name = isShownByName && function.name && function.name != program.strings.emptyString() ? std::optional(function.name.index()) : std::nullopt;
        AOT_TEST_CHECK(tables.nameOfFunction(index) == name);
        numberOfNames += name && index < functions.firstDerived();

        if (index < closureFunctions.first || index >= closureFunctions.end)
            continue;
        AOT_TEST_CHECK(unit == conventions.closureEntryOf(unit.function));
        uint32_t closureNumber = tables.closureNumberOf(index);
        Typed::ClosureCaptureCounts counts = Typed::closureCaptureCountsOf(program, conventions, unit.function);
        CaptureCounts countsInTable = tables.captureCountsOf(index);
        AOT_TEST_CHECK(countsInTable.references == counts.references && countsInTable.captures == counts.captures);
        AOT_TEST_CHECK(tables.functionLengthOf(index) == lengthOfVariedFunction(source));
        numberWithLargeCounts += lengthOfVariedFunction(source) >= countsAreListed || countsInTable.encoded() == countsAreListed;
        AOT_TEST_CHECK(std::bit_cast<uint16_t>(tables.kindOfClosureNumber(closureNumber)) == std::bit_cast<uint16_t>(record.kind));
        AOT_TEST_CHECK(tables.closureCellNumbers()[closureNumber] == numbers.closureCellNumber(unit.function));

        AOT_TEST_CHECK(!conventions.genericEntry(unit).constructEntry);
        bool onlyReturns = bodyFacts.functionsThatOnlyReturn.get(source) && record.kind.isConstructiblePlainFunction();
        AOT_TEST_CHECK(tables.closureNumberOnlyReturns(closureNumber) == onlyReturns);
        numberThatOnlyReturn += onlyReturns;

        size_t firstOfFunction = namedCaptures.size();
        for (unsigned capture = 0; capture < function.captures.size(); ++capture) {
            StringID variableName = function.captures[capture].variableName;
            if (variableName && Typed::holdsCells(Typed::closureCaptureRepresentationOf(program, conventions, unit.function, capture)))
                namedCaptures.append({ Number24(closureNumber), static_cast<uint8_t>(Typed::closureCaptureSlotOf(program, conventions, unit.function, capture)), variableName.index() });
        }
        std::ranges::stable_sort(namedCaptures.mutableSpan().subspan(firstOfFunction), { }, &NamedCaptureData::slot);
    }

    AOT_TEST_CHECK(tables.functionNames().size() == numberOfNames && numberOfNames > indicesInRangeOfLoop);
    AOT_TEST_CHECK(tables.functionsWithLargeCounts().size() == numberWithLargeCounts);
    AOT_TEST_CHECK(tables.constructEntrypoints().size() == numberThatOnlyReturn);
    Vector<NamedCaptureData> namedCapturesInTable;
    AOT_TEST_CHECK(forEachNamedCapture(tables.namedCaptures(), [&](const NamedCaptureData& row) {
        namedCapturesInTable.append(row);
    }));
    AOT_TEST_CHECK(equalSpans(asBytes(namedCapturesInTable.span()), asBytes(namedCaptures.span())));

    AOT_TEST_CHECK(tables.functionRecords().size() == unsharedFunctionRecord);
    AOT_TEST_CHECK(!tables.unsharedFunctionRecords().empty() && !tables.functionsWithLargeCounts().empty() && !tables.constructEntrypoints().empty() && !namedCaptures.isEmpty());
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(tables.unsharedFunctionRecords().front().function, tables.unsharedFunctionRecords().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(tables.functionsWithLargeCounts().front().function, tables.functionsWithLargeCounts().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(closureFunctions.first + tables.constructEntrypoints().front().function, closureFunctions.first + tables.constructEntrypoints().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(closureFunctions.first + namedCaptures.first().closureNumber, closureFunctions.first + namedCaptures.last().closureNumber));
}

static std::span<const uint8_t> partOfProgramTables(const TestImage& image, TablePartID id)
{
    uint64_t end = 0;
    for (unsigned part = 0; part < static_cast<unsigned>(id); ++part)
        end = roundUpToMultipleOf<8>(end) + image.built.bytesOfTableParts[part];
    const uint8_t* tables = reinterpret_cast<const uint8_t*>(&image.image->programTables());
    return { tables + roundUpToMultipleOf<8>(end), static_cast<size_t>(image.built.bytesOfTableParts[static_cast<unsigned>(id)]) };
}

static Vector<uint8_t> encodeTablesOfVariedFunctions(unsigned threadCount)
{
    AOT_TEST_CHECK(!ParallelLoops::isInsideLoop());
    TestBuild build(textOfVariedFunctions(), { .threadCount = threadCount });
    TestImage image = build.build(codeOfVariedFunctions(build));
    AOT_TEST_CHECK(image.image);
    BuildContext& context = build.context();
    BodyFactsForTables bodyFacts = collectBodyFactsForTables(build.program(), build.plan(), image.built.functions, context);
    checkTablesOfVariedFunctions(build, image, bodyFacts);

    TableInputs tableInputs { build.program(), build.facts(), build.plan(), image.built.functions, bodyFacts, context.options(), &context };
    TableEncoder encoder(tableInputs, *image.built.numbers, nullptr, TablePartID::Functions, context);
    encodeFunctionProgramTables(encoder);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    std::span<const uint8_t> part = encoder.part.bytesAt(0, encoder.part.size());
    AOT_TEST_CHECK(equalSpans(part, partOfProgramTables(image, TablePartID::Functions)));

    NamesOfTables names(build.program());
    nameWhatFunctionTablesName(tableInputs, GatheredMentions { }, names);
    const ProgramTables& tables = image.image->programTables();
    for (Number24 name : tables.functionNames())
        AOT_TEST_CHECK(names.strings.get(name));
    AOT_TEST_CHECK(forEachNamedCapture(tables.namedCaptures(), [&](const NamedCaptureData& row) {
        AOT_TEST_CHECK(names.strings.get(row.name));
    }));

    Vector<uint8_t> bytes(part);
    for (size_t string : names.strings) {
        uint32_t number = static_cast<uint32_t>(string);
        bytes.append(asByteSpan(number));
    }
    return bytes;
}

static void checkNamedVariablesAsBytes(const Vector<Vector<VariableData>>& modules, size_t maxSize)
{
    NamedVariablesWriter writer;
    size_t numberOfRows = 0;
    auto numberOf = [](size_t place) {
        return static_cast<uint32_t>(3 * place);
    };
    auto closureNumbersOf = [](size_t place) {
        uint32_t first = static_cast<uint32_t>(1000 - 10 * place);
        return place % 2 ? Vector<uint32_t> { } : Vector<uint32_t> { first, first + 1, first + 2, first + 5 };
    };
    for (const Vector<VariableData>& module : modules) {
        size_t place = &module - modules.begin();
        writer.beginModule(numberOf(place), module.size());
        for (const VariableData& variable : module)
            writer.append(variable);
        writer.endModule(closureNumbersOf(place).span());
        numberOfRows += module.size();
    }
    AOT_TEST_CHECK(writer.bytes().size() <= maxSize);
    Vector<Vector<VariableData>> read;
    Vector<Vector<uint32_t>> closureNumbersRead;
    AOT_TEST_CHECK(forEachNamedVariable(writer.bytes().span(), [&](uint32_t module, size_t numberOfVariables) {
        AOT_TEST_CHECK(read.size() < modules.size() && module == numberOf(read.size()) && numberOfVariables == modules[read.size()].size());
        read.append(Vector<VariableData> { });
        closureNumbersRead.append(Vector<uint32_t> { });
    }, [&](const VariableData& variable) {
        read.last().append(variable);
    }, [&](uint32_t first, uint32_t length) {
        for (uint32_t number = first; number < first + length; ++number)
            closureNumbersRead.last().append(number);
    }));
    AOT_TEST_CHECK(read.size() == modules.size());
    for (size_t module = 0; module < modules.size(); ++module) {
        AOT_TEST_CHECK(equalSpans(asBytes(read[module].span()), asBytes(modules[module].span())));
        AOT_TEST_CHECK(equalSpans(asBytes(closureNumbersRead[module].span()), asBytes(closureNumbersOf(module).span())));
    }
    for (size_t size = 0; size < writer.bytes().size(); ++size) {
        size_t numberOfRowsRead = 0;
        size_t numberOfModulesRead = 0;
        bool isWhole = forEachNamedVariable(writer.bytes().span().first(size), [&](uint32_t, size_t) {
            ++numberOfModulesRead;
        }, [&](const VariableData&) {
            ++numberOfRowsRead;
        }, [](uint32_t, uint32_t) { });
        AOT_TEST_CHECK(!isWhole || numberOfRowsRead < numberOfRows || numberOfModulesRead < modules.size());
    }
}

static void testNamedVariablesAsBytes()
{
    auto inStatics = [](uint32_t name, uint32_t offset, SlotRepresentation representation = SlotRepresentation::JSValue) {
        return VariableData { name, SlotData::create(offset, representation) };
    };
    auto closureOf = [](uint32_t name, uint32_t function) {
        return VariableData { name, SlotData::create(function, SlotRepresentation::DeferredClosure) };
    };
    checkNamedVariablesAsBytes({ }, 0);
    checkNamedVariablesAsBytes({ { inStatics(1, 0) } }, 4 + 6);
    checkNamedVariablesAsBytes({ { inStatics(VariableData::noName - 1, SlotData::maxOffset - 7) } }, 2 + 5 + 4 + 6);
    checkNamedVariablesAsBytes({ { }, { }, { inStatics(1, 0) }, { } }, 2 + 6 + 3 + 4 + 5 + 3);
    Vector<VariableData> neighbors;
    for (uint32_t index = 0; index < 50; ++index)
        neighbors.append(inStatics(600000 + index, 80000 + 8 * (index ^ 1)));
    checkNamedVariablesAsBytes({ neighbors }, 2 + 3 + 3 + 2 * 49 + 6);
    checkNamedVariablesAsBytes({
        { inStatics(5, 64), closureOf(6, 12345), inStatics(7, 72), closureOf(9, 12347) },
        { closureOf(2, 3), inStatics(3, 4, SlotRepresentation::Cell32), inStatics(3, 8, SlotRepresentation::Cell32), inStatics(400, 16, SlotRepresentation::Cell) },
        { inStatics(1, 24, SlotRepresentation::Cell) },
    }, 64 + 3 + 6 + 1 + 5);

    auto isRefused = [](std::initializer_list<uint8_t> bytes) {
        Vector<uint8_t> copy(std::span<const uint8_t>(bytes.begin(), bytes.size()));
        return !forEachNamedVariable(copy.span(), [](uint32_t, size_t) { }, [](const VariableData&) { }, [](uint32_t, uint32_t) { });
    };
    AOT_TEST_CHECK(!isRefused({ 4, 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 4, 0, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 0, 1, 1, 1 }));
    AOT_TEST_CHECK(isRefused({ 0, 0, 1, 2, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 1, 2, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 1, 1, 0x7f, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 0xff, 0xff, 0xff, 0xff, 0x0f, 0, 0 }));
}

static void testFunctionTablesOverSeveralRanges()
{
    Vector<uint8_t> expected = encodeTablesOfVariedFunctions(threadCountsOfFunctionTables[0]);
    for (unsigned threadCount : std::span(threadCountsOfFunctionTables).subspan(1))
        AOT_TEST_CHECK(encodeTablesOfVariedFunctions(threadCount) == expected);
}

std::span<const NativeTest> imageFunctionTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/functions/image-function-of"_s, testImageFunctionOf },
        { "image/functions/records"_s, testRecords },
        { "image/functions/sample-in-native-code"_s, testSampleInNativeCode },
        { "image/functions/frame-maps"_s, testFrameMaps },
        { "image/functions/has-frame-at-return-offsets"_s, testHasFrameAtReturnOffsets },
#if CPU(X86_64)
        { "image/functions/frame-at-faulting-accesses"_s, testFrameAtFaultingAccesses },
#endif
        { "image/functions/name-number-past-fifteen-bits"_s, testNameNumberPastFifteenBits },
        { "image/functions/tables-over-several-ranges"_s, testFunctionTablesOverSeveralRanges },
        { "image/functions/named-variables-as-bytes"_s, testNamedVariablesAsBytes },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
