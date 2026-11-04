# Copyright (C) 2018-2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# This file us consumed by generator/main.rb in order to generate Bytecodes.h/BytecodeStructs.h

types [
    :VirtualRegister,

    :BasicBlockLocation,
    :BoundLabel,
    :DataOnlyCallLinkInfo,
    :DebugHookType,
    :ECMAMode,
    :ErrorTypeWithExtension,
    :EnumeratorMetadata,
    :GetByIdMode,
    :GetByIdModeMetadata,
    :GetPutInfo,
    :IndexingType,
    :IterationModeMetadata,
    :JSCell,
    :JSGlobalLexicalEnvironment,
    :JSGlobalObject,
    :JSModuleEnvironment,
    :JSObject,
    :JSScope,
    :JSType,
    :JSValue,
    :LazyCallLinkInfo,
    :ResultType,
    :OperandTypes,
    :PrivateFieldPutKind,
    :ProfileTypeBytecodeFlag,
    :PropertyOffset,
    :PutByIdFlags,
    :ResolveType,
    # FIXME: We should use WriteBarrierStructureID instead.
    :StructureID,
    :StructureChain,
    :SymbolTable,
    :SymbolTableOrScopeDepth,
    :ToNumberForm,
    :ToThisStatus,
    :TypeLocation,
    :InlineWatchpointSet,
    :WriteBarrierStructureID,

    :ValueProfileAndVirtualRegisterBuffer,
    :ArrayProfile,
    :ArrayAllocationProfile,
    :ObjectAllocationProfile,
]

templates [
    :WriteBarrier,
    :WriteBarrierBase,
]


begin_section :Bytecode,
    emit_in_h_file: true,
    emit_in_structs_file: true,
    emit_in_asm_file: true,
    emit_opcode_id_string_values_in_h_file: true,
    macro_name_component: :BYTECODE,
    asm_prefix: "llint_",
    op_prefix: "op_",
    preserve_order: true

# Ops with checkpoint must come first
op :tail_call_varargs,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        thisValue?: VirtualRegister,
        arguments?: VirtualRegister,
        firstFree: VirtualRegister,
        firstVarArg: int,
    },
    metadata: {
        callLinkInfo: DataOnlyCallLinkInfo,
    },
    tmps: {
        argCountIncludingThis: unsigned
    },
    checkpoints: {
        determiningArgCount: nil,
        makeCall: nil,
    }

op :call_varargs,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        thisValue?: VirtualRegister,
        arguments?: VirtualRegister,
        firstFree: VirtualRegister,
        firstVarArg: int,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: DataOnlyCallLinkInfo,
    },
    tmps: {
        argCountIncludingThis: unsigned,
    },
    checkpoints: {
        determiningArgCount: nil,
        makeCall: nil,
    }

# Semantically, this is nextResult = next.@call(iterator); done = nextResult.done; value = done ? undefined : nextResult.value;
op :iterator_next,
    args: {
        done: VirtualRegister,
        value: VirtualRegister,
        iterable: VirtualRegister,
        next: VirtualRegister,
        iterator: VirtualRegister,
        stackOffset: unsigned,
        nextResultValueProfile: unsigned,
        doneValueProfile: unsigned,
        valueValueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
        doneModeMetadata: GetByIdModeMetadata,
        valueModeMetadata: GetByIdModeMetadata,
        iterableProfile: ArrayProfile,
        iterationMetadata: IterationModeMetadata,
    },
    tmps: {
        nextResult: JSValue,
    },
    checkpoints: {
        computeNext: nil,
        getDone: nil,
        getValue: nil,
    }

op :construct_varargs,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        thisValue?: VirtualRegister,
        arguments?: VirtualRegister,
        firstFree: VirtualRegister,
        firstVarArg: int,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: DataOnlyCallLinkInfo,
    },
    tmps: {
        argCountIncludingThis: unsigned
    },
    checkpoints: {
        determiningArgCount: nil,
        makeCall: nil,
    }

op :super_construct_varargs,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        thisValue?: VirtualRegister,
        arguments?: VirtualRegister,
        firstFree: VirtualRegister,
        firstVarArg: int,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: DataOnlyCallLinkInfo,
        cachedCallee: WriteBarrier[JSCell],
    },
    tmps: {
        argCountIncludingThis: unsigned
    },
    checkpoints: {
        determiningArgCount: nil,
        makeCall: nil,
    }

# Semantically, this is iterator = symbolIterator.@call(iterable); next = iterator.next;
# where symbolIterator the result of iterable[Symbol.iterator] (which is done in a different bytecode).
# For builtin iterators, however, this has special behavior where next becomes a sentinel, which
# indicates that we are in a known iteration mode to op_iterator_next. In IterationMode::FastArray
# and IterationMode::FastString iterator becomes the sentinel instead, and next is the index into iterable.
op :iterator_open,
    args: {
        iterator: VirtualRegister,
        next: VirtualRegister,
        symbolIterator: VirtualRegister,
        iterable: VirtualRegister,
        stackOffset: unsigned,
        iterableValueProfile: unsigned,
        iteratorValueProfile: unsigned,
        nextValueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
        modeMetadata: GetByIdModeMetadata,
        arrayProfile: ArrayProfile,
        iterationMetadata: IterationModeMetadata,
    },
    checkpoints: {
        symbolCall: nil,
        getNext: nil,
    }

# iterator = symbolIterator.@call(iterable); next = iterator.next. Mirrors op_iterator_open; the fast
# path skips the call for a genuine async generator, leaving the driver sentinel in next.
op :async_iterator_open,
    args: {
        iterator: VirtualRegister,
        next: VirtualRegister,
        symbolIterator: VirtualRegister,
        iterable: VirtualRegister,
        stackOffset: unsigned,
        iterableValueProfile: unsigned,
        iteratorValueProfile: unsigned,
        nextValueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
        modeMetadata: GetByIdModeMetadata,
        iterationMetadata: IterationModeMetadata,
    },
    checkpoints: {
        symbolCall: nil,
        getNext: nil,
    }

# Semantically, this is dst = value instanceof constructor.
op :instanceof,
    args: {
        dst: VirtualRegister,
        value: VirtualRegister,
        constructor: VirtualRegister,
        hasInstanceOrPrototype: VirtualRegister,
        hasInstanceValueProfile: unsigned,
        prototypeValueProfile: unsigned,
    },
    metadata: {
        hasInstanceModeMetadata: GetByIdModeMetadata,
        prototypeModeMetadata: GetByIdModeMetadata,
    },
    checkpoints: {
        getHasInstance: nil,
        getPrototype: nil,
        instanceof: nil,
    }

# Opcodes with metadata come next, in decreasing order of metadata alignment requirements
# Alignment: 8
op :set_private_brand, args: {
        base: VirtualRegister,
        brand: VirtualRegister,
    },
    metadata: {
        oldStructureID: StructureID,
        newStructureID: StructureID,
        brand: WriteBarrier[JSCell],
    }

op :check_private_brand,
    args: {
        base: VirtualRegister,
        brand: VirtualRegister,
    },
    metadata: {
        structureID: StructureID,
        brand: WriteBarrier[JSCell],
    }

# A RegExp literal that is the receiver of a call to its own test or exec method, as in /x/.test(string), without the g or y flag.
# Every evaluation of a literal makes a new object, but this one is only ever seen by that builtin, which neither writes to it
# nor lets it out: while that is so (the realm's watchpoints say) one object per site does. See RegExpObject::isSharedLiteral().
op :new_reg_exp_shared,
    args: {
        dst: VirtualRegister,
        regexp: VirtualRegister,
        forTest: bool,
    },
    metadata: {
        cachedObject: WriteBarrier[JSCell],
    }

op :put_by_id,
    args: {
        base: VirtualRegister,
        property: unsigned,
        value: VirtualRegister,
        flags: PutByIdFlags,
    },
    metadata: {
        oldStructureID: StructureID,
        offset: unsigned,
        newStructureID: StructureID,
        structureChain: WriteBarrierBase[StructureChain],
    }

op :construct,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
    }

op :super_construct,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
        cachedCallee: WriteBarrier[JSCell],
    }

op :tail_call,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
    }

op :call_direct_eval,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
        thisValue: VirtualRegister,
        scope: VirtualRegister,
        lexicallyScopedFeatures: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
    }

op_group :CreateInternalFieldObjectOp,
    [
        :create_generator,
        :create_async_generator,
    ],
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
    },
    metadata: {
        cachedCallee: WriteBarrier[JSCell]
    }

op :create_promise,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
    },
    metadata: {
        cachedCallee: WriteBarrier[JSCell]
    }

op :catch,
    args: {
        exception: VirtualRegister,
        thrownValue: VirtualRegister,
    },
    metadata: {
        buffer: ValueProfileAndVirtualRegisterBuffer.*,
        hasExecutedWithoutBuffer: bool, # Options::useLazyCatchLiveness(): executed before its buffer was created; CodeBlock::ensureCatchLivenessIsComputedForExecutedCatches()
    }

op :new_array_with_size,
    args: {
        dst: VirtualRegister,
        length: VirtualRegister,
    },
    metadata: {
        arrayAllocationProfile: ArrayAllocationProfile,
    }

op :new_array_buffer,
    args: {
        dst: VirtualRegister,
        immutableButterfly: VirtualRegister,
        recommendedIndexingType: IndexingType
    },
    metadata: {
        arrayAllocationProfile: ArrayAllocationProfile,
    }

op :get_by_id,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        modeMetadata: GetByIdModeMetadata,
    }

op :get_length,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        modeMetadata: GetByIdModeMetadata,
        arrayProfile: ArrayProfile,
    }

op :profile_type,
    args: {
        targetVirtualRegister: VirtualRegister,
        symbolTableOrScopeDepth: SymbolTableOrScopeDepth,
        flag: ProfileTypeBytecodeFlag,
        identifier?: unsigned,
        resolveType: ResolveType,
    },
    metadata: {
        typeLocation: TypeLocation.*,
    }

op :profile_control_flow,
    args: {
        textOffset: int,
        textEndOffset: int,
        isInLoopHeader: bool,
    },
    metadata: {
        basicBlockLocation: BasicBlockLocation.*,
    }

op :new_array_with_species,
    args: {
        dst: VirtualRegister,
        length: VirtualRegister,
        array: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        arrayAllocationProfile: ArrayAllocationProfile,
        arrayProfile: ArrayProfile,
    }

# op_call variations
op :call,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
    }

op :call_ignore_result,
    args: {
        callee: VirtualRegister,
        argc: unsigned,
        argv: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
    }

# dst = next.call(iterator [, value]), or -- if next is the fast async generator driver sentinel --
# enqueue value onto the producer instead (dst unused; the result is awaited via a separate op_yield).
# The resume value (yield* passes the received value; for-await leaves hasValue false, so next() is
# called with just `this`) is call argument index 1: like op_call's argc/argv model, there's no separate
# VirtualRegister field for it -- its register is derived from stackOffset via
# virtualRegisterForArgumentIncludingThis(1, -stackOffset) whenever hasValue is set. This keeps every
# arg field a plain unsigned/bool, so an absent resume value never forces this bytecode to Wide32
# encoding. Unlike op_iterator_next there are no getDone/getValue checkpoints: the intervening await
# makes .done/.value separate get_by_id bytecodes after suspension.
op :async_iterator_next,
    args: {
        dst: VirtualRegister,
        next: VirtualRegister,
        iterator: VirtualRegister,
        driver: VirtualRegister,
        hasValue: bool,
        stackOffset: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        callLinkInfo: LazyCallLinkInfo,
        iterationMetadata: IterationModeMetadata,
    }


op :resolve_scope,
    args: {
        dst: VirtualRegister, # offset 1
        scope: VirtualRegister, # offset 2
        var: unsigned, # offset 3
        # $begin: :private,
        resolveType: ResolveType,
        localScopeDepth: unsigned,
    },
    metadata: {
        resolveType: ResolveType, # offset 4
        _0: { # offset 5
            localScopeDepth: unsigned,
            globalLexicalBindingEpoch: unsigned,
        },
        _1: { # offset 6
             # written during linking
             symbolTable: WriteBarrierBase[SymbolTable], # lexicalEnvironment (ClosureVar)
             moduleImportSlot: unsigned, # ModuleVar: the import slot, as a variable index in the importing module environment

             constantScope: WriteBarrierBase[JSScope],

             # written from the slow path
             globalLexicalEnvironment: WriteBarrierBase[JSGlobalLexicalEnvironment],
             globalObject: WriteBarrierBase[JSGlobalObject],
        },
    }

op :get_from_scope,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
        var: unsigned,
        getPutInfo: GetPutInfo,
        localScopeDepth: unsigned,
        offset: unsigned,
        valueProfile: unsigned,
    },
    metadata: {
        getPutInfo: GetPutInfo,
        structureID: WriteBarrierStructureID,
        operand: uintptr_t,
    },
    metadata_initializers: {
        getPutInfo: :getPutInfo,
        operand: :offset,
    }

op :put_to_scope,
    args: {
        scope: VirtualRegister,
        var: unsigned,
        value: VirtualRegister,
        getPutInfo: GetPutInfo,
        symbolTableOrScopeDepth: SymbolTableOrScopeDepth,
        offset: unsigned,
    },
    metadata: {
        getPutInfo: GetPutInfo,
        _: {
            structureID: WriteBarrierStructureID,
            watchpointSet: InlineWatchpointSet.*,
        },
        operand: uintptr_t,
    },
    metadata_initializers: {
        getPutInfo: :getPutInfo,
        operand: :offset,
    }

op :create_this,
    args: {
        dst: VirtualRegister,
        callee: VirtualRegister,
        inlineCapacity: unsigned,
    },
    metadata: {
        cachedCallee: WriteBarrier[JSCell]
    }

op :new_object,
    args: {
        dst: VirtualRegister,
        inlineCapacity: unsigned,
    },
    metadata: {
        objectAllocationProfile: ObjectAllocationProfile,
    }

op :new_array,
    args: {
        dst: VirtualRegister,
        argv: VirtualRegister,
        argc: unsigned,
        recommendedIndexingType: IndexingType,
    },
    metadata: {
        arrayAllocationProfile: ArrayAllocationProfile,
    }

op :put_private_name,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        value: VirtualRegister,
        putKind: PrivateFieldPutKind,
    },
    metadata: {
        property: WriteBarrier[JSCell],
        oldStructureID: StructureID,
        offset: unsigned,
        newStructureID: StructureID,
    }

op :get_private_name,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        structureID: StructureID,
        offset: unsigned,
        property: WriteBarrier[JSCell],
    }

# Python: see below. The versions are Python::PyDict::watchedVersion() of the dicts when entry `index` was found to hold the
# name. `builtinsVersion` is 0 if that was in the globals.
op :py_load_global,
    args: {
        dst: VirtualRegister,
        name: VirtualRegister,
    },
    metadata: {
        globalsVersion: uint64_t,
        builtinsVersion: uint64_t,
        index: unsigned,
    }

op :py_store_global,
    args: {
        name: VirtualRegister,
        value: VirtualRegister,
    },
    metadata: {
        globalsVersion: uint64_t,
        index: unsigned,
    }

# The metadata of these two is a Python::AttributeCache: see python/runtime/PyAbstract.h.
op :py_get_attr,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        name: VirtualRegister,
    },
    metadata: {
        payload: uint64_t,
        structureID: StructureID,
        typeVersion: unsigned,
        offset: int,
        jsType: unsigned,
        metatypeVersion: unsigned,
    }

# For base.name(...): the function and what to pass it as its first argument, or what base.name is and the empty value.
op :py_get_method,
    args: {
        dst: VirtualRegister,
        self: VirtualRegister,
        base: VirtualRegister,
        name: VirtualRegister,
    },
    metadata: {
        payload: uint64_t,
        structureID: StructureID,
        typeVersion: unsigned,
        offset: int,
        jsType: unsigned,
        metatypeVersion: unsigned,
    }

# Alignment: 4
op :get_by_val_with_this,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        thisValue: VirtualRegister,
        property: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        arrayProfile: ArrayProfile,
    }

op :get_by_val,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        arrayProfile: ArrayProfile,
    }

op :put_by_val,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        value: VirtualRegister,
        ecmaMode: ECMAMode,
    },
    metadata: {
        arrayProfile: ArrayProfile,
    }

op :put_by_val_direct,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        value: VirtualRegister,
        ecmaMode: ECMAMode,
    },
    metadata: {
        arrayProfile: ArrayProfile,
    }

op :in_by_val,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: VirtualRegister,
    },
    metadata: {
        arrayProfile: ArrayProfile,
    }

op :enumerator_next,
    args: {
        # out
        propertyName: VirtualRegister,
        # in/out
        mode: VirtualRegister, # Will always be a JS UInt32 representing a JSForInMode.
        index: VirtualRegister, # Gets reset to zero every time mode changes.
        # in
        base: VirtualRegister,
        enumerator: VirtualRegister,
    },
    metadata: {
        arrayProfile: ArrayProfile,
        enumeratorMetadata: EnumeratorMetadata,
    }

op :enumerator_in_by_val,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        mode: VirtualRegister,
        propertyName: VirtualRegister,
        index: VirtualRegister,
        enumerator: VirtualRegister,
    },
    metadata: {
        arrayProfile: ArrayProfile,
        enumeratorMetadata: EnumeratorMetadata,
    }

op :enumerator_has_own_property,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        mode: VirtualRegister,
        propertyName: VirtualRegister,
        index: VirtualRegister,
        enumerator: VirtualRegister,
    },
    metadata: {
        arrayProfile: ArrayProfile,
        enumeratorMetadata: EnumeratorMetadata,
    }

op :enumerator_put_by_val,
    args: {
        base: VirtualRegister,
        mode: VirtualRegister,
        propertyName: VirtualRegister,
        index: VirtualRegister,
        enumerator: VirtualRegister,
        value: VirtualRegister,
        ecmaMode: ECMAMode,
    },
    metadata: {
        arrayProfile: ArrayProfile,
        enumeratorMetadata: EnumeratorMetadata,
    }

op :to_this,
    args: {
        srcDst: VirtualRegister,
        ecmaMode: ECMAMode,
        valueProfile: unsigned,
    },
    metadata: {
        cachedStructureID: StructureID,
        toThisStatus: ToThisStatus,
    }

op :enumerator_get_by_val,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        mode: VirtualRegister,
        propertyName: VirtualRegister,
        index: VirtualRegister,
        enumerator: VirtualRegister,
        valueProfile: unsigned,
    },
    metadata: {
        arrayProfile: ArrayProfile,
        enumeratorMetadata: EnumeratorMetadata,
    }

op :get_by_id_direct,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: unsigned,
        valueProfile: unsigned, # not used in llint
    },
    metadata: {
        structureID: StructureID,
        offset: unsigned,
    }

# An instance of a class with the old Structure, whose type has the version, has the attribute at the offset, or has it there
# once it has the new Structure.
op :py_set_attr,
    args: {
        base: VirtualRegister,
        name: VirtualRegister,
        value: VirtualRegister,
    },
    metadata: {
        oldStructureID: StructureID,
        newStructureID: StructureID,
        typeVersion: unsigned,
        offset: int,
    }

# Alignment: 1
op :jneq_ptr,
    args: {
        value: VirtualRegister,
        specialPointer: VirtualRegister,
        targetLabel: BoundLabel,
    },
    metadata: {
        hasJumped: bool,
    }

op :iterator_close_check,
    args: {
        iterator: VirtualRegister,
        next: VirtualRegister,
        iterable: VirtualRegister,
        targetLabel: BoundLabel,
    },
    metadata: {
        seenModes: uint8_t,
    }

# Opcodes without metadata are last
op :get_argument,
    args: {
        dst: VirtualRegister,
        index: int,
        valueProfile: unsigned,
    }

op :get_from_arguments,
    args: {
        dst: VirtualRegister,
        arguments: VirtualRegister,
        index: unsigned,
        valueProfile: unsigned,
    }

op :get_prototype_of,
    args: {
        dst: VirtualRegister,
        value: VirtualRegister,
        valueProfile: unsigned,
    }

op :get_internal_field,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        index: unsigned,
        valueProfile: unsigned,
    }

op :get_by_id_with_this,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        thisValue: VirtualRegister,
        property: unsigned,
        valueProfile: unsigned,
    }

op :to_object,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        message: unsigned,
        valueProfile: unsigned,
    }

op :in_by_id,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: unsigned,
    }

op :has_private_name,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: VirtualRegister,
    }

op :has_private_brand,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        brand: VirtualRegister,
    }

op :put_by_id_with_this,
    args: {
        base: VirtualRegister,
        thisValue: VirtualRegister,
        property: unsigned,
        value: VirtualRegister,
        ecmaMode: ECMAMode,
    }

op :del_by_id,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: unsigned,
        ecmaMode: ECMAMode,
    }

op :put_by_val_with_this,
    args: {
        base: VirtualRegister,
        thisValue: VirtualRegister,
        property: VirtualRegister,
        value: VirtualRegister,
        ecmaMode: ECMAMode,
    }

op :del_by_val,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        property: VirtualRegister,
        ecmaMode: ECMAMode,
    }

op :put_getter_by_id,
    args: {
        base: VirtualRegister,
        property: unsigned,
        attributes: unsigned,
        accessor: VirtualRegister,
    }

op :put_setter_by_id,
    args: {
        base: VirtualRegister,
        property: unsigned,
        attributes: unsigned,
        accessor: VirtualRegister,
    }

op :put_getter_setter_by_id,
    args: {
        base: VirtualRegister,
        property: unsigned,
        attributes: unsigned,
        getter: VirtualRegister,
        setter: VirtualRegister,
    }

op :put_getter_by_val,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        attributes: unsigned,
        accessor: VirtualRegister,
    }

op :put_setter_by_val,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        attributes: unsigned,
        accessor: VirtualRegister,
    }

op :define_data_property,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        value: VirtualRegister,
        attributes: VirtualRegister,
    }

op :define_accessor_property,
    args: {
        base: VirtualRegister,
        property: VirtualRegister,
        getter: VirtualRegister,
        setter: VirtualRegister,
        attributes: VirtualRegister,
    }

op :jmp,
    args: {
        targetLabel: BoundLabel,
    }

op :jtrue,
    args: {
        condition: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jfalse,
    args: {
        condition: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jeq_null,
    args: {
        value: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jneq_null,
    args: {
        value: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jundefined_or_null,
    args: {
        value: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jnundefined_or_null,
    args: {
        value: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :jeq_ptr,
    args: {
        value: VirtualRegister,
        specialPointer: VirtualRegister,
        targetLabel: BoundLabel,
    }

op_group :BinaryJmp,
    [
        :jeq,
        :jstricteq,
        :jneq,
        :jnstricteq,
        :jless,
        :jlesseq,
        :jgreater,
        :jgreatereq,
        :jnless,
        :jnlesseq,
        :jngreater,
        :jngreatereq,
        :jbelow,
        :jbeloweq,
    ],
    args: {
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :loop_hint

op_group :SwitchValue,
    [
        :switch_imm,
        :switch_char,
        :switch_string,
    ],
    args: {
        tableIndex: unsigned,
        scrutinee: VirtualRegister,
    }

op_group :NewFunction,
    [
        :new_func,
        :new_func_exp,
        :new_generator_func,
        :new_generator_func_exp,
        :new_async_func,
        :new_async_func_exp,
        :new_async_generator_func,
        :new_async_generator_func_exp,
    ],
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
        functionDecl: unsigned,
    }

op :set_function_name,
    args: {
        function: VirtualRegister,
        name: VirtualRegister,
    }

op :ret,
    args: {
        value: VirtualRegister,
    }

op :strcat,
    args: {
        dst: VirtualRegister,
        src: VirtualRegister,
        count: int,
    }

op :to_primitive,
    args: {
        dst: VirtualRegister,
        src: VirtualRegister,
    }

op :to_property_key,
    args: {
        dst: VirtualRegister,
        src: VirtualRegister,
    }

op :to_property_key_or_number,
    args: {
        dst: VirtualRegister,
        src: VirtualRegister,
    }

op :put_to_arguments,
    args: {
        arguments: VirtualRegister,
        index: unsigned,
        value: VirtualRegister,
    }

op :push_with_scope,
    args: {
        dst: VirtualRegister,
        currentScope: VirtualRegister,
        newScope: VirtualRegister,
    }

op :create_lexical_environment,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
        symbolTable: VirtualRegister,
        initialValue: VirtualRegister,
    }

op :create_generator_frame_environment,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
        symbolTable: VirtualRegister,
        initialValue: VirtualRegister,
    }

op :get_parent_scope,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
    }

op :throw,
    args: {
        value: VirtualRegister,
    }

op :throw_static_error,
    args: {
        message: VirtualRegister,
        errorType: ErrorTypeWithExtension,
    }

op :debug,
    args: {
        debugHookType: DebugHookType,
        data: VirtualRegister,
    }

op :get_property_enumerator,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
    }

op :unreachable

op :create_rest,
    args: {
        dst: VirtualRegister,
        numParametersToSkip: unsigned,
    }

op :yield,
    args: {
        yieldPoint: unsigned,
        argument: VirtualRegister,
    }

op :check_traps

# Python. What these do is in python/runtime/PySlowPaths.cpp. A `name` is a constant, a JSString.

# Puts the arguments of the call in the parameters of the function. If all parameters can be passed by position and by name
# and there is nothing else to fill in, `simpleArgumentCountIncludingThis` is what a call that passes them all by position has.
# Otherwise it is 0.
op :py_bind_args,
    args: {
        simpleArgumentCountIncludingThis: unsigned,
    }

# The way back in a loop. As check_traps, which also makes the calls the Python runtime owes (VMTraps::NeedPythonCalls), and then jmp.
op :py_jmp_back,
    args: {
        targetLabel: BoundLabel,
    }

# As ret. A float that is a whole number is an object (Python::PyFloat); code that is not Python gets the number.
op :py_ret,
    args: {
        value: VirtualRegister,
    }

# Throws if the cell is empty. `kind`: Python::UnboundKind.
op :py_load_cell,
    args: {
        dst: VirtualRegister,
        cell: VirtualRegister,
        name: VirtualRegister,
        kind: unsigned,
    }

op :py_store_cell,
    args: {
        cell: VirtualRegister,
        value: VirtualRegister,
    }

# For a list that the code itself is building: that of a comprehension, of a display with a star in it.
op :py_list_append,
    args: {
        list: VirtualRegister,
        value: VirtualRegister,
    }

op :py_load_name,
    args: {
        dst: VirtualRegister,
        locals: VirtualRegister,
        name: VirtualRegister,
    }

op :py_store_name,
    args: {
        locals: VirtualRegister,
        name: VirtualRegister,
        value: VirtualRegister,
    }

# For callee(...), where callee may be a class whose instances are made by object.__new__ and have an __init__ written in
# Python: a new instance and that function, which the code then calls itself. For any other callee, `self` is the empty value.
op :py_prepare_construct,
    args: {
        dst: VirtualRegister,
        self: VirtualRegister,
        callee: VirtualRegister,
    }

# What such a call evaluates to, after __init__ has returned `result`.
op :py_finish_construct,
    args: {
        dst: VirtualRegister,
        self: VirtualRegister,
        result: VirtualRegister,
    }

op :py_get_item,
    args: {
        dst: VirtualRegister,
        base: VirtualRegister,
        key: VirtualRegister,
    }

op :py_set_item,
    args: {
        base: VirtualRegister,
        key: VirtualRegister,
        value: VirtualRegister,
    }

# `operation`: Python::BinaryOperator
op :py_binary,
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        operation: unsigned,
    }

# The same, for the operators that the interpreter does itself when the operands are numbers.
op_group :PyArithmetic,
    [
        :py_add,
        :py_sub,
        :py_mul,
        :py_div,
        :py_floor_div,
        :py_mod,
    ],
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        operation: unsigned,
    }

# `operation`: Python::UnaryOperator
op :py_unary,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        operation: unsigned,
    }

# `operation`: Python::CompareOperator
op :py_compare,
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        operation: unsigned,
    }

op_group :PyRichCompare,
    [
        :py_eq,
        :py_ne,
        :py_lt,
        :py_le,
        :py_gt,
        :py_ge,
    ],
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
    }

# A comparison, and py_jtrue on its result. With an n: py_jfalse.
op_group :PyCompareJump,
    [
        :py_jeq,
        :py_jne,
        :py_jlt,
        :py_jle,
        :py_jgt,
        :py_jge,
        :py_jneq,
        :py_jnne,
        :py_jnlt,
        :py_jnle,
        :py_jngt,
        :py_jnge,
    ],
    args: {
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :py_jtrue,
    args: {
        condition: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :py_jfalse,
    args: {
        condition: VirtualRegister,
        targetLabel: BoundLabel,
    }

op :py_get_iter,
    args: {
        dst: VirtualRegister,
        iterable: VirtualRegister,
    }

# The next item, or a jump when there is none. `stackOffset`, as the argv of op_call, is where there is room for the frame of
# the body of a generator, which the instruction calls itself.
op :py_for_iter,
    args: {
        dst: VirtualRegister,
        iterator: VirtualRegister,
        targetLabel: BoundLabel,
        stackOffset: unsigned,
    }

# A step of `yield from` or `await`: `value` goes to the iterator, as send() does it, or throw() if `mode` says so
# (JSGenerator::ResumeMode). What the iterator yields, or what it returns and a jump.
op :py_send,
    args: {
        dst: VirtualRegister,
        iterator: VirtualRegister,
        value: VirtualRegister,
        mode: VirtualRegister,
        targetLabel: BoundLabel,
        stackOffset: unsigned,
    }

# The body of a generator (python/runtime/PyGenerator.h) has been called: the generator is running. If it has run before it
# goes on where it stopped. (The op_switch_imm that follows never gets to do that: it is there to say where control may go.)
op :py_generator_enter

# It stops running, and `value` goes to whoever resumed it. The next time it goes on with the instruction that follows.
# First it saves registers as op_py_generator_registers does, if `count` is not 0.
op :py_generator_suspend,
    args: {
        value: VirtualRegister,
        first: VirtualRegister,
        count: unsigned,
        offset: unsigned,
    }

# It is done, and `value` goes to whoever resumed it.
op :py_generator_return,
    args: {
        value: VirtualRegister,
    }

# `count` registers from `first` down go to (`isSave`), or come from, the slots of the generator (Python::PyGenerator::slots())
# from `offset` up. What comes from a slot leaves it empty.
op :py_generator_registers,
    args: {
        first: VirtualRegister,
        count: unsigned,
        offset: unsigned,
        isSave: bool,
    }

# The items of an iterable go in `before` registers from `argv` down. If `afterPlusOne` is not 0, then a list of all but the
# last `afterPlusOne - 1` goes in the next, and those in the ones that follow.
op :py_unpack,
    args: {
        argv: VirtualRegister,
        iterable: VirtualRegister,
        before: unsigned,
        afterPlusOne: unsigned,
    }

# Everything else. `function`: Python::RuntimeFunction. Its arguments are in `argc` registers from `argv` down.
op :py_runtime,
    args: {
        dst: VirtualRegister,
        function: unsigned,
        argv: VirtualRegister,
        argc: unsigned,
    }

op :log_shadow_chicken_prologue,
    args: {
        scope: VirtualRegister,
    }

op :log_shadow_chicken_tail,
    args: {
        thisValue: VirtualRegister,
        scope: VirtualRegister,
    }

op :resolve_scope_for_hoisting_func_decl_in_eval,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
        property: unsigned,
    }

op :put_internal_field,
    args: {
        base: VirtualRegister,
        index: unsigned,
        value: VirtualRegister,
    }

op :nop

op :super_sampler_begin

op :super_sampler_end

op :enter

op :get_scope,
    args: {
        dst: VirtualRegister
    }

op :create_direct_arguments,
    args: {
        dst: VirtualRegister,
    }

op :create_scoped_arguments,
    args: {
        dst: VirtualRegister,
        scope: VirtualRegister,
    }

op :create_cloned_arguments,
    args: {
        dst: VirtualRegister,
    }

op :new_promise,
    args: {
        dst: VirtualRegister,
    }

op :new_generator,
    args: {
        dst: VirtualRegister,
    }

op :new_async_function_generator,
    args: {
        dst: VirtualRegister,
    }

op :argument_count,
    args: {
        dst: VirtualRegister,
    }

op :check_tdz,
    args: {
        targetVirtualRegister: VirtualRegister,
        identifier?: VirtualRegister,
    }

op :new_array_with_spread,
    args: {
        dst: VirtualRegister,
        argv: VirtualRegister,
        argc: unsigned,
        bitVector: unsigned,
    }

op :spread,
    args: {
        dst: VirtualRegister,
        argument: VirtualRegister,
    }

op :new_reg_exp,
    args: {
        dst: VirtualRegister,
        regexp: VirtualRegister,
    }

op :mov,
    args: {
        dst: VirtualRegister,
        src: VirtualRegister,
    }

op_group :BinaryOp,
    [
        :eq,
        :neq,
        :stricteq,
        :nstricteq,
        :less,
        :lesseq,
        :greater,
        :greatereq,
        :below,
        :beloweq,
        :mod,
        :pow,
        :urshift,
    ],
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
    }

op_group :ProfiledBinaryOpWithOperandTypes,
    [
        :add,
        :mul,
        :div,
        :sub,
        :bitand,
        :bitor,
        :bitxor,
    ],
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        profileIndex: unsigned,
        operandTypes: OperandTypes,
    }

op_group :ProfiledBinaryOp,
    [
        :lshift,
        :rshift,
    ],
    args: {
        dst: VirtualRegister,
        lhs: VirtualRegister,
        rhs: VirtualRegister,
        profileIndex: unsigned,
    }

op_group :UnaryOp,
    [
        :eq_null,
        :neq_null,
        :to_string,
        :is_empty,
        :typeof_is_undefined,
        :typeof_is_object,
        :typeof_is_function,
        :is_undefined_or_null,
        :is_boolean,
        :is_number,
        :is_object,
        :is_callable,
        :is_constructor,
    ],
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
    }

op_group :UnaryInPlaceProfiledOp,
    [
        :inc,
        :dec,
    ],
    args: {
        srcDst: VirtualRegister,
        profileIndex: unsigned,
    }

op :negate,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        profileIndex: unsigned,
        resultType: ResultType,
    }

op :not,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
    }


op :identity_with_profile,
    args: {
        srcDst: VirtualRegister,
        topProfile: unsigned,
        bottomProfile: unsigned,
    }

op :typeof,
    args: {
        dst: VirtualRegister,
        value: VirtualRegister,
    }

op :is_cell_with_type,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        type: JSType,
    }

op :has_structure_with_flags,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        flags: unsigned,
    }

op :to_number,
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        profileIndex: unsigned,
        form: ToNumberForm,
    }

op_group :ProfiledUnaryOp,
    [
        :to_numeric,
        :bitnot,
        :unsigned,
    ],
    args: {
        dst: VirtualRegister,
        operand: VirtualRegister,
        profileIndex: unsigned,
    }

# If you add any bytecodes after bitnot make sure to update isOpcode

# The four ids that are no opcodes come last, so that one compare tells an ordinary narrow instruction from a prefix and from an
# instruction that is instrumented: see Instruction::opcodeID().

op :wide16

op :wide32

# No code is compiled to this. It takes the place of the opcode of an instruction, whose operands stay, for as long as
# sys.monitoring is to tell of what happens there: see python/runtime/PyMonitoring.h.
op :py_instrumented

# A prefix, as wide16 and wide32 are, and behind them if there is one of them: the byte that follows numbers an opcode of the
# section BytecodePage2.
op :page2

# An opcode is a byte, and there are only so many. These are on the second page: an instruction is a byte longer and takes the
# interpreter one more dispatch (three machine instructions). Otherwise they are opcodes like any other: nothing that
# defines, emits, decodes or executes one knows which page it is on, and one moves by being put on this list or taken off it.
# What stays on the first page: whatever has metadata or checkpoints (the id indexes tables), nop (it fills single bytes), and
# whatever reaches 0.1 % of the bytecodes that some real program executes, or 1 % of the instructions that it links.
# (To put the mechanism to the test: on_page2 opcodes_that_can_be_on_page2([:nop, :enter, :wide16, :wide32, :py_instrumented, :page2]).)
on_page2 [
    :py_eq,
    :py_ne,
    :py_lt,
    :py_le,
    :py_gt,
    :py_ge,
    :py_jne,
    :py_jlt,
    :py_jle,
    :py_jge,
    :py_jnne,
    :py_jngt,
    :py_jnge,
    :py_unary,
    :py_floor_div,
    :py_store_cell,
]

end_section :Bytecode

begin_section :CLoopHelpers,
    emit_in_h_file: true,
    macro_name_component: :CLOOP_BYTECODE_HELPER

op :llint_entry
op :llint_vm_entry_to_javascript
op :llint_vm_entry_to_native

end_section :CLoopHelpers

begin_section :NativeHelpers,
    emit_in_h_file: true,
    emit_in_asm_file: true,
    macro_name_component: :BYTECODE_HELPER

# This one has to fit in a byte (LLInt::initialize()), so it comes right after the bytecodes.
op :llint_throw_from_slow_path_trampoline, extras: { asm_section: :trampoline }
op :llint_program_prologue
op :llint_eval_prologue
op :llint_module_program_prologue
op :llint_function_for_call_prologue
op :llint_function_for_construct_prologue
op :llint_function_for_call_arity_check
op :llint_function_for_construct_arity_check
op :llint_generic_return_point
op :llint_throw_during_call_trampoline, extras: { asm_section: :trampoline }
op :llint_native_call_trampoline, extras: { asm_section: :trampoline }
op :llint_native_construct_trampoline, extras: { asm_section: :trampoline }
op :llint_internal_function_call_trampoline, extras: { asm_section: :trampoline }
op :llint_internal_function_construct_trampoline, extras: { asm_section: :trampoline }
op :llint_default_call_trampoline, extras: { asm_section: :trampoline }
op :llint_unlinked_call_trampoline, extras: { asm_section: :trampoline }
op :llint_virtual_call_trampoline, extras: { asm_section: :trampoline }
op :llint_virtual_construct_trampoline, extras: { asm_section: :trampoline }
op :llint_virtual_tail_call_trampoline, extras: { asm_section: :trampoline }
op :llint_polymorphic_normal_call_trampoline, extras: { asm_section: :trampoline }
op :llint_polymorphic_closure_call_trampoline, extras: { asm_section: :trampoline }
op :checkpoint_osr_exit_from_inlined_call_trampoline
op :checkpoint_osr_exit_trampoline
op :normal_osr_exit_trampoline
op :array_sort_comparator_return_trampoline
op :fuzzer_return_early_from_loop_hint
op :loop_osr_entry_gate
op :llint_get_host_call_return_value, extras: { asm_section: :trampoline }
op :llint_handle_uncaught_exception, extras: { asm_section: :trampoline }
op :op_call_return_location
op :op_call_ignore_result_return_location
op :op_construct_return_location
op :op_super_construct_return_location
op :op_call_varargs_return_location
op :op_construct_varargs_return_location
op :op_super_construct_varargs_return_location
op :op_get_by_id_return_location
op :op_async_iterator_open_return_location
op :op_get_by_id_direct_return_location
op :op_get_length_return_location
op :op_get_by_val_return_location
op :op_put_by_id_return_location
op :op_put_by_val_return_location
op :op_put_by_val_direct_return_location
op :op_in_by_id_return_location
op :op_in_by_val_return_location
op :op_instanceof_return_location
op :op_enumerator_get_by_val_return_location
op :op_enumerator_put_by_val_return_location
op :op_enumerator_in_by_val_return_location
op :op_iterator_open_return_location
op :op_iterator_next_return_location
op :op_async_iterator_next_return_location
op :op_call_direct_eval_slow_return_location
op :js_to_wasm_wrapper_entry, extras: { asm_section: :webAssembly }
op :wasm_to_wasm_ipint_wrapper_entry, extras: { asm_section: :webAssembly }
op :wasm_to_js_wrapper_entry, extras: { asm_section: :webAssembly }
op :ipint_trampoline, extras: { asm_section: :webAssembly }
op :ipint_entry, extras: { asm_section: :webAssembly }
op :ipint_catch_entry, extras: { asm_section: :webAssembly }
op :ipint_catch_all_entry, extras: { asm_section: :webAssembly }
op :ipint_table_catch_entry, extras: { asm_section: :webAssembly }
op :ipint_table_catch_ref_entry, extras: { asm_section: :webAssembly }
op :ipint_table_catch_all_entry, extras: { asm_section: :webAssembly }
op :ipint_table_catch_allref_entry, extras: { asm_section: :webAssembly }

op :js_trampoline_op_call
op :js_trampoline_op_call_ignore_result
op :js_trampoline_op_construct
op :js_trampoline_op_super_construct
op :js_trampoline_op_call_varargs
op :js_trampoline_op_construct_varargs
op :js_trampoline_op_super_construct_varargs
op :js_trampoline_op_iterator_next
op :js_trampoline_op_iterator_open
op :js_trampoline_op_async_iterator_open
op :js_trampoline_op_async_iterator_next
op :js_trampoline_op_call_direct_eval_slow
op :js_trampoline_llint_function_for_call_arity_check_untag
op :js_trampoline_llint_function_for_call_arity_check_tag
op :js_trampoline_llint_function_for_construct_arity_check_untag
op :js_trampoline_llint_function_for_construct_arity_check_tag
op :wasm_trampoline_wasm_ipint_call, extras: { asm_section: :webAssembly }
op :wasm_trampoline_wasm_ipint_tail_call, extras: { asm_section: :webAssembly }
op :wasm_throw_from_slow_path_trampoline, extras: { asm_section: :webAssembly }
op :wasm_throw_from_fault_handler_trampoline_reg_instance, extras: { asm_section: :webAssembly }
op :wasm_ipint_call_return_location, extras: { asm_section: :webAssembly }

end_section :NativeHelpers

begin_section :CLoopReturnHelpers,
    emit_in_h_file: true,
    macro_name_component: :CLOOP_RETURN_HELPER

op :llint_return_to_host
op :llint_cloop_did_return_from_js_1
op :llint_cloop_did_return_from_js_2
op :llint_cloop_did_return_from_js_3
op :llint_cloop_did_return_from_js_4
op :llint_cloop_did_return_from_js_5
op :llint_cloop_did_return_from_js_6
op :llint_cloop_did_return_from_js_7
op :llint_cloop_did_return_from_js_8
op :llint_cloop_did_return_from_js_9
op :llint_cloop_did_return_from_js_10
op :llint_cloop_did_return_from_js_11
op :llint_cloop_did_return_from_js_12
op :llint_cloop_did_return_from_js_13
op :llint_cloop_did_return_from_js_14
op :llint_cloop_did_return_from_js_15
op :llint_cloop_did_return_from_js_16
op :llint_cloop_did_return_from_js_17
op :llint_cloop_did_return_from_js_18
op :llint_cloop_did_return_from_js_19
op :llint_cloop_did_return_from_js_20
op :llint_cloop_did_return_from_js_21
op :llint_cloop_did_return_from_js_22
op :llint_cloop_did_return_from_js_23
op :llint_cloop_did_return_from_js_24
op :llint_cloop_did_return_from_js_25
op :llint_cloop_did_return_from_js_26
op :llint_cloop_did_return_from_js_27
op :llint_cloop_did_return_from_js_28
op :llint_cloop_did_return_from_js_29
op :llint_cloop_did_return_from_js_30
op :llint_cloop_did_return_from_js_31
op :llint_cloop_did_return_from_js_32
op :llint_cloop_did_return_from_js_33
op :llint_cloop_did_return_from_js_34
op :llint_cloop_did_return_from_js_35
op :llint_cloop_did_return_from_js_36
op :llint_cloop_did_return_from_js_37

end_section :CLoopReturnHelpers

# What on_page2 names. Their ids come behind all the others.
begin_section :BytecodePage2,
    emit_in_h_file: true,
    emit_in_structs_file: true,
    emit_in_asm_file: true,
    emit_opcode_id_string_values_in_h_file: true,
    page2: true,
    macro_name_component: :BYTECODE_PAGE2,
    asm_prefix: "llint_",
    op_prefix: "op_",
    preserve_order: true

end_section :BytecodePage2
