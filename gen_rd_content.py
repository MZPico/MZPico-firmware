from pathlib import Path

def count_ones(data: bytes) -> int:
    return sum(bin(b).count('1') for b in data)

# Load binary code
code = Path("program.bin").read_bytes()

body_size = len(code)
load_addr = 0x1200
entry_point = 0x1200

# Build the first 8 bytes of the header
header = (
    body_size.to_bytes(2, 'little') +
    load_addr.to_bytes(2, 'little') +
    entry_point.to_bytes(2, 'little') +
    count_ones(code).to_bytes(2, 'little')  # Body CRC = number of 1s in body
)

# Header CRC = number of 1s in header[0:8]
header_crc = count_ones(header)

# Final output
with open("program.rdc", "wb") as f:
    f.write(header)
    f.write(bytes([header_crc]))
    f.write(code)

