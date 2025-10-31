#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path

def generate_c_header(mzf_file: Path, var_name: str) -> str:
    """Generate C header containing a uint8_t array representing the .mzf file."""

    with mzf_file.open('rb') as f:
        dt = f.read()

    # Strip trailing zeros
    data = bytearray(dt.rstrip(b'\x00'))
    total_size = len(data)

    # Modify header: update size stored in bytes 18 & 19
    data[18] = (total_size - 128) & 0xFF
    data[19] = ((total_size - 128) >> 8) & 0xFF

    # Generate C header text
    output = []
    output.append(f"#pragma once\n")
    output.append(f"// Generated from: {mzf_file.name}")
    output.append(f"// Size: {total_size} bytes\n")
    output.append(f"const uint8_t {var_name}[{total_size}] = {{")

    for i in range(0, total_size, 8):
        chunk = data[i:i+8]
        line = ", ".join(f"0x{b:02X}" for b in chunk)
        output.append(f"    {line}," if i + 8 < total_size else f"    {line}")

    output.append("};")
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(description="Convert MZF file to a C header array.")
    parser.add_argument("input_mzf", help="Input .mzf file")
    parser.add_argument("output_h", help="Output .hpp header file")
    args = parser.parse_args()

    input_path = Path(args.input_mzf)
    output_path = Path(args.output_h)

    if not input_path.exists():
        print(f"❌ Error: Input file not found: {input_path}")
        sys.exit(1)

    # Use output filename (without extension) as variable name
    var_name = output_path.stem

    header_text = generate_c_header(input_path, var_name)

    with output_path.open("w") as f:
        f.write(header_text)

    print(f"✅ Header generated: {output_path}")
    print(f"   → Variable name: {var_name}")


if __name__ == "__main__":
    main()
