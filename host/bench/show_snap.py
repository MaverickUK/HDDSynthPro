import sys
names={23:'Status',14:'AltSt',16:'Data'}
L=open(sys.argv[1]).read().splitlines()
for l in L:
    if l.startswith('EV1'): pass
ev=[l for l in L if l.startswith('EV1')][-8:]
print('\n'.join(ev))
rows=[]
for l in L:
    if not l.startswith('O @'): continue
    p=l.split(); raw=int(p[4][4:],16)
    a=(raw>>16)&31; passes=(0x3FF-((raw>>21)&0x3FF))&0x3FF
    rows.append((p[1],names.get(a,hex(a)),raw&0xFFFF,passes*15))
i=0
while i<len(rows):
    r=rows[i]
    if r[1]=='Data':
        j=i
        while j<len(rows) and rows[j][1]=='Data': j+=1
        ns=[x[3] for x in rows[i:j]]
        print(r[0],'Data x%d strobes: first %d, then %d-%d ns'%(j-i,ns[0],min(ns[1:] or [0]),max(ns[1:] or [0])))
        i=j
    else:
        print(r[0],r[1],'%04x'%r[2],'strobe %d ns'%r[3]); i+=1
