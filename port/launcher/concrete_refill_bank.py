"""Private added Concrete refill feedback, NOT an original/shadPS4 cue fix.
All original sounds/events remain byte-identical; a new standalone event plays
copies of three existing Concrete samples only when our verified hook requests it.
"""
from concrete_audio_overlay import ROOT,fnv,chunks,pack_hirc,create_overlay
import struct,json,sys
EVENT_NAME='iss_concrete_refill_controller'
EVENT_ID=fnv(EVENT_NAME)

def patch_standalone_bank(bank,graph):
 objects={o['id']:o for o in graph};parts=list(chunks(bank));hirc=next(x for x in parts if x[0]==b'HIRC')
 count=struct.unpack_from('<I',hirc[2])[0];at=hirc[1]+12;raw={};offsets={}
 for _ in range(count):
  n=struct.unpack_from('<I',bank,at+1)[0];id=struct.unpack_from('<I',bank,at+5)[0]
  raw[id]=bank[at:at+5+n];offsets[id]=at;at+=5+n
 originals=[157585252,663383958,1037877254,883727739]
 ids={id:fnv(f'iss_concrete_refill_{id}')&0x3fffffff for id in originals}
 action=fnv('iss_concrete_refill_play')&0x3fffffff
 assert len(set([*ids.values(),action,EVENT_ID]))==6
 assert not set([*ids.values(),action,EVENT_ID]).intersection(raw)
 copies=[]
 for old in originals:
  b=bytearray(raw[old]);start=offsets[old];previous=None
  for f in objects[old]['fields']:
   if f['offset'] is None:continue
   name,value=f['name'],f['value'];at=f['offset']-start
   if name in ['ulID','DirectParentID','ulChildID','ulPlayID'] and value in ids:struct.pack_into('<I',b,at,ids[value])
   if old==883727739:
    if name=='DirectParentID':struct.pack_into('<I',b,at,0)
    if name=='OverrideBusId':struct.pack_into('<I',b,at,805203703)
    # Dedicated speaker voice gain; no other sounds/PC channels are altered.
    if name=='pValue' and previous==('pID',0):struct.pack_into('<f',b,at,-4.0)
   previous=(name,value)
  copies.append(bytes(b))
 template=244698530;b=bytearray(raw[template]);start=offsets[template]
 for f in objects[template]['fields']:
  if f['name']=='ulID':struct.pack_into('<I',b,f['offset']-start,action)
  if f['name']=='idExt':struct.pack_into('<I',b,f['offset']-start,ids[883727739])
 copies.append(bytes(b));copies.append(struct.pack('<BIIII',4,12,EVENT_ID,1,action))
 new_hirc=struct.pack('<I',count+6)+b''.join(raw.values())+b''.join(copies)
 ids.update(event=EVENT_ID,action=action)
 return pack_hirc(bank,new_hirc),ids

if __name__=='__main__':
 print(json.dumps(create_overlay(sys.argv[1],sys.argv[2],patch_standalone_bank),indent=2))
