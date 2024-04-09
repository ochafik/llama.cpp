#!/usr/bin/env python3
import json
import sys
from pathlib import Path

from gguf.gguf_reader import GGUFReader

sys.path.insert(0, str(Path(__file__).parent.parent))


def read_gguf_file(gguf_file_path):
    """
    Reads and prints key-value pairs and tensor information from a GGUF file in an improved format.

    Parameters:
    - gguf_file_path: Path to the GGUF file.
    """

    reader = GGUFReader(gguf_file_path)

    # List all key-value pairs in a columnized format
    print("Key-Value Pairs:")
    max_key_length = max(len(key) for key in reader.fields.keys())
    max_value_length = 2048

    for key, field in reader.fields.items():
        value_str = json.dumps(field.read())
        if len(value_str) > max_value_length:
            value_str = value_str[0:max_value_length] + "..."
        print(f"{key:{max_key_length}} : {value_str}")
    print("----")

    # List all tensors
    print("Tensors:")
    tensor_info_format = "{:<30} | Shape: {:<15} | Size: {:<12} | Quantization: {}"
    print(tensor_info_format.format("Tensor Name", "Shape", "Size", "Quantization"))
    print("-" * 80)
    for tensor in reader.tensors:
        shape_str = "x".join(map(str, tensor.shape))
        size_str = str(tensor.n_elements)
        quantization_str = tensor.tensor_type.name
        print(tensor_info_format.format(tensor.name, shape_str, size_str, quantization_str))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: reader.py <path_to_gguf_file>")
        sys.exit(1)
    gguf_file_path = sys.argv[1]
    read_gguf_file(gguf_file_path)
