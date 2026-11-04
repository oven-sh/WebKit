/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpected.h"
#include "AOTValue.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct Target {
    BlockID block;
    Arguments arguments;
    CompactList<uint32_t> proofs;

    friend bool operator==(const Target&, const Target&) = default;
};

template<> inline constexpr bool canMoveWithMemcpy<Target> = true;

using TargetList = InlineList<Target, 2>;

struct Terminator : Instruction {
    TerminatorKind kind { TerminatorKind::Unreachable };
    TargetList targets;

    Terminator() { resetOperandsAndPayload(numberOfNamedOperandsOf(TerminatorKind::Unreachable), defaultPayloadOf(TerminatorKind::Unreachable)); }

    explicit Terminator(TerminatorKind newKind) { become(newKind); }

    void become(TerminatorKind newKind)
    {
        kind = newKind;
        targets.clear();
        resetOperandsAndPayload(numberOfNamedOperandsOf(newKind), defaultPayloadOf(newKind));
    }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind, operandIndex); }

    bool isConditionalBranch() const { return kind == TerminatorKind::Branch || kind == TerminatorKind::TestAndBranch; }
    bool hasCases() const { return !numberOfFixedTargetsOf(kind); }

    size_t numberOfCases() const
    {
        switch (kind) {
        case TerminatorKind::Switch:
            return cases().size();
        case TerminatorKind::SwitchString:
            return strings().size();
        case TerminatorKind::SwitchOnShape:
            return layoutSets().size();
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return 0;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Target& next() LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::Jump); return targets[0]; }
    const Target& next() const LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::Jump); return targets[0]; }

    Target& taken() LIFETIME_BOUND { RELEASE_ASSERT(isConditionalBranch()); return targets[0]; }
    const Target& taken() const LIFETIME_BOUND { RELEASE_ASSERT(isConditionalBranch()); return targets[0]; }
    Target& notTaken() LIFETIME_BOUND { RELEASE_ASSERT(isConditionalBranch()); return targets[1]; }
    const Target& notTaken() const LIFETIME_BOUND { RELEASE_ASSERT(isConditionalBranch()); return targets[1]; }

    std::span<Target> caseTargets() LIFETIME_BOUND { RELEASE_ASSERT(hasCases()); return targets.mutableSpan().first(targets.size() - 1); }
    std::span<const Target> caseTargets() const LIFETIME_BOUND { RELEASE_ASSERT(hasCases()); return targets.span().first(targets.size() - 1); }
    Target& otherwise() LIFETIME_BOUND { RELEASE_ASSERT(hasCases()); return targets.last(); }
    const Target& otherwise() const LIFETIME_BOUND { RELEASE_ASSERT(hasCases()); return targets.last(); }

    Target& done() LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return targets[0]; }
    const Target& done() const LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return targets[0]; }
    Target& miss() LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return targets[1]; }
    const Target& miss() const LIFETIME_BOUND { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return targets[1]; }

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        Instruction::forEachChild(functor);
        for (Target& target : targets) {
            for (ValueID& argument : target.arguments)
                functor(argument);
        }
        touchForAddressSanitizer(this);
    }

    template<typename Functor>
    void forEachChild(const Functor& functor) const
    {
        Instruction::forEachChild(functor);
        for (const Target& target : targets) {
            for (const ValueID argument : target.arguments)
                functor(argument);
        }
        touchForAddressSanitizer(this);
    }

#include "AOTSchemaTerminatorAccessors.inc"
};

using BlockValues = InlineList<ValueID, 3>;

struct Block {
    CompactList<ValueID> parameters;
    BlockValues values;
    Terminator terminator;
    OutOfLineOptional<Target> handler;
    BlockRole role { BlockRole::Ordinary };
    Expected<BlockFrequency> frequency { BlockFrequency::Normal };
    CodeShape shape { CodeShape::Unchosen };
    ValueID subject;

    bool isHandlerBlock() const { return role != BlockRole::Ordinary; }

    void appendPartsTo(BodyWords& words) const
    {
        parameters.appendStorageTo(words);
        values.appendStorageTo(words);
        terminator.appendWordsTo(words);
        terminator.targets.appendStorageTo(words);
        handler.appendStorageTo(words);
        forEachTarget([&](const Target& target) {
            target.arguments.appendStorageTo(words);
            target.proofs.appendStorageTo(words);
        });
    }
    // This is a copy, byte for byte, of a block that was written with appendPartsTo().
    void takePartsFrom(BodyWordsReader& reader)
    {
        parameters.takeStorageFrom(reader);
        values.takeStorageFrom(reader);
        terminator.takeWordsFrom(reader);
        terminator.targets.takeStorageFrom(reader);
        handler.takeStorageFrom(reader);
        forEachTarget([&](Target& target) {
            target.arguments.takeStorageFrom(reader);
            target.proofs.takeStorageFrom(reader);
        });
    }

    template<typename Functor>
    void forEachTarget(const Functor& functor)
    {
        for (Target& target : terminator.targets)
            functor(target);
        if (handler)
            functor(*handler);
        touchForAddressSanitizer(this);
    }

    template<typename Functor>
    void forEachTarget(const Functor& functor) const
    {
        for (const Target& target : terminator.targets)
            functor(target);
        if (handler)
            functor(*handler);
        touchForAddressSanitizer(this);
    }

    template<typename Functor>
    void forEachChildOutsideValues(const Functor& functor)
    {
        terminator.forEachChild(functor);
        if (handler) {
            for (ValueID& argument : handler->arguments)
                functor(argument);
        }
        if (subject)
            functor(subject);
        touchForAddressSanitizer(this);
    }

    template<typename Functor>
    void forEachChildOutsideValues(const Functor& functor) const
    {
        terminator.forEachChild(functor);
        if (handler) {
            for (const ValueID argument : handler->arguments)
                functor(argument);
        }
        if (subject)
            functor(subject);
        touchForAddressSanitizer(this);
    }
};

static_assert(sizeof(Target) == 20 && sizeof(Block) <= 120);

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct VectorTraits<JSC::AOT::Typed::Block> : VectorTraitsBase<false, JSC::AOT::Typed::Block> {
    static constexpr bool canMoveWithMemcpy = true;
};

} // namespace WTF

#endif // ENABLE(AOT)
