# Copyright (C) 2026 Oven, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

# Handlers of bytecode in a process that runs ahead-of-time compiled code, where no JIT takes over from the interpreter.
# LLInt::initialize() puts them into the opcode maps when Options::useAOT() is set, in place of the handlers of the same opcodes, so no
# other process runs them.
#
# The Structure of a compiled object prohibits caching by offset. What its properties are is in the hashed table of
# AOT::VMPropertyCaches, under (StructureID, name, access). The slow paths fill the table.

const CompiledObjectType = constexpr CompiledObjectType

const HashedSlotKey = constexpr (AOT::HashedSlot::offsetOfKey())
const SlotStructureID = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfStructureID())
const SlotKind = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfKind())
const SlotFlags = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfFlags())
const SlotLocation = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfLocation())
const SlotPayload = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfPayload())
const SlotSecondPayloadWord = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfSecondPayloadWord())
const HashedSlotSize = constexpr (sizeof(AOT::HashedSlot))

const InstanceHashedSlots = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots))
const InstanceHashedSlotOffsetMask = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hashedSlotOffsetMask))
const InstanceCellCageBase = constexpr (AOT_INSTANCE_OFFSET(cellCageBase))
const InstanceTables = constexpr (AOT_INSTANCE_OFFSET(image.tables))
const InstanceGlobalObject = constexpr (AOT_INSTANCE_OFFSET(globalObject))
const InstanceAuxiliarySpace = constexpr (AOT_INSTANCE_OFFSET(allocation.auxiliarySpace))
const InstanceArrayAllocator = constexpr (AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator))
const InstanceNewArrayWithContiguousHeader = constexpr (AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader))

const RuntimeTypePrimitives = constexpr (AOT::RuntimeType::offsetOfPrimitives())
const RuntimeTypeFlags = constexpr (AOT::RuntimeType::offsetOfFlags())
const RuntimeTypeNumberOfCellTests = constexpr (AOT::RuntimeType::offsetOfNumberOfCellTests())
const RuntimeTypeNumberOfLiteralStrings = constexpr (AOT::RuntimeType::offsetOfNumberOfLiteralStrings())
const RuntimeTypeKindOfFirstCellTest = constexpr (AOT::RuntimeType::offsetOfCellTests() + AOT::RuntimeCellTest::offsetOfKind())

const LocalAllocatorIntervalStart = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart())
const LocalAllocatorIntervalEnd = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd())
const LocalAllocatorNextInterval = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval())
const LocalAllocatorSecret = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret())
const LocalAllocatorCellSize = constexpr (LocalAllocator::offsetOfCellSize())
const FreeCellScrambledBits = constexpr (FreeCell::offsetOfScrambledBits())
const StructureTypeInfoBlob = constexpr (Structure::indexingModeIncludingHistoryOffset())

macro aotOp(opcodeName, opcodeStruct, fn)
    commonOp(llint_aot_%opcodeName%, traceExecution, macro (size)
        macro getImpl(fieldName, dst)
            get(size, opcodeStruct, fieldName, dst)
        end

        macro dispatchImpl()
            dispatchOp(size, opcodeName)
        end

        macro metadataImpl(dst, scratch)
            metadata(size, opcodeStruct, dst, scratch)
        end

        makeReturn(getImpl, dispatchImpl, macro (return)
            fn(size, getImpl, dispatchImpl, metadataImpl, return)
        end)
    end)
end

macro loadInstanceOfRealm(instance, hasNone)
    loadp CodeBlock[cfr], instance
    loadp CodeBlock::m_globalObject[instance], instance
    loadp JSGlobalObject::m_aotInstance[instance], instance
    btpz instance, hasNone
end

macro loadIdentifier(size, opcodeStruct, fieldName, uid, scratch)
    getu(size, opcodeStruct, fieldName, scratch)
    loadp CodeBlock[cfr], uid
    loadp CodeBlock::m_unlinkedCode[uid], uid
    loadp UnlinkedCodeBlock::m_identifiers + IdentifierFixedVector::m_storage[uid], uid
    loadp (constexpr (IdentifierFixedVector::Storage::offsetOfData()))[uid, scratch, PtrSize], uid
end

# AOT::HashedSlotTable::findEntry(). The sets of the table are aligned to their size, so the ways of a set differ in the same bits of
# the address as of the offset. The key is AOT::HashedSlot::keyFor(), which for a read is the uid.
macro findHashedSlot(instance, structureID, uid, makeKey, entry, scratch, miss)
    move structureID, entry
    muli constexpr AOT::HashedSlot::structureMultiplier, entry
    urshifti constexpr AOT::HashedSlot::structureHashShift, entry
    move uid, scratch
    lshifti constexpr AOT::HashedSlot::uidHashShift, scratch
    addi scratch, entry
    andq InstanceHashedSlotOffsetMask[instance], entry
    addp InstanceHashedSlots[instance], entry
    makeKey(uid)
    bpneq uid, HashedSlotKey[entry], .secondWay
    bieq structureID, SlotStructureID[entry], .found
.secondWay:
    xorp HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], .thirdWay
    bieq structureID, SlotStructureID[entry], .found
.thirdWay:
    xorp 3 * HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], .fourthWay
    bieq structureID, SlotStructureID[entry], .found
.fourthWay:
    xorp HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], miss
    bineq structureID, SlotStructureID[entry], miss
.found:
end

macro makeKeyOfRead(uid)
end

macro makeKeyOfStore(uid)
    orp constexpr AOT::SlotAccess::Store, uid
end

macro jumpToOrdinaryHandler(size, opcodeName)
    size(macro () jmp _llint_%opcodeName% end, macro () jmp _llint_%opcodeName%_wide16 end, macro () jmp _llint_%opcodeName%_wide32 end, macro (jump) jump() end)
end

# Leaves the address of the field in address and its AOT::SlotRepresentation in representation.
macro locateFieldInCell(entry, object, address, representation, isOutOfLine)
    btbnz SlotFlags[entry], constexpr AOT::Slot::isOutOfLine, isOutOfLine
    loadi SlotPayload[entry], address
    move address, representation
    urshifti constexpr AOT::SlotData::offsetBits, representation
    andi constexpr AOT::SlotData::maxOffset, address
    addp object, address
end

# AOT::readSlot(), for the representations that take no more than the bits of the slot. A word below AOT::minCell32 is a marker, which is
# the JSValue it stands for, or names a literal string. The markers of a double are NaNs.
macro readField(instance, address, representation, result, isAbsent, cannot, done)
    bieq representation, constexpr AOT::SlotRepresentation::Cell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::JSValue, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalCell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Int32OrDouble, .int32OrDouble
    bieq representation, constexpr AOT::SlotRepresentation::OptionalDouble, .double
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Double, .double
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, .boolean
    bieq representation, constexpr AOT::SlotRepresentation::Int32, .int32
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalBoolean, .optionalBoolean
    jmp cannot

.narrowReference:
    loadi [address], result
    bib result, constexpr AOT::minCell32, .marker
    lshiftp constexpr AOT::cell32Shift, result
    addp InstanceCellCageBase[instance], result
    jmp done

.boxed:
    loadq [address], result
    bqaeq result, constexpr AOT::minCell32, done
.marker:
    btqz result, isAbsent
    bqa result, ValueUndefined, cannot
    jmp done

.int32OrDouble:
    loadi [address], result
    bilt result, constexpr AOT::minInlineInt32, .int32Marker
    orq numberTag, result
    jmp done
.int32Marker:
    bieq result, constexpr AOT::absentInt32, isAbsent
    bieq result, constexpr AOT::undefinedInt32, .undefined
    bieq result, constexpr AOT::nullInt32, .null
    jmp cannot

.int32:
    loadi [address], result
    orq numberTag, result
    jmp done

.double:
    loadd [address], ft0
    bdnequn ft0, ft0, .isNotANumber
    bcd2i ft0, result, .isNotInt32
    orq numberTag, result
    jmp done
.isNotInt32:
    fd2q ft0, result
    subq numberTag, result
    jmp done
.isNotANumber:
    bineq representation, constexpr AOT::SlotRepresentation::OptionalDouble, cannot
    loadq [address], result
    move constexpr AOT::absentDouble, representation
    bqeq result, representation, isAbsent
    move constexpr AOT::undefinedDouble, representation
    bqeq result, representation, .undefined
    move constexpr AOT::nullDouble, representation
    bqneq result, representation, cannot
.null:
    move ValueNull, result
    jmp done
.undefined:
    move ValueUndefined, result
    jmp done

.boolean:
    loadb [address], result
    orq ValueFalse, result
    jmp done

.optionalBoolean:
    loadb [address], result
    btqz result, isAbsent
    jmp done
end

# AOT::slotHoldsAbsent().
macro branchOnFieldPresence(address, representation, scratch, isPresent, isAbsent, cannot)
    bieq representation, constexpr AOT::SlotRepresentation::Cell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::JSValue, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalCell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Int32OrDouble, .int32OrDouble
    bieq representation, constexpr AOT::SlotRepresentation::OptionalDouble, .optionalDouble
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Double, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Int32, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString, .boxed
    bineq representation, constexpr AOT::SlotRepresentation::OptionalBoolean, cannot
    btbz [address], isAbsent
    jmp isPresent

.narrowReference:
    btiz [address], isAbsent
    jmp isPresent

.boxed:
    btqz [address], isAbsent
    jmp isPresent

.int32OrDouble:
    bieq [address], constexpr AOT::absentInt32, isAbsent
    jmp isPresent

.optionalDouble:
    move constexpr AOT::absentDouble, scratch
    bqeq [address], scratch, isAbsent
    jmp isPresent
end

# The word index of a property counts from the start of the cell, or down from the butterfly.
macro loadPropertyAtWordIndex(holder, wordIndex, result)
    bigteq wordIndex, 0, .isInline
    loadp JSObjectWithButterfly::m_butterfly[holder], holder
.isInline:
    loadq [holder, wordIndex, 8], result
end

macro loadExtraProperties(instance, object, structureID, result, scratch, cannot)
    structureIDToStructureWithScratch(structureID, scratch)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesSlot()))[structureID], result
    move result, scratch
    urshifti constexpr AOT::SlotData::offsetBits, scratch
    bineq scratch, constexpr AOT::SlotRepresentation::Cell32, cannot
    andi constexpr AOT::SlotData::maxOffset, result
    loadi [object, result], result
    btiz result, cannot
    lshiftp constexpr AOT::cell32Shift, result
    addp InstanceCellCageBase[instance], result
end

# AOT::uidOfKey().
macro loadNameOfKey(key, uid, isNoName)
    bbeq JSCell::m_type[key], StringType, .string
    bbneq JSCell::m_type[key], SymbolType, isNoName
    loadp (constexpr (Symbol::offsetOfSymbolImpl()))[key], uid
    jmp .done
.string:
    loadp JSString::m_fiber[key], uid
    btpnz uid, isRopeInPointer, isNoName
    btiz StringImpl::m_hashAndFlags[uid], constexpr StringImpl::s_hashFlagStringKindIsAtom, isNoName
.done:
end

# t3: the compiled object, t1: the uid. Leaves the value in t0.
macro getFromCompiledObject(slow, done)
    loadInstanceOfRealm(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::ExtraProperty, .extraProperty
    bbeq t0, constexpr AOT::Slot::Kind::Value, .value
    bbeq t0, constexpr AOT::Slot::Kind::Absent, .undefined
    bbneq t0, constexpr AOT::Slot::Kind::Constant, slow
    loadq SlotPayload[t6], t0
    jmp done

.field:
    locateFieldInCell(t6, t3, t1, t2, slow)
    readField(t5, t1, t2, t0, .absentField, slow, done)

.absentField:
    btbz SlotFlags[t6], constexpr AOT::Slot::absentIsUndefined, slow
.undefined:
    move ValueUndefined, t0
    jmp done

.extraProperty:
    loadExtraProperties(t5, t3, t2, t0, t1, slow)
    loadis SlotSecondPayloadWord[t6], t1
    loadPropertyAtWordIndex(t0, t1, t0)
    jmp done

.value:
    loadhsq SlotLocation[t6], t1
    btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .hasHolder
    loadp SlotPayload[t6], t3
.hasHolder:
    loadPropertyAtWordIndex(t3, t1, t0)
    jmp done
end

# t3: the object, t1: the uid.
macro testPresenceInObject(slow, isPresent, isAbsent)
    loadInstanceOfRealm(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::Absent, isAbsent
    move 1, t1
    lshifti t0, t1
    btinz t1, constexpr AOT::kindsOfPresentProperties, isPresent
    jmp slow

.field:
    locateFieldInCell(t6, t3, t1, t2, slow)
    branchOnFieldPresence(t1, t2, t0, isPresent, .absentField, slow)

.absentField:
    btbnz SlotFlags[t6], constexpr AOT::Slot::absentIsUndefined, isAbsent
    jmp slow
end

aotOp(op_get_by_id, OpGetById, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadConstantOrVariableCell(size, t0, t3, .slow)
    bbeq JSCell::m_type[t3], CompiledObjectType, .compiledObject
    metadata(t2, t1)
    performGetByIDHelper(OpGetById, m_modeMetadata, m_valueProfile, .slow, size, return)

.compiledObject:
    loadIdentifier(size, OpGetById, m_property, t1, t0)
    getFromCompiledObject(.slow, .done)

.done:
    valueProfile(size, OpGetById, m_valueProfile, t0, t2)
    return(t0)

.slow:
    callSlowPath(_llint_slow_path_get_by_id)
    dispatch()
end)

aotOp(op_get_by_val, OpGetByVal, macro (size, get, dispatch, metadata, return)
    getByVal(size, get, dispatch, metadata, return, macro ()
        get(m_base, t0)
        loadConstantOrVariableCell(size, t0, t3, .slow)
        bbneq JSCell::m_type[t3], CompiledObjectType, .slow
        get(m_property, t0)
        loadConstantOrVariableCell(size, t0, t2, .slow)
        loadNameOfKey(t2, t1, .slow)
        getFromCompiledObject(.slow, .done)

    .done:
        valueProfile(size, OpGetByVal, m_valueProfile, t0, t2)
        return(t0)

    .slow:
        callSlowPath(_llint_slow_path_get_by_val)
        dispatch()
    end)
end)

aotOp(op_in_by_id, OpInById, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadConstantOrVariableCell(size, t0, t3, .slow)
    bbb JSCell::m_type[t3], ObjectType, .slow
    loadIdentifier(size, OpInById, m_property, t1, t0)
    testPresenceInObject(.slow, .true, .false)

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_llint_slow_path_aot_in_by_id)
    dispatch()
end)

aotOp(op_in_by_val, OpInByVal, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadConstantOrVariableCell(size, t0, t3, .slow)
    bbb JSCell::m_type[t3], ObjectType, .slow
    get(m_property, t0)
    loadConstantOrVariableCell(size, t0, t2, .slow)
    loadNameOfKey(t2, t1, .slow)
    testPresenceInObject(.slow, .true, .false)

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_llint_slow_path_aot_in_by_val)
    dispatch()
end)

# JSPropertyNameEnumerator::computeNext() for an enumerator that is current for a compiled object, whose mode is always GenericMode.
aotOp(op_enumerator_next, OpEnumeratorNext, macro (size, get, dispatch, metadata, return)
    enumeratorNext(size, get, dispatch, macro ()
        loadVariable(get, m_mode, t0)
        bbneq t0, constexpr JSPropertyNameEnumerator::GenericMode, .slow
        get(m_base, t1)
        loadConstantOrVariableCell(size, t1, t0, .slow)
        bbneq JSCell::m_type[t0], CompiledObjectType, .slow
        loadVariable(get, m_enumerator, t1)
        loadi JSPropertyNameEnumerator::m_cachedStructureID[t1], t2
        bineq t2, JSCell::m_structureID[t0], .slow
        loadi (constexpr AOT::offsetOfOrderWord)[t0], t2
        andi JSPropertyNameEnumerator::m_cachedOrderWordMask[t1], t2
        bineq t2, JSPropertyNameEnumerator::m_cachedOrderWord[t1], .slow

        loadVariable(get, m_index, t2)
        addq 1, t2
        loadi JSPropertyNameEnumerator::m_endStructurePropertyIndex[t1], t3
        biaeq t2, t3, .slow

        storeVariable(get, m_index, t2, t3)
        loadp JSPropertyNameEnumerator::m_propertyNames[t1], t3
        zxi2q t2, t2
        loadp [t3, t2, PtrSize], t3
        storeVariable(get, m_propertyName, t3, t2)
        dispatch()

    .slow:
        callSlowPath(_slow_path_enumerator_next)
        dispatch()
    end)
end)

aotOp(op_enumerator_get_by_val, OpEnumeratorGetByVal, macro (size, get, dispatch, metadata, return)
    enumeratorGetByVal(size, get, dispatch, metadata, return, macro ()
        get(m_base, t0)
        loadConstantOrVariableCell(size, t0, t3, .slow)
        bbneq JSCell::m_type[t3], CompiledObjectType, .slow
        loadVariable(get, m_propertyName, t2)
        loadNameOfKey(t2, t1, .miss)
        getFromCompiledObject(.miss, .done)

    .done:
        valueProfile(size, OpEnumeratorGetByVal, m_valueProfile, t0, t2)
        return(t0)

    .miss:
        callSlowPath(_llint_slow_path_aot_enumerator_get_by_val)
        dispatch()

    .slow:
        callSlowPath(_slow_path_enumerator_get_by_val)
        dispatch()
    end)
end)

# Stores to a field that is in its slot. AOT::RuntimeLayoutTable::valueHasType() and AOT::tryWriteSlot(), for the types and representations
# that a test of the value's tag decides.
aotOp(op_put_by_id, OpPutById, macro (size, get, dispatch, metadata, return)
    get(m_base, t1)
    loadConstantOrVariableCell(size, t1, t0, .ordinary)
    bbeq JSCell::m_type[t0], CompiledObjectType, .compiledObject
.ordinary:
    jumpToOrdinaryHandler(size, op_put_by_id)

.compiledObject:
    getu(size, OpPutById, m_flags, t1)
    btinz t1, constexpr PutByIdFlagsFits::s_isDirectBit, .slow
    loadInstanceOfRealm(t5, .slow)
    loadIdentifier(size, OpPutById, m_property, t1, t2)
    loadi JSCell::m_structureID[t0], t2
    findHashedSlot(t5, t2, t1, makeKeyOfStore, t6, t3, .slow)
    loadb SlotKind[t6], t1
    bbeq t1, constexpr AOT::Slot::Kind::Field, .field
    bbneq t1, constexpr AOT::Slot::Kind::FieldCreation, .slow
.field:
    btinz (constexpr AOT::offsetOfOrderWord)[t0], constexpr AOT::orderWordIsFrozen, .slow
    locateFieldInCell(t6, t0, t1, t2, .slow)
    loadi SlotSecondPayloadWord[t6], t6
    addp InstanceTables[t5], t6
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t3)
    loadh RuntimeTypePrimitives[t6], t5

    # t0: object, t1: address of the field, t2: representation, t3: value, t5: primitives of the type, t6: AOT::RuntimeType
    bqaeq t3, numberTag, .int32
    btqnz t3, numberTag, .double
    btqnz t3, notCellMask, .otherImmediate
    btqz t3, .slow
    loadb JSCell::m_type[t3], t7
    bbaeq t7, ObjectType, .object
    bbneq t7, StringType, .slow
    btiz t5, constexpr AOT::Typed::Primitive::String, .slow
    loadh RuntimeTypeNumberOfLiteralStrings[t6], t7
    btinz t7, .slow
    jmp .storeCell

.object:
    loadh RuntimeTypeNumberOfCellTests[t6], t7
    btiz t7, .slow
    bbneq RuntimeTypeKindOfFirstCellTest[t6], constexpr AOT::CellTestKind::IsObject, .slow
.storeCell:
    bieq t2, constexpr AOT::SlotRepresentation::Cell32, .storeNarrowReference
    bieq t2, constexpr AOT::SlotRepresentation::OptionalCell32, .storeNarrowReference
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedCell
    bineq t2, constexpr AOT::SlotRepresentation::Cell, .slow
.storeBoxedCell:
    btqz [t1], .slow
    storeq t3, [t1]
    jmp .barrier
.storeNarrowReference:
    btiz [t1], .slow
    urshiftq constexpr AOT::cell32Shift, t3
    storei t3, [t1]
.barrier:
    writeBarrierOnCellWithReload(t0, macro () end)
    dispatch()

.int32:
    btiz t5, constexpr AOT::Typed::Primitive::Integer, .slow
    btbnz RuntimeTypeFlags[t6], constexpr AOT::RuntimeType::hasIntegerRange, .slow
    bieq t2, constexpr AOT::SlotRepresentation::Int32OrDouble, .storeInt32OrDouble
    bieq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .storeInt32AsOptionalDouble
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::Double, .storeInt32AsDouble
    bineq t2, constexpr AOT::SlotRepresentation::Int32, .slow
    storei t3, [t1]
    dispatch()
.storeInt32OrDouble:
    bieq [t1], constexpr AOT::absentInt32, .slow
    bilt t3, constexpr AOT::minInlineInt32, .slow
    storei t3, [t1]
    dispatch()
.storeInt32AsOptionalDouble:
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
.storeInt32AsDouble:
    ci2ds t3, ft0
    stored ft0, [t1]
    dispatch()

.double:
    andi constexpr AOT::Typed::Primitive::Number, t5
    bineq t5, constexpr AOT::Typed::Primitive::Number, .slow
    btbnz RuntimeTypeFlags[t6], constexpr AOT::RuntimeType::hasIntegerRange, .slow
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::Double, .storeDouble
    bineq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .slow
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
.storeDouble:
    addq numberTag, t3
    storeq t3, [t1]
    dispatch()

.otherImmediate:
    bqeq t3, ValueUndefined, .undefined
    bqeq t3, ValueNull, .null
    bqeq t3, ValueTrue, .true
    bqneq t3, ValueFalse, .slow
    btiz t5, constexpr AOT::Typed::Primitive::False, .slow
    jmp .storeBoolean
.true:
    btiz t5, constexpr AOT::Typed::Primitive::True, .slow
.storeBoolean:
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::OptionalBoolean, .storeByteOfImmediate
    bineq t2, constexpr AOT::SlotRepresentation::Boolean, .slow
    andi 1, t3
    storeb t3, [t1]
    dispatch()
.undefined:
    btiz t5, constexpr AOT::Typed::Primitive::Undefined, .slow
    jmp .storeUndefinedOrNull
.null:
    btiz t5, constexpr AOT::Typed::Primitive::Null, .slow
.storeUndefinedOrNull:
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::OptionalBoolean, .storeByteOfImmediate
    bieq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .storeMarkerInOptionalDouble
    bieq t2, constexpr AOT::SlotRepresentation::Int32OrDouble, .storeMarkerInInt32OrDouble
    bineq t2, constexpr AOT::SlotRepresentation::OptionalCell32, .slow
    btiz [t1], .slow
    storei t3, [t1]
    dispatch()
.storeMarkerInOptionalDouble:
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
    move constexpr AOT::undefinedDouble, t7
    bqeq t3, ValueUndefined, .storeDoubleMarker
    move constexpr AOT::nullDouble, t7
.storeDoubleMarker:
    storeq t7, [t1]
    dispatch()
.storeMarkerInInt32OrDouble:
    bieq [t1], constexpr AOT::absentInt32, .slow
    move constexpr AOT::undefinedInt32, t7
    bqeq t3, ValueUndefined, .storeInt32Marker
    move constexpr AOT::nullInt32, t7
.storeInt32Marker:
    storei t7, [t1]
    dispatch()
.storeByteOfImmediate:
    btbz [t1], .slow
    storeb t3, [t1]
    dispatch()
.storeBoxedPrimitive:
    btqz [t1], .slow
    storeq t3, [t1]
    dispatch()

.slow:
    callSlowPath(_llint_slow_path_put_by_id)
    dispatch()
end)

# FreeList::allocateWithCellSize(). The low half of the bits of a free cell is the distance to the next interval, the high half the length
# of this one.
macro allocateCell(allocator, result, scratch, slow)
    loadp LocalAllocatorIntervalStart[allocator], result
    bpb result, LocalAllocatorIntervalEnd[allocator], .hasInterval
    loadp LocalAllocatorNextInterval[allocator], result
    btpnz result, 1, slow
    loadi LocalAllocatorSecret[allocator], scratch
    xori FreeCellScrambledBits[result], scratch
    sxi2q scratch, scratch
    addp result, scratch
    storep scratch, LocalAllocatorNextInterval[allocator]
    loadi LocalAllocatorSecret + 4[allocator], scratch
    xori FreeCellScrambledBits + 4[result], scratch
    addp result, scratch
    storep scratch, LocalAllocatorIntervalEnd[allocator]
.hasInterval:
    loadi LocalAllocatorCellSize[allocator], scratch
    addp result, scratch
    storep scratch, LocalAllocatorIntervalStart[allocator]
end

# The collector may mark while this thread runs, so it has to see the cell's contents no later than a reference to the cell.
macro fenceAfterAllocation(scratch)
    if ARM64 or ARM64E
        loadp CodeBlock[cfr], scratch
        loadp CodeBlock::m_vm[scratch], scratch
        btbz (constexpr (VM::offsetOfHeapMutatorShouldBeFenced()))[scratch], .isNotFenced
        memfence
    .isNotFenced:
    end
end

# JSFinalObject::create().
aotOp(op_new_object, OpNewObject, macro (size, get, dispatch, metadata, return)
    metadata(t5, t0)
    loadp OpNewObject::Metadata::m_objectAllocationProfile.m_allocator[t5], t1
    btpz t1, .slow
    loadp OpNewObject::Metadata::m_objectAllocationProfile.m_structure[t5], t2
    allocateCell(t1, t0, t3, .slow)
    storei t2, JSCell::m_structureID[t0]
    loadi StructureTypeInfoBlob[t2], t3
    storei t3, JSCell::m_indexingTypeAndMisc[t0]
    storep 0, JSObjectWithButterfly::m_butterfly[t0]
    loadb (constexpr (Structure::inlineCapacityOffset()))[t2], t3
    btiz t3, .isInitialized
.clearProperty:
    subi 1, t3
    storeq ValueEmpty, sizeof JSObjectWithButterfly[t0, t3, 8]
    btinz t3, .clearProperty
.isInitialized:
    fenceAfterAllocation(t3)
    return(t0)

.slow:
    callSlowPath(_llint_slow_path_aot_new_object)
    dispatch()
end)

# constructArrayNegativeIndexed(), where storing the elements leaves the indexing type of the profile as it is.
aotOp(op_new_array, OpNewArray, macro (size, get, dispatch, metadata, return)
    loadInstanceOfRealm(t5, .slow)
    # AOT::branchIfHavingABadTime().
    btiz InstanceNewArrayWithContiguousHeader[t5], .slow
    metadata(t6, t0)

    # ArrayAllocationProfile::selectIndexingType().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t3
    move t3, t2
    urshiftq constexpr AOT::shiftOfIndexingTypeInArrayAllocationProfile, t2
    lshiftq constexpr AOT::bitsOfTypeInArrayAllocationProfile, t3
    urshiftq constexpr AOT::bitsOfTypeInArrayAllocationProfile, t3
    btqz t3, .typeIsCurrent
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    andi constexpr AllWritableArrayTypes, t0
    bineq t0, t2, .slow
.typeIsCurrent:

    # Butterfly::optimalContiguousVectorLength(): the storage has a size class of its own up to MarkedSpace::preciseCutoff.
    getu(size, OpNewArray, m_argc, t1)
    btinz t1, .hasElements
    bia t2, constexpr ArrayWithContiguous, .slow
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN_EMPTY, t3
    jmp .elementsFit
.hasElements:
    bia t1, constexpr AOT::maxLengthOfArrayLiteralAllocatedInline, .slow
    move t1, t3
    ori 1, t3
    biaeq t3, constexpr BASE_CONTIGUOUS_VECTOR_LEN, .hasVectorLength
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN, t3
.hasVectorLength:
    bieq t2, constexpr ArrayWithContiguous, .elementsFit
    bineq t2, constexpr ArrayWithInt32, .slow
    get(m_argv, t0)
    leap [cfr, t0, 8], t0
    move t1, t7
.checkElement:
    bqb [t0], numberTag, .slow
    subp 8, t0
    subi 1, t7
    btinz t7, .checkElement
.elementsFit:

    # t2: indexing type, t3: vector length. The indexing header is one more word.
    addi 1, t3
    loadp InstanceAuxiliarySpace[t5], t0
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t0, t3, 4], t0
    btpz t0, .slow
    allocateCell(t0, t7, t1, .slow)
    subi 1, t3
    getu(size, OpNewArray, m_argc, t1)
    move t3, t0
    lshiftq 32, t0
    orq t1, t0
    storeq t0, [t7]

    move ValueEmpty, t0
    bineq t2, constexpr ArrayWithDouble, .hasHole
    move constexpr AOT::holeInDoubleArray, t0
.hasHole:
    bieq t3, t1, .isCleared
.clearElement:
    storeq t0, [t7, t3, 8]
    subi 1, t3
    bineq t3, t1, .clearElement
.isCleared:
    btiz t1, .isFilled
    # The elements lie in registers of descending number. This goes from the last to the first.
    get(m_argv, t0)
    subp t1, t0
    leap 8[cfr, t0, 8], t0
.copyElement:
    loadq [t0], t3
    storeq t3, [t7, t1, 8]
    addp 8, t0
    subi 1, t1
    btinz t1, .copyElement
.isFilled:
    addp constexpr (sizeof(IndexingHeader)), t7

    loadp InstanceArrayAllocator[t5], t0
    allocateCell(t0, t1, t3, .slow)

    # JSGlobalObject::arrayStructureForIndexingTypeDuringAllocation().
    loadp InstanceGlobalObject[t5], t0
    urshifti constexpr IndexingShapeShift, t2
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t2, 4], t2
    storei t2, JSCell::m_structureID[t1]
    structureIDToStructureWithScratch(t2, t0)
    loadi StructureTypeInfoBlob[t2], t0
    storei t0, JSCell::m_indexingTypeAndMisc[t1]
    storep t7, JSObjectWithButterfly::m_butterfly[t1]
    fenceAfterAllocation(t0)

    # ArrayAllocationProfile::updateLastAllocation().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t0
    urshiftq constexpr AOT::bitsOfPointerInArrayAllocationProfile, t0
    lshiftq constexpr AOT::bitsOfPointerInArrayAllocationProfile, t0
    orq t1, t0
    storeq t0, OpNewArray::Metadata::m_arrayAllocationProfile[t6]
    return(t1)

.slow:
    callSlowPath(_llint_slow_path_aot_new_array)
    dispatch()
end)

# JSValue::toBoolean() of what needs no more than the bits of the value and, of a flat string, its length.
macro aotJumpOnTruthiness(opcodeName, opcodeStruct, ifTruthy, ifFalsy)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        macro jump()
            get(m_targetLabel, t0)
            jumpImpl(dispatchIndirect, t0)
        end

        get(m_condition, t1)
        loadConstantOrVariable(size, t1, t0)
        btqnz t0, ~0xf, .isNoBooleanUndefinedOrNull
        btinz t0, 1, .truthy
    .falsy:
        ifFalsy(jump, dispatch)

    .isNoBooleanUndefinedOrNull:
        bqb t0, numberTag, .isNoInt32
        btiz t0, .falsy
    .truthy:
        ifTruthy(jump, dispatch)

    .isNoInt32:
        btqz t0, numberTag, .isNoNumber
        addq numberTag, t0
        fq2d t0, ft0
        move 0, t1
        fq2d t1, ft1
        bdneq ft0, ft1, .truthy
        jmp .falsy

    .isNoNumber:
        btqnz t0, notCellMask, .slow
        loadb JSCell::m_type[t0], t1
        bbeq t1, StringType, .string
        bbbeq t1, constexpr LastValueCompareCellType, .slow
        btbz JSCell::m_flags[t0], constexpr MasqueradesAsUndefined, .truthy
    .slow:
        callSlowPath(_llint_slow_path_aot_%opcodeName%)
        nextInstruction()

    .string:
        loadp JSString::m_fiber[t0], t1
        btpnz t1, isRopeInPointer, .slow
        btinz StringImpl::m_length[t1], .truthy
        jmp .falsy
    end)
end

aotJumpOnTruthiness(jtrue, OpJtrue,
    macro (jump, dispatch) jump() end,
    macro (jump, dispatch) dispatch() end)

aotJumpOnTruthiness(jfalse, OpJfalse,
    macro (jump, dispatch) dispatch() end,
    macro (jump, dispatch) jump() end)

if not C_LOOP
entry(llint_aot, macro ()
    macro setEntryAddresses(opcodeStruct, opcodeName)
        setEntryAddress(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%)
        setEntryAddressWide16(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%_wide16)
        setEntryAddressWide32(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%_wide32)
    end

    setEntryAddresses(OpEnumeratorGetByVal, op_enumerator_get_by_val)
    setEntryAddresses(OpEnumeratorNext, op_enumerator_next)
    setEntryAddresses(OpGetById, op_get_by_id)
    setEntryAddresses(OpGetByVal, op_get_by_val)
    setEntryAddresses(OpInById, op_in_by_id)
    setEntryAddresses(OpInByVal, op_in_by_val)
    setEntryAddresses(OpJfalse, op_jfalse)
    setEntryAddresses(OpJtrue, op_jtrue)
    setEntryAddresses(OpNewArray, op_new_array)
    setEntryAddresses(OpNewObject, op_new_object)
    setEntryAddresses(OpPutById, op_put_by_id)
end)
end
