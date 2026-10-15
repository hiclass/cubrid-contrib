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
# Method A counts large blocks that sit alone in their mapping.  glibc serves a request above
# its mmap threshold (and lea's large blocks, which go through malloc) as a mmapped chunk:
# one chunk, or a few merged neighbours, fill the VMA and give fewer than the four hops the
# general start probe needs.  malloc(100MB), 3 x 20MB (one merged VMA) and 5 x 20MB must all
# be counted in full, a block over the 768MB walk bound too, also when an anonymous neighbour is
# merged into the same VMA in front or behind; zeroed anonymous memory stays 0.
# Size labels say what the size is: lea (db_private_alloc) grows by malloc(32K x n) and its
# base is malloc(64K), in an arena or mmapped (page-rounded); a 96B chunk is any 73-88B request.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC=${1:-"$ROOT/src/cub_top.c"}
TMP=${TMPDIR:-/tmp}/cub_top-mmchunk.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
#include <malloc.h>
static int vma_of(void *p,unsigned long *s,unsigned long *e){
    FILE *f=fopen("/proc/self/maps","r"); char ln[512]; unsigned long a=(unsigned long)(uintptr_t)p;
    while(f && fgets(ln,sizeof ln,f)) if(sscanf(ln,"%lx-%lx",s,e)==2 && a>=*s && a<*e){ fclose(f); return 1; }
    if(f) fclose(f); return 0;
}
static int bad=0;
/* want: the bytes of the chunks under test inside the mapping */
static void check(const char *what,void *p,double want){
    unsigned long s,e; if(!vma_of(p,&s,&e)){ printf("  FAIL %s: no mapping\n",what); bad=1; return; }
    memset(&A,0,sizeof A); walk_region(getpid(),s,e);
    int ok = A.total==want && A.trunc==0;
    printf("  %s   %-38s VMA %6.1fMB: counted %6.1fMB in %ld chunk(s) (want %.1fMB)\n",ok?"ok  ":"FAIL",what,
           (e-s)/1048576.0,A.total/1048576.0,A.chunks,want/1048576.0);
    bad|=!ok;
}
static double csz(void *p){ return (double)(((unsigned long*)p)[-1]&~7UL); }
int main(void){
    CAP.vm_readv=1;
    mallopt(M_MMAP_THRESHOLD,128*1024);             /* fixed: no dynamic threshold creeping up */
    /* Each case in a mapping of its own: a no-reserve spacer below keeps it from merging with the last */
    #define SPACER() mmap(NULL,1u<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0)
    void *a=malloc(100u<<20);                      check("malloc(100MB)",a,csz(a));
    SPACER(); void *b[3]; double wb=0; for(int i=0;i<3;i++){ b[i]=malloc(20u<<20); wb+=csz(b[i]); }
    check("3 x 20MB (one merged VMA)",b[0],wb);
    SPACER(); void *c[5]; double wc=0; for(int i=0;i<5;i++){ c[i]=malloc(20u<<20); wc+=csz(c[i]); }
    check("5 x 20MB",c[0],wc);
    SPACER(); void *g=malloc((size_t)900<<20);     check("malloc(900MB), over the walk bound",g,csz(g));
    /* A neighbour merged in front, holding words that are no chunk header */
    SPACER(); void *d=malloc(20u<<20);
    unsigned char *j=mmap(NULL,64u<<10,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    for(int i=0;i<(64<<10);i++) j[i]=(unsigned char)(i*37+11);
    unsigned long s0,e0; vma_of(d,&s0,&e0);
    printf("  %s   neighbour merged in front: VMA starts %s the junk\n",(unsigned long)(uintptr_t)j==s0?"info":"note",(unsigned long)(uintptr_t)j==s0?"at":"not at");
    check("20MB behind 64K of junk",d,csz(d));
    /* ... and one merged behind: the walk stops at the first header that is not a mmapped chunk */
    SPACER(); unsigned char *j2=mmap(NULL,64u<<10,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    for(int i=0;i<(64<<10);i++) j2[i]=(unsigned char)(i*53+7);
    void *f=malloc(20u<<20);
    check("20MB followed by 64K of junk",f,csz(f));
    { struct { size_t req; int want; const char *what; } cs[]={
        {32768,0,"malloc(32K), arena: lea segment"},{65536,0,"malloc(64K) (lea base), arena"},
        {10*32768,0,"malloc(320K), mmapped: lea segment"},{80,1,"malloc(80): 96B chunk"},{73,1,"malloc(73): 96B chunk"},
        {100,5,"malloc(100): other under 8K"},{(2u<<20)+5000,2,"malloc(2M+5000), mmapped: other 1M+"},{40000,4,"malloc(40000): other 8K~64K"} };
      for(unsigned k=0;k<sizeof cs/sizeof cs[0];k++){
          unsigned long *p=malloc(cs[k].req); unsigned long raw=p[-1];
          int got=classify((long)(raw&CHUNK_MASK),(int)((raw>>1)&1));
          int ok=got==cs[k].want;
          printf("  %s   %-38s chunk %lu%s -> %s\n",ok?"ok  ":"FAIL",cs[k].what,raw&CHUNK_MASK,(raw&2)?" (mmapped)":"",ACAT[got]);
          bad|=!ok; free(p); } }
    void *z=mmap(NULL,8u<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    memset(z,0,4096);                              check("zeroed anonymous 8MB (not a chunk)",z,0);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
