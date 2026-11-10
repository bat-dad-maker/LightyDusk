#!/usr/bin/env python3
"""Run the real relay and the C++ mod transport client against localhost."""

from __future__ import annotations

import os
import heapq
import random
import select
import threading
import socket
import subprocess
import sys
import time
from pathlib import Path


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class ImpairedRelay:
    """Separate per-client relay legs; impairment in each direction, seeded."""
    def __init__(self, relay_port, delay_ms, loss, asymmetric=False):
        self.listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listen.bind(("127.0.0.1", 0))
        self.port = self.listen.getsockname()[1]
        self.destination = ("127.0.0.1", relay_port)
        self.delay = delay_ms / 1000
        self.loss = loss
        self.asymmetric = asymmetric
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        rng = random.Random(711)
        peers, reverse, pending, delays = {}, {}, [], {}
        serial = 0
        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([self.listen, *reverse], [], [], 0.001)
                for source in ready:
                    try:
                        data, address = source.recvfrom(65536)
                    except ConnectionResetError:
                        # Windows surfaces ICMP for an intentionally departed
                        # client here. It must not stop other clients' routes.
                        continue
                    if source is self.listen:
                        if address not in peers:
                            peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            peer.bind(("127.0.0.1", 0))
                            delays[peer] = 0.01 if self.asymmetric and len(peers) % 2 == 0 else self.delay
                            peers[address] = peer
                            reverse[peer] = address
                        output, target = peers[address], self.destination
                    else:
                        output, target = self.listen, reverse[source]
                    if rng.random() < self.loss: continue
                    serial += 1
                    heapq.heappush(pending,(time.monotonic()+delays[output if source is self.listen else source],serial,output,target,data))
                now=time.monotonic()
                while pending and pending[0][0] <= now:
                    _,_,output,target,data=heapq.heappop(pending)
                    output.sendto(data,target)
        finally:
            for peer in reverse: peer.close()
            self.listen.close()

    def close(self):
        self.stop.set()
        self.thread.join()


def main() -> int:
    if len(sys.argv) not in (3, 4, 6):
        raise SystemExit("usage: run_relay_transport_test.py RELAY CLIENT_TEST")
    relay = Path(sys.argv[1]).resolve()
    client = Path(sys.argv[2]).resolve()
    port = reserve_port()
    test_env = os.environ.copy()
    proxy = None
    if len(sys.argv) >= 4 and sys.argv[3] not in ("--direct-wan", "--direct-asymmetric"):
        test_env["DUSKLIGHT_TEST_RELAY_ONLY"] = "1"
    if len(sys.argv) == 6:
        test_env["DUSKLIGHT_TEST_WAN"] = "1"
        proxy = ImpairedRelay(port, float(sys.argv[4]), float(sys.argv[5]), sys.argv[3] == "--direct-asymmetric")
    process = subprocess.Popen(
        [
            str(relay),
            "--host", "127.0.0.1",
            "--port", str(port),
            "--public-host", "127.0.0.1",
            "--public-port", str(port),
            "--endpoint-key", "integration-endpoint-key",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        if process.stdout is None:
            raise RuntimeError("relay output was not captured")
        first_line = process.stdout.readline().strip()
        if not first_line.startswith("Relay code: TP1-"):
            raise RuntimeError(f"relay startup failed: {first_line!r}")
        result = subprocess.run(
            [str(client), str(proxy.port if proxy else port), str(Path(__file__).parent / "reliable_udp/full_progression_upper.json")], text=True, capture_output=True, timeout=90, env=test_env
        )
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode
    finally:
        if proxy: proxy.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        if process.stdout is not None:
            process.stdout.close()


if __name__ == "__main__":
    raise SystemExit(main())
