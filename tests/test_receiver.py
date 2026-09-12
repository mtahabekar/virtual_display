import importlib.util
from pathlib import Path
import socket
import threading
import unittest

spec = importlib.util.spec_from_file_location("receiver", Path(__file__).resolve().parents[1] / "tools/receiver.py")
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


class ReceiverTests(unittest.TestCase):
    def header(self, size=4, flags=1, stream=1):
        return receiver.HEADER.pack(b"QSTV", 1, 1, stream, 1, 2560, 1440, 60, 1, 123456, size, flags)

    def test_header(self):
        self.assertEqual(len(self.header()), 32)
        self.assertEqual(receiver.parse_header(self.header()), (1, 123456, 4, True))

    def test_invalid_length_flags_and_stream(self):
        for header in (self.header(0), self.header(receiver.MAX_PAYLOAD + 1), self.header(flags=2),
                       self.header(stream=max(receiver.STREAM_IDS) + 1), b"BAD!" + self.header()[4:]):
            with self.assertRaises(ValueError): receiver.parse_header(header)

    def test_fragmented_tcp(self):
        a, b = socket.socketpair()
        try:
            data = self.header() + b"test"
            def send():
                for byte in data: a.sendall(bytes([byte]))
                a.close()
            t = threading.Thread(target=send); t.start()
            header = receiver.read_exact(b, 32)
            self.assertEqual(receiver.parse_header(header)[2], 4)
            self.assertEqual(receiver.read_exact(b, 4), b"test")
            with self.assertRaises(EOFError): receiver.read_exact(b, 1)
            t.join()
        finally: a.close(); b.close()

    def test_annexb_parameter_sets(self):
        self.assertEqual(receiver.annexb_types(b"\0\0\0\1\x67ab\0\0\1\x68cd\0\0\1\x65ef"), {7, 8, 5})
