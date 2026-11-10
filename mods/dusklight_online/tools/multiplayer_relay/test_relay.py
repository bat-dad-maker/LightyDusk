#!/usr/bin/env python3
"""Black-box, three-client compatibility tests for Dusklight Online Relay."""

from __future__ import annotations

import argparse
import json
import hashlib
import hmac
import os
import socket
import struct
import subprocess
import sys
import time
import unittest
import zlib
from collections import deque
import ctypes
from pathlib import Path
from typing import Any
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from udp_test_client import ReliableSocket


GAMEPLAY_ROUTE_TYPES = (
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "room_switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
    "key_num",
    "light_drop_num",
    "light_drop_get_flag",
    "max_life_update",
    "bottle_slots",
    "bomb_bag_slot",
    "rupee_count",
    "rupee_delta",
    "poe_count",
    "malo_fundraising",
    "charlo_offering",
    "fish_record",
    "collect_smell",
    "item_get",
    "trade_item",
    "rando_item_get",
    "item_first_bit",
    "collect_crystal",
    "collect_mirror",
    "dark_clear_lv",
    "transform_lv",
    "region_bit",
    "collect",
    "visited_room",
    "letter_get",
    "presence",
    "progression_state",
    "puppet_preference",
    "midna_preference",
    "midna_pose",
    "pvp_hit",
    "coop_presence",
    "coop_settings",
    "coop_enemy_damage",
    "ganondorf_owner_claim",
    "ganondorf_owner",
    "ganondorf_hit",
    "ganondorf_reaction",
    "ganondorf_player_damage",
    "ganondorf_state",
    "ooccoo_state",
)

HOST_CONTROL_TYPES = (
    "dummy_model",
    "sync_flags",
    "sync_world",
    "remote_collision",
    "pvp_enabled",
)

UDP_HEADER = struct.Struct("<4sBBHIHHIIH32s")
UDP_ACK = struct.Struct("<IB32sB")
UDP_REGISTER_TYPE = 6
UDP_VOICE_TYPE = 8


def udp_packet(
    packet_type: int,
    sender_id: str,
    payload: bytes,
    sequence: int = 1,
) -> bytes:
    sender = sender_id.encode("utf-8")[:31].ljust(32, b"\0")
    return UDP_HEADER.pack(
        b"DMPU",
        1,
        packet_type,
        UDP_HEADER.size,
        sequence,
        0,
        1,
        len(payload),
        len(payload),
        len(payload),
        sender,
    ) + payload


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class RelayClient:
    instances = set()
    def __init__(self, port: int) -> None:
        self.sock = ReliableSocket(port, timeout=2.0)
        self.sock.settimeout(2.0)
        self.buffer = bytearray()
        self.inbox = deque()
        self.closed = False
        self.auto_barriers = True
        self.authenticated = False
        self.instances.add(self)
        self.port = port
        self.udp = self.sock

    def close(self) -> None:
        self.instances.discard(self)
        self.sock.close()

    def send(self, message: dict[str, Any]) -> None:
        if message.get("type") == "hello":
            message = self.prepare_hello(message)
            if message is None:
                return
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
        self.sock.sendall(payload)

    def prepare_hello(self, message: dict[str, Any]) -> dict[str, Any] | None:
        message = dict(message)
        code = message.pop("lobby_code", "")
        if self.authenticated:
            return message
        if not 6 <= len(code) <= 128:
            raise ValueError("lobby code must be 6-128 characters")
        room = message["room_id"]
        action = message["action"]
        self.sock.sendall((json.dumps({"type": "auth_begin", "protocol_version": 4,
            "room_id": room, "action": action}) + "\n").encode("utf-8"))
        challenge = self.receive()
        if challenge["type"] != "auth_challenge":
            self.inbox.appendleft(challenge)
            return None
        verifier = hashlib.pbkdf2_hmac("sha256", code.encode(),
            b"dusklight-lobby-code-v1:" + room.lower().encode() + b"\0" +
            b"relay-test-endpoint-key", 100_000)
        key = hmac.new(verifier, b"dusklight-room-key-v1:" +
            bytes.fromhex(challenge["salt"]), "sha256").digest()
        message["code_proof"] = hmac.new(key,
            ("dusklight-room-auth-v1:" + challenge["nonce"]).encode(),
            "sha256").hexdigest()
        if action == "create":
            message["code_verifier"] = verifier.hex()
        self.authenticated = True
        return message

    def send_bytes(self, payload: bytes) -> None:
        self.sock.sendall(payload)

    @classmethod
    def service(cls):
        ReliableSocket.pump()
        for client in tuple(cls.instances):
            data = ctypes.create_string_buffer(65536)
            for _ in range(64):
                count = ReliableSocket.library.udp_test_receive(client.sock.handle, data, len(data))
                if count <= 0:
                    if count == 0: client.closed = True
                    break
                client.buffer.extend(data.raw[:count])
            while b"\n" in client.buffer:
                line, _, rest = client.buffer.partition(b"\n")
                client.buffer = bytearray(rest)
                message = json.loads(ReliableSocket.decode(line))
                if message.get("type") == "settings_prepare" and client.auto_barriers:
                    # These server-only tests contain no peer gameplay. The
                    # production Transport tests exercise actual stream drains.
                    client.send({"type":"settings_ready","generation":message["generation"]})
                else: client.inbox.append(message)

    def receive(self, timeout: float = 2.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while True:
            self.service()
            if self.inbox: return self.inbox.popleft()
            if self.closed: raise ConnectionError("relay closed the connection")
            if time.monotonic() >= deadline: raise TimeoutError("timed out waiting for relay message")
            time.sleep(0.001)

    def expect_type(self, message_type: str, timeout: float = 2.0) -> dict[str, Any]:
        message = self.receive(timeout)
        if message.get("type") != message_type:
            raise AssertionError(f"expected {message_type!r}, received {message!r}")
        return message

    def expect_error(self, error: str, timeout: float = 2.0) -> None:
        message = self.expect_type("error", timeout)
        if message.get("error") != error:
            raise AssertionError(f"expected error {error!r}, received {message!r}")

    def register_udp(self, welcome: dict[str, Any]) -> None:
        token = welcome["udp_token"].encode("utf-8")
        self.udp.sendto(
            udp_packet(UDP_REGISTER_TYPE, welcome["client_id"], token, 0),
            ("127.0.0.1", self.port),
        )
        self.expect_type("udp_ready")


class RelayProcess:
    def __init__(self, executable: Path) -> None:
        self.port = reserve_port()
        self.process = subprocess.Popen(
            [
                str(executable),
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--public-host",
                "127.0.0.1",
                "--public-port",
                str(self.port),
                "--hello-timeout-ms",
                "2000",
                "--endpoint-key",
                "relay-test-endpoint-key",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env={**os.environ, "DUSK_MP_RELAY_PACKET_TRACE": "0"},
        )
        if not self.process.stdout:
            raise RuntimeError("relay stdout was not captured")
        self.relay_code_line = self.process.stdout.readline().strip()
        if not self.relay_code_line.startswith("Relay code: TP1-"):
            raise RuntimeError(
                f"relay did not print a usable endpoint code: {self.relay_code_line!r}"
            )
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                output = self.process.stdout.read() if self.process.stdout else ""
                raise RuntimeError(f"relay exited during startup:\n{output}")
            try:
                with ReliableSocket(self.port, timeout=2.0):
                    return
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("relay did not start listening")

    def close(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3.0)
        if self.process.stdout:
            self.process.stdout.close()


def hello(
    name: str,
    room: str = "phase12",
    lobby_code: str = "secret",
    action: str = "join",
    settings: dict[str, bool] | None = None,
    semantic_visuals: bool | None = True,
    snapshot_deltas: bool | None = True,
) -> dict[str, Any]:
    message: dict[str, Any] = {
        "type": "hello",
        "protocol_version": 4,
        "action": action,
        "room_id": room,
        "lobby_code": lobby_code,
        "name": name,
        "session_id": f"session-{name}",
        "want_puppet": True,
        "want_midna": False,
    }
    capabilities: dict[str, bool] = {}
    if semantic_visuals is not None:
        capabilities["semantic_visual_v1"] = semantic_visuals
    if snapshot_deltas is not None:
        capabilities["semantic_snapshot_delta_v1"] = snapshot_deltas
    if capabilities:
        message["capabilities"] = capabilities
    if settings is not None:
        message["settings"] = settings
    return message


class RelayTests(unittest.TestCase):
    executable: Path
    relay: RelayProcess
    clients: list[RelayClient]

    @classmethod
    def setUpClass(cls) -> None:
        cls.relay = RelayProcess(cls.executable)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.relay.close()

    def setUp(self) -> None:
        self.clients = []
        self.created_rooms: set[str] = set()

    def tearDown(self) -> None:
        for client in self.clients:
            client.close()

    def client(self) -> RelayClient:
        client = RelayClient(self.relay.port)
        self.clients.append(client)
        return client

    def test_malformed_compressed_frame_is_rejected(self):
        client = self.client()
        client.sock.sendall(b"Z1gg\n")
        client.expect_error("invalid_compressed_json")

    def test_deep_json_before_auth_cannot_crash_relay(self):
        client = self.client()
        # Keep the probe below the relay's deliberately short test login
        # timeout; the unit test covers a full 32 KiB nested frame.
        client.send_bytes(b"[" * 128 + b"0" + b"]" * 128 + b"\n")
        client.expect_error("invalid_compressed_json")
        self.assertIsNone(self.relay.process.poll())
        owner, _ = self.join("Still Running", "after-deep-json")
        owner.send({"type": "ping"})
        owner.expect_type("pong")

    def test_stun_shares_gameplay_port(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            sock.settimeout(0.3)
            destination = ("127.0.0.1", self.relay.port)
            transaction = bytes.fromhex("0102030405060708090a0b0c")
            request = struct.pack("!HHI12s", 1, 0, 0x2112A442, transaction)
            sock.sendto(request, destination)
            response, source = sock.recvfrom(512)
            self.assertEqual(source, destination)
            self.assertEqual(response[:20], struct.pack("!HHI12s", 0x101, 20, 0x2112A442, transaction))
            kind, length, reserved, family, xport, xip = struct.unpack("!HHBBHI", response[20:32])
            self.assertEqual((kind, length, reserved, family), (0x20, 8, 0, 1))
            self.assertEqual(xport ^ 0x2112, sock.getsockname()[1])
            self.assertEqual(socket.inet_ntoa(struct.pack("!I", xip ^ 0x2112A442)), "127.0.0.1")
            self.assertEqual(response[32:36], b"\x80\x28\x00\x04")
            self.assertEqual(struct.unpack("!I", response[36:])[0], zlib.crc32(response[:32]) ^ 0x5354554E)
            # libjuice-style optional SOFTWARE and FINGERPRINT attributes.
            header = struct.pack("!HHI12s", 1, 16, 0x2112A442, transaction)
            software = b"\x80\x22\x00\x04test"
            fingerprint = zlib.crc32(header + software) ^ 0x5354554E
            with_fp = header + software + struct.pack("!HHI", 0x8028, 4, fingerprint)
            sock.sendto(with_fp, destination)
            self.assertEqual(sock.recvfrom(512)[0], response)
            for bad in (request[:-1], request[:4] + b"xxxx" + request[8:],
                        with_fp[:-1] + bytes([with_fp[-1] ^ 1]),
                        struct.pack("!HHI12sHH", 1, 4, 0x2112A442, transaction, 0x8022, 100)):
                sock.sendto(bad, destination)
                with self.assertRaises(socket.timeout):
                    sock.recvfrom(512)
        client, welcome = self.join("OnePort", "one-port")
        self.assertEqual(welcome["stun_port"], self.relay.port)
        client.send({"type": "ping"})
        client.expect_type("pong")

    def test_settings_barrier_requires_all_current_members(self):
        owner, ow = self.join("Owner", "barrier")
        peer, pw = self.join("Peer", "barrier")
        owner.expect_type("peer_joined")
        owner.auto_barriers = peer.auto_barriers = False
        owner.send({"type":"room_settings","settings":{"sync_flags":False}})
        a = owner.expect_type("settings_prepare")
        b = peer.expect_type("settings_prepare")
        self.assertEqual(a["generation"],1)
        self.assertEqual(set(a["participants"]),{ow["client_id"],pw["client_id"]})
        peer.send({"type":"settings_ready","generation":999})
        owner.send({"type":"settings_ready","generation":a["generation"]})
        with self.assertRaises(TimeoutError): owner.receive(0.1)
        observer, welcome = self.join("Observer", "barrier")
        self.assertTrue(welcome["settings_pending"])
        self.assertEqual(welcome["settings_generation"],0)
        owner.expect_type("peer_joined"); peer.expect_type("peer_joined")
        peer.send({"type":"settings_ready","generation":b["generation"]})
        for client in (owner,peer,observer):
            commit = client.expect_type("room_settings")
            self.assertEqual(commit["settings_generation"],1)
            self.assertFalse(commit["settings"]["sync_flags"])

    def test_peer_traversal_signaling_and_recipient_fallback(self):
        sender, sw = self.join("IceSender", "ice-routing")
        target, tw = self.join("IceTarget", "ice-routing")
        sender.expect_type("peer_joined")
        excluded, ew = self.join("IceExcluded", "ice-routing")
        sender.expect_type("peer_joined")
        target.expect_type("peer_joined")
        outsider, ow = self.join("IceOutsider", "ice-other-room")
        for client, welcome in ((sender, sw), (target, tw), (excluded, ew), (outsider, ow)):
            client.register_udp(welcome)
        signal = {"type": "ice_signal", "client_id": ow["client_id"],
                  "target_client_id": tw["client_id"], "kind": 0, "data": "offer", "generation": 0}
        sender.send(signal)
        routed = target.expect_type("ice_signal")
        self.assertEqual(routed["client_id"], sw["client_id"])
        signal["target_client_id"] = ow["client_id"]
        sender.send(signal)
        with self.assertRaises(TimeoutError):
            outsider.receive(0.1)
        signal["target_client_id"] = tw["client_id"]
        signal["data"] = "x" * 4096
        sender.send(signal)
        with self.assertRaises(TimeoutError):
            target.receive(0.1)

        number = lambda w: int(w["client_id"].split("_", 1)[1])
        pose = udp_packet(2, sw["client_id"], b"pose", 123)
        envelope = b"DPF1" + struct.pack("<QQ", number(sw), number(tw)) + pose
        sender.udp.sendto(envelope, ("127.0.0.1", self.relay.port))
        self.assertEqual(target.udp.recvfrom(2048)[0], envelope)
        with self.assertRaises(socket.timeout):
            excluded.udp.recvfrom(2048)
        # A single grouped upload excludes the pair using its direct path.
        group = b"DPG1" + struct.pack("<QBQ", number(sw), 1, number(tw)) + pose
        sender.udp.sendto(group, ("127.0.0.1", self.relay.port))
        self.assertEqual(target.udp.recvfrom(2048)[0], envelope)
        with self.assertRaises(socket.timeout):
            excluded.udp.recvfrom(2048)
        # Wrong endpoint, wrong room, conflicting inner sender, nested envelope.
        excluded.udp.sendto(envelope, ("127.0.0.1", self.relay.port))
        forged = b"DPF1" + struct.pack("<QQ", number(sw), number(tw)) + udp_packet(2, ow["client_id"], b"spoof", 124)
        sender.udp.sendto(forged, ("127.0.0.1", self.relay.port))
        nested = b"DPF1" + struct.pack("<QQ", number(sw), number(tw)) + envelope
        sender.udp.sendto(nested, ("127.0.0.1", self.relay.port))
        with self.assertRaises(socket.timeout):
            target.udp.recvfrom(2048)
        other = b"DPF1" + struct.pack("<QQ", number(sw), number(ow)) + pose
        sender.udp.sendto(other, ("127.0.0.1", self.relay.port))
        with self.assertRaises(socket.timeout):
            outsider.udp.recvfrom(2048)
        target.send({"type": "puppet_preference", "want_puppet": False})
        sender.expect_type("puppet_preference")
        excluded.expect_type("puppet_preference")
        sender.udp.sendto(group, ("127.0.0.1", self.relay.port))
        with self.assertRaises(socket.timeout):
            target.udp.recvfrom(2048)

    def test_relay_operator_receives_endpoint_code(self) -> None:
        self.assertTrue(self.relay.relay_code_line.startswith("Relay code: TP1-"))

    def test_authenticated_udp_visual_routing(self) -> None:
        sender, sender_welcome = self.join("UdpSender", "udp-visual")
        receiver, receiver_welcome = self.join("UdpReceiver", "udp-visual")
        sender.expect_type("peer_joined")
        sender.register_udp(sender_welcome)
        receiver.register_udp(receiver_welcome)

        pose = udp_packet(2, sender_welcome["client_id"], b"visual-pose", 17)
        sender.udp.sendto(pose, ("127.0.0.1", self.relay.port))
        routed, _ = receiver.udp.recvfrom(2048)
        self.assertEqual(routed, pose)

        semantic_pose = udp_packet(
            7, sender_welcome["client_id"], b"semantic-visual-pose", 18
        )
        sender.udp.sendto(semantic_pose, ("127.0.0.1", self.relay.port))
        routed, _ = receiver.udp.recvfrom(2048)
        self.assertEqual(routed, semantic_pose)

        spoof = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            spoof.sendto(pose, ("127.0.0.1", self.relay.port))
            with self.assertRaises(socket.timeout):
                receiver.udp.recvfrom(2048)
        finally:
            spoof.close()

        sender.send({"type": "presence", "stage": "F_SP108"})
        receiver.expect_type("presence")
        receiver.send({"type": "presence", "stage": "D_MN05"})
        sender.expect_type("presence")
        sender.udp.sendto(
            udp_packet(2, sender_welcome["client_id"], b"wrong-stage", 18),
            ("127.0.0.1", self.relay.port),
        )
        with self.assertRaises(socket.timeout):
            receiver.udp.recvfrom(2048)

        receiver.send({"type": "presence", "stage": "F_SP108"})
        sender.expect_type("presence")
        receiver.send(
            {"type": "puppet_preference", "want_puppet": False, "want_midna": False}
        )
        sender.expect_type("puppet_preference")
        sender.udp.sendto(
            udp_packet(2, sender_welcome["client_id"], b"opted-out", 19),
            ("127.0.0.1", self.relay.port),
        )
        with self.assertRaises(socket.timeout):
            receiver.udp.recvfrom(2048)

    def test_udp_ack_routing_and_plaintext_rebind_rejection(self) -> None:
        sender, sender_welcome = self.join("UdpAckSender", "udp-ack-rebind")
        receiver, receiver_welcome = self.join("UdpAckReceiver", "udp-ack-rebind")
        sender.expect_type("peer_joined")
        sender.register_udp(sender_welcome)
        receiver.register_udp(receiver_welcome)

        acked_sender = (
            sender_welcome["client_id"].encode("utf-8")[:31].ljust(32, b"\0")
        )
        ack_payload = UDP_ACK.pack(17, 2, acked_sender, 0)
        ack = udp_packet(
            5,
            receiver_welcome["client_id"],
            ack_payload,
            17,
        )
        receiver.udp.sendto(ack, ("127.0.0.1", self.relay.port))
        routed_ack, _ = sender.udp.recvfrom(2048)
        self.assertEqual(routed_ack, ack)

        rebound = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rebound.bind(("127.0.0.1", 0))
        try:
            rebound.sendto(
                udp_packet(
                    UDP_REGISTER_TYPE,
                    sender_welcome["client_id"],
                    sender_welcome["udp_token"].encode("utf-8"),
                    0,
                ),
                ("127.0.0.1", self.relay.port),
            )
            time.sleep(0.15)

            rebound_pose = udp_packet(
                2, sender_welcome["client_id"], b"rebound-pose", 18
            )
            rebound.sendto(rebound_pose, ("127.0.0.1", self.relay.port))
            with self.assertRaises(socket.timeout):
                receiver.udp.recvfrom(2048)

            valid_pose = udp_packet(2, sender_welcome["client_id"], b"encrypted", 19)
            sender.udp.sendto(valid_pose,
                ("127.0.0.1", self.relay.port),
            )
            self.assertEqual(receiver.udp.recvfrom(2048)[0], valid_pose)
        finally:
            rebound.close()

    def test_voice_routes_on_relay_and_fallback_udp(self) -> None:
        sender, sw = self.join("VoiceSender", "voice-routing")
        receiver, rw = self.join("VoiceReceiver", "voice-routing")
        sender.expect_type("peer_joined")
        outsider, ow = self.join("VoiceOutsider", "voice-other-room")
        for client, welcome in ((sender, sw), (receiver, rw), (outsider, ow)):
            client.register_udp(welcome)

        position = struct.pack("<8sfffb", b"F_SP103", 14.0, 2.0, 3.0, 2)
        voice = udp_packet(UDP_VOICE_TYPE, sw["client_id"], position + b"\x78\x12\x34")
        address = ("127.0.0.1", self.relay.port)
        sender.udp.sendto(voice, address)
        self.assertEqual(receiver.udp.recvfrom(2048)[0], voice)
        with self.assertRaises(socket.timeout):
            outsider.udp.recvfrom(2048)

        sender_num = int(sw["client_id"].split("_", 1)[1])
        receiver_num = int(rw["client_id"].split("_", 1)[1])
        fallback = b"DPF1" + struct.pack("<QQ", sender_num, receiver_num) + voice
        sender.udp.sendto(fallback, address)
        self.assertEqual(receiver.udp.recvfrom(2048)[0], fallback)

        invalid = udp_packet(UDP_VOICE_TYPE, sw["client_id"], position)
        sender.udp.sendto(invalid, address)
        with self.assertRaises(socket.timeout):
            receiver.udp.recvfrom(2048)

    def join(self, name: str, room: str = "phase12") -> tuple[RelayClient, dict[str, Any]]:
        client = self.client()
        action = "join" if room in self.created_rooms else "create"
        client.send(hello(name, room, action=action))
        welcome = client.expect_type("welcome")
        self.created_rooms.add(room)
        return client, welcome

    def test_player_colour_presence(self) -> None:
        first, welcome = self.join("ColourA", "colour-presence")
        second, _ = self.join("ColourB", "colour-presence")
        first.expect_type("peer_joined")
        second.send({"type": "presence", "stage": "D_MN05"})
        first.expect_type("presence")
        for colour, outfit in (("BC5350", "204060"), ("BC5350", ""), ("", "204060"), ("", "")):
            first.send({"type": "presence", "stage": "F_SP108",
                        "player_color": colour, "outfit_color": outfit, "client_id": "spoof"})
            received = second.expect_type("presence")
            self.assertEqual(received["player_color"], colour)
            self.assertEqual(received["outfit_color"], outfit)
            self.assertEqual(received["client_id"], welcome["client_id"])
        third, joined = self.join("ColourC", "colour-presence")
        first.expect_type("peer_joined")
        second.expect_type("peer_joined")
        # Online republishes on membership changes; no relay-side profile cache.
        first.send({"type": "presence", "player_color": "3E8AC4", "outfit_color": "508040",
                    "target_client_id": joined["client_id"]})
        received = third.expect_type("presence")
        self.assertEqual(received["player_color"], "3E8AC4")
        self.assertEqual(received["outfit_color"], "508040")

    def test_three_client_room_and_disconnect(self) -> None:
        first, first_welcome = self.join("First", "three-clients")
        self.assertEqual(first_welcome["peers"], [])

        second, second_welcome = self.join("Second", "three-clients")
        first_joined = first.expect_type("peer_joined")
        self.assertEqual(first_joined["client_id"], second_welcome["client_id"])
        self.assertEqual(second_welcome["peers"][0]["client_id"], first_welcome["client_id"])

        third, third_welcome = self.join("Third", "three-clients")
        self.assertEqual(len(third_welcome["peers"]), 2)
        first.expect_type("peer_joined")
        second.expect_type("peer_joined")

        third.close()
        self.clients.remove(third)
        self.assertEqual(first.expect_type("peer_left")["client_id"], third_welcome["client_id"])
        self.assertEqual(second.expect_type("peer_left")["client_id"], third_welcome["client_id"])

    def test_only_lobby_owner_can_kick_member(self) -> None:
        owner, _ = self.join("Owner", "owner-kick")
        member, member_welcome = self.join("Member", "owner-kick")
        owner.expect_type("peer_joined")

        member.send({"type": "kick", "target_client_id": "client_not_real"})
        member.expect_error("owner_only")

        owner.send({"type": "kick", "target_client_id": member_welcome["client_id"]})
        kicked = member.expect_type("kicked")
        self.assertEqual(kicked["reason"], "removed_by_host")
        self.assertEqual(
            owner.expect_type("peer_left", 4.0)["client_id"],
            member_welcome["client_id"],
        )

    def test_fragmented_and_coalesced_input(self) -> None:
        client = self.client()
        prepared = client.prepare_hello(hello("Fragmented", "framing", action="create"))
        encoded = json.dumps(prepared).encode("utf-8") + b"\n"
        for byte in encoded:
            client.send_bytes(bytes([byte]))
        client.expect_type("welcome")

        client.send_bytes(b'{"type":"ping"}\n{"type":"ping"}\n')
        client.expect_type("pong")
        client.expect_type("pong")

    def test_lobby_code_duplicate_name_and_repeated_hello(self) -> None:
        first, first_welcome = self.join("SameName", "validation")

        wrong = self.client()
        wrong.send(hello("Other", "validation", "wrong-code"))
        wrong.expect_error("bad_lobby_code")

        duplicate = self.client()
        duplicate.send(hello("samename", "validation"))
        duplicate_welcome = duplicate.expect_type("welcome")
        self.assertNotEqual(duplicate_welcome["client_id"], first_welcome["client_id"])
        joined = first.expect_type("peer_joined")
        self.assertEqual(joined["client_id"], duplicate_welcome["client_id"])
        self.assertEqual(joined["name"], "samename")

        first.send(hello("Moved", "other-room"))
        first.expect_error("already_joined")

    def test_lobby_proof_cannot_be_replayed(self) -> None:
        host = self.client()
        created = host.prepare_hello(hello("Host", "replay-test", action="create"))
        host.send(created)
        host.expect_type("welcome")
        replay = self.client()
        attempted = replay.prepare_hello(hello("Replay", "replay-test"))
        attempted["code_proof"] = created["code_proof"]
        replay.send(attempted)
        replay.expect_error("bad_lobby_code")

    def test_routing_and_sender_identity(self) -> None:
        first, first_welcome = self.join("Sender", "routing")
        second, second_welcome = self.join("Receiver", "routing")
        first.expect_type("peer_joined")

        first.send(
            {
                "type": "progression_state",
                "client_id": "spoofed",
                "stage": "F_SP108",
                "room": 1,
            }
        )
        routed = second.expect_type("progression_state")
        self.assertEqual(routed["client_id"], first_welcome["client_id"])

        first.send(
            {
                "type": "save_snapshot",
                "target_client_id": second_welcome["client_id"],
                "full_state": {"marker": 7},
            }
        )
        targeted = second.expect_type("save_snapshot")
        self.assertEqual(targeted["client_id"], first_welcome["client_id"])
        self.assertEqual(targeted["full_state"]["marker"], 7)

        first.send(
            {
                "type": "sync_request",
                "target_client_id": second_welcome["client_id"],
                "request_id": "manual-1",
            }
        )
        sync_request = second.expect_type("sync_request")
        self.assertEqual(sync_request["client_id"], first_welcome["client_id"])
        self.assertEqual(sync_request["request_id"], "manual-1")

        for message_type in HOST_CONTROL_TYPES:
            first.send({"type": message_type, "enabled": True})
            first.expect_error("unknown_message")

    def test_complete_protocol_4_gameplay_inventory_routes(self) -> None:
        sender, sender_welcome = self.join("InventorySender", "inventory")
        receiver, _ = self.join("InventoryReceiver", "inventory")
        sender.expect_type("peer_joined")

        for sequence, message_type in enumerate(GAMEPLAY_ROUTE_TYPES):
            sender.send({"type": message_type, "inventory_sequence": sequence})
            routed = receiver.expect_type(message_type)
            self.assertEqual(routed["client_id"], sender_welcome["client_id"])
            self.assertEqual(routed["inventory_sequence"], sequence)

    def test_target_cannot_cross_room_boundary(self) -> None:
        sender, _ = self.join("RoomOne", "target-room-one")
        outsider, outsider_welcome = self.join("RoomTwo", "target-room-two")
        sender.send(
            {
                "type": "save_snapshot",
                "target_client_id": outsider_welcome["client_id"],
            }
        )
        sender.expect_error("unknown_target")

        outsider.send({"type": "ping"})
        outsider.expect_type("pong")

    def test_peer_fallback_fanout_identity_and_receipt(self) -> None:
        sender, sw = self.join("Sender", "peer-fanout")
        first, fw = self.join("First", "peer-fanout")
        second, tw = self.join("Second", "peer-fanout")
        sender.expect_type("peer_joined")
        sender.expect_type("peer_joined")
        first.expect_type("peer_joined")
        body = {"generation": 0, "payload": {"type": "pvp_hit", "damage": 4}}
        sender.send({"type": "peer_reliable", "client_id": "forged",
                     "recipients": [{"id": fw["client_id"], "sequence": 1},
                                    {"id": tw["client_id"], "sequence": 7}], "body": body})
        for client, sequence in [(first, 1), (second, 7)]:
            message = client.expect_type("peer_reliable")
            self.assertEqual(message["client_id"], sw["client_id"])
            self.assertEqual(message["frame"], {"sequence": sequence, "body": body})
        first.send({"type": "peer_receipt", "client_id": "forged", "target_client_id": sw["client_id"], "frame": {"ack": 1}})
        receipt = sender.expect_type("peer_receipt")
        self.assertEqual(receipt["client_id"], fw["client_id"])
        self.assertEqual(receipt["frame"], {"ack": 1})

    def test_peer_fallback_rejects_cross_room(self) -> None:
        sender, _ = self.join("Sender", "peer-private")
        outsider, ow = self.join("Outsider", "peer-other")
        sender.send({"type": "peer_reliable", "recipients": [{"id": ow["client_id"], "sequence": 1}],
                     "body": {"generation": 0, "payload": {"type": "pvp_hit"}}})
        sender.expect_error("invalid_peer_delivery")
        outsider.send({"type": "ping"})
        outsider.expect_type("pong")

    def test_reliable_deduplication_and_ack(self) -> None:
        first, _ = self.join("ReliableSender", "reliable")
        second, _ = self.join("ReliableReceiver", "reliable")
        first.expect_type("peer_joined")

        payload = {"type": "reliable", "sequence": 9, "state": {"value": 3}}
        first.send(payload)
        second.expect_type("reliable")
        self.assertEqual(first.expect_type("ack")["sequence"], 9)

        first.send(payload)
        with self.assertRaises(TimeoutError):
            second.receive(0.15)

    def test_slow_reader_receives_complete_queued_lines(self) -> None:
        sender, _ = self.join("BurstSender", "queued-output")
        receiver, _ = self.join("BurstReceiver", "queued-output")
        sender.expect_type("peer_joined")

        body = "x" * 16000
        count = 128
        for sequence in range(count):
            sender.send(
                {
                    "type": "presence",
                    "sequence": sequence,
                    "padding": body,
                }
            )

        received = [receiver.expect_type("presence", 5.0) for _ in range(count)]
        self.assertEqual([message["sequence"] for message in received], list(range(count)))
        self.assertTrue(all(message["padding"] == body for message in received))

    def test_hello_timeout_returns_error_then_closes(self) -> None:
        client = self.client()
        client.expect_error("hello_timeout", 4.0)
        with self.assertRaises(ConnectionError):
            client.receive(1.0)

    def test_input_line_limit_returns_error_then_closes(self) -> None:
        # Authenticate first so the deliberately short hello deadline does not
        # race a paced half-megabyte transfer. Hello timeout is tested separately.
        client, _ = self.join("Oversize", "oversize-line")
        client.send_bytes(b"x" * (512 * 1024 + 2048 + 1) + b"\n")
        client.expect_error("message_too_large", 10.0)
        with self.assertRaises(ConnectionError):
            client.receive(1.0)

    def test_pre_hello_and_malformed_messages_are_rejected(self) -> None:
        client = self.client()
        client.send({"type": "presence"})
        client.expect_error("expected_hello")
        client.send_bytes(b"{broken json}\n")
        client.expect_error("invalid_json")
        client.send(hello("Recovered", "validation-recovery", action="create"))
        client.expect_type("welcome")

    def test_explicit_create_join_and_code_rules(self) -> None:
        missing = self.client()
        missing.send(hello("Missing", "does-not-exist"))
        missing.expect_error("lobby_not_found")

        short = self.client()
        with self.assertRaises(ValueError):
            short.send(hello("Short", "short-password", "12345", action="create"))

        owner = self.client()
        owner.send(hello("Owner", "explicit-room", action="create"))
        owner_welcome = owner.expect_type("welcome")
        self.assertEqual(owner_welcome["owner_client_id"], owner_welcome["client_id"])

        duplicate_room = self.client()
        duplicate_room.send(hello("OtherOwner", "explicit-room", action="create"))
        duplicate_room.expect_error("lobby_exists")

    def test_owner_settings_and_owner_only_enforcement(self) -> None:
        owner = self.client()
        owner.send(
            hello(
                "Owner",
                "owner-settings",
                action="create",
                settings={
                    "dummy_model": False,
                    "sync_flags": True,
                    "sync_world": True,
                    "remote_collision": False,
                    "pvp": True,
                },
            )
        )
        owner_welcome = owner.expect_type("welcome")
        self.assertFalse(owner_welcome["settings"]["dummy_model"])
        self.assertTrue(owner_welcome["settings"]["sync_world"])
        self.assertFalse(owner_welcome["settings"]["remote_collision"])
        self.assertFalse(owner_welcome["settings"]["pvp"])

        member = self.client()
        member.send(hello("Member", "owner-settings"))
        member_welcome = member.expect_type("welcome")
        owner.expect_type("peer_joined")
        self.assertEqual(member_welcome["settings"], owner_welcome["settings"])

        member.send(
            {
                "type": "room_settings",
                "settings": {"sync_flags": False},
            }
        )
        member.expect_error("owner_only")

        owner.send(
            {
                "type": "room_settings",
                "settings": {
                    "dummy_model": True,
                    "sync_flags": False,
                    "sync_world": False,
                    "remote_collision": True,
                    "pvp": True,
                },
            }
        )
        owner_update = owner.expect_type("room_settings")
        member_update = member.expect_type("room_settings")
        self.assertEqual(owner_update["settings"], member_update["settings"])
        self.assertTrue(member_update["settings"]["pvp"])
        self.assertFalse(member_update["settings"]["sync_flags"])

    def test_owner_transfers_to_oldest_remaining_member(self) -> None:
        owner, owner_welcome = self.join("Owner", "owner-transfer")
        second, second_welcome = self.join("Second", "owner-transfer")
        owner.expect_type("peer_joined")
        third, _ = self.join("Third", "owner-transfer")
        owner.expect_type("peer_joined")
        second.expect_type("peer_joined")

        owner.close()
        self.clients.remove(owner)
        self.assertEqual(second.expect_type("peer_left")["client_id"], owner_welcome["client_id"])
        owner_changed = second.expect_type("owner_changed")
        self.assertEqual(owner_changed["owner_client_id"], second_welcome["client_id"])
        third.expect_type("peer_left")
        self.assertEqual(
            third.expect_type("owner_changed")["owner_client_id"],
            second_welcome["client_id"],
        )

        second.send(
            {
                "type": "room_settings",
                "settings": {"sync_world": True},
            }
        )
        self.assertTrue(second.expect_type("room_settings")["settings"]["sync_world"])
        self.assertTrue(third.expect_type("room_settings")["settings"]["sync_world"])

    def test_semantic_visual_capability_requires_every_member(self) -> None:
        owner = self.client()
        owner.send(hello("Owner", "semantic-capability", action="create"))
        owner_welcome = owner.expect_type("welcome")
        self.assertTrue(owner_welcome["semantic_visuals_ready"])
        self.assertTrue(owner_welcome["snapshot_deltas_ready"])

        legacy = self.client()
        legacy.send(hello("Legacy", "semantic-capability", semantic_visuals=None,
                          snapshot_deltas=None))
        legacy_welcome = legacy.expect_type("welcome")
        joined = owner.expect_type("peer_joined")
        self.assertFalse(legacy_welcome["semantic_visuals_ready"])
        self.assertFalse(joined["semantic_visuals_ready"])
        self.assertFalse(legacy_welcome["snapshot_deltas_ready"])
        self.assertFalse(joined["snapshot_deltas_ready"])

        owner.register_udp(owner_welcome)
        legacy.register_udp(legacy_welcome)
        semantic_pose = udp_packet(
            7, owner_welcome["client_id"], b"must-not-reach-legacy", 1
        )
        owner.udp.sendto(semantic_pose, ("127.0.0.1", self.relay.port))
        with self.assertRaises(socket.timeout):
            legacy.udp.recvfrom(2048)

        legacy.close()
        self.clients.remove(legacy)
        left = owner.expect_type("peer_left")
        self.assertTrue(left["semantic_visuals_ready"])
        self.assertTrue(left["snapshot_deltas_ready"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", required=True, type=Path)
    parser.add_argument("--udp-library", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    if not args.relay.is_file():
        parser.error(f"relay executable does not exist: {args.relay}")

    ReliableSocket.configure(args.udp_library.resolve())
    RelayTests.executable = args.relay.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RelayTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
