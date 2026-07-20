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
#define HEAD_BONE_IDX 5
// g_drop 默认0: 发射旋转路径内挂也不加下坠, 与源码一致
volatile float g_yaw_offset=0, g_pitch_offset=0, g_drop=0.0f, g_bullet_speed=0.0f;
volatile bool g_yaw_invert=false, g_team_filter=true, g_skip_bot=false, g_Running=true;
volatile bool g_swap_pv=false, g_yaw_flip=false;   // 保险丝: x=交换V3V4  n=水平+180 (正常别按)
static void sig_handler(int){ g_Running=false; const char m[]="\n[!] 退出清理中...\n"; write(STDERR_FILENO,m,sizeof(m)-1); }
void* InputThread(void*){
    char buf[64];
    printf("\n\033[1;33m===== 弹道控制台(ToRotator版) =====\033[0m\n");
    printf("  \033[1;32m默认自动追踪, 角度已用内挂调通公式, 无需按键。\033[0m\n");
    printf("  字母后可跟数字=自定义幅度 例: l 3 / u 0.5 / o 50\n");
    printf("  b=反转水平 l/r=水平±(默认15) u/d=俯仰±(默认5)\n");
    printf("  o/p=下坠±(默认20, 通常别动) t=队友过滤 g=跳人机\n");
    printf("  \033[1;31mx=交换V3V4  n=水平+180 (仅当子弹飞反了才按!)\033[0m\n");
    printf("  \033[1;31mq=清除断点退出(Ctrl+C同效)\033[0m\n\033[1;33m======================\033[0m\n");
    while(g_Running){
        if(!fgets(buf,sizeof(buf),stdin)) continue;
        char* p=buf; while(*p==' '||*p=='\t') p++;
        if(*p==0||*p=='\n'||*p=='\r') continue;
        char c=*p; float arg=0; bool has=(sscanf(p+1,"%f",&arg)==1);
        if(c=='q'||c=='Q'){ g_Running=false; printf("\n\033[1;31m[q] 退出中...\033[0m\n"); break; }
        else if(c=='b'||c=='B'){ g_yaw_invert=!g_yaw_invert; printf("\n\033[1;32m[*] 反转水平:%s\033[0m\n",g_yaw_invert?"开":"关"); }
        else if(c=='l'||c=='L'){ float d=has?arg:15.0f; g_yaw_offset+=d; printf("\n\033[1;32m[*] 水平%+.2f -> %.2f\033[0m\n",d,g_yaw_offset); }
        else if(c=='r'||c=='R'){ float d=has?arg:15.0f; g_yaw_offset-=d; printf("\n\033[1;32m[*] 水平%+.2f -> %.2f\033[0m\n",-d,g_yaw_offset); }
        else if(c=='u'||c=='U'){ float d=has?arg:5.0f; g_pitch_offset+=d; printf("\n\033[1;32m[*] 俯仰%+.2f -> %.2f\033[0m\n",d,g_pitch_offset); }
        else if(c=='d'||c=='D'){ float d=has?arg:5.0f; g_pitch_offset-=d; printf("\n\033[1;32m[*] 俯仰%+.2f -> %.2f\033[0m\n",-d,g_pitch_offset); }
        else if(c=='o'||c=='O'){ float d=has?arg:20.0f; g_drop+=d; printf("\n\033[1;32m[*] 下坠%+.2f -> %.2f\033[0m\n",d,g_drop); }
        else if(c=='p'||c=='P'){ float d=has?arg:20.0f; g_drop-=d; if(g_drop<0)g_drop=0; printf("\n\033[1;32m[*] 下坠%+.2f -> %.2f\033[0m\n",-d,g_drop); }
        else if(c=='t'||c=='T'){ g_team_filter=!g_team_filter; printf("\n\033[1;32m[*] 队友:%s\033[0m\n",g_team_filter?"开":"关"); }
        else if(c=='g'||c=='G'){ g_skip_bot=!g_skip_bot; printf("\n\033[1;32m[*] 跳人机:%s\033[0m\n",g_skip_bot?"开":"关"); }
        else if(c=='x'||c=='X'){ g_swap_pv=!g_swap_pv; printf("\n\033[1;35m[x] 交换V3V4:%s (上下反了才开)\033[0m\n",g_swap_pv?"开":"关"); }
        else if(c=='n'||c=='N'){ g_yaw_flip=!g_yaw_flip; printf("\n\033[1;35m[n] 水平+180:%s (左右反了才开)\033[0m\n",g_yaw_flip?"开":"关"); }
        else { float v=atof(p); if(v!=0||p[0]=='0'){ g_yaw_offset=v; printf("\n\033[1;32m[*] 水平强制%.2f\033[0m\n",g_yaw_offset); } }
    }
    return NULL;
}
struct FVector{float X,Y,Z;}; struct FRotator{float Pitch,Yaw,Roll;};
struct FTransform{float rot[4];float trans[3];float scale[3];};
void getBone(paradise_driver*d,uintptr_t a,FTransform&o){uint8_t b[48]={0};d->read_fast(a,b,44);memcpy(o.rot,b,16);memcpy(o.trans,b+16,12);memcpy(o.scale,b+28,12);}
void T2M(const FTransform&T,float M[4][4]){float x=T.rot[0],y=T.rot[1],z=T.rot[2],w=T.rot[3],x2=x+x,y2=y+y,z2=z+z,xx=x*x2,xy=x*y2,xz=x*z2,yy=y*y2,yz=y*z2,zz=z*z2,wx=w*x2,wy=w*y2,wz=w*z2,sx=T.scale[0],sy=T.scale[1],sz=T.scale[2];M[0][0]=(1-(yy+zz))*sx;M[0][1]=(xy-wz)*sy;M[0][2]=(xz+wy)*sz;M[0][3]=0;M[1][0]=(xy+wz)*sx;M[1][1]=(1-(xx+zz))*sy;M[1][2]=(yz-wx)*sz;M[1][3]=0;M[2][0]=(xz-wy)*sx;M[2][1]=(yz+wx)*sy;M[2][2]=(1-(xx+yy))*sz;M[2][3]=0;M[3][0]=T.trans[0];M[3][1]=T.trans[1];M[3][2]=T.trans[2];M[3][3]=1;}
void MM(const float A[4][4],const float B[4][4],float C[4][4]){for(int i=0;i<4;i++)for(int j=0;j<4;j++)C[i][j]=A[i][0]*B[0][j]+A[i][1]*B[1][j]+A[i][2]*B[2][j]+A[i][3]*B[3][j];}
static std::vector<pid_t> enum_tids(pid_t pid){std::vector<pid_t>v;char p[64];snprintf(p,sizeof(p),"/proc/%d/task",pid);DIR*d=opendir(p);if(!d)return v;struct dirent*e;while((e=readdir(d)))if(e->d_name[0]!='.'){int t=atoi(e->d_name);if(t>0)v.push_back(t);}closedir(d);return v;}
static void fill_inject(HW_BP_INFO&i,float p,float y,bool inj){i.is_write_gp_regs=false;i.is_write_fp_regs=inj;i.fp_reg_count=2;i.fp_reg_indices[0]=3;i.fp_reg_indices[1]=4;memset(i.fp_reg_values[0],0,16);memset(i.fp_reg_values[1],0,16);memcpy(&i.fp_reg_values[0][0],&p,4);memcpy(&i.fp_reg_values[1][0],&y,4);}
static void cleanup(paradise_driver&d,std::vector<HW_BP_INFO>&v){printf("\n\033[1;33m== 清除断点 ==\033[0m\n");for(auto&i:v)d.hwbp_disable(i.pid,i.addr);d.hwbp_clear();printf("\033[1;32m[+] 已清除\033[0m\n");}
int main(){
    system("setenforce 0"); signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler);
    paradise_driver drv;
    pid_t pid=drv.get_pid("com.tencent.ig"); if(pid<=0)pid=drv.get_pid("com.rekoo.pubgm"); if(pid<=0)pid=drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;} drv.initialize(pid);
    uintptr_t base=drv.get_module_base("libUE4.so"); if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t addr=base+0x6DFE100; printf("[+] pid=%d shoot_event=0x%lx\n",pid,addr);
    auto tids=enum_tids(pid); std::vector<HW_BP_INFO>infos; int add_ok=0;
    for(pid_t t:tids){HW_BP_INFO i={0};i.pid=t;i.addr=addr;i.type=HW_BP_TYPE_X;i.len=4;if(drv.hwbp_add(&i)){add_ok++;infos.push_back(i);}}
    printf("[+] 断点 %d/%zu\n",add_ok,tids.size()); if(add_ok==0){printf("[-] 全失败\n");return 1;}
    pthread_t tid_in; pthread_create(&tid_in,NULL,InputThread,NULL); pthread_detach(tid_in);
    int tick=0; static bool last_found=false;
    while(g_Running){
        float fp=0,fy=0; bool found=false,cam_ok=false,inject=false; float minDist=0; int bestTeam=0; bool bestBot=false;
        FVector CamPos={0,0,0};
        uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900),pB=drv.read_fast<uintptr_t>(pA+0x810),UWorld=drv.read_fast<uintptr_t>(pB+0x78);
        if(UWorld>0x10000){
            uintptr_t pD=drv.read_fast<uintptr_t>(UWorld+0x38),pE=drv.read_fast<uintptr_t>(pD+0x78),pF=drv.read_fast<uintptr_t>(pE+0x30),Oneself=drv.read_fast<uintptr_t>(pF+0x28c8);
            if(Oneself>0x10000){
                int selfTeam=drv.read_fast<int>(Oneself+0x998);
                uintptr_t _w0=drv.read_fast<uintptr_t>(Oneself+0x2608),_w1=drv.read_fast<uintptr_t>(_w0+0x5d8),_w2=drv.read_fast<uintptr_t>(_w1+0x1370);
                g_bullet_speed=drv.read_fast<float>(_w2+0x560);
                uintptr_t PC=drv.read_fast<uintptr_t>(Oneself+0x4b18),Cam=drv.read_fast<uintptr_t>(PC+0x548);
                if(Cam>0x10000){ CamPos=drv.read_fast<FVector>(Cam+0x530); if(CamPos.X!=0||CamPos.Y!=0) cam_ok=true; }
                if(cam_ok){
                    uintptr_t Uleve=drv.read_fast<uintptr_t>(UWorld+0x30),Arr=drv.read_fast<uintptr_t>(Uleve+0xA0); int Cnt=drv.read_fast<int>(Uleve+0xA8);
                    minDist=999999; FVector best={0,0,0}; int loopN=Cnt; if(loopN<0)loopN=0; if(loopN>512)loopN=512;
                    for(int i=0;i<loopN;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i); if(O<0x10000||O==Oneself)continue;
                        if(drv.read_fast<float>(O+0x2b78)!=479.5f)continue;
                        int team=drv.read_fast<int>(O+0x998);
                        if(g_team_filter&&selfTeam!=0&&selfTeam!=-1&&team==selfTeam)continue;
                        int st=drv.read_fast<int>(O+0x1058); if(st==1048592||st==1048576)continue;
                        int bf=drv.read_fast<int>(O+0xa59); bool isBot=(team!=0&&(bf==16842753||bf==16843009||bf==16843008));
                        if(g_skip_bot&&isBot)continue;
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510); if(Mesh<0x10000)continue;
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30; if(BB<0x10000)continue;
                        FTransform mT,hT; getBone(&drv,Mesh+0x210,mT); getBone(&drv,BB+HEAD_BONE_IDX*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4]; T2M(mT,c2w);T2M(hT,bM);MM(bM,c2w,fM);
                        FVector H={fM[3][0],fM[3][1],fM[3][2]};
                        float dx=H.X-CamPos.X,dy=H.Y-CamPos.Y,dz=H.Z-CamPos.Z,dist=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        if(dist<minDist&&dist<300){minDist=dist;best=H;found=true;bestTeam=team;bestBot=isBot;}
                    }
                    if(found){
                        if(g_drop>0.0001f&&g_bullet_speed>10.0f){ float _t=minDist/g_bullet_speed; best.Z+=g_drop*_t*_t; }
                        // ★ 复刻内挂 ToRotator(local=CamPos, target=best), 与 shoot_event 约定匹配
                        float rx=CamPos.X-best.X, ry=CamPos.Y-best.Y, rz=CamPos.Z-best.Z;
                        float hyp=sqrtf(rx*rx+ry*ry);
                        float P=0.0f,Hh=0.0f;
                        if(hyp>0.01f){
                            P = -atan2f(rz, hyp)*(180.0f/PI);                 // 俯仰物理量
                            float q = (rx!=0.0f)? atanf(ry/rx)*(180.0f/PI) : (ry>=0?90.0f:-90.0f);
                            if(rx>=0.0f) q+=180.0f;                            // 复刻 ToRotator 的 Yaw 象限
                            Hh = q;
                        }
                        if(g_yaw_flip) Hh+=180.0f;                            // 保险丝n
                        if(g_swap_pv){ float t=P; P=Hh; Hh=t; }               // 保险丝x
                        if(g_yaw_invert) Hh=-Hh;
                        P+=g_pitch_offset; Hh+=g_yaw_offset;
                        fp=P; fy=Hh; inject=true;
                    }
                }
            }
        }
        for(auto&inf:infos){fill_inject(inf,fp,fy,inject);drv.hwbp_enable(&inf);}
        if(found!=last_found){ last_found=found; if(found) printf("\n\033[1;32m[+] 锁定 %.1fm\033[0m\n",minDist); else printf("\n\033[1;33m[-] 丢失\033[0m\n"); }
        if(++tick%5==0){
            if(found) printf("\r\033[1;32m[追踪] %.1fm 俯%.1f 水%.1f 坠%.0f x%d n%d   \033[0m",minDist,fp,fy,g_drop,g_swap_pv?1:0,g_yaw_flip?1:0);
            else if(cam_ok) printf("\r\033[1;33m[无目标]   \033[0m");
            else printf("\r\033[1;31m[无相机]   \033[0m");
            fflush(stdout);
        }
        usleep(10000);
    }
    cleanup(drv,infos); return 0;
}
