#!/usr/bin/env python3
"""d5_merge_labels.py - merge docs/offline/labels/<report>.csv into MERGED.csv and SPOTCHECK.csv.

Written by sprint agent D5.  Reads the ROM dump (gang100.bin, base 0xE0000000, Thumb) with
capstone; it never touches Ghidra or the camera.  Run:  python3 docs/offline/tools/d5_merge_labels.py
The first half is a small ROM helper (branch-target index, literal-pool search, linear
disassembly); the second half loads the proposals, checks every one, applies the naming
convention from ghidra/naming-conventions.md and writes the two CSV files.
"""
import csv,glob,os,re,struct,sys,json,collections
import numpy as np
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
R='/mnt/data/WORK/eos/'
ROM=open(R+'m50ii_dumper/dump_1.0.1/gang100.bin','rb').read()
BASE=0xE0000000
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=False
def inrom(a): return BASE<=a<BASE+len(ROM)
def word(a): return struct.unpack_from('<I',ROM,a-BASE)[0]
# BL / B.W target map
h=np.frombuffer(ROM[:len(ROM)//2*2],dtype='<u2').astype(np.int64)
def scan():
    hi=h[:-1]; lo=h[1:]
    m=((hi&0xF800)==0xF000)&((lo&0x8000)==0x8000)&(((lo&0x5000)==0x5000)|((lo&0x5000)==0x1000))
    idx=np.nonzero(m)[0]
    hi=hi[idx];lo=lo[idx]
    S=(hi>>10)&1;imm10=hi&0x3ff;J1=(lo>>13)&1;J2=(lo>>11)&1;imm11=lo&0x7ff
    I1=1-(J1^S);I2=1-(J2^S)
    off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
    off=np.where(S==1,off-(1<<25),off)
    tgt=BASE+idx*2+4+off
    isbl=(lo&0x4000)!=0
    return idx*2+BASE,tgt,isbl
src,tgt,isbl=scan()
order=np.argsort(tgt); tgs=tgt[order]; srs=src[order]; bls=isbl[order]
def callers(a):
    i=np.searchsorted(tgs,a); j=np.searchsorted(tgs,a,side='right')
    return [(int(srs[k]),bool(bls[k])) for k in range(i,j)]
w=np.frombuffer(ROM[:len(ROM)//4*4],dtype='<u4')
def litrefs(v):
    return [int(i)*4+BASE for i in np.nonzero(w==v)[0]]
def cstr(a,maxlen=200):
    if not inrom(a): return None
    o=a-BASE; e=ROM.find(b'\0',o,o+maxlen)
    if e<0: return None
    s=ROM[o:e]
    if len(s)>=3 and all(32<=c<127 or c in(10,13,9) for c in s): return s.decode()
    return None
def dis(a,n=400):
    out=[];refs=set();strs=[]
    for ins in md.disasm(ROM[a-BASE:a-BASE+n*4],a):
        out.append(ins)
        t=None
        if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
            m=re.search(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]',ins.op_str)
            if m:
                la=((ins.address+4)&~3)+int(m.group(1),0)
                refs.add(la)
                if inrom(la):
                    v=word(la); refs.add(v); refs.add(v&~1)
                    s=cstr(v)
                    if s: strs.append((ins.address,v,s))
        elif ins.mnemonic in('adr','adr.w') or (ins.mnemonic in('add','addw','sub','subw') and 'pc' in ins.op_str):
            m=re.search(r'#(-?0x[0-9a-f]+|-?\d+)$',ins.op_str)
            if m:
                la=((ins.address+4)&~3)+int(m.group(1),0)*(-1 if ins.mnemonic.startswith('sub') else 1)
                refs.add(la); s=cstr(la)
                if s: strs.append((ins.address,la,s))
        m=re.search(r'#(0x[0-9a-f]+)$',ins.op_str)
        if m:
            v=int(m.group(1),16); refs.add(v)
            if ins.mnemonic[0]=='b' or ins.mnemonic.startswith('cb'): refs.add(v)
        if len(out)>=n: break
    return out,refs,strs
def body(a,maxn=600):
    """linear sweep until a return with no later forward branch target"""
    out,refs,strs=dis(a,maxn)
    far=a; res=[]
    for ins in out:
        res.append(ins)
        mn=ins.mnemonic; op=ins.op_str
        m=re.match(r'^(b|b\.w|b[a-z]{2}|b[a-z]{2}\.w|cbz|cbnz)$',mn)
        if m:
            mm=re.search(r'#(0x[0-9a-f]+)$',op)
            if mm:
                t=int(mm.group(1),16)
                if a<=t<a+maxn*4 and t>far: far=t
        isret=(mn.startswith('pop') and 'pc' in op) or (mn=='bx') or (mn in('b','b.w') ) or (mn.startswith('ldm') and 'pc' in op) or mn in('tbb','tbh')
        if mn in('tbb','tbh'): isret=False
        if isret and ins.address>=far: break
    return res

# ===================== merge =====================
L=R+'docs/offline/labels/'
# ---------- load proposals
rows=[]
for f in sorted(glob.glob(L+'*.csv')):
    b=os.path.basename(f)[:-4]
    if not re.match(r'^[AB]\d+[abc]?$',b): continue
    for x in list(csv.reader(open(f)))[1:]:
        if len(x)>=2: rows.append(dict(src=b,raw=x[0],name=x[1],ev=','.join(x[2:]).strip()))
def norm(raw,name):
    v=int(raw,16)
    if v>=0x10000000 and (v&1) and not name.startswith('str_'): v&=~1   # Thumb bit
    return v
for r in rows: r['addr']=norm(r['raw'],r['name'])
TOTAL=len(rows)
# ---------- stubs.S
S=open(R+'magiclantern_simplified/platform/M50mk2.101/stubs.S').read()
stub_at={};stub_name={}
for m in re.finditer(r'^\s*(?:THUMB_FN|ARM32_FN|DATA_PTR|NSTUB)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z0-9_]+)\s*\)',S,re.M):
    v=int(m.group(1),16); v=v&~1 if v>0x10000000 else v
    stub_at.setdefault(v,[]).append(m.group(2)); stub_name[m.group(2)]=v
# ---------- RAM .data image (A15: ROM 0xE1394358 -> RAM 0x4000..0x2AD1C)
def raminit(a):
    if 0x4000<=a<0x2AD1C: return word(0xE1394358+(a-0x4000))
    return None
# ---------- evidence checking
DATAPAT=re.compile(r'^(str_|rom_src_)|^(edmac_|boomer_|outer_).*(table|index)$|_matrix$|^ImgSeq_DriveModeIndexMap$|^HEAD_DefaultTimerRegTable$|^MvRawPro_(LvRawGeometryTable|ModeKeyToGeomIndex)$')
def winrefs(t,span=0x80):
    refs=set();strs=[]
    for st in (t-span,t-span+2):
        st&=~1
        if not inrom(st): continue
        o,r,s=dis(st,span); refs|=r; strs+=s
    return refs,strs
def prevterm(a):
    h1=struct.unpack_from('<H',ROM,a-BASE-2)[0]; h2=struct.unpack_from('<H',ROM,a-BASE-4)[0]
    if h1==0x4770 or (h1&0xFF00)==0xBD00 or (h1&0xF800)==0xE000: return True
    if (h2&0xF800)==0xF000 and (h1&0xD000)==0x9000: return True
    if h2==0xE8BD and (h1&0x8000): return True
    if h1 in (0,0xBF00): return True
    return False
def check(r):
    a=r['addr'];e=r['ev'];n=r['name']
    isdata=a<0x10000000 or bool(DATAPAT.search(n))
    toks=set(int(t,16) for t in re.findall(r'0x([0-9A-Fa-f]{4,8})\b',e))|set(int(t,16) for t in re.findall(r'\b(E[01][0-9A-Fa-f]{6})\b',e))
    toks-={a,a|1}
    qs=re.findall(r'"([^"]{4,})"',e)+re.findall(r"'([^']{4,})'",e)
    c=dict(kind='data' if isdata else 'code',conf=[],unconf=[],allstr='')
    if isdata:
        if a<0x10000000:
            lr=litrefs(a); near=[t for t in toks if 0<a-t<=0x200 and litrefs(t)]
            c['fs']=None
            if lr: c['conf'].append('RAM address is a literal-pool word at %s'%', '.join('0x%08X'%x for x in lr[:3])); c['verdict']='PASS'
            elif near: c['conf'].append('base 0x%X is a literal-pool word (0x%08X); label is base+0x%X'%(near[0],litrefs(near[0])[0],a-near[0])); c['verdict']='PASS'
            else: c['unconf'].append('no literal-pool word for this RAM address'); c['verdict']='UNCONFIRMED'
        else:
            s=cstr(a)
            if n.startswith('str_'):
                ok = s is not None and (any(q[:16] in s for q in qs) or not qs)
                (c['conf'] if ok else c['unconf']).append('ASCII at address: %r'%(s[:60] if s else None)); c['verdict']='PASS' if ok else 'FAIL'
            else:
                lr=litrefs(a)
                ws=[word(a+4*i) for i in range(4)]
                if lr: c['conf'].append('address is a literal-pool word at %s; first words %s'%(', '.join('0x%08X'%x for x in lr[:3]),' '.join('%08X'%w for w in ws))); c['verdict']='PASS'
                else: c['unconf'].append('no literal-pool reference found; first words %s'%' '.join('%08X'%w for w in ws)); c['verdict']='UNCONFIRMED'
        return c
    bd=body(a); _,brefs,bstrs=dis(a,len(bd))
    _,drefs,dstrs=dis(a,1200)   # deeper linear sweep, for switch-heavy functions
    cs=callers(a); lr=litrefs(a|1)
    first=bd[0].mnemonic+' '+bd[0].op_str
    c.update(first=first,ncall=len(cs),nlit=len(lr),prevterm=prevterm(a))
    why=[]
    if first.startswith('push'): why.append('starts with '+first.split('{')[0].strip())
    if cs: why.append('%d direct branch/call site(s), e.g. 0x%08X'%(len(cs),cs[0][0]))
    if lr: why.append('Thumb pointer 0x%08X stored at 0x%08X'%(a|1,lr[0]))
    if not why and c['prevterm']: why.append('preceded by a function terminator; first insn '+first)
    c['fs']=bool(why); c['fswhy']='; '.join(why)
    ctx_refs=set();ctx_strs=[]
    for cc,_ in cs[:40]:
        r_,s_=winrefs(cc,0x60); ctx_refs|=r_; ctx_strs+=s_
    for l in lr[:10]:
        for k in range(-16,20,4):
            if inrom(l+k):
                v=word(l+k); sx=cstr(v) if inrom(v) else None
                if sx: ctx_strs.append((l+k,v,sx))
        r_,s_=winrefs(l-0x100,0x100); ctx_strs+=s_
    lits=[x for x in brefs if x<0x10000000]
    for t in sorted(toks):
        if t in brefs or (t|1) in brefs or (t&~1) in brefs or any(i.address==t for i in bd) or any(sa==t or ia==t for ia,sa,_ in bstrs):
            c['conf'].append('0x%X referenced in body'%t); continue
        if any(cc[0]==t or 0<=cc[0]-t<0x600 or 0<=t-cc[0]<=4 for cc in cs): c['conf'].append('call site at/after 0x%X'%t); continue
        if t in lr or any(abs(l-t)<0x10 for l in lr): c['conf'].append('pointer to label stored at/near 0x%X'%t); continue
        if inrom(t):
            r_,s_=winrefs(t); ctx_strs+=s_
            if a in r_ or (a|1) in r_: c['conf'].append('code at 0x%X references the label'%t); continue
            if any(word(t+k)==(a|1) for k in range(-16,20,4) if inrom(t+k)): c['conf'].append('table near 0x%X holds the label pointer'%t); continue
            if t in drefs or any(sa==t for _,sa,_ in dstrs): c['conf'].append('0x%X referenced later in the function (linear sweep)'%t); continue
            if cstr(t) and any(sa==t for _,sa,_ in ctx_strs): c['conf'].append('string 0x%X referenced at a caller/registration site'%t); continue
        else:
            base=[x for x in lits if 0<t-x<=0x200]
            if base: c['conf'].append('0x%X = literal base 0x%X + 0x%X'%(t,max(base),t-max(base))); continue
            ri=raminit(t)
        c['unconf'].append('0x%X not machine-checked'%t)
    # strings reachable through RAM .data pointers loaded in the body
    for x in lits:
        v=raminit(x)
        if v and inrom(v) and cstr(v): bstrs.append((0,v,cstr(v)))
        for off in range(0,0x40,4):
            v=raminit(x+off)
            if v and inrom(v) and cstr(v): dstrs.append((0,v,cstr(v)))
    for t in toks:
        v=raminit(t)
        if v and inrom(v) and cstr(v): ctx_strs.append((0,v,cstr(v)))
    sb=' | '.join(x[2] for x in bstrs); sd=' | '.join(x[2] for x in dstrs); sc=' | '.join(x[2] for x in ctx_strs)
    for q in qs:
        q2=q.replace('...','').strip()[:20]
        if q2 in sb: c['conf'].append('string %r referenced in body'%q[:40])
        elif q2 in sd: c['conf'].append('string %r referenced later in the function / via RAM pointer'%q[:40])
        elif q2 in sc: c['conf'].append('string %r referenced at a caller/registration site'%q[:40])
        else: c['unconf'].append('string %r not found at the label or its reference sites'%q[:40])
    c['allstr']=(sb+' | '+sd+' | '+sc)
    if not c['fs']: c['verdict']='FAIL'
    elif c['unconf'] and not c['conf']: c['verdict']='UNCONFIRMED' if (toks or qs) else 'PASS'
    elif c['unconf']: c['verdict']='PARTIAL'
    else: c['verdict']='PASS'
    return c
for r in rows: r['chk']=check(r)

# ---------- hand inspection by D5 of items the automatic pass could not decide
HAND={
('A4',0xE00732E8):'hand: ldr r4,=0x7FFCFFEE; cmn r3,r4 at 0xE00732EC..F0 is the compare with 0x80030012; pointer 0xE00732E9 loaded at 0xE007334A next to the call at 0xE0073352',
('A15',0xE027C1C8):'hand: adr at 0xE0043DB4 loads "PROPAD_CreateFROMPropertyHandle : RASEN (%#x)" (0xE0044194)',
('A15',0xE0760DF4):'hand: .data image gives RAM 0x292C4 and 0x292CC the initial value 0xE0760DF5',
('A15',0xE07B9C88):'hand: .data image gives RAM 0x23808 and 0x23810 the initial value 0xE07B9C89',
('A15',0xE027C114):'hand: body is ldr r0,=0xE0040000; bx lr (sector-size constant belongs to a sibling getter, not checked)',
('A15',0xE027C2DC):'hand: body is mov.w r0,#0xF0000000; bx lr; preceded by bx lr at 0xE027C2DA; no static caller found',
('A10',0xE06CAEB2):'hand: loop test at 0xE06CAED4..D8 is ldr r0,[r4]; adds r1,r0,#1; bne = terminate on 0xFFFFFFFF; assert line 0xE7 at 0xE06CAEC6',
('A8',0xE04B3468):'hand: bhs at 0xE04B348E reaches 0xE04B396E, where adr at 0xE04B3974 loads "srmGetShootMemAreaSize IrregalArea %d" (0xE04B3C74)',
('A8',0xE00412B6):'hand: thunk 0xE0608960 loads table base 0x5128 and reads [base+8] at 0xE060896C ("slot 8" is the byte offset)',
('A7',0xE00DE690):'hand: bl 0xE00DE690 present in sdIOControl 0xE00E5C84 (case value not re-derived)',
('A7',0xE00DE6D4):'hand: bl 0xE00DE6D4 present in sdIOControl 0xE00E5C84 (case value not re-derived)',
('A7',0xE00DF332):'hand: bl 0xE00DF332 present in sdIOControl 0xE00E5C84 (case value and 0xF800 limit not re-derived)',
('A7',0xE00DF166):'hand: bl 0xE00DF166 present in sdIOControl 0xE00E5C84 (case value not re-derived)',
('A14c',0xE05BE8A6):'hand: 45 insns, same callees as 0xE05BE6A6 (0xE058738C, 0xE05BE638, 0xE05BE5CE) but ends movs r0,#0xD; pop; 0 callers',
}
HAND.update({
('A14b',0xE05B802E):'hand: 0xE00BEE3A ldr r1,=0x017F0118; 0xE00BEE3E bl 0xE05B802E',
('A9',0xE07E578A):'hand: code loads literal 0xE1358FF0 at 0xE07E5796/9E/AC; 0xE1359058 = that base + 0x68 (offset arithmetic not re-derived)',
('A9',0xE1358FF8):'hand: 0xE07E578A passes literal base 0xE1358FF0 to the search routine 0xE07E5754; the label is base+8 (offset not re-derived)',
})
PART={('A9',0xE07E578A),('A9',0xE1358FF8),('A7',0xE00DE690),('A7',0xE00DE6D4),('A7',0xE00DF332),('A7',0xE00DF166)}
for r in rows:
    k=(r['src'],r['addr'])
    if k in HAND:
        r['chk']['conf'].append(HAND[k]); r['chk']['unconf']=[u for u in r['chk']['unconf'] if k in PART] or (['offset arithmetic not re-derived'] if k in PART else [])
        r['chk']['verdict']='PARTIAL' if k in PART else 'PASS'
# ---------- dropped (claim refuted in the report's Independent verification)
DROP={('A6',0xE024A8FC):'A6 verification claim 5 PARTLY REFUTED (function identity): 0xE024A8FC does not log "mvrRecStart In"; that string is referenced only at 0xE024A09C inside 0xE024A068. Replaced by two corrected labels (source A6-verify).'}
dropped=[r for r in rows if (r['src'],r['addr']) in DROP]
for r in dropped: r['chk']['verdict']='FAIL'; r['chk']['unconf'].append('DROPPED: '+DROP[(r['src'],r['addr'])])
rows=[r for r in rows if (r['src'],r['addr']) not in DROP]
# corrected labels taken from verification sections, re-checked in ROM by D5
EXTRA=[
 dict(src='A6-verify',addr=0xE024A068,name='mvrRecStart',ev='body of the MovRecState start action: push.w, asserts MovieDS::MovRecState.c, adr "mvrRecStart In" at 0xE024A09C; only reference is b.w at 0xE024A946 (tail of 0xE024A8FC)'),
 dict(src='A6-verify',addr=0xE024A8FC,name='mvrRecStart_state_entry',ev='handler word 0xE024A8FD in MovRecState descriptor at 0xE0BB79C4; logs "mvrRecStart:SetCurState:EXT_REC", takes "MovieRecorder", tail-branches b.w 0xE024A068 (mvrRecStart body)'),
 dict(src='A2-verify',addr=0xE06EF37A,name='evfVdInterrupt',ev='EvfState input 3 / state 5 action (matrix word at 0xE0B505F8); logs "evfVdInterrupt[%d][%d]" at 0xE06EF410; LiveView::EvfCapState.c'),
]
for r in EXTRA: r['raw']='0x%08X'%r['addr']; r['chk']=check(r); rows.append(r)
# ---------- naming convention + conflict resolution
RENAME={ # address -> (final name, reason)  [reason only used when reports disagree]
0xE05B802E:('edmac_select_boomer_input','ROM: Engine::Edmac.c asserts, table 0xE0EC3BD4 indexed by channel, tail-calls 0xE0683CA0; all three reports describe the same function; edmac_* family, snake_case'),
0xE05B80C2:('edmac_clear_boomer_input',''),
0xE05B7FB6:('edmac_boomer_vdkick_off','ROM: tail-calls 0xE05B56CC which stores 0; Off/Clear are synonyms; A5 wording kept'),
0xE05B7FDE:('edmac_boomer_vdkick_on','ROM: tail-calls 0xE05B56F6 which stores 1; On/Set are synonyms'),
0xE05B8006:('edmac_boomer_vdkick_pulse','ROM: tail-calls 0xE05B5720 (0 then 1); same meaning in both reports'),
0xE05B7520:('outer_boomer_vdkick_pulse','ROM: 4 insns, indexes 0xE1087A8C then b.w 0xE05B5720; spelling difference only'),
0xE05B752E:('outer_select_boomer_input','ROM: Engine::Outer.c asserts, table 0xE1087A8C; spelling difference only'),
0xE0683CA0:('boomer_selector_set',''),0xE0683CCA:('boomer_selector_get',''),
0xE05B56CC:('boomer_vdkick_off','ROM: Engine::BoomerVdKick.c assert, stores 0 to the node register; Off/Clear are synonyms'),
0xE05B56F6:('boomer_vdkick_on','ROM: stores 1 to the node register; On/Set are synonyms'),
0xE05B5720:('boomer_vdkick_pulse',''),
0xE05B7A6C:('edmac_enable_channel','same name, case differs; edmac_* family'),
0xE05B7B08:('edmac_disable_channel','same name, case differs; edmac_* family'),
0xE05B7A78:('edmac_release_hold',''),0xE05B7B14:('edmac_set_hold',''),0xE05B7B42:('edmac_prepare_channel',''),
0xE05B9690:('edmac_read_set_mode_0x11',''),
0xE05B7A32:('edmac_read_set_mode_0x12','ROM 0xE05B7A32..7A6A: assert line 0x3AC, writes 0x12 to reg+8 and 1 to reg+0xB4; name states the register value and does not assume it is a "kick"'),
0xE05B8308:('edmac_abort_write',''),0xE05B83B2:('edmac_abort_read',''),0xE05B96B8:('edmac_reset_write_channel',''),
0xE0EC37AC:('table_edmac_chan',''),0xE0EC3BD4:('table_edmac_ch_boomer','same table (stride 12, indexed by EDMAC channel); table_* family'),
0xE0EC3F64:('table_edmac_irq',''),0xE0EC367C:('table_edmac_chan_group_index',''),0xE0EC3A0C:('table_edmac_group',''),
0xE1086394:('table_boomer_selector_reg',''),0xE1086A9C:('table_boomer_vdkick_reg',''),
0xE1087A8C:('table_outer_port_boomer','same table (stride 0x14, indexed by Outer port); table_* family'),
0xE0BBE0F0:('table_EvfDevState_matrix',''),0xE0B5050C:('table_EvfState_matrix',''),
0xE0BBFA68:('table_ImgSeq_DriveModeIndexMap',''),0xE131341C:('table_HEAD_DefaultTimerReg',''),
0xE1359338:('table_MvRawPro_LvRawGeometry',''),0xE1358FF8:('table_MvRawPro_ModeKeyToGeomIndex',''),
0xE05B9DB0:('TaskClass_FreeMsgNode','ROM: pushes a node onto the free list at 0x39B84 used only by the TaskClass post functions (System::Task.c); TaskClass_* keeps it next to TaskClass_PostEvent'),
0xE05B9EEA:('TaskClass_AllocMsgNode','ROM: pops the free list at 0x39B84; called by TryPostEvent at 0xE05B9FBE'),
0xE05B9E20:('task_TaskClass_entry','ROM 0xE05B9E20..9E56: receive loop then blx [obj+0x14]; it is the task entry passed by CreateTaskClass, so the task_* family applies'),
0xE05B9F9E:('TryPostEvent','ROM: bl 0xE05B9EEA, adr "[TASK ERROR] TryPostEvent MsgQue OverFlow" at 0xE05B9FCA, assert System::Task.c:0xE5. Canon\'s own name; A4\'s "stateobj" is wrong, the object is a TaskClass (message queue), not a state object'),
0xE05BB2B2:('StateObject_Dispatch','same function; pairs with CreateStateObject (0xE05BB31A); A2 verification claim 1 confirmed the cell indexing'),
0xE05BE638:('ersc_check_nodes_free','ROM 0xE05BE638..: walks the entry\'s ids, returns 0x1F when node+0x10==1; A12 is the dedicated report and names its sibling 0xE05BE5CE in the same style'),
0xE05BE6A6:('LockEngineResources_blocking','ROM: 105 BL callers, creates semaphore "EFM_ERSC_STATIC" and waits. This is the real LockEngineResources; the suffix avoids a duplicate with the wrong ML stub at 0xE05BE8A6'),
0xE05BE8A6:('TryLockEngineResources','ROM: 0 callers, same prologue as 0xE05BE6A6 but returns 0xD without waiting; 2 of 3 reports use this name'),
0xE065B5C0:('evfDevEventDispatch','ROM: string "[DEV]evfDevEventDispatch Current=%d,..." in body; Canon\'s own name wins over the generic handler name'),
0xE065B5FC:('EvfDev_Create','ROM: creates the state object as well as the TaskClass, so the narrower "_CreateTaskClass" is misleading'),
0xE06CAFF4:('EvfCap_Create','ROM: creates EvfState as well as the TaskClass'),
0xE05BFD48:('_prop_cleanup',''),
# convention: task_* for task entry points, msg_handler_* for message dispatchers, prop_handler_*
0xE05BBC1E:('task_TaskClassB_entry',''),0xE004A7D0:('task_TaskClassC_entry',''),0xE06805D8:('task_CtrlSrv',''),
0xE03758D0:('task_DAFMonitorTask',''),0xE03C9A80:('task_DAFDelivererTask',''),0xE061DCF2:('task_AFTask',''),
0xE06EA99E:('task_DOSDriver',''),0xE00AF3BE:('task_CSMgrTask',''),
0xE072D4F8:('msg_handler_MovieRecorder',''),0xE049D3C4:('msg_handler_MovWriter',''),0xE06CAFB8:('msg_handler_EvfCap',''),
0xE06CB282:('msg_handler_LVEZM',''),0xE060ADA4:('msg_handler_EIS',''),0xE05BF654:('msg_handler_PropMgr',''),
0xE0796140:('msg_handler_FileMgr',''),0xE0396F54:('msg_handler_DAF',''),0xE06AA72A:('msg_handler_Imager',''),
0xE062C3CC:('msg_handler_ISManager',''),
0xE01B8810:('prop_handler_gui_80040004',''),
}
groups=collections.OrderedDict()
for r in sorted(rows,key=lambda r:(r['addr'],r['src'])): groups.setdefault(r['addr'],[]).append(r)
merged=[];nconf_name=0;nconf_stub=0;renamed=0
def srckey(s):
    m=re.match(r'([AB])(\d+)(.*)',s); return (m.group(1),int(m.group(2)),m.group(3))
for a,g in groups.items():
    names=[]
    for r in g:
        if r['name'] not in names: names.append(r['name'])
    final=names[0]; reason=''
    if a in RENAME: final,reason=RENAME[a]
    isram=a<0x10000000
    conflicts=[]
    if len(names)>1:
        nconf_name+=1
        conflicts.append('NAME: '+' | '.join('%s=%s'%(r['src'],r['name']) for r in g)+' -> '+final+' ('+reason+')')
    # confidence tier (naming-conventions.md): matched / maybe ; never OK without runtime evidence
    chks=[r['chk'] for r in g]
    core=re.sub(r'^(_|task_|msg_handler_|str_|table_)','',final).lower()
    allstr=' '.join(c.get('allstr','') for c in chks).lower()
    propnames=[n.lower().lstrip('_') for n in names]
    tier='maybe'
    if any(c['kind']=='code' for c in chks):
        if any(p in allstr for p in propnames) or core in allstr: tier='matched'
        if a in stub_at and (final in stub_at[a] or any(n in stub_at[a] for n in names)): tier='matched'
        if re.search(r'as (in|on) M50|exactly (like|as)|exact delta|same delta',' '.join(r['ev'] for r in g)) and not a in (0xE05B7B08,0xE05B82AE): tier='matched'
    else:
        if final.startswith('str_') and all(c['verdict']=='PASS' for c in chks): tier='matched'
    if any(c['verdict']=='FAIL' for c in chks): tier='unk'
    if isram:
        base=final
        final='g_'+base+('' if tier=='matched' else '_maybe')
    # existing ML stubs
    if a in stub_at and final not in stub_at[a]:
        nconf_stub+=1
        conflicts.append('STUB: platform/M50mk2.101/stubs.S names this address %s; reports say the stub name is wrong or less precise, kept as alias in the comment'%'/'.join(stub_at[a]))
    for n in set(names+[final]):
        k=n[2:] if n.startswith('g_') else n
        k=re.sub(r'_maybe$','',k)
        if k in stub_name and stub_name[k]!=a:
            nconf_stub+=1
            conflicts.append('STUB: stubs.S places %s at 0x%X; the report claims that address is wrong'%(k,stub_name[k]))
    conflicts=list(collections.OrderedDict.fromkeys(conflicts))
    if final!=names[0] or len(names)>1: renamed+=1
    srcs=sorted(set(r['src'] for r in g),key=lambda s:(srckey(s.split('-')[0]),s))
    ev=' || '.join('%s: %s'%(r['src'],r['ev']) for r in g)
    merged.append(dict(address='0x%08x'%a,name=final,source_reports=';'.join(srcs),evidence=ev,confidence=tier,conflict=' ;; '.join(conflicts),_g=g))
# same final name at two addresses?
cnt=collections.Counter(m['name'] for m in merged)
dups=[n for n,c in cnt.items() if c>1]
assert not dups,dups
assert all(re.match(r'^[A-Za-z_][A-Za-z0-9_]*$',m['name']) for m in merged)
with open(L+'MERGED.csv','w',newline='') as f:
    w_=csv.writer(f,lineterminator='\n'); w_.writerow(['address','name','source_reports','evidence','confidence','conflict'])
    for m in merged: w_.writerow([m[k] for k in ('address','name','source_reports','evidence','confidence','conflict')])
# ---------- spot-check record (every proposal, plus the hand-inspected sample flag)
final_by_addr={int(m['address'],16):m['name'] for m in merged}
allrows=sorted(rows+dropped,key=lambda r:(srckey(r['src'].split('-')[0]),r['src'],r['addr']))
bysrc=collections.OrderedDict()
for r in allrows: bysrc.setdefault(r['src'],[]).append(r)
sample=set()
for s,l in bysrc.items():
    if s.endswith('-verify'): idx=range(len(l))
    else: idx=sorted(set([0,len(l)//3,(2*len(l))//3,len(l)-1]))
    for i in idx: sample.add((s,l[i]['addr']))
with open(L+'SPOTCHECK.csv','w',newline='') as f:
    w_=csv.writer(f,lineterminator='\n'); w_.writerow(['source','address','proposed_name','merged_name','kind','in_sample','verdict','function_start_or_data_check','evidence_confirmed','evidence_not_checked'])
    for r in allrows:
        c=r['chk']
        w_.writerow([r['src'],'0x%08x'%r['addr'],r['name'],final_by_addr.get(r['addr'],'(dropped)') if (r['src'],r['addr']) not in DROP else '(dropped)',c['kind'],'yes' if (r['src'],r['addr']) in sample else '',c['verdict'],c.get('fswhy','') if c['kind']=='code' else '', '; '.join(c['conf']),'; '.join(c['unconf'])])
V=collections.Counter(r['chk']['verdict'] for r in allrows)
SV=collections.Counter(r['chk']['verdict'] for r in allrows if (r['src'],r['addr']) in sample)
print('TOTAL proposals',TOTAL,'unique addresses',len(set(r['addr'] for r in allrows if not r['src'].endswith('-verify'))))
print('merged rows',len(merged),'| name conflicts',nconf_name,'| rows with stub conflict',sum(1 for m in merged if 'STUB:' in m['conflict']),'| rows with any conflict',sum(1 for m in merged if m['conflict']),'| dropped',len(dropped),'| added',len(EXTRA))
print('dup-merged (same addr, same name)',sum(1 for a,g in groups.items() if len(g)>1 and len(set(r['name'] for r in g))==1))
print('renamed for convention/conflict',renamed)
print('tiers',collections.Counter(m['confidence'] for m in merged))
print('all verdicts',V,'sample',len(sample),SV)
print('per source',{s:len(l) for s,l in bysrc.items()})
json.dump(dict(sample=sorted([s,a] for s,a in sample)),open('sample.json','w'))
print('--- sample not PASS')
for r in allrows:
    if (r['src'],r['addr']) in sample and r['chk']['verdict']!='PASS': print(r['src'],hex(r['addr']),r['name'],r['chk']['verdict'],'|',r['chk'].get('fswhy',''),'|',r['chk']['conf'],'| UN',r['chk']['unconf'])
print('--- all FAIL/UNCONFIRMED')
for r in allrows:
    if r['chk']['verdict'] in('FAIL','UNCONFIRMED'): print(r['src'],hex(r['addr']),r['name'],r['chk']['verdict'],r['chk']['unconf'])
