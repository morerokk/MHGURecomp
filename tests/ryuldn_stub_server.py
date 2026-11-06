#!/usr/bin/env python3
"""A small stand-in for Ryujinx's LdnServer, for testing runtime/net/ryuldn.c without Ryujinx, .NET or Redis.

It speaks the same wire format (header 'RLDN', type, version 1, 2 padding, s32 size) and does just enough of the
server's job: Initialize / Passphrase, lobbies (create, scan, connect, leave), virtual addresses 10.114.0.n, SyncNetwork
and Connected pushes, Ping, and relaying ProxyData between the members of a lobby. Usage: ryuldn_stub_server.py [port]
"""
import socket
import struct
import sys
import threading

P = dict(INITIALIZE=0, PASSPHRASE=1, CREATE=2, SYNC=7, REJECT=8, REJECT_REPLY=9, SCAN=10, SCAN_REPLY=11,
         SCAN_END=12, CONNECT=13, CONNECTED=15, DISCONNECT=16, PROXY_CONFIG=17, PROXY_DATA=20, PING=254, ERROR=255)
INFO = 0x480
NODE0, NODE_SIZE = 0x68, 0x40
lock = threading.Lock()
lobbies = []  # {'info': bytearray, 'members': [Client], 'pass': bytes, 'next': int}
log = lambda *a: print('[stub]', *a, flush=True)


def pkt(t, payload=b''):
    return struct.pack('<IBB2xi', 0x4E444C52, t, 1, len(payload)) + payload


class Client:
    def __init__(self, s, addr):
        self.s, self.addr, self.lobby, self.ip, self.node, self.passphrase = s, addr, None, 0, 0, b''
        self.wl = threading.Lock()

    def send(self, t, payload=b''):
        with self.wl:
            try:
                self.s.sendall(pkt(t, payload))
            except OSError:
                pass


def recv_all(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def node_count(info):
    return info[0x67]


def push(lobby, t):
    for m in list(lobby['members']):
        m.send(t, bytes(lobby['info']))


def add_member(lobby, c, user):
    """Gives c the next free node slot and address, updates the NetworkInfo."""
    info = lobby['info']
    for i in range(8):
        off = NODE0 + i * NODE_SIZE
        if not info[off + 0x0B]:
            c.node, c.ip, c.lobby = i, 0x0A720001 + i, lobby
            struct.pack_into('<I', info, off, c.ip)
            info[off + 0x0A] = i
            info[off + 0x0B] = 1
            info[off + 0x0C:off + 0x0C + 0x20] = user[:0x20].ljust(0x20, b'\0')
            info[0x67] = node_count(info) + 1
            lobby['members'].append(c)
            return True
    return False


def remove_member(c):
    lobby = c.lobby
    if not lobby:
        return
    c.lobby = None
    with lock:
        if c in lobby['members']:
            lobby['members'].remove(c)
        if c.node == 0:  # the host left: the lobby ends
            for m in list(lobby['members']):
                m.lobby = None
                m.send(P['DISCONNECT'], struct.pack('<I', 0))
            lobby['members'].clear()
            if lobby in lobbies:
                lobbies.remove(lobby)
            return
        off = NODE0 + c.node * NODE_SIZE
        lobby['info'][off:off + NODE_SIZE] = bytes(NODE_SIZE)
        lobby['info'][0x67] -= 1
    push(lobby, P['SYNC'])


def serve(c):
    try:
        while True:
            h = recv_all(c.s, 12)
            magic, t, ver, size = struct.unpack('<IBB2xi', h)
            assert magic == 0x4E444C52 and ver == 1 and 0 <= size < 131072, (magic, ver, size)
            d = recv_all(c.s, size) if size else b''
            handle(c, t, d)
    except (EOFError, OSError, AssertionError) as e:
        log('client gone', c.addr, repr(e))
    finally:
        remove_member(c)
        c.s.close()


def handle(c, t, d):
    if t == P['INITIALIZE']:
        c.send(P['INITIALIZE'], bytes(range(1, 17)) + bytes([2, 0, 0, 0, 0, 1]))
    elif t == P['PASSPHRASE']:
        c.passphrase = d[:0x80].rstrip(b'\0')
    elif t == P['PING']:
        pass
    elif t == P['CREATE']:
        # security 0x44, user 0x30 @0x44, NetworkConfig 0x20 @0x74, RyuNetworkConfig 0x28 @0x94, advertise @0xBC
        assert len(d) >= 0xBC, len(d)
        info = bytearray(INFO)
        info[0x00:0x10] = d[0x74:0x84]                         # IntentId
        info[0x10:0x20] = bytes([0xAB]) * 16                   # session id
        info[0x20:0x26] = bytes([2, 9, 9, 9, 9, len(lobbies)])  # BSSID
        info[0x4A], info[0x4B] = 3, 2
        info[0x66] = d[0x74 + 0x12] or 8                       # node count max
        adv = d[0xBC:0xBC + 0x180]
        struct.pack_into('<H', info, 0x26A, len(adv))
        info[0x26C:0x26C + len(adv)] = adv
        lobby = {'info': info, 'members': [], 'pass': c.passphrase, 'game': d[0x94:0x94 + 16].rstrip(b'\0')}
        user = d[0x44:0x44 + 0x30]
        with lock:
            add_member(lobby, c, user)
            lobbies.append(lobby)
        log('lobby created by', c.addr, 'game version', lobby['game'], 'advertise', len(adv))
        c.send(P['PROXY_CONFIG'], struct.pack('<II', c.ip, 0xFFFF0000))
        c.send(P['CONNECTED'], bytes(info))
    elif t == P['SCAN']:
        with lock:
            found = [bytes(l['info']) for l in lobbies if l['pass'] == c.passphrase]
        for i in found:
            c.send(P['SCAN_REPLY'], i)
        c.send(P['SCAN_END'])
        log('scan ->', len(found))
    elif t == P['CONNECT']:
        assert len(d) >= 0x500, len(d)
        user = d[0x44:0x44 + 0x30]
        bssid = d[0x80 + 0x20:0x80 + 0x26]
        with lock:
            lobby = next((l for l in lobbies if bytes(l['info'][0x20:0x26]) == bssid and l['pass'] == c.passphrase), None)
            ok = lobby is not None and node_count(lobby['info']) < lobby['info'][0x66] and add_member(lobby, c, user)
        if not lobby:
            c.send(P['ERROR'], struct.pack('<i', 6))  # ConnectNotFound
            return
        if not ok:
            c.send(P['ERROR'], struct.pack('<i', 2))  # TooManyPlayers
            return
        log('join', c.addr, 'as 10.114.0.%d' % (c.node + 1))
        c.send(P['PROXY_CONFIG'], struct.pack('<II', c.ip, 0xFFFF0000))
        c.send(P['CONNECTED'], bytes(lobby['info']))
        push(lobby, P['SYNC'])
    elif t == P['DISCONNECT']:
        remove_member(c)
    elif t == P['PROXY_DATA']:
        lobby = c.lobby
        if not lobby or len(d) < 0x14:
            return
        dst = struct.unpack_from('<I', d, 6)[0]
        bcast = dst == 0xFFFFFFFF or dst == 0x0A72FFFF
        for m in list(lobby['members']):
            if m is not c and (bcast or m.ip == dst):
                m.send(P['PROXY_DATA'], d)
    elif t == P['SET_ADVERTISE'] if 'SET_ADVERTISE' in P else t == 23:
        if c.lobby:
            info = c.lobby['info']
            struct.pack_into('<H', info, 0x26A, len(d))
            info[0x26C:0x26C + 0x180] = d.ljust(0x180, b'\0')[:0x180]
            push(c.lobby, P['SYNC'])
    elif t == P['REJECT']:
        c.send(P['REJECT_REPLY'])


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 30456
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', port))
    ls.listen(16)
    log('listening on', port)
    while True:
        s, a = ls.accept()
        threading.Thread(target=serve, args=(Client(s, a),), daemon=True).start()


if __name__ == '__main__':
    main()
