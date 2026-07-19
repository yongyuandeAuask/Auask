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
#define HEAD_BONE_IDX 6   // 头部在 component-space 骨骼数组的索引；若打偏胸/颈，调这里

// ===== 全局热调参 / 开关 =====
volatile float g_yaw_offset   = 0.0f;
volatile bool  g_yaw_invert   = false;
volatile float g_pitch_offset = 0.0f;
volatile bool  g_team_filter  = true;   // 队友过滤：默认开（带保护，训练场安全）
volatile bool  g_skip_bot     = false;  // 跳人机：默认关（训练场目标就是人机！实战按 g 开）
volatile bool  g_Running      = true;

static void sig_handler(int){
    g_Running = false;
    const char m[] = "\n[!] 收到退出信号，正在清理断点...\n";
    write(STDERR_FILENO, m, sizeof(m) - 1);
}

void* InputThread(void*){
    char buf[64];
    printf("\n\033[1;33m========== 弹道控制台 ==========\033[0m\n");
    printf("  \033[1;32m默认自动追踪最近目标，无需按键。\033[0m\n");
    printf("  b=反转Yaw  l=Yaw+15  r=Yaw-15  u=Pitch+5  d=Pitch-5\n");
    printf("  数字(如 90/-90)=强制Yaw偏移\n");
    printf("  t=队友过滤(当前:%s)  g=跳人机(当前:%s)\n",
           g_team_filter?"开":"关", g_skip_bot?"开":"关");
    printf("  \033[1;31mq=清除断点并结束 (Ctrl+C 同效)\033[0m\n");
    printf("\033[1;33m==================================\033[0m\n");
    while(g_Running){
        if(!fgets(buf,sizeof(buf),stdin)) continue;
        char c=buf[0];
        if(c=='q'||c=='Q'){ g_Running=false; printf("\n\033[1;31m[q] 退出中...\033[0m\n"); break; }
        else if(c=='b'||c=='B'){ g_yaw_invert=!g_yaw_invert; printf("\n\033[1;32m[*] Yaw反转: %s\033[0m\n", g_yaw_invert?"开":"关"); }
        else if(c=='l'||c=='L'){ g_yaw_offset+=15; printf("\n\033[1;32m[*] Yaw偏: %.0f\033[0m\n", g_yaw_offset); }
        else if(c=='r'||c=='R'){ g_yaw_offset-=15; printf("\n\033[1;32m[*] Yaw偏: %.0f\033[0m\n", g_yaw_offset); }
        else if(c=='u'||c=='U'){ g_pitch_offset+=5; printf("\n\033[1;32m[*] Pitch偏: %.0f\033[0m\n", g_pitch_offset); }
        else if(c=='d'||c=='D'){ g_pitch_offset-=5; printf("\n\033[1;32m[*] Pitch偏: %.0f\033[0m\n", g_pitch_offset); }
        else if(c=='t'||c=='T'){ g_team_filter=!g_team_filter; printf("\n\033[1;32m[*] 队友过滤: %s\033[0m\n", g_team_filter?"开":"关"); }
        else if(c=='g'||c=='G'){ g_skip_bot=!g_skip_bot; printf("\n\033[1;32m[*] 跳人机: %s\033[0m\n", g_skip_bot?"开(实战)":"关(训练场)"); }
        else { float v=atof(buf); if(v!=0.0f||buf[0]=='0'){ g_yaw_offset=v; printf("\n\033[1;32m[*] Yaw偏强制: %.0f\033[0m\n", g_yaw_offset); } }
    }
    return NULL;
}

struct FVector{float X,Y,Z;};
struct FRotator{float Pitch,Yaw,Roll;};
struct FTransform{float rot[4];float trans[3];float scale[3];};

void getBone(paradise_driver* d,uintptr_t a,FTransform& o){
    uint8_t b[48]={0}; d->read_fast(a,b,44);
    memcpy(o.rot,b,16); memcpy(o.trans,b+16,12); memcpy(o.scale,b+28,12);
}
void T2M(const FTransform& T,float M[4][4]){
    float x=T.rot[0],y=T.rot[1],z=T.rot[2],w=T.rot[3];
    float x2=x+x,y2=y+y,z2=z+z,xx=x*x2,xy=x*y2,xz=x*z2,yy=y*y2,yz=y*z2,zz=z*z2,wx=w*x2,wy=w*y2,wz=w*z2;
    float sx=T.scale[0],sy=T.scale[1],sz=T.scale[2];
    M[0][0]=(1-(yy+zz))*sx;M[0][1]=(xy-wz)*sy;M[0][2]=(xz+wy)*sz;M[0][3]=0;
    M[1][0]=(xy+wz)*sx;M[1][1]=(1-(xx+zz))*sy;M[1][2]=(yz-wx)*sz;M[1][3]=0;
    M[2][0]=(xz-wy)*sx;M[2][1]=(yz+wx)*sy;M[2][2]=(1-(xx+yy))*sz;M[2][3]=0;
    M[3][0]=T.trans[0];M[3][1]=T.trans[1];M[3][2]=T.trans[2];M[3][3]=1;
}
void MM(const float A[4][4],const float B[4][4],float C[4][4]){
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)
        C[i][j]=A[i][0]*B[0][j]+A[i][1]*B[1][j]+A[i][2]*B[2][j]+A[i][3]*B[3][j];
}

static std::vector<pid_t> enum_tids(pid_t pid){
    std::vector<pid_t> v; char p[64]; snprintf(p,sizeof(p),"/proc/%d/task",pid);
    DIR* d=opendir(p); if(!d)return v; struct dirent* e;
    while((e=readdir(d))) if(e->d_name[0]!='.'){int t=atoi(e->d_name); if(t>0)v.push_back(t);}
    closedir(d); return v;
}

static void fill_inject(HW_BP_INFO& info,float p,float y,bool inject){
    info.is_write_gp_regs=false;
    info.is_write_fp_regs=inject;
    info.fp_reg_count=2;
    info.fp_reg_indices[0]=3; info.fp_reg_indices[1]=4;   // V3=Pitch V4=Yaw
    memset(info.fp_reg_values[0],0,16); memset(info.fp_reg_values[1],0,16);
    memcpy(&info.fp_reg_values[0][0],&p,4);
    memcpy(&info.fp_reg_values[1][0],&y,4);
}

static void cleanup(paradise_driver& drv, std::vector<HW_BP_INFO>& infos){
    printf("\n\033[1;33m========== 清除断点 ==========\033[0m\n");
    for(auto& inf : infos) drv.hwbp_disable(inf.pid, inf.addr);
    drv.hwbp_clear();
    printf("\033[1;32m[+] 断点已清除，安全结束。\033[0m\n");
}

int main(){
    system("setenforce 0");
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    paradise_driver drv;
    pid_t pid=drv.get_pid("com.tencent.ig");
    if(pid<=0)pid=drv.get_pid("com.rekoo.pubgm");
    if(pid<=0)pid=drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;}
    drv.initialize(pid);

    uintptr_t base=drv.get_module_base("libUE4.so");
    if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t addr=base+0x6DFE100;
    printf("[+] pid=%d ShootBulletInner=0x%lx\n",pid,addr);

    auto tids=enum_tids(pid);
    std::vector<HW_BP_INFO> infos; int add_ok=0;
    for(pid_t t:tids){
        HW_BP_INFO info={0};
        info.pid=t; info.addr=addr; info.type=HW_BP_TYPE_X; info.len=4;
        if(drv.hwbp_add(&info)){ add_ok++; infos.push_back(info); }
    }
    printf("[+] 断点装到 %d/%zu 线程\n",add_ok,tids.size());
    if(add_ok==0){printf("[-] 全失败\n");return 1;}

    pthread_t tid_in;
    pthread_create(&tid_in,NULL,InputThread,NULL);
    pthread_detach(tid_in);

    int tick=0;
    while(g_Running){
        float aim_pitch=0, aim_yaw=0;
        bool found=false, cam_ok=false;
        float minDist=0;
        int bestTeam=0; bool bestBot=false;

        // ---- 自身链（与绘制.cpp 更新地址数据 逐字节一致）----
        uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900);
        uintptr_t pB=drv.read_fast<uintptr_t>(pA+0x810);
        uintptr_t UWorld=drv.read_fast<uintptr_t>(pB+0x78);
        if(UWorld>0x10000){
            uintptr_t pD=drv.read_fast<uintptr_t>(UWorld+0x38);
            uintptr_t pE=drv.read_fast<uintptr_t>(pD+0x78);
            uintptr_t pF=drv.read_fast<uintptr_t>(pE+0x30);
            uintptr_t Oneself=drv.read_fast<uintptr_t>(pF+0x28c8);
            if(Oneself>0x10000){
                int selfTeam=drv.read_fast<int>(Oneself+0x998);   // 自身队伍（队友过滤用）

                // ---- 玩家控制器 -> 相机（0x4b18 / 0x548，绘制.cpp 验证）----
                uintptr_t PC =drv.read_fast<uintptr_t>(Oneself+0x4b18);
                uintptr_t Cam=drv.read_fast<uintptr_t>(PC+0x548);

                // ---- 相机位置：写死 Cam+0x530（=绘制.cpp PovLocation: +0x520+0x10）----
                FVector CamPos={0,0,0};
                if(Cam>0x10000){
                    CamPos=drv.read_fast<FVector>(Cam+0x530);
                    if(CamPos.X!=0||CamPos.Y!=0) cam_ok=true;
                }

                if(cam_ok){
                    // ---- 世界数组（非解密分支，与绘制.cpp 一致）----
                    uintptr_t Uleve=drv.read_fast<uintptr_t>(UWorld+0x30);
                    uintptr_t Arr  =drv.read_fast<uintptr_t>(Uleve+0xA0);
                    int Cnt        =drv.read_fast<int>(Uleve+0xA8);
                    minDist=999999; FVector best={0,0,0};

                    for(int i=0;i<Cnt&&i<100;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i);
                        if(O<0x10000||O==Oneself)continue;

                        // 队友过滤（带保护：自身队伍无效时不跳，防训练场误杀）
                        if(g_team_filter){
                            int team=drv.read_fast<int>(O+0x998);
                            if(selfTeam!=0 && selfTeam!=-1 && team==selfTeam) continue;
                        }
                        // 死亡/倒地过滤（绘制.cpp: 状态 1048592/1048576 跳过）
                        int status=drv.read_fast<int>(O+0x1058);
                        if(status==1048592 || status==1048576) continue;
                        // 血量过滤
                        float hp=drv.read_fast<float>(O+0xe60);
                        if(hp<=0.0f || hp>200.0f) continue;
                        // 人机判断（绘制.cpp: +0xa59 的三个魔数；开关默认关）
                        int team2=drv.read_fast<int>(O+0x998);
                        int botFlag=drv.read_fast<int>(O+0xa59);
                        bool isBot=(team2!=0 && (botFlag==16842753||botFlag==16843009||botFlag==16843008));
                        if(g_skip_bot && isBot) continue;

                        // 骨骼 -> 头部世界坐标（Mesh 偏移与绘制.cpp 一致）
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510);
                        if(Mesh<0x10000)continue;
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30;
                        if(BB<0x10000)continue;
                        FTransform mT,hT;
                        getBone(&drv,Mesh+0x210,mT);
                        getBone(&drv,BB+HEAD_BONE_IDX*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4];
                        T2M(mT,c2w); T2M(hT,bM); MM(bM,c2w,fM);
                        FVector H={fM[3][0],fM[3][1],fM[3][2]};   // 不再 +7，与绘制.cpp 一致

                        float dx=H.X-CamPos.X,dy=H.Y-CamPos.Y,dz=H.Z-CamPos.Z;
                        float dist=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        if(dist<minDist&&dist<300){
                            minDist=dist; best=H; found=true;
                            bestTeam=team2; bestBot=isBot;
                        }
                    }
                    if(found){
                        float dx=best.X-CamPos.X,dy=best.Y-CamPos.Y,dz=best.Z-CamPos.Z;
                        float d2=sqrt(dx*dx+dy*dy); if(d2<0.01f)d2=0.01f;
                        aim_pitch=atan2(dz,d2)*(180.0f/PI);
                        aim_yaw  =atan2(dy,dx)*(180.0f/PI);
                        if(g_yaw_invert)aim_yaw=-aim_yaw;
                        aim_yaw+=g_yaw_offset; aim_pitch+=g_pitch_offset;
                    }
                }
            }
        }

        // 有目标才注入，否则放行（子弹正常飞）
        for(auto& inf: infos){ fill_inject(inf, aim_pitch, aim_yaw, found); drv.hwbp_enable(&inf); }

        if(++tick%5==0){
            if(found)
                printf("\r\033[1;32m[追踪] %.1fm P%.1f Y%.1f %s | 队%d%s 队友%s 人机%s   \033[0m",
                       minDist, aim_pitch, aim_yaw, g_yaw_invert?"反":"",
                       bestTeam, bestBot?"(机)":"",
                       g_team_filter?"开":"关", g_skip_bot?"开":"关");
            else if(cam_ok)
                printf("\r\033[1;33m[无目标] 相机OK 但无合法目标(全死/被过滤) 队过滤=%s 跳人机=%s   \033[0m",
                       g_team_filter?"开":"关", g_skip_bot?"开":"关");
            else
                printf("\r\033[1;31m[无相机] Cam+0x530 读不到坐标，确认人在局内   \033[0m");
            fflush(stdout);
        }
        usleep(10000);
    }

    cleanup(drv, infos);
    return 0;
}
