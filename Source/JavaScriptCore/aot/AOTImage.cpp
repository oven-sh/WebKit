/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

#if ENABLE(FTL_JIT)

#include "AOTCompiler.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "ExecutableAllocator.h"
#include "FunctionExecutable.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "Options.h"
#include "SourceProvider.h"
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

uint64_t imageStamp()
{
    // The code has the layout of the engine's objects in it, and the order of the runtime table. Not proof against every change
    // (an image belongs in the executable that has the engine in it), but it catches the ones that come from mixing builds.
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(lowestAccessibleAddress()); // How a frame says which function it is a frame of.
    mix(Instance::offsetOfData());
    mix(numberOfStubs);
    mix(numOpcodeIDs);
    mix(sizeof(VM));
    mix(sizeof(JSGlobalObject));
    mix(sizeof(CodeBlock));
    mix(sizeof(Data));
    mix(VM::offsetOfSoftStackLimit());
    mix(VM::offsetOfHeapBarrierThreshold());
    mix(CodeBlock::offsetOfJITData());
    mix(JSGlobalObject::offsetOfGlobalLexicalBindingEpoch());
    mix(Options::useImmutableIntrinsics()); // What code takes for granted.
#if CPU(ARM64)
    mix(1 | MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() << 8);
#elif CPU(X86_64)
    mix(2);
#endif
    return stamp;
}

// The order of ImageHeader::selectorsInOrderOffset.
static int compareSelectors(bool is8BitA, std::span<const uint8_t> a, bool is8BitB, std::span<const uint8_t> b)
{
    if (is8BitA != is8BitB)
        return is8BitA ? -1 : 1;
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return memcmp(a.data(), b.data(), a.size());
}

static std::span<const uint8_t> bytesOf(const StringImpl& string) { return string.is8Bit() ? asBytes(string.span8()) : asBytes(string.span16()); }
static int compareSelectors(const StringImpl& a, const StringImpl& b) { return compareSelectors(a.is8Bit(), bytesOf(a), b.is8Bit(), bytesOf(b)); }

// ---- Building

void ImageBuilder::add(ImageKey key, uint64_t rank, CompiledCode&& code)
{
    // Where they pointed depends on where the code was when it was compiled, which is nothing to do with the code.
    for (auto& call : code.info.stubCalls)
        memset(code.bytes.mutableSpan().data() + call.offset, 0, sizeof(uint32_t));
    // Nor has the number it went by.
    memset(code.bytes.mutableSpan().data() + OBJECT_OFFSETOF(CodeHeader, index), 0, sizeof(uint32_t));
    Locker locker { m_lock };
    m_functions.append(Function { key, rank, WTF::move(code) });
}

Vector<uint8_t> ImageBuilder::finish()
{
    Locker locker { m_lock };
    std::ranges::sort(m_functions, [](const Function& a, const Function& b) {
        return a.rank < b.rank;
    });

    unsigned capacity = 16;
    while (capacity < m_functions.size() * 2)
        capacity *= 2;

    // The shapes that objects are made with and the names that properties are read by, numbered for the whole program in the order
    // they turn up in.
    Vector<UniquedStringImpl*> selectors { nullptr };
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> numberOfSelector;
    auto selectorFor = [&](UniquedStringImpl* name) {
        return numberOfSelector.ensure(name, [&] {
            selectors.append(name);
            return static_cast<uint32_t>(selectors.size() - 1);
        }).iterator->value;
    };
    struct Shape {
        unsigned inlineCapacity { 0 };
        Vector<uint32_t, 8> names;
    };
    Vector<Shape> shapes(1);
    UncheckedKeyHashMap<String, uint32_t> numberOfShape;
    BitVector selectorIsRead;
    for (auto& function : m_functions) {
        auto& info = function.code.info;
        RELEASE_ASSERT(info.siteConstants.size() == info.numSlots);
        for (uint32_t& constant : info.siteConstants) {
            if (!constant)
                continue;
            if (!(constant & CompiledFunctionInfo::siteConstantIsShape)) {
                constant = selectorFor(info.selectors[constant - 1]);
                selectorIsRead.set(constant);
                continue;
            }
            const KnownShape& known = info.shapes[(constant & ~CompiledFunctionInfo::siteConstantIsShape) - 1];
            Shape shape;
            shape.inlineCapacity = known.inlineCapacity;
            for (UniquedStringImpl* name : known.names)
                shape.names.append(selectorFor(name));
            Vector<uint32_t, 16> words { known.inlineCapacity };
            words.appendVector(shape.names);
            String key { std::span { reinterpret_cast<const Latin1Character*>(words.span().data()), words.size() * sizeof(uint32_t) } };
            // (A Structure has sixteen bits to say which in. The rest are made the way they would be if nobody had noticed.)
            if (auto it = numberOfShape.find(key); it != numberOfShape.end())
                constant = it->value;
            else if (shapes.size() > std::numeric_limits<uint16_t>::max())
                constant = 0;
            else {
                shapes.append(WTF::move(shape));
                constant = shapes.size() - 1;
                numberOfShape.add(key, constant);
            }
        }
    }
    RELEASE_ASSERT(selectors.size() < (1u << (32 - ImageDispatchEntry::locationBits)));

    // The dispatch table. The longest rows first, each as near the start as it fits.
    Vector<uint32_t> rowOfSelector;
    rowOfSelector.fill(0, selectors.size());
    Vector<uint32_t> dispatch;
    {
        Vector<Vector<std::pair<uint32_t, int32_t>, 0>> rows(selectors.size());
        for (uint32_t number = 1; number < shapes.size(); ++number) {
            auto& shape = shapes[number];
            for (unsigned i = 0; i < shape.names.size(); ++i) {
                if (!selectorIsRead.get(shape.names[i]))
                    continue;
                int32_t location = i < shape.inlineCapacity || !shape.inlineCapacity
                    ? static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + i)
                    : -static_cast<int32_t>(i - shape.inlineCapacity) - 2;
                rows[shape.names[i]].append({ number, location });
            }
        }
        Vector<uint32_t> order;
        for (uint32_t selector = 1; selector < selectors.size(); ++selector) {
            if (!rows[selector].isEmpty())
                order.append(selector);
        }
        // (Of those with one entry, in the order of the shapes: then finding each a place goes on from where the last one was found.)
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (rows[a].size() != rows[b].size())
                return rows[a].size() > rows[b].size();
            return rows[a][0].first < rows[b][0].first;
        });
        size_t firstFree = 0;
        size_t lastForOne = 0;
        auto isFree = [&](size_t index) { return index >= dispatch.size() || !dispatch[index]; };
        auto growWithZeros = [&](size_t size) {
            if (size > dispatch.size())
                dispatch.insertFill(dispatch.size(), 0, size - dispatch.size());
        };
        for (uint32_t selector : order) {
            auto& row = rows[selector];
            uint32_t first = row[0].first;
            while (!isFree(firstFree))
                ++firstFree;
            size_t at = std::max<size_t>(row.size() == 1 ? std::max(firstFree, lastForOne) : firstFree, first);
            for (;; ++at) {
                if (std::ranges::all_of(row, [&](auto& entry) { return isFree(at - first + entry.first); }))
                    break;
            }
            if (row.size() == 1)
                lastForOne = at;
            size_t start = at - first;
            growWithZeros(start + row.last().first + 1);
            for (auto& [shape, location] : row)
                dispatch[start + shape] = ImageDispatchEntry::encode(selector, location);
            rowOfSelector[selector] = safeCast<uint32_t>(start);
        }
        // Any row can be asked about any shape.
        size_t furthest = 0;
        for (uint32_t row : rowOfSelector)
            furthest = std::max<size_t>(furthest, row);
        growWithZeros(furthest + shapes.size());
    }
    Vector<ImageShape> imageShapes;
    Vector<uint32_t> namesOfShapes;
    for (auto& shape : shapes) {
        imageShapes.append({ static_cast<uint32_t>(namesOfShapes.size()), safeCast<uint16_t>(shape.names.size()), safeCast<uint16_t>(shape.inlineCapacity) });
        namesOfShapes.appendVector(shape.names);
    }
    Vector<ImageSelector> imageSelectors;
    Vector<uint8_t> textOfSelectors;
    for (uint32_t selector = 0; selector < selectors.size(); ++selector) {
        UniquedStringImpl* name = selectors[selector];
        if (!name) {
            imageSelectors.append({ 0, 0, 1 });
            continue;
        }
        if (!name->is8Bit() && textOfSelectors.size() % 2)
            textOfSelectors.append(0);
        imageSelectors.append({ safeCast<uint32_t>(textOfSelectors.size()), name->length(), name->is8Bit() });
        textOfSelectors.append(name->is8Bit() ? asBytes(name->span8()) : asBytes(name->span16()));
    }
    Vector<uint32_t> selectorsInOrder;
    for (uint32_t selector = 1; selector < selectors.size(); ++selector)
        selectorsInOrder.append(selector);
    std::ranges::sort(selectorsInOrder, [&](uint32_t a, uint32_t b) {
        return compareSelectors(*selectors[a], *selectors[b]) < 0;
    });
    if (Options::aotReportStats()) [[unlikely]]
        dataLogLn("AOT: ", shapes.size() - 1, " shapes with ", namesOfShapes.size(), " properties, ", selectors.size() - 1, " selectors of which ", selectorIsRead.bitCount(), " are read by, ", dispatch.size(), " entries in the dispatch table");

    // Which function a call is to: the first that has the key.
    Vector<uint32_t> functionWithKey;
    functionWithKey.fill(std::numeric_limits<uint32_t>::max(), capacity);
    for (size_t index = 0; index < m_functions.size(); ++index) {
        unsigned bucket = m_functions[index].key.hash() & (capacity - 1);
        while (functionWithKey[bucket] != std::numeric_limits<uint32_t>::max() && !m_functions[functionWithKey[bucket]].key.sameFunction(m_functions[index].key))
            bucket = (bucket + 1) & (capacity - 1);
        if (functionWithKey[bucket] == std::numeric_limits<uint32_t>::max())
            functionWithKey[bucket] = index;
    }
    auto directTargetOf = [&](const CompiledFunctionInfo& info, const StubCall& call) -> std::optional<size_t> {
        if (call.function == StubCall::noFunction || !call.isDirect)
            return std::nullopt;
        const ImageKey& key = info.knownCallees[call.function];
        for (unsigned bucket = key.hash() & (capacity - 1); functionWithKey[bucket] != std::numeric_limits<uint32_t>::max(); bucket = (bucket + 1) & (capacity - 1)) {
            size_t target = functionWithKey[bucket];
            if (m_functions[target].key.sameFunction(key))
                return m_functions[target].code.info.directEntryOffset ? std::optional<size_t> { target } : std::nullopt;
        }
        return std::nullopt;
    };

    // Where everything goes. The stubs come first, and again whenever the last copy is about to be out of reach.
    const StubBlob& stubs = stubBlob();
    Vector<size_t> stubsAt;
    Vector<std::pair<size_t, size_t>> placement; // Of each function: where it is, and where the stubs it calls are.
    // After each function's code, a veneer for each function that it calls directly and that is out of reach. Which those are
    // depends on where everything goes: so that is worked out without any first, and they are few enough not to change the answer.
    Vector<Vector<uint32_t, 0>> farCallees(m_functions.size());
    auto sizeWithVeneers = [&](size_t index) {
        size_t size = m_functions[index].code.bytes.size();
        return farCallees[index].isEmpty() ? size : WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size) + farCallees[index].size() * sizeOfVeneer;
    };
    if (usesStubs) {
        Vector<size_t> at;
        size_t end = 0;
        size_t lastStubs = std::numeric_limits<size_t>::max();
        for (auto& function : m_functions) {
            end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end);
            if (lastStubs == std::numeric_limits<size_t>::max() || end + function.code.bytes.size() - lastStubs > reachOfStubCall) {
                lastStubs = end;
                end = WTF::roundUpToMultipleOf<imageFunctionAlignment>(end + stubs.bytes.size());
            }
            at.append(end);
            end += function.code.bytes.size();
        }
        size_t sizeOfVeneers = 0;
        for (size_t index = 0; index < m_functions.size(); ++index) {
            auto& info = m_functions[index].code.info;
            for (auto& call : info.stubCalls) {
                auto target = directTargetOf(info, call);
                if (!target)
                    continue;
                size_t from = at[index] + call.offset;
                size_t to = at[*target] + m_functions[*target].code.info.directEntryOffset;
                if (((from > to ? from - to : to - from) <= reachOfStubCall && !Options::aotForceVeneers()) || farCallees[index].contains(static_cast<uint32_t>(*target)))
                    continue;
                farCallees[index].append(static_cast<uint32_t>(*target));
                sizeOfVeneers += sizeOfVeneer + sizeof(uint32_t) + imageFunctionAlignment;
            }
        }
        RELEASE_ASSERT(sizeOfVeneers + stubs.bytes.size() + imageFunctionAlignment < roomToSpareInReachOfStubCall);
        if (Options::aotReportStats()) [[unlikely]]
            dataLogLn("AOT: at most ", sizeOfVeneers, " bytes of veneers for calls out of reach");
    }
    size_t recordsSize = 0;
    size_t codeSize = 0;
    for (size_t indexOfFunction = 0; indexOfFunction < m_functions.size(); ++indexOfFunction) {
        auto& function = m_functions[indexOfFunction];
        recordsSize += sizeof(ImageFunction) + function.code.info.calleeSaveRegisters.registerCount() * sizeof(ImageCalleeSave) + function.code.info.catchEntrypoints.size() * sizeof(ImageCatchEntrypoint) + function.code.info.sites.size() * (sizeof(Site) + sizeof(uint32_t)) + function.code.info.knownCallees.size() * sizeof(ImageKey);
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize);
        if (usesStubs && (stubsAt.isEmpty() || codeSize + sizeWithVeneers(indexOfFunction) - stubsAt.last() > reachOfStubCall)) {
            stubsAt.append(codeSize);
            codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + stubs.bytes.size());
        }
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        codeSize += sizeWithVeneers(indexOfFunction);
    }
    RELEASE_ASSERT(recordsSize < std::numeric_limits<uint32_t>::max());

    ImageHeader header { };
    header.magic = imageMagic;
    header.stamp = imageStamp();
    header.tableOffset = sizeof(ImageHeader);
    header.tableCapacity = capacity;
    header.recordsOffset = header.tableOffset + capacity * sizeof(ImageKey);
    header.recordsSize = recordsSize;
    header.environmentsSize = m_environmentsSize;
    header.environmentsOffset = WTF::roundUpToMultipleOf<8>(static_cast<size_t>(header.recordsOffset) + recordsSize);
    header.numberOfEnvironments = m_environments.size();
    size_t endOfTables = static_cast<size_t>(header.environmentsOffset) + m_environments.size() * sizeof(ImageEnvironment);
    auto place = [&](size_t size) {
        endOfTables = WTF::roundUpToMultipleOf<8>(endOfTables);
        uint32_t result = safeCast<uint32_t>(endOfTables);
        endOfTables += size;
        return result;
    };
    header.shapesOffset = place(imageShapes.sizeInBytes());
    header.numberOfShapes = imageShapes.size();
    header.namesOfShapesOffset = place(namesOfShapes.sizeInBytes());
    header.selectorsOffset = place(imageSelectors.sizeInBytes());
    header.numberOfSelectors = imageSelectors.size();
    header.rowsOfSelectorsOffset = place(rowOfSelector.sizeInBytes());
    header.textOfSelectorsOffset = place(textOfSelectors.size());
    header.selectorsInOrderOffset = place(selectorsInOrder.sizeInBytes());
    header.dispatchOffset = place(dispatch.sizeInBytes());
    header.hashOfIntrinsics = Options::useImmutableIntrinsics() && ImmutableIntrinsics::shared() ? ImmutableIntrinsics::shared()->hash() : 0;
    header.dispatchSize = safeCast<uint32_t>(dispatch.size());
    header.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(endOfTables);
    header.codeSize = codeSize;
    header.size = WTF::roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);
    header.numberOfFunctions = m_functions.size();
    for (unsigned i = 0; i < numberOfStubs; ++i)
        header.stubOffsets[i] = stubs.offsets[i];

    Vector<uint8_t> image;
    image.fill(0, header.size);
    uint8_t* base = image.mutableSpan().data();
    memcpy(base, &header, sizeof(header));
    auto* table = reinterpret_cast<ImageKey*>(base + header.tableOffset);
    uint8_t* records = base + header.recordsOffset;
    uint8_t* code = base + header.codeOffset;

    memcpy(base + header.environmentsOffset, m_environments.span().data(), m_environments.size() * sizeof(ImageEnvironment));
    memcpy(base + header.shapesOffset, imageShapes.span().data(), imageShapes.sizeInBytes());
    memcpy(base + header.namesOfShapesOffset, namesOfShapes.span().data(), namesOfShapes.sizeInBytes());
    memcpy(base + header.selectorsOffset, imageSelectors.span().data(), imageSelectors.sizeInBytes());
    memcpy(base + header.rowsOfSelectorsOffset, rowOfSelector.span().data(), rowOfSelector.sizeInBytes());
    memcpy(base + header.textOfSelectorsOffset, textOfSelectors.span().data(), textOfSelectors.size());
    memcpy(base + header.selectorsInOrderOffset, selectorsInOrder.span().data(), selectorsInOrder.sizeInBytes());
    memcpy(base + header.dispatchOffset, dispatch.span().data(), dispatch.sizeInBytes());
    for (size_t at : stubsAt)
        memcpy(code + at, stubs.bytes.span().data(), stubs.bytes.size());

    Vector<uint32_t> functionInBucket;
    functionInBucket.fill(0, capacity);
    size_t recordAt = 0;
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        auto [codeAt, stubsForThis] = placement[index];

        ImageFunction record { };
        record.codeOffset = codeAt;
        record.codeSize = function.code.bytes.size();
        record.entryOffset = info.entryOffset;
        record.arityCheckOffset = info.arityCheckOffset;
        record.directEntryOffset = info.directEntryOffset;
        record.frameSizeInBytes = info.frameSizeInBytes;
        record.numSlots = info.numSlots;
        record.bytecodeHash = info.bytecodeHash;
        record.numberOfCalleeSaves = info.calleeSaveRegisters.registerCount();
        record.numberOfCatchEntrypoints = info.catchEntrypoints.size();
        record.numberOfKnownCallees = info.knownCallees.size();
        record.usesStaticImports = info.usesStaticImports;
        record.startsCold = info.startsCold;

        ImageKey key = function.key;
        key.record = recordAt + 1;
        unsigned mask = capacity - 1;
        unsigned bucket = key.hash() & mask;
        bool duplicate = false;
        while (table[bucket].record) {
            if (table[bucket].sameFunction(key)) {
                duplicate = true;
                break;
            }
            bucket = (bucket + 1) & mask;
        }
        if (duplicate) {
            // The same function again (the same text evaluated twice) is fine. Two functions that cannot be told apart are not:
            // neither is in the image then, since either could be asked for.
            auto& earlier = m_functions[functionInBucket[bucket]].code.bytes;
            if (earlier.size() != function.code.bytes.size() || memcmp(earlier.span().data(), function.code.bytes.span().data(), earlier.size()))
                table[bucket].kind = std::numeric_limits<uint32_t>::max();
            continue;
        }
        table[bucket] = key;
        functionInBucket[bucket] = index;

        memcpy(records + recordAt, &record, sizeof(record));
        recordAt += sizeof(record);
        for (unsigned i = 0; i < info.calleeSaveRegisters.registerCount(); ++i) {
            const RegisterAtOffset& entry = info.calleeSaveRegisters.at(i);
            ImageCalleeSave save { entry.reg().index(), static_cast<int32_t>(entry.offset()) };
            memcpy(records + recordAt, &save, sizeof(save));
            recordAt += sizeof(save);
        }
        for (auto& [bytecodeOffset, codeOffset] : info.catchEntrypoints) {
            ImageCatchEntrypoint entrypoint { bytecodeOffset, codeOffset };
            memcpy(records + recordAt, &entrypoint, sizeof(entrypoint));
            recordAt += sizeof(entrypoint);
        }
        memcpy(records + recordAt, info.sites.span().data(), info.sites.size() * sizeof(Site));
        recordAt += info.sites.size() * sizeof(Site);
        memcpy(records + recordAt, info.siteConstants.span().data(), info.siteConstants.sizeInBytes());
        recordAt += info.siteConstants.sizeInBytes();
        memcpy(records + recordAt, info.knownCallees.span().data(), info.knownCallees.size() * sizeof(ImageKey));
        recordAt += info.knownCallees.size() * sizeof(ImageKey);

        memcpy(code + codeAt, function.code.bytes.span().data(), function.code.bytes.size());
        uint32_t indexInProgram = index;
        memcpy(code + codeAt + OBJECT_OFFSETOF(CodeHeader, index), &indexInProgram, sizeof(indexInProgram));
        for (auto& call : info.stubCalls)
            retargetStubCall(code, codeAt + call.offset, stubsForThis + stubs.offsets[static_cast<unsigned>(call.stub)], call.isTailCall);
    }

    // With every function in its place: the calls from one to another.
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& info = m_functions[index].code.info;
        size_t codeAt = placement[index].first;
        for (auto& call : info.stubCalls) {
            if (call.function == StubCall::noFunction)
                continue;
            if (auto target = directTargetOf(info, call)) {
                size_t targetAt = placement[*target].first + m_functions[*target].code.info.directEntryOffset;
                size_t from = codeAt + call.offset;
                if (size_t veneer = farCallees[index].find(static_cast<uint32_t>(*target)); veneer != notFound) {
                    size_t veneerAt = codeAt + WTF::roundUpToMultipleOf<sizeof(uint32_t)>(m_functions[index].code.bytes.size()) + veneer * sizeOfVeneer;
                    writeVeneer(code, veneerAt, targetAt);
                    targetAt = veneerAt;
                }
                RELEASE_ASSERT((from > targetAt ? from - targetAt : targetAt - from) <= reachOfStubCall + roomToSpareInReachOfStubCall);
                retargetStubCall(code, from, targetAt, false);
                continue;
            }
            // (Whoever compiled the program has seen to it that there is code for what is called like that.)
            RELEASE_ASSERT(!call.hasNoOtherWay);
            const ImageKey& key = info.knownCallees[call.function];
            unsigned mask = capacity - 1;
            for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
                if (!table[bucket].sameFunction(key))
                    continue;
                size_t target = functionInBucket[bucket];
                auto& targetInfo = m_functions[target].code.info;
                if (call.isDirect && !targetInfo.directEntryOffset)
                    break;
                size_t targetAt = placement[target].first + (call.isDirect ? targetInfo.directEntryOffset : call.skipsArityCheck ? targetInfo.entryOffset : targetInfo.arityCheckOffset);
                size_t from = codeAt + call.offset;
                if ((from > targetAt ? from - targetAt : targetAt - from) <= reachOfStubCall)
                    retargetStubCall(code, from, targetAt, false);
                break;
            }
        }
    }
    m_functions.clear();
    return image;
}

// ---- Running

namespace {

struct Registry {
    Lock lock;
    Vector<Image*, 2> images;
    std::atomic<bool> hasAny { false };
};

Registry& registry()
{
    static NeverDestroyed<Registry> instance;
    return instance;
}

} // anonymous namespace

Image* Image::registerImage(std::span<const uint8_t> data, const void* code)
{
    if (data.size() < sizeof(ImageHeader))
        return nullptr;
    auto& header = *reinterpret_cast<const ImageHeader*>(data.data());
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size())
        return nullptr;
    if (!hasOneBitSet(header.tableCapacity)
        || static_cast<uint64_t>(header.tableOffset) + static_cast<uint64_t>(header.tableCapacity) * sizeof(ImageKey) > header.recordsOffset
        || static_cast<uint64_t>(header.recordsOffset) + header.recordsSize > header.environmentsOffset
        || static_cast<uint64_t>(header.environmentsOffset) + static_cast<uint64_t>(header.numberOfEnvironments) * sizeof(ImageEnvironment) > header.codeOffset
        || header.codeOffset + header.codeSize > header.size)
        return nullptr;
    // Its functions know what their numbers are.
    if (!reserveFunctionIndicesForImage(header.numberOfFunctions))
        return nullptr;

    auto* image = new Image(data, code);
    auto& all = registry();
    Locker locker { all.lock };
    all.images.append(image);
    all.hasAny.store(true, std::memory_order_release);
    return image;
}

static Image* imageWithEnvironments()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().environmentsSize)
            return all.images[i];
    }
    return nullptr;
}

Image& Image::of(const ImageFunction& function)
{
    auto& all = registry();
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<size_t>(reinterpret_cast<const uint8_t*>(&function) - image->m_data.data()) < image->m_data.size())
            return *image;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Image* Image::withShapes()
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return nullptr;
    for (unsigned i = 0; i < all.images.size(); ++i) {
        if (all.images[i]->header().numberOfShapes > 1)
            return all.images[i];
    }
    return nullptr;
}

AtomString Image::nameOfSelector(uint32_t selector) const
{
    RELEASE_ASSERT(selector && selector < header().numberOfSelectors);
    const ImageSelector& entry = at<ImageSelector>(header().selectorsOffset)[selector];
    const uint8_t* text = at<uint8_t>(header().textOfSelectorsOffset) + entry.text;
    if (entry.is8Bit)
        return AtomString(std::span { reinterpret_cast<const Latin1Character*>(text), entry.length });
    return AtomString(std::span { reinterpret_cast<const char16_t*>(text), entry.length });
}

uint32_t Image::selectorNamed(const StringImpl& name) const
{
    const uint32_t* inOrder = at<uint32_t>(header().selectorsInOrderOffset);
    const ImageSelector* all = at<ImageSelector>(header().selectorsOffset);
    const uint8_t* text = at<uint8_t>(header().textOfSelectorsOffset);
    size_t low = 0;
    size_t high = header().numberOfSelectors - 1;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const ImageSelector& entry = all[inOrder[middle]];
        int order = compareSelectors(entry.is8Bit, { text + entry.text, static_cast<size_t>(entry.length) * (entry.is8Bit ? 1 : 2) }, name.is8Bit(), bytesOf(name));
        if (!order)
            return inOrder[middle];
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return 0;
}

uint32_t Image::environmentsSize()
{
    Image* image = imageWithEnvironments();
    return image ? image->header().environmentsSize : 0;
}

uint32_t Image::numberOfFunctionsOfImageWithEnvironments()
{
    Image* image = imageWithEnvironments();
    return image ? image->header().numberOfFunctions : 0;
}

ImageEnvironment Image::environmentOf(uint32_t moduleOfGraph)
{
    Image* image = imageWithEnvironments();
    if (!image || moduleOfGraph >= image->header().numberOfEnvironments)
        return { };
    return reinterpret_cast<const ImageEnvironment*>(image->m_data.data() + image->header().environmentsOffset)[moduleOfGraph];
}

bool Image::containsCode(const void* pointer)
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return false;
    // (Nothing is ever taken out, and this may be asked while the thread that has the lock is stopped.)
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<const uint8_t*>(pointer) - static_cast<const uint8_t*>(image->m_code) < static_cast<ptrdiff_t>(image->header().codeSize) && pointer >= image->m_code)
            return true;
    }
    return false;
}

Image* Image::registerImageFromFile(const char* path)
{
    auto contents = FileSystem::readEntireFile(String::fromUTF8(path));
    if (!contents || contents->size() < sizeof(ImageHeader))
        return nullptr;
    // Both for good.
    auto* data = new Vector<uint8_t>(WTF::move(*contents));
    auto& header = *reinterpret_cast<const ImageHeader*>(data->span().data());
    if (header.magic != imageMagic || header.codeOffset + header.codeSize > data->size())
        return nullptr;
    void* code = nullptr;
    if (header.codeSize) {
        RefPtr<ExecutableMemoryHandle> handle = ExecutableAllocator::singleton().allocate(header.codeSize, JITCompilationCanFail);
        if (!handle)
            return nullptr;
        code = handle->start().untaggedPtr();
        performJITMemcpy<jitMemcpyRepatch>(code, data->span().data() + header.codeOffset, header.codeSize);
        MacroAssembler::cacheFlush(code, header.codeSize);
        auto* forGood = handle.leakRef();
        UNUSED_VARIABLE(forGood);
    }
    return registerImage(data->span(), code);
}

bool Image::hasAny()
{
    return registry().hasAny.load(std::memory_order_acquire);
}

const void* Image::addressOfStub(Stub stub)
{
    if (!usesStubs || !hasAny())
        return nullptr;
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (image->header().codeSize)
            return static_cast<const uint8_t*>(image->m_code) + image->header().stubOffsets[static_cast<unsigned>(stub)];
    }
    return nullptr;
}

const ImageFunction* Image::lookup(const ImageKey& key) const
{
    auto& header = this->header();
    auto* table = reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset);
    unsigned mask = header.tableCapacity - 1;
    for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
        if (table[bucket].sameFunction(key))
            return reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + table[bucket].record - 1);
    }
    return nullptr;
}

std::optional<ImageView> ImageView::tryCreate(std::span<const uint8_t> data, const void* address)
{
    if (data.size() < sizeof(ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const ImageHeader*>(data.data());
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size() || !header.environmentsSize)
        return std::nullopt;
    return ImageView { data, address };
}

std::optional<ImageView::Function> ImageView::find(const ImageKey& key) const
{
    auto& header = this->header();
    auto* table = reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset);
    unsigned mask = header.tableCapacity - 1;
    for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
        if (!table[bucket].sameFunction(key))
            continue;
        auto& function = *reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + table[bucket].record - 1);
        size_t start = header.codeOffset + function.codeOffset;
        auto whereItIsGoingToBe = [&](const void* pointer) { return m_address + (static_cast<const uint8_t*>(pointer) - m_data.data()); };
        return Function { const_cast<uint8_t*>(m_address) + start + function.arityCheckOffset, reinterpret_cast<const CodeHeader*>(m_data.data() + start)->index,
            reinterpret_cast<const Site*>(whereItIsGoingToBe(function.sites())), reinterpret_cast<const ImageFunction*>(whereItIsGoingToBe(&function)), function.numSlots, !!function.startsCold };
    }
    return std::nullopt;
}

void* ImageView::addressOfStub(Stub stub) const
{
    return const_cast<uint8_t*>(m_address) + header().codeOffset + header().stubOffsets[static_cast<unsigned>(stub)];
}

std::pair<Image*, const ImageFunction*> Image::find(const ImageKey& key)
{
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (auto* function = image->lookup(key))
            return { image, function };
    }
    return { nullptr, nullptr };
}

uint32_t moduleIDFor(SourceProvider& provider)
{
    if (uint32_t id = provider.aotModuleID())
        return id;
    // Without an embedder to number the modules, a module is known by its text.
    if (!Options::aotImagePath())
        return 0;
    uint32_t id = provider.hash() | 1;
    provider.setAOTModuleID(id);
    return id;
}

ImageKey imageKeyForTopLevelCode(uint32_t module)
{
    ImageKey key;
    key.module = module;
    key.start = std::numeric_limits<uint32_t>::max();
    key.kind = 0xff << 1;
    return key;
}

std::optional<ImageKey> imageKeyFor(ScriptExecutable* scriptExecutable, CodeSpecializationKind kind)
{
    SourceProvider* provider = scriptExecutable->source().provider();
    // The source of a constructor that nobody wrote is one of the engine's own.
    if (auto* function = dynamicDowncast<FunctionExecutable>(scriptExecutable); function && function->unlinkedExecutable()->isBuiltinDefaultClassConstructor() && function->topLevelExecutable())
        provider = function->topLevelExecutable()->source().provider();
    if (!provider)
        return std::nullopt;
    uint32_t module = moduleIDFor(*provider);
    if (!module)
        return std::nullopt;
    auto* executable = dynamicDowncast<FunctionExecutable>(scriptExecutable);
    if (!executable) {
        if (scriptExecutable->isEvalExecutable())
            return std::nullopt;
        return imageKeyForTopLevelCode(module);
    }
    auto functionKey = orderFunctionKey(*executable->unlinkedExecutable(), executable->source());
    if (!functionKey)
        return std::nullopt;
    ImageKey key;
    key.module = module;
    key.start = functionKey->start;
    key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
    return key;
}

unsigned hashOfCode(std::span<const uint8_t> code)
{
    unsigned hash = 2166136261u;
    for (uint8_t byte : code)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

static std::atomic<unsigned> s_installedFromImage;

static String nameForLogging(ScriptExecutable* executable)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(executable))
        return function->ecmaName().string();
    return "(top level)"_s;
}

bool moduleIsLinkedAsCompiled(JSScope* scope)
{
    while (scope && scope->type() != ModuleEnvironmentType)
        scope = scope->next();
    auto* record = scope ? dynamicDowncast<JSModuleRecord>(uncheckedDowncast<JSModuleEnvironment>(scope)->moduleRecord()) : nullptr;
    return record && record->isLinkedAsInImage(scope->globalObject());
}

ImageCode findInImage(ScriptExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSScope* scope)
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* path = Options::aotImagePath(); path && !Options::aotWriteImage()) {
            if (!Image::registerImageFromFile(path))
                dataLogLn("AOT: ", path, " is not an image for this engine");
        }
        if (Options::aotReportStats()) {
            atexit([] {
                dataLogLn("AOT: ", s_installedFromImage.load(), " functions ran from an image");
            });
        }
    });
    if (!Image::hasAny())
        return { };

    auto key = imageKeyFor(executable, kind);
    if (!key) {
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: no key for ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " bytecode ", unlinkedCodeBlock->instructionsSize());
        return { };
    }
    auto [image, function] = Image::find(*key);
    if (!function) {
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: not in the image: ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") bytecode ", unlinkedCodeBlock->instructionsSize());
        return { };
    }

    // The code finds what it has of the realm by its own number, so that is for one function of the realm. The same text evaluated
    // a second time is another function: it has constants of its own, for one thing.
    if (Instance* instance = scope->realm()->aotInstance()) {
        uint32_t index = reinterpret_cast<const CodeHeader*>(image->codeFor(*function))->index;
        if (instance->data[index] && FunctionRef { instance, index }.executable() != executable)
            return { };
    }

    if (function->usesStaticImports) {
        if (!moduleIsLinkedAsCompiled(scope)) {
            if (Options::aotVerbose()) [[unlikely]]
                dataLogLn("AOT: the module of ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " is not linked the way it was compiled for");
            return { };
        }
    }

    if (Options::aotValidateImage()) [[unlikely]] {
        if (hashOfBytecode(unlinkedCodeBlock) != function->bytecodeHash) {
            dataLogLn("AOT: the image's code for ", nameForLogging(executable), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") was compiled from other bytecode");
            return { };
        }
    }
    if (Options::aotVerbose()) [[unlikely]]
        dataLogLn("AOT: ", nameForLogging(executable), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") is at ", RawPointer(image->codeFor(*function)), " size ", function->codeSize, " hash ", hashOfCode({ image->codeFor(*function), function->codeSize }));
    return { image, function };
}

Ref<JITCode> codeFromImage(ImageCode code, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    auto [image, function] = code;
    s_installedFromImage++;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::wayInto(unlinkedCodeBlock)));
}

// ---- Options::aotWriteImage()

void addToImageBeingWritten(ScriptExecutable* executable, CodeSpecializationKind kind, const JITCode& jitCode)
{
    static NeverDestroyed<ImageBuilder> builder;
    static std::atomic<uint64_t> rank;
    static std::once_flag once;
    std::call_once(once, [] {
        atexit([] {
            Vector<uint8_t> image = builder.get().finish();
            if (!FileSystem::overwriteEntireFile(String::fromUTF8(Options::aotImagePath()), image.span()))
                dataLogLn("AOT: cannot write ", Options::aotImagePath());
        });
    });

    auto key = imageKeyFor(executable, kind);
    if (!key)
        return;
    CompiledCode code;
    code.info = jitCode.info();
    code.bytes.append(std::span { static_cast<const uint8_t*>(const_cast<JITCode&>(jitCode).dataAddressAtOffset(0)), jitCode.info().codeSize });
    builder.get().add(*key, rank++, WTF::move(code));
}

} // namespace AOT

bool isPCOfAOTImage(const void* pc)
{
    return AOT::Image::containsCode(pc);
}

bool registerAOTImage(std::span<const uint8_t> image, const void* code)
{
    return !!AOT::Image::registerImage(image, code);
}

static void compileAllIn(VM&, UnlinkedCodeBlock&, AOTCompileAllResult&);

static void compileAllOf(VM& vm, UnlinkedFunctionExecutable& executable, AOTCompileAllResult& result)
{
    auto [forCall, forConstruct] = executable.codeBlocksDecodingCached(vm);
    for (UnlinkedFunctionCodeBlock* codeBlock : { forCall, forConstruct }) {
        if (!codeBlock)
            continue;
        result.functions++;
        result.bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result.compiled++;
            result.codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, result);
    }
}

static void compileAllIn(VM& vm, UnlinkedCodeBlock& codeBlock, AOTCompileAllResult& result)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i)
        compileAllOf(vm, *codeBlock.functionDecl(i), result);
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i)
        compileAllOf(vm, *codeBlock.functionExpr(i), result);
}

std::optional<AOTCompileAllResult> aotCompileAllFunctions(VM& vm, const SourceCode& source, bool isModule)
{
    DeferGC deferGC(vm);
    vm.keepUnlinkedCode();
    ParserError error;
    UnlinkedCodeBlock* codeBlock = isModule
        ? static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes))
        : static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes));
    std::optional<AOTCompileAllResult> result;
    if (!error.isValid() && codeBlock) {
        result = AOTCompileAllResult { };
        result->functions++;
        result->bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result->compiled++;
            result->codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, *result);
    }
    vm.stopKeepingUnlinkedCode();
    return result;
}

std::optional<size_t> aotImageSize(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size())
        return std::nullopt;
    return static_cast<size_t>(header.size);
}

std::optional<std::pair<size_t, size_t>> aotImageCodeRange(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size() || header.codeOffset > header.size)
        return std::nullopt;
    return std::pair { static_cast<size_t>(header.codeOffset), static_cast<size_t>(header.size - header.codeOffset) };
}

} // namespace JSC

#endif // ENABLE(FTL_JIT)
