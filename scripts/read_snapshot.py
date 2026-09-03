#!/usr/bin/env python3
"""Read metadata from a torch snapshot PNG and print the TORCH_CAM command to reproduce it."""
import struct
import sys
from PIL import Image

def read_png_metadata(path):
    """Extract tEXt chunks from a PNG file."""
    metadata = {}
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        print(f"Error: {path} is not a valid PNG file")
        return metadata
    pos = 8
    while pos < len(data):
        if pos + 8 > len(data):
            break
        length = struct.unpack('>I', data[pos:pos+4])[0]
        chunk_type = data[pos+4:pos+8].decode('ascii', errors='replace')
        if chunk_type == 'tEXt':
            text_data = data[pos+8:pos+8+length]
            # tEXt format: keyword\0value
            null_idx = text_data.find(b'\x00')
            if null_idx >= 0:
                keyword = text_data[:null_idx].decode('latin-1')
                value = text_data[null_idx+1:].decode('latin-1')
                metadata[keyword] = value
        pos += 12 + length
        if chunk_type == 'IEND':
            break
    return metadata

def parse_metadata(meta_str):
    """Parse the TorchMapper metadata string into a dict."""
    result = {}
    parts = meta_str.split(';')
    for part in parts:
        if '=' in part:
            key, value = part.split('=', 1)
            result[key] = value
    return result

def main():
    if len(sys.argv) < 2:
        print("Usage: read_snapshot.py <snapshot.png>")
        print("       Extracts camera coordinates, mission, and terrain info from a PNG screenshot")
        sys.exit(1)

    path = sys.argv[1]
    metadata = read_png_metadata(path)

    if 'TorchMapper' in metadata:
        parsed = parse_metadata(metadata['TorchMapper'])
        print("=== Snapshot Metadata ===")
        for key, value in parsed.items():
            print(f"  {key}: {value}")

        print()
        print("=== Reproduction Command ===")
        if 'TORCH_CAM' in parsed and 'MISSION' in parsed:
            mission = parsed['MISSION']
            print(f"cd build && env TORCH_CAM=\"{parsed['TORCH_CAM']}\" ./torch -mapper {mission} -data ~/t2-linux -debug")

        if 'TEXCOORD' in parsed:
            print(f"  Terrain offset: {parsed['TEXCOORD']}")
        if 'SIZE' in parsed:
            print(f"  Terrain size: {parsed['SIZE']}")
        if 'SQ' in parsed:
            print(f"  Square size: {parsed['SQ']}")
    else:
        # Try PIL as fallback (some PNGs store tEXt in info dict)
        img = Image.open(path)
        if img.info:
            print("=== PIL Info ===")
            for k, v in img.info.items():
                print(f"  {k}: {v}")
        else:
            print("No metadata found in this PNG.")
            print(f"Image size: {img.size}, mode: {img.mode}")

if __name__ == '__main__':
    main()
