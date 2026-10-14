import struct
import zlib

PARTITION_SIZE = 0x2000
payload = b"RC6-P001-TEST"

header = struct.pack(
    "<8sHHIIII",
    b"ESPATCH\x00",          # magic
    1,                      # patch format
    28,                     # header size
    6,                      # required base: RC6
    1,                      # patch sequence P001
    len(payload),
    zlib.crc32(payload) & 0xffffffff,
)

image = header + payload
image += b"\xff" * (PARTITION_SIZE - len(image))

with open("ES-Pet-RC6-P001.bin", "wb") as f:
    f.write(image)

print(f"Created ES-Pet-RC6-P001.bin: {len(image)} bytes")
print(f"Payload: {payload!r}")
