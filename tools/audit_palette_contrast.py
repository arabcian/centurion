#!/usr/bin/env python3
"""Independent sRGB token audit, not a Qt screenshot or rendered-control test."""
import argparse
import csv
import re
from pathlib import Path

NAMES = 'BG0 BG1 BG2 BG3 BG4 WELL BORDER BORDER_SOFT FG FG_DIM MUTED ACCENT ACCENT_SOFT OK WARN DANGER DANGER_SOFT INFO PURPLE'.split()

def lum(color):
    c = [int(color[i:i+2], 16)/255 for i in (1, 3, 5)]
    c = [v/12.92 if v <= .04045 else ((v+.055)/1.055)**2.4 for v in c]
    return sum(x*y for x,y in zip(c, (.2126,.7152,.0722)))

def ratio(a,b):
    x,y = sorted((lum(a),lum(b)))
    return (y+.05)/(x+.05)

def checks(c):
    for bg in range(6):
        for role, fg in [('Text',8),('Secondary text',9),('Muted text',10)]+[('Status text', i) for i in (11,13,14,15,17,18)]:
            yield role,NAMES[fg],NAMES[bg],ratio(c[fg],c[bg]),4.5,False
        yield 'Focus / indicator','ACCENT',NAMES[bg],ratio(c[11],c[bg]),3.,False
    yield 'Input placeholder','MUTED','WELL',ratio(c[10],c[5]),4.5,False
    yield 'Disabled text (exempt)','MUTED','BG1',ratio(c[10],c[1]),4.5,True
    yield 'Selection text','ON_ACCENT','ACCENT',max(ratio('#000000',c[11]),ratio('#ffffff',c[11])),4.5,False
    for fg in (11,12,13,14,15,17,18):
        yield 'Chart mark',NAMES[fg],'BG2',ratio(c[fg],c[2]),3.,False

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('output',type=Path); a=p.parse_args()
    source=(Path(__file__).resolve().parents[1]/'gui/src/theme.cpp').read_text()
    block=source.split('const Palette PALETTES[] = {',1)[1].split('\n};',1)[0]
    palettes=re.findall(r'\{"([^"\n]+)",\s*"[^"\n]+",\s*\{([^}]+)\}\}',block)
    if len(palettes)!=57 or len({name for name,_ in palettes})!=len(palettes):
        raise ValueError('Expected 57 distinct built-in palettes; update audit for palette changes')
    total=failed=exempt=0; affected=set()
    with a.output.open('w',encoding='utf-8',newline='') as f:
        w=csv.writer(f, lineterminator='\n'); w.writerow(['palette','role','foreground','background','ratio','target','status'])
        for name, raw in palettes:
            c=re.findall(r'#[0-9a-fA-F]{6}',raw)
            if len(c)!=19: raise ValueError(name)
            rows=list(checks(c)); assert len(rows)==70
            for role,fg,bg,r,target,e in rows:
                status='EXEMPT' if e else 'PASS' if r>=target else 'BELOW_TARGET'
                total+=1; exempt+=e; failed+=status=='BELOW_TARGET'
                if status=='BELOW_TARGET': affected.add(name)
                w.writerow([name,role,fg,bg,f'{r:.6f}',target,status])
    print(f'{len(palettes)} palettes, {total} checks, {failed} below target in {len(affected)} palettes, {exempt} exempt; token-only audit')

if __name__=='__main__': main()
