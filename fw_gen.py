import struct
import sys

def count_ones(byte_data):
    """Count total number of 1-bits in a byte sequence."""
    return sum(bin(b).count('1') for b in byte_data)

def parse_mzf(filepath):
    """Parse a Sharp MZ-800 MZF file header and body."""
    with open(filepath, 'rb') as f:
        data = f.read()

    if len(data) < 128:
        raise ValueError("File too short to be a valid MZF file")

    header = data[:128]
    body = data[128:]

    file_type = header[0]
    raw_name = header[1:18]
    filename = raw_name.split(b'\x0D', 1)[0].decode('ascii', errors='replace')

    data_size = struct.unpack('<H', header[0x12:0x14])[0]
    load_address = struct.unpack('<H', header[0x14:0x16])[0]
    exec_address = struct.unpack('<H', header[0x16:0x18])[0]

    comment = header[0x24:0x24 + 104].decode('ascii', errors='replace').rstrip('\x00')

    if len(body) != data_size:
        raise ValueError(f"Body size mismatch: header says {data_size}, but actual size is {len(body)}")


    # Trim trailing 0x00 bytes from the body
    trimmed_body = body.rstrip(b'\x00')
    trimmed_size = len(trimmed_body)

    if trimmed_size > data_size:
        raise ValueError(f"Trimmed body size {trimmed_size} is larger than declared size {data_size}")

    data_size = trimmed_size  # Update to match trimmed body
    body = trimmed_body

    return {
        "file_type": file_type,
        "filename": filename,
        "data_size": data_size,
        "load_address": load_address,
        "exec_address": exec_address,
        "comment": comment,
        "body": body
    }

def generate_c_header(mzf_info, var_name="rd"):
    """Generate a C uint8_t array with 9-byte header + body."""
    body = mzf_info["body"]
    data_size = mzf_info["data_size"]
    load_addr = mzf_info["load_address"]
    exec_addr = mzf_info["exec_address"]

    # Calculate CRC values
    body_crc = count_ones(body)
    # Header CRC is number of 1s in first 8 bytes of header (without header CRC itself)
    # We'll pack header without last byte first to compute header CRC
    header_wo_crc = struct.pack('<H', data_size) + struct.pack('<H', load_addr) + struct.pack('<H', exec_addr) + struct.pack('<H', body_crc)
    header_crc = count_ones(header_wo_crc)

    # Now build final 9-byte header
    header = header_wo_crc + struct.pack('B', header_crc)

    assert len(header) == 9, f"Header length expected 9, got {len(header)}"

    # Combine header + body for C array
    full = list(header) + list(body)
    total_size = len(full)

    output = []
    output.append(f"const uint8_t {var_name}[{total_size}] = {{")
    output.append("    // --- Header (9 bytes) ---")

    # Print 9-byte header (1 line only)
    line = ", ".join(f"0x{b:02X}" for b in header)
    output.append(f"    {line},")

    output.append(f"    // --- Program Body ({data_size} bytes) ---")

    # Print body bytes (up to 8 per line)
    for i in range(9, total_size, 8):
        chunk = full[i:i+8]
        line = ", ".join(f"0x{b:02X}" for b in chunk)
        if i + 8 >= total_size:
            output.append(f"    {line}")
        else:
            output.append(f"    {line},")

    output.append("};")
    return "\n".join(output)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python mz800pico_fw_gen.py <file.mzf>")
        sys.exit(1)

    mzf_info = parse_mzf(sys.argv[1])

    print(f"// Filename: {mzf_info['filename']}")
    print(f"// Comment:  {mzf_info['comment']}")
    print(generate_c_header(mzf_info, var_name="firmware"))

