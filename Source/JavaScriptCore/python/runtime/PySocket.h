/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyNumbers.h"
#include "PyObject.h"

namespace JSC {
namespace Python {

// What another module written in C++ needs to know of a socket: _ssl, which is the host's to write, because the library is.
// (PySocketModule_APIObject in CPython.)

// A socket is a record. Its number is the descriptor, or -1.
enum SocketField : unsigned {
    SocketFamily,
    SocketKind,
    SocketProtocol,
    SocketTimeout, // In nanoseconds. Negative: it blocks. 0: it does not. Otherwise the descriptor does not, and is polled.
    SocketFieldCount
};

// _socket.socket
PyType* socketType(JSGlobalObject*);

inline int socketDescriptor(PyRecord* socket) { return static_cast<int>(socket->number); }
inline int64_t socketTimeoutOf(PyRecord* socket) { return tryInt64(socket->get(SocketTimeout)).value_or(-1); }

} // namespace Python
} // namespace JSC
