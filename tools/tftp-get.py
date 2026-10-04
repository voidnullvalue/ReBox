#!/usr/bin/env python3
"""Bounded TFTP RRQ with duplicate suppression; destination must be new."""
import argparse
import os
import pathlib
import socket
import struct

def get(host, port, name, destination, blocksize=8192):
    destination = pathlib.Path(destination)
    if destination.exists():
        raise ValueError('Refusing to overwrite destination')
    request = struct.pack('!H', 1) + name.encode() + b'\0octet\0blksize\0' + str(blocksize).encode() + b'\0'
    peer = (socket.gethostbyname(host), port)
    expected = 1
    negotiated = 512
    last = request
    total = 0
    failures = 0
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3)
    sock.sendto(request, peer)
    with destination.open('xb') as output:
        while True:
            try:
                packet, sender = sock.recvfrom(65535)
            except socket.timeout:
                failures += 1
                if failures > 6:
                    raise TimeoutError(f'TFTP stalled after {total} bytes; partial retained at {destination}')
                sock.sendto(last, peer)
                continue
            if sender[0] != peer[0] or len(packet) < 2:
                continue
            if peer[1] != port and sender != peer:
                continue
            peer = sender
            operation = struct.unpack('!H', packet[:2])[0]
            if operation == 5:
                raise OSError(packet[4:].rstrip(b'\0').decode(errors='replace'))
            if operation == 6:
                if total:
                    continue
                fields = packet[2:].rstrip(b'\0').split(b'\0')
                options = dict(zip(fields[::2], fields[1::2]))
                negotiated = int(options.get(b'blksize', b'512'))
                if not 8 <= negotiated <= blocksize:
                    raise ValueError('Invalid negotiated block size')
                last = struct.pack('!HH', 4, 0)
                sock.sendto(last, peer)
                continue
            if operation != 3 or len(packet) < 4:
                continue
            block = struct.unpack('!H', packet[2:4])[0]
            if block == ((expected - 1) & 65535):
                sock.sendto(last, peer)
                continue
            if block != expected:
                continue
            payload = packet[4:]
            if len(payload) > negotiated:
                raise ValueError('Oversized TFTP block')
            output.write(payload)
            total += len(payload)
            failures = 0
            last = struct.pack('!HH', 4, block)
            sock.sendto(last, peer)
            expected = (expected + 1) & 65535
            if len(payload) < negotiated:
                sock.settimeout(0.5)
                for _ in range(3):
                    try:
                        repeated, address = sock.recvfrom(65535)
                    except socket.timeout:
                        break
                    if address == peer and repeated[:4] == packet[:4]:
                        sock.sendto(last, peer)
                sock.close()
                return total

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host'); parser.add_argument('port', type=int)
    parser.add_argument('remote_name'); parser.add_argument('output')
    args = parser.parse_args()
    print('OK', args.output, get(args.host, args.port, args.remote_name, args.output, int(os.environ.get('TFTP_BLKSIZE', '8192'))))
