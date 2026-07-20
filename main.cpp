#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <vector>
#define PI 3.14159265358979323846f
#define HEAD_BONE 5   // 绘制.cpp 人类头=5; 若快照坐标看着不在头上, 改6/4 试

// 探针状态
volatile int  g_slot    = 3;   // 起始V槽, 写 V[s]/V[s+1]/V[s+2]
volatile int  g_payload = 2;   // 0=坐标 1=角度 2=魔数(85/90/0)
volatile bool g_inject  = true;// 注入开关(false=基线对照)
volatile bool g_print   = false;// 请求打印当前将写的值
volatile bool g_Running = true;
static const char* pname(int p){ return p==0?"坐标":(p==1?"角度":"魔数"); }
static void sig(int){ g_Running=false; }

void* Input(void*){
    char b[32];
    printf("\n\033[1;33m===== 寄存器探针(读敌人坐标试槽位) =====\033[0m\n");
    printf(" a/z=槽±(当前%d)  c/g/m=坐标/角度/魔数(当前%s)\n", g_slot, pname(g_payload));
    printf(" o=注入开关  p=打印将写的值  q=退出\n", );
    printf(" \033[1;31m用法: 单点射! 一枪一配置! 开枪前按p确认坐标非0\033[0m\n");
    printf("\033[1;33m======================================\033[0m\n");
    while(g_Running){
        if(!fgets(b,sizeof(b),stdin)) continue;
        char*p=b; while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n')continue; char c=*p;
        if(c=='q'||c=='Q'){g_Running=false;printf("\n[q] 退出\n");break;}
        else if(c=='a'||c=='A'){if(g_slot<5)g_slot++;printf("\n\033[1;35m[*] 起始槽=%d -> 写V%d,V%d,V%d\033[0m\n",g_slot,g_slot,g_slot+1,g_slot+2);}
        else if(c=='z'||c=='Z'){if(g_slot>0)g_slot--;printf("\n\033[1;35m[*] 起始槽=%d -> 写V%d,V%d,V%d\033[0m\n",g_slot,g_slot,g_slot+1,g_slot+2);}
        else if(c=='c'||c=='C'){g_payload=0;printf("\n\033[1;35m[*] payload=坐标(敌人头XYZ)\033[0m\n");}
        else if(c=='g'||c=='G'){g_payload=1;printf("\n\033[1;35m[*] payload=角度(直线指头pitch/yaw)\033[0m\n");}
        else if(c=='m'||c=='M'){g_payload=2;printf("\n\033[1;35m[*] payload=魔数(85/90/0 标定用)\033[0m\n");}
        else if(c=='o'||c=='O'){g_inject=!g_inject;printf("\n\033[1;35m[*] 注入:%s (关=基线对照)\033[0m\n",g_inject?"开":"关");}
        else if(c=='p'||c=='P'){g_print=true;}
    }
    return NULL;
}
struct FVector{float X,Y,Z;}; struct FRotator{float Pitch,Yaw,Roll;};
struct FTransform{float rot[4],trans[3],scale[3];};
void getBone(paradise_driver*d,uintptr_t a,FTransform&o){uint8_t b[48]={0};d->read_fast(a,b,44);memcpy(o.rot,b,16);memcpy(o.trans,b+16,12);memcpy(o.scale,b+28,12);}
void T2M(const FTransform&T,float M[4][4]){float x=T.rot[0],y=T.rot[1],z=T.rot[2],w=T.rot[3],x2=x+x,y2=y+y,z2=z+z,xx=x*x2,xy=x*y2,xz=x*z2,yy=y*y2,yz=y*z2,zz=z*z2,wx=w*x2,wy=w*y2,wz=w*z2,sx=T.scale[0],sy=T.scale[1],sz=T.scale[2];M[0][0]=(1-(yy+zz))*sx;M[0][1]=(xy-wz)*sy;M[0][2]=(xz+wy)*sz;M[0][3]=0;M[1][0]=(xy+wz)*sx;M[1][1]=(1-(xx+zz))*sy;M[1][2]=(yz-wx)*sz;M[1][3]=0;M[2][0]=(xz-wy)*sx;M[2][1]=(yz+wx)*sy;M[2][2]=(1-(xx+yy))*sz;M[2][3]=0;M[3][0]=T.trans[0];M[3][1]=T.trans[1];M[3][2]=T.trans[2];M[3][3]=1;}
void MM(const float A[4][4],const float B[4][4],float C[4][4]){for(int i=0;i<4;i++)for(int j=0;j<4;j++)C[i][j]=A[i][0]*B[0][j]+A[i][1]*B[1][j]+A[i][2]*B[2][j]+A[i][3]*B[3][j];}
static std::vector<pid_t> tids(pid_t p){std::vector<pid_t>v;char s[64];snprintf(s,64,"/proc/%d/task",p);DIR*d=opendir(s);if(!d)return v;struct dirent*e;while((e=readdir(d)))if(e->d_name[0]!='.'){int t=atoi(e->d_name);if(t>0)v.push_back(t);}closedir(d);return v;}
// 写3个连续V槽的低32位float
static void fill3(HW_BP_INFO&i,int s,float a,float b,float c,bool inj){
    i.is_write_gp_regs=false; i.is_write_fp_regs=inj; i.fp_reg_count=3;
    i.fp_reg_indices[0]=s; i.fp_reg_indices[1]=s+1; i.fp_reg_indices[2]=s+2;
    memset(i.fp_reg_values[0],0,16);memset(i.fp_reg_values[1],0,16);memset(i.fp_reg_values[2],0,16);
    memcpy(&i.fp_reg_values[0][0],&a,4);memcpy(&i.fp_reg_values[1][0],&b,4);memcpy(&i.fp_reg_values[2][0],&c,4);
}
static void cleanup(paradise_driver&d,std::vector<HW_BP_INFO>&v){printf("\n\033[1;33m== 清断点 ==\033[0m\n");for(auto&i:v)d.hwbp_disable(i.pid,i.addr);d.hwbp_clear();printf("\033[1;32m[+] 已清除\033[0m\n");}
int main(){
    system("setenforce 0"); signal(SIGINT,sig); signal(SIGTERM,sig);
    paradise_driver drv;
    pid_t pid=drv.get_pid("com.tencent.ig"); if(pid<=0)pid=drv.get_pid("com.rekoo.pubgm"); if(pid<=0)pid=drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 无游戏\n");return 1;} drv.initialize(pid);
    uintptr_t base=drv.get_module_base("libUE4.so"); if(!base){printf("[-] 无基址\n");return 1;}
    uintptr_t addr=base+0x6DFE100; printf("[+] pid=%d shoot_event=0x%lx\n",pid,addr);
    auto ts=tids(pid); std::vector<HW_BP_INFO>infos; int ok=0;
    for(pid_t t:ts){HW_BP_INFO i={0};i.pid=t;i.addr=addr;i.type=HW_BP_TYPE_X;i.len=4;if(drv.hwbp_add(&i)){ok++;infos.push_back(i);}}
    printf("[+] 断点 %d/%zu\n",ok,ts.size()); if(!ok){printf("[-] 全失败\n");return 1;}
    pthread_t ti; pthread_create(&ti,NULL,Input,NULL); pthread_detach(ti);
    static bool last=false;
    while(g_Running){
        bool found=false; FVector H={0,0,0}; float ap=0,ay=0;
        uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900),pB=drv.read_fast<uintptr_t>(pA+0x810),UW=drv.read_fast<uintptr_t>(pB+0x78);
        if(UW>0x10000){
            uintptr_t pD=drv.read_fast<uintptr_t>(UW+0x38),pE=drv.read_fast<uintptr_t>(pD+0x78),pF=drv.read_fast<uintptr_t>(pE+0x30),Me=drv.read_fast<uintptr_t>(pF+0x28c8);
            if(Me>0x10000){
                int selfTeam=drv.read_fast<int>(Me+0x998);
                uintptr_t PC=drv.read_fast<uintptr_t>(Me+0x4b18),Cam=drv.read_fast<uintptr_t>(PC+0x548);
                FVector CP={0,0,0}; if(Cam>0x10000)CP=drv.read_fast<FVector>(Cam+0x530);
                if(CP.X!=0||CP.Y!=0){
                    uintptr_t Lv=drv.read_fast<uintptr_t>(UW+0x30),Arr=drv.read_fast<uintptr_t>(Lv+0xA0); int Cnt=drv.read_fast<int>(Lv+0xA8);
                    float minD=999999; int N=Cnt; if(N<0)N=0; if(N>512)N=512;
                    for(int i=0;i<N;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i); if(O<0x10000||O==Me)continue;
                        if(drv.read_fast<float>(O+0x2b78)!=479.5f)continue;
                        int team=drv.read_fast<int>(O+0x998); if(selfTeam!=0&&selfTeam!=-1&&team==selfTeam)continue;
                        int st=drv.read_fast<int>(O+0x1058); if(st==1048592||st==1048576)continue;
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510); if(Mesh<0x10000)continue;
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30; if(BB<0x10000)continue;
                        FTransform mT,hT; getBone(&drv,Mesh+0x210,mT); getBone(&drv,BB+HEAD_BONE*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4]; T2M(mT,c2w);T2M(hT,bM);MM(bM,c2w,fM);
                        FVector P={fM[3][0],fM[3][1],fM[3][2]};
                        float dx=P.X-CP.X,dy=P.Y-CP.Y,dz=P.Z-CP.Z,d=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        if(d<minD&&d<300){minD=d;H=P;found=true;}
                    }
                    if(found){
                        float dx=H.X-CP.X,dy=H.Y-CP.Y,dz=H.Z-CP.Z,d2=sqrt(dx*dx+dy*dy); if(d2<0.01f)d2=0.01f;
                        ap=atan2(dz,d2)*(180.f/PI); ay=atan2(dy,dx)*(180.f/PI); // 直线指头, 无下坠
                    }
                }
            }
        }
        // 选 payload 三元组
        float v0=0,v1=0,v2=0;
        if(g_payload==0){ v0=H.X; v1=H.Y; v2=H.Z; }              // 坐标
        else if(g_payload==1){ v0=ap; v1=ay; v2=0; }              // 角度
        else { v0=85.0f; v1=90.0f; v2=0; }                        // 魔数
        // 响应打印请求(用真实将注入的值, 确认读链路没坏)
        if(g_print&&found){
            g_print=false;
            printf("\n\033[1;36m[快照] 槽=%d payload=%s 将写 V%d=%.1f V%d=%.1f V%d=%.1f\033[0m\n",
                   g_slot,pname(g_payload),g_slot,v0,g_slot+1,v1,g_slot+2,v2);
            if(g_payload==0&&(v0==0&&v1==0)) printf("\033[1;31m[!!] 坐标=0, 读链路坏, 此枪无效!\033[0m\n");
        } else if(g_print){ g_print=false; printf("\n\033[1;33m[快照] 当前无目标, 瞄个人机再按p\033[0m\n"); }
        // 注入
        for(auto&inf:infos){ fill3(inf,g_slot,v0,v1,v2,g_inject&&found); drv.hwbp_enable(&inf); }
        if(found!=last){last=found; if(found)printf("\n\033[1;32m[+] 锁定 (槽%d/%s)\033[0m\n",g_slot,pname(g_payload)); else printf("\n\033[1;33m[-] 丢失\033[0m\n");}
        usleep(10000);
    }
    cleanup(drv,infos); return 0;
}
