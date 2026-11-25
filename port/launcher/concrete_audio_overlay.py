"""Experimental, reversible Concrete speaker layer made from local original media.

Preserves original Wwise sounds/actions and mirrors the Concrete completion layer
to MasterSecondaryBus. Never edits the user's dump. Refuses other asset hashes.
"""
from pathlib import Path
import hashlib,json,struct,sys
ROOT=Path(__file__).resolve().parent
EXPECTED_SHA256='687730fd454719e4671b6757914ac4439b744447f9838d4d6d8547d9c9856352'
sys.path.insert(0,str(ROOT/'research/tools'))
from xpps import kcap,knli

def fnv(s):
 v=2166136261
 for b in s.encode():v=((v*16777619)^b)&0xffffffff
 return v

def chunks(data):
 at=0
 while at<len(data):
  assert at+8<=len(data)
  n=struct.unpack_from('<I',data,at+4)[0];assert at+8+n<=len(data)
  yield data[at:at+4],at,data[at+8:at+8+n]
  at+=8+n

def pack_hirc(bank,new_hirc):
 parts=list(chunks(bank))
 result=b''.join(tag+struct.pack('<I',len(new_hirc if tag==b'HIRC' else body))+(new_hirc if tag==b'HIRC' else body) for tag,_,body in parts)
 # Keep the external KCAP footprint exactly unchanged. Duplicate WEM payloads
 # can share a DIDX location without changing ANY media ID or decoded bytes.
 # Retain 16-byte media alignment and use the saved padding for HIRC growth.
 chunkmap={tag:body for tag,_,body in parts}
 olddata=chunkmap[b'DATA'];newdata=bytearray();newidx=bytearray();seen={}
 for ident,off,length in struct.iter_unpack('<III',chunkmap[b'DIDX']):
  media=olddata[off:off+length];assert len(media)==length
  key=hashlib.sha256(media).digest()
  if key in seen:
   newoff=seen[key];assert newdata[newoff:newoff+length]==media
  else:
   newoff=(len(newdata)+15)&~15;newdata.extend(bytes(newoff-len(newdata)));newdata.extend(media);seen[key]=newoff
  newidx.extend(struct.pack('<III',ident,newoff,length))
 growth=len(result)-len(bank);target=len(olddata)-growth
 assert len(newdata)<=target and growth>0
 newdata.extend(bytes(target-len(newdata)))
 changed={b'HIRC':new_hirc,b'DIDX':bytes(newidx),b'DATA':bytes(newdata)}
 result=b''.join(tag+struct.pack('<I',len(changed.get(tag,body)))+changed.get(tag,body) for tag,_,body in parts)
 assert len(result)==len(bank)
 return result

def create_overlay(source,dest,patcher=None):
 source=Path(source);dest=Path(dest);original=source.read_bytes()
 inventory=json.loads((ROOT/'research/bank-inventory.json').read_text())
 verified=next(x for x in inventory if x['name']=='common_sound.sprig.xpps')
 # The inventory was recorded from this user's independently verified dump.
 expected=EXPECTED_SHA256
 assert verified['sha256']==expected
 if hashlib.sha256(original).hexdigest()!=expected:raise ValueError('Unverified common_sound asset: no overlay created')
 b=verified['banks'][2];bank=original[b['offset']:b['end']]
 graph=json.loads((ROOT/'research/graph.json').read_text())
 patched,ids=patcher(bank,graph)
 with kcap.Pack(str(source)) as p:
  links=knli.read_links(p)
  old_start=b['offset']-p.doff;ptrs=[at for at in links.positions(0) if p.u64(at)==old_start]
  assert ptrs==[0x3f0] and p.u64(0x3f8)==len(bank)
  assert len(patched)==len(bank)
  result=bytearray(original);result[b['offset']:b['end']]=patched
  assert result[:b['offset']]==original[:b['offset']] and result[b['end']:]==original[b['end']:]
  dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(result)
 with kcap.Pack(str(dest)) as p:
  newlinks=knli.read_links(p)
  assert newlinks.relocs==links.relocs and newlinks.cid==links.cid and newlinks.imports==links.imports
  assert p.read(p.u64(0x3f0),p.u64(0x3f8))==patched
 manifest={'experimental':True,'source_sha256':expected,'overlay_sha256':hashlib.sha256(result).hexdigest(),'bank_bytes_before':len(bank),'bank_bytes_after':len(patched),'added_nodes':ids,'original_main_audio_layers_preserved':True,'original_media_bytes_preserved':True,'kcap_layout_unchanged':True,'original_file_modified':False}
 dest.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2))
 return manifest

