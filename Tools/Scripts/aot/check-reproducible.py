#!/usr/bin/env python3
"""Checks that compiling a program gives the same image every time, whatever the number of compiler threads.

  check-reproducible.py <jsc> [--jobs N] [--option=--someOption=1 ...] [file.js ...]

Compiles each of JSTests/stress/aot-*.js and sound-types-*.js, or the given files, once with one compiler thread and three times
with sixteen, and compares the four images byte by byte. Each --option is passed to every compilation: what is off by default
is checked only if it is turned on here.

An image that differs with one thread has memory in it that was never written, or something that comes from an address. One that
differs only with several threads shows that a decision depends on which thread was first. In whole-program type inference that
means a step that is not monotone: a conclusion from a state that is not final, which is never taken back. (A call whose callee
nothing had reached yet counted as a call of an unknown function.) That loses types at random; the other way round it would be
unsound.
"""
import argparse, hashlib, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

STRESS = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'JSTests', 'stress'))
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('jsc', type=os.path.abspath)
parser.add_argument('files', nargs='*')
parser.add_argument('--jobs', type=int, default=max(1, os.cpu_count() // 4))
parser.add_argument('--option', action='append', default=[])
args = parser.parse_args()
files = [os.path.abspath(f) for f in args.files] or [os.path.join(STRESS, f) for f in sorted(os.listdir(STRESS)) if re.match(r'(aot|sound-types)-.*\.js$', f)]


def images(path):
    module = ['-m'] if re.search(r'//@[^\n]*"-m"', open(path, errors='replace').read(2000)) else []
    result = []
    for threads in (1, 16, 16, 16):
        with tempfile.NamedTemporaryFile() as image:
            subprocess.run([args.jsc, '--useJIT=false', '--useDollarVM=true', '--numberOfAOTCompilerThreads=%d' % threads, '--writeAOTImageTo=' + image.name, *args.option, *module, path], cwd=os.path.dirname(path), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            data = open(image.name, 'rb').read()
        result.append(hashlib.sha256(data).hexdigest()[:8] if data else 'none')
    return path, result


bad = 0
with ThreadPoolExecutor(args.jobs) as pool:
    for path, result in pool.map(images, files):
        if 'none' in result or len(set(result)) > 1:
            bad += 1
            print('%s: %s' % (os.path.basename(path), ' '.join(result)))
print('%d programs, %d not reproducible' % (len(files), bad))
sys.exit(1 if bad else 0)
