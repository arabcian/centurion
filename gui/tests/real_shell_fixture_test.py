#!/usr/bin/env python3
"""Pure Python launcher contract; does not claim bubblewrap or Qt execution."""
from pathlib import Path
import sys, tempfile, json
from types import SimpleNamespace
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
import real_shell_fixture as f

def rejected(call):
    try: call()
    except (ValueError,TypeError): return
    raise AssertionError('invalid input accepted')

with tempfile.TemporaryDirectory() as tmp:
    base=Path(tmp); out=base/'out'; out.mkdir()
    args=SimpleNamespace(mode='experimental',lang='en',scale='1',size='900x650',theme='graphite',fixture='amd-nvidia')
    for name in f.FIXTURES:
        args.fixture=name; fake=base/name; fake.mkdir()
        pages=f.fixture(fake,name)
        assert set(f.BASE_PAGES).issubset(pages)
        cmd=f.command('/usr/bin/bwrap',Path('/binary'),fake,out,pages,args)
        assert cmd[-2:]==['/app/centurion','--simulate-shell']
        assert '--unshare-all' in cmd and '--clearenv' in cmd and '--share-net' not in cmd
        assert cmd[cmd.index('--bind')+1:cmd.index('--bind')+3]==[str(out),'/output']
        assert cmd.count('--bind')==1
        for directory in ('sys','proc'):
            i=cmd.index(str(fake/directory)); assert cmd[i-1]=='--ro-bind' and cmd[i+1]=='/'+directory
        assert all(cmd[i+1] not in ('/','/sys','/proc','/run','/home','/dev','/usr') for i,x in enumerate(cmd) if x=='--ro-bind')
        if name=='missing': assert not (fake/'proc/cpuinfo').exists()
        if name=='sleeping-nvidia': assert (fake/'sys/bus/pci/devices/0000:01:00.0/power/runtime_status').read_text().strip()=='suspended'
        before=f.fixture_digest(fake); (fake/'marker').write_text('modified'); assert before!=f.fixture_digest(fake)
    rejected(lambda:f.fixture(base/'bad','../../host'))
    args.fixture='amd-nvidia'; pages=list(f.BASE_PAGES)
    shot='page-00.png'; (out/shot).write_bytes(b'\x89PNG\r\n\x1a\nfixture')
    data=dict(schema=1,ok=True,fixture=args.fixture,mode=args.mode,pages=pages,theme=args.theme,scale=args.scale,language=args.lang,requested_size=args.size,health_targets=['page:Overview'],screenshots=[shot])
    (out/'manifest.json').write_text(json.dumps(data)); assert f.validate(out,args,pages)==data
    baseline=base/'baseline';baseline.mkdir(); (baseline/shot).write_bytes((out/shot).read_bytes());f.compare_baseline(out,baseline,data)
    (baseline/shot).write_bytes(b'changed');rejected(lambda:f.compare_baseline(out,baseline,data))
    for bad in ([shot,shot],['../escape.png'],['missing.png']):
        (out/'manifest.json').write_text(json.dumps(dict(data,screenshots=bad)));rejected(lambda:f.validate(out,args,pages))
    (out/'manifest.json').write_text(json.dumps(data));(out/'unexpected').write_text('x');rejected(lambda:f.validate(out,args,pages))
    main=(root/'gui/src/main.cpp').read_text(); branch=main.split('int main(',1)[1].split('// Dispatch before settings',1)[0]
    assert branch.index('simulationguard::enter()')<branch.index('QApplication app')<branch.index('runRealShell(app)')
    assert 'return 2;' in branch and 'CENTURION_PGO_TRAIN' not in branch
print('PASS: six generated fixtures, mount command, sentinel digest, manifest and baseline rejection (not Qt execution)')
