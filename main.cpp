#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>          // ★ 新增：Ctrl+C 也走清理
#include <math.h>
#include <vector>
#include <string>

#define PI 3.14159265358979323846f
#define MAGIC_PITCH 85.0f
#define MAGIC_YAW   90.0f

// ===== 全局热调参 =====
volatile float g_yaw_offset   = 0.0f;
volatile bool  g_yaw_invert   = false;
volatile float g_pitch_offset = 0.0f;
volatile int   g_mode         = 0;     // 0=瞄准 1=魔数
volatile bool  g_Running      = true;  // ★ 退出总开关：q / Ctrl+C 都置 false

// ★ Ctrl+C 处理：只置标志 + 用 async-signal-safe 的 write 提示，绝不在 handler 里调驱动
static void sig_handler(int){
    g_Running = false;
    const char m[] = "\n[!] 收到退出信号，正在清理断点...\n";
    write(STDERR_FILENO, m, sizeof(m) - 1);
}

void* InputThread(void*){
    char buf[64];
    printf("\n\033[1;33m========== 弹道控制台 ==========\033[0m\n");
    printf("  \033[1;35mm\033[0m = 切换 瞄准/魔数 模式\n");
    printf("  b=反转Yaw  l=Yaw+15  r=Yaw-15  u=Pitch+5  d=Pitch-5\n");
    printf("  数字(如 90/-90/85)=强制Yaw偏移\n");
    printf("  \033[1;31mq\033[0m = 清除所有断点并结束进程  (Ctrl+C 同样有效)\n");
    printf("\033[1;33m==================================\033[0m\n");
    while(g_Running){
        if(!fgets(buf,sizeof(buf),stdin)) continue;
        char c=buf[0];
        // ★ 每条反馈前加 \n，防止被主循环的 \r 状态行覆盖而"看起来没反应"
        if(c=='q'||c=='Q'){
            g_Running=false;
            printf("\n\033[1;31m[q] 收到退出指令，主循环将清理断点并结束。\033[0m\n");
            break;
        }
        else if(c=='m'||c=='M'){g_mode=!g_mode; printf("\n\033[1;32m[*] 模式: %s\033[0m\n",g_mode?"魔数(朝天验证)":"瞄准");}
        else if(c=='b'||c=='B'){g_yaw_invert=!g_yaw_invert; printf("\n\033[1;32m[*] Yaw反转: %s\033[0m\n",g_yaw_invert?"开":"关");}
        else if(c=='l'||c=='L'){g_yaw_offset+=15; printf("\n\033[1;32m[*] Yaw偏: %.0f\033[0m\n",g_yaw_offset);}
        else if(c=='r'||c=='R'){g_yaw_offset-=15; printf("\n\033[1;32m[*] Yaw偏: %.0f\033[0m\n",g_yaw_offset);}
        else if(c=='u'||c=='U'){g_pitch_offset+=5; printf("\n\033[1;32m[*] Pitch偏: %.0f\033[0m\n",g_pitch_offset);}
        else if(c=='d'||c=='D'){g_pitch_offset-=5; printf("\n\033[1;32m[*] Pitch偏: %.0f\033[0m\n",g_pitch_offset);}
        else { float v=atof(buf); if(v!=0.0f||buf[0]=='0'){g_yaw_offset=v; printf("\n\033[1;32m[*] Yaw偏强制: %.0f\033[0m\n",g_yaw_offset);} }
    }
    return NULL;
}

struct FVector{float X,Y,Z;}; struct FRotator{float Pitch,Yaw,Roll;};
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

// ★ 统一清理：逐 tid disable + 全局 clear，双保险，确保内核不留断点
static void cleanup(paradise_driver& drv, std::vector<HW_BP_INFO>& infos){
    printf("\n\033[1;33m========== 正在清除所有断点 ==========\033[0m\n");
    int d_ok=0;
    for(auto& inf : infos){
        if(drv.hwbp_disable(inf.pid, inf.addr)) d_ok++;   // 先逐个摘除
    }
    drv.hwbp_clear();                                      // 再兜底全清
    printf("\033[1;32m[+] 已 disable %zu 个线程断点，hwbp_clear 已执行。\033[0m\n", infos.size());
    printf("\033[1;32m[+] 断点已全部清除，进程安全结束。\033[0m\n");
    printf("\033[1;33m======================================\033[0m\n");
}

int main(){
    system("setenforce 0");
    signal(SIGINT,  sig_handler);   // ★ Ctrl+C -> 置 g_Running=false，走统一清理
    signal(SIGTERM, sig_handler);

    printf("==== 最终版：正确注入 + 干净退出(q / Ctrl+C) ====\n");
    paradise_driver drv;
    pid_t pid=drv.get_pid("com.tencent.ig");
    if(pid<=0)pid=drv.get_pid("com.rekoo.pubgm");
    if(pid<=0)pid=drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;}
    drv.initialize(pid);

    uintptr_t base=drv.get_module_base("libUE4.so");
    if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t addr=base+0x6DFE100;
    printf("[+] pid=%d base=0x%lx ShootBulletInner=0x%lx\n",pid,base,addr);
    uint32_t insn=0; bool rd=drv.read_fast(addr,&insn,4);
    printf("[地址自检] 入口指令=0x%08x (%s)\n",insn,rd?"OK":"FAIL");

    auto tids=enum_tids(pid);
    printf("[+] 线程数=%zu\n",tids.size());

    std::vector<HW_BP_INFO> infos; int add_ok=0;
    for(pid_t t:tids){
        HW_BP_INFO info={0};
        info.pid=t; info.addr=addr; info.type=HW_BP_TYPE_X; info.len=4;
        if(drv.hwbp_add(&info)){ add_ok++; infos.push_back(info); }
    }
    printf("[+] hwbp_add 成功=%d/%zu\n",add_ok,tids.size());
    if(add_ok==0){printf("[-] 全失败\n");return 1;}

    pthread_t tid_in;
    pthread_create(&tid_in,NULL,InputThread,NULL);
    pthread_detach(tid_in);   // ★ 必须 detach：绝不能 join（join 会卡在 fgets 死锁）

    printf("\033[1;33m[!] 默认瞄准。乱飞按 m 切魔数对照。退出按 q 或 Ctrl+C。\033[0m\n");
    printf("----------------------------------------------------\n");

    HWBP_HIT_ITEM hits[4]; int diag_left=3;
    int no_target_tick=0;

    while(g_Running){
        float aim_pitch=0, aim_yaw=0; bool found=false; float minDist=0;
        uintptr_t p1=drv.read_fast<uintptr_t>(base+0xf1fb900);
        uintptr_t p2=drv.read_fast<uintptr_t>(p1+0x810);
        uintptr_t UWorld=drv.read_fast<uintptr_t>(p2+0x78);
        if(UWorld>0x10000){
            uintptr_t o1=drv.read_fast<uintptr_t>(UWorld+0x38);
            uintptr_t o2=drv.read_fast<uintptr_t>(o1+0x78);
            uintptr_t o3=drv.read_fast<uintptr_t>(o2+0x30);
            uintptr_t Oneself=drv.read_fast<uintptr_t>(o3+0x28c8);
            if(Oneself>0x10000){
                uintptr_t PC=drv.read_fast<uintptr_t>(Oneself+0x4b18);
                uintptr_t Cam=drv.read_fast<uintptr_t>(PC+0x548);
                FVector CamPos={0,0,0};
                if(Cam>0x10000) for(int off=0;off<=64;off+=4){
                    FRotator tr=drv.read_fast<FRotator>(Cam+off);
                    if(tr.Pitch>=-90&&tr.Pitch<=90&&tr.Yaw!=0){
                        CamPos=drv.read_fast<FVector>(Cam+off-12);
                        if(CamPos.X==0&&CamPos.Y==0)CamPos=drv.read_fast<FVector>(Cam+off-16);
                        break;
                    }
                }
                if(CamPos.X!=0||CamPos.Y!=0){
                    uintptr_t Lv=drv.read_fast<uintptr_t>(UWorld+0x30);
                    uintptr_t Arr=drv.read_fast<uintptr_t>(Lv+0xA0);
                    int Cnt=drv.read_fast<int>(Lv+0xA8);
                    minDist=999999; FVector best={0,0,0};
                    for(int i=0;i<Cnt&&i<100;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i);
                        if(O<0x10000||O==Oneself)continue;
                        float hp=drv.read_fast<float>(O+0xe60);
                        if(hp<=0||hp>200)continue;
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510);
                        if(Mesh<0x10000)continue;
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30;
                        if(BB<0x10000)continue;
                        FTransform mT,hT; getBone(&drv,Mesh+0x210,mT); getBone(&drv,BB+6*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4]; T2M(mT,c2w); T2M(hT,bM); MM(bM,c2w,fM);
                        FVector H={fM[3][0],fM[3][1],fM[3][2]+7.0f};
                        float dx=H.X-CamPos.X,dy=H.Y-CamPos.Y,dz=H.Z-CamPos.Z;
                        float dist=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        if(dist<minDist&&dist<300){minDist=dist;best=H;found=true;}
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

        float fp, fy;
        if(g_mode==1){ fp=MAGIC_PITCH; fy=MAGIC_YAW; }
        else if(found){ fp=aim_pitch; fy=aim_yaw; }
        else { fp=0; fy=0; }
        bool inject = (g_mode==1) || found;

        for(auto& inf: infos){ fill_inject(inf, fp, fy, inject); drv.hwbp_enable(&inf); }

        if(g_mode==1)
            printf("\r\033[1;35m[魔数] 注入 P=%.0f Y=%.0f (应朝天偏东)   \033[0m",MAGIC_PITCH,MAGIC_YAW);
        else if(found)
            printf("\r\033[1;32m[瞄准] 距%.1fm 注入 P=%.1f Y=%.1f | 偏: 反=%s Y%.0f P%.0f   \033[0m",
                   minDist,aim_pitch,aim_yaw,g_yaw_invert?"T":"F",g_yaw_offset,g_pitch_o
