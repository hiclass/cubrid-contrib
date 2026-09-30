import re, sys, html

C16={30:'#000',31:'#e05561',32:'#8cc265',33:'#d18f52',34:'#4aa5f0',35:'#c162de',36:'#42b3c2',37:'#d7dae0',
     90:'#5c6370',91:'#ff616e',92:'#a5e075',93:'#f0a45d',94:'#4dc4ff',95:'#de73ff',96:'#4cd1e0',97:'#fff'}
def c256(n):
    if n<16:
        base=[0,1,2,3,4,5,6,7]
        return C16.get(30+(n%8) + (60 if n>=8 else 0),'#ccc')
    if n<232:
        n-=16; r,g,b=n//36,(n//6)%6,n%6
        v=lambda x:0 if x==0 else 55+x*40
        return '#%02x%02x%02x'%(v(r),v(g),v(b))
    v=8+(n-232)*10
    return '#%02x%02x%02x'%(v,v,v)

class Cell:
    __slots__=('ch','fg','bg','bold','dim','rev')
    def __init__(s): s.ch=' ';s.fg=None;s.bg=None;s.bold=False;s.dim=False;s.rev=False

def simulate(data, rows, cols):
    grid=[[Cell() for _ in range(cols)] for _ in range(rows)]
    r=c=0; fg=bg=None; bold=dim=rev=False
    i=0; L=len(data)
    while i<L:
        b=data[i]
        if b==0x1b:
            m=re.match(rb'\x1b\[([0-9;?<]*)([a-zA-Z])',data[i:])
            if m:
                ps,f=m.group(1).decode(),m.group(2).decode()
                if f=='H':
                    pp=ps.split(';') if ps else ['1','1']
                    r=int(pp[0] or 1)-1; c=int(pp[1] or 1)-1 if len(pp)>1 else 0
                elif f=='J' and ps in('','2'):
                    for rr in range(rows):
                        for cc in range(cols): grid[rr][cc]=Cell()
                elif f=='K':
                    if 0<=r<rows:
                        for cc in range(c,cols): grid[r][cc]=Cell()
                elif f=='m':
                    parts=[int(x) for x in ps.split(';') if x!=''] or [0]
                    j=0
                    while j<len(parts):
                        v=parts[j]
                        if v==0: fg=bg=None;bold=dim=rev=False
                        elif v==1: bold=True
                        elif v==2: dim=True
                        elif v==7: rev=True
                        elif v==27: rev=False
                        elif v==22: bold=dim=False
                        elif 30<=v<=37 or 90<=v<=97: fg=C16[v]
                        elif 40<=v<=47: bg=C16[v-10]
                        elif v==39: fg=None
                        elif v==49: bg=None
                        elif v==38 and j+2<len(parts) and parts[j+1]==5: fg=c256(parts[j+2]); j+=2
                        elif v==48 and j+2<len(parts) and parts[j+1]==5: bg=c256(parts[j+2]); j+=2
                        j+=1
                i+=m.end(); continue
            m2=re.match(rb'\x1b\][^\x07]*\x07|\x1b[()][B0]|\x1b.',data[i:])
            i+=m2.end() if m2 else 1; continue
        if b==0x0d: c=0; i+=1; continue
        if b==0x0a: r=min(r+1,rows-1); i+=1; continue
        if b==0x07 or b==0x08: i+=1; continue
        n=1
        if b>=0xC0: n=2 if b<0xE0 else 3 if b<0xF0 else 4
        ch=data[i:i+n].decode('utf-8','replace')
        if 0<=r<rows and 0<=c<cols:
            cell=grid[r][c]; cell.ch=ch; cell.fg=fg; cell.bg=bg; cell.bold=bold; cell.dim=dim; cell.rev=rev
        c+=1; i+=n
    return grid

def to_html(grid):
    out=[]
    for row in grid:
        line=[]; cur=None; run=[]
        def flush():
            if run:
                st=cur; txt=html.escape(''.join(run))
                fg,bgc,bold,dim,rev=st
                if rev: fg,bgc=(bgc or '#101010'),(fg or '#d7dae0')
                css=[]
                if fg: css.append('color:%s'%fg)
                if bgc: css.append('background:%s'%bgc)
                if bold: css.append('font-weight:700')
                if dim: css.append('opacity:.55')
                line.append('<span style="%s">%s</span>'%(';'.join(css),txt) if css else txt)
        for cell in row:
            st=(cell.fg,cell.bg,cell.bold,cell.dim,cell.rev)
            if st!=cur:
                flush(); cur=st; run=[]
            run.append(cell.ch)
        flush()
        out.append(''.join(line).rstrip() if not any(c.bg or c.rev for c in row) else ''.join(line))
    while out and not out[-1].strip(): out.pop()
    return '\n'.join(out)

data=open(sys.argv[1],'rb').read()
g=simulate(data,int(sys.argv[2]),int(sys.argv[3]))
open(sys.argv[4],'w').write(to_html(g))
print('rows out ok')
