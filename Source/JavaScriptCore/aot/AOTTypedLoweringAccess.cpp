/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"
#include "AOTTypedObject.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

#include "B3MemoryValue.h"
#include "B3StackmapGenerationParams.h"
#include "IteratorOperations.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

std::optional<FieldAccess> fieldOf(const Program& program, const Type& receiver, StringID name)
{
    LayoutID supertype = receiver.singleLayout();
    if (!supertype)
        return std::nullopt;
    auto field = program.at(supertype).fieldNamed(name);
    if (!field)
        return std::nullopt;
    return FieldAccess { supertype, *field };
}

FunctionID findMethod(const Program& program, LayoutID layout, StringID name, MethodKind kind)
{
    for (; layout; layout = program.at(layout).base) {
        bool isHidden = false;
        FunctionID found;
        for (auto& method : program.at(layout).methods) {
            if (method.name != name)
                continue;
            if (method.mayBeReplaced)
                return { };
            if (method.kind == kind)
                found = method.function;
            isHidden = true;
        }
        if (isHidden)
            return found;
    }
    return { };
}

static bool mayConsultPrototypeChain(const Layout& layout)
{
    return layout.mayHaveOtherPrototype;
}

static bool mayConsultPrototypeFor(const Layout& layout, std::optional<uint32_t> field)
{
    return layout.mayHaveOtherPrototype && !(field && layout.fields[*field].flags & fieldIsNeverObservedAbsentAsDeclared);
}

bool hasMethodField(const Program& program, LayoutID subtype, StringID name)
{
    if (!program.methodFieldNames.get(name.index()))
        return false;
    auto& layout = program.at(subtype);
    auto field = layout.kind == LayoutKind::Instance ? layout.fieldNamed(name) : std::nullopt;
    return field && layout.fields[*field].flags & fieldIsMethod;
}

bool mayInherit(const Program& program, LayoutID supertype, StringID name, bool isForStore)
{
    if (inheritsThroughLayoutPrototype(program, supertype, name, isForStore))
        return true;
    return isObjectLayout(program.at(supertype).kind) && !isPrivateName(program.string(name)) && std::ranges::any_of(program.at(supertype).subtypes, [&](LayoutID subtype) {
        auto& layout = program.at(subtype);
        return mayConsultPrototypeChain(layout) && mayConsultPrototypeFor(layout, layout.fieldNamed(name));
    });
}

bool storeMayConsultPrototypeChain(const Program& program, const Function& function, const Block& block, size_t index, LayoutID supertype, StringID name)
{
    return supertype && !isInitializingStore(program, function, block, index) && mayInherit(program, supertype, name, true);
}

bool mayDespecialize(const Program& program, LayoutID supertype)
{
    return program.at(supertype).mayDespecialize || std::ranges::any_of(program.at(supertype).subtypes, [&](LayoutID subtype) {
        return program.at(subtype).mayDespecialize;
    });
}

bool storeMayDespecializeObject(const Program& program, const Function& function, const Block& block, size_t index, LayoutID supertype)
{
    return supertype && !isInitializingStore(program, function, block, index) && mayDespecialize(program, supertype);
}

bool mayInheritAtCreation(const Program& program, const Function& function, ValueID object, StringID name)
{
    LayoutID created = function.at(object).layout();
    if (hasUnknownPrototypeAtCreation(program, function, object))
        return mayInherit(program, created, name);
    return inheritsThroughLayoutPrototype(program, created, name);
}

bool hasUnknownPrototypeAtCreation(const Program& program, const Function& function, ValueID object)
{
    auto& created = program.at(function.at(object).layout());
    if (!created.mayHaveOtherPrototype || !constructedBy(program, function, object))
        return false;
    return std::ranges::none_of(created.fields, [](const Field& field) {
        return field.flags & fieldIsNeverObservedAbsentAsDeclared;
    });
}

bool isOnObjectPrototype(const Program& program, StringID name)
{
    static constexpr ASCIILiteral namesOnObjectPrototype[] = {
        "constructor"_s, "hasOwnProperty"_s, "isPrototypeOf"_s, "propertyIsEnumerable"_s, "toLocaleString"_s, "toString"_s, "valueOf"_s,
        "__proto__"_s, "__defineGetter__"_s, "__defineSetter__"_s, "__lookupGetter__"_s, "__lookupSetter__"_s,
    };
    for (ASCIILiteral onObjectPrototype : namesOnObjectPrototype) {
        if (equalSpans(program.string(name), onObjectPrototype.span8()))
            return true;
    }
    return false;
}

bool inheritsThroughLayoutPrototype(const Program& program, LayoutID supertype, StringID name, bool isForStore)
{
    if (!isObjectLayout(program.at(supertype).kind) || isPrivateName(program.string(name)))
        return false;
    if (isOnObjectPrototype(program, name))
        return true;
    if (program.methodFieldNames.get(name.index())) {
        for (LayoutID subtype : program.at(supertype).subtypes) {
            if (hasMethodField(program, subtype, name) && !(isForStore && findMethod(program, subtype, name, MethodKind::Method)))
                return true;
        }
    }
    if (!program.prototypePropertyNames.get(name.index()))
        return false;
    for (LayoutID subtype : program.at(supertype).subtypes) {
        auto& layout = program.at(subtype);
        if (layout.kind == LayoutKind::Instance || (layout.kind == LayoutKind::Record && !layout.mayBeConstructed && (layout.mayHaveOtherPrototype || layout.mayDespecialize)))
            continue;
        auto field = layout.fieldNamed(name);
        if (!(field && layout.fields[*field].flags & fieldIsNeverObservedAbsentAsDeclared))
            return true;
    }
    return false;
}

static bool computePrototypeChainLacksProperty(const Program& program, LayoutID supertype, StringID name, bool forObjectWithoutExtraProperties, bool isKnownSubtype = false)
{
    if ((program.at(supertype).acceptsUntypedObjects && !isKnownSubtype) || inheritsThroughLayoutPrototype(program, supertype, name))
        return false;
    bool asks = !forObjectWithoutExtraProperties && isObjectLayout(program.at(supertype).kind) && !isPrivateName(program.string(name));
    for (LayoutID subtype : program.at(supertype).subtypes) {
        auto& layout = program.at(subtype);
        auto field = layout.fieldNamed(name);
        if (asks && mayConsultPrototypeFor(layout, field))
            return false;
        if (field)
            continue;
        if (layout.mayHaveOtherPrototype && !forObjectWithoutExtraProperties)
            return false;
        for (LayoutID ancestor = subtype; ancestor; ancestor = program.at(ancestor).base) {
            if (program.at(ancestor).hasComputedMethodName || (program.at(ancestor).hasMethodWithSymbolKey && isConstantSymbol(program.string(name))))
                return false;
            for (auto& method : program.at(ancestor).methods) {
                if (method.name == name)
                    return false;
            }
        }
    }
    return true;
}

bool prototypeChainLacksProperty(const Program& program, LayoutID supertype, StringID name, bool forObjectWithoutExtraProperties)
{
    if (program.at(supertype).subtypes.size() <= 4)
        return computePrototypeChainLacksProperty(program, supertype, name, forObjectWithoutExtraProperties);
    uint64_t key = static_cast<uint64_t>(forObjectWithoutExtraProperties) << 63 | static_cast<uint64_t>(supertype.index()) << 32 | name.index();
    auto& table = program.prototypeChainLacksPropertyCache[(supertype.index() * 31 + name.index()) % program.prototypeChainLacksPropertyCache.size()];
    {
        Locker locker { table.lock };
        if (auto found = table.answers.find(key); found != table.answers.end())
            return found->value;
    }
    bool answer = computePrototypeChainLacksProperty(program, supertype, name, forObjectWithoutExtraProperties);
    Locker locker { table.lock };
    table.answers.add(key, answer);
    return answer;
}

bool prototypeChainLacksPropertyForSubtypes(const Program& program, LayoutID supertype, StringID name)
{
    return computePrototypeChainLacksProperty(program, supertype, name, false, true);
}

void indexLayoutSupertypes(Program& program)
{
    unsigned count = program.layouts.size();
    program.firstSupertypeOfLayout.fill(0, count + 1);
    for (auto& supertype : program.layouts) {
        for (LayoutID subtype : supertype.subtypes)
            ++program.firstSupertypeOfLayout[subtype.index() + 1];
    }
    for (unsigned i = 0; i < count; ++i)
        program.firstSupertypeOfLayout[i + 1] += program.firstSupertypeOfLayout[i];
    program.supertypesOfLayouts.fill(LayoutID(), program.firstSupertypeOfLayout[count]);
    Vector<uint32_t> next = program.firstSupertypeOfLayout;
    for (unsigned i = 0; i < count; ++i) {
        for (LayoutID subtype : program.layouts[i].subtypes)
            program.supertypesOfLayouts[next[subtype.index()]++] = LayoutID(i);
    }
    auto removeDuplicates = [](Vector<uint64_t>& stores) {
        stores.shrink(std::ranges::unique(stores).begin() - stores.begin());
    };
    static constexpr unsigned functionsPerChunk = 512;
    unsigned numberOfFunctions = program.functions.size();
    Vector<Vector<uint64_t>> storesByChunk((numberOfFunctions + functionsPerChunk - 1) / functionsPerChunk);
    forEachIndexInParallel(storesByChunk.size(), [&](unsigned chunk) {
        auto& stores = storesByChunk[chunk];
        for (unsigned i = chunk * functionsPerChunk; i < std::min(numberOfFunctions, (chunk + 1) * functionsPerChunk); ++i) {
            auto& function = std::as_const(program).functions[i];
            auto recordStoreThroughTypeOf = [&](ValueID receiver, StringID name) {
                if (!receiver || !function.at(receiver).type)
                    return;
                for (LayoutID supertype : program.at(function.at(receiver).type).objectLayouts)
                    stores.append(static_cast<uint64_t>(supertype.index()) << 32 | name.index());
            };
            static constexpr OpcodeSet propertyStores { Opcode::PutById, Opcode::PutField };
            std::as_const(program).functions.forEachValue(i, propertyStores, [&](ValueID id) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutById)
                    recordStoreThroughTypeOf(value.child(0), value.string());
                else if (isObjectLayout(program.at(value.layout()).kind)) {
                    StringID name = program.at(value.layout()).fields[value.field()].name;
                    stores.append(static_cast<uint64_t>(value.layout().index()) << 32 | name.index());
                    recordStoreThroughTypeOf(value.child(0), name);
                }
            });
        }
        std::ranges::sort(stores);
        removeDuplicates(stores);
    });
    Vector<uint64_t> allStores;
    for (auto& stores : storesByChunk)
        allStores.appendVector(stores);
    sortOnPool(allStores.mutableSpan(), std::less { });
    removeDuplicates(allStores);
    program.fieldsStoredThroughLayouts.reserveInitialCapacity(allStores.size());
    for (uint64_t store : allStores)
        program.fieldsStoredThroughLayouts.add(store);
}

bool subtypesStoreOnlyDeclaredType(const Program& program, LayoutID supertype, uint32_t field)
{
    auto& declared = program.at(supertype).fields[field];
    if (!declared.type || program.firstSupertypeOfLayout.isEmpty())
        return false;
    auto declaresNoOtherFields = [&](const Layout& layout) {
        auto found = layout.fieldNamed(declared.name);
        if (!found)
            return true;
        TypeID type = layout.fields[*found].type;
        return type == declared.type || (type && program.at(type).isSubsetOf(program.at(declared.type), program));
    };
    for (LayoutID subtype : program.at(supertype).subtypes) {
        if (!program.at(subtype).fieldNamed(declared.name) || !declaresNoOtherFields(program.at(subtype)))
            return false;
        for (uint32_t i = program.firstSupertypeOfLayout[subtype.index()]; i < program.firstSupertypeOfLayout[subtype.index() + 1]; ++i) {
            LayoutID other = program.supertypesOfLayouts[i];
            if (program.fieldsStoredThroughLayouts.contains(static_cast<uint64_t>(other.index()) << 32 | declared.name.index()) && !declaresNoOtherFields(program.at(other)))
                return false;
        }
    }
    return true;
}

bool isOnlyInExtraProperties(const Program& program, LayoutID supertype, StringID name)
{
    if (!prototypeChainLacksProperty(program, supertype, name))
        return false;
    for (LayoutID subtype : program.at(supertype).subtypes) {
        if (program.at(subtype).fieldNamed(name))
            return false;
        for (LayoutID specialization : program.at(subtype).specializations) {
            if (program.at(specialization).fieldNamed(name))
                return false;
        }
    }
    return true;
}

static bool computeStoreMayTransition(const Program& program, LayoutID supertype, StringID name)
{
    for (LayoutID subtype : program.at(supertype).subtypes) {
        auto& layout = program.at(subtype);
        if (layout.fieldNamed(name))
            continue;
        for (LayoutID specialization : layout.specializations) {
            if (program.at(specialization).fieldNamed(name))
                return true;
        }
    }
    return false;
}

bool storeMayTransition(const Program& program, LayoutID supertype, StringID name)
{
    if (program.at(supertype).subtypes.size() <= 4)
        return computeStoreMayTransition(program, supertype, name);
    uint64_t key = static_cast<uint64_t>(supertype.index()) << 32 | name.index();
    auto& table = program.storeMayTransitionCache[(supertype.index() * 31 + name.index()) % program.storeMayTransitionCache.size()];
    {
        Locker locker { table.lock };
        if (auto found = table.answers.find(key); found != table.answers.end())
            return found->value;
    }
    bool answer = computeStoreMayTransition(program, supertype, name);
    Locker locker { table.lock };
    table.answers.add(key, answer);
    return answer;
}

LValue Lowering::marked(LValue load, unsigned flags)
{
    auto* memory = load->as<MemoryValue>();
    if (flags & IsSafe)
        memory->setControlDependent(false);
    if (flags & IsFixed)
        memory->setReadsMutability(Mutability::Immutable);
    return load;
}

uint32_t Lowering::minObjectSizeOf(LayoutID supertype) const
{
    return m_minObjectSizes.ensure(supertype.index(), [&] {
        auto& layout = m_program.at(supertype);
        if (layout.subtypes.isEmpty())
            return layout.size;
        uint32_t minimum = UINT32_MAX;
        for (LayoutID subtype : layout.subtypes)
            minimum = std::min(minimum, m_program.at(subtype).size);
        return minimum;
    }).iterator->value;
}

unsigned Lowering::safeIfReaches(CellPointer cell, unsigned end) const
{
    auto found = m_cellTypes.find(cell.value());
    if (found == m_cellTypes.end())
        return 0;
    if (end <= MarkedBlock::atomSize)
        return IsSafe;
    auto& type = m_program.at(found->value);
    if (type.primitives || type.anyObject || type.anyFunction)
        return 0;
    for (uint16_t engineClass : type.engineClasses) {
        if (m_builtins.classes()[engineClass].typedArrayType == NotTypedArray || end > sizeof(JSArrayBufferView))
            return 0;
    }
    for (LayoutID supertype : type.objectLayouts) {
        if (m_program.at(supertype).acceptsUntypedObjects || m_program.at(supertype).isTypedOnlySupertype || minObjectSizeOf(supertype) < end)
            return 0;
    }
    return IsSafe;
}

unsigned Lowering::safeIfAllSubtypesHave(CellPointer cell, const Field& field) const
{
    return field.isMissingInSomeSubtype || field.isOutOfLine ? 0 : safeIfReaches(cell, field.offset + sizeOf(field.store));
}

Operand Lowering::load(Store store, TypedPointer pointer, unsigned flags, LValue holder)
{
    switch (store) {
    case Store::Uint8:
    case Store::Boolean:
        return Word32(marked(m_out.load8ZeroExt32(pointer), flags));
    case Store::Int8:
        return Word32(marked(m_out.load8SignExt32(pointer), flags));
    case Store::Uint16:
        return Word32(marked(m_out.load16ZeroExt32(pointer), flags));
    case Store::Int16:
        return Word32(marked(m_out.load16SignExt32(pointer), flags));
    case Store::Int32:
    case Store::Int32OrNumberBox:
        return Word32(marked(m_out.load32(pointer), flags));
    case Store::Uint32:
        return zeroExtended(Word32(marked(m_out.load32(pointer), flags)));
    case Store::Int64:
    case Store::OptionalInt64:
    case Store::Cell:
    case Store::JSValue:
    case Store::LiteralString:
        return Operand(repOf(store), marked(m_out.load64(pointer), flags));
    case Store::LiteralString32: {
        LValue word = m_out.zeroExtPtr(marked(m_out.load32(pointer), flags));
        RELEASE_ASSERT(holder);
        ensureStillAliveHereForWord(CellPointer(holder), word);
        LValue cell = m_out.opaque(decodeCell32(word));
        LValue value = cell32IsPointer ? cell : m_out.select(m_out.below(word, m_out.constInt64(minCell32)), word, cell);
        m_loadedLiteralStrings.add(value, word);
        return LiteralStringWord(value);
    }
    case Store::Cell32:
    case Store::OptionalCell32: {
        LValue index = m_out.zeroExtPtr(marked(m_out.load32(pointer), flags));
        RELEASE_ASSERT(holder);
        ensureStillAliveHereForWord(CellPointer(holder), index);
        LValue cell = decodeCell32(index);
        if (!isDroppedForTesting()) [[likely]]
            cell = m_out.opaque(cell);
        m_cellIndices.add(cell, index);
        if (store == Store::Cell32)
            return CellPointer(cell);
        return boxDecoded(index, cell);
    }
    case Store::OptionalBoolean:
        return Boxed(m_out.zeroExtPtr(marked(m_out.load8ZeroExt32(pointer), flags)));
    case Store::Float:
        return Number(m_out.floatToDouble(marked(m_out.loadFloat(pointer), flags)));
    case Store::Double:
    case Store::OptionalDouble:
        return Operand(repOf(store), marked(m_out.loadDouble(pointer), flags));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed Lowering::boxDecoded(LValue index, LValue cell, LValue loaded)
{
    LValue word = loaded ? loaded : index;
    LValue boxed = !cell32IsPointer ? m_out.select(m_out.below(word, m_out.constInt64(firstCellWord)), index, cell)
        : loaded ? m_out.select(m_out.isZero64(word), m_out.constInt64(undefinedCell32), cell) : m_out.opaque(cell);
    m_decoded.add(boxed, Decoded { index, cell, word, !!loaded });
    return Boxed(boxed);
}

Lowering::Parts Lowering::partsOf(Boxed value)
{
    if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
        return { found->value.loaded, found->value.cell, found->value.absentReadsUndefined };
    return { value.value(), value.value(), false };
}

Boxed Lowering::fromParts(LValue word, LValue cell, bool zeroReadsUndefined)
{
    if (!zeroReadsUndefined)
        return boxDecoded(word, cell);
    return boxDecoded(m_out.select(m_out.isZero64(word), m_out.constInt64(undefinedCell32), word), cell, word);
}

Lowering::JoinRep Lowering::joinRepFor(const Type& type) const
{
    Rep rep = repOf(type);
    return { rep, rep == Rep::JSValue && !(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull)) && !Options::auditAOTTypedAssertions() };
}

Operand Lowering::OperandJoin::finish()
{
    Output& out = m_lowering.m_out;
    bool isInParts = false;
    if (m_joined.isReference) {
        for (auto& arm : m_incomingValues)
            isInParts |= m_lowering.m_decoded.contains(arm.operand.value);
    }
    bool keepsInt32 = false;
    bool keepsExactInteger = false;
    if (m_joined.rep == Rep::Double) {
        for (auto& arm : m_incomingValues) {
            keepsInt32 |= m_lowering.m_numberToInt32.contains(arm.operand.value);
            keepsExactInteger |= m_lowering.m_numberToExactInteger.contains(arm.operand.value);
        }
    }
    bool keepsWord = false;
    if (m_joined.rep == Rep::Double || m_joined.rep == Rep::OptionalDouble) {
        for (auto& arm : m_incomingValues)
            keepsWord |= m_lowering.m_numberToWord.contains(arm.operand.value);
    }
    Vector<ValueFromBlock, 4> values;
    Vector<ValueFromBlock, 4> cells;
    Vector<ValueFromBlock, 4> int32s;
    Vector<ValueFromBlock, 4> exactIntegers;
    Vector<ValueFromBlock, 4> words;
    bool zeroReadsUndefined = false;
    for (auto& arm : m_incomingValues) {
        out.appendTo(arm.block);
        if (keepsWord) {
            auto found = m_lowering.m_numberToWord.find(arm.operand.value);
            words.append(out.anchor(found != m_lowering.m_numberToWord.end() ? found->value : out.constInt64(INT32_MIN)));
        }
        if (keepsInt32) {
            auto found = m_lowering.m_numberToInt32.find(arm.operand.value);
            LValue int32 = found != m_lowering.m_numberToInt32.end() ? found->value : m_lowering.toInt32OnPath(arm.operand.as<Number>(), false).value();
            int32s.append(out.anchor(int32));
        }
        if (keepsExactInteger) {
            auto found = m_lowering.m_numberToExactInteger.find(arm.operand.value);
            LValue exactInteger = found != m_lowering.m_numberToExactInteger.end() ? found->value : m_lowering.exactIntegerOnPath(arm.operand.as<Number>()).value();
            exactIntegers.append(out.anchor(exactInteger));
        }
        if (isInParts) {
            Parts parts = m_lowering.partsOf(arm.operand.as<Boxed>());
            values.append(out.anchor(parts.word));
            cells.append(out.anchor(parts.cell));
            zeroReadsUndefined |= parts.zeroReadsUndefined;
        } else if (m_joined.rep != Rep::Void)
            values.append(out.anchor(arm.operand.value));
        out.jump(m_continuation);
    }
    out.appendTo(m_continuation);
    if (m_joined.rep == Rep::Void)
        return { };
    LValue value = out.phi(b3TypeOf(m_joined.rep), values);
    if (keepsInt32)
        m_lowering.m_numberToInt32.add(value, out.phi(Int32, int32s));
    if (keepsExactInteger)
        m_lowering.m_numberToExactInteger.add(value, out.phi(Int64, exactIntegers));
    if (keepsWord)
        m_lowering.m_numberToWord.add(value, out.phi(Int64, words));
    if (!isInParts)
        return Operand(m_joined.rep, value);
    return m_lowering.fromParts(value, out.phi(Int64, cells), zeroReadsUndefined);
}

Boxed Lowering::rematerializedValue(Boxed value)
{
    auto found = m_decoded.find(value.value());
    if (found == m_decoded.end())
        return value;
    Decoded decoded = found->value;
    LValue loaded = m_out.opaque(decoded.loaded);
    LValue index = decoded.absentReadsUndefined ? m_out.select(m_out.isZero64(loaded), m_out.constInt64(undefinedCell32), loaded) : loaded;
    if (cell32IsPointer)
        return Boxed(decoded.absentReadsUndefined ? m_out.select(m_out.isZero64(loaded), m_out.constInt64(undefinedCell32), m_out.opaque(decoded.cell)) : m_out.opaque(decoded.cell));
    return Boxed(m_out.select(m_out.belowOrEqual(loaded, m_out.constInt64(undefinedCell32)), index, m_out.opaque(decoded.cell)));
}

void Lowering::validateCell32(LValue index, LValue stored, bool mayBeNullish)
{
    LValue isNil = mayBeNullish ? m_out.belowOrEqual(stored, m_out.constInt64(undefinedCell32)) : m_out.isZero64(stored);
    LValue isExact = m_out.bitOr(isNil, m_out.equal(decodeCell32(m_out.zeroExtPtr(index)), stored));
    LBasicBlock isInexact = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isExact, usually(continuation), rarely(isInexact));
    m_out.appendTo(isInexact);
    AOT_TYPED_OPERATION(FactViolated, Boxed(stored), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::store(Store store, Operand operand, TypedPointer pointer)
{
    RELEASE_ASSERT(operand.rep == repOf(store));
    switch (store) {
    case Store::Uint8:
    case Store::Int8:
    case Store::Boolean:
        m_out.store32As8(operand.value, pointer);
        return;
    case Store::Uint16:
    case Store::Int16:
        m_out.store32As16(operand.value, pointer);
        return;
    case Store::Int32:
    case Store::Int32OrNumberBox:
        m_out.store32(operand.value, pointer);
        return;
    case Store::Uint32:
        m_out.store32(m_out.castToInt32(operand.value), pointer);
        return;
    case Store::Int64:
    case Store::OptionalInt64:
    case Store::Cell:
    case Store::JSValue:
    case Store::LiteralString:
        m_out.store64(operand.value, pointer);
        return;
    case Store::LiteralString32: {
        LValue word = rawWordOf(operand.as<LiteralStringWord>());
        if (word == operand.value && !cell32IsPointer && !isNeverStringCell(operand.as<LiteralStringWord>()))
            word = m_out.select(m_out.below(word, m_out.constInt64(minCell32)), word, m_out.lShr(word, m_out.constInt32(cell32Shift)));
        m_out.store32(m_out.castToInt32(word), pointer);
        if (!operand.value->hasInt64())
            ensureStillAliveHere(CellPointer(word != operand.value && operand.value->opcode() == B3::Select ? operand.value->child(2) : operand.value));
        return;
    }
    case Store::Cell32:
    case Store::OptionalCell32: {
        bool mayBeNullish = store == Store::OptionalCell32 && !m_cellTypes.contains(operand.value);
        LValue index = nullptr;
        LValue kept = operand.value;
        if (auto decoded = m_decoded.find(operand.value); decoded != m_decoded.end() && m_cellIndices.contains(decoded->value.cell)) {
            index = decoded->value.index;
            kept = decoded->value.cell;
        } else if (auto found = m_cellIndices.find(operand.value); found != m_cellIndices.end())
            index = found->value;
        bool isSlotWord = index;
        if (!index) {
            index = cell32IsPointer ? operand.value : m_out.lShr(operand.value, m_out.constInt32(cell32Shift));
            if (mayBeNullish && !cell32IsPointer)
                index = m_out.select(m_out.belowOrEqual(operand.value, m_out.constInt64(undefinedCell32)), operand.value, index);
        }
        index = m_out.castToInt32(index);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateCell32(index, operand.value, mayBeNullish);
        m_out.store32(index, pointer);
        if (!kept->hasInt64() && isSlotWord)
            ensureStillAliveHereForWord(CellPointer(kept), index);
        else if (!kept->hasInt64())
            ensureStillAliveHere(CellPointer(kept));
        return;
    }
    case Store::OptionalBoolean:
        m_out.store32As8(m_out.castToInt32(operand.value), pointer);
        return;
    case Store::Float:
        m_out.storeFloat(m_out.doubleToFloat(operand.value), pointer);
        return;
    case Store::Double:
    case Store::OptionalDouble:
        m_out.storeDouble(operand.value, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canSeparateStoreFromBarrier(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Nop:
    case B3::Identity:
    case B3::Opaque:
    case B3::Const32:
    case B3::Const64:
    case B3::ConstDouble:
    case B3::ConstFloat:
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::Neg:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
    case B3::BitwiseCast:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::ZExt32:
    case B3::Trunc:
    case B3::IToD:
    case B3::FloatToDouble:
    case B3::DoubleToFloat:
    case B3::PurifyNaN:
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::Select:
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
    case B3::Upsilon:
    case B3::Phi:
    case B3::Jump:
    case B3::Branch:
        return true;
    default:
        return false;
    }
}

bool Lowering::allPathsLeadHere(LBasicBlock block, size_t index, Vector<LBasicBlock, 8>& inProgress, Vector<LBasicBlock, 8>& walked) const
{
    if (inProgress.contains(block) || inProgress.size() + walked.size() >= 8)
        return false;
    if (walked.contains(block))
        return true;
    for (; index < block->size(); ++index) {
        LValue value = block->at(index);
        bool emitsNothing = value->opcode() == B3::Patchpoint && value->as<PatchpointValue>()->generator() == m_emptyGenerator;
        if (!emitsNothing && !canSeparateStoreFromBarrier(value->opcode()))
            return false;
    }
    if (block == m_out.m_block)
        return true;
    if (!block->numSuccessors())
        return false;
    inProgress.append(block);
    for (LBasicBlock successor : block->successorBlocks()) {
        if (!allPathsLeadHere(successor, 0, inProgress, walked))
            return false;
    }
    inProgress.removeLast();
    walked.append(block);
    return true;
}

void Lowering::deferLastBarrierTo(CellPointer owner)
{
    EmittedBarrier last = std::exchange(m_lastBarrier, EmittedBarrier { });
    if (last.owner != owner.value())
        return;
    Vector<LBasicBlock, 8> inProgress;
    Vector<LBasicBlock, 8> walked;
    if (!allPathsLeadHere(last.next, last.indexInNext, inProgress, walked))
        return;
    if (last.branchBlock)
        last.test->replaceWithJump(last.branchBlock, FrequentedBlock(last.next));
    else
        last.test->replaceWithNop();
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: f", m_id.index(), ": a write barrier is deferred to the next one");
}

bool Lowering::mayBeCell(ValueID id)
{
    auto& type = typeOf(id);
    if (!type.hasCells())
        return false;
    return m_function.at(id).rep != Rep::LiteralString || !isNeverStringCell(get(id).as<LiteralStringWord>());
}

void Lowering::emitBarrierForLiteralString(CellPointer owner, ValueID stored)
{
    if (m_function.at(stored).rep != Rep::LiteralString) {
        barrier(owner);
        return;
    }
    LBasicBlock isCell = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isStringCell(get(stored).as<LiteralStringWord>()).value(), rarely(isCell), usually(continuation));
    m_out.appendTo(isCell);
    barrier(owner);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::barrier(CellPointer owner)
{
    deferLastBarrierTo(owner);
    if (!m_isInLoop) {
#if !CPU(X86_64)
        LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
        LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
#endif
        PatchpointValue* patchpoint = m_out.patchpoint(Void);
        m_lastBarrier = EmittedBarrier { owner.value(), patchpoint, nullptr, m_out.m_block, m_out.m_block->size() };
        applyEffects(patchpoint, effectsForOperation<operationAOTTypedBarrierSlow>());
        patchpoint->append(ConstrainedValue(owner.value(), ValueRep::SomeRegister));
#if !CPU(X86_64)
        patchpoint->append(ConstrainedValue(state, ValueRep::SomeRegister));
        patchpoint->append(ConstrainedValue(threshold, ValueRep::SomeRegister));
#endif
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        if (LoweringContext::coldSitesForceFrame()) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else if (!hasFramelessFunctionStubs)
            m_context.hasColdSites = true;
        uint32_t immediate = static_cast<uint32_t>(static_cast<unsigned>(isARM64() ? Entry::operationAOTTypedBarrierSlowPreservingRegisters : Entry::operationAOTTypedBarrierSlow) * sizeof(void*));
        patchpoint->setGenerator([context = &m_context, immediate](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg owner = params[0].gpr();
#if CPU(X86_64)
            jit.load8(CCallHelpers::Address(owner, JSCell::cellStateOffset()), jit.scratchRegister());
            CCallHelpers::Jump isSlow = jit.branch32(CCallHelpers::BelowOrEqual, jit.scratchRegister(), CCallHelpers::Address(instanceGPR, Instance::offsetOfBarrierThreshold()));
#else
            CCallHelpers::Jump isSlow = jit.branch32(CCallHelpers::BelowOrEqual, params[1].gpr(), params[2].gpr());
#endif
            CCallHelpers::Label done = jit.label();
            Vector<GPRReg, 4> saved;
            for (GPRReg reg : { GPRInfo::returnValueGPR, operationArgumentGPR(1), stubTemporaryGPRs[0], stubTemporaryGPRs[1] }) {
                if (isARM64())
                    break;
                bool isTemporary = reg == stubTemporaryGPRs[0] || reg == stubTemporaryGPRs[1];
                if (isTemporary && !registersClobberedBy(Stub::ColdPlainOperation)->contains(reg, IgnoreVectors))
                    continue;
                if (params.usedRegisters().contains(reg, IgnoreVectors) && !(reg == operationArgumentGPR(1) && reg == owner))
                    saved.append(reg);
            }
            if (saved.size() & 1)
                saved.append(jit.scratchRegister());
            bool isLeaf = hasFramelessFunctionStubs && context->hasNoFrame(params.proc().code());
#if CPU(X86_64)
            bool createsFrame = context->hasNoFrame(params.proc().code());
#endif
            bool holdsFPR = false;
            RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
                holdsFPR |= reg.isFPR();
            });
            params.addNearbyLatePath(jit, [=](CCallHelpers& jit) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                isSlow.link(&jit);
#if CPU(X86_64)
                if (createsFrame)
                    jit.emitFunctionPrologue();
#endif
                for (unsigned i = 0; i < saved.size(); i += 2)
                    jit.pushPair(saved[i], saved[i + 1]);
#if CPU(ARM64)
                if (isLeaf)
                    jit.move(CCallHelpers::linkRegister, ARM64Registers::x17);
#endif
                unsigned stub = static_cast<unsigned>(Stub::ColdPlainOperation);
                if (!holdsFPR)
                    stub += 2;
                if (isLeaf)
                    ++stub;
                ArgumentMoves ownerToArgument;
#if CPU(ARM64)
                appendArgumentMove(ownerToArgument, ARM64Registers::x16, ArgumentSource::Register, operationArgumentGPR(1));
#endif
                if (owner != operationArgumentGPR(1))
                    appendArgumentMove(ownerToArgument, operationArgumentGPR(1), ArgumentSource::Register, owner);
                if (ownerToArgument.isEmpty())
                    context->stubCalls.call(jit, static_cast<Stub>(stub), immediate, CallSite { });
                else
                    context->stubCalls.call(jit, static_cast<Stub>(stub), immediate, CallSite { }, ownerToArgument);
                for (unsigned i = saved.size(); i; i -= 2)
                    jit.popPair(saved[i - 2], saved[i - 1]);
#if CPU(X86_64)
                if (createsFrame)
                    jit.emitFunctionEpilogueWithEmptyFrame();
#endif
                jit.jump().linkTo(done, &jit);
            });
        });
        return;
    }
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
    LBasicBlock branchBlock = m_out.m_block;
    m_out.branch(m_out.above(state, threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(BarrierSlow, owner);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    m_lastBarrier = EmittedBarrier { owner.value(), branchBlock->last(), branchBlock, continuation, 0 };
}

Word32 Lowering::isBeyondBarrierThreshold(CellPointer owner)
{
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
    return Word32(m_out.above(state, threshold));
}

void Lowering::barrier(CellPointer array, std::span<const Boxed> stored)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, array.value(), JSCell::cellStateOffset()));
    m_out.branch(m_out.above(state, threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    for (Boxed value : stored)
        callAllocationStub(Stub::RememberElement, { { array.value(), stubTemporaryGPRs[0] }, { value.value(), stubTemporaryGPRs[2] } }, effectsForOperation<operationAOTTypedRememberElement>(), false);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::fenceAfterInitialization()
{
#if CPU(X86_64)
    m_out.fence(&m_heaps.root, nullptr);
#else
    LBasicBlock fence = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfMutatorShouldBeFenced())), rarely(fence), usually(continuation));
    m_out.appendTo(fence);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
#endif
}

void Lowering::didLowerInitializingStore()
{
    if (!m_fieldsAwaitingFirstStore.isEmpty()) [[unlikely]] {
        if (auto field = fieldInitializedBy(m_fieldsAwaitingFirstStoreLayout, m_function.at(m_currentBlock->values[m_currentIndexInBlock])))
            m_fieldsAwaitingFirstStore.clear(*field);
    }
    size_t next = m_currentIndexInBlock + 1;
    auto& store = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if (store.opcode == Opcode::PutField && m_function.stackAllocatedEnvironments.get(originOf(store.child(0)).index()))
        return;
    if (next < m_currentBlock->values.size() && isInitializingStore(m_program, m_function, *m_currentBlock, next))
        return;
    if (store.opcode == Opcode::PutField && m_program.at(store.layout()).isCoroutineFrame && next < m_currentBlock->values.size()) {
        auto& following = m_function.at(m_currentBlock->values[next]);
        if (following.opcode == Opcode::NewClosure && m_program.isResumeFunction(m_program.at(following.function())) && following.arguments[0] == store.child(0)) {
            PatchpointValue* line = m_out.patchpoint(Void);
            line->effects = Effects::none();
            line->effects.fence = true;
            line->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
            return;
        }
    }
    if (store.opcode == Opcode::PutField && m_program.at(store.layout()).reuseVariable) {
        m_isFillingReusedEnvironment = false;
        barrier(in<CellPointer>(store.child(0)));
    }
    fenceAfterInitialization();
}

const Field* Lowering::fieldFor(const Type& receiver, StringID name) const
{
    auto access = fieldOf(m_program, receiver, name);
    return access ? &m_program.at(access->layout).fields[access->field] : nullptr;
}

bool Lowering::mayDespecialize(LayoutID supertype) const
{
    return m_mayDespecializeCache.ensure(supertype.index(), [&] {
        return Typed::mayDespecialize(m_program, supertype);
    }).iterator->value;
}

bool Lowering::mayBeFrozen(LayoutID supertype) const
{
    return m_mayBeFrozenCache.ensure(supertype.index(), [&] {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (m_program.at(subtype).mayBeFrozen)
                return true;
        }
        return false;
    }).iterator->value;
}

Word32 Lowering::extensionCovers(CellPointer cell, const Field& field)
{
    Word32 word = extensionWordOf(cell);
    if (field.offset + sizeOf(field.store) <= minExtensionSize)
        return logicalNot(isZero(word));
    LBasicBlock hasExtension = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> answers;
    answers.append(m_out.anchor(m_out.int32Zero));
    m_out.branch(isZero(word).value(), unsure(continuation), unsure(hasExtension));
    m_out.appendTo(hasExtension);
    Word32 size = loadField<Word32>(extensionAt(word), m_heaps.AOTTypedExtension_size, IsFixed);
    answers.append(m_out.anchor(aboveOrEqual(size, word32(field.offset + sizeOf(field.store))).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, answers));
}

Operand Lowering::loadSlot(CellPointer cell, const Field& field, unsigned flags)
{
    if (!field.isOutOfLine)
        return load(field.store, fieldAt(cell, field), flags, cell.value());
    RELEASE_ASSERT(representsAbsentAsZero(field.store) && (repOf(field.store) == Rep::JSValue || field.store == Store::LiteralString32));
    Word32 word = extensionWordOf(cell);
    LBasicBlock hasExtension = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> words;
    words.append(m_out.anchor(m_out.int64Zero));
    m_out.branch(isZero(word).value(), unsure(continuation), unsure(hasExtension));
    m_out.appendTo(hasExtension);
    Pointer extension = extensionAt(word);
    if (field.offset + sizeOf(field.store) > minExtensionSize) {
        LBasicBlock within = m_out.newBlock();
        Word32 size = loadField<Word32>(extension, m_heaps.AOTTypedExtension_size, IsFixed);
        words.append(m_out.anchor(m_out.int64Zero));
        m_out.branch(aboveOrEqual(size, word32(field.offset + sizeOf(field.store))).value(), unsure(within), unsure(continuation));
        m_out.appendTo(within);
    }
    TypedPointer slot = m_out.address(heapOf(field.slot), extension.value(), field.offset);
    words.append(m_out.anchor(sizeOf(field.store) == 8 ? m_out.load64(slot) : m_out.zeroExtPtr(sizeOf(field.store) == 4 ? m_out.load32(slot) : m_out.load8ZeroExt32(slot))));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    LValue raw = m_out.phi(Int64, words);
    ensureStillAliveHere(cell);
    if (field.store == Store::LiteralString32) {
        LValue string = m_out.opaque(decodeCell32(raw));
        LValue value = cell32IsPointer ? string : m_out.select(m_out.below(raw, m_out.constInt64(minCell32)), raw, string);
        m_loadedLiteralStrings.add(value, raw);
        return LiteralStringWord(value);
    }
    if (field.store != Store::OptionalCell32)
        return Boxed(raw);
    LValue decoded = decodeCell32(raw);
    if (!isDroppedForTesting()) [[likely]]
        decoded = m_out.opaque(decoded);
    m_cellIndices.add(decoded, raw);
    return boxDecoded(raw, decoded);
}

Operand Lowering::readField(CellPointer cell, const Field& field, const Type& type)
{
    if (field.store == Store::Int32OrNumberBox)
        return readInt32OrNumberBox(cell, field, type);
    Operand raw = loadSlot(cell, field, safeIfAllSubtypesHave(cell, field));
    if (field.store == Store::Uint8 || field.store == Store::Uint16)
        raw = zeroExtended(raw.as<Word32>());
    auto& stored = m_program.at(field.stored);
    if (field.flags & fieldHoldsLazyClosure && !m_readsRawField) {
        ValueID read = m_currentIndexInBlock < m_currentBlock->values.size() ? m_currentBlock->values[m_currentIndexInBlock] : ValueID();
        if (read && (m_function.at(read).opcode != Opcode::GetById || m_function.at(read).string() != field.name))
            read = ValueID();
        bool isOnlyCalled = read && isCalledWithoutAllocation(read);
        LBasicBlock isStored = m_out.newBlock();
        LBasicBlock isNotCreated = isOnlyCalled ? m_out.newBlock() : newColdBlock();
        OperandJoin join(*this, joinRepFor(type));
        if (isOnlyCalled)
            m_out.branch(isUnmaterializedClosure(raw.as<Boxed>()).value(), unsure(isNotCreated), unsure(isStored));
        else
            m_out.branch(isUnmaterializedClosure(raw.as<Boxed>()).value(), rarely(isNotCreated), usually(isStored));
        m_out.appendTo(isNotCreated);
        if (isOnlyCalled)
            join.add(Boxed(m_out.constInt64(unmaterializedClosureCell32)));
        else
            join.add(ensure(Operand(materializeLazyClosure(cell, field)), m_program.at(m_program.types.function), type));
        m_out.appendTo(isStored);
        {
            SetForScope readsRawField(m_readsRawField, true);
            join.add(readField(cell, field, type));
        }
        return join.finish();
    }
    if (!(stored.primitives & Primitive::Absent))
        return ensure(raw, stored, type);
    if (std::exchange(m_fieldIsPresent, false) && raw.rep == Rep::JSValue)
        return ensure(raw, stored.withoutPrimitives(Primitive::Absent), type);

    Type held = stored;
    held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
    if (auto found = m_decoded.find(raw.value); found != m_decoded.end()) {
        Decoded decoded = found->value;
        return ensure(boxDecoded(m_out.select(m_out.isZero64(decoded.index), m_out.constInt64(undefinedCell32), decoded.index), decoded.cell, decoded.index), held, type);
    }
    if (raw.rep == Rep::JSValue || type.primitives & Primitive::Undefined || (m_fusedCasts && &type == &m_fusedCasts->type)) {
        Integer bits = bitsOf(raw);
        LValue beforeNormalization = raw.value;
        raw = fromBits(select(isAbsent(raw), integer(markerBits(Primitive::Undefined, raw.rep)), bits), raw.rep);
        if ((raw.rep == Rep::OptionalDouble || raw.rep == Rep::OptionalInt64 || raw.rep == Rep::JSValue) && !indexOf(beforeNormalization))
            m_beforeAbsentNormalization.add(raw.value, beforeNormalization);
        static_assert(JSValue::ValueUndefined == undefinedCell32 && JSValue::ValueNull == nullCell32);
        if (raw.rep == Rep::JSValue && !(held.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull)))
            m_decoded.add(raw.value, Decoded { raw.value, bits.value(), bits.value(), true });
    }
    return ensure(raw, held, type);
}

Operand Lowering::getFromObjectWithExtraProperties(CellPointer cell, StringID name, const Type& type)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions() || isPrivateName(m_program.string(name))) [[unlikely]]
        return ensure(emitGetById(box(cell), name), type);
    TypeID readType = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    if (!m_fusedCasts && &m_program.at(readType) == &type && holdsCells(repOf(type)) && !type.isNever() && !m_useCastTest && !m_rejectsUntypedObjects && !handlerEnteredByBranch())
        return convert(AOT_TYPED_OPERATION(GetByNameChecked, cell, word32(name.index()), word32(readType.index())), type, repOf(type));
    if (!m_fusedCasts && &m_program.at(readType) == &type && (repOf(type) == Rep::OptionalDouble || repOf(type) == Rep::Double) && !m_useCastTest && !handlerEnteredByBranch())
        return convert(Operand(Rep::OptionalDouble, AOT_TYPED_OPERATION(GetByNameCheckedAsNumber, cell, word32(name.index()), word32(readType.index())).value()), type, repOf(type));
    return ensure(AOT_TYPED_OPERATION(GetByName, box(cell), word32(name.index())), type);
}

Word32 Lowering::hasPrototypeOfLayout(CellPointer cell)
{
    LValue structure = structureWithID(m_out.load32(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(JSCell::structureIDOffset())))));
    LValue flags = m_out.load8ZeroExt32(TypedPointer(m_heaps.root, m_out.add(structure, m_out.constIntPtr(JSCell::indexingTypeAndMiscOffset()))));
    return Word32(m_out.testNonZero32(flags, m_out.constInt32(Structure::hasPrototypeOfLayout)));
}

void Lowering::branchIfOwnFieldRequested(CellPointer cell, LBasicBlock isNotRequested, LBasicBlock asks)
{
    LBasicBlock isMarked = newColdBlock();
    m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(isNotRequested), rarely(isMarked));
    m_out.appendTo(isMarked);
    m_out.branch(hasPrototypeOfLayout(cell).value(), unsure(isNotRequested), unsure(asks));
}

static bool areSingleEvaluation(const Program&, const Vector<LayoutID>&);

Operand Lowering::readProperty(CellPointer cell, LayoutID supertype, const Field& field, const Type& type)
{
    if (!(m_program.at(field.stored).primitives & Primitive::Absent) || !mayInherit(m_program, supertype, field.name))
        return readField(cell, field, type);
    auto read = [&] {
        SetForScope fieldIsPresent(m_fieldIsPresent, true);
        return readField(cell, field, type);
    };
    auto ask = [&] {
        auto targets = m_program.methodFieldNames.get(field.name.index()) ? methodTargets(supertype, field.name) : std::nullopt;
        if (targets && targets->size() == 1 && targets->first().fieldCheckedFirst && m_program.genericEntrypoints.contains(targets->first().function.index()) && areSingleEvaluation(m_program, m_program.at(supertype).subtypes))
            return ensure(closureConstant(targets->first().function), m_program.at(m_program.types.function), type);
        return getByIdAndCheckType(box(cell), field.name, type);
    };
    if (inheritsThroughLayoutPrototype(m_program, supertype, field.name))
        return eitherOperand(hasField(cell, field), joinRepFor(type), read, ask);
    LBasicBlock isPresent = m_out.newBlock();
    LBasicBlock isAbsent = newUnlikelyBlock();
    LBasicBlock hasNoExtraProperties = m_out.newBlock();
    LBasicBlock asks = newColdBlock();
    OperandJoin join(*this, joinRepFor(type));
    m_out.branch(hasField(cell, field).value(), unsure(isPresent), unsure(isAbsent));
    m_out.appendTo(isPresent);
    join.add(read());
    m_out.appendTo(isAbsent);
    m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(hasNoExtraProperties), rarely(asks));
    m_out.appendTo(hasNoExtraProperties);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
    m_out.appendTo(asks);
    {
        SetForScope queriesExtraPropertiesForAbsentField(m_queriesExtraPropertiesForAbsentField, true);
        join.add(getFromObjectWithExtraProperties(cell, field.name, type));
    }
    return join.finish();
}

Word32 Lowering::hasField(CellPointer cell, const Field& field)
{
    if (!(m_program.at(field.stored).primitives & Primitive::Absent))
        return boolean(true);
    return logicalNot(isFieldAbsent(cell, field, safeIfAllSubtypesHave(cell, field)));
}

Word32 Lowering::hasOwnField(CellPointer cell, const Field& field)
{
    if (field.flags & fieldMayBeUnlisted)
        return AOT_TYPED_OPERATION(HasExtra, cell, word32(field.name.index() | nameMayBeOwnField));
    if (!(field.flags & fieldMayExistWithoutStorage))
        return hasField(cell, field);
    return either<Word32>(hasField(cell, field), [&] {
        return boolean(true);
    }, [&] {
        return AOT_TYPED_OPERATION(HasExtra, cell, word32(field.name.index() | nameMayBeOwnField));
    });
}

bool Lowering::storeNeedsNoStorage(const Field& field, ValueID stored) const
{
    if (!field.isOutOfLine)
        return false;
    ValueID constant = m_program.constantOf(m_function, stored);
    RELEASE_ASSERT(field.flags & fieldMayExistWithoutStorage && constant && m_function.at(constant).opcode == Opcode::Undefined);
    return true;
}

Word32 Lowering::isFieldAbsent(CellPointer cell, const Field& field, unsigned flags)
{
    Operand raw = loadSlot(cell, field, flags);
    if (field.store == Store::Int32OrNumberBox)
        return equal(raw.as<Word32>(), word32(absentInt32));
    return isAbsent(raw);
}

static Type inlineNumberType()
{
    return Type::integer({ minInlineInt32, INT32_MAX });
}

void Lowering::branchOnInt32Inline(Integer word, FTL::WeightedTarget inlinePath, FTL::WeightedTarget otherwise)
{
#if CPU(ARM64)
    static_assert(minInlineInt32 == -(1 << 30));
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->append(word.value(), ValueRep::SomeRegister);
    m_out.appendSuccessor(otherwise);
    m_out.appendSuccessor(inlinePath);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.m_assembler.bic<32, ARM64Assembler::S>(ARM64Registers::zr, params[0].gpr(), params[0].gpr(), ARM64Assembler::LSL, 1);
        CCallHelpers::Jump isOtherWord = jit.branchOnFlags(CCallHelpers::LessThan);
        CCallHelpers::Jump isInline;
        if (!params.fallsThroughToSuccessor(1))
            isInline = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            isOtherWord.linkTo(*labels[0], &jit);
            if (isInline.isSet())
                isInline.linkTo(*labels[1], &jit);
        });
    });
#else
    m_out.branch(lessThan(word, integer(minInlineInt32)).value(), otherwise, inlinePath);
#endif
}

Operand Lowering::readInt32OrNumberBox(CellPointer cell, const Field& field, const Type& type)
{
    auto& stored = m_program.at(field.stored);
    Integer word = signExtended(load(field.store, fieldAt(cell, field), safeIfAllSubtypesHave(cell, field)).as<Word32>());
    bool readsUndefined = stored.primitives & (std::exchange(m_fieldIsPresent, false) ? Primitive::Undefined : Primitive::Absent | Primitive::Undefined);
    bool readsNull = stored.primitives & Primitive::Null;
    Rep rep = repOf(type);
    Join join(m_out, rep, b3TypeOf(rep));
    ValueID lowered = currentValue();
    bool keepsIntegers = rep == Rep::Double && lowered;
    bool keepsInt32 = keepsIntegers && isUsedAsInt32(lowered);
    bool keepsExactInteger = keepsIntegers;
    bool keepsWord = rep == Rep::Double || rep == Rep::OptionalDouble;
    Vector<ValueFromBlock, 4> int32s;
    Vector<ValueFromBlock, 4> exactIntegers;
    Vector<ValueFromBlock, 4> words;
    bool isInlinePath = false;
    auto add = [&](Operand number, Word32 int32, Integer exactInteger) {
        if (keepsWord)
            words.append(m_out.anchor(isInlinePath ? word.value() : m_out.constInt64(INT32_MIN)));
        if (keepsInt32)
            int32s.append(m_out.anchor(int32.value()));
        if (keepsExactInteger)
            exactIntegers.append(m_out.anchor(exactInteger.value()));
        join.add(number);
    };
    LBasicBlock inlinePath = m_out.newBlock();
    LBasicBlock elsewhere = newUnlikelyBlock();
    if (!readsUndefined && !readsNull)
        branchOnInt32Inline(word, usually(inlinePath), rarely(elsewhere));
    else {
        LBasicBlock notNumber = newUnlikelyBlock();
        LBasicBlock isNil = m_out.newBlock();
        branchOnInt32Inline(word, unsure(inlinePath), unsure(notNumber));
        m_out.appendTo(notNumber);
        static_assert(absentInt32 < nullInt32 && undefinedInt32 < nullInt32);
        m_out.branch(greaterThan(word, integer(nullInt32)).value(), rarely(elsewhere), usually(isNil));
        m_out.appendTo(isNil);
        if (auto* absentSlotHandler = std::exchange(m_absentSlotHandler, nullptr)) {
            LBasicBlock isAbsent = m_out.newBlock();
            LBasicBlock decodesWord = m_out.newBlock();
            m_out.branch(equal(word, integer(absentInt32)).value(), unsure(isAbsent), unsure(decodesWord));
            m_out.appendTo(isAbsent);
            (*absentSlotHandler)();
            m_out.jump(decodesWord);
            m_out.appendTo(decodesWord);
        }
        if (readsUndefined && readsNull) {
            LBasicBlock isNull = m_out.newBlock();
            LBasicBlock isUndefined = m_out.newBlock();
            m_out.branch(equal(word, integer(nullInt32)).value(), unsure(isNull), unsure(isUndefined));
            m_out.appendTo(isNull);
            add(ensure(boxedConstant(jsNull()), Type::fromPrimitives(Primitive::Null), type), word32(0), integer(INT64_MIN));
            m_out.appendTo(isUndefined);
        }
        if (readsUndefined)
            add(ensure(undefined(), Type::fromPrimitives(Primitive::Undefined), type), word32(0), integer(INT64_MIN));
        else
            add(ensure(boxedConstant(jsNull()), Type::fromPrimitives(Primitive::Null), type), word32(0), integer(INT64_MIN));
    }
    m_out.appendTo(inlinePath);
    isInlinePath = true;
    add(ensure(word, inlineNumberType(), type), low32(word), word);
    isInlinePath = false;
    m_out.appendTo(elsewhere);
    Type numbers = stored.withoutPrimitives(Primitive::Absent | Primitive::UndefinedOrNull);
    Number boxed(m_out.loadDouble(numberBoxAt(field, low32(word))));
    ensureStillAliveHere(cell);
    Operand kept = ensure(boxed, numbers, type);
    Word32 int32 = keepsInt32 ? toInt32OnPath(kept.as<Number>(), false) : word32(0);
    add(kept, int32, keepsExactInteger ? exactIntegerOnPath(kept.as<Number>()) : integer(INT64_MIN));
    Operand joined = join.finish();
    if (keepsInt32)
        m_numberToInt32.add(joined.value, m_out.phi(Int32, int32s));
    if (keepsExactInteger)
        m_numberToExactInteger.add(joined.value, m_out.phi(Int64, exactIntegers));
    if (keepsWord)
        m_numberToWord.add(joined.value, m_out.phi(Int64, words));
    return joined;
}

bool Lowering::isStoredInline(const Field& field, ValueID stored) const
{
    return field.store != Store::Int32OrNumberBox || typeOf(stored).withoutPrimitives(Primitive::UndefinedOrNull).isSubsetOf(inlineNumberType(), m_program);
}

void Lowering::storeInt32OrNumberBox(CellPointer cell, const Field& field, ValueID stored, bool isFirstStore, LBasicBlock otherwise)
{
    auto& type = typeOf(stored);
    auto put = [&](Word32 word) {
        store(field.store, word, fieldAt(cell, field));
    };
    Type inlinePath = inlineNumberType();
    auto putInt32OrElse = [&](LBasicBlock isNotInline) {
#if CPU(ARM64)
        Integer word = narrowOrElse(get(stored), type, Type::integer(Range::int32()), isNotInline).as<Integer>();
        LBasicBlock isInline = m_out.newBlock();
        branchOnInt32Inline(word, usually(isInline), rarely(isNotInline));
        m_out.appendTo(isInline);
        put(low32(word));
#else
        put(low32(narrowOrElse(get(stored), type, inlinePath, isNotInline).as<Integer>()));
#endif
    };
    if (type.isSubsetOf(inlinePath, m_program) && isFirstStore) {
        put(low32(in<Integer>(stored)));
        return;
    }
    LBasicBlock continuation = m_out.newBlock();
    if (type.primitives & Primitive::Number && !isFirstStore) {
        LBasicBlock hasBox = newUnlikelyBlock();
        LBasicBlock hasNoBox = m_out.newBlock();
        bool mayHoldNonNumber = m_program.at(field.stored).primitives & (Primitive::Absent | Primitive::UndefinedOrNull);
        LBasicBlock hasNoInlineNumber = mayHoldNonNumber ? newUnlikelyBlock() : hasBox;
        bool isUsuallyPresent = !mayHoldNonNumber || !(field.flags & fieldIsOptional) || field.flags & fieldIsSetAtCreation;
        Integer word = signExtended(load(field.store, fieldAt(cell, field), 0).as<Word32>());
        if (isUsuallyPresent)
            branchOnInt32Inline(word, usually(hasNoBox), rarely(hasNoInlineNumber));
        else
            branchOnInt32Inline(word, unsure(hasNoBox), unsure(hasNoInlineNumber));
        m_out.appendTo(hasNoInlineNumber);
        Word32 current = load(field.store, fieldAt(cell, field), 0).as<Word32>();
        if (mayHoldNonNumber) {
            m_out.branch(greaterThan(signExtended(current), integer(nullInt32)).value(), rarely(hasBox), usually(hasNoBox));
            m_out.appendTo(hasBox);
        }
        m_out.storeDouble(narrowOrElse(get(stored), type, Type::fromPrimitives(Primitive::Number), hasNoBox).as<Number>().value(), numberBoxAt(field, current));
        ensureStillAliveHere(cell);
        m_out.jump(continuation);
        m_out.appendTo(hasNoBox);
    }
    if (type.isSubsetOf(inlinePath, m_program)) {
        put(low32(in<Integer>(stored)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    if (type.primitives & Primitive::Number) {
        LBasicBlock isNotInline = type.mayBeUndefinedOrNull() ? m_out.newBlock() : newColdBlock();
        if (type.isInteger() && type.integers.isSubsetOf(Range::int32())) {
            Integer word = in<Integer>(stored);
            LBasicBlock isInline = m_out.newBlock();
            branchOnInt32Inline(word, usually(isInline), rarely(isNotInline));
            m_out.appendTo(isInline);
            put(low32(word));
        } else if (auto found = m_numberToWord.find(get(stored).value); found != m_numberToWord.end()) {
            Integer word(found->value);
            LBasicBlock isWord = m_out.newBlock();
            LBasicBlock isNotWord = newUnlikelyBlock();
            branchOnInt32Inline(word, usually(isWord), rarely(isNotWord));
            m_out.appendTo(isWord);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                LBasicBlock doesNotHold = newColdBlock();
                LBasicBlock holds = m_out.newBlock();
                m_out.branch(equal(bitsOf(convertToNumber(word)), bitsOf(get(stored))).value(), usually(holds), rarely(doesNotHold));
                m_out.appendTo(doesNotHold);
                {
                    SetForScope isReporting(m_isReportingFactViolation, true);
                    AOT_TYPED_OPERATION(FactViolated, boxInt32(low32(word)), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
                }
                m_out.jump(holds);
                m_out.appendTo(holds);
            }
            put(low32(word));
            m_out.jump(continuation);
            m_out.appendTo(isNotWord);
            putInt32OrElse(isNotInline);
        } else
            putInt32OrElse(isNotInline);
        m_out.jump(continuation);
        m_out.appendTo(isNotInline);
    }
    for (auto [primitive, pattern] : { std::pair { Primitive::Undefined, undefinedInt32 }, std::pair { Primitive::Null, nullInt32 } }) {
        if (!(type.primitives & primitive))
            continue;
        LBasicBlock isIt = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(fits(get(stored), type, Type::fromPrimitives(primitive)).value(), unsure(isIt), unsure(isNot));
        m_out.appendTo(isIt);
        put(word32(pattern));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    if (isStoredInline(field, stored))
        m_out.unreachable();
    else if (otherwise)
        m_out.jump(otherwise);
    else {
        if (isFirstStore)
            put(word32(0));
        if (get(stored).rep == Rep::Double)
            AOT_TYPED_OPERATION(PutNumberInNumberBox, cell, word32(field.name.index()), in<Number>(stored));
        else if (get(stored).rep == Rep::OptionalDouble && !(type.primitives & ~(Primitive::Number | Primitive::UndefinedOrNull)))
            AOT_TYPED_OPERATION(PutNumberInNumberBox, cell, word32(field.name.index()), Number(get(stored).value));
        else
            AOT_TYPED_OPERATION(PutInNumberBox, cell, word32(field.name.index()), in<Boxed>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

bool Lowering::isInitializedWith(const Allocation& allocation, StringID name) const
{
    auto& layout = m_program.at(allocation.layout);
    if (!isObjectLayout(layout.kind))
        return true;
    auto field = layout.fieldNamed(name);
    if (!field)
        return false;
    auto initializedFields = allocation.order.span().first(allocation.initialFieldCount);
    static constexpr size_t maxFieldsToCompare = 16;
    if (initializedFields.size() <= maxFieldsToCompare)
        return std::ranges::find(initializedFields, *field) != initializedFields.end();
    if (m_initializedFieldsAllocation != &allocation) {
        m_initializedFieldsAllocation = &allocation;
        m_initializedFields.clearAll();
        for (uint16_t initializedField : initializedFields)
            m_initializedFields.set(initializedField);
    }
    return m_initializedFields.get(*field);
}

std::optional<uint32_t> Lowering::fieldInitializedBy(LayoutID id, const Value& store) const
{
    if (store.opcode == Opcode::PutField && store.layout() == id)
        return store.field();
    return m_program.at(id).fieldNamed(store.opcode == Opcode::PutById ? store.string() : m_program.at(store.layout()).fields[store.field()].name);
}

bool Lowering::mayHaveCollectedSinceAllocation(ValueID object) const
{
    auto& layout = m_program.at(m_function.at(object).layout());
    auto& values = m_currentBlock->values;
    auto& last = m_collectionCheckCache.add(object.index(), CollectionCheckCache { }).iterator->value;
    if (last.block != m_currentBlock || last.index > m_currentIndexInBlock)
        last = CollectionCheckCache { m_currentBlock, 0, false };
    if (last.collectorMayHaveRun)
        return true;
    size_t alreadyVisited = std::exchange(last.index, m_currentIndexInBlock);
    for (size_t i = m_currentIndexInBlock; i-- > alreadyVisited && values[i] != object;) {
        auto& earlier = m_function.at(values[i]);
        if (earlier.opcode == Opcode::NewClosure) {
            last.collectorMayHaveRun = true;
            return true;
        }
        auto field = fieldInitializedBy(m_function.at(object).layout(), earlier);
        if (field && (!isStoredInline(layout.fields[*field], earlier.child(1)) || !isInitializedWith(m_program.allocations[m_function.at(object).allocation()], layout.fields[*field].name))) {
            last.collectorMayHaveRun = true;
            return true;
        }
    }
    return false;
}

const Field* Lowering::fieldOfStoreAfterShapeTestOnlyAt(size_t index, ValueID object)
{
    auto& value = m_function.at(m_currentBlock->values[index]);
    if (value.opcode != Opcode::PutById || value.child(0) != object)
        return nullptr;
    if (m_cloneGroup || value.storesLazyClosure || lazyClosureStoredBy(value) || mayBeStoredBeforeJump(value) || objectLiteralAt(index) || initialStoresAt(index) || namedStoresAt(index) || propertyRunAt(index))
        return nullptr;
    auto& receiver = typeOf(object);
    LayoutID supertype = receiver.singleLayout();
    const Field* field = fieldFor(receiver, value.string());
    if (!supertype || !field || receiver.mayBeUndefinedOrNull() || isInitializingStore(m_program, m_function, *m_currentBlock, index))
        return nullptr;
    if (mayBeFrozen(supertype) && !isPrivateName(m_program.string(value.string())))
        return nullptr;
    return field;
}

bool Lowering::isSideEffectFreeFieldRead(size_t index)
{
    ValueID id = m_currentBlock->values[index];
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::GetById || writesOf(m_program, m_function, *m_currentBlock, index) > Writes::Nothing)
        return false;
    auto& receiver = typeOf(value.child(0));
    LayoutID supertype = receiver.singleLayout();
    const Field* field = fieldFor(receiver, value.string());
    if (!supertype || !field || !field->stored || field->isMissingInSomeSubtype || receiver.mayBeUndefinedOrNull())
        return false;
    if (m_program.at(supertype).acceptsUntypedObjects && !isAllocatedAsSubtypeOf(value.child(0), supertype))
        return false;
    Type held = m_program.at(field->stored);
    if (held.primitives & Primitive::Absent)
        held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
    return held.isSubsetOf(typeOf(id), m_program);
}

std::optional<Lowering::FieldCreationRun> Lowering::fieldCreationRunAt(size_t first, ValueID object)
{
    static constexpr unsigned maxFieldsInRun = sizeof(uint64_t) / sizeof(uint16_t);
    if (Options::countAOTTypedAllocations() || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    if ((m_out.m_block != m_unconditionalBlock && !m_isAfterStoreShapeTest) || m_isLoweringInitialStoresOneByOne)
        return std::nullopt;
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    auto& values = m_currentBlock->values;
    LayoutID supertype = typeOf(object).singleLayout();
    FieldCreationRun run;
    const Field* firstField = nullptr;
    auto isImpliedByFirstTest = [&](const Field& field) {
        if (!field.isMissingInSomeSubtype)
            return true;
        auto& test = field.subtypesWithField;
        auto& firstTest = firstField->subtypesWithField;
        return firstField->isMissingInSomeSubtype && test.first == firstTest.first && test.count == firstTest.count && test.set == firstTest.set && test.blockByte == firstTest.blockByte && test.passesEarlierBlocks == firstTest.passesEarlierBlocks;
    };
    for (size_t index = first; index < values.size() && run.fields.size() < maxFieldsInRun; ++index) {
        auto& value = m_function.at(values[index]);
        if (const Field* field = fieldOfStoreAfterShapeTestOnlyAt(index, object)) {
            if (!firstField)
                firstField = field;
            if (m_program.at(field->slot).isDiscriminant || field->hasOtherOffsetInSomeSubtype || !isImpliedByFirstTest(*field))
                break;
            if (!(m_program.at(field->stored).primitives & Primitive::Absent))
                continue;
            if (field->isMissingInSomeSubtype != firstField->isMissingInSomeSubtype)
                break;
            if (field->isOutOfLine || !slotKeyFor(field->offset, false))
                break;
            if (storeMayConsultPrototypeChain(m_program, m_function, *m_currentBlock, index, supertype, field->name))
                break;
            run.stores.append(index);
            if (!run.fields.contains(field))
                run.fields.append(field);
            continue;
        }
        if (index == first)
            return std::nullopt;
        if (isSideEffectFreeFieldRead(index))
            continue;
        if (mayThrow(m_program, m_function, value) || writesOf(m_program, m_function, *m_currentBlock, index) > Writes::RuntimeState)
            break;
    }
    if (run.fields.size() < 2 || run.stores.first() != first)
        return std::nullopt;
    return run;
}

void Lowering::didCreateFieldsInRun(CellPointer cell, uint64_t slotKeys, LValue cache)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, OperationEffects { Writes::Fields, false, false, true });
    patchpoint->append(ConstrainedValue(cell.value(), ValueRep::reg(GPRInfo::argumentGPR1)));
    patchpoint->append(ConstrainedValue(cache, ValueRep::reg(GPRInfo::argumentGPR3)));
    RegisterSet changed = RegisterSet::macroClobberedGPRs();
    changed.add(GPRInfo::argumentGPR2, IgnoreVectors);
    for (GPRReg reg : stubTemporaryGPRs)
        changed.add(reg, IgnoreVectors);
    patchpoint->clobber(changed);
    if (!hasFramelessFunctionStubs && (LoweringContext::coldSitesForceFrame() || m_activeHandler)) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasFramelessFunctionStubs)
        m_context.hasColdSites = true;
    patchpoint->setGenerator([context = &m_context, slotKeys](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasFramelessFunctionStubs && hasNoFrame;
        bool createsFrame = !hasFramelessFunctionStubs && hasNoFrame;
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (createsFrame)
            jit.emitFunctionPrologue();
        jit.move(CCallHelpers::TrustedImm64(slotKeys), GPRInfo::argumentGPR2);
        context->stubCalls.call(jit, isLeaf ? Stub::LeafDidCreateTypedFieldRun : Stub::DidCreateTypedFieldRun, CallSite { });
        if (createsFrame)
            jit.emitFunctionEpilogueWithEmptyFrame();
    });
}

void Lowering::createFieldsInRun(CellPointer cell, const FieldCreationRun& run)
{
    m_fieldCreationRunBlock = m_currentBlock;
    m_storesInFieldCreationRun = run.stores;
    m_storesInFieldCreationRun.removeAt(0);
    LBasicBlock creates = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    for (const Field* field : run.fields) {
        LBasicBlock presentCase = m_out.newBlock();
        m_out.branch(isFieldAbsent(cell, *field, 0).value(), unsure(creates), unsure(presentCase));
        m_out.appendTo(presentCase);
    }
    m_out.jump(continuation);

    m_out.appendTo(creates);
    m_context.sites.append(Site { });
    Pointer cache = slotAddress(m_context.sites.size() - 1);
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    m_out.branch(m_out.equal(words, m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer)))), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    m_out.store64(m_out.bitXor(words, m_out.zeroExtPtr(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, offset))))), cell.value(), m_heaps.AOTTypedObject_orderWord);
    m_out.jump(continuation);

    m_out.appendTo(miss);
    uint64_t slotKeys = 0;
    for (unsigned i = 0; i < run.fields.size(); ++i)
        slotKeys |= static_cast<uint64_t>(slotKeyFor(run.fields[i]->offset, false)) << (16 * i);
    didCreateFieldsInRun(cell, slotKeys, cache.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

bool Lowering::isCreatedWithRun(CellPointer cell, ValueID object)
{
    if (m_fieldCreationRunBlock == m_currentBlock && m_storesInFieldCreationRun.removeFirst(m_currentIndexInBlock))
        return true;
    RELEASE_ASSERT(m_fieldCreationRunBlock != m_currentBlock || m_storesInFieldCreationRun.isEmpty() || m_storesInFieldCreationRun.first() > m_currentIndexInBlock);
    auto run = fieldCreationRunAt(m_currentIndexInBlock, object);
    if (!run)
        return false;
    createFieldsInRun(cell, *run);
    return true;
}

uint32_t Lowering::extentOfFollowingStores(const Field& field)
{
    uint32_t reach = field.offset + sizeOf(field.store);
    auto& values = m_currentBlock->values;
    if (m_currentIndexInBlock >= values.size() || m_function.at(values[m_currentIndexInBlock]).opcode != Opcode::PutById)
        return reach;
    ValueID object = m_function.at(values[m_currentIndexInBlock]).child(0);
    for (size_t index = m_currentIndexInBlock + 1; index < values.size(); ++index) {
        auto& value = m_function.at(values[index]);
        if (value.opcode != Opcode::PutById || value.child(0) != object)
            continue;
        if (const Field* next = fieldFor(typeOf(object), value.string()); next && next->isOutOfLine)
            reach = std::max<uint32_t>(reach, next->offset + sizeOf(next->store));
    }
    return reach;
}

void Lowering::didCreateFieldInCell(CellPointer cell, const Field& field, OperationEffects effects, const ScopedLambda<void()>& onCacheMiss)
{
    uint32_t slotKey = slotKeyFor(field.offset, field.isOutOfLine);
    auto ensureStorage = [&] {
        if (!field.isOutOfLine)
            return;
        LBasicBlock hasNoExtension = m_out.newBlock();
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock hasStorage = m_out.newBlock();
        Word32 word = extensionWordOf(cell);
        if (field.offset + sizeOf(field.store) <= minExtensionSize)
            m_out.branch(isZero(word).value(), unsure(hasNoExtension), unsure(hasStorage));
        else {
            LBasicBlock hasExtension = m_out.newBlock();
            m_out.branch(isZero(word).value(), unsure(hasNoExtension), unsure(hasExtension));
            m_out.appendTo(hasExtension);
            Word32 reach = loadField<Word32>(extensionAt(word), m_heaps.AOTTypedExtension_size, IsFixed);
            m_out.branch(aboveOrEqual(reach, word32(field.offset + sizeOf(field.store))).value(), usually(hasStorage), rarely(slowPath));
        }
        m_out.appendTo(hasNoExtension);
        size_t size = WTF::roundUpToMultipleOf<minExtensionSize>(field.offset + sizeOf(field.store));
        if (isX86_64() && size <= 256) {
            static_assert(minExtensionSize == 16);
            callAllocationStub(static_cast<Stub>(static_cast<unsigned>(Stub::AllocateTypedExtension16) + size / minExtensionSize - 1), { { cell.value(), GPRInfo::argumentGPR1 } }, effects, false, true);
            barrier(cell);
            m_out.jump(hasStorage);
            m_out.appendTo(slowPath);
            AOT_TYPED_OPERATION_WITH_EFFECTS(EnsureStorage, effects, cell, word32(field.name.index()));
            m_out.jump(hasStorage);
            m_out.appendTo(hasStorage);
            return;
        }
        LValue storage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size), slowPath);
        applyEffects(storage->as<PatchpointValue>(), { Writes::RuntimeState, false });
        static_assert(!offsetOfExtensionSize);
        if (isX86_64() && size > 2 * sizeof(uint64_t)) {
            PatchpointValue* zeroes = m_out.patchpoint(Void);
            zeroes->effects = Effects::none();
            m_heaps.decoratePatchpointWrite(&m_heaps.root, zeroes);
            zeroes->append(ConstrainedValue(storage, ValueRep::SomeRegister));
            zeroes->numGPScratchRegisters = 1;
            zeroes->setGenerator([size](CCallHelpers& jit, const StackmapGenerationParams& params) {
                GPRReg zero = params.gpScratch(0);
                jit.move(CCallHelpers::TrustedImm32(0), zero);
                for (size_t offset = sizeof(uint64_t); offset < size; offset += sizeof(uint64_t))
                    jit.store64(zero, CCallHelpers::Address(params[0].gpr(), offset));
                jit.store64(CCallHelpers::TrustedImm32(size), CCallHelpers::Address(params[0].gpr()));
            });
        } else {
            for (size_t offset = 0; offset < size; offset += sizeof(uint64_t))
                m_out.store64(m_out.constInt64(offset ? 0 : size), TypedPointer(m_heaps.root, m_out.add(storage, m_out.constIntPtr(offset))));
        }
        fenceAfterInitialization();
        storeField(cell, m_heaps.AOTTypedObject_extension, Word32(m_out.castToInt32(m_out.lShr(storage, m_out.constInt32(cell32Shift)))));
        barrier(cell);
        m_out.jump(hasStorage);
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION_WITH_EFFECTS(EnsureStorage, effects, cell, word32(field.name.index()));
        m_out.jump(hasStorage);
        m_out.appendTo(hasStorage);
    };
    auto callStubPath = [&](LValue cache, uint32_t storageSize = 0) {
        onCacheMiss();
        if (!slotKey)
            AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(field.name.index()));
        else if (isARM64()) {
            PatchpointValue* patchpoint = m_out.patchpoint(Void);
            applyEffects(patchpoint, effects);
            patchpoint->append(ConstrainedValue(cell.value(), ValueRep::reg(GPRInfo::argumentGPR1)));
            patchpoint->append(ConstrainedValue(cache, ValueRep::reg(GPRInfo::argumentGPR3)));
            RegisterSet changed = RegisterSet::macroClobberedGPRs();
            changed.add(GPRInfo::argumentGPR2, IgnoreVectors);
            if (storageSize)
                changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
            for (GPRReg reg : stubTemporaryGPRs)
                changed.add(reg, IgnoreVectors);
            patchpoint->clobber(changed);
            if (!hasFramelessFunctionStubs) {
                m_context.emitsCalls = true;
                patchpoint->clobberLate(RegisterSet { callMarkerGPR });
            }
            patchpoint->setGenerator([context = &m_context, offset = slotKey, storageSize](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
                if (isLeaf)
                    jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
                jit.move(CCallHelpers::TrustedImm32(offset), GPRInfo::argumentGPR2);
                if (storageSize) {
                    jit.move(CCallHelpers::TrustedImm32(storageSize), GPRInfo::returnValueGPR);
                    context->stubCalls.call(jit, isLeaf ? Stub::LeafCreateTypedFieldOutOfLine : Stub::CreateTypedFieldOutOfLine, CallSite { });
                    return;
                }
                context->stubCalls.call(jit, isLeaf ? Stub::LeafDidCreateTypedField : Stub::DidCreateTypedField, CallSite { });
            });
        } else if (storageSize)
            callICStub(Stub::CreateTypedFieldOutOfLine, Void, { { cell.value(), GPRInfo::argumentGPR1 }, { cache, GPRInfo::argumentGPR3 }, { m_out.constIntPtr(storageSize), GPRInfo::returnValueGPR } }, std::pair { GPRInfo::argumentGPR2, slotKey }, effects);
        else
            callICStub(Stub::DidCreateTypedField, Void, { { cell.value(), GPRInfo::argumentGPR1 }, { cache, GPRInfo::argumentGPR3 } }, std::pair { GPRInfo::argumentGPR2, slotKey }, effects);
    };
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
        callStubPath(m_out.constIntPtr(0));
        ensureStorage();
        return;
    }
    m_context.sites.append(Site { });
    Pointer cache = slotAddress(m_context.sites.size() - 1);
    if (field.isOutOfLine && slotKey) {
        callStubPath(cache.value(), WTF::roundUpToMultipleOf<minExtensionSize>(extentOfFollowingStores(field)));
        return;
    }
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    Word32 isHit(m_out.equal(words, m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer)))));
    m_out.branch(isHit.value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    m_out.store64(m_out.bitXor(words, m_out.zeroExtPtr(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, offset))))), cell.value(), m_heaps.AOTTypedObject_orderWord);
    m_out.jump(continuation);
    m_out.appendTo(miss);
    callStubPath(cache.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    ensureStorage();
}

bool Lowering::isOnlyComparedWithStringsBeforeStores(ValueID read, Vector<ValueID, 4>& stringsByIndex)
{
    auto mayStoreToField = [&](const Block& block, size_t index) {
        auto& value = m_function.at(block.values[index]);
        switch (value.opcode) {
        case Opcode::CallDirect:
            return m_program.at(value.function()).writes >= Writes::Fields;
        case Opcode::PutById:
        case Opcode::PutField:
        case Opcode::PutByVal:
        case Opcode::DeleteById:
        case Opcode::DeleteByVal:
            return true;
        default:
            return writesOf(m_program, m_function, block, index) >= Writes::Fields;
        }
    };
    size_t readBlock = m_currentBlock - m_function.blocks.span().data();
    BitVector isReached(m_function.blocks.size());
    BitVector isReachedAfterStore(m_function.blocks.size());
    Vector<uint32_t, 16> worklist;
    auto leave = [&](const Block& block, bool afterStore) {
        for (auto& target : block.terminator.targets) {
            uint32_t next = target.block.index();
            if (next == readBlock || (isReached.get(next) && (!afterStore || isReachedAfterStore.get(next))))
                continue;
            isReached.set(next);
            if (afterStore)
                isReachedAfterStore.set(next);
            worklist.append(next);
        }
    };
    auto storesIn = [&](const Block& block, size_t from, size_t to) {
        for (size_t index = from; index < to; ++index) {
            if (mayStoreToField(block, index))
                return true;
        }
        return false;
    };
    leave(*m_currentBlock, storesIn(*m_currentBlock, m_currentIndexInBlock + 1, m_currentBlock->values.size()));
    while (!worklist.isEmpty()) {
        uint32_t next = worklist.takeLast();
        auto& block = m_function.blocks[next];
        leave(block, isReachedAfterStore.get(next) || storesIn(block, 0, block.values.size()));
    }
    Vector<ValueID, 4> same { read };
    auto countComparisons = [&]() -> std::optional<unsigned> {
        unsigned numberOfComparisons = 0;
        stringsByIndex.clear();
        for (size_t number = 0; number < m_function.blocks.size(); ++number) {
            auto& block = m_function.blocks[number];
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = m_function.at(block.values[index]);
                if (value.opcode != Opcode::StrictEq || (!same.contains(value.child(0)) && !same.contains(value.child(1))))
                    continue;
                ValueID text = value.child(same.contains(value.child(0)));
                if (m_function.at(text).opcode != Opcode::String)
                    return std::nullopt;
                if (number == readBlock ? storesIn(block, m_currentIndexInBlock + 1, index) : isReachedAfterStore.get(number) || storesIn(block, 0, index))
                    return std::nullopt;
                ++numberOfComparisons;
                bool isKnown = std::ranges::any_of(stringsByIndex, [&](ValueID known) {
                    return m_function.at(known).string() == m_function.at(text).string();
                });
                if (!isKnown)
                    stringsByIndex.append(text);
            }
        }
        return numberOfComparisons;
    };
    auto readComparisons = countComparisons();
    if (!readComparisons)
        return false;
    unsigned numberOfUses = useCountOf(read);
    if (*readComparisons == numberOfUses) {
        if (*readComparisons <= stringsByIndex.size() || *readComparisons < stringsByIndex.size() * stringsByIndex.size())
            stringsByIndex.clear();
        return numberOfUses;
    }
    for (size_t known = 0; known != same.size();) {
        known = same.size();
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && same.contains(value.child(0)) && !same.contains(id))
                    same.append(id);
            }
        }
    }
    for (size_t i = 1; i < same.size(); ++i)
        numberOfUses += useCountOf(same[i]);
    if (same.size() == 1)
        return false;
    auto totalComparisons = countComparisons();
    return totalComparisons && *totalComparisons && *totalComparisons + same.size() - 1 == numberOfUses;
}

TypedPointer Lowering::pinnedFieldsGuard(SlotID slot)
{
    return m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedModifiedLiterals() + slot.index() % numberOfPinnedFieldGuards);
}

Word32 Lowering::hasShapePinnedTo(const PinnedRead& read, StringID text)
{
    Vector<LayoutID, 1> withString;
    for (LayoutID subtype : m_program.at(read.supertype).subtypes) {
        auto& layout = m_program.at(subtype);
        auto& field = layout.fields[*layout.fieldNamed(read.name)];
        RELEASE_ASSERT(field.isPinned);
        if (field.literals[0].string == text)
            withString.append(subtype);
    }
    return withString.isEmpty() ? boolean(false) : passesAnyOf(read.shapeWord, withString);
}

Word32 Lowering::isPinnedTo(const PinnedRead& read, ValueID constant)
{
    StringID text = m_function.at(constant).string();
    if (read.index) {
        size_t which = read.texts.find(text);
        RELEASE_ASSERT(which != notFound);
        return equal(read.index, word32(which));
    }
    return either<Word32>(isZero(read.isModified), [&] {
        return hasShapePinnedTo(read, text);
    }, [&] {
        auto& supertype = m_program.at(read.supertype);
        auto& field = supertype.fields[*supertype.fieldNamed(read.name)];
        auto ascii = constantASCIIString(constant);
        auto& stored = m_program.at(field.stored);
        if (stored.primitives & Primitive::String && stored.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && !field.isMissingInSomeSubtype && !field.hasOtherOffsetInSomeSubtype && ascii && !ascii->empty()) {
            Operand held = readField(read.cell, field, stored);
            if (held.rep == Rep::Cell)
                return equalsLiteralByStub(held.as<CellPointer>(), constant);
            Boxed value = held.as<Boxed>();
            return either<Word32>(isCell(value, stored), [&] {
                return equalsLiteralByStub(asCell(value), constant);
            }, [&] {
                return boolean(false);
            });
        }
        return Word32(m_out.castToInt32(AOT_TYPED_OPERATION(FieldEqualsString, read.cell, word32(read.name.index()), word32(text.index())).value()));
    });
}

void Lowering::guardPinnedFields(LayoutID holder, const Field& field, ValueID stored)
{
    if (!field.slot || !m_program.at(field.slot).hasPinnedFields)
        return;
    if (ValueID constant = m_program.constantOf(m_function, stored))
        stored = constant;
    bool preservesFields = holder && m_function.at(stored).opcode == Opcode::String;
    if (preservesFields) {
        for (LayoutID subtype : m_program.at(holder).subtypes) {
            auto& layout = m_program.at(subtype);
            auto own = layout.fieldNamed(field.name);
            if (own && layout.fields[*own].slot == field.slot && layout.fields[*own].isPinned)
                preservesFields &= layout.fields[*own].literals[0].string == m_function.at(stored).string();
        }
    }
    if (!preservesFields)
        m_out.store32As8(m_out.int32One, pinnedFieldsGuard(field.slot));
}

CellPointer Lowering::materializeLazyClosure(CellPointer cell, const Field& field)
{
    auto& callee = m_program.at(field.lazyClosure);
    FunctionID general = genericEntrypointFor(field.lazyClosure);
    auto& entrypoint = m_program.at(general);
    unsigned numberOfCaptures = callee.captureTypes.size();
    RELEASE_ASSERT(m_program.usesSlimClosures(callee) && numberOfCaptures <= 1);
    uint64_t request = general.index() | static_cast<uint64_t>(slimClosureKindOf(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs)) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        AOT_TYPED_OPERATION(CountClosure, word32(general.index()), word32(numberOfCaptures));
    Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(static_cast<int64_t>(request)).value(), stubTemporaryGPRs[0] }, { cell.value(), GPRInfo::toArgumentRegister(2) }, { m_out.add(cell.value(), m_out.constIntPtr(field.offset)), GPRInfo::toArgumentRegister(3) } };
    CellPointer closure(callAllocationStub(numberOfCaptures ? Stub::NewTypedClosureOnce32 : Stub::NewTypedClosureWithoutCaptureOnce32, operands, { Writes::RuntimeState, false }, false));
    m_out.store32(m_out.castToInt32(m_out.lShr(closure.value(), m_out.constInt32(cell32Shift))), fieldAt(cell, field));
    return closure;
}

void Lowering::materializeLazyClosureIfNeeded(CellPointer cell, const Field& field)
{
    LBasicBlock isNotCreated = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.equal(m_out.load32(fieldAt(cell, field)), m_out.constInt32(unmaterializedClosureCell32)), rarely(isNotCreated), usually(continuation));
    m_out.appendTo(isNotCreated);
    materializeLazyClosure(cell, field);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::writeField(CellPointer cell, ValueID object, const Field& field, ValueID stored)
{
    if (m_function.at(m_currentBlock->values[m_currentIndexInBlock]).storesLazyClosure) {
        RELEASE_ASSERT(isLoweringInitializingStore());
        didEmitWrite(Writes::RuntimeState);
        m_out.store32(m_out.constInt32(unmaterializedClosureCell32), fieldAt(cell, field));
        didLowerInitializingStore();
        return;
    }
    guardPinnedFields(isLoweringInitializingStore() ? m_function.at(object).layout() : typeOf(object).singleLayout(), field, stored);
    auto put = [&] {
        if (field.store == Store::Int32OrNumberBox)
            storeInt32OrNumberBox(cell, field, stored, isLoweringInitializingStore());
        else
            store(field.store, in(stored, repOf(field.store)), fieldAt(cell, field));
    };
    SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
    bool isInitializing = isLoweringInitializingStore();
    if (isInitializing && isInitializedWith(m_program.allocations[m_function.at(object).allocation()], field.name)) {
        if (storeNeedsNoStorage(field, stored)) {
            didLowerInitializingStore();
            return;
        }
        didEmitWrite(Writes::RuntimeState);
        put();
        if (holdsCells(field.store) && mayBeCell(stored) && mayHaveCollectedSinceAllocation(object))
            emitBarrierForLiteralString(cell, stored);
        didLowerInitializingStore();
        return;
    }
    didEmitWrite(Writes::Fields);
    bool isConstructorStoreToThis = m_function.kind == FunctionKind::Constructor && m_function.has(Format::FunctionHasThis) && !m_function.at(m_currentBlock->values[m_currentIndexInBlock]).inlineFrame && withoutUpcasts(object) == m_function.blocks[0].parameters[m_function.captureTypes.size()];
    if (field.flags & fieldMayBeUnlisted && !isInitializing && !isConstructorStoreToThis) {
        LayoutID holder = typeOf(object).singleLayout();
        uint32_t initializedBefore = UINT32_MAX;
        bool isSameInEverySubtype = !!holder;
        if (holder) {
            for (LayoutID subtype : m_program.at(holder).subtypes) {
                auto& layout = m_program.at(subtype);
                auto named = layout.fieldNamed(field.name);
                if (!named)
                    continue;
                uint32_t count = 0;
                for (uint32_t index = 0; index < *named; ++index)
                    count += !(layout.fields[index].flags & (fieldIsAbsentAtCreation | fieldIsNeverStored));
                isSameInEverySubtype &= initializedBefore == UINT32_MAX || initializedBefore == count;
                initializedBefore = count;
            }
        }
        LBasicBlock mayBeUnlisted = newColdBlock();
        LBasicBlock isListed = m_out.newBlock();
        if (isSameInEverySubtype && initializedBefore < maxFieldsInOrder)
            m_out.branch(aboveOrEqual(bitAnd(orderWordOf(cell), word32(maxFieldsInOrder << 16)), word32((initializedBefore + 1) << 16)).value(), usually(isListed), rarely(mayBeUnlisted));
        else
            m_out.jump(mayBeUnlisted);
        m_out.appendTo(mayBeUnlisted);
        AOT_TYPED_OPERATION(ListFieldsBelow, cell, word32(field.name.index() | listsFieldNamed));
        m_out.jump(isListed);
        m_out.appendTo(isListed);
    }
    bool keepsDiscriminant = !isInitializing && m_program.at(field.slot).isDiscriminant;
    bool mayBeAbsent = m_program.at(field.stored).primitives & Primitive::Absent;
    LayoutID supertype = typeOf(object).singleLayout();
    bool mayConsultPrototypeChain = mayBeAbsent && storeMayConsultPrototypeChain(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, supertype, field.name);
    bool checksPrototypeChain = mayConsultPrototypeChain && inheritsThroughLayoutPrototype(m_program, supertype, field.name, true);
    bool mayConsultPrototype = mayConsultPrototypeChain && !checksPrototypeChain && !m_orderWordHasNoFlag;
    bool mayRefuse = mayBeAbsent && supertype && !checksPrototypeChain && !m_orderWordHasNoFlag && mayBeFrozen(supertype) && !isPrivateName(m_program.string(field.name));
    bool mayAlreadyExist = field.flags & fieldMayExistWithoutStorage;
    LBasicBlock done = keepsDiscriminant || checksPrototypeChain || mayConsultPrototype || (mayRefuse && (isInSloppyCode() || mayAlreadyExist)) ? m_out.newBlock() : nullptr;
    auto keepDiscriminant = [&] {
        LBasicBlock differs = newColdBlock();
        Boxed current = readField(cell, field, m_program.at(m_program.types.anything)).as<Boxed>();
        m_out.branch(equal(current, in<Boxed>(stored)).value(), usually(done), rarely(differs));
        m_out.appendTo(differs);
        OperationEffects effects { Writes::Fields, true, storeMayDespecializeObject(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, supertype) };
        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
        m_out.jump(done);
    };
    if (keepsDiscriminant && !mayBeAbsent) {
        keepDiscriminant();
        m_out.appendTo(done);
        return;
    }
    if (mayBeAbsent && !isCreatedWithRun(cell, object)) {
        LBasicBlock creates = field.flags & fieldIsSetAtCreation ? newUnlikelyBlock() : m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock present = keepsDiscriminant ? m_out.newBlock() : continuation;
        if (field.isOutOfLine) {
            RELEASE_ASSERT(representsAbsentAsZero(field.store));
            LBasicBlock hasExtension = m_out.newBlock();
            Word32 word = extensionWordOf(cell);
            m_out.branch(isZero(word).value(), unsure(creates), unsure(hasExtension));
            m_out.appendTo(hasExtension);
            Pointer extension = extensionAt(word);
            if (field.offset + sizeOf(field.store) > minExtensionSize) {
                LBasicBlock within = m_out.newBlock();
                Word32 size = loadField<Word32>(extension, m_heaps.AOTTypedExtension_size, IsFixed);
                m_out.branch(aboveOrEqual(size, word32(field.offset + sizeOf(field.store))).value(), unsure(within), unsure(creates));
                m_out.appendTo(within);
            }
            TypedPointer slot = m_out.address(heapOf(field.slot), extension.value(), field.offset);
            LValue isEmpty = sizeOf(field.store) == 8 ? m_out.isZero64(m_out.load64(slot)) : m_out.isZero32(sizeOf(field.store) == 4 ? m_out.load32(slot) : m_out.load8ZeroExt32(slot));
            m_out.branch(isEmpty, unsure(creates), unsure(present));
        } else
            m_out.branch(isFieldAbsent(cell, field, 0).value(), unsure(creates), unsure(present));
        if (keepsDiscriminant) {
            m_out.appendTo(present);
            keepDiscriminant();
        }
        m_out.appendTo(creates);
        if (checksPrototypeChain) {
            setByName(box(cell), field.name, in<Boxed>(stored));
            m_out.jump(done);
        } else {
            auto onCacheMiss = [&] {
                if (!mayConsultPrototype && !mayRefuse)
                    return;
                LBasicBlock asks = newColdBlock();
                LBasicBlock goesOn = newColdBlock();
                uint32_t bits = (mayConsultPrototype ? orderWordHasExtraProperties : 0) | (mayRefuse ? orderWordIsNotExtensible : 0);
                m_out.branch(hasNoBits(orderWordOf(cell), bits).value(), usually(goesOn), rarely(asks));
                m_out.appendTo(asks);
                if (mayConsultPrototype) {
                    LBasicBlock checksPrototypeChain = newColdBlock();
                    Word32 usesObjectLookup = hasPrototypeOfLayout(cell);
                    if (mayRefuse)
                        usesObjectLookup = logicalAnd(usesObjectLookup, hasNoBits(orderWordOf(cell), orderWordIsNotExtensible));
                    m_out.branch(usesObjectLookup.value(), unsure(goesOn), unsure(checksPrototypeChain));
                    m_out.appendTo(checksPrototypeChain);
                    setByName(box(cell), field.name, in<Boxed>(stored));
                    m_out.jump(done);
                } else if (mayAlreadyExist) {
                    OperationEffects effects { Writes::Fields, true, storeMayDespecializeObject(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, supertype) };
                    if (isInSloppyCode())
                        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByNameSloppy, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    else
                        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    m_out.jump(done);
                } else if (isInSloppyCode())
                    m_out.jump(done);
                else {
                    OperationEffects refusal { Writes::Nothing, true };
                    AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, refusal, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    m_out.unreachable();
                }
                m_out.appendTo(goesOn);
            };
            OperationEffects effects { Writes::Fields, false, false, true };
            if (Options::countAOTTypedAllocations()) {
                onCacheMiss();
                AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(field.name.index()));
            } else
                didCreateFieldInCell(cell, field, effects, onCacheMiss);
            m_out.jump(continuation);
        }
        m_out.appendTo(continuation);
    }
    put();
    if (field.isOutOfLine)
        ensureStillAliveHere(cell);
    if (holdsCells(field.store) && mayBeCell(stored))
        emitBarrierForLiteralString(cell, stored);
    if (isInitializing)
        didLowerInitializingStore();
    if (done) {
        m_out.jump(done);
        m_out.appendTo(done);
    }
}

const Field* Lowering::predictedFieldFor(const Type& expected, StringID name) const
{
    LayoutID supertype = expected.singleLayout();
    if (!supertype || !isObjectLayout(m_program.at(supertype).kind) || m_program.at(supertype).test.neverPasses())
        return nullptr;
    const Field* field = fieldFor(expected, name);
    if (!field || field->isOutOfLine || (field->isMissingInSomeSubtype && field->subtypesWithField.neverPasses()))
        return nullptr;
    return field;
}

CellPointer Lowering::withFieldOrElse(ValueID object, LayoutID supertype, const Field& field, LBasicBlock otherwise, bool mayBeAbsent)
{
    CellPointer cell = get(object).as<CellPointer>();
    if (field.isMissingInSomeSubtype)
        orElse(passes(shapeWordOf(cell), field.subtypesWithField, otherwise), otherwise);
    else if (m_program.at(supertype).acceptsUntypedObjects)
        orElse(passes(shapeWordOf(cell), m_program.at(supertype).test, otherwise), otherwise);
    if (!mayBeAbsent && m_program.at(field.stored).primitives & Primitive::Absent)
        orElse(hasField(cell, field), otherwise);
    return cell;
}

bool Lowering::hasSubtypeWithField(LayoutID supertype, StringID name) const
{
    for (LayoutID subtype : m_program.at(supertype).subtypes) {
        if (m_program.at(subtype).fieldNamed(name))
            return true;
    }
    return false;
}

bool Lowering::hasSubtypeKeepingField(LayoutID supertype, StringID name) const
{
    for (LayoutID subtype : m_program.at(supertype).subtypes) {
        auto& layout = m_program.at(subtype);
        if (auto field = layout.fieldNamed(name); field && layout.fields[*field].flags & fieldIsImmutable)
            return true;
    }
    return false;
}

static bool isUnescapedAllocation(const Program& program, const Function& function, const Block& block, size_t index, ValueID allocation, size_t unescapedUpToIndex)
{
    if (function.at(allocation).opcode != Opcode::NewObjectWithShape)
        return false;
    auto& layout = program.at(function.at(allocation).layout());
    if (!layout.mayDespecialize)
        return true;
    auto isAllocation = [&](ValueID id) {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        return id == allocation;
    };
    auto doesNotEscape = [&](const Value& value) {
        switch (value.opcode) {
        case Opcode::Cast:
            return value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce;
        case Opcode::GetField:
            return true;
        case Opcode::GetById:
        case Opcode::InById:
            return layout.fieldNamed(value.string()) && !mayInheritAtCreation(program, function, allocation, value.string());
        case Opcode::PutField:
            return !isAllocation(value.child(1));
        case Opcode::PutById: {
            auto field = layout.fieldNamed(value.string());
            if (!field || *field == layout.discriminant || isAllocation(value.child(1)) || mayInheritAtCreation(program, function, allocation, value.string()))
                return false;
            TypeID type = layout.fields[*field].type;
            return !type || program.typeOf(function, value.child(1)).isSubsetOf(program.at(type), program);
        }
        default:
            return false;
        }
    };
    for (size_t i = index; i-- > unescapedUpToIndex;) {
        if (block.values[i] == allocation)
            return true;
        auto& value = function.at(block.values[i]);
        bool usesAllocation = false;
        value.forEachChild([&](ValueID child) {
            usesAllocation |= isAllocation(child);
        });
        if (usesAllocation && !doesNotEscape(value))
            return false;
    }
    return !!unescapedUpToIndex;
}

bool Lowering::isStillUnescaped(ValueID allocation) const
{
    bool isKnown = m_unescapedAllocationBlock == m_currentBlock && m_unescapedAllocation == allocation && m_unescapedUpToIndex <= m_currentIndexInBlock;
    if (!isUnescapedAllocation(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, allocation, isKnown ? m_unescapedUpToIndex : 0))
        return false;
    m_unescapedAllocationBlock = m_currentBlock;
    m_unescapedAllocation = allocation;
    m_unescapedUpToIndex = m_currentIndexInBlock;
    return true;
}

bool Lowering::isAllocatedAsSubtypeOf(ValueID object, LayoutID supertype) const
{
    while (m_function.at(object).opcode == Opcode::Cast)
        object = m_function.at(object).child(0);
    auto& created = m_function.at(object);
    return created.opcode == Opcode::NewObjectWithShape && std::ranges::binary_search(m_program.at(supertype).subtypes, created.layout()) && isStillUnescaped(object);
}

static ValueID withoutCasts(const Function& function, ValueID id)
{
    while (function.at(id).opcode == Opcode::Cast)
        id = function.at(id).child(0);
    return id;
}

Operand Lowering::lowerGetField(ValueID result, const Value& value)
{
    RELEASE_ASSERT_WITH_MESSAGE(!mayDespecialize(value.layout()) || isStillUnescaped(withoutCasts(m_function, value.child(0))), "AOT: GetField on an allocation that may have despecialized");
    return readField(in<CellPointer>(value.child(0)), m_program.at(value.layout()).fields[value.field()], typeOf(result));
}

void Lowering::lowerPutField(ValueID, const Value& value)
{
    RELEASE_ASSERT_WITH_MESSAGE(!mayDespecialize(value.layout()) || isStillUnescaped(withoutCasts(m_function, value.child(0))), "AOT: PutField on an allocation that may have despecialized");
    writeField(in<CellPointer>(value.child(0)), value.child(0), m_program.at(value.layout()).fields[value.field()], value.child(1));
}

static bool areSingleEvaluation(const Program& program, const Vector<LayoutID>& classes)
{
    for (LayoutID id : classes) {
        if (!program.at(id).isEvaluatedOnce)
            return false;
    }
    return true;
}

std::optional<unsigned> Lowering::iteratorResultSlot(ValueID object, StringID name)
{
    if (!(m_function.declaredAs & functionIsAsync) || Options::auditAOTTypedAssertions())
        return std::nullopt;
    bool isValue = equalSpans(m_program.string(name), asBytes("value"_s.span8()));
    if (!isValue && !equalSpans(m_program.string(name), asBytes("done"_s.span8())))
        return std::nullopt;
    for (ValueID user : usersOf(object)) {
        auto& use = m_function.at(user);
        if (use.opcode != Opcode::CallBuiltin)
            continue;
        auto& implementation = m_builtins.at(use.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::RequireIteratorResultObject)
            return static_cast<unsigned>(isValue ? iteratorResultObjectValuePropertyOffset : iteratorResultObjectDonePropertyOffset);
    }
    return std::nullopt;
}

Operand Lowering::lowerGetById(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    if (!m_fusedCasts && (fusedCastCount(m_currentIndexInBlock) || fusedUseIndex(m_currentIndexInBlock, 0)))
        return placeholder(value.rep);
    auto& type = m_fusedCasts ? m_fusedCasts->type : typeOf(result);
    if (value.isAtKnownSlot())
        return ensure(getAtKnownSlot(value, in<Boxed>(object)), type);
    LayoutID supertype = receiver.singleLayout();
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        if (auto targets = supertype ? methodTargets(supertype, name) : std::nullopt; targets && targets->size() == 1 && targets->first().function && !targets->first().fieldCheckedFirst && m_program.genericEntrypoints.contains(targets->first().function.index()) && areSingleEvaluation(m_program, m_program.at(supertype).subtypes)) {
            CellPointer cell = nonNullish(object, name);
            auto queryObject = [&] {
                return getByIdAndCheckType(box(cell), name, type);
            };
            return ifSubtype(cell, object, supertype, repOf(type), [&] {
                return eitherOperand(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), repOf(type), [&] {
                    return ensure(closureConstant(targets->first().function), m_program.at(m_program.types.function), type);
                }, [&] {
                    return getFromObjectWithExtraProperties(cell, name, type);
                });
            }, queryObject);
        }
        if (supertype && isOnlyInExtraProperties(m_program, supertype, name)) {
            CellPointer cell = nonNullish(object, name);
            return eitherOperand(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), joinRepFor(type), [&] {
                return ensure(undefined(), m_program.at(m_program.types.undefined), type);
            }, [&] {
                return getFromObjectWithExtraProperties(cell, name, type);
            });
        }
        auto genericPath = [&] {
            if (auto property = getGlobalThisProperty(object, name))
                return ensure(*property, type);
            if (auto member = getGlobalMember(object, name))
                return ensure(*member, type);
            return getByIdAndCheckType(in<Boxed>(object), name, type, definingFunctionOf(object));
        };
        if (auto place = iteratorResultSlot(object, name)) {
            Boxed step = in<Boxed>(object);
            LBasicBlock isCellCase = m_out.newBlock();
            LBasicBlock isIteratorResult = m_out.newBlock();
            LBasicBlock otherwise = newColdBlock();
            OperandJoin join(*this, joinRepFor(type));
            m_out.branch(isCell(step).value(), usually(isCellCase), rarely(otherwise));
            m_out.appendTo(isCellCase);
            CellPointer cell = asCell(step);
            m_out.branch(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(changing32(Instance::offsetOfIteratorResultStructureID()))).value(), usually(isIteratorResult), rarely(otherwise));
            m_out.appendTo(isIteratorResult);
            join.add(ensure(Boxed(m_out.load64(TypedPointer(m_heaps.JSCellHeaderAndNamedProperties, m_out.add(cell.value(), m_out.constIntPtr(JSObject::offsetOfInlineStorage() + *place * sizeof(EncodedJSValue)))))), type));
            m_out.appendTo(otherwise);
            join.add(genericPath());
            return join.finish();
        }
        if (static const BuiltinName length = Builtins::singleton().nameOf(asBytes("length"_s.span8())); length && m_program.builtinName(name) == length && receiver == Type::function()) {
            CellPointer closure = get(object).rep == Rep::JSValue ? asCell(in<Boxed>(object)) : in<CellPointer>(object);
            return eitherOperand(isUnreifiedSlimClosure(closure), repOf(type), [&] {
                LValue data = fixedPointer(Instance::offsetOfTypedData());
                LValue lengths = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, functionLengthsOffset))), IsFixed)));
                LValue function = m_out.zeroExtPtr(closureFunction(closure, true).value());
                if (m_program.functionLengthsAreBytes)
                    return ensure(Word32(marked(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, m_out.add(lengths, function), 0)), IsFixed)), Type::integer({ 0, UINT8_MAX }), type);
                LValue address = m_out.add(lengths, m_out.shl(function, m_out.constInt32(1)));
                return ensure(Word32(marked(m_out.load16ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, address, 0)), IsFixed)), Type::integer({ 0, UINT16_MAX }), type);
            }, genericPath);
        }
        if (receiver == Type::function() && equalSpans(m_program.string(name), asBytes("name"_s.span8()))) {
            CellPointer closure = get(object).rep == Rep::JSValue ? asCell(in<Boxed>(object)) : in<CellPointer>(object);
            return eitherOperand(isUnreifiedSlimClosure(closure), repOf(type), [&] {
                LValue data = fixedPointer(Instance::offsetOfTypedData());
                LValue names = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, functionsOffset))), IsFixed)));
                LValue address = m_out.add(names, m_out.shl(m_out.zeroExtPtr(closureFunction(closure, true).value()), m_out.constInt32(2)));
                LValue key = m_out.zeroExtPtr(m_out.add(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, address, 0)), IsFixed), m_out.constInt32(1)));
                return ensure(CellPointer(callAllocationStub(Stub::TypedString, { { key, lazyConstantKeyGPR } }, { Writes::RuntimeState, false }, false)), m_program.at(m_program.types.string), type);
            }, genericPath);
        }
        if (value.isOnSlowPath)
            return genericPath();
        if (auto expectedObject = predictionOf(object, Primitive::All)) {
            if (const Field* expectedField = predictedFieldFor(*expectedObject, name)) {
                return ifPredictionHolds({ { object, &*expectedObject } }, repOf(type), [&](LBasicBlock otherwise) {
                    return readField(withFieldOrElse(object, expectedObject->singleLayout(), *expectedField, otherwise), *expectedField, type);
                }, genericPath);
            }
        }
        auto expected = predictionOf(object);
        if (!expected) {
            if (auto read = getByNameForFollowingCast(result, value))
                return *read;
            return genericPath();
        }
        if (auto builtin = predictedBuiltin(BuiltinKind::Getter, object, name, { })) {
            size_t numberOfAssumptions = m_assumptions.size();
            B3::Variable* isKnownPresent = nullptr;
            if (m_fusedCasts) {
                m_assumptions.append(Assumption { result, placeholder(repOf(type)), &type });
                if (auto element = elementReadAfterLengthCheck(object)) {
                    isKnownPresent = m_proc.addVariable(Int32);
                    m_elementsKnownPresent.add(element->index(), isKnownPresent);
                    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), isKnownPresent, m_out.constInt32(1));
                }
            }
            Operand returnedValue = lowerPredictedBuiltin(result, *builtin, std::nullopt, [&] {
                if (isKnownPresent)
                    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), isKnownPresent, m_out.constInt32(0));
                return genericPath();
            });
            m_assumptions.shrink(numberOfAssumptions);
            return returnedValue;
        }
        if (BuiltinName known = m_program.builtinName(name); known && (expected->isString() || expected->isNumber())) {
            BuiltinID method = m_builtins.findForInstance(BuiltinKind::Method, expected->isString() ? m_builtins.stringClass() : m_builtins.numberClass(), known);
            if (method && m_builtins.hasCell(method)) {
                return ifPredictionHolds({ { object, &m_program.at(expected->isString() ? m_program.types.string : m_program.types.number) } }, repOf(type), [&](LBasicBlock) {
                    return ensure(builtinCell(m_builtins.cellOf(method)), m_program.at(m_program.types.function), type);
                }, genericPath);
            }
        }
        return genericPath();
    }
    CellPointer cell = nonNullish(object, name);
    if (value.isOnSlowPath && m_program.at(supertype).acceptsUntypedObjects && !sunkCoercionAt(m_currentIndexInBlock)) {
        if (&m_program.at(value.type) == &type) {
            if (auto held = getByIdWithTypeCheck(box(cell), name, value.type))
                return *held;
        }
        return ensureProperty(cell, name, emitGetById(box(cell), name), type);
    }
    SetForScope knownTypeCell(m_knownTypeCell, cell.value());
    SetForScope knownCellType(m_knownCellType, supertype ? &m_program.at(m_program.at(supertype).type) : nullptr);
    Vector<ValueID, 4> stringsByIndex;
    if (field->isPinnedInEverySubtype && !m_fusedCasts && isOnlyComparedWithStringsBeforeStores(result, stringsByIndex)) {
        PinnedRead read { cell, shapeWordOf(cell), Word32(m_out.load8ZeroExt32(pinnedFieldsGuard(field->slot))), supertype, name, { }, { } };
        if (!stringsByIndex.isEmpty()) {
            auto findIndexWhere = [&](auto holds) {
                LValue index = m_out.constInt32(stringsByIndex.size());
                for (size_t i = stringsByIndex.size(); i--;)
                    index = m_out.select(holds(stringsByIndex[i]).value(), m_out.constInt32(i), index);
                return Word32(index);
            };
            Word32 index = either<Word32>(isZero(read.isModified), [&] {
                return findIndexWhere([&](ValueID text) {
                    return hasShapePinnedTo(read, m_function.at(text).string());
                });
            }, [&] {
                return findIndexWhere([&](ValueID text) {
                    return isPinnedTo(read, text);
                });
            });
            read.index = index;
            for (ValueID text : stringsByIndex)
                read.texts.append(m_function.at(text).string());
        }
        m_pinnedReads.set(result.index(), WTF::move(read));
        return placeholder(repOf(type));
    }
    auto has = [&] {
        return readProperty(cell, supertype, *field, type);
    };
    auto lacks = [&] {
        bool prototypeLacksProperty = prototypeChainLacksProperty(m_program, supertype, name);
        if (!prototypeLacksProperty && !prototypeChainLacksProperty(m_program, supertype, name, true)) {
            if (&m_program.at(value.type) == &type) {
                if (auto held = getByIdWithTypeCheck(box(cell), name, value.type))
                    return *held;
            }
            return ensureProperty(cell, name, emitGetById(box(cell), name), type);
        }
        if (field->hasOtherOffsetInSomeSubtype) {
            auto ask = [&] {
                SetForScope checksExtraPropertyResult(m_checksExtraPropertyResult, true);
                return ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index() | nameMayBeOwnField)), type);
            };
            return dispatchOnSubtypeField(cell, supertype, *field, joinRepFor(type), [&](LayoutID, const Field& own) {
                if (!(m_program.at(own.stored).primitives & Primitive::Absent))
                    return readField(cell, own, type);
                return eitherOperand(hasField(cell, own), joinRepFor(type), [&] {
                    return readField(cell, own, type);
                }, ask);
            }, ask);
        }
        return eitherOperand(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), joinRepFor(type), [&] {
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }, [&] {
            if (!prototypeLacksProperty)
                return getFromObjectWithExtraProperties(cell, name, type);
            SetForScope checksExtraPropertyResult(m_checksExtraPropertyResult, true);
            return ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index())), type);
        });
    };
    auto sunk = sunkCoercionAt(m_currentIndexInBlock);
    if (field->hasSlotInEverySubtype && prototypeChainLacksProperty(m_program, supertype, name, true)) {
        bool prototypeLacksProperty = prototypeChainLacksProperty(m_program, supertype, name);
        LBasicBlock again = m_out.newBlock();
        LBasicBlock asks = newColdBlock();
        OperandJoin join(*this, joinRepFor(type));
        ValueFromBlock atFirst = m_out.anchor(m_out.int32Zero);
        m_out.jump(again);
        m_out.appendTo(again);
        LValue isExecuted = m_out.phi(Int32, atFirst);
        auto continueWithAbsentSlot = [&] {
            if (sunk) {
                LBasicBlock isSource = newColdBlock();
                LBasicBlock shouldLower = newColdBlock();
                LBasicBlock isNotSource = m_out.newBlock();
                branchOnTransitionSource(cell, *sunk, rarely(isSource), usually(isNotSource));
                m_out.appendTo(isSource);
                m_out.branch(isExecuted, unsure(isNotSource), unsure(shouldLower));
                m_out.appendTo(shouldLower);
                carryOut(cell, *sunk);
                m_out.addIncomingToPhi(isExecuted, m_out.anchor(m_out.int32One));
                m_out.jump(again);
                m_out.appendTo(isNotSource);
            }
            LBasicBlock hasNoExtraProperties = m_out.newBlock();
            m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(hasNoExtraProperties), rarely(asks));
            m_out.appendTo(hasNoExtraProperties);
        };
        if (field->store == Store::Int32OrNumberBox && m_program.at(field->stored).primitives & Primitive::Absent) {
            ScopedLambda<void()> absentSlotHandler(continueWithAbsentSlot);
            SetForScope<const ScopedLambda<void()>*> absentBlock(m_absentSlotHandler, &absentSlotHandler);
            join.add(readField(cell, *field, type));
            RELEASE_ASSERT(!m_absentSlotHandler);
        } else {
            LBasicBlock isPresent = m_out.newBlock();
            LBasicBlock isMissing = m_out.newBlock();
            m_out.branch(hasField(cell, *field).value(), unsure(isPresent), unsure(isMissing));
            m_out.appendTo(isPresent);
            {
                SetForScope fieldIsPresent(m_fieldIsPresent, true);
                join.add(readField(cell, *field, type));
            }
            m_out.appendTo(isMissing);
            continueWithAbsentSlot();
            join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        }
        m_out.appendTo(asks);
        if (prototypeLacksProperty) {
            SetForScope checksExtraPropertyResult(m_checksExtraPropertyResult, true);
            join.add(ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index())), type));
        } else
            join.add(getFromObjectWithExtraProperties(cell, name, type));
        return join.finish();
    }
    if (!sunk && field->isMissingInSomeSubtype && !field->hasOtherOffsetInSomeSubtype && !field->subtypesWithField.neverPasses()
        && m_program.at(field->stored).primitives & Primitive::Absent && mayInherit(m_program, supertype, name) && !inheritsThroughLayoutPrototype(m_program, supertype, name)
        && !prototypeChainLacksProperty(m_program, supertype, name) && prototypeChainLacksProperty(m_program, supertype, name, true)) {
        LBasicBlock hasProperty = m_out.newBlock();
        LBasicBlock isPresent = m_out.newBlock();
        LBasicBlock isMissing = newUnlikelyBlock();
        LBasicBlock hasNoExtraProperties = m_out.newBlock();
        LBasicBlock asks = newColdBlock();
        OperandJoin join(*this, joinRepFor(type));
        m_out.branch(passes(shapeWordOf(cell), field->subtypesWithField, isMissing).value(), unsure(hasProperty), unsure(isMissing));
        m_out.appendTo(hasProperty);
        m_out.branch(hasField(cell, *field).value(), unsure(isPresent), unsure(isMissing));
        m_out.appendTo(isPresent);
        {
            SetForScope fieldIsPresent(m_fieldIsPresent, true);
            join.add(readField(cell, *field, type));
        }
        m_out.appendTo(isMissing);
        m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(hasNoExtraProperties), rarely(asks));
        m_out.appendTo(hasNoExtraProperties);
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        m_out.appendTo(asks);
        join.add(getFromObjectWithExtraProperties(cell, name, type));
        return join.finish();
    }
    if (!sunk)
        return accessThroughSupertype(cell, object, supertype, *field, joinRepFor(type), has, lacks);
    RELEASE_ASSERT(field->isMissingInSomeSubtype && !field->subtypesWithField.neverPasses());
    LBasicBlock hasProperty = m_out.newBlock();
    LBasicBlock mayBeMissingField = newUnlikelyBlock();
    LBasicBlock isMissingField = m_out.newBlock();
    LBasicBlock isSource = newColdBlock();
    OperandJoin join(*this, joinRepFor(type));
    m_out.branch(passes(shapeWordOf(cell), field->subtypesWithField, mayBeMissingField).value(), unsure(hasProperty), unsure(mayBeMissingField));
    m_out.appendTo(mayBeMissingField);
    branchOnTransitionSource(cell, *sunk, rarely(isSource), usually(isMissingField));
    m_out.appendTo(isSource);
    carryOut(cell, *sunk);
    m_out.branch(passes(shapeWordOf(cell), field->subtypesWithField, isMissingField).value(), unsure(hasProperty), unsure(isMissingField));
    m_out.appendTo(hasProperty);
    join.add(has());
    m_out.appendTo(isMissingField);
    join.add(lacks());
    return join.finish();
}

std::optional<Lowering::InitialStores> Lowering::initialStoresAt(size_t index) const
{
    auto& values = m_currentBlock->values;
    auto& store = m_function.at(values[index]);
    if (!store.isInitialStore() || m_isLoweringInitialStoresOneByOne)
        return std::nullopt;
    auto& constructor = store.inlineFrame ? m_program.at(m_function.inlineFrames[store.inlineFrame].callee) : m_function;
    if (store.initialStoreIndex() >= constructor.numberOfInitialStores)
        return std::nullopt;
    RELEASE_ASSERT(store.initialStoreIndex() <= index);
    InitialStores result { index - store.initialStoreIndex(), constructor.numberOfInitialStores, constructor.orderWordAfterInitialStores, &m_program.at(constructor.thisType) };
    RELEASE_ASSERT(result.first + result.count <= values.size());
    for (unsigned i = 0; i < result.count; ++i) {
        auto& other = m_function.at(values[result.first + i]);
        RELEASE_ASSERT(other.opcode == Opcode::PutById && other.initialStoreIndex() == i && other.inlineFrame == store.inlineFrame && other.child(0) == store.child(0));
    }
    if (isInitializingStore(m_program, m_function, *m_currentBlock, result.first)) {
        auto& allocation = m_program.allocations[m_function.at(store.child(0)).allocation()];
        bool isInitializedWithAll = true;
        for (unsigned i = 0; i < result.count; ++i)
            isInitializedWithAll &= isInitializedWith(allocation, m_function.at(values[result.first + i]).string());
        if (isInitializedWithAll)
            return std::nullopt;
    }
    return result;
}

void Lowering::lowerInitialStores(const InitialStores& stores)
{
    auto& values = m_currentBlock->values;
    ValueID object = m_function.at(values[stores.first]).child(0);
    CellPointer cell = nonNullish(object, m_function.at(values[stores.first]).string());
    LBasicBlock isFresh = m_out.newBlock();
    LBasicBlock isNotFresh = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    if (LayoutID supertype = stores.thisType->singleLayout(); m_program.at(supertype).acceptsUntypedObjects && !isAllocatedAsSubtypeOf(object, supertype))
        orElse(passes(shapeWordOf(cell), m_program.at(supertype).test, isNotFresh), isNotFresh);
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    m_out.branch(isZero(Word32(m_out.castToInt32(words))).value(), usually(isFresh), rarely(isNotFresh));

    m_out.appendTo(isFresh);
    {
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
        didEmitWrite(Writes::Fields);
        bool storesCells = false;
        for (unsigned i = 0; i < stores.count; ++i) {
            SetForScope currentIndex(m_currentIndexInBlock, stores.first + i);
            auto& value = m_function.at(values[stores.first + i]);
            const Field* field = fieldFor(*stores.thisType, value.string());
            RELEASE_ASSERT(field && !field->isMissingInSomeSubtype);
            bool needsNoStorage = storeNeedsNoStorage(*field, value.child(1));
            guardPinnedFields(stores.thisType->singleLayout(), *field, value.child(1));
            if (Options::countAOTTypedAllocations()) [[unlikely]] {
                OperationEffects effects { Writes::Fields, false, false, true };
                AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(value.string().index() | (needsNoStorage ? nameIsFieldCreatedWithoutStorage : 0)));
            }
            if (needsNoStorage)
                continue;
            if (field->store == Store::Int32OrNumberBox)
                storeInt32OrNumberBox(cell, *field, value.child(1), true);
            else
                store(field->store, in(value.child(1), repOf(field->store)), fieldAt(cell, *field));
            storesCells |= holdsCells(field->store) && mayBeCell(value.child(1));
        }
        if (!Options::countAOTTypedAllocations()) [[likely]]
            m_out.store64(m_out.bitOr(words, m_out.constInt64(stores.orderWord)), cell.value(), m_heaps.AOTTypedObject_orderWord);
        if (storesCells)
            barrier(cell);
    }
    m_out.jump(continuation);

    m_out.appendTo(isNotFresh);
    {
        SetForScope oneByOne(m_isLoweringInitialStoresOneByOne, true);
        m_out.setFrequency(0);
        for (unsigned i = 0; i < stores.count; ++i) {
            SetForScope currentIndex(m_currentIndexInBlock, stores.first + i);
            lowerPutById(values[stores.first + i], m_function.at(values[stores.first + i]));
        }
        m_out.setFrequency(1);
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Lowering::SpreadLiteral* Lowering::spreadLiteralAt(size_t index)
{
    static constexpr unsigned maxMembers = 32;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations()) [[unlikely]]
        return nullptr;
    if (m_spreadLiteralsBlock != m_currentBlock) {
        m_spreadLiteralsBlock = m_currentBlock;
        m_spreadLiterals.clear();
        auto& values = m_currentBlock->values;
        auto isSpread = [&](const Value& value) {
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = m_builtins.at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
        };
        auto hasOrdinaryName = [&](const Value& field) {
            auto& name = m_function.at(field.arguments[1]);
            if (name.opcode != Opcode::String)
                return false;
            auto text = m_program.string(name.string());
            return !text.empty() && !isASCIIDigit(text[0]) && !isPrivateName(text) && !equalSpans(text, asBytes("__proto__"_s.span8()));
        };
        bool isBlockWithHandler = m_currentBlock->terminator.kind == TerminatorKind::Invoke;
        for (size_t i = 0; i < values.size(); ++i) {
            ValueID object = values[i];
            if (m_function.at(object).opcode != Opcode::NewObject || m_function.at(object).hasInitialStructure())
                continue;
            SpreadLiteral literal { i, { } };
            unsigned numberOfSpreads = 0;
            bool isCheckedAgainstRecordType = false;
            for (size_t k = i + 1; k < values.size(); ++k) {
                auto& value = m_function.at(values[k]);
                bool usesObject = false;
                value.forEachChild([&](ValueID child) {
                    usesObject |= child == object;
                });
                if (!usesObject) {
                    bool dependsOnNothing = value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String
                        || value.opcode == Opcode::NewArray || value.opcode == Opcode::NewClosure || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
                    if (numberOfSpreads && !dependsOnNothing)
                        break;
                    continue;
                }
                bool hasRoom = literal.members.size() < maxMembers;
                if (hasRoom && isDefineField(value) && value.arguments.size() == 3 && value.arguments[0] == object && value.arguments[1] != object && value.arguments[2] != object && hasOrdinaryName(value)) {
                    literal.members.append(k);
                    continue;
                }
                if (hasRoom && isSpread(value) && value.arguments.size() == 2 && value.arguments[0] == object && value.arguments[1] != object && !useCountOf(values[k])
                    && numberOfSpreads < Instance::SpreadLiteralPlan::maxSources && keyOrdersExpectedOf(value.arguments[1]).isEmpty()) {
                    ++numberOfSpreads;
                    literal.members.append(k);
                    continue;
                }
                isCheckedAgainstRecordType = value.opcode == Opcode::Cast && value.castReason() != CastReason::Upcast;
                break;
            }
            if (!numberOfSpreads || literal.members.size() < 2 || isCheckedAgainstRecordType)
                continue;
            bool isCopyThenFields = numberOfSpreads == 1 && isSpread(m_function.at(values[literal.members[0]]));
            if (isCopyThenFields)
                continue;
            if (isBlockWithHandler && (literal.members.last() + 1 != values.size() || !isUnreachableFromHandler(object)))
                continue;
            m_spreadLiterals.append(WTF::move(literal));
        }
    }
    for (auto& literal : m_spreadLiterals) {
        if (literal.allocation == index || literal.members.contains(index))
            return &literal;
    }
    return nullptr;
}

void Lowering::lowerSpreadLiteral(const SpreadLiteral& literal)
{
    auto& block = m_currentBlock->values;
    ValueID object = block[literal.allocation];
    unsigned count = literal.members.size();
    unsigned more = 0;
    for (size_t k = literal.members.last() + 1; k < block.size(); ++k) {
        auto& value = m_function.at(block[k]);
        more += isDefineField(value) && !value.arguments.isEmpty() && value.arguments[0] == object;
    }
    if (m_currentBlock->terminator.kind == TerminatorKind::Invoke) {
        for (ValueID next : m_function.at(m_currentBlock->terminator.targets[0].block).values) {
            auto& value = m_function.at(next);
            more += isDefineField(value) && !value.arguments.isEmpty() && value.arguments[0] == object;
        }
    }
    Vector<uint32_t, 16> row { count | std::min(more, 255u) << 16 };
    Vector<Boxed, 16> values;
    Vector<Boxed, 16> fields;
    const Value* firstSpread = nullptr;
    for (size_t index : literal.members) {
        auto& member = m_function.at(block[index]);
        bool isField = isDefineField(member);
        row.append(isField ? m_function.at(member.arguments[1]).string().index() : Instance::SpreadLiteralPlan::isSpread);
        (isField ? fields : values).append(in<Boxed>(member.arguments[isField ? 2 : 1]));
        if (!isField && !firstSpread)
            firstSpread = &member;
    }
    unsigned numberOfSources = values.size();
    values.appendVector(fields);
    Vector<uint32_t, 16> items;
    {
        SetForScope currentIndex(m_currentIndexInBlock, m_currentIndexInBlock);
        for (size_t index : literal.members) {
            m_currentIndexInBlock = index;
            items.append(nextItemSite());
        }
    }
    LValue buffer = transientBuffer(count * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < count; ++i)
        m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    uint32_t callSite = m_nextSite;
    CellPointer created = AOT_TYPED_OPERATION_WITH_EFFECTS(NewObjectWithSpreads, builtinCallEffects(*firstSpread), word32(rowOf(row.span())), word32(numberOfSources), Pointer(buffer));
    RELEASE_ASSERT(m_nextSite == callSite + 1);
    for (unsigned i = 0; i < count; ++i)
        m_context.itemSites.append({ callSite, i, items[i] });
    defineDeferredAllocation(object, created);
}

bool Lowering::isAccessorDefinition(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Operation && (implementation.operation == LanguageOperation::DefineGetter || implementation.operation == LanguageOperation::DefineSetter);
}

Lowering::AccessorGroup* Lowering::accessorGroupAt(size_t index)
{
    static constexpr unsigned maxMembers = 64;
    if (m_accessorGroupsBlock != m_currentBlock) {
        m_accessorGroupsBlock = m_currentBlock;
        m_accessorGroups.clear();
        auto& values = m_currentBlock->values;
        AccessorGroup open;
        auto close = [&] {
            if (open.members.size() >= 2)
                m_accessorGroups.append(open);
            open.members.clear();
        };
        auto mayBeGroupMember = [&](const Value& value) {
            if (value.arguments.size() != 5 || typeOf(value.arguments[0]).primitives || !typeOf(value.arguments[0]).hasObjects())
                return false;
            auto& key = m_function.at(value.arguments[1]);
            auto& position = m_function.at(value.arguments[4]);
            return key.opcode == Opcode::String && !isPrivateName(m_program.string(key.string())) && m_function.at(value.arguments[3]).opcode == Opcode::Boolean
                && position.opcode == Opcode::Integer && position.integer() >= 0 && position.integer() <= std::numeric_limits<int32_t>::max();
        };
        for (size_t i = 0; i < values.size(); ++i) {
            auto& value = m_function.at(values[i]);
            if (isAccessorDefinition(value)) {
                if (!mayBeGroupMember(value)) {
                    close();
                    continue;
                }
                if (!open.members.isEmpty() && (open.members.size() == maxMembers || m_function.at(values[open.members[0]]).arguments[0] != value.arguments[0]))
                    close();
                open.members.append(i);
                continue;
            }
            bool maySeparate = value.opcode == Opcode::String || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::NewClosure || value.opcode == Opcode::FunctionReference
                || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
            if (!maySeparate)
                close();
        }
        close();
    }
    for (auto& group : m_accessorGroups) {
        if (group.members.contains(index))
            return &group;
    }
    return nullptr;
}

void Lowering::lowerMemberOfAccessorGroup(AccessorGroup& group, const Value& value)
{
    unsigned count = group.members.size();
    if (!group.buffer)
        group.buffer = m_out.lockedStackSlot(4 * count * sizeof(EncodedJSValue));
    size_t place = group.members.find(m_currentIndexInBlock);
    auto store = [&](unsigned word, Boxed boxed) {
        m_out.store64(boxed.value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, group.buffer, (4 * place + word) * sizeof(EncodedJSValue)));
    };
    bool isGetter = m_builtins.at(value.builtin()).implementation.operation == LanguageOperation::DefineGetter;
    store(0, boxedConstant(jsNumber(static_cast<int32_t>(m_function.at(value.arguments[1]).string().index()))));
    store(1, in<Boxed>(value.arguments[2]));
    store(2, boxedConstant(jsNumber(static_cast<int32_t>(isGetter | m_function.at(value.arguments[3]).boolean() << 1))));
    store(3, boxedConstant(jsNumber(static_cast<int32_t>(m_function.at(value.arguments[4]).integer()))));
    if (place + 1 == count)
        AOT_TYPED_OPERATION_WITH_EFFECTS(DefineAccessors, builtinCallEffects(value), in<CellPointer>(value.arguments[0]), word32(count), Pointer(group.buffer));
}

bool Lowering::isDefineField(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
}

bool Lowering::isPrivateFieldAdd(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::PrivateFieldAdd;
}

bool Lowering::isObjectCreateNull(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || m_function.at(value.arguments[0]).opcode != Opcode::Null)
        return false;
    auto& builtin = m_builtins.at(value.builtin());
    return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
}

bool Lowering::isDefineProperty(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3)
        return false;
    auto& builtin = m_builtins.at(value.builtin());
    return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
}

std::optional<size_t> Lowering::definePropertyUsingDescriptor(const ObjectLiteral& literal)
{
    auto& values = m_currentBlock->values;
    if (literal.hasNullPrototype || literal.newTarget || literal.fields.isEmpty())
        return std::nullopt;
    ValueID id = values[literal.allocation];
    bool isData = false;
    bool isAccessor = false;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(values[index]);
        bool isStore = field.opcode == Opcode::PutById;
        if (isStore && (index != literal.fields.last() || (field.hasKnownSlotWord() && !field.isStoreIntoInitialStructure())))
            return std::nullopt;
        auto name = m_program.string(isStore ? field.string() : m_function.at(field.arguments[1]).string());
        bool isFlag = equalSpans(name, asBytes("writable"_s.span8())) || equalSpans(name, asBytes("enumerable"_s.span8())) || equalSpans(name, asBytes("configurable"_s.span8()));
        if (isFlag && m_function.at(isStore ? field.child(1) : field.arguments[2]).opcode != Opcode::Boolean)
            return std::nullopt;
        isData |= equalSpans(name, asBytes("value"_s.span8())) || equalSpans(name, asBytes("writable"_s.span8()));
        isAccessor |= equalSpans(name, asBytes("get"_s.span8())) || equalSpans(name, asBytes("set"_s.span8()));
        if (!isFlag && !equalSpans(name, asBytes("value"_s.span8())) && !equalSpans(name, asBytes("get"_s.span8())) && !equalSpans(name, asBytes("set"_s.span8())))
            return std::nullopt;
    }
    if (isData && isAccessor)
        return std::nullopt;
    for (size_t k = literal.fields.last() + 1; k < values.size(); ++k) {
        auto& value = m_function.at(values[k]);
        if (value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String)
            continue;
        bool isWatched = m_currentBlock->terminator.kind == TerminatorKind::Invoke && k + 1 == values.size();
        if (!isDefineProperty(value) || isWatched || hasGenericFallback(m_program, m_function, value) || value.arguments[2] != id || value.arguments[0] == id || value.arguments[1] == id)
            return std::nullopt;
        return useCountOf(id) == literal.fields.size() + 1 ? std::optional(k) : std::nullopt;
    }
    return std::nullopt;
}

void Lowering::lowerPropertyDefinition(const ObjectLiteral& definition)
{
    auto& block = m_currentBlock->values;
    ValueID id = block[definition.allocation];
    auto& call = m_function.at(id);
    auto* descriptor = objectLiteralAt(*definition.descriptor);
    RELEASE_ASSERT(descriptor && descriptor->describes == definition.allocation);
    uint32_t literal = 0;
    Boxed valueOrGetter = boxedConstant(jsUndefined());
    Boxed setter = boxedConstant(jsUndefined());
    for (size_t index : descriptor->fields) {
        auto& field = m_function.at(block[index]);
        bool isStore = field.opcode == Opcode::PutById;
        auto name = m_program.string(isStore ? field.string() : m_function.at(field.arguments[1]).string());
        ValueID fieldValue = isStore ? field.child(1) : field.arguments[2];
        auto has = [&](uint32_t which) {
            literal |= which;
            if (isStore)
                literal |= (WTF::ctz(which) + 1) << DescriptorLiteral::storedFieldShift;
        };
        auto flag = [&](uint32_t which, uint32_t is) {
            has(which);
            literal |= m_function.at(fieldValue).boolean() ? is : 0;
        };
        if (equalSpans(name, asBytes("writable"_s.span8())))
            flag(DescriptorLiteral::hasWritable, DescriptorLiteral::isWritable);
        else if (equalSpans(name, asBytes("enumerable"_s.span8())))
            flag(DescriptorLiteral::hasEnumerable, DescriptorLiteral::isEnumerable);
        else if (equalSpans(name, asBytes("configurable"_s.span8())))
            flag(DescriptorLiteral::hasConfigurable, DescriptorLiteral::isConfigurable);
        else if (equalSpans(name, asBytes("set"_s.span8()))) {
            has(DescriptorLiteral::hasSet);
            setter = in<Boxed>(fieldValue);
        } else {
            has(equalSpans(name, asBytes("get"_s.span8())) ? DescriptorLiteral::hasGet : DescriptorLiteral::hasValue);
            valueOrGetter = in<Boxed>(fieldValue);
        }
    }
    Boxed object = in<Boxed>(call.arguments[0]);
    OperationEffects effects = builtinCallEffects(call);
    auto slot = [&]() -> std::optional<unsigned> {
        auto& key = m_function.at(call.arguments[1]);
        if (key.opcode != Opcode::String || !(literal & DescriptorLiteral::hasValue))
            return std::nullopt;
        auto text = m_program.string(key.string());
        bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
            return isASCIIDigit(character);
        });
        return mayBeIndex || isPrivateName(text) || literal >> DescriptorLiteral::storedFieldShift ? std::nullopt : allocateSite(key.string(), literal);
    }();
    if (slot)
        callICStub(Stub::DefineOwnProperty, Void, { { object.value(), firstStubOperandGPR }, { valueOrGetter.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effects, std::pair { *slot, GPRInfo::argumentGPR2 });
    else
        AOT_TYPED_OPERATION_WITH_EFFECTS(DefineOwnProperty, effects, object, in<Boxed>(call.arguments[1]), valueOrGetter, setter, word32(literal));
    define(id, ensure(object, typeOf(id)));
}

uint32_t maxFunctionSlots()
{
    return std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots);
}

Lowering::ObjectLiteral* Lowering::objectLiteralAt(size_t index)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return nullptr;
    if (m_objectLiteralsBlock != m_currentBlock) {
        m_objectLiteralsBlock = m_currentBlock;
        m_objectLiterals.clear();
        m_objectLiteralAtIndex.clear();
        m_objectLiteralIndexIsComplete = false;
        auto& values = m_currentBlock->values;
        for (size_t i = 0; i < values.size(); ++i) {
            bool hasNullPrototype = isObjectCreateNull(m_function.at(values[i]));
            bool isCreateThis = m_function.at(values[i]).opcode == Opcode::CreateThis && (!m_function.at(values[i]).child(1) || m_function.at(m_function.at(values[i]).child(1)).opcode == Opcode::FunctionReference);
            if ((m_function.at(values[i]).opcode != Opcode::NewObject && !hasNullPrototype && !isCreateThis) || isAllocatedByFirstUse(values[i]) || spreadLiteralAt(i))
                continue;
            ObjectLiteral literal { i, hasNullPrototype, { } };
            if (isCreateThis)
                literal.newTarget = m_function.at(values[i]).child(0);
                literal.constructor = m_function.at(values[i]).child(1) ? m_function.at(values[i]).child(1) : literal.newTarget;
            literal.fields = fieldsInitializedWithAllocation(m_program, m_function, *m_currentBlock, i, isCreateThis, hasNullPrototype);
            if (auto call = definePropertyUsingDescriptor(literal)) {
                literal.describes = call;
                ObjectLiteral definition { *call, false, { } };
                definition.descriptor = i;
                m_objectLiterals.append(WTF::move(literal));
                m_objectLiterals.append(WTF::move(definition));
                continue;
            }
            if (!isCreateThis)
                findStructureDescriptorOf(literal);
            if (literal.row != Format::none) {
                m_objectLiterals.append(WTF::move(literal));
                continue;
            }
            if (auto reserved = m_completeAllocationSlots.find(values[i].index()); reserved != m_completeAllocationSlots.end()) {
                literal.slot = reserved->value;
                m_objectLiterals.append(WTF::move(literal));
                continue;
            }
            unsigned numberOfSlots = isCreateThis ? 3 : 2;
            if (m_context.sites.size() + numberOfSlots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
                continue;
            literal.slot = m_context.sites.size();
            for (unsigned k = 0; k < numberOfSlots; ++k)
                m_context.sites.append(Site { });
            m_objectLiterals.append(WTF::move(literal));
        }
        if (m_objectLiterals.size() > 16) {
            for (unsigned k = 0; k < m_objectLiterals.size(); ++k) {
                m_objectLiteralAtIndex.add(m_objectLiterals[k].allocation, k);
                for (size_t field : m_objectLiterals[k].fields)
                    m_objectLiteralAtIndex.add(field, k);
            }
            m_objectLiteralIndexIsComplete = true;
        }
    }
    if (m_objectLiteralIndexIsComplete) {
        auto found = m_objectLiteralAtIndex.find(index);
        return found == m_objectLiteralAtIndex.end() ? nullptr : &m_objectLiterals[found->value];
    }
    for (auto& literal : m_objectLiterals) {
        if (literal.allocation == index || literal.fields.contains(index))
            return &literal;
    }
    return nullptr;
}

uint32_t Lowering::structureDescriptorFor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const
{
    if (names.size() > maxNamedPropertiesPerSite)
        return Format::none;
    for (size_t i = 0; i < names.size(); ++i) {
        auto text = m_program.string(names[i]);
        if ((!text.empty() && isASCIIDigit(text[0])) || std::ranges::any_of(names.first(i), [&](StringID earlier) { return earlier == names[i]; }))
            return Format::none;
    }
    return m_program.findStructureDescriptor(names, inlineCapacity, hasNullPrototype);
}

void Lowering::findStructureDescriptorOf(ObjectLiteral& literal) const
{
    auto& block = m_currentBlock->values;
    ValueID id = block[literal.allocation];
    Vector<StringID, 8> names;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(block[index]);
        names.append(field.opcode == Opcode::PutById ? field.string() : m_function.at(field.arguments[1]).string());
    }
    if (m_function.at(id).hasInitialStructure()) {
        auto& row = m_program.initialStructures[m_function.at(id).initialStructure()];
        if (row.isWhole && names.size() < row.firstSlot)
            return;
        for (size_t i = names.size() - row.firstSlot; row.isWhole && i < row.names.size(); ++i)
            names.append(row.names[i]);
    }
    uint32_t descriptorFromPass = m_function.at(id).initialStructureDescriptor();
    literal.inlineCapacity = descriptorFromPass != Format::none ? m_program.structureDescriptors[descriptorFromPass].inlineCapacity : inlineCapacityForLiteral(m_program, m_function, id, !literal.fields.isEmpty(), usersOfAllocation(id));
    literal.row = structureDescriptorFor(names.span(), literal.inlineCapacity, literal.hasNullPrototype);
}

CellPointer Lowering::newObjectWithProperties(unsigned slot, bool hasNullPrototype, ValueID newTargetID, ValueID constructorID, unsigned inlineCapacity, std::span<const StringID> names, std::span<const Boxed> values, uint64_t stores, const ObjectLiteral* group, uint32_t row)
{
    RELEASE_ASSERT(!stores || (newTargetID && group));
    unsigned count = names.size();
    RELEASE_ASSERT(count == values.size());
    if (row != Format::none) {
        m_literalPath = 'r';
        RELEASE_ASSERT(!newTargetID && structureDescriptorFor(names, inlineCapacity, hasNullPrototype) == row);
        unsigned inRegisters = std::min(count, maxLiteralPropertiesInRegisters);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { m_out.constInt64(Instance::numberOfReservedDescriptorIndices + row), GPRInfo::argumentGPR1 } };
        for (unsigned i = 0; i < inRegisters; ++i)
            operands.append({ values[i].value(), GPRInfo::toArgumentRegister(2 + i) });
        LValue created = callICStub(static_cast<Stub>(static_cast<unsigned>(Stub::NewObjectFromDescriptor0) + inRegisters), Int64, operands, std::nullopt, { Writes::RuntimeState, false });
        for (unsigned i = inRegisters; i < count; ++i)
            m_out.store64(values[i].value(), m_out.address(m_heaps.properties.atAnyNumber(), created, JSObject::offsetOfInlineStorage() + i * sizeof(EncodedJSValue)));
        if (count > inRegisters)
            fenceAfterInitialization();
        return CellPointer(created);
    }
    Pointer cache = slotAddress(slot);
    auto cellOf = [&](ValueID id) {
        Operand operand = get(id);
        return operand.rep == Rep::JSValue ? asCell(operand.as<Boxed>()) : operand.as<CellPointer>();
    };
    auto newTarget = [&] { return cellOf(newTargetID); };
    auto constructor = [&] { return cellOf(constructorID); };
    auto oneByOne = [&] {
        auto indexInBlock = m_currentIndexInBlock;
        m_currentIndexInBlock = group->allocation;
        CellPointer object = AOT_TYPED_OPERATION(CreateThisWithCapacity, constructor(), newTarget(), word32(inlineCapacity));
        LValue buffer = m_out.lockedStackSlot(count * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < count; ++i) {
            m_currentIndexInBlock = group->fields[i];
            if (stores >> i & 1)
                emitPutById(box(object), names[i], values[i]);
            else if (!emitDefineByName(box(object), names[i], values[i]))
                AOT_TYPED_OPERATION(DefineByName, box(object), word32(names[i].index()), values[i]);
            m_out.store64(integer(names[i].index() | (stores >> i & 1) << 32).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        }
        m_currentIndexInBlock = indexInBlock;
        AOT_TYPED_OPERATION(DidStoreToThis, cache, constructor(), newTarget(), object, word32(count), word32(inlineCapacity), Pointer(buffer));
        return object;
    };
    auto emitIndividualStores = [&] {
        SetForScope currentIndex(m_currentIndexInBlock, group->allocation);
        CellPointer object = createThis(constructor(), newTarget());
        for (unsigned i = 0; i < count; ++i) {
            m_currentIndexInBlock = group->fields[i];
            if (stores >> i & 1)
                emitPutById(box(object), names[i], values[i]);
            else if (!emitDefineByName(box(object), names[i], values[i]))
                AOT_TYPED_OPERATION(DefineByName, box(object), word32(names[i].index()), values[i]);
        }
        return object;
    };
    auto genericPath = [&] {
        if (stores && std::ranges::none_of(names, [&](StringID name) { return isNamedLastIndex(name); })) {
            bool mayEncounterAccessor = false;
            for (unsigned i = 0; i < count && (m_isInLoop || !m_program.runsOnce(m_id)); ++i)
                mayEncounterAccessor |= (stores >> i & 1) && m_program.possibleAccessorNames.get(names[i].index());
            LBasicBlock cacheIsExhausted = mayEncounterAccessor ? newColdBlock() : nullptr;
            LBasicBlock mayPopulateCache = mayEncounterAccessor ? newColdBlock() : nullptr;
            LBasicBlock isConstructed = mayEncounterAccessor ? newColdBlock() : nullptr;
            if (mayEncounterAccessor) {
                LValue attempts = m_out.bitAnd(m_out.load32(TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(OBJECT_OFFSETOF(AOT::Slot, offset))))), m_out.constInt32(AOT::Slot::attemptsMask));
                m_out.branch(m_out.equal(attempts, m_out.constInt32(AOT::Slot::attemptsMask)), unsure(cacheIsExhausted), unsure(mayPopulateCache));
                m_out.appendTo(mayPopulateCache);
            }
            CellPointer constructorCell = constructor();
            CellPointer newTargetCell = newTarget();
            LValue buffer = transientBuffer(count * sizeof(EncodedJSValue));
            Vector<uint32_t, 16> row;
            Vector<uint32_t, 16> items;
            SetForScope currentIndex(m_currentIndexInBlock, m_currentIndexInBlock);
            for (unsigned i = 0; i < count; ++i) {
                RELEASE_ASSERT(!(names[i].index() >> 31));
                m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
                row.append(names[i].index() | static_cast<uint32_t>(stores >> i & 1) << 31);
                m_currentIndexInBlock = group->fields[i];
                items.append(nextItemSite());
            }
            m_currentIndexInBlock = group->allocation;
            uint32_t callSite = m_nextSite;
            CellPointer created = AOT_TYPED_OPERATION(CreateThisWithStores, cache, constructorCell, newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer), word32(rowOf(row.span())));
            RELEASE_ASSERT(m_nextSite == callSite + 1);
            for (unsigned i = 0; i < count; ++i)
                m_context.itemSites.append({ callSite, i, items[i] });
            if (!mayEncounterAccessor)
                return created;
            ValueFromBlock viaRuntime = m_out.anchor(created.value());
            m_out.jump(isConstructed);
            m_out.appendTo(cacheIsExhausted);
            ValueFromBlock inlinePath = m_out.anchor(emitIndividualStores().value());
            m_out.jump(isConstructed);
            m_out.appendTo(isConstructed);
            return CellPointer(m_out.phi(Int64, viaRuntime, inlinePath));
        }
        if (stores) {
            CellPointer constructorCell = constructor();
            CellPointer newTargetCell = newTarget();
            LValue buffer = transientBuffer(2 * count * sizeof(EncodedJSValue));
            for (unsigned i = 0; i < count; ++i) {
                m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
                m_out.store64(integer(names[i].index() | (stores >> i & 1) << 32).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, (count + i) * sizeof(EncodedJSValue)));
            }
            CellPointer created = AOT_TYPED_OPERATION(TryCreateThisWithStores, cache, constructorCell, newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer));
            LBasicBlock isCreated = newColdBlock();
            LBasicBlock isNotCreated = newColdBlock();
            LBasicBlock done = newColdBlock();
            m_out.branch(m_out.notNull(created.value()), usually(isCreated), rarely(isNotCreated));
            m_out.appendTo(isCreated);
            ValueFromBlock atOnce = m_out.anchor(created.value());
            m_out.jump(done);
            m_out.appendTo(isNotCreated);
            ValueFromBlock bySteps = m_out.anchor(oneByOne().value());
            m_out.jump(done);
            m_out.appendTo(done);
            return CellPointer(m_out.phi(Int64, atOnce, bySteps));
        }
        std::optional<CellPointer> constructorCell;
        std::optional<CellPointer> newTargetCell;
        if (newTargetID) {
            constructorCell = constructor();
            newTargetCell = newTarget();
        }
        LValue buffer = transientBuffer(std::max(count, 1u) * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < count; ++i)
            m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        Word32 row = word32(rowOfNames(names));
        if (!newTargetID)
            return AOT_TYPED_OPERATION(NewObjectLiteral, cache, word32(count), word32(hasNullPrototype | inlineCapacity << 1), Pointer(buffer), row);
        return AOT_TYPED_OPERATION(CreateThisWithFields, cache, *constructorCell, *newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer), row);
    };
    auto bySlot = [&](unsigned given) {
        unsigned inRegisters = std::min(given, maxLiteralPropertiesInRegisters);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { cache.value(), GPRInfo::argumentGPR1 } };
        for (unsigned i = 0; i < inRegisters; ++i)
            operands.append({ values[i].value(), GPRInfo::toArgumentRegister(2 + i) });
        LValue created = callICStub(static_cast<Stub>(static_cast<unsigned>(Stub::NewObjectLiteral0) + inRegisters), Int64, operands, std::nullopt, { Writes::RuntimeState, false });
        bool hasStoredAfterStub = false;
        for (unsigned i = inRegisters; i < count; ++i) {
            if (values[i].value()->hasInt64() && !values[i].value()->asInt64())
                continue;
            m_out.store64(values[i].value(), m_out.address(m_heaps.properties.atAnyNumber(), created, JSObject::offsetOfInlineStorage() + i * sizeof(EncodedJSValue)));
            hasStoredAfterStub = true;
        }
        if (hasStoredAfterStub)
            fenceAfterInitialization();
        return created;
    };
    bool mayBeIndex = std::ranges::any_of(names, [&](StringID name) {
        auto text = m_program.string(name);
        return !text.empty() && isASCIIDigit(text[0]);
    });
    bool areDistinct = true;
    for (unsigned i = 0; i < count; ++i)
        areDistinct = areDistinct && std::ranges::none_of(names.first(i), [&](StringID earlier) { return earlier == names[i]; });
    m_literalPath = 'g';
    if (!newTargetID && count <= maxNamedPropertiesPerSite && inlineCapacity < 1u << 15 && !mayBeIndex && areDistinct) {
        m_literalPath = 's';
        Vector<uint32_t, 16> row { count | inlineCapacity << 16 | static_cast<uint32_t>(hasNullPrototype) << 31 };
        for (StringID name : names)
            row.append(name.index());
        row.append(m_program.findStructureDescriptor(names, inlineCapacity, hasNullPrototype) + 1);
        m_context.slotPlanOffsets.append({ slot + 1, rowOf(row.span()) });
        unsigned given = count;
        while (given && values[given - 1].value()->hasInt64() && !values[given - 1].value()->asInt64())
            --given;
        return CellPointer(bySlot(given));
    }
    if (m_program.runsOnce(m_id) && !m_isInLoop)
        return genericPath();
    LBasicBlock isFilled = m_out.newBlock();
    LBasicBlock isEmpty = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID));
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LValue isValid = m_out.notZero32(m_out.load32(inCache(0)));
    if (newTargetID) {
        isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load32(inCache(2 * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer))), loadRuntimeState32(Instance::offsetOfTypedPrototypeEpoch())));
        if (m_function.at(newTargetID).opcode != Opcode::FunctionReference)
            isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer))), newTarget().value()));
    }
    m_out.branch(isValid, usually(isFilled), rarely(isEmpty));
    m_out.appendTo(isFilled);
    ValueFromBlock fast = m_out.anchor(bySlot(count));
    m_out.jump(continuation);
    m_out.appendTo(isEmpty);
    ValueFromBlock slow = m_out.anchor(genericPath().value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

uint32_t Lowering::rowOfNames(std::span<const StringID> names)
{
    Vector<uint32_t, 16> row;
    for (StringID name : names)
        row.append(name.index());
    return rowOf(row.span());
}

uint32_t Lowering::rowOf(std::span<const uint32_t> row)
{
    auto& plans = m_context.plans;
    for (size_t start = 0; start + row.size() <= plans.size(); ++start) {
        if (std::ranges::equal(plans.span().subspan(start, row.size()), row))
            return start;
    }
    uint32_t start = plans.size();
    plans.append(row);
    return start;
}

CellPointer Lowering::newObjectLiteral(std::span<const StringID> names, std::span<const Boxed> values)
{
    if (m_context.sites.size() + 2 > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
        CellPointer object = newEmptyObject(names.size());
        for (unsigned i = 0; i < names.size(); ++i)
            AOT_TYPED_OPERATION(DefineByName, box(object), word32(names[i].index()), values[i]);
        return object;
    }
    unsigned slot = m_context.sites.size();
    m_context.sites.append(Site { });
    m_context.sites.append(Site { });
    return newObjectWithProperties(slot, false, { }, { }, roundInlineCapacityToCellSize(names.size()), names, values);
}

bool Lowering::isPredictedSymbol(const Function& function, ValueID id) const
{
    TypeID prediction = function.at(id).prediction();
    auto& type = prediction ? m_program.at(prediction) : m_program.typeOf(function, id);
    return type.primitives & Primitive::Symbol && type.hasOnlyPrimitives(Primitive::Symbol | Primitive::UndefinedOrNull);
}

static size_t cellSizeInCompiledProgram(size_t size)
{
    static LazyNeverDestroyed<Vector<size_t>> sizeClasses;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        sizeClasses.construct(MarkedSpace::sizeClassesFor(Options::sizeClassProgressionWithAOT()));
    });
    auto sizeClass = std::ranges::lower_bound(sizeClasses.get(), size);
    return sizeClass == sizeClasses->end() ? WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size) : *sizeClass;
}

unsigned Lowering::roundInlineCapacityToCellSize(unsigned inlineCapacity)
{
    if (!inlineCapacity)
        return 0;
    size_t size = cellSizeInCompiledProgram(JSFinalObject::allocationSize(inlineCapacity));
    return std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
}

static unsigned inlineCapacityToBecomeRecord(const Program& program, std::span<const StringID> names)
{
    const Vector<LayoutID, 1>* records = nullptr;
    for (StringID name : names) {
        auto found = program.recordLayoutsByFieldName.find(name.index() + 1);
        if (found == program.recordLayoutsByFieldName.end())
            return 0;
        if (!records || found->value.size() < records->size())
            records = &found->value;
    }
    if (!records)
        return 0;
    size_t size = JSFinalObject::allocationSize(0);
    for (LayoutID record : *records) {
        auto& layout = program.at(record);
        if (layout.sizeWithSpecializationCapacity <= size)
            continue;
        bool hasFieldForEachName = std::ranges::all_of(names, [&](StringID name) {
            return layout.fields.containsIf([&](auto& field) {
                return field.name == name;
            });
        });
        bool hasNameForEachRequiredField = std::ranges::all_of(layout.fields, [&](auto& field) {
            return field.flags & (fieldIsOptional | fieldIsNeverStored) || std::ranges::find(names, field.name) != names.end();
        });
        if (hasFieldForEachName && hasNameForEachRequiredField)
            size = layout.sizeWithSpecializationCapacity;
    }
    return WTF::roundUpToMultipleOf<sizeof(EncodedJSValue)>(size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
}

const UsersInBlocks* Lowering::usersOfAllocation(ValueID allocationSite) const
{
    if (!std::exchange(m_didFindAllocationUsers, true)) {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::NewObject || value.opcode == Opcode::CreateThis || isObjectCreateNull(value))
                    m_isAllocation.set(id.index());
            }
        }
        for (unsigned block = 0; block < m_function.blocks.size() && !m_isAllocation.isEmpty(); ++block) {
            for (ValueID user : m_function.blocks[block].values) {
                m_function.at(user).forEachChild([&](ValueID child) {
                    child = withoutUpcasts(child);
                    if (!m_isAllocation.get(child.index()))
                        return;
                    auto& users = m_allocationUsers.add(child.index(), UsersInBlocks { }).iterator->value;
                    if (users.isEmpty() || users.last().value != user)
                        users.append({ block, user });
                });
            }
        }
    }
    if (!m_isAllocation.get(allocationSite.index()))
        return nullptr;
    auto found = m_allocationUsers.find(allocationSite.index());
    return found == m_allocationUsers.end() ? &m_noUsers : &found->value;
}

unsigned Lowering::inlineCapacityForLiteral(const Program& program, const Function& function, ValueID id, bool hasFields, const UsersInBlocks* users)
{
    auto& builtins = Builtins::singleton();
    auto withoutUpcasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast && function.at(value).castReason() == CastReason::Upcast)
            value = function.at(value).child(0);
        return value;
    };
    auto populatesFirstArgument = [&](const Value& user) {
        if (user.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = builtins.at(user.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign)
            return true;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
    };
    Vector<StringID, 16> names;
    unsigned numberOfKeys = 0;
    collectAssignedProperties(program, function, id, 3, names, numberOfKeys, users);
    unsigned inlineCapacity = std::min<unsigned>(names.size() + numberOfKeys, JSFinalObject::maxInlineCapacity);
    if (inlineCapacity <= JSFinalObject::defaultInlineCapacity) {
        bool isStoredToByKey = false;
        bool isCopiedInto = false;
        auto look = [&](ValueID user) {
            auto& value = function.at(user);
            isStoredToByKey |= value.opcode == Opcode::PutByVal && withoutUpcasts(value.child(0)) == id;
            isCopiedInto |= populatesFirstArgument(value) && !value.arguments.isEmpty() && withoutUpcasts(value.arguments[0]) == id;
        };
        for (size_t i = 0; users && i < users->size(); ++i)
            look(users->at(i).value);
        for (size_t i = 0; !users && i < function.blocks.size(); ++i) {
            for (ValueID user : function.blocks[i].values)
                look(user);
        }
        if (isCopiedInto || (isStoredToByKey && !hasFields))
            inlineCapacity = 0;
    }
    if (auto& allocationSite = function.at(id); allocationSite.hasInitialStructure() && !program.initialStructures[allocationSite.initialStructure()].isWhole) {
        auto& row = program.initialStructures[allocationSite.initialStructure()];
        inlineCapacity = std::max<unsigned>(inlineCapacity, row.firstSlot + row.names.size());
    }
    if (inlineCapacity)
        inlineCapacity = std::max(inlineCapacity, inlineCapacityToBecomeRecord(program, names.span()));
    return roundInlineCapacityToCellSize(inlineCapacity);
}

void Lowering::assignStructureDescriptors(Program& program)
{
    static constexpr OpcodeSet literals { Opcode::NewObject };
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        std::as_const(program).functions.forEachValue(i, literals, [&](ValueID id) {
            program.functions.atKeepingValueIndex(i).at(id).immediates[1] = Format::none;
        });
    });
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Record || !layout.shape || layout.acceptsUncheckedObjects() || layout.sizeWithSpecializationCapacity <= JSFinalObject::allocationSize(2) || layout.sizeWithSpecializationCapacity > JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity))
            continue;
        for (auto& field : layout.fields)
            program.recordLayoutsByFieldName.add(field.name.index() + 1, Vector<LayoutID, 1>()).iterator->value.append(LayoutID(i));
    }
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return;
    auto& builtins = Builtins::singleton();
    struct AllocationSite {
        ValueID id;
        unsigned room;
        bool hasNullPrototype;
        unsigned initialNameCount;
        Vector<StringID, 8> names;
        Vector<unsigned> storeBlocks;
        uint32_t descriptor;
    };
    Vector<Vector<AllocationSite>> allocationsByFunctionIndex(program.functions.size());
    const UsersInBlocks noUsers;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet allocationSites { Opcode::NewObject, Opcode::CallBuiltin };
        if (!program.functions.mayContain(i, allocationSites))
            return;
        auto& function = std::as_const(program).functions[i];
        auto createsNullPrototypeObject = [&](const Value& value) {
            if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::Null)
                return false;
            auto& builtin = builtins.at(value.builtin());
            return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
        };
        BitVector isCreation;
        std::as_const(program).functions.forEachIndexedValue(i, allocationSites, [&](ValueID id) {
            if (function.at(id).opcode == Opcode::NewObject || createsNullPrototypeObject(function.at(id)))
                isCreation.set(id.index());
        });
        if (isCreation.isEmpty())
            return;
        UncheckedKeyHashMap<uint32_t, UsersInBlocks, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> allocationUsers;
        for (unsigned block = 0; block < function.blocks.size(); ++block) {
            for (ValueID user : function.blocks[block].values) {
                function.at(user).forEachChild([&](ValueID child) {
                    while (function.at(child).opcode == Opcode::Cast && function.at(child).castReason() == CastReason::Upcast)
                        child = function.at(child).child(0);
                    if (!isCreation.get(child.index()))
                        return;
                    auto& users = allocationUsers.add(child.index(), UsersInBlocks { }).iterator->value;
                    if (users.isEmpty() || users.last().value != user)
                        users.append({ block, user });
                });
            }
        }
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                auto& value = function.at(id);
                bool hasNullPrototype = createsNullPrototypeObject(value);
                if (value.opcode != Opcode::NewObject && !hasNullPrototype)
                    continue;
                auto found = allocationUsers.find(id.index());
                const UsersInBlocks& users = found == allocationUsers.end() ? noUsers : found->value;
                auto fields = fieldsInitializedWithAllocation(program, function, block, index, false, hasNullPrototype);
                Vector<StringID, 8> names;
                for (size_t field : fields) {
                    auto& member = function.at(block.values[field]);
                    names.append(member.opcode == Opcode::PutById ? member.string() : function.at(member.arguments[1]).string());
                }
                if (value.hasInitialStructure()) {
                    auto& row = program.initialStructures[value.initialStructure()];
                    for (size_t i = names.size() - std::min<size_t>(names.size(), row.firstSlot); row.isWhole && names.size() >= row.firstSlot && i < row.names.size(); ++i)
                        names.append(row.names[i]);
                }
                bool isNamedBySite = names.size() <= maxNamedPropertiesPerSite;
                for (size_t i = 0; isNamedBySite && i < names.size(); ++i) {
                    auto text = program.string(names[i]);
                    isNamedBySite = (text.empty() || !isASCIIDigit(text[0])) && names.find(names[i]) == i;
                }
                if (!isNamedBySite)
                    continue;
                unsigned room = inlineCapacityForLiteral(program, function, id, !fields.isEmpty(), &users);
                unsigned initialNameCount = names.size();
                size_t nameCapacity = std::max<size_t>(names.size(), room);
                Vector<unsigned> storeBlocks;
                for (size_t k = 0; k < users.size() && !hasNullPrototype; ++k) {
                    auto& store = function.at(users[k].value);
                    if (store.opcode != Opcode::PutById || store.isInitialStore() || store.hasKnownSlotWord() || names.size() >= std::min<size_t>(nameCapacity, maxNamedPropertiesPerSite) || names.contains(store.string()))
                        continue;
                    ValueID receiver = store.child(0);
                    while (function.at(receiver).opcode == Opcode::Cast && function.at(receiver).castReason() == CastReason::Upcast)
                        receiver = function.at(receiver).child(0);
                    auto text = program.string(store.string());
                    if (receiver != id || (!text.empty() && isASCIIDigit(text[0])) || equalSpans(text, asBytes("__proto__"_s.span8())))
                        continue;
                    names.append(store.string());
                    storeBlocks.append(users[k].block);
                }
                allocationsByFunctionIndex[i].append(AllocationSite { id, room, hasNullPrototype, initialNameCount, WTF::move(names), WTF::move(storeBlocks), Format::none });
            }
        }
    });
    unsigned allocationSites = 0;
    for (auto& allocationsInFunction : allocationsByFunctionIndex) {
        for (auto& allocationSite : allocationsInFunction) {
            ++allocationSites;
            uint32_t number = program.ensureStructureDescriptor(allocationSite.names.span().first(allocationSite.initialNameCount), allocationSite.room, allocationSite.hasNullPrototype);
            allocationSite.descriptor = number;
            if (!allocationSite.hasNullPrototype)
                program.structuresWithoutTransitions.set(number);
            unsigned block = UINT_MAX;
            for (unsigned k = 0; k < allocationSite.storeBlocks.size(); ++k) {
                if (allocationSite.storeBlocks[k] != block) {
                    if (program.structureDescriptors.size() > 60000)
                        break;
                    block = allocationSite.storeBlocks[k];
                }
                uint32_t grown = program.ensureStructureDescriptor(allocationSite.names.span().first(allocationSite.initialNameCount + k + 1), allocationSite.room, false);
                program.structureTransitions.append({ number, allocationSite.names[allocationSite.initialNameCount + k].index(), grown });
                number = grown;
            }
        }
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        for (auto& allocationSite : allocationsByFunctionIndex[i]) {
            if (!allocationSite.hasNullPrototype)
                program.functions.atKeepingValueIndex(i).values[allocationSite.id.index()].immediates[1] = allocationSite.descriptor;
        }
    });
    destroyOnHelperThread(WTF::move(allocationsByFunctionIndex));
    program.firstThisStructureDescriptor = program.structureDescriptors.size();
    for (auto& row : program.initialStructures) {
        bool isNumbered = row.isForThis && row.isWhole && row.names.size() <= maxNamedPropertiesPerSite;
        for (size_t i = 0; isNumbered && i < row.names.size(); ++i) {
            auto text = program.string(row.names[i]);
            isNumbered = text.empty() || !isASCIIDigit(text[0]);
        }
        if (isNumbered)
            row.number = program.ensureStructureDescriptor(row.names.span(), 0xff, false);
    }
    for (auto& grown : program.structureTransitions)
        program.structuresWithoutTransitions.clear(grown[0]);
    for (uint32_t number = 0; number < program.structureDescriptors.size(); ++number) {
        for (StringID name : program.structureDescriptors[number].names)
            program.structureDescriptorsByName.add(name.index() + 1, Vector<uint32_t, 1>()).iterator->value.append(number);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: structure descriptors: ", program.structureDescriptors.size(), " for ", allocationSites, " allocations; of them for this: ", program.structureDescriptors.size() - program.firstThisStructureDescriptor);
}

void assignStructureDescriptors(Program& program)
{
    Lowering::assignStructureDescriptors(program);
}

CellPointer Lowering::newEmptyObject(unsigned numberOfNames)
{
    if (numberOfNames <= JSFinalObject::defaultInlineCapacity)
        return AOT_TYPED_OPERATION(NewObject);
    return AOT_TYPED_OPERATION(NewObjectWithCapacity, word32(roundInlineCapacityToCellSize(std::min<unsigned>(numberOfNames, JSFinalObject::maxInlineCapacity))));
}

std::optional<InlineBuiltin> Lowering::enumeratingBuiltinOf(ValueID list, unsigned depth) const
{
    const Value* listing = &m_function.at(withoutCasts(m_function, list));
    if (listing->opcode == Opcode::Parameter) {
        if (depth == 2)
            return std::nullopt;
        ValueID parameter = withoutCasts(m_function, list);
        std::optional<InlineBuiltin> found;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                size_t place = m_function.at(target.block).parameters.find(parameter);
                if (place == notFound)
                    continue;
                if (place >= target.arguments.size())
                    return std::nullopt;
                auto arriving = enumeratingBuiltinOf(target.arguments[place], depth + 1);
                if (!arriving || (found && *found != *arriving))
                    return std::nullopt;
                found = arriving;
            }
        }
        return found;
    }
    if (listing->opcode == Opcode::CallDirect) {
        auto& callee = m_program.at(listing->function());
        if (callee.blocks.size() != 1 || callee.blocks[0].terminator.kind != TerminatorKind::Return || !callee.blocks[0].terminator.value)
            return std::nullopt;
        listing = &callee.at(callee.blocks[0].terminator.value);
    }
    if (listing->opcode == Opcode::Call && listing == &m_function.at(withoutCasts(m_function, list)))
        listing = &m_function.at(withoutCasts(m_function, listing->child(0)));
    if (listing->opcode != Opcode::CallBuiltin && listing->opcode != Opcode::BuiltinFunction)
        return std::nullopt;
    auto& implementation = m_builtins.at(listing->builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return std::nullopt;
    return implementation.inlined;
}

ValueID Lowering::listOfElementRead(ValueID element, bool mustBeFirstElement) const
{
    auto& value = m_function.at(withoutCasts(m_function, element));
    ValueID list;
    ValueID index = value.child(1);
    if (value.opcode == Opcode::GetByVal)
        list = value.child(0);
    else if (value.opcode == Opcode::IteratorValue && m_function.at(withoutCasts(m_function, value.child(0))).opcode == Opcode::IteratorOpen)
        list = m_function.at(withoutCasts(m_function, value.child(0))).child(0);
    else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 2) {
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || (implementation.inlined != InlineBuiltin::GetDoubleIndex && implementation.inlined != InlineBuiltin::GetContiguousIndex && implementation.inlined != InlineBuiltin::ArrayGetIndex))
            return { };
        list = value.arguments[0];
        index = value.arguments[1];
    } else
        return { };
    if (mustBeFirstElement) {
        auto& place = m_function.at(withoutUpcasts(index));
        if (place.opcode != Opcode::Integer || place.integer())
            return { };
    }
    return list;
}

bool Lowering::isEnumeratedKey(ValueID key, unsigned depth) const
{
    key = withoutUpcasts(key);
    if (m_function.at(key).opcode == Opcode::Parameter) {
        if (depth == 2)
            return false;
        bool isAnyEnumerated = false;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                size_t place = m_function.at(target.block).parameters.find(key);
                if (place == notFound)
                    continue;
                if (place >= target.arguments.size())
                    return false;
                ValueID arriving = target.arguments[place];
                if (m_function.at(withoutUpcasts(arriving)).opcode == Opcode::Undefined)
                    continue;
                if (!isEnumeratedKey(arriving, depth + 1))
                    return false;
                isAnyEnumerated = true;
            }
        }
        return isAnyEnumerated;
    }
    if (ValueID list = listOfElementRead(key, false)) {
        if (auto builtin = enumeratingBuiltinOf(list); builtin && (*builtin == InlineBuiltin::ForInKeys || *builtin == InlineBuiltin::ObjectKeys || *builtin == InlineBuiltin::ObjectGetOwnPropertyNames || *builtin == InlineBuiltin::ReflectOwnKeys))
            return true;
    }
    if (ValueID pair = listOfElementRead(key, true)) {
        if (ValueID list = listOfElementRead(pair, false))
            return enumeratingBuiltinOf(list) == InlineBuiltin::ObjectEntries;
    }
    return false;
}

bool Lowering::isFilledInLoopByComputedKey(ValueID object) const
{
    if (m_isInLoop)
        return false;
    for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
        if (!m_loopBlocks.get(i))
            continue;
        for (ValueID user : m_function.blocks[i].values) {
            auto& value = m_function.at(user);
            if (value.opcode == Opcode::PutByVal && withoutUpcasts(value.child(0)) == object && !typeOf(value.child(1)).isNumber() && !isEnumeratedKey(value.child(1)))
                return true;
        }
    }
    return false;
}

void Lowering::lowerObjectLiteral(const ObjectLiteral& literal)
{
    if (literal.describes)
        return;
    if (literal.descriptor)
        return lowerPropertyDefinition(literal);
    auto& block = m_currentBlock->values;
    ValueID id = block[literal.allocation];
    if (literal.fields.isEmpty() && !literal.newTarget && !m_function.at(id).hasInitialStructure() && isFilledInLoopByComputedKey(id)) {
        defineDeferredAllocation(id, countPlainAllocation(PlainAllocationKind::Object, id, AOT_TYPED_OPERATION(NewObjectForKeys, word32(literal.hasNullPrototype))));
        return;
    }
    Vector<StringID, 6> names;
    Vector<Boxed, 6> values;
    uint64_t stores = 0;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(block[index]);
        bool isStore = field.opcode == Opcode::PutById;
        if (isStore && literal.newTarget)
            stores |= 1ull << names.size();
        names.append(isStore ? field.string() : m_function.at(field.arguments[1]).string());
        values.append(in<Boxed>(isStore ? field.child(1) : field.arguments[2]));
    }
    if (Options::aotMapFilePath()) [[unlikely]] {
        unsigned operands = 0;
        for (size_t index : literal.fields) {
            auto& field = m_function.at(block[index]);
            switch (m_function.at(field.opcode == Opcode::PutById ? field.child(1) : field.arguments[2]).opcode) {
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::Boolean:
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::String:
            case Opcode::BigInt:
            case Opcode::FunctionReference:
                break;
            default:
                ++operands;
            }
        }
        m_context.literalsForMap.append('\t', block[m_currentIndexInBlock].index() + 1, '\t', literal.newTarget ? 't' : literal.hasNullPrototype ? 'n' : 'o', m_isInLoop ? "l"_s : ""_s, '\t', operands, '\t', names.isEmpty() ? "-"_s : ""_s);
        for (size_t i = 0; i < names.size(); ++i)
            m_context.literalsForMap.append(i ? ","_s : ""_s, names[i].index());
    }
    if (m_function.at(id).hasInitialStructure()) {
        auto& row = m_program.initialStructures[m_function.at(id).initialStructure()];
        bool isGapless = row.isWhole && names.size() >= row.firstSlot;
        for (size_t i = 0; isGapless && i < row.names.size(); ++i)
            isGapless = row.firstSlot + i < names.size() ? names[row.firstSlot + i] == row.names[i] : !names.contains(row.names[i]);
        if (!isGapless && row.isWhole)
            fail("an allocation marked as fully initialized is not"_s);
        for (size_t i = names.size() - row.firstSlot; isGapless && i < row.names.size(); ++i) {
            names.append(row.names[i]);
            values.append(Boxed(m_out.constInt64(0)));
        }
    }
    unsigned inlineCapacity = 0;
    if (literal.row != Format::none)
        inlineCapacity = literal.inlineCapacity;
    else if (!literal.newTarget)
        inlineCapacity = inlineCapacityForLiteral(m_program, m_function, id, !literal.fields.isEmpty(), usersOfAllocation(id));
    else {
        Vector<StringID, 16> names;
        unsigned numberOfKeys = 0;
        collectAssignedProperties(m_program, m_function, id, 3, names, numberOfKeys, usersOfAllocation(id));
        inlineCapacity = std::min<unsigned>(names.size() + numberOfKeys, JSFinalObject::maxInlineCapacity);
        if (!inlineCapacity)
            inlineCapacity = JSFinalObject::defaultInlineCapacity;
        inlineCapacity = roundInlineCapacityToCellSize(inlineCapacity);
    }
    defineDeferredAllocation(id, countPlainAllocation(literal.newTarget ? PlainAllocationKind::This : PlainAllocationKind::Object, id, newObjectWithProperties(literal.slot, literal.hasNullPrototype, literal.newTarget, literal.constructor, inlineCapacity, names.span(), values.span(), stores, &literal, literal.row)));
    if (Options::aotMapFilePath()) [[unlikely]]
        m_context.literalsForMap.append('\t', m_literalPath);
}

CellPointer Lowering::countPlainAllocation(PlainAllocationKind kind, ValueID id, CellPointer cell)
{
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        AOT_TYPED_OPERATION(CountPlainAllocation, cell, word32(m_id.index()), word32(static_cast<uint32_t>(kind) << plainAllocationKindShift | id.index()));
    return cell;
}

void Lowering::defineDeferredAllocation(ValueID allocation, CellPointer object)
{
    Rep rep = m_function.at(allocation).rep;
    RELEASE_ASSERT(rep == Rep::Cell || rep == Rep::JSValue);
    define(allocation, rep == Rep::Cell ? Operand(object) : Operand(box(object)));
}

Vector<LayoutID, 2> Lowering::inlineClonedLayouts(const Type& type) const
{
    auto isClonedInline = [&](LayoutID id) {
        auto& layout = m_program.at(id);
        if (layout.cloneAllocation == Format::none || layout.hasExtension || !layout.shape)
            return false;
        if (prefersGenericPathInColdCode() && std::ranges::any_of(layout.fields, [](auto& field) { return field.flags & fieldHoldsLazyClosure; }))
            return false;
        auto& allocation = m_program.allocations[layout.cloneAllocation];
        return allocation.size == layout.sizeWithSpecializationCapacity && !allocation.numberOfLiterals;
    };
    Vector<LayoutID, 2> result;
    for (LayoutID supertype : type.objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (isClonedInline(subtype) && !result.contains(subtype))
                result.append(subtype);
        }
    }
    if (result.size() <= 2)
        return result;
    result.removeAllMatching([&](LayoutID id) {
        return !type.objectLayouts.contains(id);
    });
    if (result.size() > 2)
        result.clear();
    return result;
}

bool Lowering::allSubtypesAreIn(const Type& type, const Vector<LayoutID, 2>& layouts)
{
    if (Options::validateAOTTypedFacts())
        return false;
    auto& shapes = possibleShapesOf(type);
    return !shapes.isEmpty() && std::ranges::all_of(shapes, [&](uint32_t shape) {
        return layouts.containsIf([&](LayoutID id) {
            return m_program.at(id).shape == shape;
        });
    });
}

void Lowering::branchOnSourceLayout(Word32 shapeWord, const Layout& layout, bool isLastCandidate, LBasicBlock otherwise)
{
    if (isLastCandidate)
        return;
    LBasicBlock hasLayout = m_out.newBlock();
    m_out.branch(equal(shapeWord, word32(Typed::shapeWordOf(layout.shape))).value(), usually(hasLayout), rarely(otherwise));
    m_out.appendTo(hasLayout);
}

void Lowering::unlessBoxed(CellPointer cell, const Layout& layout, LBasicBlock otherwise)
{
    for (auto& field : layout.fields) {
        if (field.store == Store::Int32OrNumberBox && !(field.flags & fieldIsNeverStored))
            orElse(logicalNot(refersToNumberBox(load(field.store, fieldAt(cell, field), 0).as<Word32>())), otherwise);
    }
}

CellPointer Lowering::copyRecord(CellPointer cell, Word32 orderWord, const Layout& layout, const Vector<StringID, 4>& overwritten)
{
    for (auto& field : layout.fields) {
        if (field.flags & fieldHoldsLazyClosure && !(field.flags & fieldIsNeverStored) && !overwritten.contains(field.name))
            materializeLazyClosureIfNeeded(cell, field);
    }
    if (unsigned fieldBytes = WTF::roundUpToMultipleOf<16>(layout.size) - offsetOfFirstField)
        return allocateObjectCopy(layout.cloneAllocation, cell, fieldBytes);
    CellPointer copy = allocate(layout.cloneAllocation);
    storeField(copy, m_heaps.AOTTypedObject_orderWord, bitAnd(orderWord, word32(orderWordHasExtraProperties - 1)));
    for (auto& field : layout.fields) {
        if (field.flags & fieldIsNeverStored || overwritten.contains(field.name))
            continue;
        TypedPointer from = fieldAt(cell, field);
        TypedPointer to = fieldAt(copy, field);
        switch (sizeOf(field.store)) {
        case 1:
            m_out.store32As8(m_out.load8ZeroExt32(from), to);
            break;
        case 2:
            m_out.store32As16(m_out.load16ZeroExt32(from), to);
            break;
        case 4:
            m_out.store32(m_out.load32(from), to);
            break;
        case 8:
            m_out.store64(m_out.load64(from), to);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    ensureStillAliveHere(cell);
    return copy;
}

std::optional<Lowering::MergeGroup> Lowering::findMergeGroup(size_t firstMemberIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    if (firstMemberIndex + 2 >= values.size())
        return std::nullopt;
    auto& first = m_function.at(values[firstMemberIndex]);
    auto& second = m_function.at(values[firstMemberIndex + 1]);
    auto& cast = m_function.at(values[firstMemberIndex + 2]);
    bool startsWithClone = m_builtins.at(first.builtin()).implementation.operation == LanguageOperation::CloneObject;
    ValueID object = startsWithClone ? values[firstMemberIndex] : first.arguments[0];
    ValueID firstSource = first.arguments[startsWithClone ? 0 : 1];
    bool isSpread = second.opcode == Opcode::CallBuiltin && m_builtins.at(second.builtin()).implementation.kind == BuiltinImplementation::Operation && m_builtins.at(second.builtin()).implementation.operation == LanguageOperation::CopyDataProperties;
    if (!isSpread || second.arguments.size() != 2 || second.arguments[0] != object || second.arguments[1] == object || firstSource == object)
        return std::nullopt;
    if (cast.opcode != Opcode::Cast || cast.castReason() != CastReason::As || cast.child(0) != object || useCountOf(object) != (startsWithClone ? 2 : 3))
        return std::nullopt;
    LayoutID supertype = typeOf(values[firstMemberIndex + 2]).singleLayout();
    if (!supertype || m_program.at(supertype).kind != LayoutKind::Record || !m_program.at(supertype).shape || !m_program.at(supertype).subtypes.contains(supertype))
        return std::nullopt;
    if (!typeOf(firstSource).singleLayout() || (!typeOf(second.arguments[1]).singleLayout() && (startsWithClone || !typeOf(second.arguments[1]).anyObject)))
        return std::nullopt;
    if (startsWithClone && typeOf(firstSource).singleLayout() != typeOf(second.arguments[1]).singleLayout())
        return std::nullopt;
    return MergeGroup { firstMemberIndex, supertype, startsWithClone };
}

Operand Lowering::lowerMergeGroup(MergeGroup group)
{
    auto& values = m_currentBlock->values;
    auto& first = m_function.at(values[group.first]);
    auto& second = m_function.at(values[group.first + 1]);
    Rep rep = m_function.at(values[group.first + 2]).rep;
    auto& layout = m_program.at(group.layout);
    LBasicBlock isMerged = m_out.newBlock();
    LBasicBlock isNotMerged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer merged = AOT_TYPED_OPERATION(TryMergeRecordsAs, word32(layout.shape), in<Boxed>(first.arguments[group.startsWithClone ? 0 : 1]), in<Boxed>(second.arguments[1]));
    m_out.branch(m_out.notZero64(merged.value()), usually(isMerged), rarely(isNotMerged));
    m_out.appendTo(isMerged);
    join.add(convert(Operand(merged), m_program.at(layout.type), rep));
    m_out.appendTo(isNotMerged);
    Operand result;
    {
        SetForScope isLoweringGroup(m_isLoweringMergeGroup, true);
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        for (size_t k = group.first; k <= group.first + 2; ++k) {
            m_currentIndexInBlock = k;
            result = lower(values[k], m_function.at(values[k]));
            define(values[k], result);
        }
    }
    join.add(result);
    return join.finish();
}

std::optional<Lowering::SpreadGroup> Lowering::findSpreadGroup(size_t spreadIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    auto& spread = m_function.at(values[spreadIndex]);
    if (spread.arguments.size() != 2 || useCountOf(values[spreadIndex]))
        return std::nullopt;
    ValueID object = spread.arguments[0];
    ValueID source = spread.arguments[1];
    if (source == object || typeOf(source).objectLayouts.isEmpty() || typeOf(source).anyObject)
        return std::nullopt;
    SpreadGroup group { spreadIndex, notFound, { }, { } };
    Vector<StringID, 4> names;
    for (size_t k = spreadIndex + 1; k < values.size() && group.cast == notFound; ++k) {
        auto& member = m_function.at(values[k]);
        if (member.opcode == Opcode::String)
            continue;
        if (member.opcode == Opcode::Cast) {
            if (member.castReason() != CastReason::As || member.child(0) != object)
                return std::nullopt;
            group.cast = k;
            continue;
        }
        if (member.opcode != Opcode::CallBuiltin || useCountOf(values[k]))
            return std::nullopt;
        auto& implementation = m_builtins.at(member.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Operation || implementation.operation != LanguageOperation::DefineField || member.arguments.size() != 3 || member.arguments[0] != object || member.arguments[2] == object)
            return std::nullopt;
        auto& name = m_function.at(member.arguments[1]);
        if (name.opcode != Opcode::String || names.contains(name.string()))
            return std::nullopt;
        names.append(name.string());
        group.definitions.append(k);
    }
    if (group.cast == notFound || group.definitions.isEmpty() || useCountOf(object) != group.definitions.size() + 2)
        return std::nullopt;
    for (LayoutID supertype : typeOf(values[group.cast]).objectLayouts) {
        if (canStoreFieldsDirectly(m_program.at(supertype), group))
            group.layouts.append(supertype);
    }
    if (group.layouts.isEmpty())
        return std::nullopt;
    return group;
}

bool Lowering::canStoreFieldsDirectly(const Layout& layout, const SpreadGroup& group)
{
    auto& values = m_currentBlock->values;
    if (layout.kind != LayoutKind::Record || !layout.shape || !layout.subtypes.contains(m_program.at(layout.type).singleLayout()))
        return false;
    return std::ranges::all_of(group.definitions, [&](size_t k) {
        auto& member = m_function.at(values[k]);
        auto index = layout.fieldNamed(group.startsWithClone ? member.string() : m_function.at(member.arguments[1]).string());
        if (!index)
            return false;
        ValueID stored = group.startsWithClone ? member.child(1) : member.arguments[2];
        auto& field = layout.fields[*index];
        return !(field.flags & (fieldIsNeverStored | fieldIsImmutable)) && !field.isOutOfLine && !field.isPinned && !m_program.at(field.slot).isDiscriminant
            && (!field.type || typeOf(stored).isSubsetOf(m_program.at(field.type), m_program)) && isStoredInline(field, stored);
    });
}

std::optional<Lowering::SpreadGroup> Lowering::findSpreadGroupAfterClone(size_t copyIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID object = values[copyIndex];
    auto& copy = m_function.at(object);
    ValueID source = copy.arguments[0];
    if (typeOf(source).objectLayouts.isEmpty() || typeOf(source).anyObject)
        return std::nullopt;
    SpreadGroup group { copyIndex, notFound, { }, { }, true };
    for (size_t k = copyIndex + 1; k < values.size() && group.cast == notFound; ++k) {
        auto& member = m_function.at(values[k]);
        if (member.opcode == Opcode::Cast && member.castReason() != CastReason::Upcast) {
            if (member.castReason() != CastReason::As || member.child(0) != object)
                return std::nullopt;
            group.cast = k;
            continue;
        }
        if (member.opcode == Opcode::PutById) {
            size_t place = group.definitions.size() + 1;
            if (member.isInitialStore() || member.child(0) != object || member.child(1) == object || member.storesLazyClosure || lazyClosureStoredBy(member) || mayBeStoredBeforeJump(member))
                return std::nullopt;
            if (place >= copy.arguments.size() || m_function.at(copy.arguments[place]).opcode != Opcode::String || m_function.at(copy.arguments[place]).string() != member.string())
                return std::nullopt;
            group.definitions.append(k);
            continue;
        }
        bool dependsOnNothing = member.opcode == Opcode::Undefined || member.opcode == Opcode::Null || member.opcode == Opcode::Boolean || member.opcode == Opcode::Integer || member.opcode == Opcode::Double || member.opcode == Opcode::String;
        if (!dependsOnNothing)
            return std::nullopt;
    }
    if (group.cast == notFound || group.definitions.size() + 1 != copy.arguments.size() || useCountOf(object) != group.definitions.size() + 1)
        return std::nullopt;
    for (size_t i = 1; i < copy.arguments.size(); ++i) {
        for (size_t k = i + 1; k < copy.arguments.size(); ++k) {
            if (m_function.at(copy.arguments[i]).string() == m_function.at(copy.arguments[k]).string())
                return std::nullopt;
        }
    }
    auto expected = predictionOf(object);
    LayoutID supertype = expected ? expected->singleLayout() : LayoutID();
    if (!supertype || !typeOf(values[group.cast]).objectLayouts.contains(supertype) || !canStoreFieldsDirectly(m_program.at(supertype), group))
        return std::nullopt;
    group.layouts.append(supertype);
    return group;
}

Operand Lowering::lowerSpreadGroup(SpreadGroup group)
{
    auto& values = m_currentBlock->values;
    Rep rep = m_function.at(values[group.cast]).rep;
    Value copy;
    if (group.startsWithClone)
        copy.arguments = m_function.at(values[group.first]).arguments;
    else {
        copy.arguments.append(m_function.at(values[group.first]).arguments[1]);
        for (size_t k : group.definitions)
            copy.arguments.append(m_function.at(values[k]).arguments[1]);
    }
    Join join(m_out, rep, b3TypeOf(rep));
    for (unsigned i = 0; i < group.layouts.size(); ++i) {
        auto& layout = m_program.at(group.layouts[i]);
        LBasicBlock isInlinedBlock = m_out.newBlock();
        LBasicBlock isNotCopied = i + 1 == group.layouts.size() ? newColdBlock() : m_out.newBlock();
        CellPointer created = copyRecordAs(copy, group.layouts[i], CopyNamesKind::StoredNext, nullptr, true, isNotCopied);
        m_out.jump(isInlinedBlock);
        m_out.appendTo(isInlinedBlock);
        for (size_t k : group.definitions) {
            auto& member = m_function.at(values[k]);
            auto& field = layout.fields[*layout.fieldNamed(group.startsWithClone ? member.string() : m_function.at(member.arguments[1]).string())];
            ValueID stored = group.startsWithClone ? member.child(1) : member.arguments[2];
            if (field.store == Store::Int32OrNumberBox)
                storeInt32OrNumberBox(created, field, stored, true);
            else
                store(field.store, in(stored, repOf(field.store)), fieldAt(created, field));
        }
        fenceAfterInitialization();
        join.add(convert(Operand(created), m_program.at(layout.type), rep));
        m_out.appendTo(isNotCopied);
    }
    Operand result;
    {
        SetForScope isLoweringGroup(m_isLoweringMergeGroup, true);
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        for (size_t k = group.first; k <= group.cast; ++k) {
            if (group.startsWithClone && k != group.first && k != group.cast && !group.definitions.contains(k))
                continue;
            m_currentIndexInBlock = k;
            if (group.startsWithClone && k != group.first && k != group.cast) {
                emitPutById(in<Boxed>(values[group.first]), m_function.at(values[k]).string(), in<Boxed>(m_function.at(values[k]).child(1)));
                continue;
            }
            result = lower(values[k], m_function.at(values[k]));
            define(values[k], result);
        }
    }
    join.add(result);
    return join.finish();
}

CellPointer Lowering::copyRecordAs(const Value& value, LayoutID to, CopyNamesKind namesKind, const Vector<LayoutID, 4>* inlineSources, bool callerInitializesNamedFields, LBasicBlock onFailure)
{
    auto& target = m_program.at(to);
    ValueID sourceID = value.arguments[0];
    auto& sourceType = typeOf(sourceID);
    bool excludesNames = namesKind == CopyNamesKind::Excluded;
    bool storesNamesFirst = namesKind == CopyNamesKind::StoredFirst;
    auto isRecordWithoutExtraProperties = [&](LValue words) {
        return m_out.bitAnd(m_out.aboveOrEqual(words, m_out.constInt64(0xffULL << 56)), m_out.testIsZero64(words, m_out.constInt64(orderWordHasExtraProperties)));
    };
    auto outOfLine = [&] {
        LBasicBlock canInline = m_out.newBlock();
        LBasicBlock notRecord = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 2> copies;
        Boxed boxed = in<Boxed>(sourceID);
        orElse(isCell(boxed, sourceType), notRecord);
        m_out.branch(isRecordWithoutExtraProperties(m_out.load64(TypedPointer(m_heaps.root, m_out.add(asCell(boxed).value(), m_out.constIntPtr(offsetOfOrderWord))))), unsure(canInline), unsure(notRecord));
        m_out.appendTo(canInline);
        auto [count, list] = argumentBuffer(value.arguments);
        copies.append(m_out.anchor(AOT_TYPED_OPERATION(TryCopyRecordAs, word32(target.shape), word32(static_cast<uint32_t>(namesKind)), count, list).value()));
        m_out.jump(continuation);
        m_out.appendTo(notRecord);
        copies.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        LValue copied = m_out.phi(Int64, copies);
        if (onFailure)
            orElse(m_out.notZero64(copied), onFailure);
        return CellPointer(copied);
    };
    Vector<StringID, 4> names;
    Vector<ValueID, 4> given;
    for (size_t i = 1; i < value.arguments.size(); i += storesNamesFirst ? 2 : 1) {
        auto& name = m_function.at(value.arguments[i]);
        if (name.opcode != Opcode::String || (storesNamesFirst && (names.contains(name.string()) || i + 1 == value.arguments.size())))
            return outOfLine();
        names.append(name.string());
        if (storesNamesFirst)
            given.append(value.arguments[i + 1]);
    }
    bool isAllocatedInline = target.cloneAllocation != Format::none && !target.hasExtension && target.shape && m_program.allocations[target.cloneAllocation].size == target.sizeWithSpecializationCapacity
        && !m_program.allocations[target.cloneAllocation].numberOfLiterals;
    if (!isAllocatedInline || sourceType.primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt) || sourceType.anyObject || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return outOfLine();
    struct Source {
        LayoutID layout;
        Vector<std::pair<uint32_t, uint32_t>, 8> moves;
    };
    auto haveSameLiterals = [](const Field& a, const Field& b) {
        return a.literals.size() == b.literals.size() && std::ranges::equal(a.literals, b.literals, [](const Literal& x, const Literal& y) {
            return x.string == y.string && std::bit_cast<uint64_t>(x.number) == std::bit_cast<uint64_t>(y.number);
        });
    };
    auto remainingFieldsAreInitializedOrAbsent = [&](const BitVector& isSupplied) {
        for (uint32_t i = 0; i < target.fields.size(); ++i) {
            auto& held = target.fields[i];
            if (held.flags & fieldIsNeverStored || isSupplied.get(i))
                continue;
            uint16_t primitives = m_program.at(held.stored).primitives;
            if (held.isOutOfLine)
                return false;
            if (!excludesNames && names.contains(held.name)) {
                if (held.flags & fieldIsImmutable || held.isPinned || m_program.at(held.slot).isDiscriminant || (!storesNamesFirst && primitives & Primitive::Absent && !(primitives & Primitive::Undefined)))
                    return false;
                if (ValueID stored = storesNamesFirst ? given[names.find(held.name)] : ValueID(); stored && ((held.type && !typeOf(stored).isSubsetOf(m_program.at(held.type), m_program)) || !isStoredInline(held, stored)))
                    return false;
            } else if (!(primitives & Primitive::Absent))
                return false;
        }
        return true;
    };
    auto transitionSourceOf = [&](LayoutID id) -> std::optional<Source> {
        auto& layout = m_program.at(id);
        if (layout.kind != LayoutKind::Record || !layout.shape || Typed::shapeWordOf(layout.shape) >> 24 != 0xff || !layout.isAllocated || layout.hasExtension)
            return std::nullopt;
        Source source { id, { } };
        BitVector isSupplied;
        for (uint32_t i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            if (field.flags & fieldHoldsLazyClosure && prefersGenericPathInColdCode())
                return std::nullopt;
            if (field.flags & fieldIsNeverStored || (!storesNamesFirst && names.contains(field.name)))
                continue;
            if (names.contains(field.name) && m_program.at(field.stored).primitives & Primitive::Absent)
                return std::nullopt;
            auto index = target.fieldNamed(field.name);
            if (!index)
                return std::nullopt;
            auto& held = target.fields[*index];
            if (held.flags & (fieldIsNeverStored | fieldIsImmutable) || field.isOutOfLine || held.isOutOfLine || (held.store != field.store && ((held.store == Store::Int32OrNumberBox && !m_program.at(field.stored).isNumber()) || field.store == Store::Int32OrNumberBox || m_program.at(field.stored).primitives & Primitive::Absent)) || held.isPinned || field.isPinned || !haveSameLiterals(held, field)
                || m_program.at(held.slot).isDiscriminant || !m_program.at(field.stored).isSubsetOf(m_program.at(held.stored), m_program))
                return std::nullopt;
            source.moves.append({ i, *index });
            isSupplied.set(*index);
        }
        if (!remainingFieldsAreInitializedOrAbsent(isSupplied))
            return std::nullopt;
        return source;
    };
    Vector<Source, 2> sources;
    for (LayoutID supertype : sourceType.objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            bool isKnown = sources.containsIf([&](auto& source) {
                return source.layout == subtype;
            });
            if (isKnown || sources.size() == 2 || (inlineSources && !inlineSources->contains(subtype)))
                continue;
            if (auto source = transitionSourceOf(subtype))
                sources.append(WTF::move(*source));
        }
    }
    bool createsEmptyRecord = namesKind == CopyNamesKind::StoredNext && sourceType.primitives & Primitive::UndefinedOrNull && remainingFieldsAreInitializedOrAbsent({ });
    for (StringID name : names) {
        if (!excludesNames && !target.fieldNamed(name)) {
            sources.clear();
            createsEmptyRecord = false;
        }
    }
    if (m_context.sites.size() + sources.size() + 1 + createsEmptyRecord > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots) || (m_program.runsOnce(m_id) && !m_isInLoop))
        return outOfLine();
    unsigned firstSlot = m_context.sites.size();
    for (unsigned i = 0; i <= sources.size() + createsEmptyRecord; ++i)
        m_context.sites.append(Site { });
    Pointer cache = slotAddress(firstSlot);
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock cacheMiss = newColdBlock();
    LBasicBlock notRecord = newColdBlock();
    LBasicBlock isNotCell = createsEmptyRecord ? m_out.newBlock() : notRecord;
    Vector<std::pair<LBasicBlock, LValue>, 2> inlineCopies;
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> copies;
    Boxed boxed = in<Boxed>(sourceID);
    orElse(isCell(boxed, sourceType), isNotCell);
    CellPointer cell = asCell(boxed);
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    orElse(Word32(m_out.notZero64(words)), notRecord);
    for (unsigned i = 0; i < sources.size(); ++i) {
        auto& layout = m_program.at(sources[i].layout);
        LBasicBlock hit = m_out.newBlock();
        LBasicBlock otherwise = i + 1 == sources.size() ? slowPath : m_out.newBlock();
        LBasicBlock inlineCopy = m_out.newBlock();
        m_out.branch(m_out.equal(words, m_out.load64(inCache(i * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)))), usually(hit), rarely(otherwise));
        m_out.appendTo(hit);
        ValueFromBlock cachedOrderWord = m_out.anchor(m_out.load32(inCache(i * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, offset))));
        m_out.jump(inlineCopy);
        m_out.appendTo(inlineCopy);
        LValue copyKeyOrderWord = m_out.phi(Int32, cachedOrderWord);
        inlineCopies.append({ inlineCopy, copyKeyOrderWord });
        Vector<LValue, 8> inlineNumbers;
        for (auto [from, into] : sources[i].moves) {
            if (layout.fields[from].store == Store::Int32OrNumberBox)
                orElse(logicalNot(refersToNumberBox(load(Store::Int32OrNumberBox, fieldAt(cell, layout.fields[from]), 0).as<Word32>())), cacheMiss);
            bool isNarrowed = layout.fields[from].store != Store::Int32OrNumberBox && target.fields[into].store == Store::Int32OrNumberBox;
            inlineNumbers.append(isNarrowed ? low32(narrowOrElse(load(layout.fields[from].store, fieldAt(cell, layout.fields[from]), 0, cell.value()), m_program.at(layout.fields[from].stored), inlineNumberType(), cacheMiss).as<Integer>()).value() : nullptr);
        }
        for (auto [from, into] : sources[i].moves) {
            if (layout.fields[from].flags & fieldHoldsLazyClosure)
                materializeLazyClosureIfNeeded(cell, layout.fields[from]);
        }
        CellPointer copy = allocate(target.cloneAllocation);
        m_out.store32(copyKeyOrderWord, copy.value(), m_heaps.AOTTypedObject_orderWord);
        BitVector isMoved;
        for (unsigned k = 0; k < sources[i].moves.size(); ++k) {
            auto [from, into] = sources[i].moves[k];
            TypedPointer source = fieldAt(cell, layout.fields[from]);
            TypedPointer destination = fieldAt(copy, target.fields[into]);
            isMoved.set(into);
            if (inlineNumbers[k]) {
                store(Store::Int32OrNumberBox, Word32(inlineNumbers[k]), destination);
                continue;
            }
            if (layout.fields[from].store != target.fields[into].store) {
                store(target.fields[into].store, convert(load(layout.fields[from].store, source, 0, cell.value()), m_program.at(layout.fields[from].stored), repOf(target.fields[into].store)), destination);
                continue;
            }
            switch (sizeOf(target.fields[into].store)) {
            case 1:
                m_out.store32As8(m_out.load8ZeroExt32(source), destination);
                break;
            case 2:
                m_out.store32As16(m_out.load16ZeroExt32(source), destination);
                break;
            case 4:
                m_out.store32(m_out.load32(source), destination);
                break;
            case 8:
                m_out.store64(m_out.load64(source), destination);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        for (uint32_t k = 0; k < target.fields.size(); ++k) {
            auto& held = target.fields[k];
            if (held.flags & fieldIsNeverStored || isMoved.get(k))
                continue;
            if (callerInitializesNamedFields && names.contains(held.name) && !holdsCells(held.store) && held.store != Store::Int32OrNumberBox)
                continue;
            if (!storesNamesFirst || !names.contains(held.name))
                store(held.store, initialFieldValue(held, !excludesNames && names.contains(held.name)), fieldAt(copy, held));
            else if (held.store == Store::Int32OrNumberBox)
                storeInt32OrNumberBox(copy, held, given[names.find(held.name)], true);
            else
                store(held.store, in(given[names.find(held.name)], repOf(held.store)), fieldAt(copy, held));
        }
        ensureStillAliveHere(cell);
        if (!callerInitializesNamedFields)
            fenceAfterInitialization();
        copies.append(m_out.anchor(copy.value()));
        m_out.jump(continuation);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
    if (sources.isEmpty())
        m_out.jump(slowPath);
    m_out.appendTo(slowPath);
    LValue found = m_out.castToInt32(callAllocationStub(Stub::LookupCopyKeyOrder, { { words, stubTemporaryGPRs[0] }, { cache.value(), stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false, true));
    LBasicBlock isFound = newColdBlock();
    m_out.branch(m_out.notZero32(found), unsure(isFound), unsure(cacheMiss));
    m_out.appendTo(isFound);
    if (inlineCopies.isEmpty())
        m_out.jump(notRecord);
    else {
        orElse(Word32(m_out.testIsZero32(found, m_out.constInt32(Instance::copyAlwaysFails))), notRecord);
        LValue orderWord = m_out.bitAnd(found, m_out.constInt32(static_cast<int32_t>(~Instance::copyKeyOrderFromSecondSource)));
        for (auto& [inlineCopy, copyKeyOrderWord] : inlineCopies)
            m_out.addIncomingToPhi(copyKeyOrderWord, m_out.anchor(orderWord));
        if (inlineCopies.size() == 1)
            m_out.jump(inlineCopies[0].first);
        else
            m_out.branch(m_out.testIsZero32(found, m_out.constInt32(static_cast<int32_t>(Instance::copyKeyOrderFromSecondSource))), unsure(inlineCopies[0].first), unsure(inlineCopies[1].first));
    }
    m_out.appendTo(cacheMiss);
    auto tryOutOfLinePath = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        LValue copied = AOT_TYPED_OPERATION(TryCopyRecordAsAt, cache, word32(sources.isEmpty() ? 0 : m_program.at(sources[0].layout).shape), word32(sources.size() > 1 ? m_program.at(sources[1].layout).shape : 0), word32(target.shape), word32(static_cast<uint32_t>(namesKind)), count, list).value();
        if (onFailure)
            orElse(m_out.notZero64(copied), onFailure);
        copies.append(m_out.anchor(copied));
        m_out.jump(continuation);
    };
    tryOutOfLinePath();
    if (createsEmptyRecord) {
        size_t slot = (sources.size() + 1) * sizeof(AOT::Slot);
        LBasicBlock isKnown = m_out.newBlock();
        LBasicBlock isNotKnown = newColdBlock();
        m_out.appendTo(isNotCell);
        m_out.branch(m_out.notZero64(m_out.load64(inCache(slot + OBJECT_OFFSETOF(AOT::Slot, pointer)))), usually(isKnown), rarely(isNotKnown));
        m_out.appendTo(isKnown);
        CellPointer created = allocate(target.cloneAllocation);
        m_out.store32(m_out.load32(inCache(slot + OBJECT_OFFSETOF(AOT::Slot, offset))), created.value(), m_heaps.AOTTypedObject_orderWord);
        for (auto& held : target.fields) {
            if (!(held.flags & fieldIsNeverStored) && !(callerInitializesNamedFields && names.contains(held.name) && !holdsCells(held.store) && held.store != Store::Int32OrNumberBox))
                store(held.store, initialFieldValue(held, names.contains(held.name)), fieldAt(created, held));
        }
        if (!callerInitializesNamedFields)
            fenceAfterInitialization();
        copies.append(m_out.anchor(created.value()));
        m_out.jump(continuation);
        m_out.appendTo(isNotKnown);
        tryOutOfLinePath();
    }
    m_out.appendTo(notRecord);
    if (onFailure)
        m_out.jump(onFailure);
    else {
        copies.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, copies));
}

Lowering::CloneCapacity Lowering::cloneCapacityOf(ValueID copy)
{
    CloneCapacity result;
    result.copy = copy;
    auto expected = predictionOf(copy);
    result.supertype = expected ? expected->singleLayout() : LayoutID();
    if (!result.supertype)
        return result;
    Vector<StringID, 8> names;
    bool mayBeAnyName = false;
    for (ValueID id : usersOf(copy)) {
        auto& user = m_function.at(id);
        if (user.opcode == Opcode::PutById && user.child(0) == copy)
            names.append(user.string());
        else if (user.opcode == Opcode::CallBuiltin && !user.arguments.isEmpty() && user.arguments[0] == copy) {
            auto& implementation = m_builtins.at(user.builtin()).implementation;
            mayBeAnyName |= implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
        }
    }
    if (names.isEmpty() && !mayBeAnyName)
        return { };
    auto& supertype = m_program.at(result.supertype);
    for (LayoutID id : supertype.subtypes) {
        auto& subtype = m_program.at(id);
        if (id == result.supertype || !subtype.shape)
            continue;
        bool hasNoRoom = std::ranges::any_of(supertype.fields, [&](const Field& field) {
            if (!mayBeAnyName && !names.contains(field.name))
                return false;
            auto index = subtype.fieldNamed(field.name);
            if (!index)
                return false;
            TypeID held = subtype.fields[*index].type;
            return held && (!field.type || !m_program.at(field.type).isSubsetOf(m_program.at(held), m_program));
        });
        if (hasNoRoom)
            result.subtypesWithoutCapacity.append(id);
    }
    return result;
}

bool Lowering::alwaysPreservesSourceLayout(const CloneCapacity& room) const
{
    return room.subtypesWithoutCapacity.isEmpty() && (!room.supertype || !m_program.at(room.supertype).acceptsUntypedObjects);
}

Word32 Lowering::preservesSourceLayout(ValueID sourceID, const CloneCapacity& room)
{
    if (alwaysPreservesSourceLayout(room))
        return boolean(true);
    auto newBlockWithCurrentFrequency = [&] {
        LBasicBlock block = m_out.newBlock();
        block->setFrequency(m_out.m_block->frequency());
        return block;
    };
    LBasicBlock isCellCase = newBlockWithCurrentFrequency();
    LBasicBlock continuation = newBlockWithCurrentFrequency();
    Boxed source = in<Boxed>(sourceID);
    ValueFromBlock forNonCell = m_out.anchor(boolean(true).value());
    m_out.branch(isCell(source, typeOf(sourceID)).value(), unsure(isCellCase), unsure(continuation));
    m_out.appendTo(isCellCase);
    Word32 shapeWord = shapeWordOf(asCell(source));
    auto isOf = [&](LayoutID id) {
        return isZero(add(shapeWord, word32(m_program.at(id).shape)));
    };
    Word32 keeps = room.subtypesWithoutCapacity.isEmpty() ? boolean(true) : hasNoBits(orderWordOf(asCell(source)), orderWordHasExtraProperties);
    if (m_program.at(room.supertype).acceptsUntypedObjects)
        keeps = logicalAnd(keeps, passes(shapeWord, m_program.at(room.supertype).test));
    constexpr unsigned maxTested = 8;
    if (room.subtypesWithoutCapacity.size() > maxTested)
        keeps = logicalAnd(keeps, isOf(room.supertype));
    else {
        for (LayoutID id : room.subtypesWithoutCapacity)
            keeps = logicalAnd(keeps, logicalNot(isOf(id)));
    }
    ValueFromBlock ofCell = m_out.anchor(keeps.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, forNonCell, ofCell));
}

CellPointer Lowering::cloneObjectOutOfLine(ValueID sourceID, const CloneCapacity& room)
{
    Boxed source = in<Boxed>(sourceID);
    if (alwaysPreservesSourceLayout(room) || m_program.at(room.supertype).kind != LayoutKind::Record || !m_program.at(room.supertype).shape || Options::auditAOTTypedAssertions())
        return AOT_TYPED_OPERATION(CloneObject, source, preservesSourceLayout(sourceID, room));
    if (auto& test = m_program.at(room.supertype).test; room.subtypesWithoutCapacity.isEmpty() && test.isRange() && !test.neverPasses())
        return AOT_TYPED_OPERATION(CloneObjectWithSubtypeCheck, source, word32(m_program.at(room.supertype).shape), word32(test.first + test.count - 1), word32(test.count));
    LBasicBlock keepsLayout = m_out.newBlock();
    LBasicBlock changesLayout = m_out.newBlock();
    LBasicBlock isInlinedBlock = m_out.newBlock();
    LBasicBlock isNotCopied = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> copies;
    m_out.branch(preservesSourceLayout(sourceID, room).value(), unsure(keepsLayout), unsure(changesLayout));
    m_out.appendTo(keepsLayout);
    copies.append(m_out.anchor(AOT_TYPED_OPERATION(CloneObject, source, boolean(true)).value()));
    m_out.jump(continuation);
    m_out.appendTo(changesLayout);
    CellPointer copied = copyRecordAs(m_function.at(room.copy), room.supertype, CopyNamesKind::StoredNext, &room.subtypesWithoutCapacity);
    m_out.branch(m_out.notZero64(copied.value()), usually(isInlinedBlock), rarely(isNotCopied));
    m_out.appendTo(isInlinedBlock);
    copies.append(m_out.anchor(copied.value()));
    m_out.jump(continuation);
    m_out.appendTo(isNotCopied);
    copies.append(m_out.anchor(AOT_TYPED_OPERATION(CloneObject, source, boolean(false)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, copies));
}

std::optional<Lowering::CloneGroup> Lowering::findCloneGroup(size_t cloneIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID copy = values[cloneIndex];
    CloneGroup group { cloneIndex };
    bool hasInterveningReadOrThrow = false;
    for (size_t k = cloneIndex + 1; k < values.size(); ++k) {
        auto& value = m_function.at(values[k]);
        if (value.opcode == Opcode::PutById && !value.isInitialStore() && value.child(0) == copy && value.child(1) != copy) {
            group.stores.append(k);
            group.isDecidedAtClone = hasInterveningReadOrThrow;
            continue;
        }
        if (value.opcode == Opcode::Cast && value.castReason() == CastReason::As && value.child(0) == copy) {
            group.cast = k;
            group.isDecidedAtClone = hasInterveningReadOrThrow;
            break;
        }
        bool usesCopy = false;
        value.forEachChild([&](ValueID child) {
            usesCopy |= child == copy;
        });
        if (usesCopy)
            break;
        bool dependsOnNothing = value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String
            || value.opcode == Opcode::NewArray || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
        if (dependsOnNothing)
            continue;
        if (value.opcode == Opcode::Cast || value.opcode == Opcode::CallDirect || writesOf(m_program, m_function, *m_currentBlock, k) > Writes::Nothing)
            break;
        hasInterveningReadOrThrow = true;
    }
    if (group.stores.isEmpty() && group.cast == notFound)
        return std::nullopt;
    group.layouts = inlineClonedLayouts(typeOf(m_function.at(copy).arguments[0]));
    group.layouts.removeAllMatching([&](LayoutID id) {
        auto& layout = m_program.at(id);
        for (size_t k : group.stores) {
            auto& store = m_function.at(values[k]);
            auto index = layout.fieldNamed(store.string());
            if (!index)
                return true;
            auto& field = layout.fields[*index];
            if (field.flags & fieldIsNeverStored || field.isOutOfLine || m_program.at(field.slot).isDiscriminant)
                return true;
            if (field.isPinned) {
                ValueID stored = store.child(1);
                if (ValueID constant = m_program.constantOf(m_function, stored))
                    stored = constant;
                if (m_function.at(stored).opcode != Opcode::String || m_function.at(stored).string() != field.literals[0].string)
                    return true;
            }
            if ((field.type && !typeOf(store.child(1)).isSubsetOf(m_program.at(field.type), m_program)) || !isStoredInline(field, store.child(1)))
                return true;
        }
        return group.cast != notFound && !m_program.castIsRedundant(m_program.at(layout.type), typeOf(values[group.cast]), CastReason::As);
    });
    if (group.layouts.isEmpty())
        return std::nullopt;
    return group;
}

std::optional<uint64_t> Lowering::typedCloneTargetAt(size_t cloneIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID copy = values[cloneIndex];
    Vector<size_t, 4> stores;
    size_t cast = notFound;
    for (size_t k = cloneIndex + 1; k < values.size() && cast == notFound; ++k) {
        auto& value = m_function.at(values[k]);
        if (value.opcode == Opcode::PutById && !value.isInitialStore() && !value.hasKnownSlotWord() && value.child(0) == copy && value.child(1) != copy) {
            stores.append(k);
            continue;
        }
        if (value.opcode == Opcode::Cast && value.castReason() == CastReason::As && value.coercesFreshData() && value.child(0) == copy) {
            cast = k;
            continue;
        }
        bool usesCopy = false;
        value.forEachChild([&](ValueID child) {
            usesCopy |= child == copy;
        });
        if (usesCopy)
            return std::nullopt;
    }
    if (cast == notFound || stores.size() > TypedCloneTarget::maxStoredFields || useCountOf(copy) != stores.size() + 1)
        return std::nullopt;
    LayoutID id = typeOf(values[cast]).singleLayout();
    if (!id)
        return std::nullopt;
    auto& layout = m_program.at(id);
    if (layout.kind != LayoutKind::Record || !layout.shape || layout.shape >= 1u << TypedCloneTarget::shapeBits || !layout.isAllocated || layout.hasExtension || layout.fields.size() + 1 >= 1u << TypedCloneTarget::fieldBits || !layout.subtypes.contains(id))
        return std::nullopt;
    uint64_t target = layout.shape;
    Vector<StringID, 4> names;
    for (size_t k : stores) {
        auto& member = m_function.at(values[k]);
        auto index = layout.fieldNamed(member.string());
        if (!index || names.contains(member.string()) || member.storesLazyClosure || lazyClosureStoredBy(member) || mayBeStoredBeforeJump(member))
            return std::nullopt;
        auto& field = layout.fields[*index];
        bool isStoredDirectly = !(field.flags & (fieldIsNeverStored | fieldIsImmutable)) && !field.isOutOfLine && !field.isPinned && !m_program.at(field.slot).isDiscriminant && field.literals.isEmpty()
            && (!field.type || typeOf(member.child(1)).isSubsetOf(m_program.at(field.type), m_program));
        if (!isStoredDirectly)
            return std::nullopt;
        uint32_t describedFieldIndex = 0;
        for (uint32_t i = 0; i < *index; ++i)
            describedFieldIndex += !(layout.fields[i].flags & fieldIsNeverStored);
        target = TypedCloneTarget::withStoredField(target, names.size(), describedFieldIndex);
        names.append(member.string());
    }
    size_t firstAdjacentStore = cast;
    while (firstAdjacentStore > cloneIndex + 1 && stores.contains(firstAdjacentStore - 1))
        --firstAdjacentStore;
    auto& to = typeOf(values[cast]);
    bool plainObjectsPassCast = to.acceptsUncheckedObjects(m_program) && !m_program.annotationCoercesUntypedObjects(to) && !m_program.isRelaxedForLogging(to);
    m_typedCloneOfUntypedSource = { m_currentBlock, copy, id, firstAdjacentStore, cast, plainObjectsPassCast, { } };
    return target;
}

void Lowering::storeToPlainClone(Boxed object, const Value& value)
{
    if (isNamedLastIndex(value.string()))
        emitPutById(object, value.string(), in<Boxed>(value.child(1)));
    else
        emitStandaloneCachedPutById(object, value.string(), in<Boxed>(value.child(1)), true);
}

void Lowering::storeToTypedCloneField(CellPointer cell, const Value& value)
{
    auto& layout = m_program.at(m_typedCloneOfUntypedSource.layout);
    auto& field = layout.fields[*layout.fieldNamed(value.string())];
    didEmitWrite(Writes::Fields);
    if (field.store == Store::Int32OrNumberBox)
        storeInt32OrNumberBox(cell, field, value.child(1), true);
    else
        store(field.store, in(value.child(1), repOf(field.store)), fieldAt(cell, field));
    if (holdsCells(field.store) && typeOf(value.child(1)).hasCells())
        barrier(cell);
}

bool Lowering::tryStoreToTypedClone(const Value& value)
{
    auto& clone = m_typedCloneOfUntypedSource;
    if (clone.block != m_currentBlock || clone.copy != value.child(0))
        return false;
    if (m_currentIndexInBlock > clone.firstAdjacentStore)
        return true;
    auto& values = m_currentBlock->values;
    auto& layout = m_program.at(clone.layout);
    bool includesCast = m_currentIndexInBlock == clone.firstAdjacentStore;
    LBasicBlock isTyped = m_out.newBlock();
    LBasicBlock isUntyped = newColdBlock();
    Boxed boxed = in<Boxed>(value.child(0));
    CellPointer cell = asCell(boxed);
    m_out.branch(equal(shapeWordOf(cell), word32(Typed::shapeWordOf(layout.shape))).value(), usually(isTyped), rarely(isUntyped));
    if (!includesCast) {
        LBasicBlock continuation = m_out.newBlock();
        m_out.appendTo(isTyped);
        storeToTypedCloneField(cell, value);
        m_out.jump(continuation);
        m_out.appendTo(isUntyped);
        storeToPlainClone(boxed, value);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return true;
    }
    Rep rep = m_function.at(values[clone.cast]).rep;
    Join join(m_out, rep, b3TypeOf(rep));
    m_out.appendTo(isTyped);
    for (size_t k = clone.firstAdjacentStore; k < clone.cast; ++k)
        storeToTypedCloneField(cell, m_function.at(values[k]));
    join.add(convert(Operand(cell), m_program.at(layout.type), rep));
    m_out.appendTo(isUntyped);
    {
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        for (size_t k = clone.firstAdjacentStore; k < clone.cast; ++k) {
            m_currentIndexInBlock = k;
            storeToPlainClone(boxed, m_function.at(values[k]));
        }
        m_currentIndexInBlock = clone.cast;
        if (clone.plainObjectsPassCast)
            join.add(convert(Operand(cell), m_program.at(m_program.types.object), rep));
        else {
            clone.block = nullptr;
            join.add(lower(values[clone.cast], m_function.at(values[clone.cast])));
            clone.block = m_currentBlock;
        }
    }
    clone.castResult = join.finish();
    return true;
}

std::optional<Operand> Lowering::tryCastTypedClone()
{
    auto& clone = m_typedCloneOfUntypedSource;
    if (clone.block != m_currentBlock || clone.cast != m_currentIndexInBlock)
        return std::nullopt;
    clone.block = nullptr;
    if (clone.firstAdjacentStore < clone.cast)
        return clone.castResult;
    if (!clone.plainObjectsPassCast)
        return std::nullopt;
    return convert(Operand(asCell(in<Boxed>(clone.copy))), m_program.at(m_program.types.object), m_function.at(m_currentBlock->values[clone.cast]).rep);
}

template<typename Functor>
void Lowering::dispatchOnCloneSourceLayout(const CloneGroup& group, LBasicBlock slowPath, const Functor& onLayout)
{
    auto& values = m_currentBlock->values;
    ValueID sourceID = m_function.at(values[group.clone]).arguments[0];
    Boxed source = in<Boxed>(sourceID);
    orElse(isCell(source, typeOf(sourceID)), slowPath);
    CellPointer cell = asCell(source);
    Word32 shapeWord = shapeWordOf(cell);
    orElse(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), slowPath);
    bool matchesAnyLayout = allSubtypesAreIn(typeOf(sourceID), group.layouts);
    for (unsigned i = 0; i < group.layouts.size(); ++i) {
        auto& layout = m_program.at(group.layouts[i]);
        LBasicBlock otherwise = i + 1 == group.layouts.size() ? slowPath : m_out.newBlock();
        branchOnSourceLayout(shapeWord, layout, matchesAnyLayout && otherwise == slowPath, otherwise);
        for (size_t k : group.stores) {
            auto& field = layout.fields[*layout.fieldNamed(m_function.at(values[k]).string())];
            if (m_program.at(field.stored).primitives & Primitive::Absent && inheritsThroughLayoutPrototype(m_program, group.layouts[i], field.name))
                orElse(logicalNot(isFieldAbsent(cell, field, 0)), slowPath);
        }
        unlessBoxed(cell, layout, slowPath);
        onLayout(i);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
}

void Lowering::decideCloneGroup(CloneGroup& group)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> decisions;
    Vector<ValueFromBlock, 3> copies;
    dispatchOnCloneSourceLayout(group, slowPath, [&](unsigned i) {
        decisions.append(m_out.anchor(m_out.constInt32(i + 1)));
        copies.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
    });
    m_out.appendTo(slowPath);
    decisions.append(m_out.anchor(m_out.int32Zero));
    ValueID copyID = m_currentBlock->values[group.clone];
    ValueID sourceID = m_function.at(copyID).arguments[0];
    copies.append(m_out.anchor(cloneObjectOutOfLine(sourceID, cloneCapacityOf(copyID)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    group.decision = m_out.phi(Int32, decisions);
    group.copied = m_out.phi(Int64, copies);
}

bool Lowering::everyCloneOffUsualPathPassesCast(const CloneGroup& group)
{
    auto& values = m_currentBlock->values;
    if (group.cast == notFound)
        return false;
    auto& to = typeOf(values[group.cast]);
    if (!to.acceptsUncheckedObjects(m_program) || m_program.annotationCoercesUntypedObjects(to) || m_program.isRelaxedForLogging(to))
        return false;
    for (LayoutID supertype : typeOf(m_function.at(values[group.clone]).arguments[0]).objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (!m_program.castIsRedundant(m_program.at(m_program.at(subtype).type), to, CastReason::As))
                return false;
        }
    }
    return true;
}

std::optional<uint64_t> Lowering::namesStoredWhereEveryLayoutIsCopiedInline(const CloneGroup& group)
{
    auto& values = m_currentBlock->values;
    if (group.isDecidedAtClone || group.stores.size() > 2)
        return std::nullopt;
    for (LayoutID supertype : typeOf(m_function.at(values[group.clone]).arguments[0]).objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (m_program.at(subtype).isAllocated && !group.layouts.contains(subtype))
                return std::nullopt;
        }
    }
    uint64_t names = 0;
    for (unsigned i = 0; i < group.stores.size(); ++i) {
        auto& member = m_function.at(values[group.stores[i]]);
        bool repeatsName = i && member.string() == m_function.at(values[group.stores[0]]).string();
        if (member.string().index() + 1 >= (1u << 24) || repeatsName || isNamedLastIndex(member.string()) || member.storesLazyClosure || lazyClosureStoredBy(member) || mayBeStoredBeforeJump(member))
            return std::nullopt;
        names |= static_cast<uint64_t>(member.string().index() + 1) << (24 * i);
    }
    return names;
}

std::optional<CellPointer> Lowering::tryCloneAsExtendedPlainObject(const CloneGroup& group)
{
    auto names = namesStoredWhereEveryLayoutIsCopiedInline(group);
    if (!names || !*names)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    Boxed source = in<Boxed>(m_function.at(values[group.clone]).arguments[0]);
    ValueID first = m_function.at(values[group.stores[0]]).child(1);
    if (group.stores.size() == 1 && get(first).rep == Rep::Double)
        return AOT_TYPED_OPERATION(CloneObjectAndExtendWithNumber, source, integer(static_cast<int64_t>(*names)), in<Number>(first));
    return AOT_TYPED_OPERATION(CloneObjectAndExtend, source, integer(static_cast<int64_t>(*names)), in<Boxed>(first), group.stores.size() > 1 ? in<Boxed>(m_function.at(values[group.stores[1]]).child(1)) : undefined());
}

Operand Lowering::lowerCloneGroup(CloneGroup group)
{
    auto& values = m_currentBlock->values;
    ValueID copyID = values[group.clone];
    ValueID sourceID = m_function.at(copyID).arguments[0];
    bool hasCast = group.cast != notFound;
    Rep castRep = hasCast ? m_function.at(values[group.cast]).rep : Rep::Void;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> copies;
    Vector<ValueFromBlock, 3> results;
    Vector<StringID, 4> overwritten;
    for (size_t k : group.stores)
        overwritten.append(m_function.at(values[k]).string());
    auto inlinePath = [&](unsigned i) {
        auto& layout = m_program.at(group.layouts[i]);
        CellPointer cell = asCell(in<Boxed>(sourceID));
        CellPointer copy = copyRecord(cell, orderWordOf(cell), layout, overwritten);
        for (size_t k : group.stores) {
            auto& member = m_function.at(values[k]);
            auto& field = layout.fields[*layout.fieldNamed(member.string())];
            if (field.store == Store::Int32OrNumberBox)
                storeInt32OrNumberBox(copy, field, member.child(1), true);
            else
                store(field.store, in(member.child(1), repOf(field.store)), fieldAt(copy, field));
        }
        Vector<StringID, 4> created;
        for (size_t k : group.stores) {
            auto& field = layout.fields[*layout.fieldNamed(m_function.at(values[k]).string())];
            if (!(m_program.at(field.stored).primitives & Primitive::Absent) || inheritsThroughLayoutPrototype(m_program, group.layouts[i], field.name) || created.contains(field.name))
                continue;
            created.append(field.name);
            SetForScope indexInBlock(m_currentIndexInBlock, k);
            SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, copyID));
            LBasicBlock creates = m_out.newBlock();
            LBasicBlock presentCase = m_out.newBlock();
            m_out.branch(isFieldAbsent(cell, field, 0).value(), unsure(creates), unsure(presentCase));
            m_out.appendTo(creates);
            didEmitWrite(Writes::Fields);
            didCreateFieldInCell(copy, field, OperationEffects { Writes::Fields, false, false, true }, [] { });
            m_out.jump(presentCase);
            m_out.appendTo(presentCase);
        }
        fenceAfterInitialization();
        copies.append(m_out.anchor(copy.value()));
        if (hasCast)
            results.append(m_out.anchor(convert(Operand(copy), m_program.at(layout.type), castRep).value));
        m_out.jump(continuation);
    };
    if (!group.isDecidedAtClone)
        dispatchOnCloneSourceLayout(group, slowPath, inlinePath);
    else {
        for (unsigned i = 0; i < group.layouts.size(); ++i) {
            LBasicBlock hasLayout = m_out.newBlock();
            LBasicBlock otherwise = i + 1 == group.layouts.size() ? slowPath : m_out.newBlock();
            m_out.branch(m_out.equal(group.decision, m_out.constInt32(i + 1)), usually(hasLayout), rarely(otherwise));
            m_out.appendTo(hasLayout);
            inlinePath(i);
            if (otherwise != slowPath)
                m_out.appendTo(otherwise);
        }
    }

    m_out.appendTo(slowPath);
    bool passesCast = everyCloneOffUsualPathPassesCast(group);
    std::optional<CellPointer> plainCopy = passesCast ? tryCloneAsExtendedPlainObject(group) : std::nullopt;
    CellPointer other = plainCopy ? *plainCopy : group.isDecidedAtClone ? CellPointer(group.copied) : cloneObjectOutOfLine(sourceID, cloneCapacityOf(copyID));
    defineDeferredAllocation(copyID, other);
    Operand result;
    if (plainCopy)
        result = convert(Operand(other), m_program.at(m_program.types.object), castRep);
    else {
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        auto carryOut = [&](size_t k) {
            m_currentIndexInBlock = k;
            result = lower(values[k], m_function.at(values[k]));
            define(values[k], result);
        };
        for (size_t k : group.stores)
            carryOut(k);
        if (passesCast)
            result = convert(Operand(other), m_program.at(m_program.types.object), castRep);
        else if (hasCast)
            carryOut(group.cast);
    }
    copies.append(m_out.anchor(other.value()));
    if (hasCast)
        results.append(m_out.anchor(result.value));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    defineDeferredAllocation(copyID, CellPointer(m_out.phi(Int64, copies)));
    if (!hasCast)
        return undefined();
    return Operand(castRep, m_out.phi(b3TypeOf(castRep), results));
}

Lowering::CloneDefinitions Lowering::definitionsOnClone(ValueID object, bool mayCarryOut)
{
    CloneDefinitions result { 0, { } };
    auto& values = m_currentBlock->values;
    m_cloneDefinitionsBlock = m_currentBlock;
    m_cloneDefinitions.clear();
    for (size_t index = m_currentIndexInBlock + 1; mayCarryOut && index < values.size() && result.values.size() < 2; ++index) {
        auto& user = m_function.at(values[index]);
        if (user.opcode == Opcode::String)
            continue;
        if (!isDefineField(user) || user.arguments.size() != 3 || user.arguments[0] != object || objectLiteralAt(index) || propertyRunAt(index))
            break;
        auto& key = m_function.at(user.arguments[1]);
        auto afterClone = values.subspan(m_currentIndexInBlock + 1, index - m_currentIndexInBlock - 1);
        bool isCreatedAfterClone = std::ranges::find(afterClone, user.arguments[2]) != afterClone.end();
        if (key.opcode != Opcode::String || key.string().index() + 1 >= (1u << 24) || parseIndex(m_program.string(key.string())) || user.arguments[2] == object || isCreatedAfterClone)
            break;
        result.more |= static_cast<uint64_t>(key.string().index() + 1) << (24 * result.values.size());
        result.values.append(user.arguments[2]);
        m_cloneDefinitions.append(index);
    }
    uint64_t others = 0;
    auto definesPropertyOnObject = [&](const Value& user) {
        return (user.opcode == Opcode::PutByVal && withoutUpcasts(user.child(0)) == object) || (isDefineField(user) && !user.arguments.isEmpty() && withoutUpcasts(user.arguments[0]) == object);
    };
    if (auto* users = usersOfAllocation(object)) {
        for (auto& user : *users) {
            bool isCreatedWithClone = m_cloneDefinitions.containsIf([&](size_t index) {
                return values[index] == user.value;
            });
            others += !isCreatedWithClone && definesPropertyOnObject(m_function.at(user.value));
        }
    } else {
        for (auto& block : m_function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                if (&block == m_currentBlock && m_cloneDefinitions.contains(index))
                    continue;
                others += definesPropertyOnObject(m_function.at(block.values[index]));
            }
        }
    }
    result.more |= std::min<uint64_t>(others, 255) << 48;
    return result;
}

Lowering::CloneDefinitions Lowering::storesDirectlyBehindClone(ValueID object)
{
    CloneDefinitions result = definitionsOnClone(object, true);
    if (!result.values.isEmpty())
        return result;
    auto& values = m_currentBlock->values;
    Vector<StringID, 2> names;
    for (size_t index = m_currentIndexInBlock + 1; index < values.size() && result.values.size() < 2; ++index) {
        auto& user = m_function.at(values[index]);
        bool isPlainStore = user.opcode == Opcode::PutById && !user.isInitialStore() && !user.hasKnownSlotWord() && user.child(0) == object && user.child(1) != object
            && !user.storesLazyClosure && !lazyClosureStoredBy(user) && !mayBeStoredBeforeJump(user);
        if (!isPlainStore || names.contains(user.string()) || user.string().index() + 1 >= (1u << 24) || isNamedLastIndex(user.string()))
            break;
        result.more |= static_cast<uint64_t>(user.string().index() + 1) << (24 * result.values.size());
        result.values.append(user.child(1));
        names.append(user.string());
        m_cloneDefinitions.append(index);
    }
    return result;
}

CellPointer Lowering::finishObjectLiteral(CellPointer object)
{
    LBasicBlock hasOutOfLineProperties = newColdBlock();
    Join join(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
    LBasicBlock isFinished = m_out.newBlock();
    m_out.branch(m_out.isNull(m_out.loadPtr(object.value(), m_heaps.JSObject_butterfly)), usually(isFinished), rarely(hasOutOfLineProperties));
    m_out.appendTo(isFinished);
    join.add(object);
    m_out.appendTo(hasOutOfLineProperties);
    join.add(AOT_TYPED_OPERATION(FinishObjectLiteral, object));
    return join.finish().as<CellPointer>();
}

CellPointer Lowering::cloneObject(ValueID sourceID, bool keepsLayout, const CloneDefinitions& defined, const CloneCapacity& room)
{
    if (!keepsLayout)
        return cloneAsPlainObject(sourceID, defined);
    Boxed source = in<Boxed>(sourceID);
    auto outOfLine = [&] {
        return cloneObjectOutOfLine(sourceID, room);
    };
    auto& type = typeOf(sourceID);
    Vector<LayoutID, 2> layouts;
    if (keepsLayout && !Options::auditAOTTypedAssertions() && !Options::countAOTTypedAllocations())
        layouts = inlineClonedLayouts(type);
    layouts.removeAllMatching([&](LayoutID id) {
        return room.subtypesWithoutCapacity.contains(id);
    });
    if (layouts.isEmpty())
        return outOfLine();

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    orElse(isCell(source, type), slowPath);
    CellPointer cell = asCell(source);
    Word32 shapeWord = shapeWordOf(cell);
    Word32 orderWord = orderWordOf(cell);
    orElse(hasNoBits(orderWord, orderWordHasExtraProperties), slowPath);
    bool matchesAnyLayout = allSubtypesAreIn(type, layouts);
    for (LayoutID id : layouts) {
        auto& layout = m_program.at(id);
        LBasicBlock otherwise = id == layouts.last() ? slowPath : m_out.newBlock();
        branchOnSourceLayout(shapeWord, layout, matchesAnyLayout && otherwise == slowPath, otherwise);
        unlessBoxed(cell, layout, slowPath);
        CellPointer copy = copyRecord(cell, orderWord, layout, { });
        fenceAfterInitialization();
        results.append(m_out.anchor(copy.value()));
        m_out.jump(continuation);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
    m_out.appendTo(slowPath);
    results.append(m_out.anchor(outOfLine().value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, results));
}

bool Lowering::populatesFirstArgument(const Value& user) const
{
    if (user.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(user.builtin()).implementation;
    if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign)
        return true;
    return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
}

bool Lowering::isUnreachableFromHandler(ValueID id)
{
    auto known = m_isUnreachableFromHandler.find(id.index() + 1);
    if (known != m_isUnreachableFromHandler.end())
        return known->value;
    auto& terminator = m_currentBlock->terminator;
    BlockID next = terminator.targets[0].block;
    bool result = terminator.targets[1].block != next && !terminator.targets[1].arguments.contains(id);
    unsigned incomingEdges = 0;
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets)
            incomingEdges += target.block == next;
    }
    result &= incomingEdges == 1;
    for (ValueID user : usersOf(id)) {
        BlockID at(placeOf(user).first);
        result &= at == m_currentBlockID || dominates(next, at);
    }
    for (unsigned b = 0; result && isUsedByTerminator(id) && b < m_function.blocks.size(); ++b) {
        if (BlockID(b) == m_currentBlockID || dominates(next, BlockID(b)))
            continue;
        auto& other = m_function.blocks[b].terminator;
        result &= other.value != id;
        for (auto& target : other.targets)
            result &= !target.arguments.contains(id);
    }
    m_isUnreachableFromHandler.add(id.index() + 1, result);
    return result;
}

std::optional<size_t> Lowering::indexOfAllocatingUse(ValueID id) const
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return std::nullopt;
    if (m_function.at(id).opcode != Opcode::NewObject)
        return std::nullopt;
    if (m_function.at(id).hasInitialStructure() && m_program.initialStructures[m_function.at(id).initialStructure()].isWhole)
        return std::nullopt;
    auto& block = m_currentBlock->values;
    size_t index = notFound;
    if (block.size() <= 64)
        index = block.find(id);
    else {
        if (m_newObjectIndicesBlock != m_currentBlock) {
            m_newObjectIndicesBlock = m_currentBlock;
            m_newObjectIndices.clear();
            for (size_t k = 0; k < block.size(); ++k) {
                if (m_function.at(block[k]).opcode == Opcode::NewObject)
                    m_newObjectIndices.add(block[k].index(), k);
            }
        }
        if (auto place = m_newObjectIndices.find(id.index()); place != m_newObjectIndices.end())
            index = place->value;
    }
    if (index == notFound)
        return std::nullopt;
    for (size_t k = index + 1; k < block.size(); ++k) {
        auto& user = m_function.at(block[k]);
        unsigned uses = 0;
        user.forEachChild([&](ValueID child) {
            uses += child == id;
        });
        if (uses) {
            if (uses != 1 || !populatesFirstArgument(user) || user.arguments.size() < 2 || user.arguments[0] != id)
                return std::nullopt;
            bool isWatched = m_currentBlock->terminator.kind == TerminatorKind::Invoke && k + 1 == block.size();
            if (isWatched && !const_cast<Lowering*>(this)->isUnreachableFromHandler(id))
                return std::nullopt;
            return k;
        }
    }
    return std::nullopt;
}

const Lowering::NamedStores* Lowering::namedStoresAt(size_t index)
{
    static constexpr unsigned minStores = 3;
    static constexpr unsigned maxStores = 8;
    if (m_namedStoresBlock != m_currentBlock) {
        m_namedStoresBlock = m_currentBlock;
        m_namedStores.clear();
        auto& values = m_currentBlock->values;
        NamedStores open;
        auto close = [&] {
            if (open.size() >= minStores)
                m_namedStores.append(open);
            open.clear();
        };
        for (size_t i = 0; i < values.size() && m_currentBlock->terminator.kind != TerminatorKind::Invoke; ++i) {
            auto& value = m_function.at(values[i]);
            switch (value.opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
            case Opcode::ToBoolean:
            case Opcode::LogicalNot:
                continue;
            case Opcode::PutById:
                if (!value.isInitialStore() && value.string() != m_length && typeOf(value.child(0)).singleEngineClass() == &m_builtins.arrayClass()) {
                    bool goesOn = open.size() < maxStores && std::ranges::all_of(open, [&](size_t earlier) {
                        auto& store = m_function.at(values[earlier]);
                        return store.child(0) == value.child(0) && store.string() != value.string();
                    });
                    if (!goesOn)
                        close();
                    open.append(i);
                    continue;
                }
                break;
            default:
                break;
            }
            close();
        }
        close();
    }
    for (auto& stores : m_namedStores) {
        if (stores.contains(index))
            return &stores;
    }
    return nullptr;
}

void Lowering::lowerNamedStores(const NamedStores& stores)
{
    auto& values = m_currentBlock->values;
    auto storeAt = [&](unsigned i) -> const Value& {
        return m_function.at(values[stores[i]]);
    };
    unsigned count = stores.size();
    CellPointer cell;
    {
        SetForScope currentIndex(m_currentIndexInBlock, stores[0]);
        cell = nonNullish(storeAt(0).child(0), storeAt(0).string());
    }
    LValue buffer = m_out.lockedStackSlot(2 * count * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < count; ++i) {
        m_out.store64(in<Boxed>(storeAt(i).child(1)).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        m_out.store64(m_out.constInt64(storeAt(i).string().index()), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, (count + i) * sizeof(EncodedJSValue)));
    }
    LBasicBlock oneByOne = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Word32 isDone = AOT_TYPED_OPERATION(TryAddNamedProperties, cell, word32(count), Pointer(buffer));
    m_out.branch(isDone.value(), usually(continuation), rarely(oneByOne));
    m_out.appendTo(oneByOne);
    for (unsigned i = 0; i < count; ++i) {
        SetForScope currentIndex(m_currentIndexInBlock, stores[i]);
        emitPutById(box(cell), storeAt(i).string(), Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)))));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

const Field* Lowering::fieldOfStoreAfterSingleTestAt(size_t index)
{
    SetForScope currentIndex(m_currentIndexInBlock, index);
    auto& value = m_function.at(m_currentBlock->values[index]);
    if (value.opcode != Opcode::PutById || value.isInitialStore() || isPrivateName(m_program.string(value.string())))
        return nullptr;
    if (value.storesLazyClosure || lazyClosureStoredBy(value) || mayBeStoredBeforeJump(value) || objectLiteralAt(index) || initialStoresAt(index) || namedStoresAt(index) || propertyRunAt(index))
        return nullptr;
    ValueID object = value.child(0);
    auto& receiver = typeOf(object);
    LayoutID supertype = receiver.singleLayout();
    const Field* field = fieldFor(receiver, value.string());
    if (!supertype || !field || receiver.mayBeUndefinedOrNull() || isInitializingStore(m_program, m_function, *m_currentBlock, index))
        return nullptr;
    if (field->isMissingInSomeSubtype || field->hasOtherOffsetInSomeSubtype || m_program.at(field->slot).isDiscriminant)
        return nullptr;
    if (m_program.at(supertype).acceptsUntypedObjects && !isAllocatedAsSubtypeOf(object, supertype))
        return nullptr;
    bool mayConsultPrototypeChain = m_program.at(field->stored).primitives & Primitive::Absent && mayInherit(m_program, supertype, field->name);
    if ((mayConsultPrototypeChain && inheritsThroughLayoutPrototype(m_program, supertype, field->name)) || (!mayConsultPrototypeChain && !mayBeFrozen(supertype)))
        return nullptr;
    return field;
}

const Lowering::StoresAfterSingleTest* Lowering::storesAfterSingleTestAt(size_t index)
{
    static constexpr unsigned maxStores = 8;
    if (m_storesAfterSingleTestBlock != m_currentBlock) {
        m_storesAfterSingleTestBlock = m_currentBlock;
        m_storesAfterSingleTest.clear();
        auto& values = m_currentBlock->values;
        StoresAfterSingleTest open;
        bool mayRunCode = false;
        bool isSeparatedByRead = false;
        auto close = [&] {
            if (open.size() >= 2)
                m_storesAfterSingleTest.append(open);
            open.clear();
            mayRunCode = false;
            isSeparatedByRead = false;
        };
        auto isStoredTo = [&](StringID name) {
            return std::ranges::any_of(open, [&](size_t earlier) {
                return m_function.at(values[earlier]).string() == name;
            });
        };
        bool mayDefer = m_currentBlock->terminator.kind != TerminatorKind::Invoke && !Options::countAOTTypedAllocations() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts();
        for (size_t i = 0; i < values.size() && mayDefer; ++i) {
            auto& value = m_function.at(values[i]);
            if (const Field* field = fieldOfStoreAfterSingleTestAt(i)) {
                bool checksPrototypeChain = m_program.at(field->stored).primitives & Primitive::Absent;
                bool goesOn = open.size() < maxStores && !isStoredTo(value.string()) && !(checksPrototypeChain && isSeparatedByRead) && std::ranges::all_of(open, [&](size_t earlier) {
                    return withoutUpcasts(m_function.at(values[earlier]).child(0)) == withoutUpcasts(value.child(0));
                });
                if (!goesOn)
                    close();
                open.append(i);
                mayRunCode |= checksPrototypeChain;
                continue;
            }
            if (open.isEmpty())
                continue;
            bool readsVariable = value.opcode == Opcode::GetModuleVariable;
            if (mayInterleaveWithPropertyRun(i) && !readsVariable)
                continue;
            if (!mayRunCode && ((readsVariable && mayInterleaveWithPropertyRun(i)) || (isSideEffectFreeFieldRead(i) && !isStoredTo(value.string())))) {
                isSeparatedByRead = true;
                continue;
            }
            close();
        }
        close();
    }
    for (auto& stores : m_storesAfterSingleTest) {
        if (stores.contains(index))
            return &stores;
    }
    return nullptr;
}

void Lowering::lowerStoresAfterSingleTest(const StoresAfterSingleTest& stores)
{
    auto& values = m_currentBlock->values;
    auto storeAt = [&](size_t index) -> const Value& {
        return m_function.at(values[index]);
    };
    CellPointer cell = in<CellPointer>(storeAt(stores.first()).child(0));
    uint32_t bits = 0;
    for (size_t index : stores) {
        LayoutID supertype = typeOf(storeAt(index).child(0)).singleLayout();
        const Field& field = *fieldOfStoreAfterSingleTestAt(index);
        if (mayBeFrozen(supertype))
            bits |= orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible;
        if (m_program.at(field.stored).primitives & Primitive::Absent && mayInherit(m_program, supertype, field.name))
            bits |= orderWordHasExtraProperties;
    }
    LBasicBlock asks = newColdBlock();
    LBasicBlock storesInline = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasNoBits(orderWordOf(cell), bits).value(), usually(storesInline), rarely(asks));
    m_out.appendTo(asks);
    if (bits & orderWordHasExtraProperties) {
        LBasicBlock checksPrototypeChain = newColdBlock();
        Word32 usesObjectLookup = hasPrototypeOfLayout(cell);
        if (uint32_t others = bits & ~orderWordHasExtraProperties)
            usesObjectLookup = logicalAnd(usesObjectLookup, hasNoBits(orderWordOf(cell), others));
        m_out.branch(usesObjectLookup.value(), unsure(storesInline), unsure(checksPrototypeChain));
        m_out.appendTo(checksPrototypeChain);
    }
    for (size_t index : stores) {
        SetForScope currentIndex(m_currentIndexInBlock, index);
        auto& store = storeAt(index);
        const Field& field = *fieldOfStoreAfterSingleTestAt(index);
        if (m_program.at(field.stored).primitives & Primitive::Absent && mayInherit(m_program, typeOf(store.child(0)).singleLayout(), field.name)) {
            setByName(box(cell), store.string(), in<Boxed>(store.child(1)));
            continue;
        }
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, store.child(0)));
        OperationEffects effects { Writes::Fields, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(store.string().index()), in<Boxed>(store.child(1)));
    }
    m_out.jump(continuation);
    m_out.appendTo(storesInline);
    {
        SetForScope orderWordHasNoFlag(m_orderWordHasNoFlag, true);
        for (size_t index : stores) {
            SetForScope currentIndex(m_currentIndexInBlock, index);
            writeField(cell, storeAt(index).child(0), *fieldOfStoreAfterSingleTestAt(index), storeAt(index).child(1));
        }
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

ValueID Lowering::withoutUpcasts(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
        id = m_function.at(id).child(0);
    return id;
}

bool Lowering::isLazyClosureStore(const Value& closure, const Value& store)
{
    if (store.opcode != Opcode::PutById || store.isInitialStore() || store.isAtKnownSlot() || Options::auditAOTTypedAssertions() || closure.opcode != Opcode::NewClosure || closure.arguments.size() != 1)
        return false;
    if (withoutUpcasts(closure.arguments[0]) != withoutUpcasts(store.child(0)) || !typeOf(store.child(0)).anyObject || fieldFor(typeOf(store.child(0)), store.string()))
        return false;
    auto& callee = m_program.at(closure.function());
    if (callee.kind == FunctionKind::Constructor || m_program.isResumeFunction(callee) || m_program.closureReferenceAndCaptureCounts(callee) != std::pair(1u, 1u))
        return false;
    return !isPrivateName(m_program.string(store.string()));
}

void Lowering::putLazyClosure(Boxed holder, StringID name, FunctionID function)
{
    constexpr unsigned isDirect = 1;
    constexpr unsigned isStrict = 2;
    auto slot = allocateSite(name, isDirect | isStrict | AOT::Site::storesLazyClosure);
    if (!slot) {
        AOT_TYPED_OPERATION(PutLazyClosure, holder, word32(genericEntrypointFor(function).index()), word32(name.index()));
        return;
    }
    Boxed cell = [&] {
        SetForScope fetchesInline(m_isInLoop, true);
        return box(closureConstant(function));
    }();
    Pointer address = slotAddress(*slot);
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(holder).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer object = asCell(holder);
    TypedPointer structureID(m_heaps.root, m_out.add(object.value(), m_out.constIntPtr(JSCell::structureIDOffset())));
    Integer word(m_out.load64(TypedPointer(m_heaps.root, address.value())));
    static_assert(OBJECT_OFFSETOF(AOT::Slot, newStructureID) == sizeof(uint64_t));
    LValue newStructureID = m_out.load32(TypedPointer(m_heaps.root, m_out.add(address.value(), m_out.constIntPtr(sizeof(uint64_t)))));
    m_out.branch(equal(low32(word), Word32(m_out.load32(structureID))).value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    LValue location = m_out.aShr(m_out.shl(word.value(), m_out.constInt32(32 - AOT::Slot::offsetBits)), m_out.constInt32(64 - AOT::Slot::offsetBits));
    LValue storage = m_out.select(m_out.lessThan(location, m_out.constInt64(0)), m_out.load64(TypedPointer(m_heaps.root, m_out.add(object.value(), m_out.constIntPtr(JSObject::butterflyOffset())))), object.value());
    m_out.store64(cell.value(), TypedPointer(m_heaps.root, m_out.add(storage, m_out.shl(location, m_out.constInt32(3)))));
    m_out.store32(newStructureID, structureID);
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        callICStub(Stub::PutById, Void, { { holder.value(), firstStubOperandGPR }, { cell.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { }, std::pair { *slot, GPRInfo::argumentGPR2 });
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

std::optional<FunctionID> Lowering::lazyClosureStoredBy(const Value& store)
{
    if (store.opcode != Opcode::PutById)
        return std::nullopt;
    ValueID stored = store.child(1);
    auto& closure = m_function.at(stored);
    if (!isLazyClosureStore(closure, store) || useCountOf(stored) != 1 || placeOf(stored).first != static_cast<uint32_t>(m_currentBlock - m_function.blocks.span().data()))
        return std::nullopt;
    return closure.function();
}

const Value* Lowering::lazyClosureStoreAfterJump(const Block& from)
{
    if (from.terminator.kind != TerminatorKind::Jump || from.values.isEmpty())
        return nullptr;
    ValueID closure = from.values.last();
    auto& target = from.terminator.targets[0];
    size_t index = target.arguments.find(closure);
    if (m_function.at(closure).opcode != Opcode::NewClosure || useCountOf(closure) != 1 || index == notFound)
        return nullptr;
    auto& to = m_function.at(target.block);
    if (to.values.isEmpty() || (to.terminator.kind == TerminatorKind::Invoke && to.values.size() == 1) || useCountOf(to.parameters[index]) != 1)
        return nullptr;
    auto& store = m_function.at(to.values.first());
    return store.opcode == Opcode::PutById && store.child(1) == to.parameters[index] && !to.parameters.contains(withoutUpcasts(store.child(0))) && isLazyClosureStore(m_function.at(closure), store) ? &store : nullptr;
}

bool Lowering::mayBeStoredBeforeJump(const Value& store)
{
    if (store.opcode != Opcode::PutById || m_currentBlock->values.isEmpty() || &m_function.at(m_currentBlock->values.first()) != &store || !m_currentBlock->parameters.contains(store.child(1)))
        return false;
    for (auto& from : m_function.blocks) {
        if (lazyClosureStoreAfterJump(from) == &store)
            return true;
    }
    return false;
}

bool Lowering::isLazilyCreated(ValueID closure)
{
    auto users = usersOf(closure);
    return useCountOf(closure) == 1 && users.size() == 1 && m_function.at(users[0]).storesLazyClosure && m_function.at(users[0]).child(1) == closure;
}

bool Lowering::isCalledWithoutAllocation(ValueID read)
{
    auto& value = m_function.at(read);
    if (value.opcode != Opcode::GetById || isUsedByTerminator(read))
        return false;
    const Field* field = fieldFor(typeOf(value.child(0)), value.string());
    if (!field || !(field->flags & fieldHoldsLazyClosure) || joinRepFor(typeOf(read)).rep != Rep::JSValue)
        return false;
    ValueID test;
    for (ValueID user : usersOf(read)) {
        auto& use = m_function.at(user);
        if (use.opcode != Opcode::IsClosureOf || use.function() != field->lazyClosure || use.immediates[1] == closureIsConstructingCallee)
            continue;
        if (test)
            return false;
        test = user;
    }
    if (!test || useCountOf(test) != 1)
        return false;
    auto& terminator = m_function.blocks[placeOf(test).first].terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.value != test || terminator.targets[0].block == terminator.targets[1].block || numberOfPredecessorsOf(terminator.targets[1].block) != 1)
        return false;
    for (ValueID user : usersOf(read)) {
        auto& use = m_function.at(user);
        if (user == test || (use.opcode == Opcode::GetCapture && use.function() == field->lazyClosure))
            continue;
        if (placeOf(user).first != terminator.targets[1].block.index())
            return false;
    }
    return true;
}

bool Lowering::isStoredAsLazyClosure(ValueID closure)
{
    if (useCountOf(closure) != 1)
        return false;
    auto users = usersOf(closure);
    if (users.empty())
        return false;
    auto [block, index] = placeOf(users[0]);
    auto& user = m_function.at(users[0]);
    if (block != static_cast<uint32_t>(m_currentBlock - m_function.blocks.span().data()) || index <= m_currentIndexInBlock || user.opcode != Opcode::PutById || user.child(1) != closure)
        return false;
    return !!lazyClosureStoredBy(user);
}

std::optional<Lowering::PropertyRunStore> Lowering::propertyRunStoreAt(size_t index)
{
    auto& value = m_function.at(m_currentBlock->values[index]);
    PropertyRunStore store;
    switch (value.opcode) {
    case Opcode::PutById:
        if (value.isInitialStore() || value.isAtKnownSlot() || value.isFirstStoreToThis() || value.storesLazyClosure || lazyClosureStoredBy(value) || mayBeStoredBeforeJump(value))
            return std::nullopt;
        store = { index, value.child(0), value.string(), { }, value.child(1), false };
        break;
    case Opcode::PutByVal:
        if (!isPredictedSymbol(m_function, value.child(1)))
            return std::nullopt;
        store = { index, value.child(0), { }, value.child(1), value.child(2), false };
        break;
    case Opcode::CallBuiltin:
        if (isPrivateFieldAdd(value)) {
            store = { index, value.arguments[0], { }, value.arguments[1], value.arguments[2], true };
            break;
        }
        if (!isDefineField(value) || m_function.at(value.arguments[1]).opcode != Opcode::String)
            return std::nullopt;
        store = { index, value.arguments[0], m_function.at(value.arguments[1]).string(), { }, value.arguments[2], true };
        break;
    default:
        return std::nullopt;
    }
    if (!typeOf(store.object).anyObject || m_function.at(store.object).prediction() || objectLiteralAt(index) || spreadLiteralAt(index))
        return std::nullopt;
    if (store.name) {
        auto text = m_program.string(store.name);
        bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
            return isASCIIDigit(character);
        });
        if (mayBeIndex || isPrivateName(text) || equalSpans(text, asBytes("__proto__"_s.span8())) || fieldFor(typeOf(store.object), store.name))
            return std::nullopt;
    }
    return store;
}

bool Lowering::mayInterleaveWithPropertyRun(size_t index)
{
    auto& value = m_function.at(m_currentBlock->values[index]);
    auto isNotVariableRead = [&](ValueID id) {
        return m_function.at(withoutUpcasts(id)).opcode != Opcode::GetModuleVariable;
    };
    switch (value.opcode) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::FunctionReference:
    case Opcode::BuiltinFunction:
    case Opcode::ConstantSymbol:
    case Opcode::GetArgument:
    case Opcode::GetRestLength:
        return true;
    case Opcode::Cast:
        return value.castReason() == CastReason::Upcast;
    case Opcode::GetModuleVariable:
        return !m_program.variables[value.variable().index()].mayBeReadUninitialized();
    case Opcode::NewArray:
    case Opcode::NewClosure:
        return std::ranges::all_of(value.arguments, isNotVariableRead);
    case Opcode::LogicalNot:
    case Opcode::IsUndefinedOrNull:
    case Opcode::TypeOfIs:
    case Opcode::ToBoolean:
        return isNotVariableRead(value.child(0));
    case Opcode::StrictEq:
        return isNotVariableRead(value.child(0)) && isNotVariableRead(value.child(1));
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Negate:
    case Opcode::BitNot:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return writesOfArithmetic(typeOf(value.child(0)), value.child(1) ? &typeOf(value.child(1)) : nullptr) == Writes::Nothing && isNotVariableRead(value.child(0)) && (!value.child(1) || isNotVariableRead(value.child(1)));
    case Opcode::NewObject:
    case Opcode::CallBuiltin: {
        auto* literal = objectLiteralAt(index);
        return literal && !literal->newTarget && !literal->descriptor && (literal->allocation == index || isNotVariableRead(value.arguments[2]));
    }
    default:
        return false;
    }
}

std::optional<FunctionID> Lowering::functionOfPrototypeRead(ValueID id) const
{
    auto& value = m_function.at(withoutUpcasts(id));
    if (!m_program.runsOnce(m_id) || m_isInLoop || value.opcode != Opcode::GetById || !equalSpans(m_program.string(value.string()), asBytes("prototype"_s.span8())))
        return std::nullopt;
    auto& function = m_function.at(withoutUpcasts(value.child(0)));
    if (function.opcode != Opcode::FunctionReference)
        return std::nullopt;
    return function.function();
}

std::optional<Boxed> Lowering::lowerPrivateAccessAtSite(const Value& value, bool isStore)
{
    auto& key = m_function.at(withoutUpcasts(value.arguments[1]));
    if (key.opcode != Opcode::GetModuleVariable || !m_program.variables[key.variable().index()].isStoredOnce)
        return std::nullopt;
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    unsigned slot = m_context.sites.size();
    m_context.sites.append(Site { });
    Boxed base = in<Boxed>(value.arguments[0]);
    Boxed name = in<Boxed>(value.arguments[1]);
    Boxed stored = isStore ? in<Boxed>(value.arguments[2]) : Boxed();
    Pointer cache = slotAddress(slot);
    LBasicBlock miss = newColdBlock();
    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID) && AOT::Slot::directLocationMask == UINT8_MAX);
    if (get(value.arguments[0]).rep != Rep::Cell)
        orElse(isCell(base), miss);
    CellPointer cell = asCell(base);
    orElse(equal(Word32(m_out.load32(TypedPointer(m_heaps.AOTTyped_collectorState, cache.value()))), loadField<Word32>(cell, m_heaps.JSCell_structureID)), miss);
    LValue location = m_out.zeroExtPtr(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, cache.value(), OBJECT_OFFSETOF(AOT::Slot, offset))));
    TypedPointer place(m_heaps.properties.atAnyNumber(), m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3))));
    Join join(m_out, Rep::JSValue, Int64);
    if (isStore) {
        didEmitWrite(Writes::Heap);
        m_out.store64(stored.value(), place);
        if (typeOf(value.arguments[2]).hasCells())
            barrier(cell);
        join.add(undefined());
    } else
        join.add(Boxed(m_out.load64(place)));

    m_out.appendTo(miss);
    if (isStore) {
        callICStub(Stub::PrivateSetAtSite, Void, { { base.value(), firstStubOperandGPR }, { name.value(), GPRInfo::argumentGPR1 }, { stored.value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value), std::pair { slot, GPRInfo::argumentGPR3 });
        join.add(undefined());
    } else
        join.add(Boxed(callICStub(Stub::PrivateGetAtSite, Int64, { { base.value(), firstStubOperandGPR }, { name.value(), GPRInfo::argumentGPR1 } }, std::nullopt, builtinCallEffects(value), std::pair { slot, GPRInfo::argumentGPR2 })));
    return join.finish().as<Boxed>();
}

bool Lowering::mayBeCreatedWithPropertyRun(ValueID id)
{
    auto& closure = m_function.at(id);
    if (closure.opcode != Opcode::NewClosure || closure.arguments.size() != 1 || closure.isStackAllocatedClosure() || useCountOf(id) != 1 || placeOf(id).first != static_cast<uint32_t>(m_currentBlock - m_function.blocks.span().data()))
        return false;
    auto& callee = m_program.at(closure.function());
    if (!m_program.usesSlimClosures(callee) || m_program.isResumeFunction(callee) || callee.kind == FunctionKind::Constructor || callee.captureTypes.size() != 1)
        return false;
    Rep rep = repOf(m_program.at(callee.captureTypes[0]));
    return (rep == Rep::JSValue || rep == Rep::Cell) && !Options::countAOTTypedAllocations() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions();
}

bool Lowering::isCreatedWithPropertyRun(ValueID id)
{
    if (!mayBeCreatedWithPropertyRun(id))
        return false;
    auto users = usersOf(id);
    if (users.empty())
        return false;
    auto [block, index] = placeOf(users[0]);
    if (block != static_cast<uint32_t>(m_currentBlock - m_function.blocks.span().data()))
        return false;
    auto* run = propertyRunAt(index);
    return run && run->closureStoresInRun.containsIf([&](unsigned store) {
        return run->stores[store].stored == id;
    });
}

bool Lowering::isKeyReadOnPropertyRunMiss(ValueID id)
{
    if (m_function.at(id).opcode != Opcode::GetModuleVariable || !m_program.variables[m_function.at(id).variable().index()].isStoredOnce || useCountOf(id) != 1)
        return false;
    auto users = usersOf(id);
    if (users.empty())
        return false;
    auto [block, index] = placeOf(users[0]);
    if (block != static_cast<uint32_t>(m_currentBlock - m_function.blocks.span().data()) || block != placeOf(id).first)
        return false;
    auto* run = propertyRunAt(index);
    return run && run->keysAreFixed && run->stores.containsIf([&](auto& store) {
        return store.index == index && store.key == id;
    });
}

const Lowering::PropertyRun* Lowering::propertyRunAt(size_t index)
{
    static constexpr unsigned minStores = 3;
    static constexpr unsigned maxStores = 64;
    static constexpr unsigned maxReplacingStores = 32;
    if (m_propertyRunsBlock != m_currentBlock) {
        m_propertyRunsBlock = m_currentBlock;
        m_propertyRuns.clear();
        auto& values = m_currentBlock->values;
        PropertyRun open;
        auto& stores = open.stores;
        auto close = [&] {
            while (!open.elementStores.isEmpty() && (stores.isEmpty() || open.elementStores.last() > stores.last().index))
                open.elementStores.removeLast();
            unsigned numberOfSlots = 1;
            for (auto& store : stores)
                numberOfSlots += !!store.key;
            open.keysAreFixed = numberOfSlots > 1 && std::ranges::all_of(stores, [&](auto& store) {
                if (!store.key)
                    return true;
                auto& key = m_function.at(withoutUpcasts(store.key));
                return key.opcode == Opcode::GetModuleVariable && m_program.variables[key.variable().index()].isStoredOnce;
            });
            if (open.keysAreFixed)
                numberOfSlots = 1;
            open.mayReplace = !open.keysAreFixed && numberOfSlots == 1 && open.elementStores.isEmpty() && !open.mayBePrototype && !stores.isEmpty() && stores.size() <= maxReplacingStores && (m_isInLoop || m_program.isHotOutsideLoops(m_id));
            bool hasNoKeysToCompare = numberOfSlots == 1;
            if (numberOfSlots == 1)
                numberOfSlots = 2;
            if (open.mayReplace)
                numberOfSlots += (stores.size() - 1) / sizeof(void*);
            open.mayFindExistingProperties = (m_isInLoop || !m_program.runsOnce(m_id)) && hasNoKeysToCompare && open.elementStores.isEmpty() && !open.mayBePrototype && stores.size() >= minStores && stores.size() <= maxReplacingStores
                && std::ranges::count_if(stores, [&](auto& store) { return mayBeCreatedWithPropertyRun(store.stored); }) < 2
                && std::ranges::any_of(usersOf(withoutUpcasts(stores.first().object)), [&](ValueID user) {
                    auto [userBlock, userIndex] = placeOf(user);
                    if (BlockID(userBlock) == m_currentBlockID)
                        return userIndex < stores.first().index;
                    return !dominates(m_currentBlockID, BlockID(userBlock));
                });
            if (open.mayFindExistingProperties)
                numberOfSlots += 1 + (stores.size() + sizeof(void*) - 1) / sizeof(void*);
            if (stores.size() >= minStores && m_context.sites.size() + numberOfSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
                open.slot = m_context.sites.size();
                for (unsigned k = 0; k < numberOfSlots; ++k)
                    m_context.sites.append(Site { });
                for (unsigned k = 0; k < stores.size() && !open.mayReplace && open.elementStores.isEmpty() && !open.mayBePrototype; ++k) {
                    if (mayBeCreatedWithPropertyRun(stores[k].stored))
                        open.closureStoresInRun.append(k);
                }
                if (open.closureStoresInRun.size() < 2)
                    open.closureStoresInRun.clear();
                bool hasNoKeyToCompare = open.keysAreFixed || std::ranges::none_of(stores, [](auto& store) { return !!store.key; });
                bool mayBeCut = (m_isInLoop || !m_program.runsOnce(m_id)) && hasNoKeyToCompare && open.elementStores.isEmpty() && !open.mayBePrototype && open.closureStoresInRun.isEmpty()
                    && m_function.at(withoutUpcasts(stores.first().object)).opcode != Opcode::GetModuleVariable;
                unsigned pieceStart = 0;
                auto closePiece = [&](unsigned end) {
                    constexpr unsigned pieceSlots = 2;
                    if (end - pieceStart >= minStores && end - pieceStart < stores.size() && m_context.sites.size() + pieceSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
                        open.pieces.append({ pieceStart, end - pieceStart, static_cast<unsigned>(m_context.sites.size()) });
                        for (unsigned k = 0; k < pieceSlots; ++k)
                            m_context.sites.append(Site { });
                    }
                    pieceStart = end + 1;
                };
                for (unsigned k = 0; mayBeCut && k < stores.size(); ++k) {
                    if (stores[k].name && m_program.possibleAccessorNames.get(stores[k].name.index()))
                        closePiece(k);
                }
                if (mayBeCut)
                    closePiece(stores.size());
                m_propertyRuns.append(open);
            }
            open.pieces.clear();
            open.closureStoresInRun.clear();
            stores.clear();
            open.elementStores.clear();
            open.mayBePrototype = false;
            open.mayFindExistingProperties = false;
        };
        auto isSameValueOrVariable = [&](ValueID a, ValueID b) {
            auto& first = m_function.at(withoutUpcasts(a));
            auto& second = m_function.at(withoutUpcasts(b));
            if (&first == &second || (first.opcode == Opcode::GetModuleVariable && second.opcode == Opcode::GetModuleVariable && first.variable() == second.variable()))
                return true;
            auto function = functionOfPrototypeRead(a);
            return function && function == functionOfPrototypeRead(b);
        };
        bool defersElementStores = m_program.runsOnce(m_id) && !m_isInLoop;
        auto isElementStoreTo = [&](size_t i, ValueID object) {
            auto& value = m_function.at(values[i]);
            if (!defersElementStores || value.opcode != Opcode::PutByVal || !isSameValueOrVariable(value.child(0), object))
                return false;
            auto& key = m_function.at(withoutUpcasts(value.child(1)));
            return key.opcode == Opcode::Integer && key.integer() >= 0 && key.integer() <= static_cast<int64_t>(MAX_ARRAY_INDEX);
        };
        for (size_t i = 0; i < values.size() && m_currentBlock->terminator.kind != TerminatorKind::Invoke; ++i) {
            if (auto store = propertyRunStoreAt(i)) {
                bool goesOn = stores.size() < maxStores && std::ranges::all_of(stores, [&](auto& earlier) {
                    if (!isSameValueOrVariable(earlier.object, store->object))
                        return false;
                    return store->key ? !earlier.key || !isSameValueOrVariable(earlier.key, store->key) : earlier.name != store->name;
                });
                if (!goesOn)
                    close();
                stores.append(*store);
                open.mayBePrototype |= !!functionOfPrototypeRead(store->object);
                continue;
            }
            if (!stores.isEmpty() && isElementStoreTo(i, stores.first().object)) {
                open.elementStores.append(i);
                continue;
            }
            if (!mayInterleaveWithPropertyRun(i) && !functionOfPrototypeRead(values[i]))
                close();
        }
        close();
    }
    auto run = std::ranges::partition_point(m_propertyRuns, [&](auto& earlier) {
        return earlier.stores.last().index < index;
    });
    if (run == m_propertyRuns.end())
        return nullptr;
    bool hasProperty = std::ranges::binary_search(run->elementStores, index) || run->stores.containsIf([&](auto& store) {
        return store.index == index;
    });
    return hasProperty ? &*run : nullptr;
}

void Lowering::lowerPropertyRun(const PropertyRun& run, const Vector<Boxed, 8>* currentValues)
{
    auto& block = m_currentBlock->values;
    auto& stores = run.stores;
    unsigned count = stores.size();
    Boxed base = in<Boxed>(stores[0].object);
    Vector<Boxed, 8> values;
    Vector<Boxed, 8> keys;
    uint64_t keyed = 0;
    bool storesCells = false;
    auto& closures = run.closureStoresInRun;
    for (unsigned i = 0; i < count; ++i) {
        values.append(currentValues ? currentValues->at(i) : closures.contains(i) ? Boxed() : in<Boxed>(stores[i].stored));
        keys.append(stores[i].key && !isKeyReadOnPropertyRunMiss(stores[i].key) ? in<Boxed>(stores[i].key) : Boxed());
        keyed |= static_cast<uint64_t>(!!stores[i].key) << i;
        storesCells |= typeOf(stores[i].stored).hasCells();
    }
    Pointer cache = slotAddress(run.slot);
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock miss = newColdBlock();
    LBasicBlock ask = newColdBlock();
    LBasicBlock oneByOne = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();

    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID) && OBJECT_OFFSETOF(AOT::Slot, offset) == sizeof(uint32_t));
    Integer word(m_out.load64(inCache(0)));
    bool hasElementStores = !run.elementStores.isEmpty();
    if (hasElementStores || run.mayBePrototype)
        orElse(boolean(false), miss);
    if (get(stores[0].object).rep != Rep::Cell)
        orElse(isCell(base), miss);
    CellPointer cell = asCell(base);
    auto* bufferSlot = m_out.m_proc.addStackSlot((keyed ? 2 : 1) * count * sizeof(EncodedJSValue));
    auto bufferAddress = [&]() -> LValue {
        return m_out.m_block->appendNew<B3::SlotBaseValue>(m_out.m_proc, m_out.origin(), bufferSlot);
    };
    LValue buffer = nullptr;
    auto inBuffer = [&](unsigned i) {
        return m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue));
    };
    auto writeValuesToBuffer = [&] {
        buffer = bufferAddress();
        for (unsigned i = 0; i < count; ++i) {
            m_out.store64(values[i].value(), inBuffer(i));
            if (keys[i])
                m_out.store64(keys[i].value(), inBuffer(count + i));
        }
    };
    LBasicBlock isNotKnown = newColdBlock();
    auto putWithStorage = [&] {
        LBasicBlock areStored = newColdBlock();
        LValue isDone = callICStub(Stub::PutPropertiesWithStorage, Int64, { { cell.value(), GPRInfo::argumentGPR1 }, { m_out.add(cache.value(), m_out.constIntPtr(sizeof(AOT::Slot))), GPRInfo::argumentGPR2 }, { buffer, GPRInfo::argumentGPR3 } }, std::pair { stubTemporaryGPRs[0], count }, { Writes::Heap, false });
        m_out.branch(m_out.notZero64(isDone), usually(areStored), rarely(isNotKnown));
        m_out.appendTo(areStored);
        barrier(cell);
        m_out.jump(continuation);
    };
    if (run.mayReplace) {
        LBasicBlock adds = m_out.newBlock();
        LBasicBlock isNotFirst = newUnlikelyBlock();
        LBasicBlock isKnown = newUnlikelyBlock();
        LBasicBlock replaces = newUnlikelyBlock();
        LBasicBlock hasStorage = newColdBlock();
        Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
        m_out.branch(equal(low32(word), structureID).value(), unsure(adds), unsure(isNotFirst));
        m_out.appendTo(isNotFirst);
        Integer second(m_out.load64(inCache(sizeof(AOT::Slot))));
        m_out.branch(equal(low32(second), structureID).value(), unsure(isKnown), rarely(miss));
        m_out.appendTo(isKnown);
        m_out.branch(m_out.testNonZero64(second.value(), m_out.constInt64(static_cast<uint64_t>(AOT::Slot::pointerIsNotCell) << 32)), usually(replaces), rarely(hasStorage));
        m_out.appendTo(replaces);
        didEmitWrite(Writes::Heap);
        LValue places = nullptr;
        for (unsigned i = 0; i < count; ++i) {
            if (!(i % sizeof(void*)))
                places = m_out.load64(inCache((1 + i / sizeof(void*)) * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)));
            LValue place = m_out.bitAnd(m_out.lShr(places, m_out.constInt32(i % sizeof(void*) * 8)), m_out.constInt64(0xff));
            m_out.store64(values[i].value(), TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(cell.value(), m_out.shl(place, m_out.constInt32(3)))));
        }
        if (storesCells)
            barrier(cell);
        m_out.jump(continuation);
        m_out.appendTo(hasStorage);
        writeValuesToBuffer();
        putWithStorage();
        m_out.appendTo(adds);
    } else
        orElse(equal(low32(word), loadField<Word32>(cell, m_heaps.JSCell_structureID)), miss);
    unsigned numberOfKeys = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (keys[i] && !run.keysAreFixed)
            orElse(equal(Boxed(m_out.load64(inCache(++numberOfKeys * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)))), keys[i]), miss);
    }
    auto createClosuresIndividually = [&] {
        for (unsigned i : closures) {
            SetForScope currentIndex(m_currentIndexInBlock, static_cast<size_t>(placeOf(stores[i].stored).second));
            values[i] = box(lowerNewClosure(m_function.at(stores[i].stored)));
        }
    };
    WTF::Function<LValue(unsigned)> inlineClosure;
    auto storeValues = [&] {
        didEmitWrite(Writes::Heap);
        LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::directLocationMask));
        LValue first = m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3)));
        for (unsigned i = 0; i < count; ++i) {
            size_t which = inlineClosure ? closures.find(i) : notFound;
            m_out.store64(which == notFound ? values[i].value() : inlineClosure(which), m_out.address(m_heaps.properties.atAnyNumber(), first, i * sizeof(EncodedJSValue)));
        }
        if (inlineClosure)
            fenceAfterInitialization();
        m_out.store32(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, newStructureID))), cell.value(), m_heaps.JSCell_structureID);
        barrier(cell);
        m_out.jump(continuation);
    };
    if (!closures.isEmpty()) {
        constexpr size_t cellSize = 32;
        static_assert(SlimClosure::allocationSize(1) <= cellSize && cellSize - SlimClosure::allocationSize(1) < MarkedSpace::sizeStep);
        LBasicBlock intervalIsTooShort = newColdBlock();
        Vector<LValue, 8> captures;
        for (unsigned i : closures) {
            auto& closure = m_function.at(stores[i].stored);
            captures.append(ensure(get(closure.arguments[0]), typeOf(closure.arguments[0]), m_program.at(m_program.at(closure.function()).captureTypes[0])).value);
        }
        LValue cells = allocateAdjacentHeapCells(m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedSlimClosureAllocator(1))), cellSize, closures.size(), intervalIsTooShort);
        applyEffects(cells->as<PatchpointValue>(), { Writes::RuntimeState, false });
        std::array<LValue, numberOfSlimClosureKinds> headers { };
        for (unsigned i : closures) {
            auto& entrypoint = m_program.at(genericEntrypointFor(m_function.at(stores[i].stored).function()));
            LValue& header = headers[slimClosureKindOf(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs)];
            if (!header)
                header = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedSlimClosureHeader(slimClosureKindOf(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs))));
        }
        inlineClosure = [&](unsigned k) {
            FunctionID general = genericEntrypointFor(m_function.at(stores[closures[k]].stored).function());
            auto& entrypoint = m_program.at(general);
            LValue created = m_out.add(cells, m_out.constIntPtr(k * cellSize));
            storeHeader(created, headers[slimClosureKindOf(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs)]);
            m_out.store64(captures[k], created, m_heaps.AOTTypedSlimClosure_captures[0]);
            m_out.store64(marked(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedFunctions()), static_cast<ptrdiff_t>(general.index()) * sizeof(uint64_t))), IsFixed), created, m_heaps.AOTTypedSlimClosure_word);
            return created;
        };
        storeValues();
        inlineClosure = nullptr;
        m_out.appendTo(intervalIsTooShort);
        createClosuresIndividually();
    }
    storeValues();

    m_out.appendTo(miss);
    if (run.mayFindExistingProperties) {
        size_t header = (2 + (run.mayReplace ? (count - 1) / sizeof(void*) : 0)) * sizeof(AOT::Slot);
        LBasicBlock isObject = newColdBlock();
        LBasicBlock hasSlots = newColdBlock();
        LBasicBlock hasNoSlots = newColdBlock();
        m_out.branch(isCell(base).value(), unsure(isObject), unsure(hasNoSlots));
        m_out.appendTo(isObject);
        m_out.branch(equal(Word32(m_out.load32(inCache(header + OBJECT_OFFSETOF(AOT::Slot, structureID)))), loadField<Word32>(cell, m_heaps.JSCell_structureID)).value(), unsure(hasSlots), unsure(hasNoSlots));
        m_out.appendTo(hasSlots);
        didEmitWrite(Writes::Heap);
        for (unsigned i = 0; i < count; ++i) {
            LValue place = m_out.load8ZeroExt32(inCache(header + (1 + i / sizeof(void*)) * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer) + i % sizeof(void*)));
            m_out.store64(values[i].value(), TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(cell.value(), m_out.shl(m_out.zeroExtPtr(place), m_out.constInt32(3)))));
        }
        m_out.store32(m_out.load32(inCache(header + OBJECT_OFFSETOF(AOT::Slot, newStructureID))), cell.value(), m_heaps.JSCell_structureID);
        if (storesCells)
            barrier(cell);
        m_out.jump(continuation);
        m_out.appendTo(hasNoSlots);
    }
    createClosuresIndividually();
    for (unsigned i = 0; i < count; ++i) {
        if (stores[i].key && !keys[i])
            keys[i] = convertTo<Boxed>(lowerGetModuleVariable(stores[i].key, m_function.at(stores[i].key)), typeOf(stores[i].key));
    }
    writeValuesToBuffer();
    if (!run.pieces.isEmpty()) {
        constexpr uint64_t isPermanentlyRejected = static_cast<uint64_t>(AOT::Slot::attemptsMask | AOT::Slot::isGetter) << 32;
        LBasicBlock canBeEvicted = newColdBlock();
        m_out.branch(equal(Integer(m_out.bitAnd(word.value(), integer(isPermanentlyRejected | UINT32_MAX).value())), integer(isPermanentlyRejected)).value(), unsure(oneByOne), unsure(canBeEvicted));
        m_out.appendTo(canBeEvicted);
    }
    if (!keyed && !hasElementStores && !run.mayBePrototype && !run.mayReplace) {
        LBasicBlock isObject = newColdBlock();
        m_out.branch(isCell(base).value(), unsure(isObject), unsure(isNotKnown));
        m_out.appendTo(isObject);
        putWithStorage();
    } else
        m_out.jump(isNotKnown);
    m_out.appendTo(isNotKnown);
    if (run.mayReplace)
        buffer = bufferAddress();
    constexpr uint64_t isTemporarilyRejected = static_cast<uint64_t>(AOT::Slot::attemptsMask | AOT::Slot::isGetter) << 32;
    m_out.branch(equal(Integer(m_out.bitAnd(word.value(), integer(isTemporarilyRejected | UINT32_MAX).value())), integer(isTemporarilyRejected)).value(), rarely(oneByOne), usually(ask));
    m_out.appendTo(ask);
    Vector<uint32_t, 16> nameIndicesOf;
    for (auto& store : stores)
        nameIndicesOf.append(store.key ? 0 : store.name.index());
    Word32 isDone = AOT_TYPED_OPERATION(TryPutProperties, cache, base, integer(static_cast<uint64_t>(rowOf(nameIndicesOf.span())) << 32 | count | (hasElementStores ? 1u << 31 : 0) | (run.mayBePrototype ? 1u << 30 : 0) | (run.mayReplace ? 1u << 29 : 0) | (run.keysAreFixed ? 1u << 28 : 0) | (run.mayFindExistingProperties ? 1u << 27 : 0)), integer(keyed), Pointer(buffer));
    LBasicBlock namesAreStored = hasElementStores ? m_out.newBlock() : continuation;
    m_out.branch(isDone.value(), usually(namesAreStored), rarely(oneByOne));
    if (hasElementStores) {
        m_out.appendTo(namesAreStored);
        for (size_t index : run.elementStores) {
            SetForScope currentIndex(m_currentIndexInBlock, index);
            auto& store = m_function.at(block[index]);
            emitPutByVal(base, in<Boxed>(store.child(1)), in<Boxed>(store.child(2)), false);
        }
        m_out.jump(continuation);
    }

    m_out.appendTo(oneByOne);
    buffer = bufferAddress();
    Vector<std::pair<ValueID, Boxed>, 4> readAgain;
    auto rereadValue = [&](ValueID id) {
        ValueID source = withoutUpcasts(id);
        for (auto& [variableRead, value] : readAgain) {
            if (variableRead == source)
                return value;
        }
        return Boxed();
    };
    auto currentValueOf = [&](ValueID id, Boxed early, unsigned inBufferAt) {
        if (Boxed value = rereadValue(id))
            return value;
        return early.value()->hasInt64() ? early : Boxed(m_out.load64(inBuffer(inBufferAt)));
    };
    unsigned next = 0;
    Vector<Boxed, 8> pieceValues;
    for (size_t index = stores.first().index; index <= stores.last().index; ++index) {
        SetForScope currentIndex(m_currentIndexInBlock, index);
        auto& value = m_function.at(block[index]);
        if (value.opcode == Opcode::GetModuleVariable) {
            readAgain.append({ block[index], convertTo<Boxed>(lowerGetModuleVariable(block[index], value), typeOf(block[index])) });
            continue;
        }
        if (run.mayBePrototype && functionOfPrototypeRead(block[index])) {
            readAgain.append({ block[index], emitGetById(in<Boxed>(value.child(0)), value.string()) });
            continue;
        }
        if (run.elementStores.contains(index)) {
            Boxed receiver = rereadValue(value.child(0));
            Boxed stored = rereadValue(value.child(2));
            emitPutByVal(receiver ? receiver : base, in<Boxed>(value.child(1)), stored ? stored : in<Boxed>(value.child(2)), false);
            continue;
        }
        if (index != stores[next].index)
            continue;
        auto& store = stores[next];
        Boxed stored = currentValueOf(store.stored, values[next], next);
        Boxed receiver = rereadValue(store.object);
        if (!receiver)
            receiver = base;
        size_t piece = run.pieces.findIf([&](auto& candidate) {
            return next >= candidate.first && next < candidate.first + candidate.count;
        });
        if (piece != notFound) {
            pieceValues.append(stored);
            if (next + 1 == run.pieces[piece].first + run.pieces[piece].count) {
                PropertyRun part;
                part.stores.append(stores.subspan(run.pieces[piece].first, run.pieces[piece].count));
                part.keysAreFixed = part.stores.containsIf([](auto& store) { return !!store.key; });
                part.slot = run.pieces[piece].slot;
                lowerPropertyRun(part, &pieceValues);
                pieceValues.clear();
            }
            ++next;
            continue;
        }
        if (store.key && store.isDefinition)
            callICStub(Stub::PrivateFieldAdd, Void, { { receiver.value(), firstStubOperandGPR }, { currentValueOf(store.key, keys[next], count + next).value(), GPRInfo::argumentGPR1 }, { stored.value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
        else if (store.key)
            emitPutByVal(receiver, currentValueOf(store.key, keys[next], count + next), stored, true);
        else if (!store.isDefinition)
            emitPutById(receiver, store.name, stored);
        else if (!emitDefineByName(receiver, store.name, stored))
            AOT_TYPED_OPERATION(DefineByName, receiver, word32(store.name.index()), stored);
        ++next;
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::lowerPutById(ValueID, const Value& value)
{
    if (value.storesLazyClosure) {
        const Field& field = *fieldFor(typeOf(value.child(0)), value.string());
        if (isLoweringInitializingStore()) {
            writeField(in<CellPointer>(value.child(0)), value.child(0), field, value.child(1));
            return;
        }
        didEmitWrite(Writes::Fields);
        m_out.store32(m_out.constInt32(unmaterializedClosureCell32), fieldAt(nonNullish(value.child(0), value.string()), field));
        return;
    }
    if (auto function = lazyClosureStoredBy(value)) {
        putLazyClosure(in<Boxed>(value.child(0)), value.string(), *function);
        return;
    }
    if (mayBeStoredBeforeJump(value)) {
        LBasicBlock isNotStored = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Boxed stored = in<Boxed>(value.child(1));
        m_out.branch(m_out.isZero64(stored.value()), unsure(continuation), unsure(isNotStored));
        m_out.appendTo(isNotStored);
        emitPutById(in<Boxed>(value.child(0)), value.string(), stored);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    if (m_cloneGroup && m_cloneGroup->stores.contains(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == m_cloneGroup->last())
            lowerCloneGroup(*std::exchange(m_cloneGroup, std::nullopt));
        return;
    }
    if (m_cloneDefinitionsBlock == m_currentBlock && m_cloneDefinitions.contains(m_currentIndexInBlock))
        return;
    if (m_spreadGroup && m_spreadGroup->startsWithClone && m_spreadGroup->definitions.contains(m_currentIndexInBlock))
        return;
    if (tryStoreToTypedClone(value))
        return;
    if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == literal->fields.last() && !tryDeferObjectLiteral(*literal))
            lowerObjectLiteral(*literal);
        return;
    }
    if (auto stores = initialStoresAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == stores->first)
            lowerInitialStores(*stores);
        return;
    }
    if (auto* stores = namedStoresAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == stores->last())
            lowerNamedStores(*stores);
        return;
    }
    if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == run->stores.last().index)
            lowerPropertyRun(*run);
        return;
    }
    if (auto* stores = storesAfterSingleTestAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == stores->last())
            lowerStoresAfterSingleTest(*stores);
        return;
    }
    ValueID object = value.child(0);
    ValueID stored = value.child(1);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    const Field* field = fieldFor(receiver, name);
    SetForScope namedStoreValueIsNotCell(m_namedStoreValueIsNotCell, !holdsCells(get(stored).rep));
    if (!field) {
        auto genericPath = [&]() -> Operand {
            emitPutById(in<Boxed>(object), name, in<Boxed>(stored));
            return { };
        };
        if (value.isFirstStoreToThis() && value.initialStructureOfFirstStore() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions()) {
            if (!value.knownSlot())
                assignThisStructure(value, in<Boxed>(object));
            if (!value.isDefinitionInThisStructure())
                emitStandaloneCachedPutById(in<Boxed>(object), name, in<Boxed>(stored), false, AOT::Site::hasThisStructure);
            else if (!emitDefineByName(in<Boxed>(object), name, in<Boxed>(stored)))
                AOT_TYPED_OPERATION(DefineByName, in<Boxed>(object), word32(name.index()), in<Boxed>(stored));
            return;
        }
        if (value.isDefinitionInThisStructure()) {
            if (!emitDefineByName(in<Boxed>(object), name, in<Boxed>(stored)))
                AOT_TYPED_OPERATION(DefineByName, in<Boxed>(object), word32(name.index()), in<Boxed>(stored));
            return;
        }
        if (value.isAtKnownSlot() && !(value.isStoreIntoInitialStructure() && value.initialStructureOfFirstStore() && !m_program.initialStructures[value.initialStructureOfFirstStore() - 1].isWhole)) {
            putAtKnownSlot(value, in<Boxed>(object), in<Boxed>(stored));
            return;
        }
        auto expected = predictionOf(object);
        const Field* expectedField = expected ? predictedFieldFor(*expected, name) : nullptr;
        if (!expectedField || hasSubtypeKeepingField(expected->singleLayout(), name) || m_program.at(expectedField->slot).isDiscriminant || (!m_isInLoop && !m_program.isStaticallyHot(m_id))) {
            genericPath();
            return;
        }
        Type held = m_program.at(expectedField->stored).withoutPrimitives(Primitive::Absent);
        if (expectedField->type && m_program.at(expectedField->type).withoutPrimitives(Primitive::Absent).isSubsetOf(held, m_program))
            held = m_program.at(expectedField->type).withoutPrimitives(Primitive::Absent);
        ifPredictionHolds({ { object, &*expected }, { stored, &held, false, true } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            LayoutID supertype = expected->singleLayout();
            bool createsInline = m_program.at(expectedField->stored).primitives & Primitive::Absent && !inheritsThroughLayoutPrototype(m_program, supertype, name);
            LBasicBlock isStored = createsInline && expectedField->store == Store::Int32OrNumberBox ? m_out.newBlock() : nullptr;
            CellPointer cell = withFieldOrElse(object, supertype, *expectedField, otherwise, createsInline);
            bool mayHaveIntegrity = mayBeFrozen(supertype) || m_program.at(supertype).subtypes.containsIf([&](LayoutID subtype) {
                return m_program.at(subtype).isImmutableAfterAllocation;
            });
            if (mayHaveIntegrity)
                orElse(hasNoBits(orderWordOf(cell), orderWordIsFrozen), otherwise);
            didEmitWrite(Writes::Fields);
            guardPinnedFields(supertype, *expectedField, stored);
            if (createsInline) {
                LBasicBlock creates = m_out.newBlock();
                LBasicBlock continuation = m_out.newBlock();
                m_out.branch(isFieldAbsent(cell, *expectedField, 0).value(), unsure(creates), unsure(continuation));
                m_out.appendTo(creates);
                Operand inlineNumber = isStored ? narrowOrElse(get(stored), typeOf(stored), inlineNumberType(), otherwise) : Operand { };
                auto onCacheMiss = [&] {
                    if (uint32_t bits = (mayInherit(m_program, supertype, name) ? orderWordHasExtraProperties : 0) | (mayHaveIntegrity ? orderWordIsNotExtensible : 0))
                        orElse(hasNoBits(orderWordOf(cell), bits), otherwise);
                };
                OperationEffects effects { Writes::Fields, false, false, true };
                if (Options::countAOTTypedAllocations()) {
                    onCacheMiss();
                    AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(name.index()));
                } else
                    didCreateFieldInCell(cell, *expectedField, effects, onCacheMiss);
                if (isStored)
                    store(expectedField->store, low32(inlineNumber.as<Integer>()), fieldAt(cell, *expectedField));
                m_out.jump(isStored ? isStored : continuation);
                m_out.appendTo(continuation);
            }
            if (expectedField->store == Store::Int32OrNumberBox)
                storeInt32OrNumberBox(cell, *expectedField, stored, false, otherwise);
            else
                store(expectedField->store, in(stored, repOf(expectedField->store)), fieldAt(cell, *expectedField));
            if (isStored) {
                m_out.jump(isStored);
                m_out.appendTo(isStored);
            }
            if (holdsCells(expectedField->store) && held.hasCells())
                barrier(cell);
            return { };
        }, genericPath);
        return;
    }
    if (isLoweringInitializingStore()) {
        writeField(in<CellPointer>(object), object, *field, stored);
        return;
    }
    LayoutID supertype = receiver.singleLayout();
    CellPointer cell = nonNullish(object, name);
    if (value.isOnSlowPath && m_program.at(supertype).acceptsUntypedObjects) {
        emitPutById(box(cell), name, in<Boxed>(stored));
        return;
    }
    accessThroughSupertype(cell, object, supertype, *field, Rep::Void, [&]() -> Operand {
        if (mayBeFrozen(supertype) && !isPrivateName(m_program.string(name))) {
            LBasicBlock asks = newColdBlock();
            LBasicBlock stores = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(hasNoBits(orderWordOf(cell), orderWordIsFrozen).value(), usually(stores), rarely(asks));
            m_out.appendTo(asks);
            if (m_program.at(field->stored).primitives & Primitive::Absent) {
                SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
                OperationEffects effects { Writes::Fields, true };
                AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(name.index()), in<Boxed>(stored));
                m_out.jump(continuation);
            } else {
                AOT_TYPED_OPERATION(ThrowIfReadOnly, cell, word32(name.index()));
                m_out.jump(stores);
            }
            m_out.appendTo(stores);
            writeField(cell, object, *field, stored);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return { };
        }
        SetForScope isAfterStoreShapeTest(m_isAfterStoreShapeTest, true);
        writeField(cell, object, *field, stored);
        return { };
    }, [&]() -> Operand {
        bool prototypeLacksProperty = prototypeChainLacksProperty(m_program, supertype, name);
        if (!prototypeLacksProperty && !prototypeChainLacksProperty(m_program, supertype, name, true)) {
            emitPutById(box(cell), name, in<Boxed>(stored));
            return { };
        }
        if (!prototypeLacksProperty) {
            LBasicBlock asks = newColdBlock();
            LBasicBlock hasNoExtraProperties = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(hasNoExtraProperties), rarely(asks));
            m_out.appendTo(asks);
            emitPutById(box(cell), name, in<Boxed>(stored));
            m_out.jump(continuation);
            m_out.appendTo(hasNoExtraProperties);
            bool mayTransition = storeMayTransition(m_program, supertype, name);
            OperationEffects effects { Writes::Fields, true, mayTransition, !mayTransition };
            AOT_TYPED_OPERATION_WITH_EFFECTS(PutExtra, effects, cell, word32(name.index()), in<Boxed>(stored));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return { };
        }
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
        bool mayTransition = storeMayTransition(m_program, supertype, name);
        OperationEffects effects { Writes::Fields, true, mayTransition, !mayTransition };
        AOT_TYPED_OPERATION_WITH_EFFECTS(PutExtra, effects, cell, word32(name.index()), in<Boxed>(stored));
        return { };
    });
}

Word32 Lowering::emitInByVal(Boxed object, Boxed key)
{
    return Word32(callICStub(Stub::InByValCached, Int32, { { object.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

Word32 Lowering::lowerInById(const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        if (LayoutID supertype = receiver.singleLayout(); supertype && get(object).rep == Rep::Cell && !isPrivateName(m_program.string(name)) && isOnlyInExtraProperties(m_program, supertype, name)) {
            CellPointer cell = get(object).as<CellPointer>();
            return either<Word32>(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
                return boolean(false);
            }, [&] {
                return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index()));
            });
        }
        auto genericPath = [&] {
            if (isPrivateName(m_program.string(name)))
                return AOT_TYPED_OPERATION(HasName, in<Boxed>(object), word32(name.index()));
            return emitInByVal(in<Boxed>(object), keyConstant(name));
        };
        auto expected = predictionOf(object);
        const Field* expectedField = expected ? predictedFieldFor(*expected, name) : nullptr;
        if (!expectedField)
            return genericPath();
        return ifPredictionHolds({ { object, &*expected } }, Rep::Int32, [&](LBasicBlock otherwise) {
            withFieldOrElse(object, expected->singleLayout(), *expectedField, otherwise);
            return boolean(true);
        }, genericPath).as<Word32>();
    }
    LayoutID supertype = receiver.singleLayout();
    Operand operand = get(object);
    if (operand.rep != Rep::Cell) {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isNil = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(boxed, receiver).value(), usually(continuation), rarely(isNil));
        m_out.appendTo(isNil);
        OperationEffects onlyThrows { Writes::Nothing, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(HasName, onlyThrows, rematerializedValue(boxed), word32(name.index()));
        m_out.unreachable();
        m_out.appendTo(continuation);
        operand = asCell(boxed);
    }
    CellPointer cell = operand.as<CellPointer>();
    return accessThroughSupertype(cell, object, supertype, *field, Rep::Int32, [&]() -> Operand {
        if (!(m_program.at(field->stored).primitives & Primitive::Absent) || !mayInherit(m_program, supertype, name))
            return hasOwnField(cell, *field);
        auto ask = [&] {
            return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
        };
        if (inheritsThroughLayoutPrototype(m_program, supertype, name)) {
            return either<Word32>(hasField(cell, *field), [&] {
                return boolean(true);
            }, ask);
        }
        LBasicBlock isPresent = m_out.newBlock();
        LBasicBlock isAbsent = m_out.newBlock();
        LBasicBlock hasNoExtraProperties = m_out.newBlock();
        LBasicBlock asks = newColdBlock();
        OperandJoin join(*this, Rep::Int32);
        m_out.branch(hasOwnField(cell, *field).value(), unsure(isPresent), unsure(isAbsent));
        m_out.appendTo(isPresent);
        join.add(boolean(true));
        m_out.appendTo(isAbsent);
        branchIfOwnFieldRequested(cell, hasNoExtraProperties, asks);
        m_out.appendTo(hasNoExtraProperties);
        join.add(boolean(false));
        m_out.appendTo(asks);
        join.add(ask());
        return join.finish();
    }, [&]() -> Operand {
        bool prototypeLacksProperty = prototypeChainLacksProperty(m_program, supertype, name);
        if (!prototypeLacksProperty && !prototypeChainLacksProperty(m_program, supertype, name, true))
            return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
        if (field->hasOtherOffsetInSomeSubtype) {
            return dispatchOnSubtypeField(cell, supertype, *field, Rep::Int32, [&](LayoutID, const Field& own) -> Operand {
                return hasOwnField(cell, own);
            }, [&]() -> Operand {
                return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index() | nameMayBeOwnField));
            });
        }
        return either<Word32>(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
            return boolean(false);
        }, [&] {
            if (!prototypeLacksProperty)
                return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
            return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index()));
        });
    }).as<Word32>();
}

Integer Lowering::allocationFor(const Value& value)
{
    auto& allocation = m_program.allocations[value.allocation()];
    Integer itself = integer(value.allocation());
    ValueID child = value.child(0);
    if (!child || !allocation.numberOfLiterals)
        return itself;
    auto& type = typeOf(child);
    Type anyInteger = Type::integer(Range::full());
    auto allocationForLiteral = [&](Integer discriminant) {
        Integer index = sub(discriminant, integer(allocation.firstLiteral));
        Integer count = integer(allocation.numberOfLiterals);
        if (!type.isInteger() || !type.integers.isSubsetOf({ allocation.firstLiteral, static_cast<int64_t>(allocation.firstLiteral) + allocation.numberOfLiterals - 1 }))
            index = select(below(index, count), index, count);
        return add(index, integer(allocation.firstLiteralAllocation));
    };
    if (type.isSubsetOf(anyInteger, m_program))
        return allocationForLiteral(in<Integer>(child));
    LBasicBlock isNotInteger = m_out.newBlock();
    Join join(m_out, Rep::Int64, b3TypeOf(Rep::Int64));
    join.add(allocationForLiteral(narrowOrElse(get(child), type, anyInteger, isNotInteger).as<Integer>()));
    m_out.appendTo(isNotInteger);
    join.add(itself);
    return join.finish().as<Integer>();
}

std::optional<uint32_t> Lowering::constantAllocation(const Value& value) const
{
    auto& site = m_program.allocations[value.allocation()];
    ValueID child = value.child(0);
    if (!child || !site.numberOfLiterals)
        return value.allocation();
    ValueID origin = child;
    if (ValueID constant = m_program.constantOf(m_function, child))
        origin = constant;
    auto& type = typeOf(child);
    std::optional<int64_t> constant;
    if (m_function.at(origin).opcode == Opcode::Integer)
        constant = m_function.at(origin).integer();
    else if (type.isInteger() && type.integers.minimum == type.integers.maximum)
        constant = type.integers.minimum;
    if (!constant)
        return std::nullopt;
    uint64_t index = static_cast<uint64_t>(*constant) - static_cast<uint64_t>(static_cast<int64_t>(site.firstLiteral));
    return site.firstLiteralAllocation + static_cast<uint32_t>(std::min<uint64_t>(index, site.numberOfLiterals));
}

bool Lowering::layoutVariesByDiscriminant(const Allocation& site) const
{
    if (!site.numberOfLiterals)
        return false;
    for (uint32_t i = 0; i <= site.numberOfLiterals; ++i) {
        if (m_program.allocations[site.firstLiteralAllocation + i].layout != site.layout)
            return true;
    }
    return false;
}

CellPointer Lowering::lowerMaterializeEnvironment(const Value& value)
{
    CellPointer current = in<CellPointer>(value.child(0));
    auto& allocationSite = m_function.at(value.child(1));
    auto& layout = m_program.at(allocationSite.layout());
    RELEASE_ASSERT(layout.mayBeStackAllocated && !layout.isStackAllocated && !layout.hasExtension);
    for (auto& field : layout.fields)
        RELEASE_ASSERT(!(field.flags & fieldIsNeverStored) && !field.isOutOfLine && field.store != Store::Int32OrNumberBox && field.store != Store::Cell32 && field.store != Store::OptionalCell32);
    RELEASE_ASSERT(layout.size > sizeof(JSCell) && !(layout.size % sizeof(uint64_t)));
    if (Options::countAOTTypedAllocations()) [[unlikely]] {
        LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, current.value(), JSCell::cellStateOffset()));
        return either<CellPointer>(Word32(m_out.notEqual(state, m_out.constInt32(UINT8_MAX))), [&] {
            return current;
        }, [&] {
            return AOT_TYPED_OPERATION(MaterializeEnvironment, word32(allocationSite.allocation()), Pointer(current.value()));
        });
    }
    m_context.emitsCalls = true;
    return CellPointer(callAllocationStub(Stub::MaterializeEnvironment, { { current.value(), GPRInfo::returnValueGPR }, { integer(static_cast<int64_t>(allocationSite.allocation() | static_cast<uint64_t>(layout.size) << 32)).value(), stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedMaterializeEnvironment>(), false));
}

CellPointer Lowering::stackAllocatedCell(size_t size)
{
    m_context.emitsCalls = true;
    LValue slot = m_out.lockedStackSlot(size);
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(slot, ValueRep::SomeRegister);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(params[1].gpr(), params[0].gpr());
    });
    applyEffects(patchpoint, { Writes::RuntimeState, false });
    return CellPointer(patchpoint);
}

Operand Lowering::initialFieldValue(const Field& field, bool wasAllocated)
{
    uint16_t primitives = m_program.at(field.stored).primitives;
    bool startsAbsent = primitives & Primitive::Absent && !wasAllocated;
    bool startsUndefined = !startsAbsent && primitives & Primitive::Undefined;
    switch (repOf(field.store)) {
    case Rep::Double:
        return number(0);
    case Rep::Int32:
        return word32(field.store != Store::Int32OrNumberBox ? 0 : startsAbsent ? absentInt32 : startsUndefined ? undefinedInt32 : 0);
    case Rep::Int64:
        return integer(0);
    case Rep::OptionalInt64:
        return OptionalInteger(integer(startsAbsent ? absentInteger : startsUndefined ? undefinedInteger : 0).value());
    case Rep::OptionalDouble:
        return OptionalNumber(m_out.constDouble(std::bit_cast<double>(startsAbsent ? absentDouble : startsUndefined ? undefinedDouble : 0)));
    case Rep::Cell:
        return CellPointer(m_out.int64Zero);
    case Rep::JSValue:
        return startsUndefined ? undefined() : Boxed(m_out.int64Zero);
    case Rep::LiteralString:
        return LiteralStringWord(m_out.constInt64(startsUndefined ? undefinedCell32 : absentCell32));
    case Rep::Void:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand Lowering::lowerNewObject(ValueID id, const Value& value)
{
    auto constant = constantAllocation(value);
    bool fromTemplate = !constant && layoutVariesByDiscriminant(m_program.allocations[value.allocation()]);
    auto& allocation = m_program.allocations[constant.value_or(value.allocation())];
    auto& layout = m_program.at(allocation.layout);
    BitVector initializedFields;
    if (isObjectLayout(layout.kind)) {
        for (uint16_t field : allocation.order.span().first(allocation.initialFieldCount))
            initializedFields.set(field);
    }
    BitVector storedNext;
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    bool mayHaveCollected = false;
    for (; next < values.size() && isInitializingStore(m_program, m_function, *m_currentBlock, next); ++next) {
        auto& following = m_function.at(values[next]);
        uint32_t field = *fieldInitializedBy(allocation.layout, following);
        if (!mayHaveCollected && (initializedFields.get(field) || !isObjectLayout(layout.kind)))
            storedNext.set(field);
        mayHaveCollected |= !isStoredInline(layout.fields[field], following.child(1)) || (isObjectLayout(layout.kind) && !initializedFields.get(field));
    }
    bool isStackAllocated = m_function.stackAllocatedEnvironments.get(id.index());
    RELEASE_ASSERT(!isStackAllocated || layout.mayBeStackAllocated);
    auto allocateCell = [&] {
        return constant ? allocate(*constant) : allocate(value.allocation(), allocationFor(value), fromTemplate ? FieldInit::FromTemplate : FieldInit::ByCaller);
    };
    auto reuseOrAllocateCell = [&] {
        auto& variable = m_program.variables[layout.reuseVariable.index()];
        TypedPointer place = addressOf(layout.reuseVariable);
        TypedPointer address(m_heaps.AOTTyped_runtimeState, place.base(), place.offset());
        Boxed held = convertTo<Boxed>(load(variable.store, address, IsSafe), m_program.at(variable.type));
        LBasicBlock reused = m_out.newBlock();
        LBasicBlock fresh = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(variable.storesUndefinedAsZero ? m_out.notZero64(held.value()) : isCell(held).value(), unsure(reused), unsure(fresh));
        m_out.appendTo(reused);
        didEmitWrite(Writes::RuntimeState);
        store(variable.store, variable.storesUndefinedAsZero ? Operand(Boxed(m_out.int64Zero)) : convert(undefined(), Type::fromPrimitives(Primitive::Undefined), repOf(variable.store)), address);
        CellPointer taken = asCell(held);
        ValueFromBlock first = m_out.anchor(taken.value());
        m_out.jump(continuation);
        m_out.appendTo(fresh);
        ValueFromBlock second = m_out.anchor(allocateCell().value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, first, second));
    };
    bool mayBeReused = !isStackAllocated && layout.reuseVariable && !Options::validateAOTTypedFacts();
    CellPointer cell = isStackAllocated ? stackAllocatedCell(layout.size) : mayBeReused ? reuseOrAllocateCell() : allocateCell();
    m_isFillingReusedEnvironment = mayBeReused;
    if (isStackAllocated && !layout.isStackAllocated)
        m_out.store32As8(m_out.constInt32(UINT8_MAX), m_out.address(m_heaps.AOTTyped_collectorState, cell.value(), JSCell::cellStateOffset()));
    if (!fromTemplate) {
        m_fieldsAwaitingFirstStore = storedNext;
        m_fieldsAwaitingFirstStoreLayout = allocation.layout;
    }
    if (layout.hasExtension)
        storeField(cell, m_heaps.AOTTypedObject_extension, word32(0));
    BitVector isFilledWithZeroes;
    if (!fromTemplate && isObjectLayout(layout.kind) && !isStackAllocated && !mayBeReused) {
        constexpr uint32_t chunkBytes = 2 * sizeof(uint64_t);
        Vector<std::pair<uint32_t, uint32_t>, 16> ranges;
        uint32_t end = 0;
        for (uint32_t i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            if (field.isOutOfLine || field.flags & fieldIsNeverStored)
                continue;
            end = std::max(end, field.offset + sizeOf(field.store));
            if (storedNext.get(i))
                continue;
            LValue initial = initialFieldValue(field, initializedFields.get(i)).value;
            bool startsAsZero = (initial->hasInt() && !initial->asInt()) || (initial->hasDouble() && !std::bit_cast<uint64_t>(initial->asDouble()));
            if (!startsAsZero)
                continue;
            ranges.append({ field.offset, field.offset + sizeOf(field.store) });
            isFilledWithZeroes.set(i);
        }
        end = WTF::roundUpToMultipleOf<chunkBytes>(end);
        std::ranges::sort(ranges);
        Vector<int32_t, 8> chunks;
        uint32_t filledUpTo = 0;
        for (auto [first, last] : ranges) {
            while (filledUpTo < last) {
                uint32_t start = std::min(std::max(first, filledUpTo) & ~static_cast<uint32_t>(sizeof(uint64_t) - 1), end - chunkBytes);
                chunks.append(start);
                filledUpTo = start + chunkBytes;
            }
        }
        bool takesNoMoreInstructions = isX86_64() ? chunks.size() + 1 <= ranges.size() : chunks.size() < ranges.size();
        if (ranges.size() < 2 || !takesNoMoreInstructions || end < Typed::offsetOfFirstField + chunkBytes)
            isFilledWithZeroes.clearAll();
        else {
            PatchpointValue* zeroes = m_out.patchpoint(Void);
            zeroes->effects = Effects::none();
            m_heaps.decoratePatchpointWrite(&m_heaps.AOTTyped_slots.atAnyNumber(), zeroes);
            zeroes->append(ConstrainedValue(cell.value(), ValueRep::SomeRegister));
            zeroes->numFPScratchRegisters = isX86_64();
            zeroes->setGenerator([chunks = WTF::move(chunks)](CCallHelpers& jit, const StackmapGenerationParams& params) {
                GPRReg object = params[0].gpr();
#if CPU(X86_64)
                FPRReg zero = params.fpScratch(0);
                jit.moveZeroToDouble(zero);
                for (int32_t offset : chunks) {
                    if (CCallHelpers::supportsAVX())
                        jit.m_assembler.vmovups_rm(zero, offset, object);
                    else
                        jit.m_assembler.movups_rm(zero, offset, object);
                }
#else
                for (int32_t offset : chunks)
                    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, object, CCallHelpers::TrustedImm32(offset));
#endif
            });
        }
    }
    for (uint32_t i = 0; i < layout.fields.size() && !fromTemplate; ++i) {
        auto& field = layout.fields[i];
        if (storedNext.get(i) || field.isOutOfLine || field.flags & fieldIsNeverStored || isFilledWithZeroes.get(i))
            continue;
        if (layout.isCoroutineFrame && !holdsCells(field.store))
            continue;
        if (layout.isCoroutineFrame && i == CoroutineFrame::resumeClosureField && next < values.size() && isResumeClosureCreatedByStub(next) && m_function.at(values[next]).arguments[0] == id)
            continue;
        store(field.store, initialFieldValue(field, initializedFields.get(i)), fieldAt(cell, field));
    }
    if (next == m_currentIndexInBlock + 1 && layout.reuseVariable) {
        m_isFillingReusedEnvironment = false;
        barrier(cell);
    }
    if (next == m_currentIndexInBlock + 1 && !isStackAllocated)
        fenceAfterInitialization();
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
