/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PythonOperations.h"
#include <errno.h>

// The module errno: Modules/errnomodule.c of CPython.

namespace JSC { namespace Python {

JSObject* createErrnoModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "errno"_s);
    PyDict* names = PyDict::create(globalObject);
    module->putDirect(vm, Identifier::fromString(vm, "errorcode"_s), names);
    // Where two names are for one number, the number is known by the later of them.
    auto add = [&] (ASCIILiteral name, int code) {
        module->putDirect(vm, Identifier::fromString(vm, name), jsNumber(code));
        names->set(globalObject, jsNumber(code), jsString(vm, String(name)));
    };

    // In the order in which CPython has them, which is the order that errorcode is in.
#ifdef ENODEV
    add("ENODEV"_s, ENODEV);
#endif
#ifdef ENOCSI
    add("ENOCSI"_s, ENOCSI);
#endif
#ifdef EHOSTUNREACH
    add("EHOSTUNREACH"_s, EHOSTUNREACH);
#else
#ifdef WSAEHOSTUNREACH
    add("EHOSTUNREACH"_s, WSAEHOSTUNREACH);
#endif
#endif
#ifdef ENOMSG
    add("ENOMSG"_s, ENOMSG);
#endif
#ifdef EUCLEAN
    add("EUCLEAN"_s, EUCLEAN);
#endif
#ifdef EL2NSYNC
    add("EL2NSYNC"_s, EL2NSYNC);
#endif
#ifdef EL2HLT
    add("EL2HLT"_s, EL2HLT);
#endif
#ifdef ENODATA
    add("ENODATA"_s, ENODATA);
#endif
#ifdef ENOTBLK
    add("ENOTBLK"_s, ENOTBLK);
#endif
#ifdef ENOSYS
    add("ENOSYS"_s, ENOSYS);
#endif
#ifdef EPIPE
    add("EPIPE"_s, EPIPE);
#endif
#ifdef EINVAL
    add("EINVAL"_s, EINVAL);
#else
#ifdef WSAEINVAL
    add("EINVAL"_s, WSAEINVAL);
#endif
#endif
#ifdef EOVERFLOW
    add("EOVERFLOW"_s, EOVERFLOW);
#endif
#ifdef EADV
    add("EADV"_s, EADV);
#endif
#ifdef EINTR
    add("EINTR"_s, EINTR);
#else
#ifdef WSAEINTR
    add("EINTR"_s, WSAEINTR);
#endif
#endif
#ifdef EUSERS
    add("EUSERS"_s, EUSERS);
#else
#ifdef WSAEUSERS
    add("EUSERS"_s, WSAEUSERS);
#endif
#endif
#ifdef ENOTEMPTY
    add("ENOTEMPTY"_s, ENOTEMPTY);
#else
#ifdef WSAENOTEMPTY
    add("ENOTEMPTY"_s, WSAENOTEMPTY);
#endif
#endif
#ifdef ENOBUFS
    add("ENOBUFS"_s, ENOBUFS);
#else
#ifdef WSAENOBUFS
    add("ENOBUFS"_s, WSAENOBUFS);
#endif
#endif
#ifdef EPROTO
    add("EPROTO"_s, EPROTO);
#endif
#ifdef EREMOTE
    add("EREMOTE"_s, EREMOTE);
#else
#ifdef WSAEREMOTE
    add("EREMOTE"_s, WSAEREMOTE);
#endif
#endif
#ifdef ENAVAIL
    add("ENAVAIL"_s, ENAVAIL);
#endif
#ifdef ECHILD
    add("ECHILD"_s, ECHILD);
#endif
#ifdef ELOOP
    add("ELOOP"_s, ELOOP);
#else
#ifdef WSAELOOP
    add("ELOOP"_s, WSAELOOP);
#endif
#endif
#ifdef EXDEV
    add("EXDEV"_s, EXDEV);
#endif
#ifdef E2BIG
    add("E2BIG"_s, E2BIG);
#endif
#ifdef ESRCH
    add("ESRCH"_s, ESRCH);
#endif
#ifdef EMSGSIZE
    add("EMSGSIZE"_s, EMSGSIZE);
#else
#ifdef WSAEMSGSIZE
    add("EMSGSIZE"_s, WSAEMSGSIZE);
#endif
#endif
#ifdef EAFNOSUPPORT
    add("EAFNOSUPPORT"_s, EAFNOSUPPORT);
#else
#ifdef WSAEAFNOSUPPORT
    add("EAFNOSUPPORT"_s, WSAEAFNOSUPPORT);
#endif
#endif
#ifdef EBADR
    add("EBADR"_s, EBADR);
#endif
#ifdef EHOSTDOWN
    add("EHOSTDOWN"_s, EHOSTDOWN);
#else
#ifdef WSAEHOSTDOWN
    add("EHOSTDOWN"_s, WSAEHOSTDOWN);
#endif
#endif
#ifdef EPFNOSUPPORT
    add("EPFNOSUPPORT"_s, EPFNOSUPPORT);
#else
#ifdef WSAEPFNOSUPPORT
    add("EPFNOSUPPORT"_s, WSAEPFNOSUPPORT);
#endif
#endif
#ifdef ENOPROTOOPT
    add("ENOPROTOOPT"_s, ENOPROTOOPT);
#else
#ifdef WSAENOPROTOOPT
    add("ENOPROTOOPT"_s, WSAENOPROTOOPT);
#endif
#endif
#ifdef EBUSY
    add("EBUSY"_s, EBUSY);
#endif
#ifdef EWOULDBLOCK
    add("EWOULDBLOCK"_s, EWOULDBLOCK);
#else
#ifdef WSAEWOULDBLOCK
    add("EWOULDBLOCK"_s, WSAEWOULDBLOCK);
#endif
#endif
#ifdef EBADFD
    add("EBADFD"_s, EBADFD);
#endif
#ifdef EDOTDOT
    add("EDOTDOT"_s, EDOTDOT);
#endif
#ifdef EISCONN
    add("EISCONN"_s, EISCONN);
#else
#ifdef WSAEISCONN
    add("EISCONN"_s, WSAEISCONN);
#endif
#endif
#ifdef ENOANO
    add("ENOANO"_s, ENOANO);
#endif
#if defined(__wasi__) && !defined(ESHUTDOWN)
#define ESHUTDOWN EPIPE
#endif
#ifdef ESHUTDOWN
    add("ESHUTDOWN"_s, ESHUTDOWN);
#else
#ifdef WSAESHUTDOWN
    add("ESHUTDOWN"_s, WSAESHUTDOWN);
#endif
#endif
#ifdef ECHRNG
    add("ECHRNG"_s, ECHRNG);
#endif
#ifdef ELIBBAD
    add("ELIBBAD"_s, ELIBBAD);
#endif
#ifdef ENONET
    add("ENONET"_s, ENONET);
#endif
#ifdef EBADE
    add("EBADE"_s, EBADE);
#endif
#ifdef EBADF
    add("EBADF"_s, EBADF);
#else
#ifdef WSAEBADF
    add("EBADF"_s, WSAEBADF);
#endif
#endif
#ifdef EMULTIHOP
    add("EMULTIHOP"_s, EMULTIHOP);
#endif
#ifdef EIO
    add("EIO"_s, EIO);
#endif
#ifdef EUNATCH
    add("EUNATCH"_s, EUNATCH);
#endif
#ifdef EPROTOTYPE
    add("EPROTOTYPE"_s, EPROTOTYPE);
#else
#ifdef WSAEPROTOTYPE
    add("EPROTOTYPE"_s, WSAEPROTOTYPE);
#endif
#endif
#ifdef ENOSPC
    add("ENOSPC"_s, ENOSPC);
#endif
#ifdef ENOEXEC
    add("ENOEXEC"_s, ENOEXEC);
#endif
#ifdef EALREADY
    add("EALREADY"_s, EALREADY);
#else
#ifdef WSAEALREADY
    add("EALREADY"_s, WSAEALREADY);
#endif
#endif
#ifdef ENETDOWN
    add("ENETDOWN"_s, ENETDOWN);
#else
#ifdef WSAENETDOWN
    add("ENETDOWN"_s, WSAENETDOWN);
#endif
#endif
#ifdef ENOTNAM
    add("ENOTNAM"_s, ENOTNAM);
#endif
#ifdef EACCES
    add("EACCES"_s, EACCES);
#else
#ifdef WSAEACCES
    add("EACCES"_s, WSAEACCES);
#endif
#endif
#ifdef ELNRNG
    add("ELNRNG"_s, ELNRNG);
#endif
#ifdef EILSEQ
    add("EILSEQ"_s, EILSEQ);
#endif
#ifdef ENOTDIR
    add("ENOTDIR"_s, ENOTDIR);
#endif
#ifdef ENOTUNIQ
    add("ENOTUNIQ"_s, ENOTUNIQ);
#endif
#ifdef EPERM
    add("EPERM"_s, EPERM);
#endif
#ifdef EDOM
    add("EDOM"_s, EDOM);
#endif
#ifdef EXFULL
    add("EXFULL"_s, EXFULL);
#endif
#ifdef ECONNREFUSED
    add("ECONNREFUSED"_s, ECONNREFUSED);
#else
#ifdef WSAECONNREFUSED
    add("ECONNREFUSED"_s, WSAECONNREFUSED);
#endif
#endif
#ifdef EISDIR
    add("EISDIR"_s, EISDIR);
#endif
#ifdef EPROTONOSUPPORT
    add("EPROTONOSUPPORT"_s, EPROTONOSUPPORT);
#else
#ifdef WSAEPROTONOSUPPORT
    add("EPROTONOSUPPORT"_s, WSAEPROTONOSUPPORT);
#endif
#endif
#ifdef EROFS
    add("EROFS"_s, EROFS);
#endif
#ifdef EADDRNOTAVAIL
    add("EADDRNOTAVAIL"_s, EADDRNOTAVAIL);
#else
#ifdef WSAEADDRNOTAVAIL
    add("EADDRNOTAVAIL"_s, WSAEADDRNOTAVAIL);
#endif
#endif
#ifdef EIDRM
    add("EIDRM"_s, EIDRM);
#endif
#ifdef ECOMM
    add("ECOMM"_s, ECOMM);
#endif
#ifdef ESRMNT
    add("ESRMNT"_s, ESRMNT);
#endif
#ifdef EREMOTEIO
    add("EREMOTEIO"_s, EREMOTEIO);
#endif
#ifdef EL3RST
    add("EL3RST"_s, EL3RST);
#endif
#ifdef EBADMSG
    add("EBADMSG"_s, EBADMSG);
#endif
#ifdef ENFILE
    add("ENFILE"_s, ENFILE);
#endif
#ifdef ELIBMAX
    add("ELIBMAX"_s, ELIBMAX);
#endif
#ifdef ESPIPE
    add("ESPIPE"_s, ESPIPE);
#endif
#ifdef ENOLINK
    add("ENOLINK"_s, ENOLINK);
#endif
#ifdef ENETRESET
    add("ENETRESET"_s, ENETRESET);
#else
#ifdef WSAENETRESET
    add("ENETRESET"_s, WSAENETRESET);
#endif
#endif
#ifdef ETIMEDOUT
    add("ETIMEDOUT"_s, ETIMEDOUT);
#else
#ifdef WSAETIMEDOUT
    add("ETIMEDOUT"_s, WSAETIMEDOUT);
#endif
#endif
#ifdef ENOENT
    add("ENOENT"_s, ENOENT);
#endif
#ifdef EEXIST
    add("EEXIST"_s, EEXIST);
#endif
#ifdef EDQUOT
    add("EDQUOT"_s, EDQUOT);
#else
#ifdef WSAEDQUOT
    add("EDQUOT"_s, WSAEDQUOT);
#endif
#endif
#ifdef ENOSTR
    add("ENOSTR"_s, ENOSTR);
#endif
#ifdef EBADSLT
    add("EBADSLT"_s, EBADSLT);
#endif
#ifdef EBADRQC
    add("EBADRQC"_s, EBADRQC);
#endif
#ifdef ELIBACC
    add("ELIBACC"_s, ELIBACC);
#endif
#ifdef EFAULT
    add("EFAULT"_s, EFAULT);
#else
#ifdef WSAEFAULT
    add("EFAULT"_s, WSAEFAULT);
#endif
#endif
#ifdef EFBIG
    add("EFBIG"_s, EFBIG);
#endif
#ifdef EDEADLK
    add("EDEADLK"_s, EDEADLK);
#endif
#ifdef ENOTCONN
    add("ENOTCONN"_s, ENOTCONN);
#else
#ifdef WSAENOTCONN
    add("ENOTCONN"_s, WSAENOTCONN);
#endif
#endif
#ifdef EDESTADDRREQ
    add("EDESTADDRREQ"_s, EDESTADDRREQ);
#else
#ifdef WSAEDESTADDRREQ
    add("EDESTADDRREQ"_s, WSAEDESTADDRREQ);
#endif
#endif
#ifdef ELIBSCN
    add("ELIBSCN"_s, ELIBSCN);
#endif
#ifdef ENOLCK
    add("ENOLCK"_s, ENOLCK);
#endif
#ifdef EISNAM
    add("EISNAM"_s, EISNAM);
#endif
#ifdef ECONNABORTED
    add("ECONNABORTED"_s, ECONNABORTED);
#else
#ifdef WSAECONNABORTED
    add("ECONNABORTED"_s, WSAECONNABORTED);
#endif
#endif
#ifdef ENETUNREACH
    add("ENETUNREACH"_s, ENETUNREACH);
#else
#ifdef WSAENETUNREACH
    add("ENETUNREACH"_s, WSAENETUNREACH);
#endif
#endif
#ifdef ESTALE
    add("ESTALE"_s, ESTALE);
#else
#ifdef WSAESTALE
    add("ESTALE"_s, WSAESTALE);
#endif
#endif
#ifdef ENOSR
    add("ENOSR"_s, ENOSR);
#endif
#ifdef ENOMEM
    add("ENOMEM"_s, ENOMEM);
#endif
#ifdef ENOTSOCK
    add("ENOTSOCK"_s, ENOTSOCK);
#else
#ifdef WSAENOTSOCK
    add("ENOTSOCK"_s, WSAENOTSOCK);
#endif
#endif
#ifdef ESTRPIPE
    add("ESTRPIPE"_s, ESTRPIPE);
#endif
#ifdef EMLINK
    add("EMLINK"_s, EMLINK);
#endif
#ifdef ERANGE
    add("ERANGE"_s, ERANGE);
#endif
#ifdef ELIBEXEC
    add("ELIBEXEC"_s, ELIBEXEC);
#endif
#ifdef EL3HLT
    add("EL3HLT"_s, EL3HLT);
#endif
#ifdef ECONNRESET
    add("ECONNRESET"_s, ECONNRESET);
#else
#ifdef WSAECONNRESET
    add("ECONNRESET"_s, WSAECONNRESET);
#endif
#endif
#ifdef EADDRINUSE
    add("EADDRINUSE"_s, EADDRINUSE);
#else
#ifdef WSAEADDRINUSE
    add("EADDRINUSE"_s, WSAEADDRINUSE);
#endif
#endif
#ifdef EOPNOTSUPP
    add("EOPNOTSUPP"_s, EOPNOTSUPP);
#else
#ifdef WSAEOPNOTSUPP
    add("EOPNOTSUPP"_s, WSAEOPNOTSUPP);
#endif
#endif
#ifdef EREMCHG
    add("EREMCHG"_s, EREMCHG);
#endif
#ifdef EAGAIN
    add("EAGAIN"_s, EAGAIN);
#endif
#ifdef ENAMETOOLONG
    add("ENAMETOOLONG"_s, ENAMETOOLONG);
#else
#ifdef WSAENAMETOOLONG
    add("ENAMETOOLONG"_s, WSAENAMETOOLONG);
#endif
#endif
#ifdef ENOTTY
    add("ENOTTY"_s, ENOTTY);
#endif
#ifdef ERESTART
    add("ERESTART"_s, ERESTART);
#endif
#ifdef ESOCKTNOSUPPORT
    add("ESOCKTNOSUPPORT"_s, ESOCKTNOSUPPORT);
#else
#ifdef WSAESOCKTNOSUPPORT
    add("ESOCKTNOSUPPORT"_s, WSAESOCKTNOSUPPORT);
#endif
#endif
#ifdef ETIME
    add("ETIME"_s, ETIME);
#endif
#ifdef EBFONT
    add("EBFONT"_s, EBFONT);
#endif
#ifdef EDEADLOCK
    add("EDEADLOCK"_s, EDEADLOCK);
#endif
#ifdef ETOOMANYREFS
    add("ETOOMANYREFS"_s, ETOOMANYREFS);
#else
#ifdef WSAETOOMANYREFS
    add("ETOOMANYREFS"_s, WSAETOOMANYREFS);
#endif
#endif
#ifdef EMFILE
    add("EMFILE"_s, EMFILE);
#else
#ifdef WSAEMFILE
    add("EMFILE"_s, WSAEMFILE);
#endif
#endif
#ifdef ETXTBSY
    add("ETXTBSY"_s, ETXTBSY);
#endif
#ifdef EINPROGRESS
    add("EINPROGRESS"_s, EINPROGRESS);
#else
#ifdef WSAEINPROGRESS
    add("EINPROGRESS"_s, WSAEINPROGRESS);
#endif
#endif
#ifdef ENXIO
    add("ENXIO"_s, ENXIO);
#endif
#ifdef ENOPKG
    add("ENOPKG"_s, ENOPKG);
#endif
#ifdef WSASY
    add("WSASY"_s, WSASY);
#endif
#ifdef WSAEHOSTDOWN
    add("WSAEHOSTDOWN"_s, WSAEHOSTDOWN);
#endif
#ifdef WSAENETDOWN
    add("WSAENETDOWN"_s, WSAENETDOWN);
#endif
#ifdef WSAENOTSOCK
    add("WSAENOTSOCK"_s, WSAENOTSOCK);
#endif
#ifdef WSAEHOSTUNREACH
    add("WSAEHOSTUNREACH"_s, WSAEHOSTUNREACH);
#endif
#ifdef WSAELOOP
    add("WSAELOOP"_s, WSAELOOP);
#endif
#ifdef WSAEMFILE
    add("WSAEMFILE"_s, WSAEMFILE);
#endif
#ifdef WSAESTALE
    add("WSAESTALE"_s, WSAESTALE);
#endif
#ifdef WSAVERNOTSUPPORTED
    add("WSAVERNOTSUPPORTED"_s, WSAVERNOTSUPPORTED);
#endif
#ifdef WSAENETUNREACH
    add("WSAENETUNREACH"_s, WSAENETUNREACH);
#endif
#ifdef WSAEPROCLIM
    add("WSAEPROCLIM"_s, WSAEPROCLIM);
#endif
#ifdef WSAEFAULT
    add("WSAEFAULT"_s, WSAEFAULT);
#endif
#ifdef WSANOTINITIALISED
    add("WSANOTINITIALISED"_s, WSANOTINITIALISED);
#endif
#ifdef WSAEUSERS
    add("WSAEUSERS"_s, WSAEUSERS);
#endif
#ifdef WSAMAKEASYNCREPL
    add("WSAMAKEASYNCREPL"_s, WSAMAKEASYNCREPL);
#endif
#ifdef WSAENOPROTOOPT
    add("WSAENOPROTOOPT"_s, WSAENOPROTOOPT);
#endif
#ifdef WSAECONNABORTED
    add("WSAECONNABORTED"_s, WSAECONNABORTED);
#endif
#ifdef WSAENAMETOOLONG
    add("WSAENAMETOOLONG"_s, WSAENAMETOOLONG);
#endif
#ifdef WSAENOTEMPTY
    add("WSAENOTEMPTY"_s, WSAENOTEMPTY);
#endif
#ifdef WSAESHUTDOWN
    add("WSAESHUTDOWN"_s, WSAESHUTDOWN);
#endif
#ifdef WSAEAFNOSUPPORT
    add("WSAEAFNOSUPPORT"_s, WSAEAFNOSUPPORT);
#endif
#ifdef WSAETOOMANYREFS
    add("WSAETOOMANYREFS"_s, WSAETOOMANYREFS);
#endif
#ifdef WSAEACCES
    add("WSAEACCES"_s, WSAEACCES);
#endif
#ifdef WSATR
    add("WSATR"_s, WSATR);
#endif
#ifdef WSABASEERR
    add("WSABASEERR"_s, WSABASEERR);
#endif
#ifdef WSADESCRIPTIO
    add("WSADESCRIPTIO"_s, WSADESCRIPTIO);
#endif
#ifdef WSAEMSGSIZE
    add("WSAEMSGSIZE"_s, WSAEMSGSIZE);
#endif
#ifdef WSAEBADF
    add("WSAEBADF"_s, WSAEBADF);
#endif
#ifdef WSAECONNRESET
    add("WSAECONNRESET"_s, WSAECONNRESET);
#endif
#ifdef WSAGETSELECTERRO
    add("WSAGETSELECTERRO"_s, WSAGETSELECTERRO);
#endif
#ifdef WSAETIMEDOUT
    add("WSAETIMEDOUT"_s, WSAETIMEDOUT);
#endif
#ifdef WSAENOBUFS
    add("WSAENOBUFS"_s, WSAENOBUFS);
#endif
#ifdef WSAEDISCON
    add("WSAEDISCON"_s, WSAEDISCON);
#endif
#ifdef WSAEINTR
    add("WSAEINTR"_s, WSAEINTR);
#endif
#ifdef WSAEPROTOTYPE
    add("WSAEPROTOTYPE"_s, WSAEPROTOTYPE);
#endif
#ifdef WSAHOS
    add("WSAHOS"_s, WSAHOS);
#endif
#ifdef WSAEADDRINUSE
    add("WSAEADDRINUSE"_s, WSAEADDRINUSE);
#endif
#ifdef WSAEADDRNOTAVAIL
    add("WSAEADDRNOTAVAIL"_s, WSAEADDRNOTAVAIL);
#endif
#ifdef WSAEALREADY
    add("WSAEALREADY"_s, WSAEALREADY);
#endif
#ifdef WSAEPROTONOSUPPORT
    add("WSAEPROTONOSUPPORT"_s, WSAEPROTONOSUPPORT);
#endif
#ifdef WSASYSNOTREADY
    add("WSASYSNOTREADY"_s, WSASYSNOTREADY);
#endif
#ifdef WSAEWOULDBLOCK
    add("WSAEWOULDBLOCK"_s, WSAEWOULDBLOCK);
#endif
#ifdef WSAEPFNOSUPPORT
    add("WSAEPFNOSUPPORT"_s, WSAEPFNOSUPPORT);
#endif
#ifdef WSAEOPNOTSUPP
    add("WSAEOPNOTSUPP"_s, WSAEOPNOTSUPP);
#endif
#ifdef WSAEISCONN
    add("WSAEISCONN"_s, WSAEISCONN);
#endif
#ifdef WSAEDQUOT
    add("WSAEDQUOT"_s, WSAEDQUOT);
#endif
#ifdef WSAENOTCONN
    add("WSAENOTCONN"_s, WSAENOTCONN);
#endif
#ifdef WSAEREMOTE
    add("WSAEREMOTE"_s, WSAEREMOTE);
#endif
#ifdef WSAEINVAL
    add("WSAEINVAL"_s, WSAEINVAL);
#endif
#ifdef WSAEINPROGRESS
    add("WSAEINPROGRESS"_s, WSAEINPROGRESS);
#endif
#ifdef WSAGETSELECTEVEN
    add("WSAGETSELECTEVEN"_s, WSAGETSELECTEVEN);
#endif
#ifdef WSAESOCKTNOSUPPORT
    add("WSAESOCKTNOSUPPORT"_s, WSAESOCKTNOSUPPORT);
#endif
#ifdef WSAGETASYNCERRO
    add("WSAGETASYNCERRO"_s, WSAGETASYNCERRO);
#endif
#ifdef WSAMAKESELECTREPL
    add("WSAMAKESELECTREPL"_s, WSAMAKESELECTREPL);
#endif
#ifdef WSAGETASYNCBUFLE
    add("WSAGETASYNCBUFLE"_s, WSAGETASYNCBUFLE);
#endif
#ifdef WSAEDESTADDRREQ
    add("WSAEDESTADDRREQ"_s, WSAEDESTADDRREQ);
#endif
#ifdef WSAECONNREFUSED
    add("WSAECONNREFUSED"_s, WSAECONNREFUSED);
#endif
#ifdef WSAENETRESET
    add("WSAENETRESET"_s, WSAENETRESET);
#endif
#ifdef WSAN
    add("WSAN"_s, WSAN);
#endif
#ifdef ENOMEDIUM
    add("ENOMEDIUM"_s, ENOMEDIUM);
#endif
#ifdef EMEDIUMTYPE
    add("EMEDIUMTYPE"_s, EMEDIUMTYPE);
#endif
#ifdef ECANCELED
    add("ECANCELED"_s, ECANCELED);
#endif
#ifdef ENOKEY
    add("ENOKEY"_s, ENOKEY);
#endif
#ifdef EHWPOISON
    add("EHWPOISON"_s, EHWPOISON);
#endif
#ifdef EKEYEXPIRED
    add("EKEYEXPIRED"_s, EKEYEXPIRED);
#endif
#ifdef EKEYREVOKED
    add("EKEYREVOKED"_s, EKEYREVOKED);
#endif
#ifdef EKEYREJECTED
    add("EKEYREJECTED"_s, EKEYREJECTED);
#endif
#ifdef EOWNERDEAD
    add("EOWNERDEAD"_s, EOWNERDEAD);
#endif
#ifdef ENOTRECOVERABLE
    add("ENOTRECOVERABLE"_s, ENOTRECOVERABLE);
#endif
#ifdef ERFKILL
    add("ERFKILL"_s, ERFKILL);
#endif
#ifdef ECANCELED
    add("ECANCELED"_s, ECANCELED);
#endif
#ifdef ENOTSUP
    add("ENOTSUP"_s, ENOTSUP);
#endif
#ifdef EOWNERDEAD
    add("EOWNERDEAD"_s, EOWNERDEAD);
#endif
#ifdef ENOTRECOVERABLE
    add("ENOTRECOVERABLE"_s, ENOTRECOVERABLE);
#endif
#ifdef ELOCKUNMAPPED
    add("ELOCKUNMAPPED"_s, ELOCKUNMAPPED);
#endif
#ifdef ENOTACTIVE
    add("ENOTACTIVE"_s, ENOTACTIVE);
#endif
#ifdef EAUTH
    add("EAUTH"_s, EAUTH);
#endif
#ifdef EBADARCH
    add("EBADARCH"_s, EBADARCH);
#endif
#ifdef EBADEXEC
    add("EBADEXEC"_s, EBADEXEC);
#endif
#ifdef EBADMACHO
    add("EBADMACHO"_s, EBADMACHO);
#endif
#ifdef EBADRPC
    add("EBADRPC"_s, EBADRPC);
#endif
#ifdef EDEVERR
    add("EDEVERR"_s, EDEVERR);
#endif
#ifdef EFTYPE
    add("EFTYPE"_s, EFTYPE);
#endif
#ifdef ENEEDAUTH
    add("ENEEDAUTH"_s, ENEEDAUTH);
#endif
#ifdef ENOATTR
    add("ENOATTR"_s, ENOATTR);
#endif
#ifdef ENOPOLICY
    add("ENOPOLICY"_s, ENOPOLICY);
#endif
#ifdef EPROCLIM
    add("EPROCLIM"_s, EPROCLIM);
#endif
#ifdef EPROCUNAVAIL
    add("EPROCUNAVAIL"_s, EPROCUNAVAIL);
#endif
#ifdef EPROGMISMATCH
    add("EPROGMISMATCH"_s, EPROGMISMATCH);
#endif
#ifdef EPROGUNAVAIL
    add("EPROGUNAVAIL"_s, EPROGUNAVAIL);
#endif
#ifdef EPWROFF
    add("EPWROFF"_s, EPWROFF);
#endif
#ifdef ERPCMISMATCH
    add("ERPCMISMATCH"_s, ERPCMISMATCH);
#endif
#ifdef ESHLIBVERS
    add("ESHLIBVERS"_s, ESHLIBVERS);
#endif
#ifdef EQFULL
    add("EQFULL"_s, EQFULL);
#endif
#ifdef ENOTCAPABLE
    add("ENOTCAPABLE"_s, ENOTCAPABLE);
#endif
    return module;
}

} } // namespace JSC::Python
