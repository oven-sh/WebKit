/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <cstdint>
#include <wtf/PlatformRegisters.h>

namespace JSC { namespace AOT {

void installFaultHandler();

// A thread that has run out of stack has none left for whoever reports that.
void takeFaultsOfThisThreadOnAnotherStack();

// Only where readsOffNoCellFault: compiled code leaves the question whether a value is a cell to a read through it, and a boxed double is no address that an x86-64 processor takes.
// An emulator on an arm64 machine may hand the address to a processor that ignores its top byte: what is left of such a double may name memory that is mapped, and the read goes through.
// One read, in the form that the encoder writes, through a word of our own with the byte above it, under an action for the signal that lasts as long as the read. True: it faulted. With a byte of zero the read goes through on any host.
inline constexpr uint8_t topByteOfProbedAddress = 0x41;
JS_EXPORT_PRIVATE bool readThroughAddressWithTopByteFaults(uint8_t topByte = topByteOfProbedAddress);

// Whether compiled code raised this access fault on purpose. If so, the registers continue where that code handles it.
JS_EXPORT_PRIVATE bool handleAccessFault(void* faultingAddress, PlatformRegisters&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
