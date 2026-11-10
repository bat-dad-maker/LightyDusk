"""Socket-shaped test helper over the production native reliable UDP adapter."""
import ctypes
import time

class ReliableSocket:
    library = None
    instances = set()

    @classmethod
    def configure(cls, path):
        cls.library = ctypes.CDLL(str(path))
        signatures = {
            'decode': ([ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int], ctypes.c_int),
            'create': ([ctypes.c_int, ctypes.c_char_p], ctypes.c_void_p),
            'destroy': ([ctypes.c_void_p], None),
            'poll': ([ctypes.c_void_p], ctypes.c_int),
            'send': ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int], ctypes.c_int),
            'receive': ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int], ctypes.c_int),
            'send_realtime': ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int], ctypes.c_int),
            'receive_realtime': ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int], ctypes.c_int),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(cls.library, 'udp_test_' + name)
            fn.argtypes, fn.restype = args, result

    @classmethod
    def decode(cls, data):
        data = data.encode() if isinstance(data, str) else bytes(data)
        output = ctypes.create_string_buffer(512 * 1024)
        count = cls.library.udp_test_decode(data, len(data), output, len(output))
        if count < 0: raise ValueError("invalid encoded JSON")
        return output.raw[:count]

    @classmethod
    def pump(cls):
        for instance in tuple(cls.instances):
            instance.state = cls.library.udp_test_poll(instance.handle)

    def __init__(self, port, timeout=2.0, invite_secret='relay-test-endpoint-key'):
        self.timeout = timeout
        self.handle = self.library.udp_test_create(port, invite_secret.encode())
        if not self.handle:
            raise ConnectionError('could not create UDP session')
        self.instances.add(self)
        self.state = 0
        deadline = time.monotonic() + timeout
        while self.state == 0 and time.monotonic() < deadline:
            self.pump()
            time.sleep(0.001)
        if self.state != 1:
            self.close()
            raise ConnectionError('UDP session handshake failed')

    def settimeout(self, timeout):
        self.timeout = timeout

    def close(self):
        if self.handle:
            self.instances.discard(self)
            self.library.udp_test_destroy(self.handle)
            self.handle = None

    def sendall(self, data):
        if self.library.udp_test_send(self.handle, data, len(data)) != len(data):
            raise ConnectionError('reliable UDP queue rejected message')
        self.pump()

    def recv(self, size):
        deadline = time.monotonic() + self.timeout
        buffer = ctypes.create_string_buffer(size)
        while time.monotonic() < deadline:
            self.pump()
            count = self.library.udp_test_receive(self.handle, buffer, size)
            if count >= 0:
                return buffer.raw[:count]
            time.sleep(0.001)
        raise TimeoutError('reliable UDP receive timed out')

    def sendto(self, data, address):
        data = bytes(data)
        result = self.library.udp_test_send_realtime(self.handle, data, len(data))
        self.pump()
        return result

    def recvfrom(self, size):
        deadline = time.monotonic() + min(self.timeout, 0.3)
        buffer = ctypes.create_string_buffer(size)
        while time.monotonic() < deadline:
            self.pump()
            count = self.library.udp_test_receive_realtime(self.handle, buffer, size)
            if count >= 0:
                return buffer.raw[:count], ('127.0.0.1', 0)
            time.sleep(0.001)
        raise TimeoutError('realtime UDP receive timed out')

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
