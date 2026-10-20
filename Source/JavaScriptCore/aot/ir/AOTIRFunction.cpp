/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRFunction.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
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

    // Diagnostics ask for locations in functions that failed verification, so a malformed frame list gives no location.
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

static void removeUnreachableBlocks(Function& function, const BitVector& isReachable)
{
    unsigned count = function.blocks.size();
    if (isReachable.bitCount() == count)
        return;

    Vector<BlockID> movedTo(count);
    unsigned next = 0;
    for (unsigned b = 0; b < count; ++b) {
        if (!isReachable.quickGet(b))
            continue;
        if (next != b)
            function.blocks[next] = WTF::move(function.blocks[b]);
        movedTo[b] = BlockID(next++);
    }
    function.blocks.shrink(next);
    for (auto& block : function.blocks) {
        block.forEachTarget([&](Target& target) {
            target.block = movedTo[target.block.index()];
        });
    }
}

static void sortIntoReversePostOrder(Function& function)
{
    CFG cfg(function);
    std::span<const BlockID> order = cfg.reversePostOrder();
    RELEASE_ASSERT(order.size() == function.blocks.size() && order[0] == entryBlock);

    decltype(function.blocks) blocks;
    blocks.reserveInitialCapacity(order.size());
    for (BlockID block : order)
        blocks.append(WTF::move(function.at(block)));
    function.blocks = WTF::move(blocks);

    for (auto& block : function.blocks) {
        block.forEachTarget([&](Target& target) {
            target.block = BlockID(cfg.reversePostOrderIndex(target.block));
        });
    }
}

static void renumberValues(Function& function, size_t numberOfListedValues)
{
    Vector<ValueID> newID(function.values.size());
    decltype(function.values) values;
    values.reserveInitialCapacity(numberOfListedValues);
    auto moveListedValue = [&](ValueID& id) {
        unsigned oldIndex = id.index();
        RELEASE_ASSERT(!newID[oldIndex]);
        id = ValueID(values.size());
        newID[oldIndex] = id;
        values.append(WTF::move(function.values[oldIndex]));
    };
    for (auto& block : function.blocks) {
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
    for (auto& block : function.blocks)
        block.forEachChildOutsideValues(remapChild);

    function.values = WTF::move(values);
}

bool compactFunction(Function& function)
{
    if (!function.hasBody())
        return false;

    // By induction a block with an earlier predecessor is reachable, so a valid order has no unreachable block.
    bool wasOrdered = hasEarlierPredecessors(function);
    if (!wasOrdered) {
        removeUnreachableBlocks(function, findReachableBlocks(function));
        if (!hasEarlierPredecessors(function))
            sortIntoReversePostOrder(function);
    }

    size_t numberOfListedValues = 0;
    for (const Block& block : function.blocks)
        numberOfListedValues += block.parameters.size() + block.values.size();
    if (wasOrdered && numberOfListedValues == function.values.size())
        return false;

    renumberValues(function, numberOfListedValues);
    return true;
}

namespace {

struct ProvisionalIDRewriter {
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

void remapIDs(Function& function, const TypeRemap& types, const StringRemap& strings, const FunctionRemap& functions)
{
    if (types.isIdentity() && strings.isIdentity() && functions.isIdentity())
        return;

    ProvisionalIDRewriter rewriter { types, strings, functions };
    forEachReference(static_cast<FunctionRecord&>(function), rewriter);
    for (Value& value : function.values) {
        types.apply(value.type);
        value.setWritten(types(value.written()));
        value.setPrediction(types(value.prediction()));
        forEachReference(value, rewriter);
    }
    for (Block& block : function.blocks)
        forEachReference(block.terminator, rewriter);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
