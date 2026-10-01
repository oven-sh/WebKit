#!/usr/bin/env python3
# capped.py <GB> <seconds> <command...>: runs the command and kills it (and its process group) as soon as its physical footprint passes
# the cap or its time is up. macOS does not enforce RLIMIT_AS or RLIMIT_DATA, so this polls.
import sys,os,subprocess,time,signal,ctypes
cap=float(sys.argv[1])*(1<<30); limit=float(sys.argv[2])
lib=ctypes.CDLL('/usr/lib/libproc.dylib')
class RUsage(ctypes.Structure): # rusage_info_v0 up to ri_phys_footprint
    _fields_=[('uuid',ctypes.c_uint8*16)]+[(n,ctypes.c_uint64) for n in ('user','system','pkg_idle_wkups','interrupt_wkups','pageins','wired','resident','phys_footprint','start','exit')]
def footprint(pid):
    r=RUsage()
    return r.phys_footprint if lib.proc_pid_rusage(pid,0,ctypes.byref(r))==0 else 0
p=subprocess.Popen(sys.argv[3:],start_new_session=True)
t0=time.time(); peak=0
while p.poll() is None:
    f=footprint(p.pid); peak=max(peak,f)
    if f>cap or time.time()-t0>limit:
        os.killpg(p.pid,signal.SIGKILL); p.wait()
        print('\n[capped] killed after %.1f s at %.2f GB (%s)'%(time.time()-t0,f/(1<<30),'memory cap' if f>cap else 'time limit'),file=sys.stderr); sys.exit(137)
    time.sleep(0.02)
print('[capped] peak %.2f GB, %.1f s'%(peak/(1<<30),time.time()-t0),file=sys.stderr)
sys.exit(p.returncode if p.returncode>=0 else 128-p.returncode)
