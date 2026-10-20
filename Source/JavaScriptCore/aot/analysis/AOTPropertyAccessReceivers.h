/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGuardAvailability.h"
#include "AOTIR.h"
#include "AOTSortedSet.h"
#include <compare>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;

using LayoutList = SortedSet<LayoutID, 4>;

struct PropertyAccessContext {
    const Program& program;
    const FlowResults* flow { nullptr };
};

struct ReceiverState {
    OptionSet<ObjectMutation> impossibleMutations;
    KeyEnumeration keys { KeyEnumeration::Unknown };
    bool addedKeysAreComplete { false };

    bool cannot(ObjectMutation mutation) const { return impossibleMutations.contains(mutation); }
    void meetWith(const ReceiverState&);

    friend bool operator==(const ReceiverState&, const ReceiverState&) = default;
};

ReceiverState receiverStateOf(const Program&, LayoutID);
ReceiverState receiverStateOf(const Program&, std::span<const LayoutID>);

enum class OwnPropertyGain : uint8_t { Unknown, Impossible };

OwnPropertyGain ownPropertyGainOf(const Program&, LayoutID, PropertyKeyID);

enum class LayoutTestKind : uint8_t { Shape, Type };

LayoutTestKind layoutTestKindFor(const Program&, std::span<const LayoutID>);

enum class InstanceCount : uint8_t { Unknown, AtMostOne };

InstanceCount instanceCountOf(const PropertyAccessContext&);

struct MemberCondition {
    enum class Kind : uint8_t { MemberUnchanged, PrototypeLinkUnchanged, ClassEvaluatedOnce };

    Kind kind { Kind::MemberUnchanged };
    LayoutID layout;
    ClassSide side { ClassSide::Prototype };
    PropertyKeyID key;

    friend bool operator==(const MemberCondition&, const MemberCondition&) = default;
    friend auto operator<=>(const MemberCondition&, const MemberCondition&) = default;
};

using MemberConditions = Vector<MemberCondition, 1>;

void addCondition(MemberConditions&, const MemberCondition&);
void addConditions(MemberConditions&, std::span<const MemberCondition>);

enum class ReceiverProof : uint8_t { Unknown, Guard, Type };

struct ReceiverCandidates {
    ReceiverProof proof { ReceiverProof::Unknown };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    LayoutList proven;
    LayoutList predicted;
    bool hasOtherParts { false };
    bool mayBeNullish { false };

    static ReceiverCandidates underGuard(std::span<const LayoutID> testedLayouts);

    bool isProven() const { return proof != ReceiverProof::Unknown; }

    void dump(PrintStream&) const;
};

// The allocated layouts whose objects have every one of the names, each as a field or as a member of a class above them.
LayoutList allocatedLayoutsWith(const Program&, std::span<const PropertyKeyID> names);

ReceiverCandidates candidatesOf(const Program&, const Type& receiver, const Type* prediction, LayoutSetID stamp);
ReceiverCandidates receiverCandidates(const Program&, const Function&, ValueID receiver, LayoutSetID stamp);

enum class UnionPartKind : uint8_t {
    OtherObjects,
    Nullish,
    Boolean,
    Number,
    String,
    Symbol,
    BigInt,
    Absent,
    Layouts,
    BuiltinClass,
    Functions,
    Builtins,
};

struct UnionPart {
    UnionPartKind kind { UnionPartKind::OtherObjects };
    Type type;
};

using UnionParts = Vector<UnionPart, 4>;

UnionParts unionPartsOf(const Type&);
bool isClosedUnion(const UnionParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
