#!/usr/bin/env python3
"""Exercise an isolated copy of the real hook against temporary fake files only."""
from pathlib import Path
import json, subprocess, tempfile, re
root=Path(__file__).resolve().parents[2]
original=(root/'packaging/sleep/centurion-sleep-log').read_text()
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    # Every absolute host data path used by the hook is relocated before execution.
    source=re.sub(r"/proc/sys/kernel/random/boot_id|/run/centurion|/var/log/centurion|/var/lib/centurion|/sys/",lambda m:str(p)+m.group(0),original)
    hook=p/'hook';hook.write_text(source)
    def file(name,value):
        f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(str(value)+'\n')
    def invoke(phase): subprocess.run(['sh',str(hook),phase,'suspend'],check=True)
    boot='12345678-abcd-1234-abcd-123456789abc'
    file('proc/sys/kernel/random/boot_id',boot)
    file('sys/class/power_supply/BAT0/type','Battery')
    file('sys/class/power_supply/BAT0/energy_now',20000000)
    file('sys/class/power_supply/BAT0/energy_full',40000000)
    file('sys/class/power_supply/BAT0/status','Discharging')
    file('sys/power/mem_sleep','[s2idle] deep')
    invoke('pre')
    (p/'sys/class/power_supply/BAT0/energy_now').unlink()
    file('sys/class/power_supply/BAT0/charge_now',1000000)
    file('sys/class/power_supply/BAT0/charge_full',2000000)
    file('sys/class/power_supply/BAT0/status','Charging')
    invoke('post')
    log=p/'var/log/centurion/sleep.log';record=json.loads(log.read_text().strip())
    assert record['boot']==boot and record['unit']=='uWh' and record['unit_after']=='uAh' and record['status_after']=='Charging'
    invoke('pre');marker=p/'var/lib/centurion/sleep-inflight';before=marker.read_bytes();log_before=log.read_bytes()
    file('proc/sys/kernel/random/boot_id','12345678-abcd-1234-abcd-123456789abd')
    invoke('post');assert marker.read_bytes()==before and log.read_bytes()==log_before
print('PASS: isolated real sleep hook: boot provenance, changed unit and AC status, cross-boot failure marker preserved')
