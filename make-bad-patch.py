import struct
import zlib

PARTITION_SIZE = 0x2000
payload = b"THIS-MUST-BE-IGNORED"

header = struct.pack(
    "<8sHHIIII",
    b"ESPATCH\x00",
    1,                      # format
    28,                     # header size
    5,                      # WRONG BASE deliberately: RC5
    2,                      # P002
    len(payload),
    zlib.crc32(payload) & 0xffffffff,
)

image = header + payload
image += b"\xff" * (PARTITION_SIZE - len(image))

with open("ES-Pet-RC6-P002-BADBASE.bin", "wb") as f:
    f.write(image)

print("Created ES-Pet-RC6-P002-BADBASE.bin")
print(f"Size: {len(image)} bytes")
