import struct,re,sys,os,json
def fnv(vals):
    h=0xcbf29ce484222325
    for v in vals:
        for b in struct.pack('<I',v):
            h^=b; h=(h*0x100000001b3)&0xFFFFFFFFFFFFFFFF
    return h
def bits(v,s,n): return (v>>s)&((1<<n)-1)
def fsb_lengths(d,i):
    numSamples,shs,nts,ds,mode=struct.unpack_from('<IIIII',d,i+8)
    ver=struct.unpack_from('<I',d,i+4)[0]
    p=i+60+(4 if ver==0 else 0)
    L=[]
    for k in range(numSamples):
        raw=struct.unpack_from('<Q',d,p); raw=raw[0]; p+=8
        nxt=bits(raw,0,1); ch=bits(raw,5,1)+1; n=bits(raw,34,30)
        while nxt:
            r=struct.unpack_from('<I',d,p)[0]; p+=4
            nxt=bits(r,0,1); sz=bits(r,1,24); p+=sz
        L.append(n)
    return L
def analyse(path):
    d=open(path,'rb').read()
    out=[]
    def walk(o,end,path,):
        while o+8<=end:
            cid=d[o:o+4]; sz=struct.unpack('<I',d[o+4:o+8])[0]
            if cid in (b'RIFF',b'LIST'):
                typ=d[o+8:o+12]; out.append(('L:'+typ.decode('latin1'),o+12,sz-4)); walk(o+12,o+8+sz,path)
            else: out.append((cid.decode('latin1'),o+8,sz))
            o+=8+sz+(sz&1)
    walk(0,len(d),None)
    objs={}; pmlos=[]; wavs={}
    for c,o,sz in out:
        if c=='WAV ': wavs[d[o:o+16]]=d[o:o+sz]
        if c.startswith('L:') and len(c)==6:
            body={}
            first=None
            for c2,o2,s2 in out:
                if o<=o2<o+sz and not c2.startswith('L:'):
                    if first is None: first=(c2,o2,s2)
                    body.setdefault(c2,(o2,s2))
            if not first or len(first[0])!=4 or first[0][-1]!='B': continue
            gid=d[first[1]:first[1]+16]
            if c[2:]=='PMLO': pmlos.append(body)
            else: objs[gid]=(c[2:],body)
    def chunk(gid,n):
        o,s=objs[gid][1][n]; return d[o:o+s]
    def lst(b,off):
        if off+4>len(b): return []
        t,es=struct.unpack_from('<HH',b,off)
        if not (t&1) or es<16: return []
        n=t>>1
        return [b[off+4+i*es:off+4+i*es+16] for i in range(n)]
    def event_insts(ev):
        res=[]
        for gid,(t,ch) in objs.items():
            if t=='TMLN':
                b=chunk(gid,'TLNB')
                if b[16:32]==ev: res+=lst(b,32)
        for ch in pmlos:
            o,s=ch['PMLB']; b=d[o:o+s]
            if b[32:48]==ev: res+=lst(b,48)
        return res
    acc=[]; seen=set()
    def resolve(gid):
        if gid in seen or gid not in objs: return
        seen.add(gid); t=objs[gid][0]
        try:
            if t=='WAIT':
                w=wavs.get(chunk(gid,'WAIB')[16:32])
                if w: acc.append(struct.unpack_from('<H',w,22)[0])
            elif t=='MUIT':
                b=chunk(gid,'PLST'); n=(len(b)-12)//20
                for i in range(n): resolve(b[12+i*20:28+i*20])
            elif t=='SCIT':
                pass
            elif t=='EVIT': resolve(chunk(gid,'EVIB')[16:32])
            elif t=='EVNT':
                for i in event_insts(gid): resolve(i)
        except KeyError: pass
    params=[gid for gid,v in objs.items() if v[0]=='PARM' and b'\x00Backfire\x00' in chunk(gid,'PRMB')]
    for ch in pmlos:
        o,s=ch['PMLB']; b=d[o:o+s]
        if b[:16] in params:
            for i in lst(b,48): resolve(i)
    i=d.find(b'FSB5')
    if i<0: return None
    L=fsb_lengths(d,i)
    bf=[k for k in sorted(set(acc)) if k<len(L)]
    return {'n':len(L),'sig':fnv(L),'bf':bf,'len':[L[k] for k in bf]}
res={}
for f in sorted(os.listdir(sys.argv[1])):
    if not f.endswith('.bank') or not (f.startswith('PG_') or f.startswith('GS_')): continue
    try:
        r=analyse(os.path.join(sys.argv[1],f))
        if r: res[f]=r
    except Exception as e: res[f]={'err':str(e)}
json.dump(res,open(sys.argv[2],'w'))
print(len(res), sum(1 for v in res.values() if v.get('bf')), [k for k,v in res.items() if 'err' in v][:5])
