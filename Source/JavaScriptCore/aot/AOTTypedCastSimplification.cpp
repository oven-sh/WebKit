/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Once bodies have been copied to where they are called, many a value is held to a type its own type is within. It compares types that
// are given, and finds out none.
class CastSimplification {
public:
    CastSimplification(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    unsigned run()
    {
        unsigned changed = 0;
        // An allocation that is made with another layout leaves behind the cast that asked for it.
        for (;;) {
            unsigned changedNow = removeCasts();
            changedNow += removeRepeatedCasts();
            changedNow += specializeAllocations();
            if (!changedNow)
                break;
            changed += changedNow;
        }
        if (changed)
            cleanUp(m_program, m_function);
        updateAllocations();
        return changed;
    }

private:
    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    ValueID resolve(ValueID value) const
    {
        while (m_replaced[value.index()] != value)
            value = m_replaced[value.index()];
        return value;
    }

    void resetReplacements()
    {
        m_replaced.resize(m_function.values.size());
        for (unsigned i = 0; i < m_replaced.size(); ++i)
            m_replaced[i] = ValueID(i);
    }

    void rewrite()
    {
        auto replace = [&](ValueID& value) {
            value = resolve(value);
        };
        for (auto& value : m_function.values)
            value.forEachChild(replace);
        for (auto& block : m_function.blocks)
            block.terminator.forEachChild(replace);
    }

    bool finds(LayoutID layout, const Field& field) const
    {
        return m_program.at(layout).fieldNamed(field.name)
            || findMethod(m_program, layout, field.name, MethodKind::Method)
            || findMethod(m_program, layout, field.name, MethodKind::Getter);
    }

    // The type of a value says how an object is looked at: through which layout its properties are found. Whether the users of the
    // cast may go by the type of its operand, which is within that of the cast.
    bool mayTakeOperand(ValueID cast, const Type& operand, const Type& target) const
    {
        LayoutID own = operand.onlyView();
        LayoutID view = target.onlyView();
        if (own == view)
            return true;
        // What is stored was held to the type the field has in the layout the receiver was looked at by, if any. In another it may be less.
        if (m_isStoredThrough.get(cast.index()))
            return false;
        if (!view || !operand.hasObjects())
            return true;
        // They would find less.
        if (!own)
            return false;
        for (auto& field : m_program.at(view).fields) {
            if (!finds(own, field))
                return false;
        }
        return true;
    }

    // A cast whose operand is of the type tests nothing. Its users take the operand, or, where they would see less by the type of
    // that or by what is expected of it, it stays as a way to look at it.
    unsigned removeCasts()
    {
        resetReplacements();
        m_isStoredThrough.clearAll();
        m_isStoredThrough.ensureSize(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (m_function.at(id).opcode != Opcode::PutById)
                    continue;
                for (ValueID receiver = m_function.at(id).child(0); m_function.at(receiver).opcode == Opcode::Cast; receiver = m_function.at(receiver).child(0))
                    m_isStoredThrough.set(receiver.index());
            }
        }
        unsigned changed = 0;
        for (auto& block : m_function.blocks) {
            bool removedAny = false;
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                auto& cast = m_function.at(id);
                if (cast.opcode != Opcode::Cast)
                    continue;
                ValueID operand = resolve(cast.child(0));
                if (!m_program.isHeldAlready(typeOf(operand), typeOf(id), cast.castReason()))
                    continue;
                // Its operand is always of its type, as far as that is an object. It goes if the operand is of the type after `as`, or if
                // nothing can be made of what is not.
                if (cast.castReason() == CastReason::Coerce) {
                    auto& coercedTo = m_program.at(cast.coercedType());
                    if (!m_program.isHeldAlready(typeOf(operand), coercedTo, CastReason::As) && coercionMayChangeObjects(m_program, typeOf(operand), coercedTo))
                        continue;
                }
                // With the name of a property it turns away `undefined` and `null` whatever its type says. So does `x!`.
                if ((cast.accessedName() || cast.castReason() == CastReason::NonNull) && typeOf(operand).mayBeUndefinedOrNull())
                    continue;
                if (!cast.hasPredictionUnlike(m_function.at(operand)) && mayTakeOperand(id, typeOf(operand), typeOf(id))) {
                    m_replaced[id.index()] = operand;
                    block.values[i] = ValueID();
                    removedAny = true;
                } else if (cast.castReason() != CastReason::Upcast)
                    cast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                else
                    continue;
                ++changed;
                // It was what might have thrown.
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            if (removedAny) {
                block.values.removeAllMatching([](ValueID id) {
                    return !id;
                });
            }
        }
        if (changed)
            rewrite();
        return changed;
    }

    static bool onlyTests(const Value& value)
    {
        if (value.opcode != Opcode::Cast)
            return false;
        return value.castReason() == CastReason::Store || value.castReason() == CastReason::Claim || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull;
    }

    // The value that this one is, looked at another way or tested.
    ValueID beforeTests(ValueID value) const
    {
        for (value = resolve(value); onlyTests(m_function.at(value)) || (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast); value = resolve(m_function.at(value).child(0))) { }
        return value;
    }

    // Whether the access of a property of an object may go by another way to look at the object, which it is known to be of as well.
    bool mayGoBy(const Value& access, ValueID known) const
    {
        LayoutID view = typeOf(known).onlyView();
        if (!view || typeOf(access.child(0)).isWithin(typeOf(known), m_program))
            return false;
        auto field = m_program.at(view).fieldNamed(access.string());
        if (!field)
            return false;
        // What is stored was held to the type the field has in the layout the receiver was looked at by. In another it may be less.
        TypeID type = m_program.at(view).fields[*field].type;
        return access.opcode != Opcode::PutById || !type || typeOf(access.child(1)).isWithin(m_program.at(type), m_program);
    }

    // A test that has passed stays passed: a value does not change, and an object is only ever turned into one of a specialization of
    // its layout, which is a member of all that its layout is a member of. So a cast that only tests, and that comes after a cast of
    // the same value to the same type whichever way it is reached, is that cast. Likewise a coercion of the same value to the same type:
    // what the first could make nothing of, the second cannot either, and plain data is better copied once.
    //
    // And an object that has passed the test for a type is of that type from there on, however it is looked at: `(x as T).name` finds
    // the field without asking whether the object has one.
    unsigned removeRepeatedCasts()
    {
        auto isRepeatable = [&](ValueID id) {
            return onlyTests(m_function.at(id)) || (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Coerce);
        };
        unsigned numberOfCasts = 0;
        unsigned numberOfTests = 0;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                numberOfCasts += isRepeatable(id);
                numberOfTests += onlyTests(m_function.at(id));
            }
        }
        if (numberOfCasts < 2 && !numberOfTests)
            return 0;

        unsigned numberOfBlocks = m_function.blocks.size();
        static constexpr unsigned unknown = UINT_MAX;
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        Vector<unsigned> order;
        Vector<unsigned> numberInOrder;
        numberInOrder.fill(unknown, numberOfBlocks);
        {
            // Postorder, then turned around.
            BitVector seen(numberOfBlocks);
            seen.set(0);
            Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next == targets.size()) {
                    order.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned target = targets[next++].block.index();
                predecessors[target].append(block);
                if (!seen.set(target))
                    stack.append({ target, 0u });
            }
            order.reverse();
            for (unsigned i = 0; i < order.size(); ++i)
                numberInOrder[order[i]] = i;
        }

        Vector<unsigned> dominator;
        dominator.fill(unknown, numberOfBlocks);
        dominator[0] = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned i = 1; i < order.size(); ++i) {
                unsigned found = unknown;
                for (unsigned other : predecessors[order[i]]) {
                    if (dominator[other] == unknown)
                        continue;
                    if (found == unknown) {
                        found = other;
                        continue;
                    }
                    while (found != other) {
                        while (numberInOrder[other] > numberInOrder[found])
                            other = dominator[other];
                        while (numberInOrder[found] > numberInOrder[other])
                            found = dominator[found];
                    }
                }
                if (dominator[order[i]] != found) {
                    dominator[order[i]] = found;
                    changed = true;
                }
            }
        }
        Vector<Vector<unsigned, 2>> dominated(numberOfBlocks);
        for (unsigned i = 1; i < order.size(); ++i)
            dominated[dominator[order[i]]].append(order[i]);

        resetReplacements();
        unsigned changed = 0;
        // By operand and type: the cast that has passed on every way here.
        UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> passed;
        Vector<uint64_t> added;
        auto keyOf = [&](ValueID id) {
            auto& cast = m_function.at(id);
            uint32_t type = cast.castReason() == CastReason::Coerce ? cast.coercedType().index() | 1u << 31 : cast.type.index();
            return static_cast<uint64_t>(resolve(cast.child(0)).index()) << 32 | type;
        };
        // By the value before all tests: the test it has passed on every way here that says the most of it. And what was there before.
        UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> knownAs;
        Vector<std::pair<uint32_t, ValueID>> knownBefore;
        auto add = [&](ValueID id) {
            if (passed.add(keyOf(id), id).isNewEntry)
                added.append(keyOf(id));
            if (!onlyTests(m_function.at(id)))
                return;
            uint32_t tested = beforeTests(id).index();
            ValueID before = knownAs.get(tested);
            if (before && typeOf(before).isWithin(typeOf(id), m_program))
                return;
            knownBefore.append({ tested, before });
            knownAs.set(tested, id);
        };
        auto enter = [&](unsigned b) {
            auto& block = m_function.blocks[b];
            // A cast that a handler watches has passed where control goes on, and not in the handler.
            if (predecessors[b].size() == 1) {
                auto& before = m_function.blocks[predecessors[b][0]];
                auto& terminator = before.terminator;
                if (terminator.kind == TerminatorKind::Invoke && terminator.targets[0].block.index() == b && terminator.targets[1].block.index() != b && !before.values.isEmpty() && isRepeatable(before.values.last()))
                    add(before.values.last());
            }
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::InById || value.opcode == Opcode::CallMethod) {
                    if (ValueID known = knownAs.get(beforeTests(value.child(0)).index()); known && mayGoBy(value, known)) {
                        value.children[0] = known;
                        ++changed;
                    }
                    continue;
                }
                if (!isRepeatable(id))
                    continue;
                bool isWatched = i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke;
                ValueID earlier = passed.get(keyOf(id));
                if (earlier && m_function.at(earlier).type != value.type)
                    earlier = { };
                if (!earlier) {
                    if (!isWatched)
                        add(id);
                    continue;
                }
                m_replaced[id.index()] = earlier;
                block.values[i] = ValueID();
                ++changed;
                if (isWatched) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            block.values.removeAllMatching([](ValueID id) {
                return !id;
            });
        };
        struct Visit {
            unsigned block;
            unsigned next;
            unsigned numberAddedBefore;
            unsigned numberKnownBefore;
        };
        Vector<Visit> visits { Visit { 0, 0, 0, 0 } };
        enter(0);
        while (!visits.isEmpty()) {
            auto& visit = visits.last();
            if (visit.next < dominated[visit.block].size()) {
                unsigned inner = dominated[visit.block][visit.next++];
                visits.append(Visit { inner, 0, static_cast<unsigned>(added.size()), static_cast<unsigned>(knownBefore.size()) });
                enter(inner);
                continue;
            }
            while (added.size() > visit.numberAddedBefore)
                passed.remove(added.takeLast());
            while (knownBefore.size() > visit.numberKnownBefore) {
                auto [tested, before] = knownBefore.takeLast();
                if (before)
                    knownAs.set(tested, before);
                else
                    knownAs.remove(tested);
            }
            visits.removeLast();
        }
        if (changed)
            rewrite();
        return changed;
    }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    const Field& fieldStoredTo(const Value& store) const
    {
        if (store.opcode == Opcode::PutField)
            return m_program.at(store.layout()).fields[store.field()];
        auto& layout = m_program.at(m_function.at(store.child(0)).layout());
        return layout.fields[*layout.fieldNamed(store.string())];
    }

    unsigned specializeAllocations()
    {
        unsigned changed = 0;
        for (auto& block : m_function.blocks) {
            for (unsigned k = 0; k < block.values.size(); ++k) {
                auto& cast = m_function.at(block.values[k]);
                if (cast.opcode == Opcode::Cast && (cast.castReason() == CastReason::As || cast.castReason() == CastReason::Coerce))
                    changed += specializeAllocationFor(block, k);
            }
        }
        return changed;
    }

    // `New L1`, stores to it, `as L2`, where the run time would turn the object into one of L2 where it is: it is made as one of L2.
    // Nobody can tell, because nobody has seen the object: all that happens to it from the allocation to the cast, which are in one
    // block, is that its first stores go to it and that it is looked at as what it is anyway. Whatever else uses it comes after the cast.
    bool specializeAllocationFor(Block& block, unsigned k)
    {
        auto& cast = m_function.at(block.values[k]);
        LayoutID target = m_program.at(cast.castReason() == CastReason::Coerce ? cast.coercedType() : cast.type).onlyView();
        if (!target)
            return false;
        ValueID object = cast.child(0);
        while (m_function.at(object).opcode == Opcode::Cast) {
            if (m_function.at(object).castReason() != CastReason::Upcast)
                return false;
            object = m_function.at(object).child(0);
        }
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        LayoutID made = m_function.at(object).layout();
        auto& from = m_program.at(made);
        auto& to = m_program.at(target);
        // The prototype of an object of a class comes with its layout.
        if (from.kind != LayoutKind::Record || to.kind != LayoutKind::Record)
            return false;
        // Every use of the object that was fine for one of L1 is fine for one of L2.
        if (!std::ranges::binary_search(from.specializations, target) || !m_program.isUpcast(target, made))
            return false;
        size_t birth = block.values.find(object);
        if (birth == notFound || birth >= k)
            return false;

        for (size_t i = birth + 1; i < k; ++i) {
            auto& value = m_function.at(block.values[i]);
            bool usesObject = false;
            value.forEachChild([&](ValueID child) {
                usesObject |= originOf(child) == object;
            });
            if (usesObject && !(value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast) && !isInitializingStore(m_program, m_function, block, i))
                return false;
        }
        // A property may be declared to be less in L2. What is stored going by the type of the allocation, now or later, has to be of that.
        Vector<std::pair<ValueID, uint32_t>, 8> stores;
        for (auto& other : m_function.blocks) {
            for (ValueID id : other.values) {
                auto& store = m_function.at(id);
                if ((store.opcode != Opcode::PutField && store.opcode != Opcode::PutById) || store.child(0) != object)
                    continue;
                StringID name = store.opcode == Opcode::PutField ? m_program.at(store.layout()).fields[store.field()].name : store.string();
                auto field = to.fieldNamed(name);
                // By a name that L1 has no field for either, it is no store to a field.
                if (!field && store.opcode == Opcode::PutById && !from.fieldNamed(name))
                    continue;
                if (!field)
                    return false;
                TypeID type = to.fields[*field].type;
                if (type && !typeOf(store.child(1)).isWithin(m_program.at(type), m_program))
                    return false;
                stores.append({ id, *field });
            }
        }
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.layout = target;
        for (uint16_t& field : allocation.order) {
            auto mapped = to.fieldNamed(from.fields[field].name);
            if (!mapped)
                return false;
            field = *mapped;
        }

        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        auto& allocated = m_function.at(object);
        allocated.immediates = { target.index(), number };
        allocated.type = to.type;
        allocated.rep = repOf(m_program.at(to.type));
        for (auto [store, field] : stores) {
            if (m_function.at(store).opcode == Opcode::PutField)
                m_function.at(store).immediates = { target.index(), field };
        }
        return true;
    }

    // Blocks have been merged, casts have gone, constructors have been copied in behind the allocation: more stores may follow an
    // allocation now than when it was given its Allocation. By the rule of assignAllocations().
    void updateAllocations()
    {
        for (auto& block : m_function.blocks) {
            for (size_t i = 0; i < block.values.size(); ++i) {
                ValueID object = block.values[i];
                if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
                    continue;
                auto& layout = m_program.at(m_function.at(object).layout());
                if (layout.kind != LayoutKind::Record)
                    continue;
                Allocation allocation;
                allocation.layout = m_function.at(object).layout();
                allocation.constructedBy = m_function.allocations[m_function.at(object).allocation()].constructedBy;
                for (size_t next = i + 1; next < block.values.size() && isInitializingStore(m_program, m_function, block, next); ++next) {
                    uint16_t field = *layout.fieldNamed(fieldStoredTo(m_function.at(block.values[next])).name);
                    if (!allocation.order.contains(field))
                        allocation.order.append(field);
                }
                allocation.born = allocation.order.size();
                m_function.at(object).immediates[1] = m_function.allocationOf(WTF::move(allocation));
            }
        }
    }

    const Program& m_program;
    Function& m_function;
    Vector<ValueID> m_replaced;
    // By value: a cast that the receiver of a PutById is, or comes down to through casts.
    BitVector m_isStoredThrough;
};

} // anonymous namespace

unsigned simplifyCasts(const Program& program, Function& function)
{
    return CastSimplification(program, function).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
