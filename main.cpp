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
// 头部 component-space 骨骼索引。打脖子/胸口就改这里试 0/5/7。
#define HEAD_BONE_IDX 6

volatile float g_yaw_offset   = 0.0f;
volatile bool  g_yaw_invert   = false;
volatile float g_pitch_offset = 0.0f;
volatile bool  g_team_filter  = true;
volatile bool  g_skip_bot     = false;
volatile float g_drop_k       = 0.01f;  // ★ 下坠抬枪系数(cm/米²)。子弹受重力指头必偏低，靠它抬。o/p 调
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
    printf("  \033[1;35mo=下坠补偿+0.005  p=下坠补偿-0.005\033[0m (当前:%.3f)\n", g_drop_k);
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
        else if(c=='o'||c=='O'){ g_drop_k+=0.005f; printf("\n\033[1;32m[*] 下坠补偿: %.3f (打偏低就按o加大)\033[0m\n", g_drop_k); }
        else if(c=='p'||c=='P'){ g_drop_k-=0.005f; if(g_drop_k<0)g_drop_k=0; printf("\n\033[1;32m[*] 下坠补偿: %.3f (打偏高就按p减小)\033[0m\n", g_drop_k); }
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
    info.fp_reg_indices[0]=3; info.fp_reg_indices[1]=4;
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

        uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900);
        uintptr_t pB=drv.read_fast<uintptr_t>(pA+0x810);
        uintptr_t UWorld=drv.read_fast<uintptr_t>(pB+0x78);
        if(UWorld>0x10000){
            uintptr_t pD=drv.read_fast<uintptr_t>(UWorld+0x38);
            uintptr_t pE=drv.read_fast<uintptr_t>(pD+0x78);
            uintptr_t pF=drv.read_fast<uintptr_t>(pE+0x30);
            uintptr_t Oneself=drv.read_fast<uintptr_t>(pF+0x28c8);
            if(Oneself>0x10000){
                int selfTeam=drv.read_fast<int>(Oneself+0x998);

                uintptr_t PC =drv.read_fast<uintptr_t>(Oneself+0x4b18);
                uintptr_t Cam=drv.read_fast<uintptr_t>(PC+0x548);

                FVector CamPos={0,0,0};
                if(Cam>0x10000){
                    CamPos=drv.read_fast<FVector>(Cam+0x530);
                    if(CamPos.X!=0||CamPos.Y!=0) cam_ok=true;
                }

                if(cam_ok){
                    uintptr_t Uleve=drv.read_fast<uintptr_t>(UWorld+0x30);
                    uintptr_t Arr  =drv.read_fast<uintptr_t>(Uleve+0xA0);
                    int Cnt        =drv.read_fast<int>(Uleve+0xA8);

                    minDist=999999; FVector best={0,0,0};
                    int loopN = Cnt; if(loopN<0) loopN=0; if(loopN>512) loopN=512;
                    int c_see=0, c_fp=0, c_team=0, c_dead=0, c_mesh=0, c_ok=0;
                    for(int i=0;i<loopN;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i);
                        if(O<0x10000||O==Oneself)continue;
                        c_see++;
                        float fp=drv.read_fast<float>(O+0x2b78);
                        if(fp!=479.5f) continue;
                        c_fp++;
                        int team=drv.read_fast<int>(O+0x998);
                        if(g_team_filter && selfTeam!=0 && selfTeam!=-1 && team==selfTeam){ c_team++; continue; }
                        int status=drv.read_fast<int>(O+0x1058);
                        if(status==1048592 || status==1048576){ c_dead++; continue; }
                        int botFlag=drv.read_fast<int>(O+0xa59);
                        bool isBot=(team!=0 && (botFlag==16842753||botFlag==16843009||botFlag==16843008));
                        if(g_skip_bot && isBot) continue;
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510);
                        if(Mesh<0x10000){ c_mesh++; continue; }
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30;
                        if(BB<0x10000){ c_mesh++; continue; }
                        FTransform mT,hT;
                        getBone(&drv,Mesh+0x210,mT);
                        getBone(&drv,BB+HEAD_BONE_IDX*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4];
                        T2M(mT,c2w); T2M(hT,bM); MM(bM,c2w,fM);
                        FVector H={fM[3][0],fM[3][1],fM[3][2]};
                        float dx=H.X-CamPos.X,dy=H.Y-CamPos.Y,dz=H.Z-CamPos.Z;
                        float dist=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        c_ok++;
                        if(dist<minDist&&dist<300){ minDist=dist; best=H; found=true; bestTeam=team; bestBot=isBot; }
                    }
                    static int tick_diag=0;
                    if(++tick_diag>=50){
                        tick_diag=0;
                        printf("\n\033[1;36m[漏斗] 遍历%d 见%d 指纹%d 队杀%d 死杀%d 网格杀%d 合格%d -> found=%s\033[0m\n",
                               loopN, c_see, c_fp, c_team, c_dead, c_mesh, c_ok, found?"Y":"N");
                    }
                    if(found){
                        // ★ 下坠抬枪：子弹受重力，直线指头必偏低，按距离平方把瞄准点往上抬
                        best.Z += g_drop_k * minDist * minDist;
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
                printf("\r\033[1;32m[追踪] %.1fm P%.1f Y%.1f %s drop%.3f | 队%d%s   \033[0m",
                       minDist, aim_pitch, aim_yaw, g_yaw_invert?"反":"", g_drop_k,
                       bestTeam, bestBot?"(机)":"");
            else if(cam_ok)
                printf("\r\033[1;33m[无目标] 相机OK 但无合法目标(看上方漏斗)   \033[0m");
            else
                printf("\r\033[1;31m[无相机] Cam+0x530 读不到坐标，确认人在局内   \033[0m");
            fflush(stdout);
        }
        usleep(10000);
    }

    cleanup(drv, infos);
    return 0;
}
