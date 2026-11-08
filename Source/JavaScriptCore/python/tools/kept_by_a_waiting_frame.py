#!/usr/bin/env python3
"""What a thread has let go of and that stays alive while the thread waits: is it the stack that holds it, and whose frame?

    bun kept_by_a_waiting_frame.py KIND [SYMBOLS]

KIND: unbuffered, buffered, readinto, recv, recvfrom, recvmsg, recv_into, ctypes, lock, event, join, or all. SYMBOLS: `nm -C --defined-only` of the binary that runs this, or of its twin that has
symbols; without it the frames have sizes and no names. Linux, with frame pointers.

A thread makes an object, uses it in one of some ways, lets go of it and blocks. After six collections, for an object that is still
alive, this prints what a heap snapshot taken at that moment says of the cell (its incoming edges, whether it is among the roots: the
collector reports neither for what it has found on a stack) and which words of the blocked thread's stack point at it, each with the frame
it lies in. See "The stack" in README.md: the frames under which a thread waits have little room that is not written to.
"""
import bisect, copy, ctypes, gc, json, os, platform, socket, struct, sys, threading, time, weakref
import js


class Kept:
    def method(self, *arguments):
        return None


def deep(o, depth=12):
    return deep(o, depth - 1) if depth else repr(o)


WAYS = {
    'repr': repr,
    'index in a list': lambda o: [o, o].index(o),
    'key of a dict': lambda o: {o: 1}[o],
    'formatted': lambda o: '%s %r' % (o, o),
    'argument of its method': lambda o: o.method(o, o),
    'hash of a tuple': lambda o: hash((o, 1)),
    'isinstance': lambda o: isinstance(o, Kept),
    'getattr, hasattr': lambda o: (getattr(o, 'method'), hasattr(o, 'none')),
    'copy.copy': copy.copy,
    'sorted': lambda o: sorted([o, o], key=id),
    'printed': lambda o: print(o, file=open(os.devnull, 'w')),
    'weakref called': lambda o: weakref.ref(o)(),
    'twelve calls deep': deep,
    'map': lambda o: list(map(repr, [o])),
    'in a set': lambda o: o in {o},
    'str, encoded': lambda o: str(o).encode(),
}


def snapshot_says(cell):
    take = js.eval('() => JSON.stringify(process.getBuiltinModule("node:module").createRequire("/")("bun:jsc").generateHeapSnapshotForDebugging())')
    snapshot = json.loads(str(take()))
    nodes, edges, roots = snapshot['nodes'], snapshot['edges'], snapshot['roots']
    identifier = next((nodes[i] for i in range(0, len(nodes), 7) if nodes[i + 5] == '%#x' % cell), None)
    if identifier is None:
        return 'the snapshot has no such cell'
    return 'in the snapshot: %d incoming edges, %d entries among the roots' % (
        sum(edges[i + 1] == identifier for i in range(0, len(edges), 4)),
        sum(roots[i] == identifier for i in range(0, len(roots), 3)),
    )


FUTEX = {'x86_64': 202, 'aarch64': 98}.get(platform.machine())


def system_call_of(thread, sleeps=False):
    # The fields of /proc's line if the thread is blocked in a system call, and not for its turn to run. Else nothing: it is "running", between two pieces of its wait.
    # `sleeps`: what it waits for is another thread, which is the same system call as the wait for its turn. Then: it has not run while this thread slept.
    def read(name):
        with open('/proc/self/task/%d/%s' % (thread, name)) as file:
            return file.read().split()
    fields = read('syscall')
    if len(fields) != 9:
        return None
    if not sleeps:
        return fields if int(fields[0]) != FUTEX else None
    time_on_processor = read('schedstat')[0]
    time.sleep(0.005)
    return fields if (read('syscall'), read('schedstat')[0]) == (fields, time_on_processor) else None


def words_that_point_at(cell, size, thread, symbols, sleeps):
    maps = [(int(p[0].split('-')[0], 16), int(p[0].split('-')[1], 16), p[1], p[5] if len(p) > 5 else '') for p in (line.split() for line in open('/proc/self/maps'))]
    executable = os.path.realpath('/proc/self/exe')
    starts = [s[0] for s in symbols]

    def name(address):
        for low, high, mode, path in maps:
            if low <= address < high:
                if 'x' not in mode:
                    return None
                if path != executable:
                    return '[%s]' % (os.path.basename(path) or 'jit')
                return symbols[bisect.bisect_right(starts, address) - 1][1].split('(')[0] if symbols else '?'

    fields = None
    while not fields:
        fields = system_call_of(thread, sleeps)
    bottom = int(fields[7], 16)
    top = next((high for low, high, mode, path in maps if low <= bottom < high), bottom)
    # (Not ctypes.string_at(): a build with ASAN stops at the red zones between the variables of another thread's frames.)
    with open('/proc/self/mem', 'rb', buffering=0) as memory:
        raw = os.pread(memory.fileno(), top - bottom, bottom)
    words = struct.unpack('<%dQ' % (len(raw) // 8), raw)

    def chain(pointer):
        links = []
        while bottom <= pointer < top - 16 and not pointer % 8:
            caller = name(words[(pointer + 8 - bottom) // 8])
            if caller is None:
                break
            links.append((pointer, caller))
            following = words[(pointer - bottom) // 8]
            if following <= pointer:
                break
            pointer = following
        return links

    links = next((c for c in (chain(bottom + 8 * i) for i in range(len(words) - 2)) if len(c) >= 6), None)
    if links is None:
        yield 'a stack in which no chain of frames is found'
        return
    frames, low, owner = [], bottom, '(the system call and libc)'
    for pointer, caller in links:
        frames.append((low, pointer + 16, owner))
        low, owner = pointer + 16, caller
    for i, word in enumerate(words):
        if cell <= word < cell + size:
            address = bottom + 8 * i
            for low, high, owner in frames:
                if low <= address < high:
                    yield '%s, %d bytes below the top of its %d' % (owner, high - address, high - low)


def look(kind, symbols):
    reader, writer = os.pipe()
    here, there = socket.socketpair()
    unbuffered, buffered = os.fdopen(os.dup(reader), 'rb', buffering=0), os.fdopen(os.dup(reader), 'rb')
    libc, room = ctypes.CDLL(None), ctypes.create_string_buffer(1)
    block = {
        'unbuffered': lambda: unbuffered.read(1),
        'buffered': lambda: buffered.read(1),
        'readinto': lambda: os.readinto(reader, bytearray(1)),
        'recv': lambda: here.recv(1),
        'recvfrom': lambda: here.recvfrom(1),
        'recvmsg': lambda: here.recvmsg(1),
        'recv_into': lambda: here.recv_into(bytearray(1)),
        'ctypes': lambda: libc.read(reader, room, 1),
        'lock': lambda: lock.acquire(),
        'event': lambda: event.wait(),
        'join': lambda: other.join(),
    }[kind]
    sleeps = kind in ('lock', 'event', 'join')

    alive = 0
    for way, use in WAYS.items():
        box, ready = [], threading.Event()
        lock, event = threading.Lock(), threading.Event()
        lock.acquire()
        if kind == 'join':
            other = threading.Thread(target=event.wait, daemon=True)
            other.start()

        def run():
            def uses():
                o = Kept()
                box.append((weakref.ref(o), id(o)))
                use(o)

            uses()
            ready.set()
            block()

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        ready.wait()
        while not (system_call_of(thread.native_id, sleeps) and system_call_of(thread.native_id, sleeps)):
            pass
        for _ in range(6):
            gc.collect()
        reference, cell = box[0]
        if reference() is not None:
            alive += 1
            print('%s, %s: alive; %s' % (kind, way, snapshot_says(cell)))
            for line in words_that_point_at(cell, 80, thread.native_id, symbols, sleeps):
                print('    a word in ' + line)
        lock.release(), event.set()
        if not sleeps:
            there.send(b'x') if kind.startswith('recv') else os.write(writer, b'x')
        thread.join()
    print('%s: %d of %d alive after six collections' % (kind, alive, len(WAYS)))
    for file in (unbuffered, buffered, here, there):
        file.close()
    os.close(reader)
    os.close(writer)


symbols = sorted((int(line[:16], 16), line[19:].rstrip()) for line in open(sys.argv[2]) if line[17:18] in 'tTwW') if len(sys.argv) > 2 else []
for kind in ('unbuffered', 'buffered', 'readinto', 'recv', 'recvfrom', 'recvmsg', 'recv_into', 'ctypes', 'lock', 'event', 'join') if sys.argv[1] == 'all' else sys.argv[1:2]:
    look(kind, symbols)
