/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// For Guesses: the ladder of names. It only says what is expected of a value, which a test of a guess then asks at the use, so no suspension and no throw concerns it.
// Withheld: nobody asks.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTLayoutsByNames.h"
#include "AOTLinkStrings.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// How much an object that lacks the name would be missed. A store creates the name, a plain read of what is not there gives undefined, and what is called or looked into throws where it is not there.
enum class DemandOfNameInStringStrays : uint8_t { Stored, Read, Required };
constexpr unsigned numberOfDemandsInStringStrays = 3;
constexpr unsigned numberOfRungsInStringStrays = 2 * numberOfDemandsInStringStrays;
constexpr uint32_t noResultInStringStrays = UINT32_MAX;

struct NameAskedInStringStrays {
    uint32_t root { 0 };
    PropertyKeyID name;
    DemandOfNameInStringStrays demand { DemandOfNameInStringStrays::Stored };
    bool isAskedOnEveryWay { false };
    // Of a read: what it answers with.
    uint32_t result { noResultInStringStrays };
    uint32_t block { 0 };
};

// What the operations of one body ask of its values by name, each value under what it stands for. It follows from the body alone.
struct NamesAskedInBodyInStringStrays {
    // By value, as Walker::namesUsedOff() has them: what only hands a value on stands for it.
    Vector<uint32_t> roots;
    // By root, and one more.
    Vector<uint32_t> firstNameOfRoot;
    // Ascending by root and name, each pair once.
    Vector<NameAskedInStringStrays> names;
};

struct KeptByLadderOfNamesInStringStrays final : Walker::KeptForBuild {
    HashMap<FunctionID, std::unique_ptr<NamesAskedInBodyInStringStrays>> bodies;
};

} // anonymous namespace

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
