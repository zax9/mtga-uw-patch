#!/usr/bin/env python3
"""Synthetic parser/patcher robustness fixtures, not historical Arena releases.
Requires dnfile for development only. Never modifies the source binary.
"""
import argparse, hashlib, json, struct
from pathlib import Path
import dnfile

def sha(data): return hashlib.sha256(data).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--original',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
 args=ap.parse_args(); original=args.original.read_bytes(); p=dnfile.dnPE(data=original,clr_lazy_load=True)
 matches=[]
 for t in p.net.mdtables.TypeDef:
  if str(t.TypeName)=='SettingsPanelGraphics':
   for m in t.MethodList:
    if str(m.row.Name)=='get_IsScreenAllowableRatio': matches.append(m.row)
 assert len(matches)==1
 getter=matches[0]; getter_offset=p.get_offset_from_rva(getter.Rva)
 assert original[getter_offset]==0x72
 getter_code=original[getter_offset+1:getter_offset+29]
 assert getter_code[:10]==bytes.fromhex('221f85cb3f2214aee73f')
 helper_token=struct.unpack_from('<I',getter_code,23)[0]
 helper=p.net.mdtables.MethodDef.rows[(helper_token&0xffffff)-1]
 helper_offset=p.get_offset_from_rva(helper.Rva)
 assert original[helper_offset+12:helper_offset+15]==bytes.fromhex('04055b')
 width_token=struct.unpack_from('<I',getter_code,11)[0];height_token=struct.unpack_from('<I',getter_code,17)[0]
 width=p.net.mdtables.MemberRef.rows[(width_token&0xffffff)-1]
 height=p.net.mdtables.MemberRef.rows[(height_token&0xffffff)-1]
 args.out.mkdir(parents=True,exist_ok=True); cases=[]
 def save(name,data,expect,detail,upper_offset=None):
  file=args.out/(name+'.dll');file.write_bytes(data)
  cases.append(dict(name=name,file=file.name,expect=expect,detail=detail,sha256=sha(data),upper_offset=upper_offset))
 def base(): return bytearray(original)
 def pack(data,off,value): struct.pack_into('<I',data,off,value)
 def relocate(data,code):
  # Extend the last readable section and point only the MethodDef RVA at a
  # new tiny-header body. No new section header or code token is hardcoded.
  s=p.sections[-1]; body=bytes([(len(code)<<2)|2])+bytes(code)
  raw_off=len(data);pad=(-raw_off)%4;data+=b'\0'*pad;raw_off=len(data)
  rva=s.VirtualAddress+(raw_off-s.PointerToRawData)
  data+=body;raw_size=((len(data)-s.PointerToRawData+511)//512)*512
  data+=b'\0'*(s.PointerToRawData+raw_size-len(data))
  vsize=(raw_off-s.PointerToRawData)+len(body)
  pack(data,s.get_file_offset()+8,vsize);pack(data,s.get_file_offset()+16,raw_size)
  sizeimage=((s.VirtualAddress+vsize+p.OPTIONAL_HEADER.SectionAlignment-1)//p.OPTIONAL_HEADER.SectionAlignment)*p.OPTIONAL_HEADER.SectionAlignment
  pack(data,p.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfImage'),sizeimage)
  pack(data,getter.struct.get_field_absolute_offset('Rva'),rva)
  return raw_off+1
 def remap(data,code):
  a=width.struct.get_file_offset();b=height.struct.get_file_offset();size=width.struct.sizeof()
  data[a:a+size],data[b:b+size]=data[b:b+size],data[a:a+size]
  code=bytearray(code);pack(code,11,height_token);pack(code,17,width_token);return code
 def nops(code):
  # One nop before every original instruction; getter has no branches.
  splits=(0,5,10,15,16,21,22,27,28)
  return b''.join(b'\0'+code[a:b] for a,b in zip(splits,splits[1:]))
 save('supplied_original',original,'Patchable','User supplied real client',getter_offset+7)
 d=base();d[getter_offset+7:getter_offset+11]=struct.pack('<f',2.4)
 save('supplied_v1_patched',d,'AlreadyCompatible','Exactly the previous v1 four-byte change',getter_offset+7)
 d=base();pack(d,p.FILE_HEADER.get_field_absolute_offset('TimeDateStamp'),p.FILE_HEADER.TimeDateStamp^12345)
 save('different_build_header',d,'Patchable','Synthetic PE timestamp change; whole-file hash differs',getter_offset+7)
 d=base();o=relocate(d,getter_code)
 save('relocated_method',d,'Patchable','Getter RVA and byte offset moved to end of PE',o+6)
 d=base();c=remap(d,getter_code);d[getter_offset+1:getter_offset+29]=c
 save('remapped_member_tokens',d,'Patchable','Width/height MemberRef rows and getter tokens swapped',getter_offset+7)
 d=base();o=relocate(d,nops(getter_code))
 save('relocated_method_with_nops',d,'Patchable','Getter body moved and harmless no-ops inserted',o+8)
 d=base();c=remap(d,getter_code);o=relocate(d,nops(c))
 save('relocated_nops_remapped_tokens',d,'Patchable','Combined synthetic layout/compiler/token variation',o+8)
 d=base();pack(d,getter.struct.get_field_absolute_offset('Name_StringIndex'),helper.struct.Name_StringIndex)
 save('missing_getter',d,'Unsupported','Target method name missing')
 d=base(); other=p.net.mdtables.MethodDef.rows[0x23c9-1]
 pack(d,other.struct.get_field_absolute_offset('Name_StringIndex'),getter.struct.Name_StringIndex)
 pack(d,other.struct.get_field_absolute_offset('Signature_BlobIndex'),getter.struct.Signature_BlobIndex)
 save('ambiguous_getter',d,'Unsupported','Two methods in target type have target getter name/signature')
 d=base();d[helper_offset+14]=0x5a
 save('helper_multiplies',d,'Unsupported','IsAllowableRatio changed from division to multiplication')
 d=base();d[getter_offset+16]=0x6c
 save('getter_uses_double_conversion',d,'Unsupported','Getter changed conv.r4 to conv.r8')
 d=base();pack(d,getter_offset+12,height_token)
 save('getter_uses_height_twice',d,'Unsupported','Getter calls height in place of width')
 d=base();d[getter_offset+7:getter_offset+11]=struct.pack('<f',float('nan'))
 save('nan_upper_bound',d,'Unsupported','Upper limit is non-finite')
 d=base();pack(d,p.net.struct.get_field_absolute_offset('Flags'),p.net.struct.Flags|8)
 save('strong_name_signed_flag',d,'Unsupported','CLR strong-name-signed flag set')
 d=base();directory=p.OPTIONAL_HEADER.DATA_DIRECTORY[4]
 pack(d,directory.get_field_absolute_offset('VirtualAddress'),len(d));pack(d,directory.get_field_absolute_offset('Size'),8);d+=b'\x08\0\0\0\0\x02\x02\0'
 save('authenticode_directory',d,'Unsupported','PE certificate directory present')
 save('truncated_pe',original[:300],'Unsupported','Truncated PE')
 save('not_a_pe',b'not a PE file','Unsupported','Non-PE data')
 (args.out/'cases.json').write_text(json.dumps(dict(source_sha256=sha(original),synthetic_notice='Only supplied_original/supplied_v1_patched reflect a real supplied build; other DLLs are synthetic robustness fixtures, not historical releases.',cases=cases),indent=2)+'\n')
 print(f'Generated {len(cases)} fixtures; source unchanged: {sha(args.original.read_bytes())==sha(original)}')
if __name__=='__main__': main()
