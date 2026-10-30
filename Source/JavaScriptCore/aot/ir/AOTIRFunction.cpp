/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRFunction.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTObservableOperations.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static Wire::SourceLocation locationInModule(ModuleID module, uint32_t position)
{
    if (!module || position == Instruction::noPosition)
        return noSourceLocation;
    return { module.index(), position };
}

const DeclaredType& Function::result() const
{
    RELEASE_ASSERT(results.size() == 1);
    return results[0];
}

void Function::willEditEdgesOf(const Block& block)
{
    block.forEachTarget([&](const Target& target) {
        if (target.block)
            m_changes.log->didEdit(target.block, BlockEdit::Incoming);
    });
}

void Function::releaseBody()
{
    RELEASE_ASSERT(bodyIsReleased && !blocks.isEmpty());
    editAllBlocks().clear();
    editAllValues().clear();
    writtenPositions.clear();
}

// A word is written as its difference from the word `stride` words in front of it, in 0, 1, 2 or 4 bytes: a record of a table differs little from the record before it.
// In front of every four words stands a byte that says how many bytes each takes.
static constexpr std::array<uint8_t, 4> bytesOfPackedWord { 0, 1, 2, 4 };
static constexpr std::array<uint32_t, 4> maskOfPackedWord { 0, 0xff, 0xffff, 0xffffffff };
static constexpr size_t spareBytesBehindPackedWords = sizeof(uint32_t);
static_assert(std::endian::native == std::endian::little);

static constexpr size_t mostBytesOfPackedWords(size_t numberOfWords) { return numberOfWords * sizeof(uint32_t) + (numberOfWords + 3) / 4; }

static ALWAYS_INLINE unsigned packWord(uint8_t*& target, uint32_t word, uint32_t wordInFront)
{
    uint32_t difference = word - wordInFront;
    uint32_t written = (difference << 1) ^ static_cast<uint32_t>(static_cast<int32_t>(difference) >> 31);
    unsigned size = !!written + (written > 0xff) + (written > 0xffff);
    memcpy(target, &written, sizeof(written));
    target += bytesOfPackedWord[size];
    return size;
}

static ALWAYS_INLINE uint32_t unpackWord(const uint8_t*& source, unsigned size, uint32_t wordInFront)
{
    uint32_t written;
    memcpy(&written, source, sizeof(written));
    written &= maskOfPackedWord[size];
    source += bytesOfPackedWord[size];
    return ((written >> 1) ^ (0 - (written & 1))) + wordInFront;
}

// The first `stride` words have zero in front of them. What is left over behind the last four takes a byte of sizes too.
static uint8_t* packWords(uint8_t* target, std::span<const uint32_t> span, size_t stride)
{
    const uint32_t* words = span.data();
    size_t count = span.size();
    size_t index = 0;
    auto packUpTo = [&](size_t end, auto wordInFrontOf) ALWAYS_INLINE_LAMBDA {
        for (; index + 4 <= end; index += 4) {
            uint8_t* sizes = target++;
            unsigned first = packWord(target, words[index], wordInFrontOf(index));
            unsigned second = packWord(target, words[index + 1], wordInFrontOf(index + 1));
            unsigned third = packWord(target, words[index + 2], wordInFrontOf(index + 2));
            unsigned fourth = packWord(target, words[index + 3], wordInFrontOf(index + 3));
            *sizes = static_cast<uint8_t>(first | second << 2 | third << 4 | fourth << 6);
        }
    };
    auto checked = [&](size_t at) ALWAYS_INLINE_LAMBDA { return at >= stride ? words[at - stride] : 0; };
    packUpTo(std::min(count, (stride + 3) & ~static_cast<size_t>(3)), checked);
    packUpTo(count, [&](size_t at) ALWAYS_INLINE_LAMBDA { return words[at - stride]; });
    if (index < count) {
        uint8_t* sizes = target++;
        unsigned sizesOfRest = 0;
        for (unsigned shift = 0; index < count; ++index, shift += 2)
            sizesOfRest |= packWord(target, words[index], checked(index)) << shift;
        *sizes = static_cast<uint8_t>(sizesOfRest);
    }
    return target;
}

static const uint8_t* unpackWords(const uint8_t* source, uint32_t* words, size_t count, size_t stride)
{
    size_t index = 0;
    auto unpackUpTo = [&](size_t end, auto wordInFrontOf) ALWAYS_INLINE_LAMBDA {
        for (; index + 4 <= end; index += 4) {
            unsigned sizes = *source++;
            words[index] = unpackWord(source, sizes & 3, wordInFrontOf(index));
            words[index + 1] = unpackWord(source, sizes >> 2 & 3, wordInFrontOf(index + 1));
            words[index + 2] = unpackWord(source, sizes >> 4 & 3, wordInFrontOf(index + 2));
            words[index + 3] = unpackWord(source, sizes >> 6, wordInFrontOf(index + 3));
        }
    };
    auto checked = [&](size_t at) ALWAYS_INLINE_LAMBDA { return at >= stride ? words[at - stride] : 0; };
    unpackUpTo(std::min(count, (stride + 3) & ~static_cast<size_t>(3)), checked);
    unpackUpTo(count, [&](size_t at) ALWAYS_INLINE_LAMBDA { return words[at - stride]; });
    if (index < count) {
        unsigned sizes = *source++;
        for (; index < count; ++index, sizes >>= 2)
            words[index] = unpackWord(source, sizes & 3, checked(index));
    }
    return source;
}

template<typename Table>
static std::span<const uint32_t> wordsOf(const Table& table)
{
    using T = typename Table::ValueType;
    static_assert(!(sizeof(T) % sizeof(uint32_t)) && VectorTraits<T>::canMoveWithMemcpy);
    return { reinterpret_cast<const uint32_t*>(table.span().data()), table.size() * sizeof(T) / sizeof(uint32_t) };
}

// The elements point to what the elements that were written pointed to.
template<typename Table>
static const uint8_t* unpackTable(const uint8_t* source, Table& table, size_t size)
{
    using T = typename Table::ValueType;
    static_assert(sizeof(Table) == sizeof(VectorWords));
    RELEASE_ASSERT(table.isEmpty() && !table.capacity());
    if (!size)
        return source;
    VectorWords& words = reinterpret_cast<VectorWords&>(table);
    words.buffer = fastMalloc(size * sizeof(T));
    words.capacity = static_cast<unsigned>(size);
    words.size = static_cast<unsigned>(size);
    return unpackWords(source, static_cast<uint32_t*>(words.buffer), size * sizeof(T) / sizeof(uint32_t), sizeof(T) / sizeof(uint32_t));
}

ParkedBody::ParkedBody(Function::Blocks::Storage& blocks, Function::Values::Storage& values)
    : m_numberOfValues(values.size())
    , m_numberOfBlocks(blocks.size())
{
    BodyWords parts;
    for (const Value& value : values)
        value.appendWordsTo(parts);
    for (const Block& block : blocks)
        block.appendPartsTo(parts);
    RELEASE_ASSERT(parts.size() <= UINT32_MAX);
    m_numberOfWordsOfParts = static_cast<uint32_t>(parts.size());

    auto wordsOfValues = wordsOf(values);
    auto wordsOfBlocks = wordsOf(blocks);
    Vector<uint8_t, 8192> bytes(mostBytesOfPackedWords(wordsOfValues.size()) + mostBytesOfPackedWords(wordsOfBlocks.size()) + mostBytesOfPackedWords(parts.size()) + spareBytesBehindPackedWords);
    uint8_t* end = packWords(bytes.mutableSpan().data(), wordsOfValues, sizeof(Value) / sizeof(uint32_t));
    end = packWords(end, wordsOfBlocks, sizeof(Block) / sizeof(uint32_t));
    end = packWords(end, parts.span(), 1);
    m_bytes = Vector<uint8_t>(bytes.span().first(end - bytes.span().data() + spareBytesBehindPackedWords));
    values.clear();
    blocks.clear();
}

void ParkedBody::takeInto(Function::Blocks::Storage& blocks, Function::Values::Storage& values)
{
    const uint8_t* source = unpackTable(m_bytes.span().data(), values, m_numberOfValues);
    source = unpackTable(source, blocks, m_numberOfBlocks);
    BodyWords parts;
    parts.grow(m_numberOfWordsOfParts);
    source = unpackWords(source, parts.mutableSpan().data(), parts.size(), 1);
    RELEASE_ASSERT(source + spareBytesBehindPackedWords == m_bytes.span().data() + m_bytes.size());
    BodyWordsReader reader(parts.span());
    for (Value& value : values)
        value.takeWordsFrom(reader);
    for (Block& block : blocks)
        block.takePartsFrom(reader);
    RELEASE_ASSERT(reader.isAtEnd());
    m_bytes = { };
}

bool Function::isModuleBody() const
{
    switch (kind) {
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Function::isUnsplitCoroutine() const
{
    return hasSourceBodyOf(role) && hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
}

ConstructorForm Function::constructorForm() const
{
    switch (kind) {
    case FunctionKind::Plain:
        return hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) ? ConstructorForm::NotConstructor : ConstructorForm::Base;
    case FunctionKind::BaseConstructor:
        return ConstructorForm::Base;
    case FunctionKind::DerivedConstructor:
        return ConstructorForm::Derived;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return ConstructorForm::NotConstructor;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Wire::SourceLocation Function::locationOf(ValueID id) const
{
    return locationOf(at(id).origin());
}

Wire::SourceLocation Function::locationOf(Origin origin) const
{
    if (origin.position == Instruction::noPosition)
        return noSourceLocation;
    if (!origin.inlineFrame)
        return locationInModule(module, origin.position);

    if (origin.inlineFrame >= inlineFrames.size())
        return noSourceLocation;
    if (ModuleID calleeModule = inlineFrames[origin.inlineFrame].module)
        return locationInModule(calleeModule, origin.position);

    uint32_t outermostFrame = origin.inlineFrame;
    while (uint32_t parent = inlineFrames[outermostFrame].parent) {
        if (parent >= outermostFrame)
            return noSourceLocation;
        outermostFrame = parent;
    }
    return locationInModule(module, inlineFrames[outermostFrame].callPosition);
}

Origin Function::enterFrameOfBuiltin(FunctionID self, Origin call, BuiltinID builtin)
{
    if (inlineFrames.isEmpty())
        inlineFrames.append(InlineFrame { });
    const InlineFrame& caller = inlineFrames[call.inlineFrame];
    if (caller.calledBuiltin)
        return call;
    // The steps keep the position of the call, so the frame names the function that holds the call.
    inlineFrames.append(InlineFrame {
        .callee = call.inlineFrame ? caller.callee : self,
        .callPosition = call.position,
        .parent = call.inlineFrame,
        .isTailCall = call.isTailCall,
        .callIsPartOfConstruction = call.isPartOfConstruction,
        .module = call.inlineFrame ? caller.module : module,
        .calledBuiltin = builtin,
    });
    return { .position = call.position, .inlineFrame = static_cast<uint32_t>(inlineFrames.size() - 1) };
}

std::optional<uint32_t> Function::writtenPositionOf(ValueID id) const
{
    auto found = std::ranges::lower_bound(writtenPositions, id.index(), { }, [](const WrittenPosition& written) { return written.value.index(); });
    if (found == writtenPositions.end() || found->value != id)
        return std::nullopt;
    return found->position;
}

Wire::SourceLocation Function::writtenLocationOf(ValueID id) const
{
    Wire::SourceLocation location = locationOf(id);
    if (std::optional<uint32_t> written = writtenPositionOf(id))
        location.position = *written;
    return location;
}

Wire::SourceLocation Function::declarationLocation() const
{
    return locationInModule(module, position);
}

static unsigned entryParameterCount(const Function& function, EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
        return function.captures.size();
    case EntryParameter::Parameter:
        return function.parameters.size();
    case EntryParameter::This:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return function.flags.containsAll(presentWithOf(kind)) ? 1 : 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static unsigned numberBefore(const Function& function, EntryParameter kind)
{
    unsigned count = 0;
    for (EntryParameter earlierKind : allMembersOf<EntryParameter>) {
        if (earlierKind == kind)
            break;
        count += entryParameterCount(function, earlierKind);
    }
    return count;
}

static std::optional<unsigned> singleEntryParameterIndex(const Function& function, EntryParameter kind)
{
    if (!entryParameterCount(function, kind))
        return std::nullopt;
    return numberBefore(function, kind);
}

unsigned Function::numberOfEntryParameters() const
{
    unsigned count = 0;
    for (EntryParameter kind : allMembersOf<EntryParameter>)
        count += entryParameterCount(*this, kind);
    return count;
}

std::optional<unsigned> Function::thisParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::This);
}

std::optional<unsigned> Function::calleeParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::Callee);
}

std::optional<unsigned> Function::newTargetParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::NewTarget);
}

std::optional<unsigned> Function::argumentCountParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::ArgumentCount);
}

unsigned Function::firstParameterIndex() const
{
    return numberBefore(*this, EntryParameter::Parameter);
}

std::optional<unsigned> Function::hiddenRestParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::HiddenRest);
}

unsigned Function::numberOfPositionalParameters() const
{
    unsigned numberOfRestParameters = has(FunctionFlags::HasRestParameter);
    RELEASE_ASSERT(parameters.size() >= numberOfRestParameters);
    return parameters.size() - numberOfRestParameters;
}

EntryParameter Function::entryParameterKind(unsigned entryParameterIndex) const
{
    unsigned end = 0;
    for (EntryParameter kind : allMembersOf<EntryParameter>) {
        end += entryParameterCount(*this, kind);
        if (entryParameterIndex < end)
            return kind;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const DeclaredType* Function::declaredEntryParameter(unsigned entryParameterIndex) const
{
    EntryParameter kind = entryParameterKind(entryParameterIndex);
    switch (kind) {
    case EntryParameter::Capture:
        return &captures[entryParameterIndex - numberBefore(*this, kind)];
    case EntryParameter::This:
        return &thisType;
    case EntryParameter::Parameter:
        return &parameters[entryParameterIndex - numberBefore(*this, kind)];
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

DeclaredType* Function::declaredEntryParameter(unsigned entryParameterIndex)
{
    return const_cast<DeclaredType*>(std::as_const(*this).declaredEntryParameter(entryParameterIndex));
}

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function& function)
{
    OptionSet<FunctionCapabilities> capabilities = capabilitiesOf(function.role);
    if (function.isUnsplitCoroutine())
        capabilities.remove({ FunctionCapabilities::MayBeInlined, FunctionCapabilities::HasOrdinaryFrame });
    return capabilities;
}

OptionSet<FunctionCapabilities> capabilitiesInPlaceOf(const Function& replacement, OptionSet<FunctionCapabilities> ofReplaced, OptionSet<FunctionCapabilities> alsoKept)
{
    OptionSet<FunctionCapabilities> kept = alsoKept | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost, FunctionCapabilities::ClosuresAreReadWithoutCode, FunctionCapabilities::NotesLooksForName, FunctionCapabilities::IsLongConstructor, FunctionCapabilities::MakesNoTailCallIfConstructed };
    return (initialCapabilitiesOf(replacement) - kept) | (ofReplaced & kept);
}

static bool hasEarlierPredecessors(const Function& function)
{
    unsigned count = function.blocks.size();
    BitVector hasEarlierPredecessor(count);
    for (unsigned index = 0; index < count; ++index) {
        if (BlockID(index) != entryBlock && !hasEarlierPredecessor.quickGet(index))
            return false;
        function.blocks[index].forEachTarget([&](const Target& target) {
            RELEASE_ASSERT(target.block.index() < count);
            if (target.block.index() > index)
                hasEarlierPredecessor.quickSet(target.block.index());
        });
    }
    return true;
}

static BitVector findReachableBlocks(const Function& function)
{
    unsigned count = function.blocks.size();
    BitVector isReachable(count);
    Vector<BlockID, 16> worklist;
    isReachable.quickSet(entryBlock.index());
    worklist.append(entryBlock);
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        function.at(block).forEachTarget([&](const Target& target) {
            RELEASE_ASSERT(target.block.index() < count);
            if (!isReachable.quickSet(target.block.index()))
                worklist.append(target.block);
        });
    }
    return isReachable;
}

static Vector<BlockID> removeUnreachableBlocks(Function& function, const BitVector& isReachable)
{
    unsigned count = function.blocks.size();
    if (isReachable.bitCount() == count)
        return { };

    Vector<BlockID> movedTo(count);
    Function::Blocks::Storage& blocks = function.editAllBlocks();
    unsigned next = 0;
    for (unsigned b = 0; b < count; ++b) {
        if (!isReachable.quickGet(b))
            continue;
        if (next != b)
            blocks[next] = WTF::move(blocks[b]);
        movedTo[b] = BlockID(next++);
    }
    blocks.shrink(next);
    for (auto& block : blocks) {
        block.forEachTarget([&](Target& target) {
            target.block = movedTo[target.block.index()];
        });
    }
    return movedTo;
}

static Vector<BlockID> sortIntoReversePostOrder(Function& function)
{
    CFG cfg(function);
    std::span<const BlockID> order = cfg.reversePostOrder();
    RELEASE_ASSERT(order.size() == function.blocks.size() && order[0] == entryBlock);

    Function::Blocks::Storage blocks;
    blocks.reserveInitialCapacity(order.size());
    for (BlockID block : order)
        blocks.append(WTF::move(function.editAllBlocks()[block.index()]));
    function.editAllBlocks() = WTF::move(blocks);

    for (auto& block : function.editAllBlocks()) {
        block.forEachTarget([&](Target& target) {
            target.block = BlockID(cfg.reversePostOrderIndex(target.block));
        });
    }
    return Vector<BlockID>(order.size(), [&](size_t index) {
        return BlockID(cfg.reversePostOrderIndex(BlockID(index)));
    });
}

static Vector<BlockID> movedFirstAndThen(Vector<BlockID>&& first, Vector<BlockID>&& then)
{
    if (first.isEmpty())
        return WTF::move(then);
    if (then.isEmpty())
        return WTF::move(first);
    for (BlockID& block : first) {
        if (block)
            block = then[block.index()];
    }
    return WTF::move(first);
}

static Vector<ValueID> renumberValues(Function& function, size_t numberOfListedValues)
{
    Vector<ValueID> newID(function.values.size());
    Function::Values::Storage values;
    Function::Values::Storage& formerValues = function.editAllValues();
    Function::Blocks::Storage& blocks = function.editAllBlocks();
    values.reserveInitialCapacity(numberOfListedValues);
    auto moveListedValue = [&](ValueID& id) {
        unsigned oldIndex = id.index();
        RELEASE_ASSERT(!newID[oldIndex]);
        id = ValueID(values.size());
        newID[oldIndex] = id;
        values.append(WTF::move(formerValues[oldIndex]));
    };
    for (auto& block : blocks) {
        for (ValueID& parameter : block.parameters)
            moveListedValue(parameter);
        for (ValueID& value : block.values)
            moveListedValue(value);
    }

    auto remapChild = [&](ValueID& child) {
        child = newID[child.index()];
        RELEASE_ASSERT(child);
    };
    for (auto& value : values)
        value.forEachChild(remapChild);
    for (auto& block : blocks)
        block.forEachChildOutsideValues(remapChild);

    formerValues = WTF::move(values);

    if (!function.writtenPositions.isEmpty()) {
        function.writtenPositions.removeAllMatching([&](WrittenPosition& written) {
            written.value = written.value.index() < newID.size() ? newID[written.value.index()] : ValueID();
            return !written.value;
        });
        AOT::sortInLine(function.writtenPositions, { }, [](const WrittenPosition& written) { return written.value.index(); });
    }
    return newID;
}

static size_t countListedValues(const Function& function)
{
    size_t count = 0;
    for (const Block& block : function.blocks)
        count += block.parameters.size() + block.values.size();
    return count;
}

bool needsCompaction(const Function& function)
{
    return function.hasBody() && (!hasEarlierPredecessors(function) || countListedValues(function) != function.values.size());
}

bool compactFunction(Function& function, Renumbering* renumbering)
{
    if (!function.hasBody())
        return false;

    bool valuesAreTypedAsDefined = function.valuesAreTypedAsDefined();
    bool wasOrdered = hasEarlierPredecessors(function);
    Vector<BlockID> movedBlocks;
    if (!wasOrdered) {
        movedBlocks = removeUnreachableBlocks(function, findReachableBlocks(function));
        if (!hasEarlierPredecessors(function))
            movedBlocks = movedFirstAndThen(WTF::move(movedBlocks), sortIntoReversePostOrder(function));
    }

    size_t numberOfListedValues = countListedValues(function);
    if (wasOrdered && numberOfListedValues == function.values.size())
        return false;

    Vector<ValueID> movedValues = renumberValues(function, numberOfListedValues);
    if (ReasonsForOperationsThatWent* listener = function.listenerForOperationsThatGo()) [[unlikely]]
        listener->didRenumber(movedBlocks.span(), movedValues.span());
    if (renumbering)
        *renumbering = { WTF::move(movedBlocks), WTF::move(movedValues) };
    if (valuesAreTypedAsDefined)
        function.setValuesAreTypedAsDefined();
    return true;
}

namespace {

struct FunctionProvisionalIDRewriter {
    void operator()(TypeID& id) const { types.apply(id); }
    void operator()(LayoutSetID& id) const { types.apply(id); }
    void operator()(CalleeSetID& id) const { types.apply(id); }
    void operator()(PropertyKeyID& id) const { types.apply(id); }
    void operator()(StringID& id) const { strings.apply(id); }
    void operator()(FunctionID& id) const { functions.apply(id); }
    template<typename Tag> void operator()(ID<Tag>&) const { }

    const TypeRemap& types;
    const StringRemap& strings;
    const FunctionRemap& functions;
};

} // anonymous namespace

namespace {

struct ForeignIDAdopter {
    void operator()(TypeID& id) const { id = types.adopt(id); }
    void operator()(LayoutSetID& id) const { id = types.adopt(id); }
    void operator()(CalleeSetID& id) const { id = types.adopt(id); }
    void operator()(PropertyKeyID& id) const { id = types.adopt(id); }
    template<typename Tag> void operator()(ID<Tag>&) const { }

    TypeArena& types;
};

} // anonymous namespace

namespace {

struct ForeignIDCounter {
    void operator()(TypeID& id) const { count += !types.owns(id); }
    void operator()(LayoutSetID& id) const { count += !types.owns(id); }
    void operator()(CalleeSetID& id) const { count += !types.owns(id); }
    void operator()(PropertyKeyID& id) const { count += !types.owns(id); }
    template<typename Tag> void operator()(ID<Tag>&) const { }

    const TypeArena& types;
    size_t& count;
};

} // anonymous namespace

size_t numberOfForeignIDs(const Function& constFunction, const TypeArena& types)
{
    Function& function = const_cast<Function&>(constFunction);
    size_t count = 0;
    ForeignIDCounter counter { types, count };
    forEachReference(static_cast<FunctionRecord&>(function), counter);
    for (Value& value : function.storageOfValues()) {
        count += !types.owns(value.type) + !types.owns(value.expected().predicted());
        forEachReference(value, counter);
    }
    for (Block& block : function.storageOfBlocks())
        forEachReference(block.terminator, counter);
    return count;
}

void adoptIDs(Function& function, TypeArena& types)
{
    bool valuesAreTypedAsDefined = function.valuesAreTypedAsDefined();
    ForeignIDAdopter adopter { types };
    forEachReference(static_cast<FunctionRecord&>(function), adopter);
    for (Value& value : function.editAllValues()) {
        value.type = types.adopt(value.type);
        TypeID expected = types.adopt(value.expectedForTyping().predicted());
        value.setExpected(ExpectedType(expected == value.type ? TypeID() : expected));
        forEachReference(value, adopter);
    }
    for (Block& block : function.editAllBlocks())
        forEachReference(block.terminator, adopter);
    if (valuesAreTypedAsDefined)
        function.setValuesAreTypedAsDefined();
}

void remapIDs(Function& function, const TypeRemap& types, const StringRemap& strings, const FunctionRemap& functions)
{
    if (types.isIdentity() && strings.isIdentity() && functions.isIdentity())
        return;

    bool valuesAreTypedAsDefined = function.valuesAreTypedAsDefined();
    FunctionProvisionalIDRewriter rewriter { types, strings, functions };
    forEachReference(static_cast<FunctionRecord&>(function), rewriter);
    for (Value& value : function.editAllValues()) {
        types.apply(value.type);
        TypeID expected = types(value.expectedForTyping().predicted());
        value.setExpected(ExpectedType(expected == value.type ? TypeID() : expected));
        forEachReference(value, rewriter);
    }
    for (Block& block : function.editAllBlocks())
        forEachReference(block.terminator, rewriter);
    if (valuesAreTypedAsDefined)
        function.setValuesAreTypedAsDefined();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
