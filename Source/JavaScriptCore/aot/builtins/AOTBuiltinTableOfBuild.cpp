/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinNameSlots.h"
#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>

// What a test run unlocks. It runs once per build, over short lists.

namespace JSC { namespace AOT {

enum class NameIs : bool { OfMember, New };

// What the language, an iterator's user or a call reads without a lookup that compiled code could test.
// A conversion to a primitive or to JSON looks `toString`, `valueOf` and `toJSON` up like any call that names them, where the object has such a member.
// So does what resolves a promise with a promise look `then` up. `await` of a promise of the language reads no `then`, replaced or not.
static bool isReadWithoutLookup(const BuiltinTable& table, BuiltinName name, NameIs nameIs)
{
    // No function of the engine gets a `call`, `apply` or `bind` of its own. The members that have these names are read by whoever names them, and by nobody else: a spread call reads no `apply`.
    bool isLookedUpByCalls = nameIs == NameIs::OfMember && (name == BuiltinNames::call || name == BuiltinNames::apply || name == BuiltinNames::bind);
    if (table.isSymbolName(name) || (table.isClosedOnHostFunctions(name) && !isLookedUpByCalls) || name == BuiltinNames::next || name == BuiltinNames::returnKeyword || name == BuiltinNames::throwKeyword)
        return true;
    for (Protocol protocol : allMembersOf<Protocol>) {
        // A `toJSON` that a test adds, to Object.prototype say, is looked up too: what converts to JSON is left to the host where the name is opened.
        if (protocol == Protocol::ToJSON || (nameIs == NameIs::OfMember && (protocol == Protocol::ToPrimitive || protocol == Protocol::Thenable)))
            continue;
        if (std::ranges::contains(table.keysReadBy(protocol), name))
            return true;
    }
    return false;
}

bool BuiltinTable::canBeReplacedInTestRuns(BuiltinID id) const
{
    const Builtin& member = at(id);
    if (member.mutability != BuiltinMutability::AlwaysLocked || member.scope != BuiltinScope::Realm || !member.holder || !member.name)
        return false;
    if (member.flags.containsAny({ BuiltinFlag::IsOwnPropertyOfInstances, BuiltinFlag::IsCompilerHelper, BuiltinFlag::IsDefaultExport }))
        return false;
    switch (member.kind) {
    case BuiltinKind::Function:
    case BuiltinKind::Method:
    case BuiltinKind::Accessor:
    // `process.versions`, `process.release`, `process.isBun`: the front end folds no read of them in a test run. The others have names that are read without a lookup.
    case BuiltinKind::DataProperty:
        break;
    case BuiltinKind::Constructor:
    case BuiltinKind::Namespace:
        // `globalThis.Set = Fake`, `globalThis.Math = Object.create(Math)`. Under any other holder a constructor is the `constructor` of a prototype object.
        if (!at(member.holder).flags.contains(BuiltinFlag::IsGlobalObject))
            return false;
        break;
    case BuiltinKind::Prototype:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    // Whoever walks a map or a set asks what its prototype holds under @@iterator, and an object of the host is walked by the protocol alone.
    if (member.name == nameOf(WellKnownSymbol::Iterator)) {
        BuiltinID holder = canonicalID(member.holder);
        if (holder == canonicalID(Builtins::MapPrototype) || holder == canonicalID(Builtins::SetPrototype) || at(holder).origin == BuiltinOrigin::Host)
            return true;
    }
    return !isReadWithoutLookup(*this, member.name, NameIs::OfMember);
}

bool BuiltinTable::canTakeNameInTestRuns(BuiltinID holder, BuiltinName name) const
{
    const Builtin& object = at(holder);
    if (!object.flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || object.flags.contains(BuiltinFlag::IsGlobalObject) || object.scope != BuiltinScope::Realm)
        return false;
    if (!name)
        return true;
    // A key that is `undefined`, `null`, `true` or `false` is looked up under its text.
    for (ASCIILiteral text : { "undefined"_s, "null"_s, "true"_s, "false"_s }) {
        if (equalSpans(textOf(name), byteCast<uint8_t>(text.span())))
            return false;
    }
    return !isReadWithoutLookup(*this, name, NameIs::New) && !findOwnMember(holder, name);
}

BuiltinTableOfBuild::BuiltinTableOfBuild(const BuiltinTable& table)
    : m_data(table.m_data)
    , m_table(m_data)
{
}

// As Function.prototype is locked: each member by its name, closed to every other text and to the symbols of the language.
void BuiltinTableOfBuild::lockByNames(BuiltinID wholeObject)
{
    wholeObject = m_table.canonicalID(wholeObject);
    if (!m_builtins[wholeObject.index()].flags.contains(BuiltinFlag::IsLockedObject))
        return;
    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        Builtin& entry = m_builtins[index];
        if (BuiltinID(index) != wholeObject && entry.sameObjectAs != wholeObject)
            continue;
        if (entry.flags.contains(BuiltinFlag::IsLockedObject))
            entry.flags.add({ BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames });
        entry.flags.remove(BuiltinFlag::IsLockedObject);
    }
    m_lockedObjects.removeFirst(wholeObject);

    Vector<BuiltinName> symbols;
    for (uint32_t index = 0; index < m_table.nameCount(); ++index) {
        if (m_table.isSymbolName(BuiltinName(index)) && !m_table.findOwnMember(wholeObject, BuiltinName(index)))
            symbols.append(BuiltinName(index));
    }
    m_closedSymbols.append(WTF::move(symbols));
    size_t position = std::ranges::lower_bound(m_closedKeys, wholeObject, { }, &BuiltinClosedKeys::holder) - m_closedKeys.begin();
    RELEASE_ASSERT(position == m_closedKeys.size() || m_closedKeys[position].holder != wholeObject);
    m_closedKeys.insert(position, BuiltinClosedKeys { wholeObject, m_closedSymbols.last().span(), { } });
}

// The names that no test run adds stay closed, each by itself.
void BuiltinTableOfBuild::openToNames(BuiltinID holder)
{
    lockByNames(holder);
    Vector<BuiltinName> closed(m_table.closedKeysOf(holder));
    size_t position = std::ranges::lower_bound(m_closedKeys, holder, { }, &BuiltinClosedKeys::holder) - m_closedKeys.begin();
    bool hasRecord = position < m_closedKeys.size() && m_closedKeys[position].holder == holder;
    if (hasRecord)
        closed = Vector<BuiltinName>(m_closedKeys[position].names);
    for (uint32_t index = 0; index < m_table.nameCount(); ++index) {
        BuiltinName name(index);
        if (name && !m_table.isSymbolName(name) && !m_table.findOwnMember(holder, name) && !m_table.canTakeNameInTestRuns(holder, name) && !std::ranges::binary_search(m_table.openNamesOf(holder), name))
            closed.append(name);
    }
    AOT::sort(closed);
    closed.shrink(closed.size() - std::ranges::unique(closed).size());
    m_closedSymbols.append(WTF::move(closed));
    if (hasRecord)
        m_closedKeys[position] = BuiltinClosedKeys { holder, m_closedSymbols.last().span(), { } };
    else
        m_closedKeys.insert(position, BuiltinClosedKeys { holder, m_closedSymbols.last().span(), { } });
    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        if (BuiltinID(index) == holder || m_builtins[index].sameObjectAs == holder)
            m_builtins[index].flags.remove(BuiltinFlag::IsClosedToOtherNames);
    }
}

void BuiltinTableOfBuild::unlock(std::span<const BuiltinID> members, std::span<const BuiltinID> holdersOfNewNames, bool opensEveryHolder)
{
    RELEASE_ASSERT(m_builtins.isEmpty());
    for (BuiltinID member : members) {
        // A member that is constant unless written is listed for the sake of an image that does not hold the write: it is not to lock the member at its start.
        if (m_table.contains(member) && (m_table.canBeReplacedInTestRuns(member) || m_table.at(member).mutability == BuiltinMutability::ConstantUnlessWritten))
            m_unlockedMembers.append(member);
    }
    if (m_unlockedMembers.isEmpty() && holdersOfNewNames.empty() && !opensEveryHolder)
        return;
    m_builtins = Vector<Builtin>(m_data.builtins);
    m_lockedObjects = Vector<BuiltinID>(m_data.runtime.lockedObjects);
    m_closedKeys = Vector<BuiltinClosedKeys>(m_data.runtime.closedKeys);
    for (BuiltinID member : m_data.runtime.lockedMembers) {
        if (!std::ranges::binary_search(m_unlockedMembers, member))
            m_lockedMembers.append(member);
    }
    for (BuiltinID member : m_unlockedMembers) {
        m_builtins[member.index()].mutability = BuiltinMutability::ConstantUnlessWritten;
        lockByNames(m_builtins[member.index()].holder);
    }
    for (BuiltinID holder : holdersOfNewNames)
        lockByNames(holder);
    for (uint32_t index = 0; opensEveryHolder && index < m_builtins.size(); ++index) {
        if (m_table.canonicalID(BuiltinID(index)) == BuiltinID(index) && m_table.canTakeNameInTestRuns(BuiltinID(index), { }))
            openToNames(BuiltinID(index));
    }
    m_data.builtins = m_builtins.span();
    m_data.runtime.lockedMembers = m_lockedMembers.span();
    m_data.runtime.lockedObjects = m_lockedObjects.span();
    m_data.runtime.closedKeys = m_closedKeys.span();
}

namespace Typed {

void Program::unlockReplaceableBuiltins()
{
    Vector<BuiltinID> members;
    // A member that is constant unless written never had a lock of the table, and no row for a pristine operand rests on it: flow answers for it, as it does in a program that holds the write.
    // It is listed so that this image does not lock it. An image over others takes over hundreds of them, and `constructor`, `toString` and `toJSON` are among their names.
    BitVector losesLockOfTable(m_builtins.builtinCount());
    for (const ReplaceableBuiltin& entry : replaceableBuiltins) {
        members.append(entry.member);
        if (m_builtins.contains(entry.member) && m_builtins.canBeReplacedInTestRuns(entry.member))
            losesLockOfTable.quickSet(entry.member.index());
    }
    openedNames.removeAllMatching([&](OpenedName& opened) {
        opened.holder = m_builtins.canonicalID(opened.holder);
        return !m_builtins.canTakeNameInTestRuns(opened.holder, m_builtins.nameOf(strings[opened.name]));
    });
    Vector<BuiltinID> holdersOfNewNames;
    for (const OpenedName& opened : openedNames) {
        holdersOfNewNames.append(opened.holder);
        m_namesOpenedSomewhere.set(opened.name.index());
    }
    m_tableOfBuild.unlock(members.span(), holdersOfNewNames.span(), takesAnyNameFromTests);
    m_testsReplaceBuiltins = takesAnyNameFromTests || !openedNames.isEmpty() || !losesLockOfTable.isEmpty();
    if (unlockedBuiltins().empty() && openedNames.isEmpty())
        return;

    BitVector unlockedNames(m_builtins.nameCount());
    for (BuiltinID member : unlockedBuiltins()) {
        if (losesLockOfTable.quickGet(member.index()))
            unlockedNames.quickSet(m_builtins.at(member).name.index());
    }
    // `Object.prototype.toJSON = f`: every object has the member from then on.
    for (const OpenedName& opened : openedNames) {
        if (BuiltinName name = m_builtins.nameOf(strings[opened.name]))
            unlockedNames.quickSet(name.index());
    }
    auto looksUpUnlockedName = [&](const BuiltinParameter& parameter) {
        auto isUnlocked = [&](BuiltinName name) {
            return unlockedNames.quickGet(name.index());
        };
        for (Protocol protocol : parameter.protocols) {
            if (std::ranges::any_of(m_builtins.keysReadBy(protocol), isUnlocked))
                return true;
        }
        return std::ranges::any_of(parameter.namedMembers, isUnlocked) || std::ranges::any_of(parameter.calledMembers, isUnlocked) || std::ranges::any_of(parameter.convertedMembers, isUnlocked);
    };
    const std::pair<BuiltinID, BuiltinID> constructorsAndWhatAddsTheirEntries[] = {
        { Builtins::Map, Builtins::MapPrototypeSet }, { Builtins::Set, Builtins::SetPrototypeAdd },
        { Builtins::WeakMap, Builtins::WeakMapPrototypeSet }, { Builtins::WeakSet, Builtins::WeakSetPrototypeAdd },
    };
    m_functionsCallingUnlockedMembersThatNoRowNames = BitVector(m_builtins.builtinCount());
    for (auto [constructor, member] : constructorsAndWhatAddsTheirEntries) {
        if (isUnlocked(member))
            m_functionsCallingUnlockedMembersThatNoRowNames.quickSet(m_builtins.canonicalID(constructor).index());
    }
    // A row that takes everything from an operand names nothing: Set.prototype.union asks the other set for `has`, `keys` and `size`.
    // So a member that is unlocked anywhere above the instances of a class counts for each function with such a row and a row for a pristine instance of that class.
    auto hasUnlockedMemberAbove = [&](BuiltinClassID instanceClass) {
        for (BuiltinClassID current = instanceClass; current; current = m_builtins.at(current).parent) {
            BuiltinID prototype = m_builtins.at(current).prototype;
            bool isHolder = prototype && std::ranges::any_of(unlockedBuiltins(), [&](BuiltinID member) {
                return losesLockOfTable.quickGet(member.index()) && m_builtins.canonicalID(m_builtins.at(member).holder) == m_builtins.canonicalID(prototype);
            });
            if (isHolder)
                return true;
        }
        return false;
    };
    auto forEachParameter = [](const BuiltinOverload& overload, NOESCAPE const Invocable<bool(const BuiltinParameter&)> auto& predicate) {
        return (overload.receiver && predicate(*overload.receiver)) || std::ranges::any_of(overload.parameters, predicate);
    };
    auto asksOperandForUnlockedMemberWithoutName = [&](std::span<const BuiltinOverload> overloads) {
        return std::ranges::any_of(overloads, [&](const BuiltinOverload& special) {
            return forEachParameter(special, [&](const BuiltinParameter& pristine) {
                bool isAbove = std::ranges::any_of(m_builtins.at(pristine.accepted).parts, [&](const BuiltinTypePart& part) {
                    return !part.pristine.isEmpty() && part.builtinClass && hasUnlockedMemberAbove(part.builtinClass);
                });
                return isAbove && std::ranges::any_of(overloads, [&](const BuiltinOverload& general) {
                    return general.mode == special.mode && forEachParameter(general, [&](const BuiltinParameter& parameter) {
                        return parameter.operand == pristine.operand && parameter.uses == OperandUses::all();
                    });
                });
            });
        });
    };

    // A function that calls such a function under its name is one too: String.prototype.replace calls @@replace, which calls `exec`.
    m_functionsLookingUpUnlockedNames = BitVector(m_builtins.builtinCount());
    for (bool didMark = true; std::exchange(didMark, false);) {
        for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
            std::span<const BuiltinOverload> overloads = m_builtins.overloadsOf(BuiltinID(index));
            if (overloads.empty() || m_functionsLookingUpUnlockedNames.quickGet(m_builtins.canonicalID(BuiltinID(index)).index()))
                continue;
            bool looksUp = asksOperandForUnlockedMemberWithoutName(overloads) || std::ranges::any_of(overloads, [&](const BuiltinOverload& overload) {
                return forEachParameter(overload, looksUpUnlockedName);
            });
            if (!looksUp)
                continue;
            m_functionsLookingUpUnlockedNames.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
            if (BuiltinName name = m_builtins.at(BuiltinID(index)).name)
                didMark |= !unlockedNames.quickSet(name.index());
        }
    }

    // So far a function is marked whichever object holds the member. A row for a pristine instance leaves out what a lookup ON THE INSTANCE finds, and nothing stands in the way of that
    // lookup but the chain of its class. What follows marks fewer functions, class by class, and never one that is not marked above.
    auto isMarked = [&](BuiltinID function) {
        return m_functionsLookingUpUnlockedNames.quickGet(m_builtins.canonicalID(function).index());
    };
    auto forEachClassFrom = [&](BuiltinClassID instanceClass, NOESCAPE const Invocable<bool(BuiltinClassID)> auto& predicate) {
        for (BuiltinClassID current = instanceClass; current; current = m_builtins.at(current).parent) {
            if (predicate(current))
                return true;
        }
        return false;
    };
    unsigned classCount = m_builtins.classCount();
    BitVector classesOfPristineParts(classCount);
    BitVector isWanted(m_builtins.builtinCount());
    Vector<BuiltinID> functionsWithRowForPristineInstance;
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        if (!isMarked(BuiltinID(index)))
            continue;
        bool hasSuchRow = false;
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(BuiltinID(index))) {
            forEachParameter(overload, [&](const BuiltinParameter& parameter) {
                for (const BuiltinTypePart& part : m_builtins.at(parameter.accepted).parts) {
                    if (part.pristine.isEmpty() || !part.builtinClass)
                        continue;
                    hasSuchRow = true;
                    classesOfPristineParts.quickSet(part.builtinClass.index());
                }
                return false;
            });
        }
        if (hasSuchRow && !isWanted.quickSet(m_builtins.canonicalID(BuiltinID(index)).index()))
            functionsWithRowForPristineInstance.append(m_builtins.canonicalID(BuiltinID(index)));
    }

    struct Chain {
        BuiltinClassID instanceClass;
        BitVector closedNames;
        // Something that a test may have put there is found on an instance, under whatever name.
        bool holdsWhatTestsPut { false };
        // The marked functions that a lookup on an instance finds, each with the name that finds it.
        Vector<std::pair<BuiltinID, BuiltinName>> functionsFound;
    };
    auto walk = [&](BuiltinClassID instanceClass) -> std::optional<Chain> {
        BuiltinID holder = m_builtins.at(instanceClass).prototype;
        if (!holder)
            return std::nullopt;
        Chain chain { instanceClass, BitVector(m_builtins.nameCount()), false, { } };
        BitVector seen(m_builtins.nameCount());
        holder = m_builtins.canonicalID(holder);
        while (true) {
            for (const OpenedName& opened : openedNames) {
                if (opened.holder != holder)
                    continue;
                chain.holdsWhatTestsPut = true;
                if (BuiltinName name = m_builtins.nameOf(strings[opened.name]); name && !seen.quickSet(name.index()))
                    chain.closedNames.quickSet(name.index());
            }
            for (BuiltinID member : m_builtins.membersOf(holder)) {
                BuiltinName name = m_builtins.at(member).name;
                if (!name || seen.quickGet(name.index()))
                    continue;
                if (losesLockOfTable.quickGet(member.index()) && isUnlocked(member)) {
                    chain.holdsWhatTestsPut = true;
                    chain.closedNames.quickSet(name.index());
                }
                if (isMarked(member))
                    chain.functionsFound.append({ m_builtins.canonicalID(member), name });
                // Only what is there for certain, and stays, hides what stands under the same name further up.
                if (m_builtins.at(member).mutability == BuiltinMutability::AlwaysLocked && !m_builtins.has(BuiltinMark::MayBeAbsent, member))
                    seen.quickSet(name.index());
            }
            switch (m_builtins.at(holder).prototypeLink) {
            case BuiltinPrototypeLink::Unknown:
                return std::nullopt;
            case BuiltinPrototypeLink::Null:
                return chain;
            case BuiltinPrototypeLink::Builtin:
                holder = m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom);
                break;
            }
        }
    };
    // A part of class TypedArray stands for a Uint8Array too, whose chain is longer.
    m_classesWithKnownChains = classesOfPristineParts;
    Vector<Chain> chains;
    for (uint32_t index = 0; index < classCount; ++index) {
        BuiltinClassID instanceClass(index);
        bool standsForPristinePart = forEachClassFrom(instanceClass, [&](BuiltinClassID above) {
            return classesOfPristineParts.quickGet(above.index());
        });
        if (!standsForPristinePart)
            continue;
        if (std::optional<Chain> chain = walk(instanceClass)) {
            for (auto [function, name] : chain->functionsFound)
                isWanted.quickSet(function.index());
            chains.append(WTF::move(*chain));
            continue;
        }
        forEachClassFrom(instanceClass, [&](BuiltinClassID above) {
            m_classesWithKnownChains.quickClear(above.index());
            return false;
        });
    }

    struct Lookups {
        Vector<BuiltinName> names;
        // It has a row for a pristine instance of the class beside a row that takes everything from the same operand, and so names nothing.
        Vector<BuiltinClassID> classesAskedWithoutName;
    };
    constexpr uint32_t hasNoLookups = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> positionOfLookups(FillWith { }, m_builtins.builtinCount(), hasNoLookups);
    Vector<Lookups> lookups;
    for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
        std::span<const BuiltinOverload> overloads = m_builtins.overloadsOf(BuiltinID(index));
        uint32_t function = m_builtins.canonicalID(BuiltinID(index)).index();
        if (overloads.empty() || !isWanted.quickGet(function))
            continue;
        if (positionOfLookups[function] == hasNoLookups) {
            positionOfLookups[function] = lookups.size();
            lookups.append(Lookups { });
        }
        Lookups& ofFunction = lookups[positionOfLookups[function]];
        for (const BuiltinOverload& special : overloads) {
            forEachParameter(special, [&](const BuiltinParameter& parameter) {
                for (Protocol protocol : parameter.protocols)
                    ofFunction.names.append(m_builtins.keysReadBy(protocol));
                for (const BuiltinNameStretch& stretch : { parameter.namedMembers, parameter.calledMembers, parameter.convertedMembers }) {
                    for (BuiltinName name : stretch)
                        ofFunction.names.append(name);
                }
                bool isBesideRowThatTakesEverything = std::ranges::any_of(overloads, [&](const BuiltinOverload& general) {
                    return general.mode == special.mode && forEachParameter(general, [&](const BuiltinParameter& other) {
                        return other.operand == parameter.operand && other.uses == OperandUses::all();
                    });
                });
                for (const BuiltinTypePart& part : m_builtins.at(parameter.accepted).parts) {
                    if (isBesideRowThatTakesEverything && !part.pristine.isEmpty() && part.builtinClass)
                        ofFunction.classesAskedWithoutName.append(part.builtinClass);
                }
                return false;
            });
        }
    }
    for (Lookups& ofFunction : lookups) {
        AOT::sort(ofFunction.names);
        ofFunction.names.shrink(ofFunction.names.size() - std::ranges::unique(ofFunction.names).size());
    }

    m_functionsLookingUpUnlockedNamesOnInstances = Vector<BitVector>(classCount);
    for (Chain& chain : chains) {
        auto looksUpWhatTestsPut = [&](BuiltinID function) {
            if (positionOfLookups[function.index()] == hasNoLookups)
                return false;
            const Lookups& ofFunction = lookups[positionOfLookups[function.index()]];
            bool looksUpName = std::ranges::any_of(ofFunction.names, [&](BuiltinName name) {
                return chain.closedNames.quickGet(name.index());
            });
            return looksUpName || (chain.holdsWhatTestsPut && std::ranges::any_of(ofFunction.classesAskedWithoutName, [&](BuiltinClassID asked) {
                return forEachClassFrom(chain.instanceClass, [&](BuiltinClassID above) {
                    return above == asked;
                });
            }));
        };
        // A function that calls such a function under its name is one too, where the name finds it on an instance of this class.
        for (bool didClose = true; std::exchange(didClose, false);) {
            for (auto [function, name] : chain.functionsFound) {
                if (!chain.closedNames.quickGet(name.index()) && looksUpWhatTestsPut(function)) {
                    chain.closedNames.quickSet(name.index());
                    didClose = true;
                }
            }
        }
        BitVector functions(m_builtins.builtinCount());
        for (BuiltinID function : functionsWithRowForPristineInstance) {
            if (looksUpWhatTestsPut(function))
                functions.quickSet(function.index());
        }
        forEachClassFrom(chain.instanceClass, [&](BuiltinClassID above) {
            if (classesOfPristineParts.quickGet(above.index()))
                m_functionsLookingUpUnlockedNamesOnInstances[above.index()].merge(functions);
            return false;
        });
    }
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
