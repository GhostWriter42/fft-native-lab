#!/usr/bin/env python3
r"""netplay.py -- input-delay lockstep for two players (prototype; Python standard library only).

Two players each run their own copy of the native game (play.py: a docker container + a window). The game is deterministic (NATIVE-RUNTIME.md "Result 5"): the same program fed the same
controller values frame by frame stays bit-identical, so nothing but the controllers has to cross the network. Player 1 (the host) is controller 1, player 2 (the joiner) is controller 2;
the driver decides which of the two controllers plays at any moment (lockstep.c "seats": in battle the turn of a unit that seat 2 owns is played with controller 2).

The protocol is one 9-byte record per message, little endian: type ('P' pad / 'H' state hash), frame (u32), value (u32).
  'P' frame v : my controller value v applies to game frame `frame` (sampled `delay` frames before it is needed: the input delay hides the network latency)
  'H' frame h : my game's state hash after `frame` (the driver prints one every N frames); a mismatch with the peer's hash of the same frame is a desync
The first `delay` frames an answer is given for have no sampled input: both sides use 0 for them.

  Lockstep(sock, is_host, delay).step(n, local_pad) -> (pad1, pad2) for game frame n + 1, or None while the peer's controller value has not arrived yet (call again).
"""
import socket
import struct
import threading
import time

REC = struct.Struct('<cII')


def host(port, bind='0.0.0.0', timeout=120):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    srv.listen(1)
    srv.settimeout(timeout)
    conn, addr = srv.accept()
    srv.close()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return conn, addr


def join(addr, port, timeout=120):
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((addr, port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(None)
            return s
        except OSError:
            if time.time() - t0 > timeout:
                raise
            time.sleep(0.2)


class Lockstep:
    def __init__(self, sock, is_host, delay=6):
        self.sock = sock
        self.is_host = is_host
        self.delay = delay
        self.mine = {}                     # frame -> my controller value (sampled `delay` frames earlier)
        self.theirs = {}                   # frame -> the peer's controller value
        self.peer_hash = {}                # frame -> the peer's state hash
        self.my_hash = {}
        self.desync = None                 # (frame, mine, theirs) of the first mismatch
        self.closed = False
        self.lock = threading.Lock()
        self.sent = 0
        self.checked = 0                   # state-hash comparisons that matched
        self.f0 = None                    # the first game frame this side answered for (the zero-input window is f0 .. f0 + delay - 1)
        self.blobs = {}                    # kind -> (frame, bytes): configuration ('C'), a machine state ('S'), "start from the beginning" ('N')
        self.blob_cv = threading.Condition(self.lock)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b''
        need = None                        # (kind, frame, length) of a blob whose payload has not arrived completely
        while True:
            try:
                data = self.sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while True:
                if need is None:
                    if len(buf) < REC.size:
                        break
                    kind, frame, value = REC.unpack(buf[:REC.size])
                    buf = buf[REC.size:]
                    if kind in (b'C', b'S', b'N'):
                        need = (kind, frame, value)
                    else:
                        with self.lock:
                            if kind == b'P':
                                self.theirs[frame] = value
                            elif kind == b'H':
                                self.peer_hash[frame] = value
                                self._check(frame)
                        continue
                kind, frame, length = need
                if len(buf) < length:
                    break
                payload, buf = buf[:length], buf[length:]
                need = None
                with self.lock:
                    self.blobs[kind] = (frame, payload)
                    self.blob_cv.notify_all()
        with self.lock:
            self.closed = True
            self.blob_cv.notify_all()

    def send_blob(self, kind, frame, data):
        """A record whose `value` field is the length of the payload that follows ('C' configuration text, 'S' a whole-machine state of game frame `frame`, 'N' nothing)."""
        with self.lock:
            try:
                self.sock.sendall(REC.pack(kind, frame, len(data)) + data)
            except OSError:
                self.closed = True

    def wait_blob(self, kinds, timeout=120):
        """Wait for a blob of one of the kinds: (kind, (frame, payload)), or None when the link closed or the time ran out."""
        end = time.time() + timeout
        with self.blob_cv:
            while True:
                for k in kinds:
                    if k in self.blobs:
                        return k, self.blobs.pop(k)
                if self.closed or time.time() > end:
                    return None
                self.blob_cv.wait(0.2)

    def _send(self, kind, frame, value):
        try:
            self.sock.sendall(REC.pack(kind, frame, value & 0xffffffff))
            self.sent += 1
        except OSError:
            self.closed = True

    def _check(self, frame):                # (lock held)
        if frame in self.my_hash and frame in self.peer_hash:
            if self.my_hash[frame] != self.peer_hash[frame]:
                if self.desync is None:
                    self.desync = (frame, self.my_hash[frame], self.peer_hash[frame])
            else:
                self.checked += 1                                       # one more frame at which both games' state hashes were compared and equal

    def step(self, n, local_pad):
        """The answer for the driver after it reported frame n: the controllers of frame n + 1. Sends my controller for frame n + 1 + delay (once per n)."""
        f = n + 1
        with self.lock:
            if self.f0 is None:
                self.f0 = f
            if f + self.delay not in self.mine:
                self.mine[f + self.delay] = local_pad & 0xffff
                self._send(b'P', f + self.delay, local_pad & 0xffff)
            if f < self.f0 + self.delay:
                theirs = 0
            elif f in self.theirs:
                theirs = self.theirs.pop(f)
            else:
                return None
            mine = 0 if f < self.f0 + self.delay else self.mine.pop(f, 0)
        return (mine, theirs) if self.is_host else (theirs, mine)

    def report_hash(self, frame, h):
        with self.lock:
            self.my_hash[frame] = h
            self._send(b'H', frame, h)
            self._check(frame)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
