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
#include "PythonPosix.h"

#if OS(UNIX)

#include <dlfcn.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <unistd.h>
#if OS(DARWIN)
#include <copyfile.h>
#endif

// The constants of posix, and the names that pathconf(), confstr() and sysconf() go by: all_ins() and the tables of Modules/posixmodule.c of CPython. Each is there if the system has it.

namespace JSC { namespace Python {

void addPosixConstants(JSGlobalObject* globalObject, JSObject* module)
{
    VM& vm = globalObject->vm();
    auto add = [&] (ASCIILiteral name, int64_t value) { module->putDirect(vm, Identifier::fromString(vm, name), intFromInt64(globalObject, value)); };

#ifdef F_OK
    add("F_OK"_s, F_OK);
#endif
#ifdef R_OK
    add("R_OK"_s, R_OK);
#endif
#ifdef W_OK
    add("W_OK"_s, W_OK);
#endif
#ifdef X_OK
    add("X_OK"_s, X_OK);
#endif
#ifdef NGROUPS_MAX
    add("NGROUPS_MAX"_s, NGROUPS_MAX);
#endif
#ifdef TMP_MAX
    add("TMP_MAX"_s, TMP_MAX);
#endif
#ifdef WCONTINUED
    add("WCONTINUED"_s, WCONTINUED);
#endif
#ifdef WNOHANG
    add("WNOHANG"_s, WNOHANG);
#endif
#ifdef WUNTRACED
    add("WUNTRACED"_s, WUNTRACED);
#endif
#ifdef O_RDONLY
    add("O_RDONLY"_s, O_RDONLY);
#endif
#ifdef O_WRONLY
    add("O_WRONLY"_s, O_WRONLY);
#endif
#ifdef O_RDWR
    add("O_RDWR"_s, O_RDWR);
#endif
#ifdef O_NDELAY
    add("O_NDELAY"_s, O_NDELAY);
#endif
#ifdef O_NONBLOCK
    add("O_NONBLOCK"_s, O_NONBLOCK);
#endif
#ifdef O_APPEND
    add("O_APPEND"_s, O_APPEND);
#endif
#ifdef O_DSYNC
    add("O_DSYNC"_s, O_DSYNC);
#endif
#ifdef O_RSYNC
    add("O_RSYNC"_s, O_RSYNC);
#endif
#ifdef O_SYNC
    add("O_SYNC"_s, O_SYNC);
#endif
#ifdef O_NOCTTY
    add("O_NOCTTY"_s, O_NOCTTY);
#endif
#ifdef O_CREAT
    add("O_CREAT"_s, O_CREAT);
#endif
#ifdef O_EXCL
    add("O_EXCL"_s, O_EXCL);
#endif
#ifdef O_TRUNC
    add("O_TRUNC"_s, O_TRUNC);
#endif
#ifdef O_BINARY
    add("O_BINARY"_s, O_BINARY);
#endif
#ifdef O_TEXT
    add("O_TEXT"_s, O_TEXT);
#endif
#ifdef O_XATTR
    add("O_XATTR"_s, O_XATTR);
#endif
#ifdef O_LARGEFILE
    add("O_LARGEFILE"_s, O_LARGEFILE);
#endif
#ifdef O_SHLOCK
    add("O_SHLOCK"_s, O_SHLOCK);
#endif
#ifdef O_EXLOCK
    add("O_EXLOCK"_s, O_EXLOCK);
#endif
#ifdef O_EXEC
    add("O_EXEC"_s, O_EXEC);
#endif
#ifdef O_SEARCH
    add("O_SEARCH"_s, O_SEARCH);
#endif
#ifdef O_PATH
    add("O_PATH"_s, O_PATH);
#endif
#ifdef O_TTY_INIT
    add("O_TTY_INIT"_s, O_TTY_INIT);
#endif
#ifdef O_TMPFILE
    add("O_TMPFILE"_s, O_TMPFILE);
#endif
#ifdef PRIO_PROCESS
    add("PRIO_PROCESS"_s, PRIO_PROCESS);
#endif
#ifdef PRIO_PGRP
    add("PRIO_PGRP"_s, PRIO_PGRP);
#endif
#ifdef PRIO_USER
    add("PRIO_USER"_s, PRIO_USER);
#endif
#ifdef PRIO_DARWIN_THREAD
    add("PRIO_DARWIN_THREAD"_s, PRIO_DARWIN_THREAD);
#endif
#ifdef PRIO_DARWIN_PROCESS
    add("PRIO_DARWIN_PROCESS"_s, PRIO_DARWIN_PROCESS);
#endif
#ifdef PRIO_DARWIN_BG
    add("PRIO_DARWIN_BG"_s, PRIO_DARWIN_BG);
#endif
#ifdef PRIO_DARWIN_NONUI
    add("PRIO_DARWIN_NONUI"_s, PRIO_DARWIN_NONUI);
#endif
#ifdef O_CLOEXEC
    add("O_CLOEXEC"_s, O_CLOEXEC);
#endif
#ifdef O_ACCMODE
    add("O_ACCMODE"_s, O_ACCMODE);
#endif
#ifdef O_EVTONLY
    add("O_EVTONLY"_s, O_EVTONLY);
#endif
#ifdef O_FSYNC
    add("O_FSYNC"_s, O_FSYNC);
#endif
#ifdef O_SYMLINK
    add("O_SYMLINK"_s, O_SYMLINK);
#endif
#ifdef SEEK_HOLE
    add("SEEK_HOLE"_s, SEEK_HOLE);
#endif
#ifdef SEEK_DATA
    add("SEEK_DATA"_s, SEEK_DATA);
#endif
#ifdef O_NOINHERIT
    add("O_NOINHERIT"_s, O_NOINHERIT);
#endif
#ifdef O_TEMPORARY
    add("O_TEMPORARY"_s, O_TEMPORARY);
#endif
#ifdef O_RANDOM
    add("O_RANDOM"_s, O_RANDOM);
#endif
#ifdef O_SEQUENTIAL
    add("O_SEQUENTIAL"_s, O_SEQUENTIAL);
#endif
#ifdef O_ASYNC
    add("O_ASYNC"_s, O_ASYNC);
#endif
#ifdef O_DIRECT
    add("O_DIRECT"_s, O_DIRECT);
#endif
#ifdef O_DIRECTORY
    add("O_DIRECTORY"_s, O_DIRECTORY);
#endif
#ifdef O_NOFOLLOW
    add("O_NOFOLLOW"_s, O_NOFOLLOW);
#endif
#ifdef O_NOFOLLOW_ANY
    add("O_NOFOLLOW_ANY"_s, O_NOFOLLOW_ANY);
#endif
#ifdef O_NOLINKS
    add("O_NOLINKS"_s, O_NOLINKS);
#endif
#ifdef O_NOATIME
    add("O_NOATIME"_s, O_NOATIME);
#endif
#ifdef EX_OK
    add("EX_OK"_s, EX_OK);
#endif
#ifdef EX_USAGE
    add("EX_USAGE"_s, EX_USAGE);
#endif
#ifdef EX_DATAERR
    add("EX_DATAERR"_s, EX_DATAERR);
#endif
#ifdef EX_NOINPUT
    add("EX_NOINPUT"_s, EX_NOINPUT);
#endif
#ifdef EX_NOUSER
    add("EX_NOUSER"_s, EX_NOUSER);
#endif
#ifdef EX_NOHOST
    add("EX_NOHOST"_s, EX_NOHOST);
#endif
#ifdef EX_UNAVAILABLE
    add("EX_UNAVAILABLE"_s, EX_UNAVAILABLE);
#endif
#ifdef EX_SOFTWARE
    add("EX_SOFTWARE"_s, EX_SOFTWARE);
#endif
#ifdef EX_OSERR
    add("EX_OSERR"_s, EX_OSERR);
#endif
#ifdef EX_OSFILE
    add("EX_OSFILE"_s, EX_OSFILE);
#endif
#ifdef EX_CANTCREAT
    add("EX_CANTCREAT"_s, EX_CANTCREAT);
#endif
#ifdef EX_IOERR
    add("EX_IOERR"_s, EX_IOERR);
#endif
#ifdef EX_TEMPFAIL
    add("EX_TEMPFAIL"_s, EX_TEMPFAIL);
#endif
#ifdef EX_PROTOCOL
    add("EX_PROTOCOL"_s, EX_PROTOCOL);
#endif
#ifdef EX_NOPERM
    add("EX_NOPERM"_s, EX_NOPERM);
#endif
#ifdef EX_CONFIG
    add("EX_CONFIG"_s, EX_CONFIG);
#endif
#ifdef EX_NOTFOUND
    add("EX_NOTFOUND"_s, EX_NOTFOUND);
#endif
#ifdef ST_RDONLY
    add("ST_RDONLY"_s, ST_RDONLY);
#endif
#ifdef ST_NOSUID
    add("ST_NOSUID"_s, ST_NOSUID);
#endif
#ifdef ST_NODEV
    add("ST_NODEV"_s, ST_NODEV);
#endif
#ifdef ST_NOEXEC
    add("ST_NOEXEC"_s, ST_NOEXEC);
#endif
#ifdef ST_SYNCHRONOUS
    add("ST_SYNCHRONOUS"_s, ST_SYNCHRONOUS);
#endif
#ifdef ST_MANDLOCK
    add("ST_MANDLOCK"_s, ST_MANDLOCK);
#endif
#ifdef ST_WRITE
    add("ST_WRITE"_s, ST_WRITE);
#endif
#ifdef ST_APPEND
    add("ST_APPEND"_s, ST_APPEND);
#endif
#ifdef ST_NOATIME
    add("ST_NOATIME"_s, ST_NOATIME);
#endif
#ifdef ST_NODIRATIME
    add("ST_NODIRATIME"_s, ST_NODIRATIME);
#endif
#ifdef ST_RELATIME
    add("ST_RELATIME"_s, ST_RELATIME);
#endif
#ifdef SF_NODISKIO
    add("SF_NODISKIO"_s, SF_NODISKIO);
#endif
#ifdef SF_MNOWAIT
    add("SF_MNOWAIT"_s, SF_MNOWAIT);
#endif
#ifdef SF_SYNC
    add("SF_SYNC"_s, SF_SYNC);
#endif
#ifdef SF_NOCACHE
    add("SF_NOCACHE"_s, SF_NOCACHE);
#endif
#ifdef TFD_NONBLOCK
    add("TFD_NONBLOCK"_s, TFD_NONBLOCK);
#endif
#ifdef TFD_CLOEXEC
    add("TFD_CLOEXEC"_s, TFD_CLOEXEC);
#endif
#ifdef TFD_TIMER_ABSTIME
    add("TFD_TIMER_ABSTIME"_s, TFD_TIMER_ABSTIME);
#endif
#ifdef TFD_TIMER_CANCEL_ON_SET
    add("TFD_TIMER_CANCEL_ON_SET"_s, TFD_TIMER_CANCEL_ON_SET);
#endif
#ifdef POSIX_FADV_NORMAL
    add("POSIX_FADV_NORMAL"_s, POSIX_FADV_NORMAL);
#endif
#ifdef POSIX_FADV_SEQUENTIAL
    add("POSIX_FADV_SEQUENTIAL"_s, POSIX_FADV_SEQUENTIAL);
#endif
#ifdef POSIX_FADV_RANDOM
    add("POSIX_FADV_RANDOM"_s, POSIX_FADV_RANDOM);
#endif
#ifdef POSIX_FADV_NOREUSE
    add("POSIX_FADV_NOREUSE"_s, POSIX_FADV_NOREUSE);
#endif
#ifdef POSIX_FADV_WILLNEED
    add("POSIX_FADV_WILLNEED"_s, POSIX_FADV_WILLNEED);
#endif
#ifdef POSIX_FADV_DONTNEED
    add("POSIX_FADV_DONTNEED"_s, POSIX_FADV_DONTNEED);
#endif
    add("P_PID"_s, P_PID);
    add("P_PGID"_s, P_PGID);
    add("P_ALL"_s, P_ALL);
#ifdef P_PIDFD
    add("P_PIDFD"_s, P_PIDFD);
#endif
#ifdef PIDFD_NONBLOCK
    add("PIDFD_NONBLOCK"_s, PIDFD_NONBLOCK);
#endif
#ifdef WEXITED
    add("WEXITED"_s, WEXITED);
#endif
#ifdef WNOWAIT
    add("WNOWAIT"_s, WNOWAIT);
#endif
#ifdef WSTOPPED
    add("WSTOPPED"_s, WSTOPPED);
#endif
#ifdef CLD_EXITED
    add("CLD_EXITED"_s, CLD_EXITED);
#endif
#ifdef CLD_KILLED
    add("CLD_KILLED"_s, CLD_KILLED);
#endif
#ifdef CLD_DUMPED
    add("CLD_DUMPED"_s, CLD_DUMPED);
#endif
#ifdef CLD_TRAPPED
    add("CLD_TRAPPED"_s, CLD_TRAPPED);
#endif
#ifdef CLD_STOPPED
    add("CLD_STOPPED"_s, CLD_STOPPED);
#endif
#ifdef CLD_CONTINUED
    add("CLD_CONTINUED"_s, CLD_CONTINUED);
#endif
#ifdef F_LOCK
    add("F_LOCK"_s, F_LOCK);
#endif
#ifdef F_TLOCK
    add("F_TLOCK"_s, F_TLOCK);
#endif
#ifdef F_ULOCK
    add("F_ULOCK"_s, F_ULOCK);
#endif
#ifdef F_TEST
    add("F_TEST"_s, F_TEST);
#endif
#ifdef RWF_DSYNC
    add("RWF_DSYNC"_s, RWF_DSYNC);
#endif
#ifdef RWF_HIPRI
    add("RWF_HIPRI"_s, RWF_HIPRI);
#endif
#ifdef RWF_SYNC
    add("RWF_SYNC"_s, RWF_SYNC);
#endif
#ifdef RWF_NOWAIT
    add("RWF_NOWAIT"_s, RWF_NOWAIT);
#endif
#ifdef RWF_APPEND
    add("RWF_APPEND"_s, RWF_APPEND);
#endif
#ifdef SPLICE_F_MOVE
    add("SPLICE_F_MOVE"_s, SPLICE_F_MOVE);
#endif
#ifdef SPLICE_F_NONBLOCK
    add("SPLICE_F_NONBLOCK"_s, SPLICE_F_NONBLOCK);
#endif
#ifdef SPLICE_F_MORE
    add("SPLICE_F_MORE"_s, SPLICE_F_MORE);
#endif
    add("POSIX_SPAWN_OPEN"_s, 0);
    add("POSIX_SPAWN_CLOSE"_s, 1);
    add("POSIX_SPAWN_DUP2"_s, 2);
#ifdef SCHED_OTHER
    add("SCHED_OTHER"_s, SCHED_OTHER);
#endif
#ifdef SCHED_DEADLINE
    add("SCHED_DEADLINE"_s, SCHED_DEADLINE);
#endif
#ifdef SCHED_FIFO
    add("SCHED_FIFO"_s, SCHED_FIFO);
#endif
#ifdef SCHED_NORMAL
    add("SCHED_NORMAL"_s, SCHED_NORMAL);
#endif
#ifdef SCHED_RR
    add("SCHED_RR"_s, SCHED_RR);
#endif
#ifdef SCHED_SPORADIC
    add("SCHED_SPORADIC"_s, SCHED_SPORADIC);
#endif
#ifdef SCHED_BATCH
    add("SCHED_BATCH"_s, SCHED_BATCH);
#endif
#ifdef SCHED_IDLE
    add("SCHED_IDLE"_s, SCHED_IDLE);
#endif
#ifdef SCHED_RESET_ON_FORK
    add("SCHED_RESET_ON_FORK"_s, SCHED_RESET_ON_FORK);
#endif
#ifdef SCHED_SYS
    add("SCHED_SYS"_s, SCHED_SYS);
#endif
#ifdef SCHED_IA
    add("SCHED_IA"_s, SCHED_IA);
#endif
#ifdef SCHED_FSS
    add("SCHED_FSS"_s, SCHED_FSS);
#endif
#ifdef SCHED_FSS
    add("SCHED_FX"_s, SCHED_FSS);
#endif
#ifdef CLONE_FS
    add("CLONE_FS"_s, CLONE_FS);
#endif
#ifdef CLONE_FILES
    add("CLONE_FILES"_s, CLONE_FILES);
#endif
#ifdef CLONE_NEWNS
    add("CLONE_NEWNS"_s, CLONE_NEWNS);
#endif
#ifdef CLONE_NEWCGROUP
    add("CLONE_NEWCGROUP"_s, CLONE_NEWCGROUP);
#endif
#ifdef CLONE_NEWUTS
    add("CLONE_NEWUTS"_s, CLONE_NEWUTS);
#endif
#ifdef CLONE_NEWIPC
    add("CLONE_NEWIPC"_s, CLONE_NEWIPC);
#endif
#ifdef CLONE_NEWUSER
    add("CLONE_NEWUSER"_s, CLONE_NEWUSER);
#endif
#ifdef CLONE_NEWPID
    add("CLONE_NEWPID"_s, CLONE_NEWPID);
#endif
#ifdef CLONE_NEWNET
    add("CLONE_NEWNET"_s, CLONE_NEWNET);
#endif
#ifdef CLONE_NEWTIME
    add("CLONE_NEWTIME"_s, CLONE_NEWTIME);
#endif
#ifdef CLONE_SYSVSEM
    add("CLONE_SYSVSEM"_s, CLONE_SYSVSEM);
#endif
#ifdef CLONE_THREAD
    add("CLONE_THREAD"_s, CLONE_THREAD);
#endif
#ifdef CLONE_SIGHAND
    add("CLONE_SIGHAND"_s, CLONE_SIGHAND);
#endif
#ifdef CLONE_VM
    add("CLONE_VM"_s, CLONE_VM);
#endif
#ifdef XATTR_CREATE
    add("XATTR_CREATE"_s, XATTR_CREATE);
#endif
#ifdef XATTR_REPLACE
    add("XATTR_REPLACE"_s, XATTR_REPLACE);
#endif
#ifdef XATTR_SIZE_MAX
    add("XATTR_SIZE_MAX"_s, XATTR_SIZE_MAX);
#endif
#ifdef RTLD_LAZY
    add("RTLD_LAZY"_s, RTLD_LAZY);
#endif
#ifdef RTLD_NOW
    add("RTLD_NOW"_s, RTLD_NOW);
#endif
#ifdef RTLD_GLOBAL
    add("RTLD_GLOBAL"_s, RTLD_GLOBAL);
#endif
#ifdef RTLD_LOCAL
    add("RTLD_LOCAL"_s, RTLD_LOCAL);
#endif
#ifdef RTLD_NODELETE
    add("RTLD_NODELETE"_s, RTLD_NODELETE);
#endif
#ifdef RTLD_NOLOAD
    add("RTLD_NOLOAD"_s, RTLD_NOLOAD);
#endif
#ifdef RTLD_DEEPBIND
    add("RTLD_DEEPBIND"_s, RTLD_DEEPBIND);
#endif
#ifdef RTLD_MEMBER
    add("RTLD_MEMBER"_s, RTLD_MEMBER);
#endif
#ifdef GRND_RANDOM
    add("GRND_RANDOM"_s, GRND_RANDOM);
#endif
#ifdef GRND_NONBLOCK
    add("GRND_NONBLOCK"_s, GRND_NONBLOCK);
#endif
#ifdef MFD_CLOEXEC
    add("MFD_CLOEXEC"_s, MFD_CLOEXEC);
#endif
#ifdef MFD_ALLOW_SEALING
    add("MFD_ALLOW_SEALING"_s, MFD_ALLOW_SEALING);
#endif
#ifdef MFD_HUGETLB
    add("MFD_HUGETLB"_s, MFD_HUGETLB);
#endif
#ifdef MFD_HUGE_SHIFT
    add("MFD_HUGE_SHIFT"_s, MFD_HUGE_SHIFT);
#endif
#ifdef MFD_HUGE_MASK
    add("MFD_HUGE_MASK"_s, MFD_HUGE_MASK);
#endif
#ifdef MFD_HUGE_64KB
    add("MFD_HUGE_64KB"_s, MFD_HUGE_64KB);
#endif
#ifdef MFD_HUGE_512KB
    add("MFD_HUGE_512KB"_s, MFD_HUGE_512KB);
#endif
#ifdef MFD_HUGE_1MB
    add("MFD_HUGE_1MB"_s, MFD_HUGE_1MB);
#endif
#ifdef MFD_HUGE_2MB
    add("MFD_HUGE_2MB"_s, MFD_HUGE_2MB);
#endif
#ifdef MFD_HUGE_8MB
    add("MFD_HUGE_8MB"_s, MFD_HUGE_8MB);
#endif
#ifdef MFD_HUGE_16MB
    add("MFD_HUGE_16MB"_s, MFD_HUGE_16MB);
#endif
#ifdef MFD_HUGE_32MB
    add("MFD_HUGE_32MB"_s, MFD_HUGE_32MB);
#endif
#ifdef MFD_HUGE_256MB
    add("MFD_HUGE_256MB"_s, MFD_HUGE_256MB);
#endif
#ifdef MFD_HUGE_512MB
    add("MFD_HUGE_512MB"_s, MFD_HUGE_512MB);
#endif
#ifdef MFD_HUGE_1GB
    add("MFD_HUGE_1GB"_s, MFD_HUGE_1GB);
#endif
#ifdef MFD_HUGE_2GB
    add("MFD_HUGE_2GB"_s, MFD_HUGE_2GB);
#endif
#ifdef MFD_HUGE_16GB
    add("MFD_HUGE_16GB"_s, MFD_HUGE_16GB);
#endif
#ifdef EFD_CLOEXEC
    add("EFD_CLOEXEC"_s, EFD_CLOEXEC);
#endif
#ifdef EFD_NONBLOCK
    add("EFD_NONBLOCK"_s, EFD_NONBLOCK);
#endif
#ifdef EFD_SEMAPHORE
    add("EFD_SEMAPHORE"_s, EFD_SEMAPHORE);
#endif
#ifdef COPYFILE_DATA
    add("_COPYFILE_DATA"_s, COPYFILE_DATA);
#endif
#ifdef COPYFILE_STAT
    add("_COPYFILE_STAT"_s, COPYFILE_STAT);
#endif
#ifdef COPYFILE_ACL
    add("_COPYFILE_ACL"_s, COPYFILE_ACL);
#endif
#ifdef COPYFILE_XATTR
    add("_COPYFILE_XATTR"_s, COPYFILE_XATTR);
#endif
}

JSValue newPosixPathconfNames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyDict* result = PyDict::create(globalObject);
    auto add = [&] (ASCIILiteral name, int value) { result->set(globalObject, jsString(vm, String(name)), jsNumber(value)); };
    UNUSED_VARIABLE(add);
#ifdef _PC_ABI_AIO_XFER_MAX
    add("PC_ABI_AIO_XFER_MAX"_s, _PC_ABI_AIO_XFER_MAX);
#endif
#ifdef _PC_ABI_ASYNC_IO
    add("PC_ABI_ASYNC_IO"_s, _PC_ABI_ASYNC_IO);
#endif
#ifdef _PC_ASYNC_IO
    add("PC_ASYNC_IO"_s, _PC_ASYNC_IO);
#endif
#ifdef _PC_CHOWN_RESTRICTED
    add("PC_CHOWN_RESTRICTED"_s, _PC_CHOWN_RESTRICTED);
#endif
#ifdef _PC_FILESIZEBITS
    add("PC_FILESIZEBITS"_s, _PC_FILESIZEBITS);
#endif
#ifdef _PC_LAST
    add("PC_LAST"_s, _PC_LAST);
#endif
#ifdef _PC_LINK_MAX
    add("PC_LINK_MAX"_s, _PC_LINK_MAX);
#endif
#ifdef _PC_MAX_CANON
    add("PC_MAX_CANON"_s, _PC_MAX_CANON);
#endif
#ifdef _PC_MAX_INPUT
    add("PC_MAX_INPUT"_s, _PC_MAX_INPUT);
#endif
#ifdef _PC_NAME_MAX
    add("PC_NAME_MAX"_s, _PC_NAME_MAX);
#endif
#ifdef _PC_NO_TRUNC
    add("PC_NO_TRUNC"_s, _PC_NO_TRUNC);
#endif
#ifdef _PC_PATH_MAX
    add("PC_PATH_MAX"_s, _PC_PATH_MAX);
#endif
#ifdef _PC_PIPE_BUF
    add("PC_PIPE_BUF"_s, _PC_PIPE_BUF);
#endif
#ifdef _PC_PRIO_IO
    add("PC_PRIO_IO"_s, _PC_PRIO_IO);
#endif
#ifdef _PC_SOCK_MAXBUF
    add("PC_SOCK_MAXBUF"_s, _PC_SOCK_MAXBUF);
#endif
#ifdef _PC_SYNC_IO
    add("PC_SYNC_IO"_s, _PC_SYNC_IO);
#endif
#ifdef _PC_VDISABLE
    add("PC_VDISABLE"_s, _PC_VDISABLE);
#endif
#ifdef _PC_ACL_ENABLED
    add("PC_ACL_ENABLED"_s, _PC_ACL_ENABLED);
#endif
#ifdef _PC_MIN_HOLE_SIZE
    add("PC_MIN_HOLE_SIZE"_s, _PC_MIN_HOLE_SIZE);
#endif
#ifdef _PC_ALLOC_SIZE_MIN
    add("PC_ALLOC_SIZE_MIN"_s, _PC_ALLOC_SIZE_MIN);
#endif
#ifdef _PC_REC_INCR_XFER_SIZE
    add("PC_REC_INCR_XFER_SIZE"_s, _PC_REC_INCR_XFER_SIZE);
#endif
#ifdef _PC_REC_MAX_XFER_SIZE
    add("PC_REC_MAX_XFER_SIZE"_s, _PC_REC_MAX_XFER_SIZE);
#endif
#ifdef _PC_REC_MIN_XFER_SIZE
    add("PC_REC_MIN_XFER_SIZE"_s, _PC_REC_MIN_XFER_SIZE);
#endif
#ifdef _PC_REC_XFER_ALIGN
    add("PC_REC_XFER_ALIGN"_s, _PC_REC_XFER_ALIGN);
#endif
#ifdef _PC_SYMLINK_MAX
    add("PC_SYMLINK_MAX"_s, _PC_SYMLINK_MAX);
#endif
#ifdef _PC_XATTR_ENABLED
    add("PC_XATTR_ENABLED"_s, _PC_XATTR_ENABLED);
#endif
#ifdef _PC_XATTR_EXISTS
    add("PC_XATTR_EXISTS"_s, _PC_XATTR_EXISTS);
#endif
#ifdef _PC_TIMESTAMP_RESOLUTION
    add("PC_TIMESTAMP_RESOLUTION"_s, _PC_TIMESTAMP_RESOLUTION);
#endif
    return result;
}

JSValue newPosixConfstrNames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyDict* result = PyDict::create(globalObject);
    auto add = [&] (ASCIILiteral name, int value) { result->set(globalObject, jsString(vm, String(name)), jsNumber(value)); };
    UNUSED_VARIABLE(add);
#ifdef _CS_ARCHITECTURE
    add("CS_ARCHITECTURE"_s, _CS_ARCHITECTURE);
#endif
#ifdef _CS_GNU_LIBC_VERSION
    add("CS_GNU_LIBC_VERSION"_s, _CS_GNU_LIBC_VERSION);
#endif
#ifdef _CS_GNU_LIBPTHREAD_VERSION
    add("CS_GNU_LIBPTHREAD_VERSION"_s, _CS_GNU_LIBPTHREAD_VERSION);
#endif
#ifdef _CS_HOSTNAME
    add("CS_HOSTNAME"_s, _CS_HOSTNAME);
#endif
#ifdef _CS_HW_PROVIDER
    add("CS_HW_PROVIDER"_s, _CS_HW_PROVIDER);
#endif
#ifdef _CS_HW_SERIAL
    add("CS_HW_SERIAL"_s, _CS_HW_SERIAL);
#endif
#ifdef _CS_INITTAB_NAME
    add("CS_INITTAB_NAME"_s, _CS_INITTAB_NAME);
#endif
#ifdef _CS_LFS64_CFLAGS
    add("CS_LFS64_CFLAGS"_s, _CS_LFS64_CFLAGS);
#endif
#ifdef _CS_LFS64_LDFLAGS
    add("CS_LFS64_LDFLAGS"_s, _CS_LFS64_LDFLAGS);
#endif
#ifdef _CS_LFS64_LIBS
    add("CS_LFS64_LIBS"_s, _CS_LFS64_LIBS);
#endif
#ifdef _CS_LFS64_LINTFLAGS
    add("CS_LFS64_LINTFLAGS"_s, _CS_LFS64_LINTFLAGS);
#endif
#ifdef _CS_LFS_CFLAGS
    add("CS_LFS_CFLAGS"_s, _CS_LFS_CFLAGS);
#endif
#ifdef _CS_LFS_LDFLAGS
    add("CS_LFS_LDFLAGS"_s, _CS_LFS_LDFLAGS);
#endif
#ifdef _CS_LFS_LIBS
    add("CS_LFS_LIBS"_s, _CS_LFS_LIBS);
#endif
#ifdef _CS_LFS_LINTFLAGS
    add("CS_LFS_LINTFLAGS"_s, _CS_LFS_LINTFLAGS);
#endif
#ifdef _CS_MACHINE
    add("CS_MACHINE"_s, _CS_MACHINE);
#endif
#ifdef _CS_PATH
    add("CS_PATH"_s, _CS_PATH);
#endif
#ifdef _CS_RELEASE
    add("CS_RELEASE"_s, _CS_RELEASE);
#endif
#ifdef _CS_SRPC_DOMAIN
    add("CS_SRPC_DOMAIN"_s, _CS_SRPC_DOMAIN);
#endif
#ifdef _CS_SYSNAME
    add("CS_SYSNAME"_s, _CS_SYSNAME);
#endif
#ifdef _CS_VERSION
    add("CS_VERSION"_s, _CS_VERSION);
#endif
#ifdef _CS_XBS5_ILP32_OFF32_CFLAGS
    add("CS_XBS5_ILP32_OFF32_CFLAGS"_s, _CS_XBS5_ILP32_OFF32_CFLAGS);
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LDFLAGS
    add("CS_XBS5_ILP32_OFF32_LDFLAGS"_s, _CS_XBS5_ILP32_OFF32_LDFLAGS);
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LIBS
    add("CS_XBS5_ILP32_OFF32_LIBS"_s, _CS_XBS5_ILP32_OFF32_LIBS);
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LINTFLAGS
    add("CS_XBS5_ILP32_OFF32_LINTFLAGS"_s, _CS_XBS5_ILP32_OFF32_LINTFLAGS);
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_CFLAGS
    add("CS_XBS5_ILP32_OFFBIG_CFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_CFLAGS);
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LDFLAGS
    add("CS_XBS5_ILP32_OFFBIG_LDFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_LDFLAGS);
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LIBS
    add("CS_XBS5_ILP32_OFFBIG_LIBS"_s, _CS_XBS5_ILP32_OFFBIG_LIBS);
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LINTFLAGS
    add("CS_XBS5_ILP32_OFFBIG_LINTFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_LINTFLAGS);
#endif
#ifdef _CS_XBS5_LP64_OFF64_CFLAGS
    add("CS_XBS5_LP64_OFF64_CFLAGS"_s, _CS_XBS5_LP64_OFF64_CFLAGS);
#endif
#ifdef _CS_XBS5_LP64_OFF64_LDFLAGS
    add("CS_XBS5_LP64_OFF64_LDFLAGS"_s, _CS_XBS5_LP64_OFF64_LDFLAGS);
#endif
#ifdef _CS_XBS5_LP64_OFF64_LIBS
    add("CS_XBS5_LP64_OFF64_LIBS"_s, _CS_XBS5_LP64_OFF64_LIBS);
#endif
#ifdef _CS_XBS5_LP64_OFF64_LINTFLAGS
    add("CS_XBS5_LP64_OFF64_LINTFLAGS"_s, _CS_XBS5_LP64_OFF64_LINTFLAGS);
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_CFLAGS
    add("CS_XBS5_LPBIG_OFFBIG_CFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_CFLAGS);
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LDFLAGS
    add("CS_XBS5_LPBIG_OFFBIG_LDFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_LDFLAGS);
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LIBS
    add("CS_XBS5_LPBIG_OFFBIG_LIBS"_s, _CS_XBS5_LPBIG_OFFBIG_LIBS);
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS
    add("CS_XBS5_LPBIG_OFFBIG_LINTFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS);
#endif
#ifdef _MIPS_CS_AVAIL_PROCESSORS
    add("MIPS_CS_AVAIL_PROCESSORS"_s, _MIPS_CS_AVAIL_PROCESSORS);
#endif
#ifdef _MIPS_CS_BASE
    add("MIPS_CS_BASE"_s, _MIPS_CS_BASE);
#endif
#ifdef _MIPS_CS_HOSTID
    add("MIPS_CS_HOSTID"_s, _MIPS_CS_HOSTID);
#endif
#ifdef _MIPS_CS_HW_NAME
    add("MIPS_CS_HW_NAME"_s, _MIPS_CS_HW_NAME);
#endif
#ifdef _MIPS_CS_NUM_PROCESSORS
    add("MIPS_CS_NUM_PROCESSORS"_s, _MIPS_CS_NUM_PROCESSORS);
#endif
#ifdef _MIPS_CS_OSREL_MAJ
    add("MIPS_CS_OSREL_MAJ"_s, _MIPS_CS_OSREL_MAJ);
#endif
#ifdef _MIPS_CS_OSREL_MIN
    add("MIPS_CS_OSREL_MIN"_s, _MIPS_CS_OSREL_MIN);
#endif
#ifdef _MIPS_CS_OSREL_PATCH
    add("MIPS_CS_OSREL_PATCH"_s, _MIPS_CS_OSREL_PATCH);
#endif
#ifdef _MIPS_CS_OS_NAME
    add("MIPS_CS_OS_NAME"_s, _MIPS_CS_OS_NAME);
#endif
#ifdef _MIPS_CS_OS_PROVIDER
    add("MIPS_CS_OS_PROVIDER"_s, _MIPS_CS_OS_PROVIDER);
#endif
#ifdef _MIPS_CS_PROCESSORS
    add("MIPS_CS_PROCESSORS"_s, _MIPS_CS_PROCESSORS);
#endif
#ifdef _MIPS_CS_SERIAL
    add("MIPS_CS_SERIAL"_s, _MIPS_CS_SERIAL);
#endif
#ifdef _MIPS_CS_VENDOR
    add("MIPS_CS_VENDOR"_s, _MIPS_CS_VENDOR);
#endif
    return result;
}

JSValue newPosixSysconfNames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyDict* result = PyDict::create(globalObject);
    auto add = [&] (ASCIILiteral name, int value) { result->set(globalObject, jsString(vm, String(name)), jsNumber(value)); };
    UNUSED_VARIABLE(add);
#ifdef _SC_2_CHAR_TERM
    add("SC_2_CHAR_TERM"_s, _SC_2_CHAR_TERM);
#endif
#ifdef _SC_2_C_BIND
    add("SC_2_C_BIND"_s, _SC_2_C_BIND);
#endif
#ifdef _SC_2_C_DEV
    add("SC_2_C_DEV"_s, _SC_2_C_DEV);
#endif
#ifdef _SC_2_C_VERSION
    add("SC_2_C_VERSION"_s, _SC_2_C_VERSION);
#endif
#ifdef _SC_2_FORT_DEV
    add("SC_2_FORT_DEV"_s, _SC_2_FORT_DEV);
#endif
#ifdef _SC_2_FORT_RUN
    add("SC_2_FORT_RUN"_s, _SC_2_FORT_RUN);
#endif
#ifdef _SC_2_LOCALEDEF
    add("SC_2_LOCALEDEF"_s, _SC_2_LOCALEDEF);
#endif
#ifdef _SC_2_SW_DEV
    add("SC_2_SW_DEV"_s, _SC_2_SW_DEV);
#endif
#ifdef _SC_2_UPE
    add("SC_2_UPE"_s, _SC_2_UPE);
#endif
#ifdef _SC_2_VERSION
    add("SC_2_VERSION"_s, _SC_2_VERSION);
#endif
#ifdef _SC_ABI_ASYNCHRONOUS_IO
    add("SC_ABI_ASYNCHRONOUS_IO"_s, _SC_ABI_ASYNCHRONOUS_IO);
#endif
#ifdef _SC_ACL
    add("SC_ACL"_s, _SC_ACL);
#endif
#ifdef _SC_AIO_LISTIO_MAX
    add("SC_AIO_LISTIO_MAX"_s, _SC_AIO_LISTIO_MAX);
#endif
#ifdef _SC_AIO_MAX
    add("SC_AIO_MAX"_s, _SC_AIO_MAX);
#endif
#ifdef _SC_AIO_PRIO_DELTA_MAX
    add("SC_AIO_PRIO_DELTA_MAX"_s, _SC_AIO_PRIO_DELTA_MAX);
#endif
#ifdef _SC_ARG_MAX
    add("SC_ARG_MAX"_s, _SC_ARG_MAX);
#endif
#ifdef _SC_ASYNCHRONOUS_IO
    add("SC_ASYNCHRONOUS_IO"_s, _SC_ASYNCHRONOUS_IO);
#endif
#ifdef _SC_ATEXIT_MAX
    add("SC_ATEXIT_MAX"_s, _SC_ATEXIT_MAX);
#endif
#ifdef _SC_AUDIT
    add("SC_AUDIT"_s, _SC_AUDIT);
#endif
#ifdef _SC_AVPHYS_PAGES
    add("SC_AVPHYS_PAGES"_s, _SC_AVPHYS_PAGES);
#endif
#ifdef _SC_BC_BASE_MAX
    add("SC_BC_BASE_MAX"_s, _SC_BC_BASE_MAX);
#endif
#ifdef _SC_BC_DIM_MAX
    add("SC_BC_DIM_MAX"_s, _SC_BC_DIM_MAX);
#endif
#ifdef _SC_BC_SCALE_MAX
    add("SC_BC_SCALE_MAX"_s, _SC_BC_SCALE_MAX);
#endif
#ifdef _SC_BC_STRING_MAX
    add("SC_BC_STRING_MAX"_s, _SC_BC_STRING_MAX);
#endif
#ifdef _SC_CAP
    add("SC_CAP"_s, _SC_CAP);
#endif
#ifdef _SC_CHARCLASS_NAME_MAX
    add("SC_CHARCLASS_NAME_MAX"_s, _SC_CHARCLASS_NAME_MAX);
#endif
#ifdef _SC_CHAR_BIT
    add("SC_CHAR_BIT"_s, _SC_CHAR_BIT);
#endif
#ifdef _SC_CHAR_MAX
    add("SC_CHAR_MAX"_s, _SC_CHAR_MAX);
#endif
#ifdef _SC_CHAR_MIN
    add("SC_CHAR_MIN"_s, _SC_CHAR_MIN);
#endif
#ifdef _SC_CHILD_MAX
    add("SC_CHILD_MAX"_s, _SC_CHILD_MAX);
#endif
#ifdef _SC_CLK_TCK
    add("SC_CLK_TCK"_s, _SC_CLK_TCK);
#endif
#ifdef _SC_COHER_BLKSZ
    add("SC_COHER_BLKSZ"_s, _SC_COHER_BLKSZ);
#endif
#ifdef _SC_COLL_WEIGHTS_MAX
    add("SC_COLL_WEIGHTS_MAX"_s, _SC_COLL_WEIGHTS_MAX);
#endif
#ifdef _SC_DCACHE_ASSOC
    add("SC_DCACHE_ASSOC"_s, _SC_DCACHE_ASSOC);
#endif
#ifdef _SC_DCACHE_BLKSZ
    add("SC_DCACHE_BLKSZ"_s, _SC_DCACHE_BLKSZ);
#endif
#ifdef _SC_DCACHE_LINESZ
    add("SC_DCACHE_LINESZ"_s, _SC_DCACHE_LINESZ);
#endif
#ifdef _SC_DCACHE_SZ
    add("SC_DCACHE_SZ"_s, _SC_DCACHE_SZ);
#endif
#ifdef _SC_DCACHE_TBLKSZ
    add("SC_DCACHE_TBLKSZ"_s, _SC_DCACHE_TBLKSZ);
#endif
#ifdef _SC_DELAYTIMER_MAX
    add("SC_DELAYTIMER_MAX"_s, _SC_DELAYTIMER_MAX);
#endif
#ifdef _SC_EQUIV_CLASS_MAX
    add("SC_EQUIV_CLASS_MAX"_s, _SC_EQUIV_CLASS_MAX);
#endif
#ifdef _SC_EXPR_NEST_MAX
    add("SC_EXPR_NEST_MAX"_s, _SC_EXPR_NEST_MAX);
#endif
#ifdef _SC_FSYNC
    add("SC_FSYNC"_s, _SC_FSYNC);
#endif
#ifdef _SC_GETGR_R_SIZE_MAX
    add("SC_GETGR_R_SIZE_MAX"_s, _SC_GETGR_R_SIZE_MAX);
#endif
#ifdef _SC_GETPW_R_SIZE_MAX
    add("SC_GETPW_R_SIZE_MAX"_s, _SC_GETPW_R_SIZE_MAX);
#endif
#ifdef _SC_ICACHE_ASSOC
    add("SC_ICACHE_ASSOC"_s, _SC_ICACHE_ASSOC);
#endif
#ifdef _SC_ICACHE_BLKSZ
    add("SC_ICACHE_BLKSZ"_s, _SC_ICACHE_BLKSZ);
#endif
#ifdef _SC_ICACHE_LINESZ
    add("SC_ICACHE_LINESZ"_s, _SC_ICACHE_LINESZ);
#endif
#ifdef _SC_ICACHE_SZ
    add("SC_ICACHE_SZ"_s, _SC_ICACHE_SZ);
#endif
#ifdef _SC_INF
    add("SC_INF"_s, _SC_INF);
#endif
#ifdef _SC_INT_MAX
    add("SC_INT_MAX"_s, _SC_INT_MAX);
#endif
#ifdef _SC_INT_MIN
    add("SC_INT_MIN"_s, _SC_INT_MIN);
#endif
#ifdef _SC_IOV_MAX
    add("SC_IOV_MAX"_s, _SC_IOV_MAX);
#endif
#ifdef _SC_IP_SECOPTS
    add("SC_IP_SECOPTS"_s, _SC_IP_SECOPTS);
#endif
#ifdef _SC_JOB_CONTROL
    add("SC_JOB_CONTROL"_s, _SC_JOB_CONTROL);
#endif
#ifdef _SC_KERN_POINTERS
    add("SC_KERN_POINTERS"_s, _SC_KERN_POINTERS);
#endif
#ifdef _SC_KERN_SIM
    add("SC_KERN_SIM"_s, _SC_KERN_SIM);
#endif
#ifdef _SC_LINE_MAX
    add("SC_LINE_MAX"_s, _SC_LINE_MAX);
#endif
#ifdef _SC_LOGIN_NAME_MAX
    add("SC_LOGIN_NAME_MAX"_s, _SC_LOGIN_NAME_MAX);
#endif
#ifdef _SC_LOGNAME_MAX
    add("SC_LOGNAME_MAX"_s, _SC_LOGNAME_MAX);
#endif
#ifdef _SC_LONG_BIT
    add("SC_LONG_BIT"_s, _SC_LONG_BIT);
#endif
#ifdef _SC_MAC
    add("SC_MAC"_s, _SC_MAC);
#endif
#ifdef _SC_MAPPED_FILES
    add("SC_MAPPED_FILES"_s, _SC_MAPPED_FILES);
#endif
#ifdef _SC_MAXPID
    add("SC_MAXPID"_s, _SC_MAXPID);
#endif
#ifdef _SC_MB_LEN_MAX
    add("SC_MB_LEN_MAX"_s, _SC_MB_LEN_MAX);
#endif
#ifdef _SC_MEMLOCK
    add("SC_MEMLOCK"_s, _SC_MEMLOCK);
#endif
#ifdef _SC_MEMLOCK_RANGE
    add("SC_MEMLOCK_RANGE"_s, _SC_MEMLOCK_RANGE);
#endif
#ifdef _SC_MEMORY_PROTECTION
    add("SC_MEMORY_PROTECTION"_s, _SC_MEMORY_PROTECTION);
#endif
#ifdef _SC_MESSAGE_PASSING
    add("SC_MESSAGE_PASSING"_s, _SC_MESSAGE_PASSING);
#endif
#ifdef _SC_MMAP_FIXED_ALIGNMENT
    add("SC_MMAP_FIXED_ALIGNMENT"_s, _SC_MMAP_FIXED_ALIGNMENT);
#endif
#ifdef _SC_MQ_OPEN_MAX
    add("SC_MQ_OPEN_MAX"_s, _SC_MQ_OPEN_MAX);
#endif
#ifdef _SC_MQ_PRIO_MAX
    add("SC_MQ_PRIO_MAX"_s, _SC_MQ_PRIO_MAX);
#endif
#ifdef _SC_NACLS_MAX
    add("SC_NACLS_MAX"_s, _SC_NACLS_MAX);
#endif
#ifdef _SC_NGROUPS_MAX
    add("SC_NGROUPS_MAX"_s, _SC_NGROUPS_MAX);
#endif
#ifdef _SC_NL_ARGMAX
    add("SC_NL_ARGMAX"_s, _SC_NL_ARGMAX);
#endif
#ifdef _SC_NL_LANGMAX
    add("SC_NL_LANGMAX"_s, _SC_NL_LANGMAX);
#endif
#ifdef _SC_NL_MSGMAX
    add("SC_NL_MSGMAX"_s, _SC_NL_MSGMAX);
#endif
#ifdef _SC_NL_NMAX
    add("SC_NL_NMAX"_s, _SC_NL_NMAX);
#endif
#ifdef _SC_NL_SETMAX
    add("SC_NL_SETMAX"_s, _SC_NL_SETMAX);
#endif
#ifdef _SC_NL_TEXTMAX
    add("SC_NL_TEXTMAX"_s, _SC_NL_TEXTMAX);
#endif
#ifdef _SC_NPROCESSORS_CONF
    add("SC_NPROCESSORS_CONF"_s, _SC_NPROCESSORS_CONF);
#endif
#ifdef _SC_NPROCESSORS_ONLN
    add("SC_NPROCESSORS_ONLN"_s, _SC_NPROCESSORS_ONLN);
#endif
#ifdef _SC_NPROC_CONF
    add("SC_NPROC_CONF"_s, _SC_NPROC_CONF);
#endif
#ifdef _SC_NPROC_ONLN
    add("SC_NPROC_ONLN"_s, _SC_NPROC_ONLN);
#endif
#ifdef _SC_NZERO
    add("SC_NZERO"_s, _SC_NZERO);
#endif
#ifdef _SC_OPEN_MAX
    add("SC_OPEN_MAX"_s, _SC_OPEN_MAX);
#endif
#ifdef _SC_PAGESIZE
    add("SC_PAGESIZE"_s, _SC_PAGESIZE);
#endif
#ifdef _SC_PAGE_SIZE
    add("SC_PAGE_SIZE"_s, _SC_PAGE_SIZE);
#endif
#ifdef _SC_AIX_REALMEM
    add("SC_AIX_REALMEM"_s, _SC_AIX_REALMEM);
#endif
#ifdef _SC_PASS_MAX
    add("SC_PASS_MAX"_s, _SC_PASS_MAX);
#endif
#ifdef _SC_PHYS_PAGES
    add("SC_PHYS_PAGES"_s, _SC_PHYS_PAGES);
#endif
#ifdef _SC_PII
    add("SC_PII"_s, _SC_PII);
#endif
#ifdef _SC_PII_INTERNET
    add("SC_PII_INTERNET"_s, _SC_PII_INTERNET);
#endif
#ifdef _SC_PII_INTERNET_DGRAM
    add("SC_PII_INTERNET_DGRAM"_s, _SC_PII_INTERNET_DGRAM);
#endif
#ifdef _SC_PII_INTERNET_STREAM
    add("SC_PII_INTERNET_STREAM"_s, _SC_PII_INTERNET_STREAM);
#endif
#ifdef _SC_PII_OSI
    add("SC_PII_OSI"_s, _SC_PII_OSI);
#endif
#ifdef _SC_PII_OSI_CLTS
    add("SC_PII_OSI_CLTS"_s, _SC_PII_OSI_CLTS);
#endif
#ifdef _SC_PII_OSI_COTS
    add("SC_PII_OSI_COTS"_s, _SC_PII_OSI_COTS);
#endif
#ifdef _SC_PII_OSI_M
    add("SC_PII_OSI_M"_s, _SC_PII_OSI_M);
#endif
#ifdef _SC_PII_SOCKET
    add("SC_PII_SOCKET"_s, _SC_PII_SOCKET);
#endif
#ifdef _SC_PII_XTI
    add("SC_PII_XTI"_s, _SC_PII_XTI);
#endif
#ifdef _SC_POLL
    add("SC_POLL"_s, _SC_POLL);
#endif
#ifdef _SC_PRIORITIZED_IO
    add("SC_PRIORITIZED_IO"_s, _SC_PRIORITIZED_IO);
#endif
#ifdef _SC_PRIORITY_SCHEDULING
    add("SC_PRIORITY_SCHEDULING"_s, _SC_PRIORITY_SCHEDULING);
#endif
#ifdef _SC_REALTIME_SIGNALS
    add("SC_REALTIME_SIGNALS"_s, _SC_REALTIME_SIGNALS);
#endif
#ifdef _SC_RE_DUP_MAX
    add("SC_RE_DUP_MAX"_s, _SC_RE_DUP_MAX);
#endif
#ifdef _SC_RTSIG_MAX
    add("SC_RTSIG_MAX"_s, _SC_RTSIG_MAX);
#endif
#ifdef _SC_SAVED_IDS
    add("SC_SAVED_IDS"_s, _SC_SAVED_IDS);
#endif
#ifdef _SC_SCHAR_MAX
    add("SC_SCHAR_MAX"_s, _SC_SCHAR_MAX);
#endif
#ifdef _SC_SCHAR_MIN
    add("SC_SCHAR_MIN"_s, _SC_SCHAR_MIN);
#endif
#ifdef _SC_SELECT
    add("SC_SELECT"_s, _SC_SELECT);
#endif
#ifdef _SC_SEMAPHORES
    add("SC_SEMAPHORES"_s, _SC_SEMAPHORES);
#endif
#ifdef _SC_SEM_NSEMS_MAX
    add("SC_SEM_NSEMS_MAX"_s, _SC_SEM_NSEMS_MAX);
#endif
#ifdef _SC_SEM_VALUE_MAX
    add("SC_SEM_VALUE_MAX"_s, _SC_SEM_VALUE_MAX);
#endif
#ifdef _SC_SHARED_MEMORY_OBJECTS
    add("SC_SHARED_MEMORY_OBJECTS"_s, _SC_SHARED_MEMORY_OBJECTS);
#endif
#ifdef _SC_SHRT_MAX
    add("SC_SHRT_MAX"_s, _SC_SHRT_MAX);
#endif
#ifdef _SC_SHRT_MIN
    add("SC_SHRT_MIN"_s, _SC_SHRT_MIN);
#endif
#ifdef _SC_SIGQUEUE_MAX
    add("SC_SIGQUEUE_MAX"_s, _SC_SIGQUEUE_MAX);
#endif
#ifdef _SC_SIGRT_MAX
    add("SC_SIGRT_MAX"_s, _SC_SIGRT_MAX);
#endif
#ifdef _SC_SIGRT_MIN
    add("SC_SIGRT_MIN"_s, _SC_SIGRT_MIN);
#endif
#ifdef _SC_SOFTPOWER
    add("SC_SOFTPOWER"_s, _SC_SOFTPOWER);
#endif
#ifdef _SC_SPLIT_CACHE
    add("SC_SPLIT_CACHE"_s, _SC_SPLIT_CACHE);
#endif
#ifdef _SC_SSIZE_MAX
    add("SC_SSIZE_MAX"_s, _SC_SSIZE_MAX);
#endif
#ifdef _SC_STACK_PROT
    add("SC_STACK_PROT"_s, _SC_STACK_PROT);
#endif
#ifdef _SC_STREAM_MAX
    add("SC_STREAM_MAX"_s, _SC_STREAM_MAX);
#endif
#ifdef _SC_SYNCHRONIZED_IO
    add("SC_SYNCHRONIZED_IO"_s, _SC_SYNCHRONIZED_IO);
#endif
#ifdef _SC_THREADS
    add("SC_THREADS"_s, _SC_THREADS);
#endif
#ifdef _SC_THREAD_ATTR_STACKADDR
    add("SC_THREAD_ATTR_STACKADDR"_s, _SC_THREAD_ATTR_STACKADDR);
#endif
#ifdef _SC_THREAD_ATTR_STACKSIZE
    add("SC_THREAD_ATTR_STACKSIZE"_s, _SC_THREAD_ATTR_STACKSIZE);
#endif
#ifdef _SC_THREAD_DESTRUCTOR_ITERATIONS
    add("SC_THREAD_DESTRUCTOR_ITERATIONS"_s, _SC_THREAD_DESTRUCTOR_ITERATIONS);
#endif
#ifdef _SC_THREAD_KEYS_MAX
    add("SC_THREAD_KEYS_MAX"_s, _SC_THREAD_KEYS_MAX);
#endif
#ifdef _SC_THREAD_PRIORITY_SCHEDULING
    add("SC_THREAD_PRIORITY_SCHEDULING"_s, _SC_THREAD_PRIORITY_SCHEDULING);
#endif
#ifdef _SC_THREAD_PRIO_INHERIT
    add("SC_THREAD_PRIO_INHERIT"_s, _SC_THREAD_PRIO_INHERIT);
#endif
#ifdef _SC_THREAD_PRIO_PROTECT
    add("SC_THREAD_PRIO_PROTECT"_s, _SC_THREAD_PRIO_PROTECT);
#endif
#ifdef _SC_THREAD_PROCESS_SHARED
    add("SC_THREAD_PROCESS_SHARED"_s, _SC_THREAD_PROCESS_SHARED);
#endif
#ifdef _SC_THREAD_SAFE_FUNCTIONS
    add("SC_THREAD_SAFE_FUNCTIONS"_s, _SC_THREAD_SAFE_FUNCTIONS);
#endif
#ifdef _SC_THREAD_STACK_MIN
    add("SC_THREAD_STACK_MIN"_s, _SC_THREAD_STACK_MIN);
#endif
#ifdef _SC_THREAD_THREADS_MAX
    add("SC_THREAD_THREADS_MAX"_s, _SC_THREAD_THREADS_MAX);
#endif
#ifdef _SC_TIMERS
    add("SC_TIMERS"_s, _SC_TIMERS);
#endif
#ifdef _SC_TIMER_MAX
    add("SC_TIMER_MAX"_s, _SC_TIMER_MAX);
#endif
#ifdef _SC_TTY_NAME_MAX
    add("SC_TTY_NAME_MAX"_s, _SC_TTY_NAME_MAX);
#endif
#ifdef _SC_TZNAME_MAX
    add("SC_TZNAME_MAX"_s, _SC_TZNAME_MAX);
#endif
#ifdef _SC_T_IOV_MAX
    add("SC_T_IOV_MAX"_s, _SC_T_IOV_MAX);
#endif
#ifdef _SC_UCHAR_MAX
    add("SC_UCHAR_MAX"_s, _SC_UCHAR_MAX);
#endif
#ifdef _SC_UINT_MAX
    add("SC_UINT_MAX"_s, _SC_UINT_MAX);
#endif
#ifdef _SC_UIO_MAXIOV
    add("SC_UIO_MAXIOV"_s, _SC_UIO_MAXIOV);
#endif
#ifdef _SC_ULONG_MAX
    add("SC_ULONG_MAX"_s, _SC_ULONG_MAX);
#endif
#ifdef _SC_USHRT_MAX
    add("SC_USHRT_MAX"_s, _SC_USHRT_MAX);
#endif
#ifdef _SC_VERSION
    add("SC_VERSION"_s, _SC_VERSION);
#endif
#ifdef _SC_WORD_BIT
    add("SC_WORD_BIT"_s, _SC_WORD_BIT);
#endif
#ifdef _SC_XBS5_ILP32_OFF32
    add("SC_XBS5_ILP32_OFF32"_s, _SC_XBS5_ILP32_OFF32);
#endif
#ifdef _SC_XBS5_ILP32_OFFBIG
    add("SC_XBS5_ILP32_OFFBIG"_s, _SC_XBS5_ILP32_OFFBIG);
#endif
#ifdef _SC_XBS5_LP64_OFF64
    add("SC_XBS5_LP64_OFF64"_s, _SC_XBS5_LP64_OFF64);
#endif
#ifdef _SC_XBS5_LPBIG_OFFBIG
    add("SC_XBS5_LPBIG_OFFBIG"_s, _SC_XBS5_LPBIG_OFFBIG);
#endif
#ifdef _SC_XOPEN_CRYPT
    add("SC_XOPEN_CRYPT"_s, _SC_XOPEN_CRYPT);
#endif
#ifdef _SC_XOPEN_ENH_I18N
    add("SC_XOPEN_ENH_I18N"_s, _SC_XOPEN_ENH_I18N);
#endif
#ifdef _SC_XOPEN_LEGACY
    add("SC_XOPEN_LEGACY"_s, _SC_XOPEN_LEGACY);
#endif
#ifdef _SC_XOPEN_REALTIME
    add("SC_XOPEN_REALTIME"_s, _SC_XOPEN_REALTIME);
#endif
#ifdef _SC_XOPEN_REALTIME_THREADS
    add("SC_XOPEN_REALTIME_THREADS"_s, _SC_XOPEN_REALTIME_THREADS);
#endif
#ifdef _SC_XOPEN_SHM
    add("SC_XOPEN_SHM"_s, _SC_XOPEN_SHM);
#endif
#ifdef _SC_XOPEN_UNIX
    add("SC_XOPEN_UNIX"_s, _SC_XOPEN_UNIX);
#endif
#ifdef _SC_XOPEN_VERSION
    add("SC_XOPEN_VERSION"_s, _SC_XOPEN_VERSION);
#endif
#ifdef _SC_XOPEN_XCU_VERSION
    add("SC_XOPEN_XCU_VERSION"_s, _SC_XOPEN_XCU_VERSION);
#endif
#ifdef _SC_XOPEN_XPG2
    add("SC_XOPEN_XPG2"_s, _SC_XOPEN_XPG2);
#endif
#ifdef _SC_XOPEN_XPG3
    add("SC_XOPEN_XPG3"_s, _SC_XOPEN_XPG3);
#endif
#ifdef _SC_XOPEN_XPG4
    add("SC_XOPEN_XPG4"_s, _SC_XOPEN_XPG4);
#endif
#ifdef _SC_MINSIGSTKSZ
    add("SC_MINSIGSTKSZ"_s, _SC_MINSIGSTKSZ);
#endif
    return result;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
