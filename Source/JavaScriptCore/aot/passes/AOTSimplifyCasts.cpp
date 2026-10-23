/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResultRules.h"
#include "AOTSimplifyCastsInternal.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CastKey {
    ValueID operand;
    TypeID type;
    Opcode opcode { Opcode::Undefined };

    friend bool operator==(const CastKey&, const CastKey&) = default;
};

struct CastKeyHash {
    static unsigned hash(const CastKey& key) { return computeHash(key.operand, key.type, key.opcode); }
    static bool equal(const CastKey& a, const CastKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};

struct CastKeyHashTraits : WTF::GenericHashTraits<CastKey> {
    static void constructDeletedValue(CastKey& slot) { slot = CastKey { ValueID(ValueID::hashTableDeletedIndex), TypeID(), Opcode::Undefined }; }
    static bool isDeletedValue(const CastKey& key) { return key.operand.index() == ValueID::hashTableDeletedIndex; }
};

struct PendingEdits {
    Vector<std::pair<BlockID, BitVector>, 4> erasedValues;
};

struct DominatingCasts {
    // Of a block that adds casts or bindings, or hides some. Any other block leaves the tables as they are.
    struct Scope {
        BlockID block;
        size_t numberOfAddedCasts;
        size_t numberOfReplacedNarrowestCasts;
        size_t numberOfBindingsPastDeadZone;
        Vector<ValueID> hiddenCasts;
        Vector<uint64_t> hiddenBindingsPastDeadZone;
    };

    HashMap<CastKey, ValueID, CastKeyHash, CastKeyHashTraits> passed;
    // A number stands for a variable, or for a field of the environment that a value of the function holds. In the order in which they were found: one that is found again further down is not listed twice.
    Vector<uint64_t> bindingsPastDeadZone;
    HashSet<uint64_t> isPastDeadZone;
    Vector<ValueID> addedCasts;
    Vector<ValueID> narrowestCast;
    Vector<std::pair<ValueID, ValueID>> replacedNarrowestCasts;
    Vector<Scope> scopes;
};

class CastSimplification {
public:
    explicit CastSimplification(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
        , m_substitution(context.function)
    {
    }

    unsigned run()
    {
        unsigned changed = static_cast<unsigned>(moveCastsToEdges(m_context));
        for (;;) {
            changed += removeCasts();
            // removeCasts() judges each cast by what it leaves behind, so it finds nothing more until removeRepeatedCasts() has changed something.
            unsigned repeated = removeRepeatedCasts();
            if (!repeated) {
                RELEASE_ASSERT(!m_context.build.options().verifiesAfterEveryPass || (!removeCasts() && !removeRepeatedCasts()));
                break;
            }
            changed += repeated;
        }
        return changed;
    }

private:
    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    const Type& expectedOrTypeOf(ValueID value) const { return m_program.expectedOrTypeOf(m_function, value); }

    // isSubsetOf() does not compare what a type only declares: contents that are not proven, a signature.
    bool isUnchangedByCastTo(const Type& type, const Type& castType) const
    {
        return &type == &castType || type.castTo(castType, m_context.types) == type;
    }

    bool canReplaceWithOperand(const Type& operand, const Type& target) const
    {
        return operand.hasLiteralStrings() == target.hasLiteralStrings() && isUnchangedByCastTo(operand, target);
    }

    bool expectsNarrowerTypeThan(const Value& cast, ValueID operand) const
    {
        TypeID expected = cast.expected();
        if (!expected || expected == m_function.at(operand).expected())
            return false;
        return !typeOf(operand).isSubsetOf(m_program.at(expected), m_program);
    }

    ValueID withoutRemovableUpcasts(ValueID operand) const
    {
        for (;;) {
            const Value& value = m_function.at(operand);
            if (value.opcode != Opcode::Upcast)
                return operand;
            ValueID inner = m_substitution.resolve(value.operand(0));
            if (expectsNarrowerTypeThan(value, inner) || !isUnchangedByCastTo(typeOf(inner), typeOf(operand)))
                return operand;
            operand = inner;
        }
    }

    void eraseValue(PendingEdits& edits, BlockID block, uint32_t index) const
    {
        if (edits.erasedValues.isEmpty() || edits.erasedValues.last().first != block)
            edits.erasedValues.append({ block, BitVector() });
        edits.erasedValues.last().second.set(index);
    }

    void applyEdits(const PendingEdits& edits)
    {
        for (auto& [block, indexes] : edits.erasedValues)
            m_builder.eraseValues(block, indexes);
        m_substitution.apply(m_function, &m_context.analyses);
        m_context.analyses.didChangeValues();
    }

    unsigned removeCasts()
    {
        PendingEdits edits;
        unsigned changed = 0;
        for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
            const BlockValues& values = m_function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (isSimplifiable(m_function.at(values[index]).opcode))
                    changed += simplifyCast(block, index, edits);
            }
        }
        if (changed)
            applyEdits(edits);
        return changed;
    }

    unsigned simplifyCast(BlockID block, uint32_t index, PendingEdits& edits)
    {
        ValueID id = m_function.at(block).values[index];
        const Value& cast = m_function.at(id);
        ValueID operand = m_substitution.resolve(cast.operand(0));

        unsigned changed = 0;
        if (cast.opcode != Opcode::Upcast) {
            ValueID narrowest = withoutRemovableUpcasts(operand);
            if (narrowest != operand) {
                operand = narrowest;
                m_function.edit(id, ValueEdit::Definition).setOperand(0, operand);
                ++changed;
            }
        }
        if (!typeOf(operand).isSubsetOf(typeOf(id), m_program))
            return changed;

        if (!expectsNarrowerTypeThan(cast, operand) && canReplaceWithOperand(typeOf(operand), typeOf(id))) {
            m_substitution.replace(id, operand);
            eraseValue(edits, block, index);
        } else if (repeatableCasts.contains(cast.opcode))
            becomeUpcast(m_function, id, operand);
        else
            return changed;
        return changed + 1;
    }

    CastKey keyOf(ValueID id) const
    {
        const Value& cast = m_function.at(id);
        return { m_substitution.resolve(cast.operand(0)), cast.type, cast.opcode };
    }

    bool hasRepeatableCast() const
    {
        for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (repeatableCasts.contains(m_function.at(id).opcode))
                    return true;
            }
        }
        return false;
    }

    void addDominatingCast(DominatingCasts& casts, ValueID id) const
    {
        casts.passed.add(keyOf(id), id);
        casts.addedCasts.append(id);
        ValueID object = m_context.analyses.sameObjectThroughCasts(id);
        ValueID before = casts.narrowestCast[object.index()];
        if (before && typeOf(before).isSubsetOf(typeOf(id), m_program))
            return;
        casts.replacedNarrowestCasts.append({ object, before });
        casts.narrowestCast[object.index()] = id;
    }

    void removeCastsAddedIn(DominatingCasts& casts, const DominatingCasts::Scope& scope) const
    {
        while (casts.addedCasts.size() > scope.numberOfAddedCasts)
            casts.passed.remove(keyOf(casts.addedCasts.takeLast()));
        while (casts.replacedNarrowestCasts.size() > scope.numberOfReplacedNarrowestCasts) {
            auto [object, before] = casts.replacedNarrowestCasts.takeLast();
            casts.narrowestCast[object.index()] = before;
        }
    }

    bool notePastDeadZone(DominatingCasts& casts, uint64_t binding) const
    {
        if (!casts.isPastDeadZone.add(binding).isNewEntry)
            return false;
        casts.bindingsPastDeadZone.append(binding);
        return true;
    }

    void forgetBindingsFoundBehind(DominatingCasts& casts, size_t numberOfBindings) const
    {
        while (casts.bindingsPastDeadZone.size() > numberOfBindings)
            casts.isPastDeadZone.remove(casts.bindingsPastDeadZone.takeLast());
    }

    bool hasScope(const DominatingCasts& casts, BlockID block) const
    {
        return !casts.scopes.isEmpty() && casts.scopes.last().block == block;
    }

    void enterScope(DominatingCasts& casts, BlockID block, Vector<ValueID>&& hiddenCasts, Vector<uint64_t>&& hiddenBindings) const
    {
        casts.scopes.append(DominatingCasts::Scope { block, casts.addedCasts.size(), casts.replacedNarrowestCasts.size(), casts.bindingsPastDeadZone.size(), WTF::move(hiddenCasts), WTF::move(hiddenBindings) });
    }

    void ensureScope(DominatingCasts& casts, BlockID block) const
    {
        if (!hasScope(casts, block))
            enterScope(casts, block, { }, { });
    }

    // A covered block dominates its handler and whatever the handler leads to, and its casts do not.
    void hideCastsAndBindingsOfDominator(DominatingCasts& casts, const Dominators& dominators, BlockID block) const
    {
        if (casts.scopes.isEmpty())
            return;
        const DominatingCasts::Scope& innermost = casts.scopes.last();
        if (casts.addedCasts.size() == innermost.numberOfAddedCasts && casts.bindingsPastDeadZone.size() == innermost.numberOfBindingsPastDeadZone)
            return;
        if (innermost.block != dominators.idom(block) || dominators.terminatorDominates(innermost.block, block))
            return;
        Vector<ValueID> hiddenCasts;
        Vector<uint64_t> hiddenBindings;
        hiddenCasts.append(casts.addedCasts.subspan(innermost.numberOfAddedCasts));
        hiddenBindings.append(casts.bindingsPastDeadZone.subspan(innermost.numberOfBindingsPastDeadZone));
        removeCastsAddedIn(casts, innermost);
        forgetBindingsFoundBehind(casts, innermost.numberOfBindingsPastDeadZone);
        enterScope(casts, block, WTF::move(hiddenCasts), WTF::move(hiddenBindings));
    }

    void leaveScope(DominatingCasts& casts) const
    {
        DominatingCasts::Scope scope = casts.scopes.takeLast();
        removeCastsAddedIn(casts, scope);
        forgetBindingsFoundBehind(casts, scope.numberOfBindingsPastDeadZone);
        for (ValueID cast : scope.hiddenCasts)
            addDominatingCast(casts, cast);
        for (uint64_t binding : scope.hiddenBindingsPastDeadZone)
            notePastDeadZone(casts, binding);
    }

    uint64_t bindingOf(const Value& access) const
    {
        switch (access.opcode) {
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
            return 1ull << 63 | access.variable().index();
        case Opcode::GetEnvironmentField:
        case Opcode::PutEnvironmentField: {
            ValueID environment = m_context.analyses.sameObjectThroughCasts(m_substitution.resolve(access.environmentOperand()));
            return access.field() < 1u << 24 ? 1ull << 62 | static_cast<uint64_t>(environment.index()) << 24 | access.field() : 0;
        }
        default:
            return 0;
        }
    }

    // A binding never goes back into its dead zone: it has left it where a store has run, where the test for it has passed, and where a cast stands for that test.
    uint64_t bindingPastDeadZoneBehind(ValueID id) const
    {
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::PutModuleVariable || value.opcode == Opcode::PutEnvironmentField)
            return typeOf(value.valueOperand()).primitives & Primitive::Absent ? 0 : bindingOf(value);
        if (!leavesDeadZoneBehind(m_program, m_function, value))
            return 0;
        return bindingOf(m_function.at(definitionThroughUpcasts(m_function, m_substitution.resolve(value.valueOperand()))));
    }

    void assumeInitialized(ValueID id)
    {
        Value& test = m_function.edit(id);
        ValueID operand = test.valueOperand();
        TypeID type = test.type;
        test.become(Opcode::AssumeType);
        test.setValueOperand(operand);
        test.setCastType(type);
    }

    unsigned repointOperand(ValueID id, uint32_t operandIndex, const DominatingCasts& casts)
    {
        ValueID current = m_function.at(id).operand(operandIndex);
        ValueID cast = casts.narrowestCast[m_context.analyses.sameObjectThroughCasts(current).index()];
        if (!cast || typeOf(cast) == typeOf(current) || !isUnchangedByCastTo(typeOf(cast), typeOf(current)))
            return 0;
        if (!expectedOrTypeOf(cast).isSubsetOf(expectedOrTypeOf(current), m_program))
            return 0;
        Value& user = m_function.edit(id, ValueEdit::Definition);
        user.setOperand(operandIndex, cast);
        // A check that nothing passes is typed by its cast type, as behind a check that passed, and that is more than it let through of the wider operand.
        if (user.opcode == Opcode::Check && !computeResultType(TypingContext { m_program, m_function, m_context.types }, user).isSubsetOf(typeOf(id), m_program)) {
            user.setOperand(operandIndex, current);
            return 0;
        }
        return 1;
    }

    unsigned repointAccess(ValueID id, const DominatingCasts& casts)
    {
        std::optional<uint8_t> receiver = receiverOf(m_function.at(id).opcode);
        return receiver ? repointOperand(id, *receiver, casts) : 0;
    }

    unsigned visitBlock(BlockID block, DominatingCasts& casts, PendingEdits& edits)
    {
        const BlockValues& values = m_function.at(block).values;
        unsigned changed = 0;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            Opcode opcode = m_function.at(id).opcode;
            if (accessClassOf(opcode) == AccessClass::ByName) {
                changed += repointAccess(id, casts);
                continue;
            }
            uint64_t binding = bindingPastDeadZoneBehind(id);
            if (binding)
                ensureScope(casts, block);
            bool wasPastDeadZone = binding && !notePastDeadZone(casts, binding);
            if (!repeatableCasts.contains(opcode))
                continue;
            ValueID earlier = casts.passed.get(keyOf(id));
            if (!earlier && wasPastDeadZone && opcode == Opcode::CheckInitialized) {
                assumeInitialized(id);
                ++changed;
                continue;
            }
            if (!earlier) {
                // A cast of what an earlier cast has let through asks only for what that one left open, and the next round removes it where that is nothing.
                changed += repointOperand(id, 0, casts);
                ensureScope(casts, block);
                addDominatingCast(casts, id);
                continue;
            }
            m_substitution.replace(id, earlier);
            eraseValue(edits, block, index);
            ++changed;
        }
        return changed;
    }

    unsigned removeRepeatedCasts()
    {
        if (!hasRepeatableCast())
            return 0;

        const Dominators& dominators = m_context.analyses.dominators();
        DominatingCasts casts;
        casts.narrowestCast.fill(ValueID(), m_function.values.size());
        PendingEdits edits;
        unsigned changed = 0;
        auto enter = [&](BlockID block) {
            hideCastsAndBindingsOfDominator(casts, dominators, block);
            changed += visitBlock(block, casts, edits);
        };
        auto leave = [&](BlockID block) {
            if (hasScope(casts, block))
                leaveScope(casts);
        };
        dominators.walkTree(enter, leave);
        if (changed)
            applyEdits(edits);
        return changed;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    IRBuilder m_builder;
    ValueSubstitution m_substitution;
};

} // anonymous namespace

PassOutcome simplifyCasts(FunctionPassContext& context)
{
    if (!CastSimplification(context).run())
        return PassOutcome::Unchanged;
    // Every step drops what its edits have made stale, so what is held at the end was made behind the last edit.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
