/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <sys/ioctl.h>
#include <termios.h>
#if __has_include(<sys/ttydefaults.h>)
#include <sys/ttydefaults.h>
#endif
#if OS(LINUX)
// For the constants of the serial lines.
#include <linux/tty.h>
#include <asm/ioctls.h>
#endif

// One name for the other, where there is only one.
#if defined(VSWTCH) && !defined(VSWTC)
#define VSWTC VSWTCH
#endif
#if defined(VSWTC) && !defined(VSWTCH)
#define VSWTCH VSWTC
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module termios. (Modules/termios.c in CPython is the reference.)

static constexpr TypeDef termiosErrorTypeDef { "termios.error"_s, { TypeFlag::Base }, { }, { }, { } };

static PyType* termiosErrorType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(termiosErrorTypeDef, rt.Exception());
}

// termios.error(errno, message)
static JSValue termiosError(JSGlobalObject* globalObject)
{
    int error = errno;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, termiosErrorType(globalObject), { jsNumber(error), posixDecode(globalObject, strerror(error)) });
}

static JSValue termiosGetAttr(JSGlobalObject* globalObject, JSValue fdValue)
{
    int fd;
    if (!posixToFileDescriptor(globalObject, fdValue, fd))
        return { };
    struct termios mode { };
    if (tcgetattr(fd, &mode) < 0)
        return termiosError(globalObject);
    JSArray* characters = newList(globalObject);
    bool isCanonical = mode.c_lflag & ICANON;
    for (unsigned i = 0; i < NCCS; ++i) {
        // Unless lines are being edited, these two are numbers. (If they are, the slots may be those of EOF and EOL.)
        if (!isCanonical && (i == VMIN || i == VTIME))
            listAppend(globalObject, characters, jsNumber(mode.c_cc[i]));
        else
            listAppend(globalObject, characters, PyBytes::create(globalObject, std::span<const uint8_t> { &mode.c_cc[i], 1 }));
    }
    JSArray* result = newList(globalObject);
    for (uint64_t number : { static_cast<uint64_t>(mode.c_iflag), static_cast<uint64_t>(mode.c_oflag), static_cast<uint64_t>(mode.c_cflag), static_cast<uint64_t>(mode.c_lflag),
             static_cast<uint64_t>(cfgetispeed(&mode)), static_cast<uint64_t>(cfgetospeed(&mode)) })
        listAppend(globalObject, result, posixMakeInt(globalObject, number));
    listAppend(globalObject, result, characters);
    return result;
}

static JSValue termiosSetAttr(JSGlobalObject* globalObject, JSValue fdValue, JSValue whenValue, JSValue attributes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int when;
    if (!posixToFileDescriptor(globalObject, fdValue, fd) || !posixToInt(globalObject, whenValue, when))
        return { };
    JSArray* list = tryAsList(attributes);
    if (!list || listLength(list) != 7)
        return raiseTypeError(globalObject, scope, "tcsetattr, arg 3: must be 7 element list"_s);
    // What there is no item for stays as it is.
    struct termios mode;
    if (tcgetattr(fd, &mode) < 0)
        return termiosError(globalObject);
    int64_t numbers[6];
    for (unsigned i = 0; i < 6; ++i) {
        // An __index__() may have shortened the list.
        if (listLength(list) != 7)
            return raiseTypeError(globalObject, scope, "tcsetattr, arg 3: must be 7 element list"_s);
        if (!posixToLong(globalObject, listGet(list, i), numbers[i]))
            return { };
    }
    mode.c_iflag = static_cast<tcflag_t>(numbers[0]);
    mode.c_oflag = static_cast<tcflag_t>(numbers[1]);
    mode.c_cflag = static_cast<tcflag_t>(numbers[2]);
    mode.c_lflag = static_cast<tcflag_t>(numbers[3]);
    JSArray* characters = listLength(list) == 7 ? tryAsList(listGet(list, 6)) : nullptr;
    if (!characters || listLength(characters) != NCCS)
        return raiseTypeError(globalObject, scope, "tcsetattr: attributes[6] must be "_s, static_cast<unsigned>(NCCS), " element list"_s);
    for (unsigned i = 0; i < NCCS; ++i) {
        JSValue item = listGet(characters, i);
        auto* bytes = PyBytes::tryFrom(item);
        if (bytes && bytes->length() == 1)
            mode.c_cc[i] = bytes->data()[0];
        else if (isInt(item)) {
            int64_t number;
            if (!posixToLong(globalObject, item, number))
                return { };
            mode.c_cc[i] = static_cast<cc_t>(number);
        } else
            return raiseTypeError(globalObject, scope, "tcsetattr: elements of attributes must be bytes objects of length 1 or integers"_s);
    }
    if (cfsetispeed(&mode, static_cast<speed_t>(numbers[4])) < 0 || cfsetospeed(&mode, static_cast<speed_t>(numbers[5])) < 0 || tcsetattr(fd, when, &mode) < 0)
        return termiosError(globalObject);
    return jsUndefined();
}

template<int (*function)(int, int)>
static JSValue termiosControl(JSGlobalObject* globalObject, JSValue fdValue, JSValue argumentValue)
{
    int fd;
    int argument;
    if (!posixToFileDescriptor(globalObject, fdValue, fd) || !posixToInt(globalObject, argumentValue, argument))
        return { };
    if (function(fd, argument) < 0)
        return termiosError(globalObject);
    return jsUndefined();
}

static JSValue termiosDrain(JSGlobalObject* globalObject, JSValue fdValue)
{
    int fd;
    if (!posixToFileDescriptor(globalObject, fdValue, fd))
        return { };
    int result;
    {
        AllowThreads allow(globalObject);
        result = tcdrain(fd);
    }
    if (result < 0)
        return termiosError(globalObject);
    return jsUndefined();
}

static JSValue termiosGetWinSize(JSGlobalObject* globalObject, JSValue fdValue)
{
    int fd;
    if (!posixToFileDescriptor(globalObject, fdValue, fd))
        return { };
    struct winsize size;
    if (ioctl(fd, TIOCGWINSZ, &size) < 0)
        return termiosError(globalObject);
    return PyTuple::create(globalObject, { jsNumber(size.ws_row), jsNumber(size.ws_col) });
}

static JSValue termiosSetWinSize(JSGlobalObject* globalObject, JSValue fdValue, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (!posixToFileDescriptor(globalObject, fdValue, fd))
        return { };
    int64_t count = posixIsSequence(globalObject, sizeValue) ? length(globalObject, sizeValue) : -1;
    if (count != 2) {
        takePendingException(globalObject);
        return raiseTypeError(globalObject, scope, "tcsetwinsize, arg 2: must be a two-item sequence"_s);
    }
    int64_t numbers[2];
    for (unsigned i = 0; i < 2; ++i) {
        JSValue item = getItem(globalObject, sizeValue, jsNumber(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!posixToLong(globalObject, item, numbers[i]))
            return { };
    }
    // The size in pixels stays as it is.
    struct winsize size;
    if (ioctl(fd, TIOCGWINSZ, &size) < 0)
        return termiosError(globalObject);
    size.ws_row = static_cast<unsigned short>(numbers[0]);
    size.ws_col = static_cast<unsigned short>(numbers[1]);
    if (size.ws_row != numbers[0] || size.ws_col != numbers[1])
        return raiseOverflowError(globalObject, scope, "winsize value(s) out of range."_s);
    if (ioctl(fd, TIOCSWINSZ, &size) < 0)
        return termiosError(globalObject);
    return jsUndefined();
}

static constexpr NativeDef termiosFunctions[] = {
    PY_FUNCTION("tcgetattr", termiosGetAttr, "(fd, /)"),
    PY_FUNCTION("tcsetattr", termiosSetAttr, "(fd, when, attributes, /)"),
    PY_FUNCTION("tcsendbreak", termiosControl<tcsendbreak>, "(fd, duration, /)"),
    PY_FUNCTION("tcdrain", termiosDrain, "(fd, /)"),
    PY_FUNCTION("tcflush", termiosControl<tcflush>, "(fd, queue, /)"),
    PY_FUNCTION("tcflow", termiosControl<tcflow>, "(fd, action, /)"),
    PY_FUNCTION("tcgetwinsize", termiosGetWinSize, "(fd, /)"),
    PY_FUNCTION("tcsetwinsize", termiosSetWinSize, "(fd, winsize, /)"),
};

#define TERMIOS_CONSTANT(name) { #name ""_s, static_cast<int64_t>(name) }

static constexpr PosixConstant termiosConstants[] = {
#ifdef ALTWERASE
    TERMIOS_CONSTANT(ALTWERASE),
#endif
#ifdef B0
    TERMIOS_CONSTANT(B0),
#endif
#ifdef B1000000
    TERMIOS_CONSTANT(B1000000),
#endif
#ifdef B110
    TERMIOS_CONSTANT(B110),
#endif
#ifdef B115200
    TERMIOS_CONSTANT(B115200),
#endif
#ifdef B1152000
    TERMIOS_CONSTANT(B1152000),
#endif
#ifdef B1200
    TERMIOS_CONSTANT(B1200),
#endif
#ifdef B134
    TERMIOS_CONSTANT(B134),
#endif
#ifdef B14400
    TERMIOS_CONSTANT(B14400),
#endif
#ifdef B150
    TERMIOS_CONSTANT(B150),
#endif
#ifdef B1500000
    TERMIOS_CONSTANT(B1500000),
#endif
#ifdef B1800
    TERMIOS_CONSTANT(B1800),
#endif
#ifdef B19200
    TERMIOS_CONSTANT(B19200),
#endif
#ifdef B200
    TERMIOS_CONSTANT(B200),
#endif
#ifdef B2000000
    TERMIOS_CONSTANT(B2000000),
#endif
#ifdef B230400
    TERMIOS_CONSTANT(B230400),
#endif
#ifdef B2400
    TERMIOS_CONSTANT(B2400),
#endif
#ifdef B2500000
    TERMIOS_CONSTANT(B2500000),
#endif
#ifdef B28800
    TERMIOS_CONSTANT(B28800),
#endif
#ifdef B300
    TERMIOS_CONSTANT(B300),
#endif
#ifdef B3000000
    TERMIOS_CONSTANT(B3000000),
#endif
#ifdef B3500000
    TERMIOS_CONSTANT(B3500000),
#endif
#ifdef B38400
    TERMIOS_CONSTANT(B38400),
#endif
#ifdef B4000000
    TERMIOS_CONSTANT(B4000000),
#endif
#ifdef B460800
    TERMIOS_CONSTANT(B460800),
#endif
#ifdef B4800
    TERMIOS_CONSTANT(B4800),
#endif
#ifdef B50
    TERMIOS_CONSTANT(B50),
#endif
#ifdef B500000
    TERMIOS_CONSTANT(B500000),
#endif
#ifdef B57600
    TERMIOS_CONSTANT(B57600),
#endif
#ifdef B576000
    TERMIOS_CONSTANT(B576000),
#endif
#ifdef B600
    TERMIOS_CONSTANT(B600),
#endif
#ifdef B7200
    TERMIOS_CONSTANT(B7200),
#endif
#ifdef B75
    TERMIOS_CONSTANT(B75),
#endif
#ifdef B76800
    TERMIOS_CONSTANT(B76800),
#endif
#ifdef B921600
    TERMIOS_CONSTANT(B921600),
#endif
#ifdef B9600
    TERMIOS_CONSTANT(B9600),
#endif
#ifdef BRKINT
    TERMIOS_CONSTANT(BRKINT),
#endif
#ifdef BS0
    TERMIOS_CONSTANT(BS0),
#endif
#ifdef BS1
    TERMIOS_CONSTANT(BS1),
#endif
#ifdef BSDLY
    TERMIOS_CONSTANT(BSDLY),
#endif
#ifdef CBAUD
    TERMIOS_CONSTANT(CBAUD),
#endif
#ifdef CBAUDEX
    TERMIOS_CONSTANT(CBAUDEX),
#endif
#ifdef CCAR_OFLOW
    TERMIOS_CONSTANT(CCAR_OFLOW),
#endif
#ifdef CCTS_OFLOW
    TERMIOS_CONSTANT(CCTS_OFLOW),
#endif
#ifdef CDEL
    TERMIOS_CONSTANT(CDEL),
#endif
#ifdef CDSR_OFLOW
    TERMIOS_CONSTANT(CDSR_OFLOW),
#endif
#ifdef CDSUSP
    TERMIOS_CONSTANT(CDSUSP),
#endif
#ifdef CDTR_IFLOW
    TERMIOS_CONSTANT(CDTR_IFLOW),
#endif
#ifdef CEOF
    TERMIOS_CONSTANT(CEOF),
#endif
#ifdef CEOL
    TERMIOS_CONSTANT(CEOL),
#endif
#ifdef CEOL2
    TERMIOS_CONSTANT(CEOL2),
#endif
#ifdef CEOT
    TERMIOS_CONSTANT(CEOT),
#endif
#ifdef CERASE
    TERMIOS_CONSTANT(CERASE),
#endif
#ifdef CESC
    TERMIOS_CONSTANT(CESC),
#endif
#ifdef CFLUSH
    TERMIOS_CONSTANT(CFLUSH),
#endif
#ifdef CIBAUD
    TERMIOS_CONSTANT(CIBAUD),
#endif
#ifdef CIGNORE
    TERMIOS_CONSTANT(CIGNORE),
#endif
#ifdef CINTR
    TERMIOS_CONSTANT(CINTR),
#endif
#ifdef CKILL
    TERMIOS_CONSTANT(CKILL),
#endif
#ifdef CLNEXT
    TERMIOS_CONSTANT(CLNEXT),
#endif
#ifdef CLOCAL
    TERMIOS_CONSTANT(CLOCAL),
#endif
#ifdef CNUL
    TERMIOS_CONSTANT(CNUL),
#endif
#ifdef COMMON
    TERMIOS_CONSTANT(COMMON),
#endif
#ifdef CQUIT
    TERMIOS_CONSTANT(CQUIT),
#endif
#ifdef CR0
    TERMIOS_CONSTANT(CR0),
#endif
#ifdef CR1
    TERMIOS_CONSTANT(CR1),
#endif
#ifdef CR2
    TERMIOS_CONSTANT(CR2),
#endif
#ifdef CR3
    TERMIOS_CONSTANT(CR3),
#endif
#ifdef CRDLY
    TERMIOS_CONSTANT(CRDLY),
#endif
#ifdef CREAD
    TERMIOS_CONSTANT(CREAD),
#endif
#ifdef CRPRNT
    TERMIOS_CONSTANT(CRPRNT),
#endif
#ifdef CRTS_IFLOW
    TERMIOS_CONSTANT(CRTS_IFLOW),
#endif
#ifdef CRTSCTS
    TERMIOS_CONSTANT(CRTSCTS),
#endif
#ifdef CS5
    TERMIOS_CONSTANT(CS5),
#endif
#ifdef CS6
    TERMIOS_CONSTANT(CS6),
#endif
#ifdef CS7
    TERMIOS_CONSTANT(CS7),
#endif
#ifdef CS8
    TERMIOS_CONSTANT(CS8),
#endif
#ifdef CSIZE
    TERMIOS_CONSTANT(CSIZE),
#endif
#ifdef CSTART
    TERMIOS_CONSTANT(CSTART),
#endif
#ifdef CSTOP
    TERMIOS_CONSTANT(CSTOP),
#endif
#ifdef CSTOPB
    TERMIOS_CONSTANT(CSTOPB),
#endif
#ifdef CSUSP
    TERMIOS_CONSTANT(CSUSP),
#endif
#ifdef CSWTCH
    TERMIOS_CONSTANT(CSWTCH),
#endif
#ifdef CWERASE
    TERMIOS_CONSTANT(CWERASE),
#endif
#ifdef ECHO
    TERMIOS_CONSTANT(ECHO),
#endif
#ifdef ECHOCTL
    TERMIOS_CONSTANT(ECHOCTL),
#endif
#ifdef ECHOE
    TERMIOS_CONSTANT(ECHOE),
#endif
#ifdef ECHOK
    TERMIOS_CONSTANT(ECHOK),
#endif
#ifdef ECHOKE
    TERMIOS_CONSTANT(ECHOKE),
#endif
#ifdef ECHONL
    TERMIOS_CONSTANT(ECHONL),
#endif
#ifdef ECHOPRT
    TERMIOS_CONSTANT(ECHOPRT),
#endif
#ifdef EXTA
    TERMIOS_CONSTANT(EXTA),
#endif
#ifdef EXTB
    TERMIOS_CONSTANT(EXTB),
#endif
#ifdef EXTPROC
    TERMIOS_CONSTANT(EXTPROC),
#endif
#ifdef FF0
    TERMIOS_CONSTANT(FF0),
#endif
#ifdef FF1
    TERMIOS_CONSTANT(FF1),
#endif
#ifdef FFDLY
    TERMIOS_CONSTANT(FFDLY),
#endif
#ifdef FIOASYNC
    TERMIOS_CONSTANT(FIOASYNC),
#endif
#ifdef FIOCLEX
    TERMIOS_CONSTANT(FIOCLEX),
#endif
#ifdef FIONBIO
    TERMIOS_CONSTANT(FIONBIO),
#endif
#ifdef FIONCLEX
    TERMIOS_CONSTANT(FIONCLEX),
#endif
#ifdef FIONREAD
    TERMIOS_CONSTANT(FIONREAD),
#endif
#ifdef FLUSHO
    TERMIOS_CONSTANT(FLUSHO),
#endif
#ifdef HUPCL
    TERMIOS_CONSTANT(HUPCL),
#endif
#ifdef IBSHIFT
    TERMIOS_CONSTANT(IBSHIFT),
#endif
#ifdef ICANON
    TERMIOS_CONSTANT(ICANON),
#endif
#ifdef ICRNL
    TERMIOS_CONSTANT(ICRNL),
#endif
#ifdef IEXTEN
    TERMIOS_CONSTANT(IEXTEN),
#endif
#ifdef IGNBRK
    TERMIOS_CONSTANT(IGNBRK),
#endif
#ifdef IGNCR
    TERMIOS_CONSTANT(IGNCR),
#endif
#ifdef IGNPAR
    TERMIOS_CONSTANT(IGNPAR),
#endif
#ifdef IMAXBEL
    TERMIOS_CONSTANT(IMAXBEL),
#endif
#ifdef INIT_C_CC
    TERMIOS_CONSTANT(INIT_C_CC),
#endif
#ifdef INLCR
    TERMIOS_CONSTANT(INLCR),
#endif
#ifdef INPCK
    TERMIOS_CONSTANT(INPCK),
#endif
#ifdef IOCSIZE_MASK
    TERMIOS_CONSTANT(IOCSIZE_MASK),
#endif
#ifdef IOCSIZE_SHIFT
    TERMIOS_CONSTANT(IOCSIZE_SHIFT),
#endif
#ifdef ISIG
    TERMIOS_CONSTANT(ISIG),
#endif
#ifdef ISTRIP
    TERMIOS_CONSTANT(ISTRIP),
#endif
#ifdef IUCLC
    TERMIOS_CONSTANT(IUCLC),
#endif
#ifdef IUTF8
    TERMIOS_CONSTANT(IUTF8),
#endif
#ifdef IXANY
    TERMIOS_CONSTANT(IXANY),
#endif
#ifdef IXOFF
    TERMIOS_CONSTANT(IXOFF),
#endif
#ifdef IXON
    TERMIOS_CONSTANT(IXON),
#endif
#ifdef MDMBUF
    TERMIOS_CONSTANT(MDMBUF),
#endif
#ifdef NCC
    TERMIOS_CONSTANT(NCC),
#endif
#ifdef NCCS
    TERMIOS_CONSTANT(NCCS),
#endif
#ifdef NL0
    TERMIOS_CONSTANT(NL0),
#endif
#ifdef NL1
    TERMIOS_CONSTANT(NL1),
#endif
#ifdef NL2
    TERMIOS_CONSTANT(NL2),
#endif
#ifdef NL3
    TERMIOS_CONSTANT(NL3),
#endif
#ifdef NLDLY
    TERMIOS_CONSTANT(NLDLY),
#endif
#ifdef NOFLSH
    TERMIOS_CONSTANT(NOFLSH),
#endif
#ifdef NOKERNINFO
    TERMIOS_CONSTANT(NOKERNINFO),
#endif
#ifdef NSWTCH
    TERMIOS_CONSTANT(NSWTCH),
#endif
#ifdef N_MOUSE
    TERMIOS_CONSTANT(N_MOUSE),
#endif
#ifdef N_PPP
    TERMIOS_CONSTANT(N_PPP),
#endif
#ifdef N_SLIP
    TERMIOS_CONSTANT(N_SLIP),
#endif
#ifdef N_STRIP
    TERMIOS_CONSTANT(N_STRIP),
#endif
#ifdef N_TTY
    TERMIOS_CONSTANT(N_TTY),
#endif
#ifdef OCRNL
    TERMIOS_CONSTANT(OCRNL),
#endif
#ifdef OFDEL
    TERMIOS_CONSTANT(OFDEL),
#endif
#ifdef OFILL
    TERMIOS_CONSTANT(OFILL),
#endif
#ifdef OLCUC
    TERMIOS_CONSTANT(OLCUC),
#endif
#ifdef ONLCR
    TERMIOS_CONSTANT(ONLCR),
#endif
#ifdef ONLRET
    TERMIOS_CONSTANT(ONLRET),
#endif
#ifdef ONOCR
    TERMIOS_CONSTANT(ONOCR),
#endif
#ifdef ONOEOT
    TERMIOS_CONSTANT(ONOEOT),
#endif
#ifdef OPOST
    TERMIOS_CONSTANT(OPOST),
#endif
#ifdef OXTABS
    TERMIOS_CONSTANT(OXTABS),
#endif
#ifdef PARENB
    TERMIOS_CONSTANT(PARENB),
#endif
#ifdef PARMRK
    TERMIOS_CONSTANT(PARMRK),
#endif
#ifdef PARODD
    TERMIOS_CONSTANT(PARODD),
#endif
#ifdef PENDIN
    TERMIOS_CONSTANT(PENDIN),
#endif
#ifdef TAB0
    TERMIOS_CONSTANT(TAB0),
#endif
#ifdef TAB1
    TERMIOS_CONSTANT(TAB1),
#endif
#ifdef TAB2
    TERMIOS_CONSTANT(TAB2),
#endif
#ifdef TAB3
    TERMIOS_CONSTANT(TAB3),
#endif
#ifdef TABDLY
    TERMIOS_CONSTANT(TABDLY),
#endif
#ifdef TCFLSH
    TERMIOS_CONSTANT(TCFLSH),
#endif
#ifdef TCGETA
    TERMIOS_CONSTANT(TCGETA),
#endif
#ifdef TCGETS
    TERMIOS_CONSTANT(TCGETS),
#endif
#ifdef TCIFLUSH
    TERMIOS_CONSTANT(TCIFLUSH),
#endif
#ifdef TCIOFF
    TERMIOS_CONSTANT(TCIOFF),
#endif
#ifdef TCIOFLUSH
    TERMIOS_CONSTANT(TCIOFLUSH),
#endif
#ifdef TCION
    TERMIOS_CONSTANT(TCION),
#endif
#ifdef TCOFLUSH
    TERMIOS_CONSTANT(TCOFLUSH),
#endif
#ifdef TCOOFF
    TERMIOS_CONSTANT(TCOOFF),
#endif
#ifdef TCOON
    TERMIOS_CONSTANT(TCOON),
#endif
#ifdef TCSADRAIN
    TERMIOS_CONSTANT(TCSADRAIN),
#endif
#ifdef TCSAFLUSH
    TERMIOS_CONSTANT(TCSAFLUSH),
#endif
#ifdef TCSANOW
    TERMIOS_CONSTANT(TCSANOW),
#endif
#ifdef TCSASOFT
    TERMIOS_CONSTANT(TCSASOFT),
#endif
#ifdef TCSBRK
    TERMIOS_CONSTANT(TCSBRK),
#endif
#ifdef TCSBRKP
    TERMIOS_CONSTANT(TCSBRKP),
#endif
#ifdef TCSETA
    TERMIOS_CONSTANT(TCSETA),
#endif
#ifdef TCSETAF
    TERMIOS_CONSTANT(TCSETAF),
#endif
#ifdef TCSETAW
    TERMIOS_CONSTANT(TCSETAW),
#endif
#ifdef TCSETS
    TERMIOS_CONSTANT(TCSETS),
#endif
#ifdef TCSETSF
    TERMIOS_CONSTANT(TCSETSF),
#endif
#ifdef TCSETSW
    TERMIOS_CONSTANT(TCSETSW),
#endif
#ifdef TCXONC
    TERMIOS_CONSTANT(TCXONC),
#endif
#ifdef TIOCCONS
    TERMIOS_CONSTANT(TIOCCONS),
#endif
#ifdef TIOCEXCL
    TERMIOS_CONSTANT(TIOCEXCL),
#endif
#ifdef TIOCGETD
    TERMIOS_CONSTANT(TIOCGETD),
#endif
#ifdef TIOCGICOUNT
    TERMIOS_CONSTANT(TIOCGICOUNT),
#endif
#ifdef TIOCGLCKTRMIOS
    TERMIOS_CONSTANT(TIOCGLCKTRMIOS),
#endif
#ifdef TIOCGPGRP
    TERMIOS_CONSTANT(TIOCGPGRP),
#endif
#ifdef TIOCGSERIAL
    TERMIOS_CONSTANT(TIOCGSERIAL),
#endif
#ifdef TIOCGSIZE
    TERMIOS_CONSTANT(TIOCGSIZE),
#endif
#ifdef TIOCGSOFTCAR
    TERMIOS_CONSTANT(TIOCGSOFTCAR),
#endif
#ifdef TIOCGWINSZ
    TERMIOS_CONSTANT(TIOCGWINSZ),
#endif
#ifdef TIOCINQ
    TERMIOS_CONSTANT(TIOCINQ),
#endif
#ifdef TIOCLINUX
    TERMIOS_CONSTANT(TIOCLINUX),
#endif
#ifdef TIOCMBIC
    TERMIOS_CONSTANT(TIOCMBIC),
#endif
#ifdef TIOCMBIS
    TERMIOS_CONSTANT(TIOCMBIS),
#endif
#ifdef TIOCMGET
    TERMIOS_CONSTANT(TIOCMGET),
#endif
#ifdef TIOCMIWAIT
    TERMIOS_CONSTANT(TIOCMIWAIT),
#endif
#ifdef TIOCMSET
    TERMIOS_CONSTANT(TIOCMSET),
#endif
#ifdef TIOCM_CAR
    TERMIOS_CONSTANT(TIOCM_CAR),
#endif
#ifdef TIOCM_CD
    TERMIOS_CONSTANT(TIOCM_CD),
#endif
#ifdef TIOCM_CTS
    TERMIOS_CONSTANT(TIOCM_CTS),
#endif
#ifdef TIOCM_DSR
    TERMIOS_CONSTANT(TIOCM_DSR),
#endif
#ifdef TIOCM_DTR
    TERMIOS_CONSTANT(TIOCM_DTR),
#endif
#ifdef TIOCM_LE
    TERMIOS_CONSTANT(TIOCM_LE),
#endif
#ifdef TIOCM_RI
    TERMIOS_CONSTANT(TIOCM_RI),
#endif
#ifdef TIOCM_RNG
    TERMIOS_CONSTANT(TIOCM_RNG),
#endif
#ifdef TIOCM_RTS
    TERMIOS_CONSTANT(TIOCM_RTS),
#endif
#ifdef TIOCM_SR
    TERMIOS_CONSTANT(TIOCM_SR),
#endif
#ifdef TIOCM_ST
    TERMIOS_CONSTANT(TIOCM_ST),
#endif
#ifdef TIOCNOTTY
    TERMIOS_CONSTANT(TIOCNOTTY),
#endif
#ifdef TIOCNXCL
    TERMIOS_CONSTANT(TIOCNXCL),
#endif
#ifdef TIOCOUTQ
    TERMIOS_CONSTANT(TIOCOUTQ),
#endif
#ifdef TIOCPKT
    TERMIOS_CONSTANT(TIOCPKT),
#endif
#ifdef TIOCPKT_DATA
    TERMIOS_CONSTANT(TIOCPKT_DATA),
#endif
#ifdef TIOCPKT_DOSTOP
    TERMIOS_CONSTANT(TIOCPKT_DOSTOP),
#endif
#ifdef TIOCPKT_FLUSHREAD
    TERMIOS_CONSTANT(TIOCPKT_FLUSHREAD),
#endif
#ifdef TIOCPKT_FLUSHWRITE
    TERMIOS_CONSTANT(TIOCPKT_FLUSHWRITE),
#endif
#ifdef TIOCPKT_NOSTOP
    TERMIOS_CONSTANT(TIOCPKT_NOSTOP),
#endif
#ifdef TIOCPKT_START
    TERMIOS_CONSTANT(TIOCPKT_START),
#endif
#ifdef TIOCPKT_STOP
    TERMIOS_CONSTANT(TIOCPKT_STOP),
#endif
#ifdef TIOCSCTTY
    TERMIOS_CONSTANT(TIOCSCTTY),
#endif
#ifdef TIOCSERCONFIG
    TERMIOS_CONSTANT(TIOCSERCONFIG),
#endif
#ifdef TIOCSERGETLSR
    TERMIOS_CONSTANT(TIOCSERGETLSR),
#endif
#ifdef TIOCSERGETMULTI
    TERMIOS_CONSTANT(TIOCSERGETMULTI),
#endif
#ifdef TIOCSERGSTRUCT
    TERMIOS_CONSTANT(TIOCSERGSTRUCT),
#endif
#ifdef TIOCSERGWILD
    TERMIOS_CONSTANT(TIOCSERGWILD),
#endif
#ifdef TIOCSERSETMULTI
    TERMIOS_CONSTANT(TIOCSERSETMULTI),
#endif
#ifdef TIOCSERSWILD
    TERMIOS_CONSTANT(TIOCSERSWILD),
#endif
#ifdef TIOCSER_TEMT
    TERMIOS_CONSTANT(TIOCSER_TEMT),
#endif
#ifdef TIOCSETD
    TERMIOS_CONSTANT(TIOCSETD),
#endif
#ifdef TIOCSLCKTRMIOS
    TERMIOS_CONSTANT(TIOCSLCKTRMIOS),
#endif
#ifdef TIOCSPGRP
    TERMIOS_CONSTANT(TIOCSPGRP),
#endif
#ifdef TIOCSSERIAL
    TERMIOS_CONSTANT(TIOCSSERIAL),
#endif
#ifdef TIOCSSIZE
    TERMIOS_CONSTANT(TIOCSSIZE),
#endif
#ifdef TIOCSSOFTCAR
    TERMIOS_CONSTANT(TIOCSSOFTCAR),
#endif
#ifdef TIOCSTI
    TERMIOS_CONSTANT(TIOCSTI),
#endif
#ifdef TIOCSWINSZ
    TERMIOS_CONSTANT(TIOCSWINSZ),
#endif
#ifdef TIOCTTYGSTRUCT
    TERMIOS_CONSTANT(TIOCTTYGSTRUCT),
#endif
#ifdef TOSTOP
    TERMIOS_CONSTANT(TOSTOP),
#endif
#ifdef VDISCARD
    TERMIOS_CONSTANT(VDISCARD),
#endif
#ifdef VDSUSP
    TERMIOS_CONSTANT(VDSUSP),
#endif
#ifdef VEOF
    TERMIOS_CONSTANT(VEOF),
#endif
#ifdef VEOL
    TERMIOS_CONSTANT(VEOL),
#endif
#ifdef VEOL2
    TERMIOS_CONSTANT(VEOL2),
#endif
#ifdef VERASE
    TERMIOS_CONSTANT(VERASE),
#endif
#ifdef VINTR
    TERMIOS_CONSTANT(VINTR),
#endif
#ifdef VKILL
    TERMIOS_CONSTANT(VKILL),
#endif
#ifdef VLNEXT
    TERMIOS_CONSTANT(VLNEXT),
#endif
#ifdef VMIN
    TERMIOS_CONSTANT(VMIN),
#endif
#ifdef VQUIT
    TERMIOS_CONSTANT(VQUIT),
#endif
#ifdef VREPRINT
    TERMIOS_CONSTANT(VREPRINT),
#endif
#ifdef VSTART
    TERMIOS_CONSTANT(VSTART),
#endif
#ifdef VSTATUS
    TERMIOS_CONSTANT(VSTATUS),
#endif
#ifdef VSTOP
    TERMIOS_CONSTANT(VSTOP),
#endif
#ifdef VSUSP
    TERMIOS_CONSTANT(VSUSP),
#endif
#ifdef VSWTC
    TERMIOS_CONSTANT(VSWTC),
#endif
#ifdef VSWTCH
    TERMIOS_CONSTANT(VSWTCH),
#endif
#ifdef VT0
    TERMIOS_CONSTANT(VT0),
#endif
#ifdef VT1
    TERMIOS_CONSTANT(VT1),
#endif
#ifdef VTDLY
    TERMIOS_CONSTANT(VTDLY),
#endif
#ifdef VTIME
    TERMIOS_CONSTANT(VTIME),
#endif
#ifdef VWERASE
    TERMIOS_CONSTANT(VWERASE),
#endif
#ifdef XCASE
    TERMIOS_CONSTANT(XCASE),
#endif
#ifdef XTABS
    TERMIOS_CONSTANT(XTABS),
#endif
#ifdef _POSIX_VDISABLE
    TERMIOS_CONSTANT(_POSIX_VDISABLE),
#endif
};

void initializeTermiosModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, termiosFunctions);
    module->add(globalObject, "error"_s, termiosErrorType(globalObject));
    addPosixConstants(globalObject, module, termiosConstants);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
