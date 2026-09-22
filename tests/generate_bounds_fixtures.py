#!/usr/bin/env python3
"""Malformed-input regression fixtures for the native PE/CLI/IL parser.

Uses only the supplied real Core.dll as the seed and never changes that file.
The output files contain game code and are development artifacts, not a release.
Requires dnfile solely to obtain source field locations for controlled mutations.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
from pathlib import Path
import dnfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    source = args.original.read_bytes()
    pe = dnfile.dnPE(data=source, clr_lazy_load=True)
    args.out.mkdir(parents=True, exist_ok=True)
    cases = []

    def save(name, data, detail):
        path = args.out / (name + '.dll')
        path.write_bytes(data)
        cases.append(dict(name=name, file=path.name, expect='Unsupported',
                          detail=detail, sha256=hashlib.sha256(data).hexdigest(),
                          upper_offset=None))

    def change(name, offset, value, detail, fmt='<I'):
        data = bytearray(source)
        struct.pack_into(fmt, data, offset, value)
        save(name, data, detail)

    metadata = pe.net.metadata
    streams = metadata.streams
    tables = pe.net.mdtables
    md_start = metadata.struct.get_file_offset()
    for n in (0, 1, 63, 127, 255, 511, md_start + 4,
              md_start + 31, streams[b'#~'].file_offset + 23,
              streams[b'#Strings'].file_offset + 1,
              streams[b'#Blob'].file_offset + 1):
        save('truncated_' + str(n), source[:n], 'File truncated at a structural boundary')
    change('pe_header_pointer_outside_file', 0x3c, 0xfffffff0, 'e_lfanew outside file')
    change('metadata_rva_outside_sections', pe.net.struct.get_field_absolute_offset('MetaDataRva'),
           0xfffffff0, 'CLR metadata RVA outside mapped sections')
    change('metadata_size_outside_file', pe.net.struct.get_field_absolute_offset('MetaDataSize'),
           0xfffffff0, 'CLR metadata size extends outside file')
    change('metadata_version_length_overflow', metadata.struct.get_field_absolute_offset('VersionLength'),
           0xfffffff0, 'Metadata version string length overflows bounds')
    for name in (b'#~', b'#Strings', b'#Blob'):
        stream = streams[name]
        label = name.decode().replace('#', '').replace('~', 'Tables')
        change(label + '_stream_offset_outside_metadata', stream.struct.get_field_absolute_offset('Offset'),
               0xfffffff0, 'Metadata stream offset exceeds root')
        change(label + '_stream_size_outside_metadata', stream.struct.get_field_absolute_offset('Size'),
               0xfffffff0, 'Metadata stream length exceeds root')
    # The first present table is Module; its count begins after the 24-byte header.
    change('table_row_count_overflow', streams[b'#~'].file_offset + 24, 0xffffffff,
           'Metadata table row count cannot fit the table stream')

    target_type = next(t for t in tables.TypeDef if str(t.TypeName) == 'SettingsPanelGraphics')
    getter = next(m.row for m in target_type.MethodList if str(m.row.Name) == 'get_IsScreenAllowableRatio')
    helper = next(m.row for m in target_type.MethodList if str(m.row.Name) == 'IsAllowableRatio')
    change('getter_name_outside_string_heap', getter.struct.get_field_absolute_offset('Name_StringIndex'),
           0xffffffff, 'Target method name index outside string heap')
    change('getter_signature_outside_blob_heap', getter.struct.get_field_absolute_offset('Signature_BlobIndex'),
           0xffffffff, 'Target method signature index outside blob heap')
    change('getter_rva_outside_file', getter.struct.get_field_absolute_offset('Rva'),
           0xffffffff, 'Method body RVA outside file sections')
    change('type_name_outside_string_heap', target_type.struct.get_field_absolute_offset('TypeName_StringIndex'),
           0xffffffff, 'Target type name index outside string heap')

    getter_offset = pe.get_offset_from_rva(getter.Rva)
    helper_offset = pe.get_offset_from_rva(helper.Rva)
    change('getter_operand_bad_member_token', getter_offset + 12, 0x0affffff,
           'MemberRef call token outside table')
    change('helper_code_size_overflow', helper_offset + 4, 0xffffffff,
           'Fat IL code size exceeds bounds')
    # Helper's existing lower-bound branch has a one-byte displacement.
    helper_code = source[helper_offset + 12:helper_offset + 40]
    branch_index = helper_code.index(b'\x37')
    change('helper_branch_outside_body', helper_offset + 12 + branch_index + 1, 0x7f,
           'Branch target outside IL body', fmt='<B')
    change('helper_local_signature_bad_token', helper_offset + 8, 0x11ffffff,
           'Local signature token outside StandAloneSig table')
    manifest = dict(source_sha256=hashlib.sha256(source).hexdigest(),
                    synthetic_notice='Malformed synthetic files derived from the one supplied Arena release.',
                    cases=cases)
    (args.out / 'cases.json').write_text(json.dumps(manifest, indent=2) + '\n')
    assert args.original.read_bytes() == source
    print(f'Generated {len(cases)} malformed fixtures; original unchanged.')


if __name__ == '__main__':
    main()
