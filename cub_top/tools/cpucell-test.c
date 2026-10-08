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
/* Check no CPU-row cell overflows its width on worst-case input */
#define CPUCELL(dst,lab,val,cap) do{ \
    snprintf(dst,sizeof dst,"%s %.0f%%",lab,val); \
    if((int)strlen(dst)>(cap)) snprintf(dst,sizeof dst,"%.0f%%",val); \
    if((int)strlen(dst)>(cap)) snprintf(dst,sizeof dst,"%.0f",val); \
}while(0)
int main(void){
    int bad=0;
    double vals[]={0,5,17,99,100,100.4};
    for(unsigned i=0;i<sizeof vals/sizeof*vals;i++){
        char c4[16],c5[16];
        CPUCELL(c4,"us",vals[i],7);
        CPUCELL(c5,"sy",vals[i],6);
        printf("  %6.1f%% -> us \"%-7s\"(%d) sy \"%-6s\"(%d) %s\n",
               vals[i],c4,(int)strlen(c4),c5,(int)strlen(c5),
               ((int)strlen(c4)<=7&&(int)strlen(c5)<=6)?"OK":"<<< overflow");
        if((int)strlen(c4)>7||(int)strlen(c5)>6) bad++;
    }
    /* sat cell (7) */
    for(int n=0;n<=999;n+=333){
        char c3[16]; snprintf(c3,sizeof c3,"sat %d",n);
        printf("  sat=%-3d  -> \"%-7s\"(%d) %s\n",n,c3,(int)strlen(c3),
               (int)strlen(c3)<=7?"OK":"<<< overflow");
        if((int)strlen(c3)>7) bad++;
    }
    printf("%s\n",bad?"FAIL":"PASS");
    return bad?1:0;
}
