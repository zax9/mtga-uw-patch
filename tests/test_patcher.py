#!/usr/bin/env python3
"""Integration checks for the v3 native command-line engine.
Copies real/synthetic DLLs into disposable installations. Does not execute Arena.
"""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

def digest(data): return hashlib.sha256(data).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--patcher',type=Path,required=True)
 ap.add_argument('--fixtures',type=Path,required=True);ap.add_argument('--report',type=Path)
 args=ap.parse_args();args.patcher=args.patcher.resolve()
 cases=json.loads((args.fixtures/'cases.json').read_text())['cases']
 fixtures={x['name']:x for x in cases}
 def load(name):return (args.fixtures/fixtures[name]['file']).read_bytes()
 original=load('supplied_original');patched=load('supplied_v1_patched');passed=[]
 with tempfile.TemporaryDirectory(prefix='mtga-patcher-v3-test-') as work:
  root=Path(work);game=root/'game with spaces';target_dir=game/'MTGA_Data'/'Managed';target_dir.mkdir(parents=True)
  target=target_dir/'Core.dll';lock=target_dir/'.Core.dll.ultrawide-patch.lock'
  def backup_path(data):return target_dir/('Core.dll.ultrawide.'+digest(data)+'.original.bak')
  def reset(data=original):
   for item in target_dir.iterdir():item.unlink()
   target.write_bytes(data)
  def snapshot():return {x.name:digest(x.read_bytes()) for x in target_dir.iterdir()}
  def run(action,success=True,extra=()):
   r=subprocess.run([str(args.patcher),action,'--game-path',str(game),*extra],text=True,capture_output=True,timeout=30)
   assert (r.returncode==0)==success,f'{action} exit={r.returncode}\n{r.stdout}\n{r.stderr}'
   return r.stdout+r.stderr
  def ok(name):passed.append(name);print('PASS:',name,flush=True)
  reset();before=snapshot();assert 'Status: Patchable' in run('status');assert snapshot()==before
  ok('Original status is read-only')
  run('install');backup=backup_path(original)
  assert target.read_bytes()==patched and backup.read_bytes()==original
  assert len(snapshot())==2
  ok('Install changes exactly the intended four bytes; verifies and retains original backup; removes temporaries')
  before=snapshot();mtime=backup.stat().st_mtime_ns;run('install');assert snapshot()==before and backup.stat().st_mtime_ns==mtime
  ok('Repeated install is a no-op and preserves backup metadata')
  assert 'Status: AlreadyCompatible' in run('status') and snapshot()==before
  ok('Patched status is read-only and identifies restorable backup')
  run('restore');assert target.read_bytes()==original and backup.read_bytes()==original
  ok('Restore reproduces original byte-for-byte')
  before=snapshot();run('restore');assert snapshot()==before
  ok('Repeated restore is a no-op')
  run('install');assert target.read_bytes()==patched and backup.stat().st_mtime_ns==mtime
  ok('Reinstall validates and reuses original backup without overwrite')
  reset(patched);backup_path(original).write_bytes(original);run('restore');assert target.read_bytes()==original
  ok('Restores patch installed by v1 using its existing original backup name')
  reset(patched);before=snapshot();run('restore',False);assert snapshot()==before
  ok('Missing backup blocks restore without modifying target')
  backup_path(original).write_bytes(b'corrupt');before=snapshot();run('restore',False);assert snapshot()==before
  ok('Corrupt backup blocks restore and is retained untouched')
  reset();backup_path(original).write_bytes(b'corrupt');before=snapshot();run('install',False);assert snapshot()==before
  ok('Corrupt existing backup blocks installation without overwrite')
  reset();lock.write_bytes(b'another operation');before=snapshot();run('install',False);assert snapshot()==before
  ok('Existing operation lock blocks installation and remains untouched')
  reset(patched);backup_path(original).write_bytes(original);changed=bytearray(patched);changed[-1]^=1;target.write_bytes(changed)
  before=snapshot();run('restore',False);assert snapshot()==before
  ok('Unrelated post-patch DLL change blocks restore from stale original')
  new_original=load('different_build_header');new_patched=bytearray(new_original)
  off=fixtures['different_build_header']['upper_offset'];new_patched[off:off+4]=struct.pack('<f',2.4)
  reset(new_patched);backup_path(original).write_bytes(original);before=snapshot();run('restore',False);assert snapshot()==before
  ok('Backup from older build cannot overwrite newer patched build')
  target.write_bytes(new_original);run('install');assert target.read_bytes()==new_patched and backup_path(new_original).read_bytes()==new_original
  assert backup_path(original).read_bytes()==original
  run('restore');assert target.read_bytes()==new_original
  ok('Synthetic changed build installs/restores using its own backup and preserves older build backup')
  reset(patched);backup_path(original).write_bytes(original);alternative=bytearray(original)
  off=fixtures['supplied_original']['upper_offset'];alternative[off:off+4]=struct.pack('<f',1.9);backup_path(alternative).write_bytes(alternative)
  before=snapshot();run('restore',False);assert snapshot()==before
  ok('Ambiguous different originals yielding same patched image block restore')
  for name in ('relocated_method','remapped_member_tokens','relocated_method_with_nops','relocated_nops_remapped_tokens'):
   image=load(name);reset(image);offset=fixtures[name]['upper_offset'];expected=bytearray(image);expected[offset:offset+4]=struct.pack('<f',2.4)
   run('install');assert target.read_bytes()==expected and backup_path(image).read_bytes()==image
   run('restore');assert target.read_bytes()==image
   ok('Install/restore synthetic '+name+' with dynamically located operand')
  for name in ('missing_getter','ambiguous_getter','helper_multiplies','getter_uses_height_twice','strong_name_signed_flag','authenticode_directory','not_a_pe'):
   reset(load(name));before=snapshot()
   for action in ('status','install','restore'):
    run(action,False);assert snapshot()==before
   ok('All actions reject '+name+' without mutation')
  sleep=shutil.which('sleep') if sys.platform!='win32' else None
  if sleep:
   for name in ('MTGA','MTGALauncher','MTGAUpdater'):
    reset();exe=root/name;shutil.copyfile(sleep,exe);exe.chmod(0o700);process=subprocess.Popen([str(exe),'30'])
    try:
     before=snapshot();output=run('install',False);assert 'Close Arena and its launcher/updater' in output,output;assert snapshot()==before
     ok('Running '+name+' process blocks installation')
    finally:process.terminate();process.wait(timeout=5)
  reset();before=snapshot();run('install',False,extra=('restore',));assert snapshot()==before
  ok('Conflicting CLI actions are rejected before mutation')
  reset();before=snapshot();run('status',False,extra=('--unknown',));assert snapshot()==before
  ok('Unknown CLI options are rejected before mutation')
 assert load('supplied_original')==original
 report=dict(checks=len(passed),passed=passed,runtime='native C++ executable',platform=sys.platform,limitations=['Native C++ engine tested on Linux; Windows GUI launch, UAC, ACLs, antivirus, updater behavior and fullscreen graphics were not tested.','Other than supplied original and its exact v1 patch, fixtures are synthetic; no other real Arena version was tested.'])
 if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
 print(f'\n{len(passed)} integration checks passed. Original fixture unchanged.')
if __name__=='__main__':main()
