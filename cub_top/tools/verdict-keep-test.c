/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
#include <stdio.h>
#include <string.h>
#define VDKEEP 2
static struct { int kind; char msg[256]; char hm[8]; int live; } g_vd[VDKEEP];
static int g_vdn=0;
/* Identical to cub_top.c's retention logic */
static void step(int verdict,const char*msg,const char*hm){
    if(verdict){
        if(g_vdn>0 && g_vd[0].kind==verdict){
            snprintf(g_vd[0].msg,sizeof g_vd[0].msg,"%s",msg);
            snprintf(g_vd[0].hm,sizeof g_vd[0].hm,"%s",hm);
            g_vd[0].live=1;
        } else {
            if(g_vdn>0) g_vd[0].live=0;
            for(int k=VDKEEP-1;k>0;k--) g_vd[k]=g_vd[k-1];
            g_vd[0].kind=verdict;
            snprintf(g_vd[0].msg,sizeof g_vd[0].msg,"%s",msg);
            snprintf(g_vd[0].hm,sizeof g_vd[0].hm,"%s",hm);
            g_vd[0].live=1;
            if(g_vdn<VDKEEP) g_vdn++;
        }
    } else if(g_vdn>0) g_vd[0].live=0;
}
static void show(const char*tag){
    printf("  %-22s rows=%d :",tag,g_vdn);
    for(int k=0;k<g_vdn;k++)
        printf(" [%s %s k%d %s]",g_vd[k].hm,g_vd[k].msg,g_vd[k].kind,g_vd[k].live?"live":"past");
    printf("\n");
}
int main(void){
    int bad=0;
    step(0,"","");                      show("1 idle");
    if(g_vdn!=0) bad++;
    step(1,"bottleneck","10:01");             show("2 bottleneck occurs");
    if(g_vdn!=1||!g_vd[0].live) bad++;
    step(0,"","");                      show("3 condition clears (must be retained)");
    if(g_vdn!=1||g_vd[0].live) { bad++; puts("     <<< vanished, or live was not cleared"); }
    step(1,"bottleneck2","10:03");            show("4 same kind recurs (no extra row)");
    if(g_vdn!=1) { bad++; puts("     <<< same kind but a row was added"); }
    step(2,"masked","10:05");           show("5 different kind (pushes down)");
    if(g_vdn!=2||g_vd[0].kind!=2||g_vd[1].kind!=1) { bad++; puts("     <<< push-down failed"); }
    step(4,"someone else","10:07");         show("6 third entry (two-row limit)");
    if(g_vdn!=2||g_vd[0].kind!=4||g_vd[1].kind!=2) { bad++; puts("     <<< over the limit or out of order"); }
    step(0,"","");                      show("7 idle again");
    if(g_vdn!=2||g_vd[0].live) bad++;
    if(g_vd[1].live) { bad++; puts("     <<< a pushed-down entry stayed live"); }
    printf("%s\n",bad?"FAIL":"PASS (7 steps)");
    return bad?1:0;
}
