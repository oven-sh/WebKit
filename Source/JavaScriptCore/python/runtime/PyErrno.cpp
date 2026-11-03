/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <cerrno>
#include <sys/stat.h>

namespace JSC {
namespace Python {

// The modules errno and _stat. (Modules/errnomodule.c and _stat.c in CPython.)

// ---- errno ----

namespace {

struct ErrnoEntry {
    ASCIILiteral name;
    int code;
};

}

// In CPython's order: where two names share a code, errorcode has the later one, at the place of the earlier.
static constexpr ErrnoEntry errnoTable[] = {
#if defined(ENODEV)
    { "ENODEV"_s, ENODEV },
#endif
#if defined(ENOCSI)
    { "ENOCSI"_s, ENOCSI },
#endif
#if defined(EHOSTUNREACH)
    { "EHOSTUNREACH"_s, EHOSTUNREACH },
#elif defined(WSAEHOSTUNREACH)
    { "EHOSTUNREACH"_s, WSAEHOSTUNREACH },
#endif
#if defined(ENOMSG)
    { "ENOMSG"_s, ENOMSG },
#endif
#if defined(EUCLEAN)
    { "EUCLEAN"_s, EUCLEAN },
#endif
#if defined(EL2NSYNC)
    { "EL2NSYNC"_s, EL2NSYNC },
#endif
#if defined(EL2HLT)
    { "EL2HLT"_s, EL2HLT },
#endif
#if defined(ENODATA)
    { "ENODATA"_s, ENODATA },
#endif
#if defined(ENOTBLK)
    { "ENOTBLK"_s, ENOTBLK },
#endif
#if defined(ENOSYS)
    { "ENOSYS"_s, ENOSYS },
#endif
#if defined(EPIPE)
    { "EPIPE"_s, EPIPE },
#endif
#if defined(EINVAL)
    { "EINVAL"_s, EINVAL },
#elif defined(WSAEINVAL)
    { "EINVAL"_s, WSAEINVAL },
#endif
#if defined(EOVERFLOW)
    { "EOVERFLOW"_s, EOVERFLOW },
#endif
#if defined(EADV)
    { "EADV"_s, EADV },
#endif
#if defined(EINTR)
    { "EINTR"_s, EINTR },
#elif defined(WSAEINTR)
    { "EINTR"_s, WSAEINTR },
#endif
#if defined(EUSERS)
    { "EUSERS"_s, EUSERS },
#elif defined(WSAEUSERS)
    { "EUSERS"_s, WSAEUSERS },
#endif
#if defined(ENOTEMPTY)
    { "ENOTEMPTY"_s, ENOTEMPTY },
#elif defined(WSAENOTEMPTY)
    { "ENOTEMPTY"_s, WSAENOTEMPTY },
#endif
#if defined(ENOBUFS)
    { "ENOBUFS"_s, ENOBUFS },
#elif defined(WSAENOBUFS)
    { "ENOBUFS"_s, WSAENOBUFS },
#endif
#if defined(EPROTO)
    { "EPROTO"_s, EPROTO },
#endif
#if defined(EREMOTE)
    { "EREMOTE"_s, EREMOTE },
#elif defined(WSAEREMOTE)
    { "EREMOTE"_s, WSAEREMOTE },
#endif
#if defined(ENAVAIL)
    { "ENAVAIL"_s, ENAVAIL },
#endif
#if defined(ECHILD)
    { "ECHILD"_s, ECHILD },
#endif
#if defined(ELOOP)
    { "ELOOP"_s, ELOOP },
#elif defined(WSAELOOP)
    { "ELOOP"_s, WSAELOOP },
#endif
#if defined(EXDEV)
    { "EXDEV"_s, EXDEV },
#endif
#if defined(E2BIG)
    { "E2BIG"_s, E2BIG },
#endif
#if defined(ESRCH)
    { "ESRCH"_s, ESRCH },
#endif
#if defined(EMSGSIZE)
    { "EMSGSIZE"_s, EMSGSIZE },
#elif defined(WSAEMSGSIZE)
    { "EMSGSIZE"_s, WSAEMSGSIZE },
#endif
#if defined(EAFNOSUPPORT)
    { "EAFNOSUPPORT"_s, EAFNOSUPPORT },
#elif defined(WSAEAFNOSUPPORT)
    { "EAFNOSUPPORT"_s, WSAEAFNOSUPPORT },
#endif
#if defined(EBADR)
    { "EBADR"_s, EBADR },
#endif
#if defined(EHOSTDOWN)
    { "EHOSTDOWN"_s, EHOSTDOWN },
#elif defined(WSAEHOSTDOWN)
    { "EHOSTDOWN"_s, WSAEHOSTDOWN },
#endif
#if defined(EPFNOSUPPORT)
    { "EPFNOSUPPORT"_s, EPFNOSUPPORT },
#elif defined(WSAEPFNOSUPPORT)
    { "EPFNOSUPPORT"_s, WSAEPFNOSUPPORT },
#endif
#if defined(ENOPROTOOPT)
    { "ENOPROTOOPT"_s, ENOPROTOOPT },
#elif defined(WSAENOPROTOOPT)
    { "ENOPROTOOPT"_s, WSAENOPROTOOPT },
#endif
#if defined(EBUSY)
    { "EBUSY"_s, EBUSY },
#endif
#if defined(EWOULDBLOCK)
    { "EWOULDBLOCK"_s, EWOULDBLOCK },
#elif defined(WSAEWOULDBLOCK)
    { "EWOULDBLOCK"_s, WSAEWOULDBLOCK },
#endif
#if defined(EBADFD)
    { "EBADFD"_s, EBADFD },
#endif
#if defined(EDOTDOT)
    { "EDOTDOT"_s, EDOTDOT },
#endif
#if defined(EISCONN)
    { "EISCONN"_s, EISCONN },
#elif defined(WSAEISCONN)
    { "EISCONN"_s, WSAEISCONN },
#endif
#if defined(ENOANO)
    { "ENOANO"_s, ENOANO },
#endif
#if defined(ESHUTDOWN)
    { "ESHUTDOWN"_s, ESHUTDOWN },
#elif defined(WSAESHUTDOWN)
    { "ESHUTDOWN"_s, WSAESHUTDOWN },
#endif
#if defined(ECHRNG)
    { "ECHRNG"_s, ECHRNG },
#endif
#if defined(ELIBBAD)
    { "ELIBBAD"_s, ELIBBAD },
#endif
#if defined(ENONET)
    { "ENONET"_s, ENONET },
#endif
#if defined(EBADE)
    { "EBADE"_s, EBADE },
#endif
#if defined(EBADF)
    { "EBADF"_s, EBADF },
#elif defined(WSAEBADF)
    { "EBADF"_s, WSAEBADF },
#endif
#if defined(EMULTIHOP)
    { "EMULTIHOP"_s, EMULTIHOP },
#endif
#if defined(EIO)
    { "EIO"_s, EIO },
#endif
#if defined(EUNATCH)
    { "EUNATCH"_s, EUNATCH },
#endif
#if defined(EPROTOTYPE)
    { "EPROTOTYPE"_s, EPROTOTYPE },
#elif defined(WSAEPROTOTYPE)
    { "EPROTOTYPE"_s, WSAEPROTOTYPE },
#endif
#if defined(ENOSPC)
    { "ENOSPC"_s, ENOSPC },
#endif
#if defined(ENOEXEC)
    { "ENOEXEC"_s, ENOEXEC },
#endif
#if defined(EALREADY)
    { "EALREADY"_s, EALREADY },
#elif defined(WSAEALREADY)
    { "EALREADY"_s, WSAEALREADY },
#endif
#if defined(ENETDOWN)
    { "ENETDOWN"_s, ENETDOWN },
#elif defined(WSAENETDOWN)
    { "ENETDOWN"_s, WSAENETDOWN },
#endif
#if defined(ENOTNAM)
    { "ENOTNAM"_s, ENOTNAM },
#endif
#if defined(EACCES)
    { "EACCES"_s, EACCES },
#elif defined(WSAEACCES)
    { "EACCES"_s, WSAEACCES },
#endif
#if defined(ELNRNG)
    { "ELNRNG"_s, ELNRNG },
#endif
#if defined(EILSEQ)
    { "EILSEQ"_s, EILSEQ },
#endif
#if defined(ENOTDIR)
    { "ENOTDIR"_s, ENOTDIR },
#endif
#if defined(ENOTUNIQ)
    { "ENOTUNIQ"_s, ENOTUNIQ },
#endif
#if defined(EPERM)
    { "EPERM"_s, EPERM },
#endif
#if defined(EDOM)
    { "EDOM"_s, EDOM },
#endif
#if defined(EXFULL)
    { "EXFULL"_s, EXFULL },
#endif
#if defined(ECONNREFUSED)
    { "ECONNREFUSED"_s, ECONNREFUSED },
#elif defined(WSAECONNREFUSED)
    { "ECONNREFUSED"_s, WSAECONNREFUSED },
#endif
#if defined(EISDIR)
    { "EISDIR"_s, EISDIR },
#endif
#if defined(EPROTONOSUPPORT)
    { "EPROTONOSUPPORT"_s, EPROTONOSUPPORT },
#elif defined(WSAEPROTONOSUPPORT)
    { "EPROTONOSUPPORT"_s, WSAEPROTONOSUPPORT },
#endif
#if defined(EROFS)
    { "EROFS"_s, EROFS },
#endif
#if defined(EADDRNOTAVAIL)
    { "EADDRNOTAVAIL"_s, EADDRNOTAVAIL },
#elif defined(WSAEADDRNOTAVAIL)
    { "EADDRNOTAVAIL"_s, WSAEADDRNOTAVAIL },
#endif
#if defined(EIDRM)
    { "EIDRM"_s, EIDRM },
#endif
#if defined(ECOMM)
    { "ECOMM"_s, ECOMM },
#endif
#if defined(ESRMNT)
    { "ESRMNT"_s, ESRMNT },
#endif
#if defined(EREMOTEIO)
    { "EREMOTEIO"_s, EREMOTEIO },
#endif
#if defined(EL3RST)
    { "EL3RST"_s, EL3RST },
#endif
#if defined(EBADMSG)
    { "EBADMSG"_s, EBADMSG },
#endif
#if defined(ENFILE)
    { "ENFILE"_s, ENFILE },
#endif
#if defined(ELIBMAX)
    { "ELIBMAX"_s, ELIBMAX },
#endif
#if defined(ESPIPE)
    { "ESPIPE"_s, ESPIPE },
#endif
#if defined(ENOLINK)
    { "ENOLINK"_s, ENOLINK },
#endif
#if defined(ENETRESET)
    { "ENETRESET"_s, ENETRESET },
#elif defined(WSAENETRESET)
    { "ENETRESET"_s, WSAENETRESET },
#endif
#if defined(ETIMEDOUT)
    { "ETIMEDOUT"_s, ETIMEDOUT },
#elif defined(WSAETIMEDOUT)
    { "ETIMEDOUT"_s, WSAETIMEDOUT },
#endif
#if defined(ENOENT)
    { "ENOENT"_s, ENOENT },
#endif
#if defined(EEXIST)
    { "EEXIST"_s, EEXIST },
#endif
#if defined(EDQUOT)
    { "EDQUOT"_s, EDQUOT },
#elif defined(WSAEDQUOT)
    { "EDQUOT"_s, WSAEDQUOT },
#endif
#if defined(ENOSTR)
    { "ENOSTR"_s, ENOSTR },
#endif
#if defined(EBADSLT)
    { "EBADSLT"_s, EBADSLT },
#endif
#if defined(EBADRQC)
    { "EBADRQC"_s, EBADRQC },
#endif
#if defined(ELIBACC)
    { "ELIBACC"_s, ELIBACC },
#endif
#if defined(EFAULT)
    { "EFAULT"_s, EFAULT },
#elif defined(WSAEFAULT)
    { "EFAULT"_s, WSAEFAULT },
#endif
#if defined(EFBIG)
    { "EFBIG"_s, EFBIG },
#endif
#if defined(EDEADLK)
    { "EDEADLK"_s, EDEADLK },
#endif
#if defined(ENOTCONN)
    { "ENOTCONN"_s, ENOTCONN },
#elif defined(WSAENOTCONN)
    { "ENOTCONN"_s, WSAENOTCONN },
#endif
#if defined(EDESTADDRREQ)
    { "EDESTADDRREQ"_s, EDESTADDRREQ },
#elif defined(WSAEDESTADDRREQ)
    { "EDESTADDRREQ"_s, WSAEDESTADDRREQ },
#endif
#if defined(ELIBSCN)
    { "ELIBSCN"_s, ELIBSCN },
#endif
#if defined(ENOLCK)
    { "ENOLCK"_s, ENOLCK },
#endif
#if defined(EISNAM)
    { "EISNAM"_s, EISNAM },
#endif
#if defined(ECONNABORTED)
    { "ECONNABORTED"_s, ECONNABORTED },
#elif defined(WSAECONNABORTED)
    { "ECONNABORTED"_s, WSAECONNABORTED },
#endif
#if defined(ENETUNREACH)
    { "ENETUNREACH"_s, ENETUNREACH },
#elif defined(WSAENETUNREACH)
    { "ENETUNREACH"_s, WSAENETUNREACH },
#endif
#if defined(ESTALE)
    { "ESTALE"_s, ESTALE },
#elif defined(WSAESTALE)
    { "ESTALE"_s, WSAESTALE },
#endif
#if defined(ENOSR)
    { "ENOSR"_s, ENOSR },
#endif
#if defined(ENOMEM)
    { "ENOMEM"_s, ENOMEM },
#endif
#if defined(ENOTSOCK)
    { "ENOTSOCK"_s, ENOTSOCK },
#elif defined(WSAENOTSOCK)
    { "ENOTSOCK"_s, WSAENOTSOCK },
#endif
#if defined(ESTRPIPE)
    { "ESTRPIPE"_s, ESTRPIPE },
#endif
#if defined(EMLINK)
    { "EMLINK"_s, EMLINK },
#endif
#if defined(ERANGE)
    { "ERANGE"_s, ERANGE },
#endif
#if defined(ELIBEXEC)
    { "ELIBEXEC"_s, ELIBEXEC },
#endif
#if defined(EL3HLT)
    { "EL3HLT"_s, EL3HLT },
#endif
#if defined(ECONNRESET)
    { "ECONNRESET"_s, ECONNRESET },
#elif defined(WSAECONNRESET)
    { "ECONNRESET"_s, WSAECONNRESET },
#endif
#if defined(EADDRINUSE)
    { "EADDRINUSE"_s, EADDRINUSE },
#elif defined(WSAEADDRINUSE)
    { "EADDRINUSE"_s, WSAEADDRINUSE },
#endif
#if defined(EOPNOTSUPP)
    { "EOPNOTSUPP"_s, EOPNOTSUPP },
#elif defined(WSAEOPNOTSUPP)
    { "EOPNOTSUPP"_s, WSAEOPNOTSUPP },
#endif
#if defined(EREMCHG)
    { "EREMCHG"_s, EREMCHG },
#endif
#if defined(EAGAIN)
    { "EAGAIN"_s, EAGAIN },
#endif
#if defined(ENAMETOOLONG)
    { "ENAMETOOLONG"_s, ENAMETOOLONG },
#elif defined(WSAENAMETOOLONG)
    { "ENAMETOOLONG"_s, WSAENAMETOOLONG },
#endif
#if defined(ENOTTY)
    { "ENOTTY"_s, ENOTTY },
#endif
#if defined(ERESTART)
    { "ERESTART"_s, ERESTART },
#endif
#if defined(ESOCKTNOSUPPORT)
    { "ESOCKTNOSUPPORT"_s, ESOCKTNOSUPPORT },
#elif defined(WSAESOCKTNOSUPPORT)
    { "ESOCKTNOSUPPORT"_s, WSAESOCKTNOSUPPORT },
#endif
#if defined(ETIME)
    { "ETIME"_s, ETIME },
#endif
#if defined(EBFONT)
    { "EBFONT"_s, EBFONT },
#endif
#if defined(EDEADLOCK)
    { "EDEADLOCK"_s, EDEADLOCK },
#endif
#if defined(ETOOMANYREFS)
    { "ETOOMANYREFS"_s, ETOOMANYREFS },
#elif defined(WSAETOOMANYREFS)
    { "ETOOMANYREFS"_s, WSAETOOMANYREFS },
#endif
#if defined(EMFILE)
    { "EMFILE"_s, EMFILE },
#elif defined(WSAEMFILE)
    { "EMFILE"_s, WSAEMFILE },
#endif
#if defined(ETXTBSY)
    { "ETXTBSY"_s, ETXTBSY },
#endif
#if defined(EINPROGRESS)
    { "EINPROGRESS"_s, EINPROGRESS },
#elif defined(WSAEINPROGRESS)
    { "EINPROGRESS"_s, WSAEINPROGRESS },
#endif
#if defined(ENXIO)
    { "ENXIO"_s, ENXIO },
#endif
#if defined(ENOPKG)
    { "ENOPKG"_s, ENOPKG },
#endif
#if defined(WSASY)
    { "WSASY"_s, WSASY },
#endif
#if defined(WSAEHOSTDOWN)
    { "WSAEHOSTDOWN"_s, WSAEHOSTDOWN },
#endif
#if defined(WSAENETDOWN)
    { "WSAENETDOWN"_s, WSAENETDOWN },
#endif
#if defined(WSAENOTSOCK)
    { "WSAENOTSOCK"_s, WSAENOTSOCK },
#endif
#if defined(WSAEHOSTUNREACH)
    { "WSAEHOSTUNREACH"_s, WSAEHOSTUNREACH },
#endif
#if defined(WSAELOOP)
    { "WSAELOOP"_s, WSAELOOP },
#endif
#if defined(WSAEMFILE)
    { "WSAEMFILE"_s, WSAEMFILE },
#endif
#if defined(WSAESTALE)
    { "WSAESTALE"_s, WSAESTALE },
#endif
#if defined(WSAVERNOTSUPPORTED)
    { "WSAVERNOTSUPPORTED"_s, WSAVERNOTSUPPORTED },
#endif
#if defined(WSAENETUNREACH)
    { "WSAENETUNREACH"_s, WSAENETUNREACH },
#endif
#if defined(WSAEPROCLIM)
    { "WSAEPROCLIM"_s, WSAEPROCLIM },
#endif
#if defined(WSAEFAULT)
    { "WSAEFAULT"_s, WSAEFAULT },
#endif
#if defined(WSANOTINITIALISED)
    { "WSANOTINITIALISED"_s, WSANOTINITIALISED },
#endif
#if defined(WSAEUSERS)
    { "WSAEUSERS"_s, WSAEUSERS },
#endif
#if defined(WSAMAKEASYNCREPL)
    { "WSAMAKEASYNCREPL"_s, WSAMAKEASYNCREPL },
#endif
#if defined(WSAENOPROTOOPT)
    { "WSAENOPROTOOPT"_s, WSAENOPROTOOPT },
#endif
#if defined(WSAECONNABORTED)
    { "WSAECONNABORTED"_s, WSAECONNABORTED },
#endif
#if defined(WSAENAMETOOLONG)
    { "WSAENAMETOOLONG"_s, WSAENAMETOOLONG },
#endif
#if defined(WSAENOTEMPTY)
    { "WSAENOTEMPTY"_s, WSAENOTEMPTY },
#endif
#if defined(WSAESHUTDOWN)
    { "WSAESHUTDOWN"_s, WSAESHUTDOWN },
#endif
#if defined(WSAEAFNOSUPPORT)
    { "WSAEAFNOSUPPORT"_s, WSAEAFNOSUPPORT },
#endif
#if defined(WSAETOOMANYREFS)
    { "WSAETOOMANYREFS"_s, WSAETOOMANYREFS },
#endif
#if defined(WSAEACCES)
    { "WSAEACCES"_s, WSAEACCES },
#endif
#if defined(WSATR)
    { "WSATR"_s, WSATR },
#endif
#if defined(WSABASEERR)
    { "WSABASEERR"_s, WSABASEERR },
#endif
#if defined(WSADESCRIPTIO)
    { "WSADESCRIPTIO"_s, WSADESCRIPTIO },
#endif
#if defined(WSAEMSGSIZE)
    { "WSAEMSGSIZE"_s, WSAEMSGSIZE },
#endif
#if defined(WSAEBADF)
    { "WSAEBADF"_s, WSAEBADF },
#endif
#if defined(WSAECONNRESET)
    { "WSAECONNRESET"_s, WSAECONNRESET },
#endif
#if defined(WSAGETSELECTERRO)
    { "WSAGETSELECTERRO"_s, WSAGETSELECTERRO },
#endif
#if defined(WSAETIMEDOUT)
    { "WSAETIMEDOUT"_s, WSAETIMEDOUT },
#endif
#if defined(WSAENOBUFS)
    { "WSAENOBUFS"_s, WSAENOBUFS },
#endif
#if defined(WSAEDISCON)
    { "WSAEDISCON"_s, WSAEDISCON },
#endif
#if defined(WSAEINTR)
    { "WSAEINTR"_s, WSAEINTR },
#endif
#if defined(WSAEPROTOTYPE)
    { "WSAEPROTOTYPE"_s, WSAEPROTOTYPE },
#endif
#if defined(WSAHOS)
    { "WSAHOS"_s, WSAHOS },
#endif
#if defined(WSAEADDRINUSE)
    { "WSAEADDRINUSE"_s, WSAEADDRINUSE },
#endif
#if defined(WSAEADDRNOTAVAIL)
    { "WSAEADDRNOTAVAIL"_s, WSAEADDRNOTAVAIL },
#endif
#if defined(WSAEALREADY)
    { "WSAEALREADY"_s, WSAEALREADY },
#endif
#if defined(WSAEPROTONOSUPPORT)
    { "WSAEPROTONOSUPPORT"_s, WSAEPROTONOSUPPORT },
#endif
#if defined(WSASYSNOTREADY)
    { "WSASYSNOTREADY"_s, WSASYSNOTREADY },
#endif
#if defined(WSAEWOULDBLOCK)
    { "WSAEWOULDBLOCK"_s, WSAEWOULDBLOCK },
#endif
#if defined(WSAEPFNOSUPPORT)
    { "WSAEPFNOSUPPORT"_s, WSAEPFNOSUPPORT },
#endif
#if defined(WSAEOPNOTSUPP)
    { "WSAEOPNOTSUPP"_s, WSAEOPNOTSUPP },
#endif
#if defined(WSAEISCONN)
    { "WSAEISCONN"_s, WSAEISCONN },
#endif
#if defined(WSAEDQUOT)
    { "WSAEDQUOT"_s, WSAEDQUOT },
#endif
#if defined(WSAENOTCONN)
    { "WSAENOTCONN"_s, WSAENOTCONN },
#endif
#if defined(WSAEREMOTE)
    { "WSAEREMOTE"_s, WSAEREMOTE },
#endif
#if defined(WSAEINVAL)
    { "WSAEINVAL"_s, WSAEINVAL },
#endif
#if defined(WSAEINPROGRESS)
    { "WSAEINPROGRESS"_s, WSAEINPROGRESS },
#endif
#if defined(WSAGETSELECTEVEN)
    { "WSAGETSELECTEVEN"_s, WSAGETSELECTEVEN },
#endif
#if defined(WSAESOCKTNOSUPPORT)
    { "WSAESOCKTNOSUPPORT"_s, WSAESOCKTNOSUPPORT },
#endif
#if defined(WSAGETASYNCERRO)
    { "WSAGETASYNCERRO"_s, WSAGETASYNCERRO },
#endif
#if defined(WSAMAKESELECTREPL)
    { "WSAMAKESELECTREPL"_s, WSAMAKESELECTREPL },
#endif
#if defined(WSAGETASYNCBUFLE)
    { "WSAGETASYNCBUFLE"_s, WSAGETASYNCBUFLE },
#endif
#if defined(WSAEDESTADDRREQ)
    { "WSAEDESTADDRREQ"_s, WSAEDESTADDRREQ },
#endif
#if defined(WSAECONNREFUSED)
    { "WSAECONNREFUSED"_s, WSAECONNREFUSED },
#endif
#if defined(WSAENETRESET)
    { "WSAENETRESET"_s, WSAENETRESET },
#endif
#if defined(WSAN)
    { "WSAN"_s, WSAN },
#endif
#if defined(ENOMEDIUM)
    { "ENOMEDIUM"_s, ENOMEDIUM },
#endif
#if defined(EMEDIUMTYPE)
    { "EMEDIUMTYPE"_s, EMEDIUMTYPE },
#endif
#if defined(ECANCELED)
    { "ECANCELED"_s, ECANCELED },
#endif
#if defined(ENOKEY)
    { "ENOKEY"_s, ENOKEY },
#endif
#if defined(EHWPOISON)
    { "EHWPOISON"_s, EHWPOISON },
#endif
#if defined(EKEYEXPIRED)
    { "EKEYEXPIRED"_s, EKEYEXPIRED },
#endif
#if defined(EKEYREVOKED)
    { "EKEYREVOKED"_s, EKEYREVOKED },
#endif
#if defined(EKEYREJECTED)
    { "EKEYREJECTED"_s, EKEYREJECTED },
#endif
#if defined(EOWNERDEAD)
    { "EOWNERDEAD"_s, EOWNERDEAD },
#endif
#if defined(ENOTRECOVERABLE)
    { "ENOTRECOVERABLE"_s, ENOTRECOVERABLE },
#endif
#if defined(ERFKILL)
    { "ERFKILL"_s, ERFKILL },
#endif
#if defined(ENOTSUP)
    { "ENOTSUP"_s, ENOTSUP },
#endif
#if defined(ELOCKUNMAPPED)
    { "ELOCKUNMAPPED"_s, ELOCKUNMAPPED },
#endif
#if defined(ENOTACTIVE)
    { "ENOTACTIVE"_s, ENOTACTIVE },
#endif
#if defined(EAUTH)
    { "EAUTH"_s, EAUTH },
#endif
#if defined(EBADARCH)
    { "EBADARCH"_s, EBADARCH },
#endif
#if defined(EBADEXEC)
    { "EBADEXEC"_s, EBADEXEC },
#endif
#if defined(EBADMACHO)
    { "EBADMACHO"_s, EBADMACHO },
#endif
#if defined(EBADRPC)
    { "EBADRPC"_s, EBADRPC },
#endif
#if defined(EDEVERR)
    { "EDEVERR"_s, EDEVERR },
#endif
#if defined(EFTYPE)
    { "EFTYPE"_s, EFTYPE },
#endif
#if defined(ENEEDAUTH)
    { "ENEEDAUTH"_s, ENEEDAUTH },
#endif
#if defined(ENOATTR)
    { "ENOATTR"_s, ENOATTR },
#endif
#if defined(ENOPOLICY)
    { "ENOPOLICY"_s, ENOPOLICY },
#endif
#if defined(EPROCLIM)
    { "EPROCLIM"_s, EPROCLIM },
#endif
#if defined(EPROCUNAVAIL)
    { "EPROCUNAVAIL"_s, EPROCUNAVAIL },
#endif
#if defined(EPROGMISMATCH)
    { "EPROGMISMATCH"_s, EPROGMISMATCH },
#endif
#if defined(EPROGUNAVAIL)
    { "EPROGUNAVAIL"_s, EPROGUNAVAIL },
#endif
#if defined(EPWROFF)
    { "EPWROFF"_s, EPWROFF },
#endif
#if defined(ERPCMISMATCH)
    { "ERPCMISMATCH"_s, ERPCMISMATCH },
#endif
#if defined(ESHLIBVERS)
    { "ESHLIBVERS"_s, ESHLIBVERS },
#endif
#if defined(EQFULL)
    { "EQFULL"_s, EQFULL },
#endif
#if defined(ENOTCAPABLE)
    { "ENOTCAPABLE"_s, ENOTCAPABLE },
#endif
};

void initializeErrnoModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "This module makes available standard errno system symbols.\n\n"
        "The value of each symbol is the corresponding integer value,\n"
        "e.g., on most systems, errno.ENOENT equals the integer 2.\n\n"
        "The dictionary errno.errorcode maps numeric codes to symbol names,\n"
        "e.g., errno.errorcode[2] could be the string 'ENOENT'.\n\n"
        "Symbols that are not relevant to the underlying system are not defined.\n\n"
        "To map error codes to error messages, use the function os.strerror(),\n"
        "e.g. os.strerror(2) could return 'No such file or directory'."_s));
    PyDict* errorCode = PyDict::create(globalObject);
    module->add(globalObject, "errorcode"_s, errorCode);
    for (auto& entry : errnoTable) {
        JSString* name = internString(vm, entry.name);
        module->dict()->set(vm, name, jsNumber(entry.code));
        errorCode->set(globalObject, jsNumber(entry.code), name);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// ---- _stat ----

#if OS(WINDOWS)
using StatMode = unsigned short;
#else
using StatMode = mode_t;
#endif

// The bits are those of every Unix, and of Lib/stat.py.
static constexpr StatMode statTypeMask = 0170000;
static constexpr StatMode statDirectory = 0040000;
static constexpr StatMode statCharacterDevice = 0020000;
static constexpr StatMode statBlockDevice = 0060000;
static constexpr StatMode statRegular = 0100000;
static constexpr StatMode statFifo = 0010000;
static constexpr StatMode statLink = 0120000;
static constexpr StatMode statSocket = 0140000;
// 0: the platform has no such files.
#ifdef S_IFDOOR
static constexpr StatMode statDoor = S_IFDOOR;
#else
static constexpr StatMode statDoor = 0;
#endif
#ifdef S_IFPORT
static constexpr StatMode statPort = S_IFPORT;
#else
static constexpr StatMode statPort = 0;
#endif
#ifdef S_IFWHT
static constexpr StatMode statWhiteout = S_IFWHT;
#else
static constexpr StatMode statWhiteout = 0;
#endif

// False: an exception.
static bool statGetMode(JSGlobalObject* globalObject, JSValue value, StatMode& mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseOverflowError(globalObject, scope, "can't convert negative value to unsigned int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned long"_s);
        return false;
    }
    mode = static_cast<StatMode>(*converted);
    if (mode != *converted) {
        raiseOverflowError(globalObject, scope, "mode out of range"_s);
        return false;
    }
    return true;
}

template<StatMode type>
static JSValue statIsType(JSGlobalObject* globalObject, JSValue value)
{
    StatMode mode;
    if (!statGetMode(globalObject, value, mode))
        return { };
    return jsBoolean(type && (mode & statTypeMask) == type);
}

template<StatMode mask>
static JSValue statMasked(JSGlobalObject* globalObject, JSValue value)
{
    StatMode mode;
    if (!statGetMode(globalObject, value, mode))
        return { };
    return jsNumber(static_cast<uint32_t>(mode & mask));
}

static JSValue statFileMode(JSGlobalObject* globalObject, JSValue value)
{
    StatMode mode;
    if (!statGetMode(globalObject, value, mode))
        return { };

    static constexpr struct {
        StatMode type;
        Latin1Character letter;
    } types[] = {
        { statRegular, '-' }, { statDirectory, 'd' }, { statLink, 'l' }, { statBlockDevice, 'b' }, { statCharacterDevice, 'c' },
        { statFifo, 'p' }, { statSocket, 's' }, { statDoor, 'D' }, { statPort, 'P' }, { statWhiteout, 'w' },
    };
    std::array<Latin1Character, 10> text;
    text[0] = '?';
    for (auto& entry : types) {
        if (entry.type && (mode & statTypeMask) == entry.type) {
            text[0] = entry.letter;
            break;
        }
    }
    // For the user, the group and the others; in place of x, each shows one of the set-uid, set-gid and sticky bits too.
    for (unsigned i = 0; i < 3; ++i) {
        unsigned bits = mode >> (6 - 3 * i);
        bool isSpecial = mode & (04000 >> i);
        text[1 + 3 * i] = bits & 4 ? 'r' : '-';
        text[2 + 3 * i] = bits & 2 ? 'w' : '-';
        if (isSpecial)
            text[3 + 3 * i] = i == 2 ? (bits & 1 ? 't' : 'T') : (bits & 1 ? 's' : 'S');
        else
            text[3 + 3 * i] = bits & 1 ? 'x' : '-';
    }
    return makeStr(globalObject, String(std::span<const Latin1Character> { text }));
}

static constexpr NativeDef statFunctions[] = {
    PY_FUNCTION("S_ISDIR", statIsType<statDirectory>, "(object, /)"),
    PY_FUNCTION("S_ISCHR", statIsType<statCharacterDevice>, "(object, /)"),
    PY_FUNCTION("S_ISBLK", statIsType<statBlockDevice>, "(object, /)"),
    PY_FUNCTION("S_ISREG", statIsType<statRegular>, "(object, /)"),
    PY_FUNCTION("S_ISFIFO", statIsType<statFifo>, "(object, /)"),
    PY_FUNCTION("S_ISLNK", statIsType<statLink>, "(object, /)"),
    PY_FUNCTION("S_ISSOCK", statIsType<statSocket>, "(object, /)"),
    PY_FUNCTION("S_ISDOOR", statIsType<statDoor>, "(object, /)"),
    PY_FUNCTION("S_ISPORT", statIsType<statPort>, "(object, /)"),
    PY_FUNCTION("S_ISWHT", statIsType<statWhiteout>, "(object, /)"),
    PY_FUNCTION("S_IMODE", statMasked<07777>, "(object, /)"),
    PY_FUNCTION("S_IFMT", statMasked<statTypeMask>, "(object, /)"),
    PY_FUNCTION("filemode", statFileMode, "(object, /)"),
};

namespace {

struct StatConstant {
    ASCIILiteral name;
    uint32_t value;
};

}

static constexpr StatConstant statConstants[] = {
    { "S_IFDIR"_s, statDirectory },
    { "S_IFCHR"_s, statCharacterDevice },
    { "S_IFBLK"_s, statBlockDevice },
    { "S_IFREG"_s, statRegular },
    { "S_IFIFO"_s, statFifo },
    { "S_IFLNK"_s, statLink },
    { "S_IFSOCK"_s, statSocket },
    { "S_IFDOOR"_s, statDoor },
    { "S_IFPORT"_s, statPort },
    { "S_IFWHT"_s, statWhiteout },

    { "S_ISUID"_s, 04000 },
    { "S_ISGID"_s, 02000 },
    { "S_ISVTX"_s, 01000 },
    { "S_ENFMT"_s, 02000 },

    { "S_IREAD"_s, 00400 },
    { "S_IWRITE"_s, 00200 },
    { "S_IEXEC"_s, 00100 },
    { "S_IRWXU"_s, 00700 },
    { "S_IRUSR"_s, 00400 },
    { "S_IWUSR"_s, 00200 },
    { "S_IXUSR"_s, 00100 },
    { "S_IRWXG"_s, 00070 },
    { "S_IRGRP"_s, 00040 },
    { "S_IWGRP"_s, 00020 },
    { "S_IXGRP"_s, 00010 },
    { "S_IRWXO"_s, 00007 },
    { "S_IROTH"_s, 00004 },
    { "S_IWOTH"_s, 00002 },
    { "S_IXOTH"_s, 00001 },

    // The flags of chflags().
    { "UF_SETTABLE"_s, 0x0000ffff },
    { "UF_NODUMP"_s, 0x00000001 },
    { "UF_IMMUTABLE"_s, 0x00000002 },
    { "UF_APPEND"_s, 0x00000004 },
    { "UF_OPAQUE"_s, 0x00000008 },
    { "UF_NOUNLINK"_s, 0x00000010 },
    { "UF_COMPRESSED"_s, 0x00000020 },
    { "UF_TRACKED"_s, 0x00000040 },
    { "UF_DATAVAULT"_s, 0x00000080 },
    { "UF_HIDDEN"_s, 0x00008000 },
#if OS(DARWIN)
    { "SF_SETTABLE"_s, 0x3fff0000 },
#else
    { "SF_SETTABLE"_s, 0xffff0000 },
#endif
    { "SF_ARCHIVED"_s, 0x00010000 },
    { "SF_IMMUTABLE"_s, 0x00020000 },
    { "SF_APPEND"_s, 0x00040000 },
    { "SF_NOUNLINK"_s, 0x00100000 },
    { "SF_SNAPSHOT"_s, 0x00200000 },
    { "SF_FIRMLINK"_s, 0x00800000 },
    { "SF_DATALESS"_s, 0x40000000 },
#if OS(DARWIN)
    { "SF_SUPPORTED"_s, 0x009f0000 },
    { "SF_SYNTHETIC"_s, 0xc0000000 },
#endif

    // The indices of the items of os.stat_result.
    { "ST_MODE"_s, 0 },
    { "ST_INO"_s, 1 },
    { "ST_DEV"_s, 2 },
    { "ST_NLINK"_s, 3 },
    { "ST_UID"_s, 4 },
    { "ST_GID"_s, 5 },
    { "ST_SIZE"_s, 6 },
    { "ST_ATIME"_s, 7 },
    { "ST_MTIME"_s, 8 },
    { "ST_CTIME"_s, 9 },
};

void initializeStatModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, statFunctions);
    for (auto& constant : statConstants)
        module->add(globalObject, constant.name, jsNumber(constant.value));
}

} // namespace Python
} // namespace JSC
