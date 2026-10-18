"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check.

--addcont DIR: a folder of the player's own add-ons, one subfolder each with sce_sys/param.sfo
(category ac). Add-ons of this title are reported installed by their entitlement label (the last
16 characters of the content ID); unlockers carry no data, so nothing is mounted.
"""
import argparse
import json
import re
import struct
from pathlib import Path
from prepare import sfo

LABEL = re.compile(r'[A-Za-z0-9]{16}')
MAX_ADDONS = 32  # src/runtime.h RUNTIME_ADDCONT_MAX


def addon_labels(addcont, title_id):
    """Entitlement labels of the title's add-ons under addcont, sorted; [] without a folder."""
    labels = set()
    for path in sorted(Path(addcont).glob('*/sce_sys/param.sfo')) if addcont else []:
        values = sfo(path.read_bytes())
        content_id = values.get('CONTENT_ID', '')
        # XXYYYY-CUSAnnnnn_00-LABEL: the title ID at 7, the label at 20.
        if values.get('CATEGORY') == 'ac' and content_id[7:16] == title_id and LABEL.fullmatch(content_id[20:]):
            labels.add(content_id[20:])
    if len(labels) > MAX_ADDONS:
        raise ValueError(f'more than {MAX_ADDONS} add-ons')
    return sorted(labels)


def prepare(game, out, sku='full', addcont=None):
    values=sfo((game/'sce_sys/param.sfo').read_bytes())
    params=[values.get(f'USER_DEFINED_PARAM_{i}',0) for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('invalid user-defined parameter')
    labels=addon_labels(addcont, values.get('TITLE_ID'))
    profile=dict(title_id=values.get('TITLE_ID'),sku=sku,sku_source='explicit probe setting',
                 user_params=params, mounted_addons=[], installed_addons=labels, boot_attr=0)
    out.mkdir(parents=True,exist_ok=True)
    head=struct.pack('<8s5I',b'G3CONT02' if labels else b'G3CONT01',{'full':3,'trial':1}[sku],*params)
    tail=struct.pack('<I',len(labels))+''.join(labels).encode('ascii') if labels else b''
    (out/'content.bin').write_bytes(head+tail)
    (out/'content-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    print(f'AppContent profile: SKU={sku} (probe setting), user params={params}, mounted add-ons=0, '
          f'installed add-ons without data={len(labels)}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--sku',choices=['full','trial'],default='full')
    p.add_argument('--addcont',type=Path,help="folder of the player's add-ons (one subfolder each)")
    a=p.parse_args();prepare(a.game,a.out,a.sku,a.addcont)
