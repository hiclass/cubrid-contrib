#!/bin/sh
#
# Copyright 2016 CUBRID Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# BCB header recheck: a page replaced between the BCB read and the header read must
# become the new page as a whole (flags, zone, oldest LSA), and the totals must follow.
# In degraded mode only verified records may be counted, dumped or compared, and an
# allocation failure must not turn mismatches into a pass; carried headers must stay current.
# pgbuf_scan() reads a fake buffer pool in the test process; a hook inserted after
# pass 1 replaces one BCB's page, deterministically.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-recheck.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
sed 's|^    if(PG.badvpid>nbuf/100){ PG.ok=0;|    { extern void test_after_pass1(void); test_after_pass1(); }\n&|' "$SRC" > "$TMP/src.c"
grep -q test_after_pass1 "$TMP/src.c" || { echo "  hook point not found"; echo "→ FAIL"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'HC'
/* Fake buffer pool in this process: NB BCBs, each with its own io page.  After pass 1
   the hook replaces BCB 3's page (dirty page A -> clean page B), as a replacement between
   the BCB read and the header read would. */
#define NB 8
static unsigned char pool_[4096], *bcbs_, *iop_[NB];
static void put_bcb(int i,int pageid,int volid,uint32_t flags,uint64_t olsa){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb;
    memcpy(b+g_bt->pg.vpid,&pageid,4); int16_t v=(int16_t)volid; memcpy(b+g_bt->pg.vpid+4,&v,2);
    memcpy(b+g_bt->pg.flags,&flags,4); memcpy(b+g_bt->pg.olsa,&olsa,8);
    uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8);
}
static void put_hdr(int i,int pageid,int volid,uint64_t lsa){
    unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int16_t v=(int16_t)volid;
    memcpy(h,&lsa,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
static uint64_t lsa_raw(long long pg,int off){ return ((uint64_t)pg)|((uint64_t)(uint16_t)off<<48); }
void test_after_pass1(void){
    put_bcb(3,200,0,0x00010000u /* LRU1, clean */,lsa_raw(-1,-1));
}
int main(void){
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    for(int i=0;i<NB;i++) iop_[i]=calloc(1,(size_t)g_bt->pg.iobuf_iopg+64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){ put_bcb(i,10+i,0,0x00020000u,lsa_raw(-1,-1)); put_hdr(i,10+i,0,lsa_raw(5,8)); }
    put_bcb(3,100,0,0x80030000u /* dirty, LRU3 */,lsa_raw(77,40));   /* page A, dirty since LSA 77|40 */
    put_hdr(3,200,0,lsa_raw(90,16));                                     /* the frame already holds page B */
    pgbuf_scan(&P);
    int k=-1; for(int i=0;i<PG.nrec;i++) if(PG.rec[i].pageid==200) k=i;
    if(k<0){ printf("record for page B not found (recheck=%d mismatch=%d)\n",PG.recheck,PG.mismatch); return 1; }
    pgrec_t*r=&PG.rec[k];
    printf("page B record: hdr_ok=%d dirty_flag=%d zone=%d oldest_lsa=%s | totals: dirty=%d z2=%d z3=%d z1=%d oldest=%s\n",
           r->hdr_ok,(r->flags&PGBUF_F_DIRTY)?1:0,r->zone,lsa_str(lsa_dec(r->oldest_lsa)),
           PG.dirty,PG.z2,PG.z3,PG.z1,PG.have_oldest?lsa_str(PG.oldest_dirty):"-");
    int ok = r->hdr_ok==1 && !(r->flags&PGBUF_F_DIRTY) && r->zone==1 && lsa_isnull(lsa_dec(r->oldest_lsa))
          && PG.dirty==0 && PG.z1==1 && PG.z3==0 && PG.z2==NB-1 && !PG.have_oldest;
    printf("%s\n",ok?"consistent":"MIXED");
    return ok?0:1;
}
HC
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
"$TMP/t" > "$TMP/out"; rc=$?
sed 's/^/  /' "$TMP/out"
# Degraded mode: above 5% unverifiable records, only verified records are counted, the
# dump is not overwritten and turnover skips the frame.
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/d.c"
cat >> "$TMP/d.c" <<'HD'
void test_after_pass1(void){}   /* no replacement in this case */
/* Fake buffer pool: 200 BCBs, 20 of them (dirty) never match their header - 10%, which is
   above the 5% degrade threshold and below the 50% cut-off. */
#define NB 200
#define NBAD 20
static unsigned char pool_[4096], *bcbs_, *iop_[NB];
static void put_bcb(int i,int pageid,uint32_t flags,uint64_t olsa){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2);
    memcpy(b+g_bt->pg.flags,&flags,4); memcpy(b+g_bt->pg.olsa,&olsa,8);
    uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8);
}
static void put_hdr(int i,int pageid,uint64_t lsa){
    unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int16_t v=0;
    memcpy(h,&lsa,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
static uint64_t lsa_raw(long long pg,int off){ return ((uint64_t)pg)|((uint64_t)(uint16_t)off<<48); }
int main(int argc,char**argv){
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    if(argc>1) snprintf(g_bcb_dump,sizeof g_bcb_dump,"%s",argv[1]);
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    for(int i=0;i<NB;i++) iop_[i]=calloc(1,(size_t)g_bt->pg.iobuf_iopg+64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){
        if(i<NBAD){ put_bcb(i,1000+i,0x80020000u,lsa_raw(50,i)); put_hdr(i,5000+i,lsa_raw(5,8)); }   /* never verifiable */
        else      { put_bcb(i,1000+i,0x00020000u,lsa_raw(-1,-1)); put_hdr(i,1000+i,lsa_raw(5,8)); }
    }
    int ver=0; for(int f=0;f<3;f++){ pgbuf_scan(&P); }
    for(int i=0;i<PG.nrec;i++) if(PG.rec[i].hdr_ok) ver++;
    printf("degraded=%d nrec=%d verified=%d resident=%d dirty=%d vol0_res=%d oldest=%s",
           PG.degraded,PG.nrec,ver,PG.resident,PG.dirty,PG.vol_res[0],PG.have_oldest?lsa_str(PG.oldest_dirty):"-");
    if(g_bcb_dump[0]){ FILE*f=fopen(g_bcb_dump,"rb"); printf(" dump=%s",f?"written":"not written"); if(f) fclose(f); }
    printf("\n");
    int ok = PG.degraded && PG.nrec==NB-NBAD && ver==NB-NBAD && PG.resident==NB-NBAD && PG.dirty==0 && !PG.have_oldest;
    printf("%s\n",ok?"verified only":"UNVERIFIED COUNTED");
    return ok?0:1;
}
HD
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/d" "$TMP/d.c" 2>"$TMP/cc2.err" || { cat "$TMP/cc2.err"; echo "→ FAIL (compile failed)"; exit 1; }
"$TMP/d" "$TMP/dump" > "$TMP/out2" || rc=1
sed 's/^/  /' "$TMP/out2"
[ -f "$TMP/dump" ] && { echo "  dump written by a degraded frame"; rc=1; }
# Allocation failure: with the mismatch index arrays unavailable, a pool whose headers
# all disagree must not pass as healthy.
sed 's|midx=(int\*)malloc(sizeof(int)\*(size_t)PG.nrec);|midx=NULL; /* injected failure */|' "$TMP/src.c" > "$TMP/srcm.c"
if grep -q "injected failure" "$TMP/srcm.c"; then
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/srcm.c" > "$TMP/m.c"
cat >> "$TMP/m.c" <<'HM'
/* Fake buffer pool: 200 BCBs, 20 of them (dirty) never match their header - 10%, which is
   above the 5% degrade threshold and below the 50% cut-off. */
#define NB 200
#define NBAD 200
static unsigned char pool_[4096], *bcbs_, *iop_[NB];
static void put_bcb(int i,int pageid,uint32_t flags,uint64_t olsa){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2);
    memcpy(b+g_bt->pg.flags,&flags,4); memcpy(b+g_bt->pg.olsa,&olsa,8);
    uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8);
}
static void put_hdr(int i,int pageid,uint64_t lsa){
    unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int16_t v=0;
    memcpy(h,&lsa,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
static uint64_t lsa_raw(long long pg,int off){ return ((uint64_t)pg)|((uint64_t)(uint16_t)off<<48); }
int main(int argc,char**argv){
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    if(argc>1) snprintf(g_bcb_dump,sizeof g_bcb_dump,"%s",argv[1]);
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    for(int i=0;i<NB;i++) iop_[i]=calloc(1,(size_t)g_bt->pg.iobuf_iopg+64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){
        if(i<NBAD){ put_bcb(i,1000+i,0x80020000u,lsa_raw(50,i)); put_hdr(i,5000+i,lsa_raw(5,8)); }   /* never verifiable */
        else      { put_bcb(i,1000+i,0x00020000u,lsa_raw(-1,-1)); put_hdr(i,1000+i,lsa_raw(5,8)); }
    }
    pgbuf_scan(&P);
    printf("every header wrong, mismatch-index allocation failed: PG.ok=%d degraded=%d mismatch=%d raw=%d why=\"%s\"\n",
           PG.ok,PG.degraded,PG.mismatch,PG.mismatch_raw,PG.why);
    return PG.ok?1:0;
}
void test_after_pass1(void){}
HM
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/m" "$TMP/m.c" 2>"$TMP/cc3.err" || { cat "$TMP/cc3.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/m" > "$TMP/out3"; then echo "  ok   allocation failure drops the frame"; else echo "  FAIL allocation failure passed as healthy"; rc=1; fi
sed 's/^/       /' "$TMP/out3"
else echo "  FAIL allocation point not found"; rc=1; fi
# Header reuse: a carried header must never hide a dirty page's change or a new page, and a
# clean page whose LSA moved must be picked up within HC_SLICE frames.
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/r.c"
cat >> "$TMP/r.c" <<'HR'
void test_after_pass1(void){}
/* Header reuse: 64 BCBs.  BCB 5 stays clean while its header LSA changes (modified and
   flushed between frames), BCB 6 is dirty and changes every frame, BCB 7 gets a new page,
   BCB 8 was clean when verified and turns dirty with a new LSA. */
#define NB 64
static unsigned char pool_[4096], *bcbs_, *iop_[NB];
static void put_bcb(int i,int pageid,uint32_t flags){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0; uint64_t ol=~0ull;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2);
    memcpy(b+g_bt->pg.flags,&flags,4); memcpy(b+g_bt->pg.olsa,&ol,8);
    uint64_t ip=(uint64_t)(uintptr_t)iop_[i]; memcpy(b+g_bt->pg.iopg,&ip,8);
}
static void put_hdr(int i,int pageid,uint64_t lsa){
    unsigned char*h=iop_[i]+g_bt->pg.iobuf_iopg; int16_t v=0;
    memcpy(h,&lsa,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
static uint64_t rec_lsa(int pageid){ for(int i=0;i<PG.nrec;i++) if(PG.rec[i].pageid==pageid) return PG.rec[i].page_lsa; return 0; }
int main(void){
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb);
    for(int i=0;i<NB;i++) iop_[i]=calloc(1,(size_t)g_bt->pg.iobuf_iopg+64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++){ put_bcb(i,100+i,0x00020000u); put_hdr(i,100+i,1000+i); }
    put_bcb(6,106,0x80020000u);                         /* dirty */
    int bad=0;
    pgbuf_scan(&P);
    printf("frame 1: read %d reused %d (all read)\n",PG.hdr_read,PG.hdr_reused);
    if(PG.hdr_read!=NB) bad++;
    put_hdr(5,105,5555);                                /* clean page, LSA moved */
    put_bcb(7,207,0x00020000u); put_hdr(7,207,7777);    /* new page in BCB 7 */
    put_bcb(8,108,0x80020000u); put_hdr(8,108,8888);    /* clean when verified, dirty now */
    int seen5=-1, max_read=0;
    for(int f=2;f<=10;f++){
        put_hdr(6,106,6000+f);                          /* dirty page keeps changing */
        pgbuf_scan(&P);
        if(PG.hdr_read>max_read) max_read=PG.hdr_read;
        if(rec_lsa(106)!=(uint64_t)(6000+f)){ bad++; printf("frame %d: dirty page LSA stale\n",f); }
        if(f==2 && rec_lsa(207)!=7777){ bad++; printf("frame 2: new page not read\n"); }
        if(f==2 && rec_lsa(108)!=8888){ bad++; printf("frame 2: page turned dirty was not read\n"); }
        if(seen5<0 && rec_lsa(105)==5555) seen5=f;
    }
    printf("frames 2-10: at most %d headers read per frame; moved clean LSA picked up at frame %d\n",max_read,seen5);
    if(seen5<0 || seen5>2+HC_SLICE) bad++;
    if(max_read>NB/HC_SLICE+4) bad++;
    printf("%s\n",bad?"REUSE WRONG":"reuse ok");
    return bad?1:0;
}
HR
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/r" "$TMP/r.c" 2>"$TMP/cc4.err" || { cat "$TMP/cc4.err"; echo "→ FAIL (compile failed)"; exit 1; }
"$TMP/r" > "$TMP/out4" || rc=1
sed 's/^/  /' "$TMP/out4"
[ $rc -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
