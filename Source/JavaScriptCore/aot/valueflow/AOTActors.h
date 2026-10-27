/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <array>
#include <bit>
#include <cstdint>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

// Everybody who can put a value somewhere, take one out, or enter code. Every switch names each.
enum class Actor : uint8_t {
    // Operations of bodies that flow has solved, the compiled library's too, with receiver and key both listed.
    Statements = 1 << 0,
    // What the operation, the class definition or the engine lays on an object as it makes it.
    Creation = 1 << 1,
    // Members that a holder of the table lists, and what a row says its built-in writes on what it makes and on its operands.
    Table = 1 << 2,
    // Engine and host where no row speaks.
    NativeCode = 1 << 3,
    // Links, namespace objects, synthetic exports, module.exports, and when each file begins and ends.
    Loader = 1 << 4,
    // A seen write whose key flow cannot list.
    KeysNobodyKnows = 1 << 5,
    // A seen write through the unknown or the untracked object.
    ReceiversNobodyKnows = 1 << 6,
    // Evaluated text, files from disk, addons, and bodies of the program that flow has not solved.
    UnseenCode = 1 << 7,
};

inline constexpr unsigned numberOfActors = 8;
inline constexpr std::array<Actor, numberOfActors> actorsInOrder { Actor::Statements, Actor::Creation, Actor::Table, Actor::NativeCode, Actor::Loader, Actor::KeysNobodyKnows, Actor::ReceiversNobodyKnows, Actor::UnseenCode };
inline constexpr OptionSet<Actor> everyActor = OptionSet<Actor>::fromRaw(0xff);

constexpr unsigned indexOf(Actor actor) { return std::countr_zero(static_cast<unsigned>(actor)); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
