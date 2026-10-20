/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSimplifyCastsInternal.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet repeatableCasts { Opcode::Claim, Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject };

static constexpr bool isSimplifiable(Opcode opcode)
{
    return isCast(opcode) && opcode != Opcode::FinishObject;
}

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
    struct Scope {
        size_t numberOfAddedCasts;
        size_t numberOfReplacedNarrowestCasts;
        Vector<ValueID> hiddenCasts;
    };

    HashMap<CastKey, ValueID, CastKeyHash, CastKeyHashTraits> passed;
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
            unsigned changedInRound = removeCasts();
            changedInRound += removeRepeatedCasts();
            if (!changedInRound)
                break;
            changed += changedInRound;
        }
        return changed;
    }

private:
    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    const Type& predictionOrTypeOf(ValueID value) const { return m_program.predictionOrTypeOf(m_function, value); }

    // isSubsetOf() does not compare what a type only declares: contents that are not proven, a signature.
    bool isUnchangedByCastTo(const Type& type, const Type& castType) const
    {
        return &type == &castType || type.castTo(castType, m_context.types) == type;
    }

    bool canReplaceWithOperand(const Type& operand, const Type& target) const
    {
        return operand.hasLiteralStrings() == target.hasLiteralStrings() && isUnchangedByCastTo(operand, target);
    }

    bool predictionIsNarrowerThanType(const Value& cast, ValueID operand) const
    {
        TypeID prediction = cast.opcode == Opcode::Predict ? cast.predicted() : cast.prediction();
        if (!prediction || prediction == m_function.at(operand).prediction())
            return false;
        return !typeOf(operand).isSubsetOf(m_program.at(prediction), m_program);
    }

    ValueID withoutRemovableUpcasts(ValueID operand) const
    {
        for (;;) {
            const Value& value = m_function.at(operand);
            if (value.opcode != Opcode::Upcast)
                return operand;
            ValueID inner = m_substitution.resolve(value.operand(0));
            if (predictionIsNarrowerThanType(value, inner) || !isUnchangedByCastTo(typeOf(inner), typeOf(operand)))
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
        Value& cast = m_function.at(id);
        ValueID operand = m_substitution.resolve(cast.operand(0));

        unsigned changed = 0;
        if (cast.opcode != Opcode::Upcast) {
            ValueID narrowest = withoutRemovableUpcasts(operand);
            if (narrowest != operand) {
                operand = narrowest;
                cast.setOperand(0, operand);
                ++changed;
            }
        }
        if (!typeOf(operand).isSubsetOf(typeOf(id), m_program))
            return changed;

        if (!predictionIsNarrowerThanType(cast, operand) && canReplaceWithOperand(typeOf(operand), typeOf(id))) {
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

    void enterScope(DominatingCasts& casts, const Dominators& dominators, BlockID block) const
    {
        Vector<ValueID> hiddenCasts;
        BlockID dominator = dominators.idom(block);
        // A covered block dominates its handler and whatever the handler leads to, and its casts do not.
        if (dominator && !dominators.terminatorDominates(dominator, block)) {
            const DominatingCasts::Scope& dominatorScope = casts.scopes.last();
            hiddenCasts.append(casts.addedCasts.subspan(dominatorScope.numberOfAddedCasts));
            removeCastsAddedIn(casts, dominatorScope);
        }
        casts.scopes.append(DominatingCasts::Scope { casts.addedCasts.size(), casts.replacedNarrowestCasts.size(), WTF::move(hiddenCasts) });
    }

    void leaveScope(DominatingCasts& casts) const
    {
        DominatingCasts::Scope scope = casts.scopes.takeLast();
        removeCastsAddedIn(casts, scope);
        for (ValueID cast : scope.hiddenCasts)
            addDominatingCast(casts, cast);
    }

    unsigned repointAccess(ValueID id, const DominatingCasts& casts)
    {
        Value& access = m_function.at(id);
        std::optional<uint8_t> receiver = receiverOf(access.opcode);
        if (!receiver)
            return 0;

        ValueID current = access.operand(*receiver);
        ValueID cast = casts.narrowestCast[m_context.analyses.sameObjectThroughCasts(current).index()];
        if (!cast || typeOf(cast) == typeOf(current) || !isUnchangedByCastTo(typeOf(cast), typeOf(current)))
            return 0;
        if (!predictionOrTypeOf(cast).isSubsetOf(predictionOrTypeOf(current), m_program))
            return 0;
        access.setOperand(*receiver, cast);
        return 1;
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
            if (!repeatableCasts.contains(opcode))
                continue;
            ValueID earlier = casts.passed.get(keyOf(id));
            if (!earlier) {
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
            enterScope(casts, dominators, block);
            changed += visitBlock(block, casts, edits);
        };
        auto leave = [&](BlockID) {
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
    return CastSimplification(context).run() ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
