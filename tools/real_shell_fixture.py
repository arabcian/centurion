#!/usr/bin/env python3
"""Developer-only real MainWindow screenshot runner; no live fallback.
Requires an unprivileged x86-64 Linux user, bubblewrap and a native Qt build.
Only the private candidate output directory is writable on the host.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

FIXTURES = ('amd-nvidia', 'intel-nvidia', 'intel-only', 'amd-radeon', 'missing', 'sleeping-nvidia')
BASE_PAGES = ('Home', 'Scenes', 'Optimizations', 'Health')


def fixture(root, name):
    if name not in FIXTURES:
        raise ValueError('unknown fixture')
    for directory in ('sys', 'proc'):
        (root / directory).mkdir(parents=True)
    def put(path, content):
        p = root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content + '\n', encoding='utf-8')
    put('marker', 'centurion real shell fixture v1')
    pages = list(BASE_PAGES)
    if name == 'missing':
        return pages
    amd = name in ('amd-nvidia', 'amd-radeon', 'sleeping-nvidia')
    pages.append('Ryzen Curve' if amd else 'Intel Undervolt')
    put('proc/cpuinfo', 'processor : 0\nvendor_id : ' + ('AuthenticAMD' if amd else 'GenuineIntel') + '\ncpu family : ' + ('26' if amd else '6') + '\nmodel name : Synthetic CPU')
    put('proc/meminfo', 'MemTotal: 67108864 kB\nMemAvailable: 33554432 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB')
    put('proc/sys/kernel/random/boot_id', '11111111-2222-4333-8444-555555555555')
    for key, value in {'sys_vendor':'LENOVO', 'product_version':'Legion 16ARX Fixture', 'product_family':'Legion Fixture', 'bios_version':'FIXTURE'}.items():
        put('sys/class/dmi/id/' + key, value)
    for key, value in {'type':'Battery','status':'Discharging' if name=='sleeping-nvidia' else 'Charging','capacity':'65','energy_full':'80000000','energy_full_design':'90000000'}.items():
        put('sys/class/power_supply/BAT0/' + key, value)
    put('sys/class/power_supply/AC/type','Mains')
    put('sys/class/power_supply/AC/online','0' if name=='sleeping-nvidia' else '1')
    put('sys/firmware/acpi/platform_profile','balanced')
    put('sys/firmware/acpi/platform_profile_choices','low-power balanced performance')
    cpu_count = 8 if name != 'intel-only' else 4
    put('sys/devices/system/cpu/online',f'0-{cpu_count-1}')
    put('sys/devices/system/cpu/present',f'0-{cpu_count-1}')
    for i in range(cpu_count):
        base=f'sys/devices/system/cpu/cpu{i}/'
        put(base+'topology/core_id',str(i//2 if amd else i))
        put(base+'topology/physical_package_id','0')
        put(base+'topology/thread_siblings_list',f'{i//2*2}-{i//2*2+1}' if amd else str(i))
        put(base+'cache/index3/level','3')
        put(base+'cache/index3/type','Unified')
        put(base+'cache/index3/shared_cpu_list',('0-3' if i<4 else '4-7') if amd else f'0-{cpu_count-1}')
        put(base+'cache/index3/size',('98304K' if i<4 else '32768K') if amd else '24576K')
        if name=='intel-nvidia': put(base+'topology/core_type','2' if i<4 else '1')
    put('sys/class/hwmon/hwmon0/name','k10temp' if amd else 'coretemp')
    put('sys/class/hwmon/hwmon0/temp1_label','Tctl' if amd else 'Package id 0')
    put('sys/class/hwmon/hwmon0/temp1_input','65000')
    if name in ('amd-nvidia','intel-nvidia'):
        pages.extend(['Firmware','Lighting'])
        for k,v in {'current_value':'65','default_value':'65','min_value':'35','max_value':'100','scalar_increment':'1','display_name':'Fixture CPU power limit'}.items():
            put('sys/class/firmware-attributes/fixture/attributes/cpu_fixture_power/'+k,v)
    if name in ('amd-nvidia','intel-nvidia','sleeping-nvidia'):
        pages.append('NVIDIA Curve')
        for key, value in {'vendor':'0x10de','device':'0x0001','class':'0x030000','power/runtime_status':'suspended' if name=='sleeping-nvidia' else 'active'}.items():
            put('sys/bus/pci/devices/0000:01:00.0/' + key, value)
    if name == 'amd-radeon':
        pages.append('AMD GPU')
        for key, value in {'vendor':'0x1002','device':'0x0001','product_name':'Fixture Radeon','mem_info_vram_total':'8589934592','pp_dpm_sclk':'0: 800Mhz\n1: 1500Mhz *'}.items():
            put('sys/class/drm/card0/device/' + key, value)
        (root/'sys/bus/pci/drivers/amdgpu').mkdir(parents=True)
        (root/'sys/class/drm/card0/device/driver').symlink_to('/sys/bus/pci/drivers/amdgpu')
    return pages


def command(bwrap, binary, root, out, pages, args):
    cmd = [str(bwrap), '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL', '--clearenv', '--tmpfs', '/', '--dev', '/dev', '--tmpfs', '/tmp', '--tmpfs', '/state', '--dir', '/app']
    for name in ('HOME','XDG_CONFIG_HOME','XDG_CACHE_HOME','XDG_DATA_HOME','XDG_STATE_HOME','XDG_RUNTIME_DIR'):
        cmd += ['--dir', '/state/'+name]
    # No host /etc, /home, /run, /sys, /proc, /dev or /usr bind. Only runtime
    # library/font directories are shared read-only. Custom SDK layouts may fail.
    for path in ('/lib','/lib64','/usr/lib','/usr/lib64','/usr/share/fonts','/usr/share/fontconfig','/etc/fonts','/etc/ld.so.cache'):
        if Path(path).exists():
            cmd += ['--ro-bind', path, path]
    cmd += ['--ro-bind', str(binary), '/app/centurion', '--ro-bind', str(root/'sys'), '/sys', '--ro-bind', str(root/'proc'), '/proc', '--ro-bind', str(root/'marker'), '/centurion-shell-fixture-v1', '--bind', str(out), '/output', '--remount-ro', '/', '--chdir', '/state']
    env = {'PATH':'/nonexistent', 'QT_QPA_PLATFORM':'offscreen', 'QT_SCALE_FACTOR':args.scale, 'QT_FONT_DPI':'96', 'LC_ALL':'C.UTF-8', 'CENTURION_UI':args.mode, 'CENTURION_LANG':args.lang, 'CENTURION_FIXTURE_ID':args.fixture, 'CENTURION_FIXTURE_PAGES':','.join(pages), 'CENTURION_FIXTURE_SIZE':args.size, 'CENTURION_FIXTURE_THEME':args.theme}
    if args.fixture == 'amd-nvidia': env['CENTURION_LIGHTING_FORCE']='1'
    if args.fixture == 'intel-nvidia': env['CENTURION_ZONE4_FORCE']='1'
    for key, val in env.items():
        cmd += ['--setenv',key,val]
    return cmd + ['/app/centurion','--simulate-shell']


def validate(out, args, pages):
    report = out/'manifest.json'
    if not report.is_file() or report.is_symlink() or report.stat().st_size > 1024*1024:
        raise ValueError('missing or invalid manifest')
    data = json.loads(report.read_text())
    if data.get('schema') != 1 or data.get('ok') is not True or data.get('fixture') != args.fixture or data.get('mode') != args.mode or sorted(pages) != sorted(data.get('pages',[])) or data.get('theme') != args.theme or data.get('scale') != args.scale or data.get('language') != args.lang or data.get('requested_size') != args.size or not data.get('health_targets'):
        raise ValueError('manifest does not prove expected pages')
    shots = data.get('screenshots', [])
    if not isinstance(shots,list) or not 1 <= len(shots) <= 256 or len(set(shots)) != len(shots):
        raise ValueError('invalid screenshot set')
    for name in shots:
        if not isinstance(name,str) or not re.fullmatch(r'(page|health)-[0-9]+(?:-sub-[0-9]+-[0-9]+)?\.png',name):
            raise ValueError('invalid screenshot path')
        p = out/name
        if p.is_symlink() or not p.is_file() or p.stat().st_size > 32*1024*1024:
            raise ValueError('invalid screenshot file')
        with p.open('rb') as f:
            if f.read(8) != b'\x89PNG\r\n\x1a\n':
                raise ValueError('not a PNG')
    if {p.name for p in out.iterdir()} != {'manifest.json',*shots}:
        raise ValueError('unexpected artifact set')
    return data


def compare_baseline(out, baseline, data):
    # Strict candidate identity, never creates/updates approved baselines.
    expected = {p.name for p in baseline.glob('*.png')}
    if expected != set(data['screenshots']):
        raise ValueError('baseline screenshot set differs')
    changed = [n for n in data['screenshots'] if hashlib.sha256((out/n).read_bytes()).digest() != hashlib.sha256((baseline/n).read_bytes()).digest()]
    if changed:
        raise ValueError('baseline changed: ' + ', '.join(changed))


def fixture_digest(root):
    result={}
    for p in sorted(root.rglob('*')):
        if p.is_symlink(): result[str(p.relative_to(root))]=('link',os.readlink(p))
        elif p.is_file(): result[str(p.relative_to(root))]=('file',hashlib.sha256(p.read_bytes()).hexdigest())
        elif p.is_dir(): result[str(p.relative_to(root))]=('dir',)
        else: raise ValueError('unexpected fixture entry')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path); p.add_argument('output',type=Path)
    p.add_argument('--fixture',choices=FIXTURES,default=FIXTURES[0]); p.add_argument('--mode',choices=('classic','experimental'),default='experimental')
    p.add_argument('--lang',choices=('en','tr'),default='en'); p.add_argument('--scale',choices=('1','1.25','1.5','1.75','2'),default='1')
    p.add_argument('--size',choices=('760x540','900x650','1280x800'),default='900x650'); p.add_argument('--theme',choices=('graphite','gruvbox','oled-black','pastel-mint-cream'),default='graphite')
    p.add_argument('--baseline',type=Path); p.add_argument('--require-baseline',action='store_true')
    args = p.parse_args()
    if args.require_baseline and not args.baseline:
        p.error('approved baseline required')
    bwrap = shutil.which('bwrap')
    if not bwrap or os.geteuid()==0 or os.uname().machine!='x86_64':
        p.error('unprivileged x86-64 Linux and bubblewrap required; no live fallback')
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary,os.X_OK) or binary.stat().st_mode & (stat.S_ISUID|stat.S_ISGID):
        p.error('ordinary executable required')
    if args.output.exists() or args.output.is_symlink():
        p.error('output must be a new directory')
    with tempfile.TemporaryDirectory(prefix='centurion-shell-') as tmp:
        root = Path(tmp)/'fixture'; out=Path(tmp)/'output'; root.mkdir(); out.mkdir()
        pages = fixture(root,args.fixture)
        before=fixture_digest(root)
        rc = subprocess.run(command(bwrap,binary,root,out,pages,args),timeout=180,check=False,close_fds=True,stdin=subprocess.DEVNULL)
        if rc.returncode != 0:
            raise SystemExit(f'isolated real shell failed ({rc.returncode}); no live fallback')
        data=validate(out,args,pages)
        if before != fixture_digest(root):
            raise ValueError('fixture changed during execution')
        # Preserve candidate output even when comparison fails for inspection.
        args.output.mkdir(parents=True,exist_ok=False)
        for name in ['manifest.json',*data['screenshots']]:
            shutil.copyfile(out/name,args.output/name)
        if args.baseline:
            compare_baseline(args.output,args.baseline,data)
        print(f"Captured {len(data['screenshots'])} candidate images; no approved baseline generated")


if __name__ == '__main__':
    main()
