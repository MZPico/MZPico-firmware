import struct
import sys

def generate_c_header(mzf_file, var_name="rd"):

    with open(mzf_file, 'rb') as f:
        data = f.read()

    total_size = len(data)
    output = []
    output.append(f"const uint8_t {var_name}[{total_size}] = {{")

    for i in range(0, total_size, 8):
        chunk = data[i:i+8]
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

    print(generate_c_header(sys.argv[1], var_name="basic"))

