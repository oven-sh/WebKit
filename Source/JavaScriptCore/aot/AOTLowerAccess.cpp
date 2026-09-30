/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "B3PatchpointValue.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

// The slot's first word is { structureID, offset }, low half first.
static LValue lowHalf(FTL::Output& out, LValue word) { return out.castToInt32(word); }
static LValue highHalf(FTL::Output& out, LValue word) { return out.castToInt32(out.lShr(word, out.constInt32(32))); }

LValue Lowering::loadProperty(LValue object, LValue offset)
{
    LBasicBlock inlineCase = m_out.newBlock();
    LBasicBlock outOfLineCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.lessThan(offset, m_out.constInt32(firstOutOfLineOffset)), unsure(inlineCase), unsure(outOfLineCase));

    m_out.appendTo(inlineCase, outOfLineCase);
    LValue inlineAddress = m_out.add(object, m_out.add(m_out.shl(m_out.signExt32ToPtr(offset), m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage())));
    ValueFromBlock inlineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), inlineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(outOfLineCase, continuation);
    LValue butterfly = m_out.loadPtr(object, m_heaps.JSObject_butterfly);
    // Out of line property n is at butterfly[-(n - firstOutOfLineOffset) - 2].
    LValue index = m_out.sub(m_out.constIntPtr(firstOutOfLineOffset - 2), m_out.signExt32ToPtr(offset));
    LValue outOfLineAddress = m_out.add(butterfly, m_out.shl(index, m_out.constInt32(3)));
    ValueFromBlock outOfLineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), outOfLineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, inlineResult, outOfLineResult);
}

// word: the first of a slot whose offset is the location of a property (locationOfProperty()).
TypedPointer Lowering::cachedPropertyAddress(LValue object, LValue word, const AbstractHeap* heap)
{
    LValue location = m_out.aShr(m_out.shl(word, m_out.constInt32(32 - Slot::offsetBits)), m_out.constInt32(64 - Slot::offsetBits));
    LValue storage = m_out.select(m_out.lessThan(location, m_out.int64Zero), m_out.loadPtr(object, m_heaps.JSObject_butterfly), object);
    return TypedPointer(heap ? *heap : m_heaps.properties.atAnyNumber(), m_out.add(storage, m_out.shl(location, m_out.constInt32(3))));
}

std::optional<TypeTable::Field> Lowering::fieldAccessedBy(Node* node, unsigned identifier)
{
    if (!TypeTable::shared())
        return std::nullopt;
    UniquedStringImpl* name = node->graph->codeBlock()->identifier(identifier).impl();
    if (uint32_t tag = Graph::typeTagOf(node)) {
        if (auto field = TypeTable::shared()->fieldOf(tag, name))
            return field;
    }
    if (node->guard)
        return std::nullopt;
    return Graph::fieldOfWhatIsBornAs(node->use(node->opcode == op_get_by_id ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base), name);
}

// TEMPORARY-SHAPE-COUNTS
static std::atomic<uint64_t> s_shapeSites[Instance::NumberOfShapeCounts];
void reportShapeStatistics()
{
    dataLogLn("AOT: sites that go by a type: ", s_shapeSites[Instance::ReadHas].load(), " reads (", s_shapeSites[Instance::ReadLacks].load(), " of which also know what lacks it) of ", s_shapeSites[Instance::ReadHas].load() + s_shapeSites[Instance::ReadUntyped].load(),
        ", ", s_shapeSites[Instance::WriteHas].load(), " writes of ", s_shapeSites[Instance::WriteHas].load() + s_shapeSites[Instance::WriteUntyped].load(),
        "; ", s_shapeSites[Instance::LiteralWithLayout].load(), " literals of ", s_shapeSites[Instance::LiteralWithLayout].load() + s_shapeSites[Instance::LiteralWithout].load());
    dataLogLn("AOT: accesses in first copies: ", s_shapeSites[Instance::AssertionMade].load(), " test what the object was born as, ", s_shapeSites[Instance::ServedWithoutAssertion].load(), " know");
}
void noteShapeSite(Instance::ShapeCount which) { s_shapeSites[which].fetch_add(1, std::memory_order_relaxed); }

void Lowering::countShape(Instance::ShapeCount which)
{
    if (!Options::aotCountsAllocations()) [[likely]]
        return;
    TypedPointer count = m_out.address(m_heaps.root, m_instance, Instance::offsetOfShapeCounts() + which * sizeof(uint64_t));
    m_out.store64(m_out.add(m_out.load64(count), m_out.constInt64(1)), count);
}

LValue Lowering::layoutOf(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfKnownShape()));
}

LValue Lowering::layoutBornAs(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfBornAs()));
}

TypedPointer Lowering::slotOfStruct(LValue object, const TypeTable::Field& field)
{
    if (field.isInObject())
        return m_out.address(m_heaps.properties.atAnyNumber(), object, JSObject::offsetOfInlineStorage() + field.slot * sizeof(EncodedJSValue));
    LValue butterfly = m_out.loadPtr(m_out.address(m_heaps.root, object, JSObject::butterflyOffset()));
    return m_out.address(m_heaps.properties.atAnyNumber(), butterfly, offsetInButterfly(firstOutOfLineOffset + (field.slot - field.inlineSlots)) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)));
}

LValue Lowering::asHeld(Node* valueNode, LValue value, TypeTable::Holds holds)
{
    if (holds.atoms && TypeTable::areStructs())
        makeAtomIfString(valueNode, value);
    if (!holds.saysSomething() || !TypeTable::areStructs() || !mayBe(valueNode->type, TInt32))
        return value;
    if (isSubtype(valueNode->type, TNumber))
        return boxDouble(lowDouble(valueNode));
    return m_out.select(isInt32(value), boxDouble(m_out.intToDouble(unboxInt32(value))), value);
}

static Node* whatIsHandedOn(Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    return node;
}

auto Lowering::fieldInHand(Node* base, const TypeTable::Field& field) const -> const FieldInHand*
{
    base = whatIsHandedOn(base);
    for (auto& inHand : m_fieldsInHand) {
        if (inHand.base == base && inHand.family == field.first && inHand.slot == field.slot)
            return &inHand;
    }
    return nullptr;
}

void Lowering::noteFieldInHand(Node* base, const TypeTable::Field& field, LValue value, Rep rep, LValue asJSValue, bool isWritten)
{
    // TEMPORARY: for telling whether something is this one's doing.
    if (isWithout(WithoutFieldsInHand))
        return;
    base = whatIsHandedOn(base);
    // (Two values may be the one object. An object is of one family for life.)
    m_fieldsInHand.removeAllMatching([&](auto& inHand) {
        return inHand.family == field.first && inHand.slot == field.slot && (isWritten || inHand.base == base);
    });
    m_fieldsInHand.append({ base, field.first, field.slot, rep, value, asJSValue });
}

// Whatever the lowering makes of it, it neither writes to an object nor runs any of the program's code. This has to say no more than the lowerings deliver.
bool Lowering::leavesFieldsAlone(Node* node)
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
    case NodeKind::Narrow:
        return true;
    case NodeKind::Guard:
        return false;
    case NodeKind::Bytecode:
        break;
    }
    if (node->guard)
        return false;
    // (An object is asked what it is worth as a number or as a string, and the answer is the program's to give.)
    auto nothingIsAnObject = [&] {
        for (auto& use : node->uses) {
            if (!use.node->type || mayBe(use.node->type, TAnyObject))
                return false;
        }
        return true;
    };
    switch (node->opcode) {
    case op_type_tag:
    case op_check_type:
    case op_check_tdz:
    case op_to_this:
    case op_typeof:
    case op_not:
    case op_stricteq:
    case op_nstricteq:
    case op_jstricteq:
    case op_jnstricteq:
    case op_jmp:
    case op_jtrue:
    case op_jfalse:
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null:
    case op_eq_null:
    case op_neq_null:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_object:
    case op_is_callable:
    case op_is_cell_with_type:
    case op_is_empty:
    case op_loop_hint:
    case op_get_scope:
    case op_ret:
        return true;
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
    case op_less:
    case op_lesseq:
    case op_greater:
    case op_greatereq:
    case op_jless:
    case op_jlesseq:
    case op_jgreater:
    case op_jgreatereq:
    case op_jnless:
    case op_jnlesseq:
    case op_jngreater:
    case op_jngreatereq:
    case op_eq:
    case op_neq:
    case op_jeq:
    case op_jneq:
        return nothingIsAnObject();
    case op_get_length: {
        Type base = node->use(node->as<OpGetLength>().m_base)->type;
        return base && isSubtype(base, TArray | TString);
    }
    case op_get_from_scope:
        switch (node->as<OpGetFromScope>().m_getPutInfo.resolveType()) {
        case ClosureVar:
        case ModuleVar:
        case GlobalLexicalVar:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

bool Lowering::isThisOfWhatAnybodyMayCall(Node* node)
{
    return Graph::isThisOfWhatAnybodyMayCall(node);
}

void Lowering::assertBornAs(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t family)
{
    // Only what is born gets to be of the type, going by the text of the program. Whoever gets here has not been able to prove it, so it is looked into: what code without types puts into
    // an array that it was handed, say, is not in that text. TEMPORARY: BUN_AOT_TAKES_TYPES_AT_THEIR_WORD=1, as it used to be, for finding out what this costs.
    static const bool takesTypesAtTheirWord = [] { const char* text = getenv("BUN_AOT_TAKES_TYPES_AT_THEIR_WORD"); return text && !strcmp(text, "1"); }();
    if (takesTypesAtTheirWord && !isThisOfWhatAnybodyMayCall(valueNode))
        return;
    LBasicBlock isNot = newColdBlock();
    LBasicBlock is = m_out.newBlock();
    if (!isSubtype(valueNode->type, TCell)) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(isNot));
        m_out.appendTo(cellCase);
    }
    m_out.branch(m_out.equal(layoutBornAs(value), m_out.constInt32(family)), usually(is), rarely(isNot));
    m_out.appendTo(isNot);
    coldCall(onBehalfOf, Entry::operationAOTAssertBornAs, value, m_out.constInt32(family));
    m_out.jump(is);
    m_out.appendTo(is);
}

LValue Lowering::viewFoundFor(Node* node, uint16_t family)
{
    for (;;) {
        if (node->isBytecode(op_type_tag) && node->firstLayout == family) {
            if (auto it = m_views.find(node); it != m_views.end())
                return it->value;
        }
        if (node->kind != NodeKind::Narrow && !node->isBytecode(op_check_type) && !node->isBytecode(op_check_tdz) && !node->isBytecode(op_type_tag))
            return nullptr;
        node = node->uses[0].node;
    }
}

// (What an object was born as is for life, so this is good for as long as the value is.)
LValue Lowering::viewAs(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t family)
{
    LBasicBlock isNot = newColdBlock();
    LBasicBlock done = m_out.newBlock();
    if (!isSubtype(valueNode->type, TCell)) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(isNot));
        m_out.appendTo(cellCase);
    }
    ValueFromBlock itself = m_out.anchor(value);
    m_out.branch(m_out.equal(layoutBornAs(value), m_out.constInt32(family)), usually(done), rarely(isNot));
    m_out.appendTo(isNot);
    ValueFromBlock whatItIsSeenAs = m_out.anchor(coldCallForValue(onBehalfOf, Entry::operationAOTViewAs, value, m_out.constInt32(family)));
    m_out.jump(done);
    m_out.appendTo(done);
    return m_out.phi(Int64, itself, whatItIsSeenAs);
}

Lowering::StructToLookIn Lowering::structToLookIn(Node* onBehalfOf, Node* baseNode, LValue base, uint16_t family)
{
    if (baseNode->isKnownToBeBornWithin(family, family))
        return { base, false };
    // (An access whose base is in a register that is not followed has no op_type_tag ahead of it, and says so itself.)
    uint32_t tag = Graph::typeTagOf(onBehalfOf);
    if (!TypeTable::shared()->isOpen(family) || (tag && TypeTable::shared()->isTakenAtItsWord(tag) && TypeTable::shared()->familyOf(tag) == family && !isThisOfWhatAnybodyMayCall(baseNode))) {
        assertBornAs(onBehalfOf, baseNode, base, family);
        return { base, false };
    }
    if (LValue view = viewFoundFor(baseNode, family))
        return { view, true };
    return { viewAs(onBehalfOf, baseNode, base, family), true };
}

// Zero for what is not a cell. It is for life, so once it has been asked of a value it need not be asked again: not after a call, and not after a store.
LValue Lowering::layoutBornAsOrNone(Node* node, LValue value)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz))
        node = node->uses[0].node;
    if (auto it = m_layoutsBornAs.find(node); it != m_layoutsBornAs.end() && it->value.first == m_block)
        return it->value.second;
    LValue result;
    if (isSubtype(node->type, TCell))
        result = layoutBornAs(value);
    else {
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock none = m_out.anchor(m_out.int32Zero);
        m_out.branch(isCell(value), usually(cellCase), rarely(continuation));
        m_out.appendTo(cellCase);
        ValueFromBlock ofCell = m_out.anchor(layoutBornAs(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        result = m_out.phi(Int32, none, ofCell);
    }
    m_layoutsBornAs.set(node, std::pair { m_block, result });
    return result;
}

LValue Lowering::isOneOf(LValue layout, uint16_t first, uint16_t last)
{
    if (first == last)
        return m_out.equal(layout, m_out.constInt32(first));
    return m_out.belowOrEqual(m_out.sub(layout, m_out.constInt32(first)), m_out.constInt32(last - first));
}

void Lowering::lowerGetById(Node* node)
{
    auto bytecode = node->as<OpGetById>();
    Node* baseNode = node->use(bytecode.m_base);
    // A closed method that is called with no need of the function object: which function it is is known, and all that reading it would do is throw if there is nothing to read it from.
    if (node->isReadOnlyToBeCalled) {
        if (!isSubtype(baseNode->type, TCell)) {
            LValue base = lowJSValue(baseNode);
            LBasicBlock isNone = newColdBlock();
            LBasicBlock isSomething = m_out.newBlock();
            m_out.branch(isCell(base), usually(isSomething), rarely(isNone));
            m_out.appendTo(isNone);
            coldCall(node, Entry::operationAOTCheckType, base, m_out.constInt32(MaskOtherObject), ColdCall::ChangesNothing);
            m_out.unreachable();
            m_out.appendTo(isSomething);
        }
        return;
    }
    if (node->builtinCalled) {
        isLoweredThisWay(3);
        lowerReadOfBuiltin(node, baseNode);
        return;
    }
    // (With Options::aotAssertsTypes() that is for the guards of the first copy. What gets here is the other.)
    if (auto field = (Options::aotShapes() & 2) && !Options::aotAssertsTypes() && !Options::aotAuditsTypes() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt) {
        // An object that a literal of the program made says how it is laid out, and the type says which layouts have the property, and
        // where. Whatever else the base may be, in spite of its type, is dealt with as if nothing had been said.
        LValue base = lowJSValue(baseNode);
        noteShapeSite(Instance::ReadHas);
        if (Options::aotTypesFields() && TypeTable::areStructs()) {
            // The property is in its slot, or the object has none.
            auto [structOfBase, mayStandForSomethingElse] = structToLookIn(node, baseNode, base, field->first);
            isLoweredThisWay(mayStandForSomethingElse ? 2 : 1);
            countShape(Instance::ReadHas);
            if (!mayStandForSomethingElse) {
                m_nodeLeavesFieldsAlone = true;
                if (const FieldInHand* inHand = fieldInHand(baseNode, *field)) {
                    if (inHand->asJSValue && node->rep() == Rep::JSValue)
                        setJSValue(node, inHand->asJSValue);
                    else {
                        setResult(node, inHand->value, inHand->rep);
                        if (inHand->asJSValue && node->rep() != Rep::JSValue)
                            node->loweredAsJSValue = inHand->asJSValue;
                    }
                    return;
                }
            }
            LValue whatIsThere = m_out.load64(slotOfStruct(structOfBase, *field));
            bool undefinedWillDo = field->isOptional || !field->holds.saysSomething() || (field->holds.kinds & MaskUndefined);
            if (mayStandForSomethingElse) {
                RELEASE_ASSERT(field->isInObject());
                LBasicBlock theLongWay = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                Vector<ValueFromBlock, 3> results;
                results.append(m_out.anchor(whatIsThere));
                if ((field->isOptional || field->mayBeEmpty) && undefinedWillDo) {
                    LBasicBlock nothingIsThere = m_out.newBlock();
                    m_out.branch(m_out.notZero64(whatIsThere), usually(continuation), rarely(nothingIsThere));
                    m_out.appendTo(nothingIsThere);
                    results.append(m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined()))));
                    m_out.branch(m_out.equal(structOfBase, base), usually(continuation), rarely(theLongWay));
                } else
                    m_out.branch(m_out.notZero64(whatIsThere), usually(continuation), rarely(theLongWay));
                m_out.appendTo(theLongWay);
                // (What comes next takes it for what the slot holds. So it is that, or this does not come back.)
                uint64_t which = static_cast<uint64_t>(numberOf(bytecode.m_property)) | static_cast<uint64_t>(field->first) << 32 | static_cast<uint64_t>(field->slot) << 48 | static_cast<uint64_t>(undefinedWillDo) << 56 | 1ull << 63; // (The last so that it is not taken for an address.)
                results.append(m_out.anchor(coldCallForValue(node, Entry::operationAOTGetFieldTheLongWay, base, m_out.constInt64(which))));
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                setJSValue(node, m_out.phi(Int64, results));
                return;
            }
            if (field->isOptional || (field->mayBeEmpty && undefinedWillDo))
                whatIsThere = m_out.select(m_out.isZero64(whatIsThere), m_out.constInt64(JSValue::encode(jsUndefined())), whatIsThere);
            else if (field->mayBeEmpty) {
                // The type says it is there. What comes next takes it for what the type says.
                LBasicBlock isMissing = newColdBlock();
                LBasicBlock isThere = m_out.newBlock();
                m_out.branch(m_out.notZero64(whatIsThere), usually(isThere), rarely(isMissing));
                m_out.appendTo(isMissing);
                coldCall(node, Entry::operationAOTCheckType, m_out.constInt64(JSValue::encode(jsUndefined())), m_out.constInt32(field->holds.kinds));
                m_out.unreachable();
                m_out.appendTo(isThere);
            }
            setJSValue(node, whatIsThere);
            noteFieldInHand(baseNode, *field, node->lowered, node->rep(), whatIsThere, false);
            return;
        }
        bool resultIsTyped = Options::aotTypesFields() && field->holds.saysSomething();
        // (undefined has to be something the field is said to hold.)
        bool testsForLack = field->firstWithout && (Options::aotShapes() & 8) && Options::useImmutableIntrinsics() && (!resultIsTyped || (field->holds.kinds & MaskUndefined));
        LBasicBlock has = m_out.newBlock();
        LBasicBlock hasNot = m_out.newBlock();
        LBasicBlock mayLack = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (baseNode->isKnownToBeBornWithin(field->first, field->last)) {
            noteShapeSite(Instance::ServedWithoutAssertion);
            countShape(Instance::ServedWithoutAssertion);
            m_out.jump(has);
        } else {
            noteShapeSite(Instance::AssertionMade);
            countShape(Instance::AssertionMade);
            // What it was born as, whatever has become of it since: the property is where it was then, or nothing is (Structure::bornAs()).
            LValue layout = layoutBornAsOrNone(baseNode, base);
            if (testsForLack)
                noteShapeSite(Instance::ReadLacks);
            if (!testsForLack)
                m_out.branch(isOneOf(layout, field->first, field->last), usually(has), rarely(otherwise));
            else {
                // (What was never born as anything may not be a cell at all.)
                LBasicBlock hasItNot = m_out.newBlock();
                m_out.branch(isOneOf(layout, field->first, field->last), usually(has), unsure(hasItNot));
                m_out.appendTo(hasItNot);
                m_out.branch(m_out.notZero32(layout), usually(mayLack), rarely(otherwise));
            }
        }
        m_out.appendTo(has, mayLack);
        LValue whatIsThere = m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        LBasicBlock isThere = m_out.newBlock();
        m_out.branch(m_out.notZero64(whatIsThere), usually(isThere), rarely(otherwise));
        m_out.appendTo(isThere);
        countShape(Instance::ReadHas);
        ValueFromBlock found = m_out.anchor(whatIsThere);
        m_out.jump(continuation);
        // (Such an object inherits from Object.prototype, which has what it had to begin with: TypeTable::fieldOf() has seen to that.)
        m_out.appendTo(mayLack, hasNot);
        if (testsForLack)
            // (That it has no such property, and inherits none, goes for what it is now.)
            m_out.branch(isOneOf(layoutOf(base), field->firstWithout, field->lastWithout), unsure(hasNot), unsure(otherwise));
        else
            m_out.unreachable();
        m_out.appendTo(hasNot, otherwise);
        countShape(Instance::ReadLacks);
        ValueFromBlock lacking = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.jump(continuation);
        m_out.appendTo(otherwise, continuation);
        countShape(Instance::ReadOther);
        LValue readTheLongWay = getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property);
        if (resultIsTyped) {
            // What comes next takes it for what the field is said to hold.
            LBasicBlock isThat = m_out.newBlock();
            LBasicBlock notSettled = m_out.newBlock();
            emitTypeTests(nullptr, TTop, readTheLongWay, field->holds.kinds, isThat, notSettled);
            m_out.appendTo(notSettled);
            vmCall(node, Void, Entry::operationAOTCheckType, m_globalObject, readTheLongWay, m_out.constInt32(field->holds.kinds));
            m_out.jump(isThat);
            m_out.appendTo(isThat);
        }
        ValueFromBlock other = m_out.anchor(readTheLongWay);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, found, lacking, other));
        return;
    }
    if (node->hasFact(FactField, 1)) {
        LValue address = m_out.add(lowJSValue(baseNode), m_out.constIntPtr(JSObject::offsetOfInlineStorage() + 8 * (node->fact & 255)));
        setJSValue(node, m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), address)));
        return;
    }
    if (node->hasFact(FactBuiltin, 8)) {
        // The function, from where the realm would keep it.
        LValue address = m_out.add(m_globalObject, m_out.constIntPtr(1024 + 8 * (node->fact & 1023)));
        setJSValue(node, m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), address)));
        return;
    }
    noteShapeSite(Instance::ReadUntyped);
    countShape(Instance::ReadUntyped);
    if (Options::aotCountsAllocations()) [[unlikely]] {
        uint32_t tag = Graph::typeTagOf(node);
        TypedPointer count = m_out.address(m_heaps.root, m_instance, Instance::offsetOfReadsForReason() + (tag && TypeTable::shared() ? TypeTable::shared()->reasonOf(tag) : 0) * sizeof(uint64_t));
        m_out.store64(m_out.add(m_out.load64(count), m_out.constInt64(1)), count);
    }
    setJSValue(node, getByIdCached(node, lowJSValue(baseNode), baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
}

// operation: takes the global object, the base, identifier (whatever that means to it) and the cache, which it fills.
LValue Lowering::getByIdCached(Node* node, LValue base, Type baseType, Entry operation, unsigned identifierOfFunction)
{
    unsigned identifier = operation == Entry::operationAOTGetByIdWellKnown ? identifierOfFunction : numberOf(identifierOfFunction);
    std::optional<Stub> stub;
    if (usesStubs && Site::fits(identifier, 0)) {
        if (operation == Entry::operationAOTGetById)
            stub = Stub::GetById;
        else if (operation == Entry::operationAOTGetByIdWellKnown)
            stub = Stub::GetByIdWellKnown;
    }
    unsigned slot = stub ? sharedSite(node, identifier) : allocateSlot();
    if (stub == Stub::GetById)
        m_graph.noteSelectorOfSite(slot, code().codeBlock()->identifier(identifierOfFunction).impl());
    auto throughStub = [&]() -> LValue {
        return callStub(*stub, Int64, { { base, GPRInfo::argumentGPR0 }, { slotAddress(slot), GPRInfo::argumentGPR1 } }, { });
    };
    if (stub && isCompact())
        return throughStub();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock rightStructure = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseType, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, rightStructure);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(rightStructure), rarely(slowCase));

    m_out.appendTo(rightStructure, hit);
    ValueFromBlock fastResult;
    if (stub) {
        // Whatever takes more than a load from the base itself is the stub's business: a getter, what is inherited, what is out of line.
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter | Slot::isIntricate) << 32)), usually(hit), rarely(slowCase));
        m_out.appendTo(hit, slowCase);
        LValue location = m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::offsetMask));
        fastResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(base, m_out.shl(location, m_out.constInt32(3))))));
    } else {
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter) << 32)), usually(hit), rarely(slowCase));

        // In the base, or in an object that every base of this structure inherits it from (see cacheGetById()).
        // (Unless there is more to it than a place in the base, what is there is the name: Slot::name.)
        m_out.appendTo(hit, slowCase);
        LValue holder = m_out.loadPtr(slotWord(slot, 1));
        LValue isElsewhere = m_out.bitAnd(m_out.testNonZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isIntricate) << 32)), m_out.notNull(holder));
        fastResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(isElsewhere, holder, base), word)));
    }
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(stub ? throughStub() : vmCall(node, Int64, operation, m_globalObject, base, m_out.constInt32(identifier), slotAddress(slot)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, fastResult, slowResult);
}

void Lowering::lowerPutById(Node* node)
{
    auto bytecode = node->as<OpPutById>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);
    uint32_t flags = (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0);
    LBasicBlock afterTypedStore = nullptr;
    if (auto field = (Options::aotShapes() & 4) && !Options::aotAssertsTypes() && !Options::aotAuditsTypes() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt) {
        // As for a read. A property of such a layout is one that can be written, like any that a literal makes.
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock has = m_out.newBlock();
        LBasicBlock otherwise = Options::aotTypesFields() && TypeTable::areStructs() ? newColdBlock() : m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (Options::aotTypesFields() && TypeTable::areStructs()) {
            auto [structOfBase, mayStandForSomethingElse] = structToLookIn(node, baseNode, base, field->first);
            isLoweredThisWay(mayStandForSomethingElse ? 2 : 1);
            // What the slot does not hold goes the long way, where it is made to be that or refused. So does what makes the object have a property it did not have.
            bool mayNotBeHeld = branchUnlessHeld(valueNode, value, field->holds, otherwise);
            TypedPointer slotOfField = slotOfStruct(structOfBase, *field);
            LValue valueAsHeld = asHeld(valueNode, value, field->holds);
            // (If it may not be what the slot holds, what is here is not on every way to what comes next.)
            if (!mayStandForSomethingElse && !mayNotBeHeld) {
                // (Giving an object a field it did not have may take a call. It is of the engine's own code, and it does nothing to any other field.)
                m_nodeLeavesFieldsAlone = true;
                if (valueNode->type && isSubtype(valueNode->type, TNumber))
                    noteFieldInHand(baseNode, *field, lowDouble(valueNode), Rep::Double, valueAsHeld, true);
                else
                    noteFieldInHand(baseNode, *field, valueAsHeld, Rep::JSValue, valueAsHeld, true);
            }
            // (What is no struct is seen as one with nothing in it, and nothing is ever put there.)
            if (field->isOptional || field->mayBeEmpty || mayStandForSomethingElse) {
                RELEASE_ASSERT(!mayStandForSomethingElse || field->isInObject());
                LBasicBlock isThere = m_out.newBlock();
                LBasicBlock isEmpty = (field->isOptional || field->mayBeEmpty) && field->isInObject() && !isWithout(WithoutAddsOfFields) ? m_out.newBlock() : nullptr;
                m_out.branch(m_out.notZero64(m_out.load64(slotOfField)), unsure(isThere), isEmpty ? unsure(isEmpty) : rarely(otherwise));
                if (isEmpty) {
                    // The object is given the field. What it is of afterwards is what the last of its kind to be given it was (Instance::addsOfFields).
                    m_out.appendTo(isEmpty);
                    if (mayStandForSomethingElse)
                        orElse(m_out.equal(structOfBase, base), otherwise);
                    if (isCompact())
                        orElse(m_out.notNull(callHelper(Stub::HelperAddField, { base, valueAsHeld, m_out.constInt32(field->slot) })), otherwise);
                    else
                        addFieldOfStruct(base, valueAsHeld, m_out.constInt32(field->slot), otherwise);
                    // (Whatever the value is: the object refers to another Structure now.)
                    storeBarrier(base);
                    m_out.jump(afterTypedStore);
                }
                m_out.appendTo(isThere);
            }
            noteShapeSite(Instance::WriteHas);
            countShape(Instance::WriteHas);
            m_out.store64(valueAsHeld, slotOfField);
            if (mayBe(valueNode->type, TCell))
                storeBarrier(base);
            m_out.jump(afterTypedStore);
            m_out.appendTo(cellCase);
            m_out.unreachable();
            m_out.appendTo(has);
            m_out.unreachable();
            m_out.appendTo(otherwise, afterTypedStore);
            countShape(Instance::WriteOther);
        } else {
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(otherwise));
        m_out.appendTo(cellCase, has);
        m_out.branch(isOneOf(layoutOf(base), field->first, field->last), usually(has), rarely(otherwise));
        m_out.appendTo(has, otherwise);
        // (What the slot does not hold is stored the long way, which takes the property out of the slot.)
        if (Options::aotTypesFields())
            branchUnlessHeld(valueNode, value, field->holds.kindsOnly(), otherwise);
        noteShapeSite(Instance::WriteHas);
        countShape(Instance::WriteHas);
        m_out.store64(value, m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise, afterTypedStore);
        countShape(Instance::WriteOther);
        }
    } else {
        noteShapeSite(Instance::WriteUntyped);
        countShape(Instance::WriteUntyped);
    }
    if (!afterTypedStore && !bytecode.m_flags.isDirect() && mayBe(baseNode->type, TArray) && mayBe(valueNode->type, TInt32) && code().codeBlock()->identifier(bytecode.m_property).impl() == m_graph.vm().propertyNames->length.impl()) {
        LBasicBlock otherwise = m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (isCompact())
            orElse(m_out.notNull(callHelper(Stub::HelperSetArrayLength, { base, value })), otherwise);
        else
            setLengthOfArray(base, value, otherwise);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise);
    }
    auto finish = makeScopeExit([&] {
        if (afterTypedStore) {
            m_out.jump(afterTypedStore);
            m_out.appendTo(afterTypedStore);
        }
    });
    if (isCompact() && Site::fits(numberOf(bytecode.m_property), flags)) {
        unsigned slot = sharedSite(node, numberOf(bytecode.m_property), flags);
        m_graph.noteSelectorOfSite(slot, code().codeBlock()->identifier(bytecode.m_property).impl());
        callStub(Stub::PutById, Void, { { base, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(slot), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    unsigned slot = allocateSlot();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock transition = m_out.newBlock();
    LBasicBlock stored = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseNode->type, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, hit);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(hit), rarely(slowCase));

    m_out.appendTo(hit, transition);
    LValue secondWord = m_out.load64(slotWord(slot, 1));
    if (Options::aotTypesFields() && TypeTable::areStructs()) {
        // (Slot::held: there is looking at the value to be done. The runtime does that.)
        LBasicBlock isPlain = m_out.newBlock();
        m_out.branch(m_out.isZero64(m_out.lShr(secondWord, m_out.constInt32(32))), usually(isPlain), rarely(slowCase));
        m_out.appendTo(isPlain);
    }
    m_out.store64(value, cachedPropertyAddress(base, word));
    LValue newStructureID = lowHalf(m_out, secondWord);
    m_out.branch(m_out.notZero32(newStructureID), unsure(transition), unsure(stored));

    m_out.appendTo(transition, stored);
    m_out.store32(newStructureID, base, m_heaps.JSCell_structureID);
    m_out.jump(stored);

    m_out.appendTo(stored, slowCase);
    if (mayBe(valueNode->type, TCell))
        storeBarrier(base);
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    vmCall(node, Void, Entry::operationAOTPutById, m_globalObject, base, value, m_out.constInt32(numberOf(bytecode.m_property)), slotAddress(slot), m_out.constInt32(flags));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The property as an array index, on the way to haveIndex. Everything that is not one goes to notIndex.
LValue Lowering::lowIndex(Node* propertyNode, LBasicBlock haveIndex, LBasicBlock notIndex)
{
    if (propertyNode->rep() == Rep::Int32) {
        LValue index = lowInt32(propertyNode);
        m_out.jump(haveIndex);
        return index;
    }
    if (propertyNode->rep() == Rep::Int64) {
        LValue wide = lowRaw(propertyNode);
        LValue index = m_out.castToInt32(wide);
        m_out.branch(m_out.belowOrEqual(wide, m_out.constInt64(INT32_MAX)), usually(haveIndex), rarely(notIndex));
        return index;
    }
    if (propertyNode->rep() == Rep::Double) {
        LValue asDouble = lowDouble(propertyNode);
        LValue index = m_out.doubleToInt32(asDouble);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(index), asDouble), usually(haveIndex), rarely(notIndex));
        return index;
    }
    LValue property = lowJSValue(propertyNode);
    LValue index = unboxInt32(property);
    m_out.branch(isInt32(property), usually(haveIndex), unsure(notIndex));
    return index;
}

void Lowering::lowerGetByVal(Node* node)
{
    auto bytecode = node->as<OpGetByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    LValue base = lowJSValue(baseNode);
    bool emptyWillDo = node->graph->readsElementsOrEmpty;

    if (node->hasFact(FactElement, 32)) {
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = isSubtype(propertyNode->type, TInt32) ? lowInt32(propertyNode) : unboxInt32(lowJSValue(propertyNode));
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        ValueFromBlock beyond = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(continuation));
        m_out.appendTo(inBounds, continuation);
        ValueFromBlock element = m_out.anchor(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index))));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, beyond, element));
        return;
    }

    if (auto* view = viewOf(node, baseNode)) {
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock theLongWay = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = lowInt64(propertyNode);
        m_out.branch(m_out.below(index, view->limit), usually(inBounds), rarely(theLongWay));
        m_out.appendTo(inBounds);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, view->butterfly, index));
        ValueFromBlock fast = m_out.anchor(element);
        // (An array that keeps its elements this way has nothing at an index where it keeps nothing: what it inherits from has no elements, or it would keep them another way.)
        if (emptyWillDo)
            m_out.jump(continuation);
        else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(theLongWay));
        m_out.appendTo(theLongWay);
        ValueFromBlock slow = m_out.anchor(coldCallForValue(node, emptyWillDo ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode), ColdCall::ChangesNothing));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fast, slow));
        return;
    }

    if (isCompact() && !emptyWillDo) {
        if (propertyNode->rep() == Rep::Int64)
            setJSValue(node, callBinaryStub(node, Stub::GetByValAtIndex, Int64, base, lowRaw(propertyNode)));
        else
            setJSValue(node, callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode)));
        return;
    }

    bool isOfArray = isSubtype(baseNode->type, TArray);
    // (Of what is not known for an array, nothing is made of there being nothing where an element would be kept: it is asked.)
    bool nothingKeptIsNothingThere = emptyWillDo && isOfArray;
    if (emptyWillDo && !isOfArray && (Options::aotVerbose() || Options::aotReportStats())) [[unlikely]] {
        dataLog("AOT: LEAN an element is read from what is not known for an array: ");
        baseNode->dump(WTF::dataFile());
        dataLogLn(m_block->isGeneric ? " (in the second copy of a loop)" : "", m_block->isInLoop ? " (in a loop)" : "");
    }
    LBasicBlock slowCase = isOfArray ? newColdBlock() : m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    // An in-bounds, non-hole element of an array-like with contiguous or int32 storage.
    if (!(Options::aotDisableFastPaths() & 8) && mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        // Int32Shape and ContiguousShape hold JSValues; DoubleShape sits between them.
        LValue isJSValueShape = m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape)));
        m_out.branch(isJSValueShape, usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, inBounds);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(slowCase));

        m_out.appendTo(inBounds, slowCase);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        results.append(m_out.anchor(element));
        if (nothingKeptIsNothingThere)
            m_out.jump(continuation);
        else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if (emptyWillDo && !isOfArray)
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetElementOrEmpty, m_globalObject, base, lowJSValue(propertyNode))));
    else if (isOfArray)
        results.append(m_out.anchor(coldCallForValue(node, emptyWillDo ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode))));
    else if constexpr (usesStubs)
        results.append(m_out.anchor(callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode))));
    else
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetByVal, m_globalObject, base, lowJSValue(propertyNode))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutByVal(Node* node)
{
    auto bytecode = node->as<OpPutByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);

    auto throughStub = [&] {
        bool isInteger = propertyNode->rep() == Rep::Int64;
        callStub(isInteger ? Stub::PutByValAtIndex : Stub::PutByVal, Void, { { base, GPRInfo::argumentGPR0 }, { isInteger ? lowRaw(propertyNode) : lowJSValue(propertyNode), GPRInfo::argumentGPR1 }, { value, GPRInfo::argumentGPR2 } },
            { { GPRInfo::argumentGPR3, bytecode.m_ecmaMode.isStrict() } });
    };
    if (isCompact()) {
        throughStub();
        return;
    }

    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    // An element for which there is room in contiguous storage. Growing that and every other shape are the runtime's.
    if (!(Options::aotDisableFastPaths() & 16) && mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock beyondLength = m_out.newBlock();
        LBasicBlock lengthen = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        // Copy on write storage has the shape bits of what it holds and one more bit.
        LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, beyondLength);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), unsure(beyondLength));

        m_out.appendTo(beyondLength, lengthen);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)), unsure(lengthen), unsure(slowCase));

        // What is between the old length and here are holes already.
        m_out.appendTo(lengthen, inBounds);
        m_out.store32(m_out.add(index, m_out.int32One), butterfly, m_heaps.Butterfly_publicLength);
        m_out.jump(inBounds);

        m_out.appendTo(inBounds, slowCase);
        m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if constexpr (usesStubs)
        throughStub();
    else
        vmCall(node, Void, Entry::operationAOTPutByVal, m_globalObject, base, lowJSValue(propertyNode), value, m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// ---- Scopes

B3::Variable* Lowering::variableOfEnvironment(Node* environment, unsigned offset)
{
    auto& variables = m_variablesOfEnvironments.ensure(environment, [&] {
        JSValue table = environment->graph->codeBlock()->getConstant(environment->as<OpCreateLexicalEnvironment>().m_symbolTable);
        Vector<B3::Variable*> result;
        for (unsigned i = uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize(); i--;)
            result.append(m_proc.addVariable(Int64));
        return result;
    }).iterator->value;
    RELEASE_ASSERT(offset < variables.size());
    return variables[offset];
}

LValue Lowering::scopeThatIsOutFrom(Node* scope, unsigned hops)
{
    LValue current = lowCell(scope);
    for (unsigned i = 0; i < hops; ++i)
        current = m_out.loadPtr(current, m_heaps.JSScope_next);
    return current;
}

LValue Lowering::scopeToResolveFrom(Node* resolve)
{
    if (resolve->environmentsPassedOver)
        return scopeThatIsOutFrom(resolve->scopeToStartFrom, resolve->hopsFromThere);
    return lowCell(resolve->use(resolve->as<OpResolveScope>().m_scope));
}

void Lowering::lowerResolveScope(Node* node)
{
    auto bytecode = node->as<OpResolveScope>();
    if (auto distance = m_graph.distanceOfEnvironmentResolvedTo(node)) {
        setJSValue(node, environmentAt(*distance));
        return;
    }
    if (node->scopeToStartFrom && !node->environmentsPassedOver) {
        setJSValue(node, scopeThatIsOutFrom(node->scopeToStartFrom, node->hopsFromThere));
        return;
    }
    LValue scope = scopeToResolveFrom(node);

    auto walk = [&](unsigned depth) {
        RELEASE_ASSERT(depth >= node->environmentsPassedOver);
        LValue current = scope;
        for (unsigned i = node->environmentsPassedOver; i < depth; ++i)
            current = m_out.loadPtr(current, m_heaps.JSScope_next);
        return current;
    };

    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        setJSValue(node, walk(bytecode.m_localScopeDepth + staticClosureVarHops(bytecode.m_resolveType)));
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
    if (variable.isAtStaticDepth()) {
        setJSValue(node, walk(variable.depth));
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The environment of the module that has the variable, which the environment of this one is given the first time it is
        // asked for.
        LValue importer = walk(variable.depth);
        LValue exporter = m_out.load64(importer, m_heaps.JSLexicalEnvironment_variables[variable.import.scopeOffsetOfSlot]);
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(exporter);
        m_out.branch(m_out.isZero64(exporter), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTFillImportSlot, m_globalObject, importer, m_out.constInt32(variable.import.slot)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
        return;
    }

    unsigned extra = code().extraOfResolveScope(bytecode);
    if (isFusedWithGetFromScope(node))
        return;
    if (usesStubs && variable.kind == StaticVariable::Unresolved && Site::fits(numberOf(bytecode.m_var), extra)) {
        setJSValue(node, callStub(Stub::ResolveScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), extra)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    if (variable.kind == StaticVariable::Unresolved) {
        LValue cached = m_out.loadPtr(slotWord(slot, 1));
        LValue epochPlusOne = highHalf(m_out, m_out.load64(slotWord(slot, 0)));
        LValue epoch = m_out.load32(m_out.address(m_heaps.root, m_globalObject, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()));
        results.append(m_out.anchor(cached));
        m_out.branch(m_out.equal(epochPlusOne, m_out.add(epoch, m_out.constInt32(1))), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTResolveScope, m_globalObject, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(variable.isInGlobalScopes ? Site::resolvesInGlobalScopes : 0))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::isFusedWithGetFromScope(Node* node)
{
    if (!usesStubs || !node->isBytecode(op_resolve_scope) || node->block != m_block || node->useCount != 1)
        return false;
    auto resolve = node->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(resolve.m_resolveType) || !Site::fits(numberOf(resolve.m_var), code().extraOfResolveScope(resolve)))
        return false;
    if (resolveStatically(resolve.m_var, resolve.m_localScopeDepth, resolve.m_resolveType).kind != StaticVariable::Unresolved)
        return false;

    // Asked when lowering either of the two.
    unsigned index = m_block->nodes[m_nodeIndex] == node ? m_nodeIndex : m_nodeIndex - 1;
    if (index >= m_block->nodes.size() || m_block->nodes[index] != node || index + 1 >= m_block->nodes.size())
        return false;
    Node* next = m_block->nodes[index + 1];
    if (!next->isBytecode(op_get_from_scope))
        return false;
    auto get = next->as<OpGetFromScope>();
    ResolveType type = get.m_getPutInfo.resolveType();
    return next->use(get.m_scope) == node && get.m_var == resolve.m_var
        && type != ResolvedClosureVar && type != ResolvedLazyClosureVar
        && resolveStatically(get.m_var, get.m_localScopeDepth, type).kind == StaticVariable::Unresolved;
}

void Lowering::lowerGetFromScope(Node* node)
{
    auto bytecode = node->as<OpGetFromScope>();
    if (node->promotedEnvironment) {
        setJSValue(node, m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variableOfEnvironment(node->promotedEnvironment, node->offsetInEnvironment)));
        return;
    }
    if (Node* resolveNode = node->use(bytecode.m_scope); isFusedWithGetFromScope(resolveNode)) {
        auto resolve = resolveNode->as<OpResolveScope>();
        LValue scope = scopeToResolveFrom(resolveNode);
        unsigned site = allocateSite(resolveNode, numberOf(resolve.m_var), code().extraOfResolveScope(resolve));
        unsigned siteOfGet = allocateSite(node, numberOf(bytecode.m_var), code().extraOfGetFromScope(bytecode));
        RELEASE_ASSERT(siteOfGet == site + 1);
        setJSValue(node, callStub(Stub::GetGlobal, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(site), GPRInfo::argumentGPR1 } }, { }));
        return;
    }
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    auto loadClosureVariable = [&](unsigned offset, bool mayBeLazy) {
        LValue value = m_out.load64(scope, m_heaps.JSLexicalEnvironment_variables[offset]);
        if (!mayBeLazy) {
            setJSValue(node, value);
            return;
        }
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(value);
        m_out.branch(m_out.isZero64(value), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTReadLazyClosureVar, m_globalObject, distance ? environmentAt(*distance) : scope, m_out.constInt32(offset)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
    };

    if (type == ResolvedClosureVar) {
        loadClosureVariable(bytecode.m_offset, false);
        return;
    }
    if (type == ResolvedLazyClosureVar) {
        loadClosureVariable(bytecode.m_offset, true);
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
    if (variable.kind == StaticVariable::Closure) {
        loadClosureVariable(variable.offset.offset(), variable.inModule);
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The scope is the environment of the module that has it (lowerResolveScope()).
        loadClosureVariable(variable.offset.offset(), true);
        return;
    }

    unsigned throwIfNotFound = code().extraOfGetFromScope(bytecode);
    if (usesStubs && variable.isCachedInSlot() && Site::fits(numberOf(bytecode.m_var), throwIfNotFound)) {
        setJSValue(node, callStub(Stub::GetFromScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), throwIfNotFound)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    if (variable.isCachedInSlot()) {
        LBasicBlock hasAddress = m_out.newBlock();
        LBasicBlock noAddress = m_out.newBlock();
        LBasicBlock structureHit = m_out.newBlock();

        // Either way what is cached is only good for the kind of scope it was found in: the name may resolve to another.
        LValue word = m_out.load64(slotWord(slot, 0));
        m_out.branch(m_out.equal(m_out.load32(scope, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(structureHit), rarely(slowCase));

        m_out.appendTo(structureHit, hasAddress);
        LValue address = m_out.loadPtr(slotWord(slot, 1));
        m_out.branch(m_out.notNull(address), unsure(hasAddress), unsure(noAddress));

        m_out.appendTo(hasAddress, noAddress);
        LValue value = m_out.load64(TypedPointer(m_heaps.root, address));
        results.append(m_out.anchor(value));
        m_out.branch(m_out.notZero64(value), usually(continuation), rarely(slowCase));

        m_out.appendTo(noAddress, slowCase);
        results.append(m_out.anchor(loadProperty(scope, highHalf(m_out, word))));
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetFromScope, m_globalObject, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(throwIfNotFound))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutToScope(Node* node)
{
    auto bytecode = node->as<OpPutToScope>();
    if (node->promotedEnvironment) {
        m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variableOfEnvironment(node->promotedEnvironment, node->offsetInEnvironment), lowJSValue(node->use(bytecode.m_value)));
        return;
    }
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    Node* valueNode = node->use(bytecode.m_value);
    LValue value = lowJSValue(valueNode);
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    std::optional<unsigned> closureOffset;
    if (type == ResolvedClosureVar)
        closureOffset = bytecode.m_offset;
    else {
        StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_symbolTableOrScopeDepth.scopeDepth(), type);
        if (variable.kind == StaticVariable::Closure && (!variable.isReadOnly || isInitialization(bytecode.m_getPutInfo.initializationMode())))
            closureOffset = variable.offset.offset();
    }

    if (closureOffset) {
        // Nobody is watching it: where there is code from the static compiler, the optimizing JITs take no closure variable for a
        // constant (DFG::Graph::tryGetConstantClosureVar()).
        m_out.store64(value, scope, m_heaps.JSLexicalEnvironment_variables[*closureOffset]);
        if (mayBe(valueNode->type, TCell))
            storeBarrier(scope);
        return;
    }

    // What the operation has to know about the instruction, small enough for a Site.
    GetPutInfo info = bytecode.m_getPutInfo;
    static_assert(static_cast<unsigned>(ThrowIfNotFound) <= 1 && static_cast<unsigned>(DoNotThrowIfNotFound) <= 1);
    unsigned how = static_cast<unsigned>(info.resolveMode()) | static_cast<unsigned>(info.initializationMode()) << 1 | info.ecmaMode().isStrict() << 3;
    RELEASE_ASSERT(static_cast<unsigned>(info.initializationMode()) <= 3);

    if (usesStubs && Site::fits(numberOf(bytecode.m_var), how)) {
        callStub(Stub::PutToScope, Void, { { scope, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, numberOf(bytecode.m_var), how)), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    vmCall(node, Void, Entry::operationAOTPutToScope, m_globalObject, scope, value, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(allocateSlot()), m_out.constInt32(how));
}

bool Lowering::tryLowerAccess(Node* node)
{
    switch (node->opcode) {
    case op_get_by_id:
        lowerGetById(node);
        return true;
    case op_put_by_id:
        lowerPutById(node);
        return true;
    case op_get_by_val:
        lowerGetByVal(node);
        return true;
    case op_put_by_val:
        lowerPutByVal(node);
        return true;
    case op_resolve_scope:
        lowerResolveScope(node);
        return true;
    case op_get_from_scope:
        lowerGetFromScope(node);
        return true;
    case op_put_to_scope:
        lowerPutToScope(node);
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
