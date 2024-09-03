#!/usr/bin/env python3
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
from gguf.gguf_reader import GGUFReader, GGUFValueType


def read_field(reader, name):
    field = reader.fields[name]
    if not field.types:
        return None
    if field.types[:1] == [GGUFValueType.ARRAY]:
        itype = field.types[-1]
        if itype == GGUFValueType.STRING:
            return [str(bytes(field.parts[idx]), encoding="utf-8") for idx in field.data]
        else:
            return [pv for idx in field.data for pv in field.parts[idx].tolist()]
    elif field.types[0] == GGUFValueType.STRING:
        return str(bytes(field.parts[-1]), encoding="utf-8")
    else:
        assert(field.types[0] in reader.gguf_scalar_to_np)
        return field.parts[-1].tolist()[0]

def print_usage_and_exit(field_names=None):
    print("Usage: read_field.py <path_to_gguf_file> <field_name>", file=sys.stderr)
    if field_names:
        print("Valid field names:\n\t" + "\n\t".join(f'- {n}' for n in field_names), file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    if len(sys.argv) >= 2:
        gguf_file_path = sys.argv[1]
        reader = GGUFReader(gguf_file_path)
        field_names = sorted(reader.fields.keys())
    if len(sys.argv) < 3:
        print_usage_and_exit(field_names)
    field_name = sys.argv[2]
    if field_name not in field_names:
        print_usage_and_exit(field_names)

    reader = GGUFReader(gguf_file_path)
    value = read_field(reader, field_name)
    print(value if isinstance(value, str) else json.dumps(value, indent=2))