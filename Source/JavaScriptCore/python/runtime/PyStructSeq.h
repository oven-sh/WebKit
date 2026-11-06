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
// The type of a definition, made when it is first asked for, and kept where no program gets at it.
PyType* structSeqTypeFor(JSGlobalObject*, const StructSeqDef&);
// One value for each field. Null, with an exception: the type is none of these, or has another number of fields.
PyTuple* makeStructSeq(JSGlobalObject*, PyType*, std::span<const JSValue> values);
inline PyTuple* makeStructSeq(JSGlobalObject* globalObject, PyType* type, std::initializer_list<JSValue> values) { return makeStructSeq(globalObject, type, std::span<const JSValue> { values.begin(), values.size() }); }
// type(sequence, dict): the __new__ of the types, for one that has something to add to it.
JSValue structSeqNew(JSGlobalObject*, JSValue type, const CallArgs&);
// Whether the type is one of these.
bool isStructSeqType(JSGlobalObject*, PyType*);
// The value of a field, by its index among all the fields. None if the tuple has no such field: an instance need not have been made by its type
// (tuple.__new__, once the program has written a __new__ of its own into the type; the __class__ of an instance of another of these, assigned).
// Whoever reads the items of one that a program hands it asks this.
JSValue structSeqField(JSGlobalObject*, PyTuple*, unsigned index);

} // namespace Python
} // namespace JSC
