/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyTuple.h"
#include "PyType.h"

namespace JSC {
namespace Python {

// Tuples whose items have names: sys.version_info, os.stat_result, time.struct_time. (Objects/structseq.c in CPython.)

struct StructSeqDef {
    ASCIILiteral name; // With the module: "sys.version_info".
    std::span<const ASCIILiteral> fields; // A null name: the item has none.
    // How many of the fields are items of the tuple. The rest are attributes only.
    unsigned sequenceFieldCount;
    bool canBeInstantiated { true };
    // Those of sys are like int: nothing can be set on the type.
    bool isImmutable { false };
};

static constexpr unsigned maxStructSeqFields = 32;

// The definition must outlive the type.
PyType* makeStructSeqType(JSGlobalObject*, const StructSeqDef&);
// One value for each field.
PyTuple* makeStructSeq(JSGlobalObject*, PyType*, std::span<const JSValue> values);
inline PyTuple* makeStructSeq(JSGlobalObject* globalObject, PyType* type, std::initializer_list<JSValue> values) { return makeStructSeq(globalObject, type, std::span<const JSValue> { values.begin(), values.size() }); }
// type(sequence, dict): the __new__ of the types, for one that has something to add to it.
JSValue structSeqNew(JSGlobalObject*, JSValue type, const CallArgs&);
// The value of a field, by its index among all the fields.
JSValue structSeqField(JSGlobalObject*, PyTuple*, unsigned index);

} // namespace Python
} // namespace JSC
