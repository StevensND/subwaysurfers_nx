# extract_entrypoints.py -- recover UnityPlayer native method tables from a
# Unity libunity.so. Usage: python3 extract_entrypoints.py libunity.so
# (edit the JNI_OnLoad address + reg-fn list if Unity version changes;
#  find JNI_OnLoad via: readelf -sW libunity.so | grep JNI_OnLoad)
# Requires: pip install capstone pyelftools
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
import sys; f=open(sys.argv[1] if len(sys.argv)>1 else 'libunity.so','rb'); elf=ELFFile(f)
def v2o(va):
    for s in elf.iter_sections():
        a=s['sh_addr']; sz=s['sh_size']
        if s['sh_type']!='SHT_NOBITS' and a<=va<a+sz: return s['sh_offset']+(va-a)
def read(va,n): o=v2o(va); f.seek(o); return f.read(n)
def cstr(va):
    o=v2o(va)
    if o is None: return "?"
    f.seek(o); b=b''
    while True:
        c=f.read(1)
        if c in (b'\x00',b''): break
        b+=c
    return b.decode('latin1')
R=1027; rel={}
for r in elf.get_section_by_name('.rela.dyn').iter_relocations():
    if r['r_info_type']==R: rel[r['r_offset']]=r['r_addend']
md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN); md.detail=True
def find_table(fn):
    code=read(fn,0x200); adrp={}; addv={}; movw={}; tab=cnt=None
    for i in md.disasm(code,fn):
        op=i.op_str
        if i.mnemonic=='adrp':
            r=i.operands[0].reg; adrp[r]=i.operands[1].imm
        elif i.mnemonic=='add' and len(i.operands)==3 and i.operands[2].type==2:
            r=i.operands[0].reg
            if i.operands[1].reg in adrp: addv[r]=adrp[i.operands[1].reg]+i.operands[2].imm
        elif i.mnemonic in ('mov','movz') and len(i.operands)==2 and i.operands[1].type==2:
            movw[i.operands[0].reg]=i.operands[1].imm
        elif i.mnemonic=='ldr' and '#0x6b8]' in op:   # env->RegisterNatives
            pass
        elif i.mnemonic=='blr':
            # x2 = methods, w3 = count  (capstone reg ids: resolve by name)
            for rn,val in addv.items():
                if md.reg_name(rn)=='x2': tab=val
            for rn,val in movw.items():
                if md.reg_name(rn) in ('w3','x3'): cnt=val
            if tab and cnt: return tab,cnt
        if i.mnemonic=='ret': break
    return tab,cnt
fns=[0x5ddf9c,0x5de090,0x5de184,0x5b6b9c,0x5d64b0,0x5bc5ec,0x5c2924,0x5bb9c8,0x5c2b78,0x5b5e88]
for k,fn in enumerate(fns,1):
    tab,cnt=find_table(fn)
    if not tab: print("\n#%d fn=0x%x : (table not auto-found)"%(k,fn)); continue
    # class guess from first method name's flavour
    names=[cstr(rel.get(tab+i*24)) for i in range(cnt)]
    print("\n#%d fn=0x%x  methods@0x%x count=%d"%(k,fn,tab,cnt))
    for i in range(cnt):
        b=tab+i*24
        print("   %-30s %-42s 0x%x"%(cstr(rel.get(b)),cstr(rel.get(b+8)),rel.get(b+16) or 0))
