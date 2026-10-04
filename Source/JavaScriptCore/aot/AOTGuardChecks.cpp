/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

struct CheckedName {
    bool operator==(const CheckedName&) const = default;

    const Node* value;
    uint16_t nameID;
    uint8_t slot;
};

struct CheckedFamily {
    bool operator==(const CheckedFamily&) const = default;

    const Node* value;
    uint16_t family;
    bool isStale;
};

struct CheckedValues {
    const CheckedFamily* find(const Node* value, uint16_t family) const
    {
        for (auto& checked : families) {
            if (checked.value == value && checked.family == family)
                return &checked;
        }
        return nullptr;
    }

    void intersectWith(const CheckedValues& other)
    {
        names.removeAllMatching([&](const CheckedName& name) {
            return !other.names.contains(name);
        });
        families.removeAllMatching([&](CheckedFamily& checked) {
            const CheckedFamily* theirs = other.find(checked.value, checked.family);
            if (!theirs)
                return true;
            checked.isStale |= theirs->isStale;
            return false;
        });
        cells.removeAllMatching([&](const Node* cell) {
            return !other.cells.contains(cell);
        });
    }

    bool holdsTheSameAs(const CheckedValues& other) const
    {
        if (names.size() != other.names.size() || families.size() != other.families.size() || cells.size() != other.cells.size())
            return false;
        for (auto& name : names) {
            if (!other.names.contains(name))
                return false;
        }
        for (auto& checked : families) {
            if (!other.families.contains(checked))
                return false;
        }
        for (const Node* cell : cells) {
            if (!other.cells.contains(cell))
                return false;
        }
        return true;
    }

    void noteEffect()
    {
        names.shrink(0);
        families.removeAllMatching([](CheckedFamily& checked) {
            checked.isStale = true;
            return !Instance::hasByteForFamily(checked.family);
        });
    }

    void noteCleanFamily(uint16_t family)
    {
        for (auto& checked : families) {
            if (checked.family == family)
                checked.isStale = false;
        }
    }

    Vector<CheckedName> names;
    Vector<CheckedFamily> families;
    Vector<const Node*> cells;
};

} // anonymous namespace

static const Node* valueBehindAliases(const Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    return node;
}

bool Lowering::preservesLayouts(Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::ConstantCell:
    case NodeKind::Intrinsic:
    case NodeKind::LinkTimeConstant:
    case NodeKind::Argument:
    case NodeKind::Phi:
    case NodeKind::Proj:
    case NodeKind::GetStack:
    case NodeKind::SetStack:
        return true;
    case NodeKind::Narrow:
        return !node->fieldOrigin;
    case NodeKind::Guard:
        return node->guardKind != GuardKind::Whole || node->checksName();
    case NodeKind::Bytecode:
        break;
    }
    if (node->guard)
        return node->guard->checksName();
    switch (node->opcode) {
    case op_check_tdz:
    case op_to_this:
    case op_to_object:
    case op_typeof:
    case op_typeof_is_undefined:
    case op_typeof_is_object:
    case op_typeof_is_function:
    case op_not:
    case op_stricteq:
    case op_nstricteq:
    case op_eq_null:
    case op_neq_null:
    case op_is_empty:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_big_int:
    case op_is_object:
    case op_is_callable:
    case op_is_constructor:
    case op_is_cell_with_type:
    case op_has_structure_with_flags:
    case op_identity_with_profile:
    case op_jmp:
    case op_jtrue:
    case op_jfalse:
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null:
    case op_jeq_ptr:
    case op_jneq_ptr:
    case op_jstricteq:
    case op_jnstricteq:
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
    case op_ret:
    case op_unreachable:
    case op_get_scope:
    case op_get_parent_scope:
    case op_get_argument:
    case op_argument_count:
    case op_get_from_arguments:
    case op_put_to_arguments:
    case op_get_internal_field:
    case op_put_internal_field:
    case op_get_private_name:
    case op_has_private_name:
    case op_has_private_brand:
    case op_check_private_brand:
    case op_set_private_brand:
    case op_set_function_name:
    case op_new_array:
    case op_new_array_buffer:
    case op_new_array_with_size:
    case op_new_func:
    case op_new_func_exp:
    case op_new_generator_func:
    case op_new_generator_func_exp:
    case op_new_async_func:
    case op_new_async_func_exp:
    case op_new_async_generator_func:
    case op_new_async_generator_func_exp:
    case op_new_reg_exp:
    case op_new_reg_exp_shared:
    case op_new_generator:
    case op_new_promise:
    case op_create_lexical_environment:
    case op_create_rest:
    case op_create_direct_arguments:
    case op_create_scoped_arguments:
    case op_create_cloned_arguments:
    case op_strcat:
        return true;
    case op_new_object:
        return !TypeTable::hasTypedFields() || !Graph::typeTagOf(node);
    case op_add:
    case op_sub:
    case op_mul:
    case op_div:
    case op_mod:
    case op_pow:
    case op_negate:
    case op_inc:
    case op_dec:
    case op_bitand:
    case op_bitor:
    case op_bitxor:
    case op_bitnot:
    case op_lshift:
    case op_rshift:
    case op_urshift:
    case op_to_number:
    case op_to_numeric:
    case op_to_string:
    case op_to_primitive:
    case op_to_property_key:
    case op_to_property_key_or_number:
    case op_less:
    case op_lesseq:
    case op_greater:
    case op_greatereq:
    case op_below:
    case op_beloweq:
    case op_eq:
    case op_neq:
    case op_jless:
    case op_jlesseq:
    case op_jgreater:
    case op_jgreatereq:
    case op_jnless:
    case op_jnlesseq:
    case op_jngreater:
    case op_jngreatereq:
    case op_jbelow:
    case op_jbeloweq:
    case op_jeq:
    case op_jneq:
        for (auto& use : node->uses) {
            if (!use.node->type || mayBe(use.node->type, TAnyObject))
                return false;
        }
        return true;
    case op_get_length: {
        Type base = node->use(node->as<OpGetLength>().m_base)->type;
        return base && isSubtype(base, TArray | TString);
    }
    case op_resolve_scope: {
        auto bytecode = node->as<OpResolveScope>();
        return isStaticClosureVarResolveType(bytecode.m_resolveType) || node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType).isAtStaticDepth();
    }
    case op_get_from_scope: {
        if (node->promotedEnvironment)
            return true;
        auto bytecode = node->as<OpGetFromScope>();
        ResolveType type = bytecode.m_getPutInfo.resolveType();
        return type == ResolvedClosureVar || type == ResolvedLazyClosureVar || node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type).kind == StaticVariable::Closure;
    }
    case op_put_to_scope:
        return node->promotedEnvironment || offsetOfVariableStoredInline(node);
    default:
        return false;
    }
}

void Lowering::chooseChecksOfGuards()
{
    if (!m_graph.placesToGuard)
        return;
    UncheckedKeyHashMap<BasicBlock*, CheckedValues> checkedAtTail;
    auto checkedAtHead = [&](BasicBlock* block) {
        CheckedValues checked;
        bool isFirst = true;
        for (BasicBlock* predecessor : block->predecessors) {
            if (predecessor->isGeneric)
                return CheckedValues { };
            auto atTail = checkedAtTail.find(predecessor);
            if (atTail == checkedAtTail.end())
                continue;
            if (std::exchange(isFirst, false))
                checked = atTail->value;
            else
                checked.intersectWith(atTail->value);
        }
        return checked;
    };
    auto walk = [&](BasicBlock* block, CheckedValues& checked, bool chooses) {
        for (Node* node : block->nodes) {
            if (node->isElided)
                continue;
            if (!node->checksName() || node->guardKind != GuardKind::Whole) {
                if (!preservesLayouts(node))
                    checked.noteEffect();
                continue;
            }
            bool isRead = node->opcode == op_get_by_id;
            const Node* value = valueBehindAliases(node->use(isRead ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base));
            bool isKnownCell = checked.cells.contains(value);
            if (!isKnownCell)
                checked.cells.append(value);
            GuardCheck check = GuardCheck::None;
            ASCIILiteral remark = "guard-checks-nothing"_s;
            if (uint16_t family = node->checkedPlace.family) {
                if (const CheckedFamily* known = checked.find(value, family)) {
                    if (known->isStale) {
                        check = GuardCheck::Byte;
                        remark = "guard-checks-byte"_s;
                        checked.noteCleanFamily(family);
                    }
                } else {
                    check = GuardCheck::Family;
                    remark = isKnownCell ? "guard-checks-family-of-known-cell"_s : "guard-checks-family"_s;
                    checked.families.append(CheckedFamily { value, family, false });
                }
            } else if (CheckedName name { value, node->checkedPlace.nameID, node->checkedPlace.slot }; !checked.names.contains(name)) {
                check = GuardCheck::Name;
                remark = isKnownCell ? "guard-checks-name-of-known-cell"_s : "guard-checks-name"_s;
                checked.names.append(name);
            }
            if (!chooses)
                continue;
            node->check = check;
            node->checkedValueIsCell = isKnownCell;
            m_graph.remark(remark, node->graph->codeBlock()->identifier(isRead ? node->as<OpGetById>().m_property : node->as<OpPutById>().m_property).string());
        }
    };
    for (bool changed = true; std::exchange(changed, false);) {
        for (BasicBlock* block : m_graph.m_rpo) {
            if (block->isGeneric)
                continue;
            CheckedValues checked = checkedAtHead(block);
            walk(block, checked, false);
            auto result = checkedAtTail.add(block, checked);
            if (result.isNewEntry)
                changed = true;
            else if (!result.iterator->value.holdsTheSameAs(checked)) {
                result.iterator->value = WTF::move(checked);
                changed = true;
            }
        }
    }
    for (BasicBlock* block : m_graph.m_rpo) {
        if (block->isGeneric)
            continue;
        CheckedValues checked = checkedAtHead(block);
        walk(block, checked, true);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))
