/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <dlfcn.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sysexits.h>

#if OS(LINUX)
#include <linux/memfd.h>
#include <linux/sched.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/timerfd.h>
#include <sys/xattr.h>
#if __has_include(<sys/pidfd.h>)
#include <sys/pidfd.h>
#endif
#endif

#if OS(DARWIN)
#include <copyfile.h>
#include <sys/socket.h>
#endif

namespace JSC {
namespace Python {

// The numbers the module posix has names for: whichever of them the system's headers define.

static constexpr PosixConstant posixConstants[] = {
    { "F_OK"_s, F_OK },
    { "R_OK"_s, R_OK },
    { "W_OK"_s, W_OK },
    { "X_OK"_s, X_OK },
#ifdef NGROUPS_MAX
    { "NGROUPS_MAX"_s, NGROUPS_MAX },
#endif
#ifdef TMP_MAX
    { "TMP_MAX"_s, TMP_MAX },
#endif
#ifdef WCONTINUED
    { "WCONTINUED"_s, WCONTINUED },
#endif
#ifdef WNOHANG
    { "WNOHANG"_s, WNOHANG },
    { "P_WAIT"_s, 0 }, // For spawnv(): see posixSpawnFamily().
    { "P_NOWAIT"_s, 1 },
    { "P_NOWAITO"_s, 1 },
#endif
#ifdef WUNTRACED
    { "WUNTRACED"_s, WUNTRACED },
#endif
    { "O_RDONLY"_s, O_RDONLY },
    { "O_WRONLY"_s, O_WRONLY },
    { "O_RDWR"_s, O_RDWR },
#ifdef O_NDELAY
    { "O_NDELAY"_s, O_NDELAY },
#endif
#ifdef O_NONBLOCK
    { "O_NONBLOCK"_s, O_NONBLOCK },
#endif
#ifdef O_APPEND
    { "O_APPEND"_s, O_APPEND },
#endif
#ifdef O_DSYNC
    { "O_DSYNC"_s, O_DSYNC },
#endif
#ifdef O_RSYNC
    { "O_RSYNC"_s, O_RSYNC },
#endif
#ifdef O_SYNC
    { "O_SYNC"_s, O_SYNC },
#endif
#ifdef O_NOCTTY
    { "O_NOCTTY"_s, O_NOCTTY },
#endif
#ifdef O_CREAT
    { "O_CREAT"_s, O_CREAT },
#endif
#ifdef O_EXCL
    { "O_EXCL"_s, O_EXCL },
#endif
#ifdef O_TRUNC
    { "O_TRUNC"_s, O_TRUNC },
#endif
#ifdef O_LARGEFILE
    { "O_LARGEFILE"_s, O_LARGEFILE },
#endif
#ifdef O_SHLOCK
    { "O_SHLOCK"_s, O_SHLOCK },
#endif
#ifdef O_EXLOCK
    { "O_EXLOCK"_s, O_EXLOCK },
#endif
#ifdef O_EXEC
    { "O_EXEC"_s, O_EXEC },
#endif
#ifdef O_SEARCH
    { "O_SEARCH"_s, O_SEARCH },
#endif
#ifdef O_PATH
    { "O_PATH"_s, O_PATH },
#endif
#ifdef O_TTY_INIT
    { "O_TTY_INIT"_s, O_TTY_INIT },
#endif
#ifdef O_TMPFILE
    { "O_TMPFILE"_s, O_TMPFILE },
#endif
#ifdef PRIO_PROCESS
    { "PRIO_PROCESS"_s, PRIO_PROCESS },
#endif
#ifdef PRIO_PGRP
    { "PRIO_PGRP"_s, PRIO_PGRP },
#endif
#ifdef PRIO_USER
    { "PRIO_USER"_s, PRIO_USER },
#endif
#ifdef PRIO_DARWIN_THREAD
    { "PRIO_DARWIN_THREAD"_s, PRIO_DARWIN_THREAD },
#endif
#ifdef PRIO_DARWIN_PROCESS
    { "PRIO_DARWIN_PROCESS"_s, PRIO_DARWIN_PROCESS },
#endif
#ifdef PRIO_DARWIN_BG
    { "PRIO_DARWIN_BG"_s, PRIO_DARWIN_BG },
#endif
#ifdef PRIO_DARWIN_NONUI
    { "PRIO_DARWIN_NONUI"_s, PRIO_DARWIN_NONUI },
#endif
#ifdef O_CLOEXEC
    { "O_CLOEXEC"_s, O_CLOEXEC },
#endif
#ifdef O_ACCMODE
    { "O_ACCMODE"_s, O_ACCMODE },
#endif
#ifdef O_EVTONLY
    { "O_EVTONLY"_s, O_EVTONLY },
#endif
#ifdef O_FSYNC
    { "O_FSYNC"_s, O_FSYNC },
#endif
#ifdef O_SYMLINK
    { "O_SYMLINK"_s, O_SYMLINK },
#endif
#ifdef SEEK_HOLE
    { "SEEK_HOLE"_s, SEEK_HOLE },
#endif
#ifdef SEEK_DATA
    { "SEEK_DATA"_s, SEEK_DATA },
#endif
#ifdef O_ASYNC
    { "O_ASYNC"_s, O_ASYNC },
#endif
#ifdef O_DIRECT
    { "O_DIRECT"_s, O_DIRECT },
#endif
#ifdef O_DIRECTORY
    { "O_DIRECTORY"_s, O_DIRECTORY },
#endif
#ifdef O_NOFOLLOW
    { "O_NOFOLLOW"_s, O_NOFOLLOW },
#endif
#ifdef O_NOFOLLOW_ANY
    { "O_NOFOLLOW_ANY"_s, O_NOFOLLOW_ANY },
#endif
#ifdef O_NOATIME
    { "O_NOATIME"_s, O_NOATIME },
#endif
#ifdef EX_OK
    { "EX_OK"_s, EX_OK },
#endif
#ifdef EX_USAGE
    { "EX_USAGE"_s, EX_USAGE },
#endif
#ifdef EX_DATAERR
    { "EX_DATAERR"_s, EX_DATAERR },
#endif
#ifdef EX_NOINPUT
    { "EX_NOINPUT"_s, EX_NOINPUT },
#endif
#ifdef EX_NOUSER
    { "EX_NOUSER"_s, EX_NOUSER },
#endif
#ifdef EX_NOHOST
    { "EX_NOHOST"_s, EX_NOHOST },
#endif
#ifdef EX_UNAVAILABLE
    { "EX_UNAVAILABLE"_s, EX_UNAVAILABLE },
#endif
#ifdef EX_SOFTWARE
    { "EX_SOFTWARE"_s, EX_SOFTWARE },
#endif
#ifdef EX_OSERR
    { "EX_OSERR"_s, EX_OSERR },
#endif
#ifdef EX_OSFILE
    { "EX_OSFILE"_s, EX_OSFILE },
#endif
#ifdef EX_CANTCREAT
    { "EX_CANTCREAT"_s, EX_CANTCREAT },
#endif
#ifdef EX_IOERR
    { "EX_IOERR"_s, EX_IOERR },
#endif
#ifdef EX_TEMPFAIL
    { "EX_TEMPFAIL"_s, EX_TEMPFAIL },
#endif
#ifdef EX_PROTOCOL
    { "EX_PROTOCOL"_s, EX_PROTOCOL },
#endif
#ifdef EX_NOPERM
    { "EX_NOPERM"_s, EX_NOPERM },
#endif
#ifdef EX_CONFIG
    { "EX_CONFIG"_s, EX_CONFIG },
#endif
#ifdef EX_NOTFOUND
    { "EX_NOTFOUND"_s, EX_NOTFOUND },
#endif
#ifdef ST_RDONLY
    { "ST_RDONLY"_s, ST_RDONLY },
#endif
#ifdef ST_NOSUID
    { "ST_NOSUID"_s, ST_NOSUID },
#endif
#ifdef ST_NODEV
    { "ST_NODEV"_s, ST_NODEV },
#endif
#ifdef ST_NOEXEC
    { "ST_NOEXEC"_s, ST_NOEXEC },
#endif
#ifdef ST_SYNCHRONOUS
    { "ST_SYNCHRONOUS"_s, ST_SYNCHRONOUS },
#endif
#ifdef ST_MANDLOCK
    { "ST_MANDLOCK"_s, ST_MANDLOCK },
#endif
#ifdef ST_WRITE
    { "ST_WRITE"_s, ST_WRITE },
#endif
#ifdef ST_APPEND
    { "ST_APPEND"_s, ST_APPEND },
#endif
#ifdef ST_NOATIME
    { "ST_NOATIME"_s, ST_NOATIME },
#endif
#ifdef ST_NODIRATIME
    { "ST_NODIRATIME"_s, ST_NODIRATIME },
#endif
#ifdef ST_RELATIME
    { "ST_RELATIME"_s, ST_RELATIME },
#endif
#ifdef SF_NODISKIO
    { "SF_NODISKIO"_s, SF_NODISKIO },
#endif
#ifdef SF_MNOWAIT
    { "SF_MNOWAIT"_s, SF_MNOWAIT },
#endif
#ifdef SF_SYNC
    { "SF_SYNC"_s, SF_SYNC },
#endif
#ifdef SF_NOCACHE
    { "SF_NOCACHE"_s, SF_NOCACHE },
#endif
#ifdef TFD_NONBLOCK
    { "TFD_NONBLOCK"_s, TFD_NONBLOCK },
#endif
#ifdef TFD_CLOEXEC
    { "TFD_CLOEXEC"_s, TFD_CLOEXEC },
#endif
#ifdef TFD_TIMER_ABSTIME
    { "TFD_TIMER_ABSTIME"_s, TFD_TIMER_ABSTIME },
#endif
#ifdef TFD_TIMER_CANCEL_ON_SET
    { "TFD_TIMER_CANCEL_ON_SET"_s, TFD_TIMER_CANCEL_ON_SET },
#endif
#ifdef POSIX_FADV_NORMAL
    { "POSIX_FADV_NORMAL"_s, POSIX_FADV_NORMAL },
#endif
#ifdef POSIX_FADV_SEQUENTIAL
    { "POSIX_FADV_SEQUENTIAL"_s, POSIX_FADV_SEQUENTIAL },
#endif
#ifdef POSIX_FADV_RANDOM
    { "POSIX_FADV_RANDOM"_s, POSIX_FADV_RANDOM },
#endif
#ifdef POSIX_FADV_NOREUSE
    { "POSIX_FADV_NOREUSE"_s, POSIX_FADV_NOREUSE },
#endif
#ifdef POSIX_FADV_WILLNEED
    { "POSIX_FADV_WILLNEED"_s, POSIX_FADV_WILLNEED },
#endif
#ifdef POSIX_FADV_DONTNEED
    { "POSIX_FADV_DONTNEED"_s, POSIX_FADV_DONTNEED },
#endif
    { "P_PID"_s, P_PID },
    { "P_PGID"_s, P_PGID },
    { "P_ALL"_s, P_ALL },
#ifdef PIDFD_NONBLOCK
    { "PIDFD_NONBLOCK"_s, PIDFD_NONBLOCK },
#endif
#ifdef WEXITED
    { "WEXITED"_s, WEXITED },
#endif
#ifdef WNOWAIT
    { "WNOWAIT"_s, WNOWAIT },
#endif
#ifdef WSTOPPED
    { "WSTOPPED"_s, WSTOPPED },
#endif
#ifdef CLD_EXITED
    { "CLD_EXITED"_s, CLD_EXITED },
#endif
#ifdef CLD_KILLED
    { "CLD_KILLED"_s, CLD_KILLED },
#endif
#ifdef CLD_DUMPED
    { "CLD_DUMPED"_s, CLD_DUMPED },
#endif
#ifdef CLD_TRAPPED
    { "CLD_TRAPPED"_s, CLD_TRAPPED },
#endif
#ifdef CLD_STOPPED
    { "CLD_STOPPED"_s, CLD_STOPPED },
#endif
#ifdef CLD_CONTINUED
    { "CLD_CONTINUED"_s, CLD_CONTINUED },
#endif
#ifdef F_LOCK
    { "F_LOCK"_s, F_LOCK },
#endif
#ifdef F_TLOCK
    { "F_TLOCK"_s, F_TLOCK },
#endif
#ifdef F_ULOCK
    { "F_ULOCK"_s, F_ULOCK },
#endif
#ifdef F_TEST
    { "F_TEST"_s, F_TEST },
#endif
#ifdef SCHED_OTHER
    { "SCHED_OTHER"_s, SCHED_OTHER },
#endif
#ifdef SCHED_DEADLINE
    { "SCHED_DEADLINE"_s, SCHED_DEADLINE },
#endif
#ifdef SCHED_FIFO
    { "SCHED_FIFO"_s, SCHED_FIFO },
#endif
#ifdef SCHED_NORMAL
    { "SCHED_NORMAL"_s, SCHED_NORMAL },
#endif
#ifdef SCHED_RR
    { "SCHED_RR"_s, SCHED_RR },
#endif
#ifdef SCHED_BATCH
    { "SCHED_BATCH"_s, SCHED_BATCH },
#endif
#ifdef SCHED_IDLE
    { "SCHED_IDLE"_s, SCHED_IDLE },
#endif
#ifdef SCHED_RESET_ON_FORK
    { "SCHED_RESET_ON_FORK"_s, SCHED_RESET_ON_FORK },
#endif
#ifdef CLONE_FS
    { "CLONE_FS"_s, CLONE_FS },
#endif
#ifdef CLONE_FILES
    { "CLONE_FILES"_s, CLONE_FILES },
#endif
#ifdef CLONE_NEWNS
    { "CLONE_NEWNS"_s, CLONE_NEWNS },
#endif
#ifdef CLONE_NEWCGROUP
    { "CLONE_NEWCGROUP"_s, CLONE_NEWCGROUP },
#endif
#ifdef CLONE_NEWUTS
    { "CLONE_NEWUTS"_s, CLONE_NEWUTS },
#endif
#ifdef CLONE_NEWIPC
    { "CLONE_NEWIPC"_s, CLONE_NEWIPC },
#endif
#ifdef CLONE_NEWUSER
    { "CLONE_NEWUSER"_s, CLONE_NEWUSER },
#endif
#ifdef CLONE_NEWPID
    { "CLONE_NEWPID"_s, CLONE_NEWPID },
#endif
#ifdef CLONE_NEWNET
    { "CLONE_NEWNET"_s, CLONE_NEWNET },
#endif
#ifdef CLONE_NEWTIME
    { "CLONE_NEWTIME"_s, CLONE_NEWTIME },
#endif
#ifdef CLONE_SYSVSEM
    { "CLONE_SYSVSEM"_s, CLONE_SYSVSEM },
#endif
#ifdef CLONE_THREAD
    { "CLONE_THREAD"_s, CLONE_THREAD },
#endif
#ifdef CLONE_SIGHAND
    { "CLONE_SIGHAND"_s, CLONE_SIGHAND },
#endif
#ifdef CLONE_VM
    { "CLONE_VM"_s, CLONE_VM },
#endif
#ifdef XATTR_CREATE
    { "XATTR_CREATE"_s, XATTR_CREATE },
#endif
#ifdef XATTR_REPLACE
    { "XATTR_REPLACE"_s, XATTR_REPLACE },
#endif
#ifdef RTLD_LAZY
    { "RTLD_LAZY"_s, RTLD_LAZY },
#endif
#ifdef RTLD_NOW
    { "RTLD_NOW"_s, RTLD_NOW },
#endif
#ifdef RTLD_GLOBAL
    { "RTLD_GLOBAL"_s, RTLD_GLOBAL },
#endif
#ifdef RTLD_LOCAL
    { "RTLD_LOCAL"_s, RTLD_LOCAL },
#endif
#ifdef RTLD_NODELETE
    { "RTLD_NODELETE"_s, RTLD_NODELETE },
#endif
#ifdef RTLD_NOLOAD
    { "RTLD_NOLOAD"_s, RTLD_NOLOAD },
#endif
#ifdef RTLD_DEEPBIND
    { "RTLD_DEEPBIND"_s, RTLD_DEEPBIND },
#endif
#ifdef GRND_RANDOM
    { "GRND_RANDOM"_s, GRND_RANDOM },
#endif
#ifdef GRND_NONBLOCK
    { "GRND_NONBLOCK"_s, GRND_NONBLOCK },
#endif
#ifdef MFD_CLOEXEC
    { "MFD_CLOEXEC"_s, MFD_CLOEXEC },
#endif
#ifdef MFD_ALLOW_SEALING
    { "MFD_ALLOW_SEALING"_s, MFD_ALLOW_SEALING },
#endif
#ifdef MFD_HUGETLB
    { "MFD_HUGETLB"_s, MFD_HUGETLB },
#endif
#ifdef MFD_HUGE_SHIFT
    { "MFD_HUGE_SHIFT"_s, MFD_HUGE_SHIFT },
#endif
#ifdef MFD_HUGE_MASK
    { "MFD_HUGE_MASK"_s, MFD_HUGE_MASK },
#endif
#ifdef MFD_HUGE_64KB
    { "MFD_HUGE_64KB"_s, MFD_HUGE_64KB },
#endif
#ifdef MFD_HUGE_512KB
    { "MFD_HUGE_512KB"_s, MFD_HUGE_512KB },
#endif
#ifdef MFD_HUGE_1MB
    { "MFD_HUGE_1MB"_s, MFD_HUGE_1MB },
#endif
#ifdef MFD_HUGE_2MB
    { "MFD_HUGE_2MB"_s, MFD_HUGE_2MB },
#endif
#ifdef MFD_HUGE_8MB
    { "MFD_HUGE_8MB"_s, MFD_HUGE_8MB },
#endif
#ifdef MFD_HUGE_16MB
    { "MFD_HUGE_16MB"_s, MFD_HUGE_16MB },
#endif
#ifdef MFD_HUGE_32MB
    { "MFD_HUGE_32MB"_s, MFD_HUGE_32MB },
#endif
#ifdef MFD_HUGE_256MB
    { "MFD_HUGE_256MB"_s, MFD_HUGE_256MB },
#endif
#ifdef MFD_HUGE_512MB
    { "MFD_HUGE_512MB"_s, MFD_HUGE_512MB },
#endif
#ifdef MFD_HUGE_1GB
    { "MFD_HUGE_1GB"_s, MFD_HUGE_1GB },
#endif
#ifdef MFD_HUGE_2GB
    { "MFD_HUGE_2GB"_s, MFD_HUGE_2GB },
#endif
#ifdef MFD_HUGE_16GB
    { "MFD_HUGE_16GB"_s, MFD_HUGE_16GB },
#endif
#ifdef EFD_CLOEXEC
    { "EFD_CLOEXEC"_s, EFD_CLOEXEC },
#endif
#ifdef EFD_NONBLOCK
    { "EFD_NONBLOCK"_s, EFD_NONBLOCK },
#endif
#ifdef EFD_SEMAPHORE
    { "EFD_SEMAPHORE"_s, EFD_SEMAPHORE },
#endif
#ifdef AT_NO_AUTOMOUNT
    { "AT_NO_AUTOMOUNT"_s, AT_NO_AUTOMOUNT },
#endif
#ifdef STATX_TYPE
    { "STATX_TYPE"_s, STATX_TYPE },
#endif
#ifdef STATX_MODE
    { "STATX_MODE"_s, STATX_MODE },
#endif
#ifdef STATX_NLINK
    { "STATX_NLINK"_s, STATX_NLINK },
#endif
#ifdef STATX_UID
    { "STATX_UID"_s, STATX_UID },
#endif
#ifdef STATX_GID
    { "STATX_GID"_s, STATX_GID },
#endif
#ifdef STATX_ATIME
    { "STATX_ATIME"_s, STATX_ATIME },
#endif
#ifdef STATX_MTIME
    { "STATX_MTIME"_s, STATX_MTIME },
#endif
#ifdef STATX_CTIME
    { "STATX_CTIME"_s, STATX_CTIME },
#endif
#ifdef STATX_INO
    { "STATX_INO"_s, STATX_INO },
#endif
#ifdef STATX_SIZE
    { "STATX_SIZE"_s, STATX_SIZE },
#endif
#ifdef STATX_BLOCKS
    { "STATX_BLOCKS"_s, STATX_BLOCKS },
#endif
#ifdef STATX_BASIC_STATS
    { "STATX_BASIC_STATS"_s, STATX_BASIC_STATS },
#endif
#ifdef STATX_BTIME
    { "STATX_BTIME"_s, STATX_BTIME },
#endif
#ifdef STATX_BASIC_STATS
    // Whatever the headers of the system know: os.statx() has its own description of the structure, with the members these ask for.
    { "STATX_MNT_ID"_s, 0x1000 },
    { "STATX_DIOALIGN"_s, 0x2000 },
    { "STATX_MNT_ID_UNIQUE"_s, 0x4000 },
    { "STATX_SUBVOL"_s, 0x8000 },
    { "STATX_WRITE_ATOMIC"_s, 0x10000 },
    { "STATX_DIO_READ_ALIGN"_s, 0x20000 },
#endif
#ifdef AT_STATX_SYNC_AS_STAT
    { "AT_STATX_SYNC_AS_STAT"_s, AT_STATX_SYNC_AS_STAT },
#endif
#ifdef AT_STATX_FORCE_SYNC
    { "AT_STATX_FORCE_SYNC"_s, AT_STATX_FORCE_SYNC },
#endif
#ifdef AT_STATX_DONT_SYNC
    { "AT_STATX_DONT_SYNC"_s, AT_STATX_DONT_SYNC },
#endif
#ifdef RWF_DSYNC
    { "RWF_DSYNC"_s, RWF_DSYNC },
#endif
#ifdef RWF_HIPRI
    { "RWF_HIPRI"_s, RWF_HIPRI },
#endif
#ifdef RWF_SYNC
    { "RWF_SYNC"_s, RWF_SYNC },
#endif
#ifdef RWF_NOWAIT
    { "RWF_NOWAIT"_s, RWF_NOWAIT },
#endif
#ifdef RWF_DONTCACHE
    { "RWF_DONTCACHE"_s, RWF_DONTCACHE },
#endif
#ifdef RWF_ATOMIC
    { "RWF_ATOMIC"_s, RWF_ATOMIC },
#endif
#ifdef RWF_APPEND
    { "RWF_APPEND"_s, RWF_APPEND },
#endif
#ifdef SPLICE_F_MOVE
    { "SPLICE_F_MOVE"_s, SPLICE_F_MOVE },
#endif
#ifdef SPLICE_F_NONBLOCK
    { "SPLICE_F_NONBLOCK"_s, SPLICE_F_NONBLOCK },
#endif
#ifdef SPLICE_F_MORE
    { "SPLICE_F_MORE"_s, SPLICE_F_MORE },
#endif
#if OS(LINUX)
    // Of the kernel's headers, <linux/wait.h> and <linux/limits.h>, rather than the C library's.
    { "P_PIDFD"_s, 3 },
    { "XATTR_SIZE_MAX"_s, 65536 },
#endif
#if OS(DARWIN)
    { "_COPYFILE_DATA"_s, COPYFILE_DATA },
    { "_COPYFILE_STAT"_s, COPYFILE_STAT },
    { "_COPYFILE_ACL"_s, COPYFILE_ACL },
    { "_COPYFILE_XATTR"_s, COPYFILE_XATTR },
#endif
};

// The names pathconf(), confstr() and sysconf() take.

static constexpr PosixConstant posixPathconfNames[] = {
#ifdef _PC_ABI_AIO_XFER_MAX
    { "PC_ABI_AIO_XFER_MAX"_s, _PC_ABI_AIO_XFER_MAX },
#endif
#ifdef _PC_ABI_ASYNC_IO
    { "PC_ABI_ASYNC_IO"_s, _PC_ABI_ASYNC_IO },
#endif
#ifdef _PC_ASYNC_IO
    { "PC_ASYNC_IO"_s, _PC_ASYNC_IO },
#endif
#ifdef _PC_CHOWN_RESTRICTED
    { "PC_CHOWN_RESTRICTED"_s, _PC_CHOWN_RESTRICTED },
#endif
#ifdef _PC_FILESIZEBITS
    { "PC_FILESIZEBITS"_s, _PC_FILESIZEBITS },
#endif
#ifdef _PC_LAST
    { "PC_LAST"_s, _PC_LAST },
#endif
#ifdef _PC_LINK_MAX
    { "PC_LINK_MAX"_s, _PC_LINK_MAX },
#endif
#ifdef _PC_MAX_CANON
    { "PC_MAX_CANON"_s, _PC_MAX_CANON },
#endif
#ifdef _PC_MAX_INPUT
    { "PC_MAX_INPUT"_s, _PC_MAX_INPUT },
#endif
#ifdef _PC_NAME_MAX
    { "PC_NAME_MAX"_s, _PC_NAME_MAX },
#endif
#ifdef _PC_NO_TRUNC
    { "PC_NO_TRUNC"_s, _PC_NO_TRUNC },
#endif
#ifdef _PC_PATH_MAX
    { "PC_PATH_MAX"_s, _PC_PATH_MAX },
#endif
#ifdef _PC_PIPE_BUF
    { "PC_PIPE_BUF"_s, _PC_PIPE_BUF },
#endif
#ifdef _PC_PRIO_IO
    { "PC_PRIO_IO"_s, _PC_PRIO_IO },
#endif
#ifdef _PC_SOCK_MAXBUF
    { "PC_SOCK_MAXBUF"_s, _PC_SOCK_MAXBUF },
#endif
#ifdef _PC_SYNC_IO
    { "PC_SYNC_IO"_s, _PC_SYNC_IO },
#endif
#ifdef _PC_VDISABLE
    { "PC_VDISABLE"_s, _PC_VDISABLE },
#endif
#ifdef _PC_ACL_ENABLED
    { "PC_ACL_ENABLED"_s, _PC_ACL_ENABLED },
#endif
#ifdef _PC_MIN_HOLE_SIZE
    { "PC_MIN_HOLE_SIZE"_s, _PC_MIN_HOLE_SIZE },
#endif
#ifdef _PC_ALLOC_SIZE_MIN
    { "PC_ALLOC_SIZE_MIN"_s, _PC_ALLOC_SIZE_MIN },
#endif
#ifdef _PC_REC_INCR_XFER_SIZE
    { "PC_REC_INCR_XFER_SIZE"_s, _PC_REC_INCR_XFER_SIZE },
#endif
#ifdef _PC_REC_MAX_XFER_SIZE
    { "PC_REC_MAX_XFER_SIZE"_s, _PC_REC_MAX_XFER_SIZE },
#endif
#ifdef _PC_REC_MIN_XFER_SIZE
    { "PC_REC_MIN_XFER_SIZE"_s, _PC_REC_MIN_XFER_SIZE },
#endif
#ifdef _PC_REC_XFER_ALIGN
    { "PC_REC_XFER_ALIGN"_s, _PC_REC_XFER_ALIGN },
#endif
#ifdef _PC_SYMLINK_MAX
    { "PC_SYMLINK_MAX"_s, _PC_SYMLINK_MAX },
#endif
#ifdef _PC_XATTR_ENABLED
    { "PC_XATTR_ENABLED"_s, _PC_XATTR_ENABLED },
#endif
#ifdef _PC_XATTR_EXISTS
    { "PC_XATTR_EXISTS"_s, _PC_XATTR_EXISTS },
#endif
#ifdef _PC_TIMESTAMP_RESOLUTION
    { "PC_TIMESTAMP_RESOLUTION"_s, _PC_TIMESTAMP_RESOLUTION },
#endif
};

#if !OS(ANDROID)
static constexpr PosixConstant posixConfstrNames[] = {
#ifdef _CS_ARCHITECTURE
    { "CS_ARCHITECTURE"_s, _CS_ARCHITECTURE },
#endif
#ifdef _CS_GNU_LIBC_VERSION
    { "CS_GNU_LIBC_VERSION"_s, _CS_GNU_LIBC_VERSION },
#endif
#ifdef _CS_GNU_LIBPTHREAD_VERSION
    { "CS_GNU_LIBPTHREAD_VERSION"_s, _CS_GNU_LIBPTHREAD_VERSION },
#endif
#ifdef _CS_HOSTNAME
    { "CS_HOSTNAME"_s, _CS_HOSTNAME },
#endif
#ifdef _CS_HW_PROVIDER
    { "CS_HW_PROVIDER"_s, _CS_HW_PROVIDER },
#endif
#ifdef _CS_HW_SERIAL
    { "CS_HW_SERIAL"_s, _CS_HW_SERIAL },
#endif
#ifdef _CS_INITTAB_NAME
    { "CS_INITTAB_NAME"_s, _CS_INITTAB_NAME },
#endif
#ifdef _CS_LFS64_CFLAGS
    { "CS_LFS64_CFLAGS"_s, _CS_LFS64_CFLAGS },
#endif
#ifdef _CS_LFS64_LDFLAGS
    { "CS_LFS64_LDFLAGS"_s, _CS_LFS64_LDFLAGS },
#endif
#ifdef _CS_LFS64_LIBS
    { "CS_LFS64_LIBS"_s, _CS_LFS64_LIBS },
#endif
#ifdef _CS_LFS64_LINTFLAGS
    { "CS_LFS64_LINTFLAGS"_s, _CS_LFS64_LINTFLAGS },
#endif
#ifdef _CS_LFS_CFLAGS
    { "CS_LFS_CFLAGS"_s, _CS_LFS_CFLAGS },
#endif
#ifdef _CS_LFS_LDFLAGS
    { "CS_LFS_LDFLAGS"_s, _CS_LFS_LDFLAGS },
#endif
#ifdef _CS_LFS_LIBS
    { "CS_LFS_LIBS"_s, _CS_LFS_LIBS },
#endif
#ifdef _CS_LFS_LINTFLAGS
    { "CS_LFS_LINTFLAGS"_s, _CS_LFS_LINTFLAGS },
#endif
#ifdef _CS_MACHINE
    { "CS_MACHINE"_s, _CS_MACHINE },
#endif
#ifdef _CS_PATH
    { "CS_PATH"_s, _CS_PATH },
#endif
#ifdef _CS_RELEASE
    { "CS_RELEASE"_s, _CS_RELEASE },
#endif
#ifdef _CS_SRPC_DOMAIN
    { "CS_SRPC_DOMAIN"_s, _CS_SRPC_DOMAIN },
#endif
#ifdef _CS_SYSNAME
    { "CS_SYSNAME"_s, _CS_SYSNAME },
#endif
#ifdef _CS_VERSION
    { "CS_VERSION"_s, _CS_VERSION },
#endif
#ifdef _CS_XBS5_ILP32_OFF32_CFLAGS
    { "CS_XBS5_ILP32_OFF32_CFLAGS"_s, _CS_XBS5_ILP32_OFF32_CFLAGS },
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LDFLAGS
    { "CS_XBS5_ILP32_OFF32_LDFLAGS"_s, _CS_XBS5_ILP32_OFF32_LDFLAGS },
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LIBS
    { "CS_XBS5_ILP32_OFF32_LIBS"_s, _CS_XBS5_ILP32_OFF32_LIBS },
#endif
#ifdef _CS_XBS5_ILP32_OFF32_LINTFLAGS
    { "CS_XBS5_ILP32_OFF32_LINTFLAGS"_s, _CS_XBS5_ILP32_OFF32_LINTFLAGS },
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_CFLAGS
    { "CS_XBS5_ILP32_OFFBIG_CFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_CFLAGS },
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LDFLAGS
    { "CS_XBS5_ILP32_OFFBIG_LDFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_LDFLAGS },
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LIBS
    { "CS_XBS5_ILP32_OFFBIG_LIBS"_s, _CS_XBS5_ILP32_OFFBIG_LIBS },
#endif
#ifdef _CS_XBS5_ILP32_OFFBIG_LINTFLAGS
    { "CS_XBS5_ILP32_OFFBIG_LINTFLAGS"_s, _CS_XBS5_ILP32_OFFBIG_LINTFLAGS },
#endif
#ifdef _CS_XBS5_LP64_OFF64_CFLAGS
    { "CS_XBS5_LP64_OFF64_CFLAGS"_s, _CS_XBS5_LP64_OFF64_CFLAGS },
#endif
#ifdef _CS_XBS5_LP64_OFF64_LDFLAGS
    { "CS_XBS5_LP64_OFF64_LDFLAGS"_s, _CS_XBS5_LP64_OFF64_LDFLAGS },
#endif
#ifdef _CS_XBS5_LP64_OFF64_LIBS
    { "CS_XBS5_LP64_OFF64_LIBS"_s, _CS_XBS5_LP64_OFF64_LIBS },
#endif
#ifdef _CS_XBS5_LP64_OFF64_LINTFLAGS
    { "CS_XBS5_LP64_OFF64_LINTFLAGS"_s, _CS_XBS5_LP64_OFF64_LINTFLAGS },
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_CFLAGS
    { "CS_XBS5_LPBIG_OFFBIG_CFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_CFLAGS },
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LDFLAGS
    { "CS_XBS5_LPBIG_OFFBIG_LDFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_LDFLAGS },
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LIBS
    { "CS_XBS5_LPBIG_OFFBIG_LIBS"_s, _CS_XBS5_LPBIG_OFFBIG_LIBS },
#endif
#ifdef _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS
    { "CS_XBS5_LPBIG_OFFBIG_LINTFLAGS"_s, _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS },
#endif
#ifdef _MIPS_CS_AVAIL_PROCESSORS
    { "MIPS_CS_AVAIL_PROCESSORS"_s, _MIPS_CS_AVAIL_PROCESSORS },
#endif
#ifdef _MIPS_CS_BASE
    { "MIPS_CS_BASE"_s, _MIPS_CS_BASE },
#endif
#ifdef _MIPS_CS_HOSTID
    { "MIPS_CS_HOSTID"_s, _MIPS_CS_HOSTID },
#endif
#ifdef _MIPS_CS_HW_NAME
    { "MIPS_CS_HW_NAME"_s, _MIPS_CS_HW_NAME },
#endif
#ifdef _MIPS_CS_NUM_PROCESSORS
    { "MIPS_CS_NUM_PROCESSORS"_s, _MIPS_CS_NUM_PROCESSORS },
#endif
#ifdef _MIPS_CS_OSREL_MAJ
    { "MIPS_CS_OSREL_MAJ"_s, _MIPS_CS_OSREL_MAJ },
#endif
#ifdef _MIPS_CS_OSREL_MIN
    { "MIPS_CS_OSREL_MIN"_s, _MIPS_CS_OSREL_MIN },
#endif
#ifdef _MIPS_CS_OSREL_PATCH
    { "MIPS_CS_OSREL_PATCH"_s, _MIPS_CS_OSREL_PATCH },
#endif
#ifdef _MIPS_CS_OS_NAME
    { "MIPS_CS_OS_NAME"_s, _MIPS_CS_OS_NAME },
#endif
#ifdef _MIPS_CS_OS_PROVIDER
    { "MIPS_CS_OS_PROVIDER"_s, _MIPS_CS_OS_PROVIDER },
#endif
#ifdef _MIPS_CS_PROCESSORS
    { "MIPS_CS_PROCESSORS"_s, _MIPS_CS_PROCESSORS },
#endif
#ifdef _MIPS_CS_SERIAL
    { "MIPS_CS_SERIAL"_s, _MIPS_CS_SERIAL },
#endif
#ifdef _MIPS_CS_VENDOR
    { "MIPS_CS_VENDOR"_s, _MIPS_CS_VENDOR },
#endif
};
#endif

static constexpr PosixConstant posixSysconfNames[] = {
#ifdef _SC_2_CHAR_TERM
    { "SC_2_CHAR_TERM"_s, _SC_2_CHAR_TERM },
#endif
#ifdef _SC_2_C_BIND
    { "SC_2_C_BIND"_s, _SC_2_C_BIND },
#endif
#ifdef _SC_2_C_DEV
    { "SC_2_C_DEV"_s, _SC_2_C_DEV },
#endif
#ifdef _SC_2_C_VERSION
    { "SC_2_C_VERSION"_s, _SC_2_C_VERSION },
#endif
#ifdef _SC_2_FORT_DEV
    { "SC_2_FORT_DEV"_s, _SC_2_FORT_DEV },
#endif
#ifdef _SC_2_FORT_RUN
    { "SC_2_FORT_RUN"_s, _SC_2_FORT_RUN },
#endif
#ifdef _SC_2_LOCALEDEF
    { "SC_2_LOCALEDEF"_s, _SC_2_LOCALEDEF },
#endif
#ifdef _SC_2_SW_DEV
    { "SC_2_SW_DEV"_s, _SC_2_SW_DEV },
#endif
#ifdef _SC_2_UPE
    { "SC_2_UPE"_s, _SC_2_UPE },
#endif
#ifdef _SC_2_VERSION
    { "SC_2_VERSION"_s, _SC_2_VERSION },
#endif
#ifdef _SC_ABI_ASYNCHRONOUS_IO
    { "SC_ABI_ASYNCHRONOUS_IO"_s, _SC_ABI_ASYNCHRONOUS_IO },
#endif
#ifdef _SC_ACL
    { "SC_ACL"_s, _SC_ACL },
#endif
#ifdef _SC_AIO_LISTIO_MAX
    { "SC_AIO_LISTIO_MAX"_s, _SC_AIO_LISTIO_MAX },
#endif
#ifdef _SC_AIO_MAX
    { "SC_AIO_MAX"_s, _SC_AIO_MAX },
#endif
#ifdef _SC_AIO_PRIO_DELTA_MAX
    { "SC_AIO_PRIO_DELTA_MAX"_s, _SC_AIO_PRIO_DELTA_MAX },
#endif
#ifdef _SC_ARG_MAX
    { "SC_ARG_MAX"_s, _SC_ARG_MAX },
#endif
#ifdef _SC_ASYNCHRONOUS_IO
    { "SC_ASYNCHRONOUS_IO"_s, _SC_ASYNCHRONOUS_IO },
#endif
#ifdef _SC_ATEXIT_MAX
    { "SC_ATEXIT_MAX"_s, _SC_ATEXIT_MAX },
#endif
#ifdef _SC_AUDIT
    { "SC_AUDIT"_s, _SC_AUDIT },
#endif
#ifdef _SC_AVPHYS_PAGES
    { "SC_AVPHYS_PAGES"_s, _SC_AVPHYS_PAGES },
#endif
#ifdef _SC_BC_BASE_MAX
    { "SC_BC_BASE_MAX"_s, _SC_BC_BASE_MAX },
#endif
#ifdef _SC_BC_DIM_MAX
    { "SC_BC_DIM_MAX"_s, _SC_BC_DIM_MAX },
#endif
#ifdef _SC_BC_SCALE_MAX
    { "SC_BC_SCALE_MAX"_s, _SC_BC_SCALE_MAX },
#endif
#ifdef _SC_BC_STRING_MAX
    { "SC_BC_STRING_MAX"_s, _SC_BC_STRING_MAX },
#endif
#ifdef _SC_CAP
    { "SC_CAP"_s, _SC_CAP },
#endif
#ifdef _SC_CHARCLASS_NAME_MAX
    { "SC_CHARCLASS_NAME_MAX"_s, _SC_CHARCLASS_NAME_MAX },
#endif
#ifdef _SC_CHAR_BIT
    { "SC_CHAR_BIT"_s, _SC_CHAR_BIT },
#endif
#ifdef _SC_CHAR_MAX
    { "SC_CHAR_MAX"_s, _SC_CHAR_MAX },
#endif
#ifdef _SC_CHAR_MIN
    { "SC_CHAR_MIN"_s, _SC_CHAR_MIN },
#endif
#ifdef _SC_CHILD_MAX
    { "SC_CHILD_MAX"_s, _SC_CHILD_MAX },
#endif
#ifdef _SC_CLK_TCK
    { "SC_CLK_TCK"_s, _SC_CLK_TCK },
#endif
#ifdef _SC_COHER_BLKSZ
    { "SC_COHER_BLKSZ"_s, _SC_COHER_BLKSZ },
#endif
#ifdef _SC_COLL_WEIGHTS_MAX
    { "SC_COLL_WEIGHTS_MAX"_s, _SC_COLL_WEIGHTS_MAX },
#endif
#ifdef _SC_DCACHE_ASSOC
    { "SC_DCACHE_ASSOC"_s, _SC_DCACHE_ASSOC },
#endif
#ifdef _SC_DCACHE_BLKSZ
    { "SC_DCACHE_BLKSZ"_s, _SC_DCACHE_BLKSZ },
#endif
#ifdef _SC_DCACHE_LINESZ
    { "SC_DCACHE_LINESZ"_s, _SC_DCACHE_LINESZ },
#endif
#ifdef _SC_DCACHE_SZ
    { "SC_DCACHE_SZ"_s, _SC_DCACHE_SZ },
#endif
#ifdef _SC_DCACHE_TBLKSZ
    { "SC_DCACHE_TBLKSZ"_s, _SC_DCACHE_TBLKSZ },
#endif
#ifdef _SC_DELAYTIMER_MAX
    { "SC_DELAYTIMER_MAX"_s, _SC_DELAYTIMER_MAX },
#endif
#ifdef _SC_EQUIV_CLASS_MAX
    { "SC_EQUIV_CLASS_MAX"_s, _SC_EQUIV_CLASS_MAX },
#endif
#ifdef _SC_EXPR_NEST_MAX
    { "SC_EXPR_NEST_MAX"_s, _SC_EXPR_NEST_MAX },
#endif
#ifdef _SC_FSYNC
    { "SC_FSYNC"_s, _SC_FSYNC },
#endif
#ifdef _SC_GETGR_R_SIZE_MAX
    { "SC_GETGR_R_SIZE_MAX"_s, _SC_GETGR_R_SIZE_MAX },
#endif
#ifdef _SC_GETPW_R_SIZE_MAX
    { "SC_GETPW_R_SIZE_MAX"_s, _SC_GETPW_R_SIZE_MAX },
#endif
#ifdef _SC_ICACHE_ASSOC
    { "SC_ICACHE_ASSOC"_s, _SC_ICACHE_ASSOC },
#endif
#ifdef _SC_ICACHE_BLKSZ
    { "SC_ICACHE_BLKSZ"_s, _SC_ICACHE_BLKSZ },
#endif
#ifdef _SC_ICACHE_LINESZ
    { "SC_ICACHE_LINESZ"_s, _SC_ICACHE_LINESZ },
#endif
#ifdef _SC_ICACHE_SZ
    { "SC_ICACHE_SZ"_s, _SC_ICACHE_SZ },
#endif
#ifdef _SC_INF
    { "SC_INF"_s, _SC_INF },
#endif
#ifdef _SC_INT_MAX
    { "SC_INT_MAX"_s, _SC_INT_MAX },
#endif
#ifdef _SC_INT_MIN
    { "SC_INT_MIN"_s, _SC_INT_MIN },
#endif
#ifdef _SC_IOV_MAX
    { "SC_IOV_MAX"_s, _SC_IOV_MAX },
#endif
#ifdef _SC_IP_SECOPTS
    { "SC_IP_SECOPTS"_s, _SC_IP_SECOPTS },
#endif
#ifdef _SC_JOB_CONTROL
    { "SC_JOB_CONTROL"_s, _SC_JOB_CONTROL },
#endif
#ifdef _SC_KERN_POINTERS
    { "SC_KERN_POINTERS"_s, _SC_KERN_POINTERS },
#endif
#ifdef _SC_KERN_SIM
    { "SC_KERN_SIM"_s, _SC_KERN_SIM },
#endif
#ifdef _SC_LINE_MAX
    { "SC_LINE_MAX"_s, _SC_LINE_MAX },
#endif
#ifdef _SC_LOGIN_NAME_MAX
    { "SC_LOGIN_NAME_MAX"_s, _SC_LOGIN_NAME_MAX },
#endif
#ifdef _SC_LOGNAME_MAX
    { "SC_LOGNAME_MAX"_s, _SC_LOGNAME_MAX },
#endif
#ifdef _SC_LONG_BIT
    { "SC_LONG_BIT"_s, _SC_LONG_BIT },
#endif
#ifdef _SC_MAC
    { "SC_MAC"_s, _SC_MAC },
#endif
#ifdef _SC_MAPPED_FILES
    { "SC_MAPPED_FILES"_s, _SC_MAPPED_FILES },
#endif
#ifdef _SC_MAXPID
    { "SC_MAXPID"_s, _SC_MAXPID },
#endif
#ifdef _SC_MB_LEN_MAX
    { "SC_MB_LEN_MAX"_s, _SC_MB_LEN_MAX },
#endif
#ifdef _SC_MEMLOCK
    { "SC_MEMLOCK"_s, _SC_MEMLOCK },
#endif
#ifdef _SC_MEMLOCK_RANGE
    { "SC_MEMLOCK_RANGE"_s, _SC_MEMLOCK_RANGE },
#endif
#ifdef _SC_MEMORY_PROTECTION
    { "SC_MEMORY_PROTECTION"_s, _SC_MEMORY_PROTECTION },
#endif
#ifdef _SC_MESSAGE_PASSING
    { "SC_MESSAGE_PASSING"_s, _SC_MESSAGE_PASSING },
#endif
#ifdef _SC_MMAP_FIXED_ALIGNMENT
    { "SC_MMAP_FIXED_ALIGNMENT"_s, _SC_MMAP_FIXED_ALIGNMENT },
#endif
#ifdef _SC_MQ_OPEN_MAX
    { "SC_MQ_OPEN_MAX"_s, _SC_MQ_OPEN_MAX },
#endif
#ifdef _SC_MQ_PRIO_MAX
    { "SC_MQ_PRIO_MAX"_s, _SC_MQ_PRIO_MAX },
#endif
#ifdef _SC_NACLS_MAX
    { "SC_NACLS_MAX"_s, _SC_NACLS_MAX },
#endif
#ifdef _SC_NGROUPS_MAX
    { "SC_NGROUPS_MAX"_s, _SC_NGROUPS_MAX },
#endif
#ifdef _SC_NL_ARGMAX
    { "SC_NL_ARGMAX"_s, _SC_NL_ARGMAX },
#endif
#ifdef _SC_NL_LANGMAX
    { "SC_NL_LANGMAX"_s, _SC_NL_LANGMAX },
#endif
#ifdef _SC_NL_MSGMAX
    { "SC_NL_MSGMAX"_s, _SC_NL_MSGMAX },
#endif
#ifdef _SC_NL_NMAX
    { "SC_NL_NMAX"_s, _SC_NL_NMAX },
#endif
#ifdef _SC_NL_SETMAX
    { "SC_NL_SETMAX"_s, _SC_NL_SETMAX },
#endif
#ifdef _SC_NL_TEXTMAX
    { "SC_NL_TEXTMAX"_s, _SC_NL_TEXTMAX },
#endif
#ifdef _SC_NPROCESSORS_CONF
    { "SC_NPROCESSORS_CONF"_s, _SC_NPROCESSORS_CONF },
#endif
#ifdef _SC_NPROCESSORS_ONLN
    { "SC_NPROCESSORS_ONLN"_s, _SC_NPROCESSORS_ONLN },
#endif
#ifdef _SC_NPROC_CONF
    { "SC_NPROC_CONF"_s, _SC_NPROC_CONF },
#endif
#ifdef _SC_NPROC_ONLN
    { "SC_NPROC_ONLN"_s, _SC_NPROC_ONLN },
#endif
#ifdef _SC_NZERO
    { "SC_NZERO"_s, _SC_NZERO },
#endif
#ifdef _SC_OPEN_MAX
    { "SC_OPEN_MAX"_s, _SC_OPEN_MAX },
#endif
#ifdef _SC_PAGESIZE
    { "SC_PAGESIZE"_s, _SC_PAGESIZE },
#endif
#ifdef _SC_PAGE_SIZE
    { "SC_PAGE_SIZE"_s, _SC_PAGE_SIZE },
#endif
#ifdef _SC_AIX_REALMEM
    { "SC_AIX_REALMEM"_s, _SC_AIX_REALMEM },
#endif
#ifdef _SC_PASS_MAX
    { "SC_PASS_MAX"_s, _SC_PASS_MAX },
#endif
#ifdef _SC_PHYS_PAGES
    { "SC_PHYS_PAGES"_s, _SC_PHYS_PAGES },
#endif
#ifdef _SC_PII
    { "SC_PII"_s, _SC_PII },
#endif
#ifdef _SC_PII_INTERNET
    { "SC_PII_INTERNET"_s, _SC_PII_INTERNET },
#endif
#ifdef _SC_PII_INTERNET_DGRAM
    { "SC_PII_INTERNET_DGRAM"_s, _SC_PII_INTERNET_DGRAM },
#endif
#ifdef _SC_PII_INTERNET_STREAM
    { "SC_PII_INTERNET_STREAM"_s, _SC_PII_INTERNET_STREAM },
#endif
#ifdef _SC_PII_OSI
    { "SC_PII_OSI"_s, _SC_PII_OSI },
#endif
#ifdef _SC_PII_OSI_CLTS
    { "SC_PII_OSI_CLTS"_s, _SC_PII_OSI_CLTS },
#endif
#ifdef _SC_PII_OSI_COTS
    { "SC_PII_OSI_COTS"_s, _SC_PII_OSI_COTS },
#endif
#ifdef _SC_PII_OSI_M
    { "SC_PII_OSI_M"_s, _SC_PII_OSI_M },
#endif
#ifdef _SC_PII_SOCKET
    { "SC_PII_SOCKET"_s, _SC_PII_SOCKET },
#endif
#ifdef _SC_PII_XTI
    { "SC_PII_XTI"_s, _SC_PII_XTI },
#endif
#ifdef _SC_POLL
    { "SC_POLL"_s, _SC_POLL },
#endif
#ifdef _SC_PRIORITIZED_IO
    { "SC_PRIORITIZED_IO"_s, _SC_PRIORITIZED_IO },
#endif
#ifdef _SC_PRIORITY_SCHEDULING
    { "SC_PRIORITY_SCHEDULING"_s, _SC_PRIORITY_SCHEDULING },
#endif
#ifdef _SC_REALTIME_SIGNALS
    { "SC_REALTIME_SIGNALS"_s, _SC_REALTIME_SIGNALS },
#endif
#ifdef _SC_RE_DUP_MAX
    { "SC_RE_DUP_MAX"_s, _SC_RE_DUP_MAX },
#endif
#ifdef _SC_RTSIG_MAX
    { "SC_RTSIG_MAX"_s, _SC_RTSIG_MAX },
#endif
#ifdef _SC_SAVED_IDS
    { "SC_SAVED_IDS"_s, _SC_SAVED_IDS },
#endif
#ifdef _SC_SCHAR_MAX
    { "SC_SCHAR_MAX"_s, _SC_SCHAR_MAX },
#endif
#ifdef _SC_SCHAR_MIN
    { "SC_SCHAR_MIN"_s, _SC_SCHAR_MIN },
#endif
#ifdef _SC_SELECT
    { "SC_SELECT"_s, _SC_SELECT },
#endif
#ifdef _SC_SEMAPHORES
    { "SC_SEMAPHORES"_s, _SC_SEMAPHORES },
#endif
#ifdef _SC_SEM_NSEMS_MAX
    { "SC_SEM_NSEMS_MAX"_s, _SC_SEM_NSEMS_MAX },
#endif
#ifdef _SC_SEM_VALUE_MAX
    { "SC_SEM_VALUE_MAX"_s, _SC_SEM_VALUE_MAX },
#endif
#ifdef _SC_SHARED_MEMORY_OBJECTS
    { "SC_SHARED_MEMORY_OBJECTS"_s, _SC_SHARED_MEMORY_OBJECTS },
#endif
#ifdef _SC_SHRT_MAX
    { "SC_SHRT_MAX"_s, _SC_SHRT_MAX },
#endif
#ifdef _SC_SHRT_MIN
    { "SC_SHRT_MIN"_s, _SC_SHRT_MIN },
#endif
#ifdef _SC_SIGQUEUE_MAX
    { "SC_SIGQUEUE_MAX"_s, _SC_SIGQUEUE_MAX },
#endif
#ifdef _SC_SIGRT_MAX
    { "SC_SIGRT_MAX"_s, _SC_SIGRT_MAX },
#endif
#ifdef _SC_SIGRT_MIN
    { "SC_SIGRT_MIN"_s, _SC_SIGRT_MIN },
#endif
#ifdef _SC_SOFTPOWER
    { "SC_SOFTPOWER"_s, _SC_SOFTPOWER },
#endif
#ifdef _SC_SPLIT_CACHE
    { "SC_SPLIT_CACHE"_s, _SC_SPLIT_CACHE },
#endif
#ifdef _SC_SSIZE_MAX
    { "SC_SSIZE_MAX"_s, _SC_SSIZE_MAX },
#endif
#ifdef _SC_STACK_PROT
    { "SC_STACK_PROT"_s, _SC_STACK_PROT },
#endif
#ifdef _SC_STREAM_MAX
    { "SC_STREAM_MAX"_s, _SC_STREAM_MAX },
#endif
#ifdef _SC_SYNCHRONIZED_IO
    { "SC_SYNCHRONIZED_IO"_s, _SC_SYNCHRONIZED_IO },
#endif
#ifdef _SC_THREADS
    { "SC_THREADS"_s, _SC_THREADS },
#endif
#ifdef _SC_THREAD_ATTR_STACKADDR
    { "SC_THREAD_ATTR_STACKADDR"_s, _SC_THREAD_ATTR_STACKADDR },
#endif
#ifdef _SC_THREAD_ATTR_STACKSIZE
    { "SC_THREAD_ATTR_STACKSIZE"_s, _SC_THREAD_ATTR_STACKSIZE },
#endif
#ifdef _SC_THREAD_DESTRUCTOR_ITERATIONS
    { "SC_THREAD_DESTRUCTOR_ITERATIONS"_s, _SC_THREAD_DESTRUCTOR_ITERATIONS },
#endif
#ifdef _SC_THREAD_KEYS_MAX
    { "SC_THREAD_KEYS_MAX"_s, _SC_THREAD_KEYS_MAX },
#endif
#ifdef _SC_THREAD_PRIORITY_SCHEDULING
    { "SC_THREAD_PRIORITY_SCHEDULING"_s, _SC_THREAD_PRIORITY_SCHEDULING },
#endif
#ifdef _SC_THREAD_PRIO_INHERIT
    { "SC_THREAD_PRIO_INHERIT"_s, _SC_THREAD_PRIO_INHERIT },
#endif
#ifdef _SC_THREAD_PRIO_PROTECT
    { "SC_THREAD_PRIO_PROTECT"_s, _SC_THREAD_PRIO_PROTECT },
#endif
#ifdef _SC_THREAD_PROCESS_SHARED
    { "SC_THREAD_PROCESS_SHARED"_s, _SC_THREAD_PROCESS_SHARED },
#endif
#ifdef _SC_THREAD_SAFE_FUNCTIONS
    { "SC_THREAD_SAFE_FUNCTIONS"_s, _SC_THREAD_SAFE_FUNCTIONS },
#endif
#ifdef _SC_THREAD_STACK_MIN
    { "SC_THREAD_STACK_MIN"_s, _SC_THREAD_STACK_MIN },
#endif
#ifdef _SC_THREAD_THREADS_MAX
    { "SC_THREAD_THREADS_MAX"_s, _SC_THREAD_THREADS_MAX },
#endif
#ifdef _SC_TIMERS
    { "SC_TIMERS"_s, _SC_TIMERS },
#endif
#ifdef _SC_TIMER_MAX
    { "SC_TIMER_MAX"_s, _SC_TIMER_MAX },
#endif
#ifdef _SC_TTY_NAME_MAX
    { "SC_TTY_NAME_MAX"_s, _SC_TTY_NAME_MAX },
#endif
#ifdef _SC_TZNAME_MAX
    { "SC_TZNAME_MAX"_s, _SC_TZNAME_MAX },
#endif
#ifdef _SC_T_IOV_MAX
    { "SC_T_IOV_MAX"_s, _SC_T_IOV_MAX },
#endif
#ifdef _SC_UCHAR_MAX
    { "SC_UCHAR_MAX"_s, _SC_UCHAR_MAX },
#endif
#ifdef _SC_UINT_MAX
    { "SC_UINT_MAX"_s, _SC_UINT_MAX },
#endif
#ifdef _SC_UIO_MAXIOV
    { "SC_UIO_MAXIOV"_s, _SC_UIO_MAXIOV },
#endif
#ifdef _SC_ULONG_MAX
    { "SC_ULONG_MAX"_s, _SC_ULONG_MAX },
#endif
#ifdef _SC_USHRT_MAX
    { "SC_USHRT_MAX"_s, _SC_USHRT_MAX },
#endif
#ifdef _SC_VERSION
    { "SC_VERSION"_s, _SC_VERSION },
#endif
#ifdef _SC_WORD_BIT
    { "SC_WORD_BIT"_s, _SC_WORD_BIT },
#endif
#ifdef _SC_XBS5_ILP32_OFF32
    { "SC_XBS5_ILP32_OFF32"_s, _SC_XBS5_ILP32_OFF32 },
#endif
#ifdef _SC_XBS5_ILP32_OFFBIG
    { "SC_XBS5_ILP32_OFFBIG"_s, _SC_XBS5_ILP32_OFFBIG },
#endif
#ifdef _SC_XBS5_LP64_OFF64
    { "SC_XBS5_LP64_OFF64"_s, _SC_XBS5_LP64_OFF64 },
#endif
#ifdef _SC_XBS5_LPBIG_OFFBIG
    { "SC_XBS5_LPBIG_OFFBIG"_s, _SC_XBS5_LPBIG_OFFBIG },
#endif
#ifdef _SC_XOPEN_CRYPT
    { "SC_XOPEN_CRYPT"_s, _SC_XOPEN_CRYPT },
#endif
#ifdef _SC_XOPEN_ENH_I18N
    { "SC_XOPEN_ENH_I18N"_s, _SC_XOPEN_ENH_I18N },
#endif
#ifdef _SC_XOPEN_LEGACY
    { "SC_XOPEN_LEGACY"_s, _SC_XOPEN_LEGACY },
#endif
#ifdef _SC_XOPEN_REALTIME
    { "SC_XOPEN_REALTIME"_s, _SC_XOPEN_REALTIME },
#endif
#ifdef _SC_XOPEN_REALTIME_THREADS
    { "SC_XOPEN_REALTIME_THREADS"_s, _SC_XOPEN_REALTIME_THREADS },
#endif
#ifdef _SC_XOPEN_SHM
    { "SC_XOPEN_SHM"_s, _SC_XOPEN_SHM },
#endif
#ifdef _SC_XOPEN_UNIX
    { "SC_XOPEN_UNIX"_s, _SC_XOPEN_UNIX },
#endif
#ifdef _SC_XOPEN_VERSION
    { "SC_XOPEN_VERSION"_s, _SC_XOPEN_VERSION },
#endif
#ifdef _SC_XOPEN_XCU_VERSION
    { "SC_XOPEN_XCU_VERSION"_s, _SC_XOPEN_XCU_VERSION },
#endif
#ifdef _SC_XOPEN_XPG2
    { "SC_XOPEN_XPG2"_s, _SC_XOPEN_XPG2 },
#endif
#ifdef _SC_XOPEN_XPG3
    { "SC_XOPEN_XPG3"_s, _SC_XOPEN_XPG3 },
#endif
#ifdef _SC_XOPEN_XPG4
    { "SC_XOPEN_XPG4"_s, _SC_XOPEN_XPG4 },
#endif
#ifdef _SC_MINSIGSTKSZ
    { "SC_MINSIGSTKSZ"_s, _SC_MINSIGSTKSZ },
#endif
};

void addPosixConstants(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    addPosixConstants(globalObject, module, posixConstants);
    module->add(globalObject, "NODEV"_s, posixFromDev(globalObject, posixNoDevice));

    // The functions look the names up in the same dicts that the module shows.
    PyDict* state = posixState(globalObject);
    auto addNames = [&](ASCIILiteral name, std::span<const PosixConstant> constants) {
        PyDict* dict = posixConstantDict(globalObject, constants);
        state->set(vm, internString(vm, name), dict);
        module->add(globalObject, name, dict);
    };
    addNames("pathconf_names"_s, posixPathconfNames);
#if !OS(ANDROID)
    addNames("confstr_names"_s, posixConfstrNames);
#endif
    addNames("sysconf_names"_s, posixSysconfNames);
}

} // namespace Python
} // namespace JSC

#endif // !OS(WINDOWS)
