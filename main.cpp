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
#define HEAD_BONE_IDX 6   // 打脖子/胸口改这里试 0/5/7
volatile float g_yaw_offset=0, g_pitch_offset=0, g_drop=540.0f, g_bullet_speed=0.0f, g_fov_deg=25.0f;
volatile bool g_yaw_invert=false, g_team_filter=true, g_skip_bot=false, g_Running=true;
volatile bool g_capture=false, g_inject_q=false, g_verbose=false, g_swap_pv=false;
static void sig_handler(int){ g_Running=false; const char m[]="\n[!] 退出清理中...\n"; write(STDERR_FILENO,m,sizeof(m)-1); }
void* InputThread(void*){
    char buf[64];
    printf("\n\033[1;33m===== 弹道控制台(默认静默) =====\033[0m\n");
    printf(" b=反转Yaw l/r=Yaw±15 u/d=Pitch±5 数字=强制Yaw偏\n");
    printf(" o/p=下坠常数±20 t=队友过滤 g=跳人机\n");
    printf(" c=采样对照 v=注入源:%s s=实时行:%s x=交换P/Y:%s\n", g_inject_q?"准星Q":"aim", g_verbose?"开":"关", g_swap_pv?"开":"关");
    printf(" \033[1;35mf=FOV+5 h=FOV-5 (当前:%.0f度 | 瞄谁打谁,没瞄不打)\033[0m\n", g_fov_deg);
    printf(" \033[1;31mq=清除断点退出(Ctrl+C同效)\033[0m\n\033[1;33m======================\033[0m\n");
    while(g_Running){
        if(!fgets(buf,sizeof(buf),stdin)) continue; char c=buf[0];
        if(c=='q'||c=='Q'){g_Running=false;printf("\n[q] 退出\n");break;}
        else if(c=='b'||c=='B'){g_yaw_invert=!g_yaw_invert;printf("\n[*] 反转:%s\n",g_yaw_invert?"开":"关");}
        else if(c=='l'||c=='L'){g_yaw_offset+=15;printf("\n[*] Y%.0f\n",g_yaw_offset);}
        else if(c=='r'||c=='R'){g_yaw_offset-=15;printf("\n[*] Y%.0f\n",g_yaw_offset);}
        else if(c=='u'||c=='U'){g_pitch_offset+=5;printf("\n[*] P%.0f\n",g_pitch_offset);}
        else if(c=='d'||c=='D'){g_pitch_offset-=5;printf("\n[*] P%.0f\n",g_pitch_offset);}
        else if(c=='o'||c=='O'){g_drop+=20.0f;printf("\n[*] 下坠%.0f 速%.0f (偏低按o)\n",g_drop,g_bullet_speed);}
        else if(c=='p'||c=='P'){g_drop-=20.0f;if(g_drop<0)g_drop=0;printf("\n[*] 下坠%.0f 速%.0f (偏高按p)\n",g_drop,g_bullet_speed);}
        else if(c=='t'||c=='T'){g_team_filter=!g_team_filter;printf("\n[*] 队友:%s\n",g_team_filter?"开":"关");}
        else if(c=='g'||c=='G'){g_skip_bot=!g_skip_bot;printf("\n[*] 跳人机:%s\n",g_skip_bot?"开":"关");}
        else if(c=='c'||c=='C'){g_capture=true;printf("\n\033[1;35m[c] 下一帧采样...\033[0m\n");}
        else if(c=='v'||c=='V'){g_inject_q=!g_inject_q;printf("\n\033[1;35m[v] 注入源:%s\033[0m\n",g_inject_q?"准星Q":"aim");}
        else if(c=='s'||c=='S'){g_verbose=!g_verbose;printf("\n\033[1;35m[s] 实时行:%s\033[0m\n",g_verbose?"开":"关");}
        else if(c=='x'||c=='X'){g_swap_pv=!g_swap_pv;printf("\n\033[1;35m[x] 交换P/Y:%s\033[0m\n",g_swap_pv?"开":"关");}
        else if(c=='f'||c=='F'){g_fov_deg+=5;printf("\n\033[1;35m[*] FOV%.0f度 (锁太窄/瞄偏一点就不锁,按f加宽)\033[0m\n",g_fov_deg);}
        else if(c=='h'||c=='H'){g_fov_deg-=5;if(g_fov_deg<5)g_fov_deg=5;printf("\n\033[1;35m[*] FOV%.0f度 (锁太宽/还锁旁边的,按h收窄)\033[0m\n",g_fov_deg);}
        else {float v=atof(buf);if(v!=0||buf[0]=='0'){g_yaw_offset=v;printf("\n[*] Y强制%.0f\n",g_yaw_offset);}}
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
static void do_capture(paradise_driver&drv,uintptr_t base,float aim_p,float aim_y,bool found,std::vector<HW_BP_INFO>&infos,uintptr_t addr){
    uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900),pB=drv.read_fast<uintptr_t>(pA+0x810),UW=drv.read_fast<uintptr_t>(pB+0x78);
    uintptr_t o1=drv.read_fast<uintptr_t>(UW+0x38),o2=drv.read_fast<uintptr_t>(o1+0x78),o3=drv.read_fast<uintptr_t>(o2+0x30),Me=drv.read_fast<uintptr_t>(o3+0x28c8);
    uintptr_t PC=drv.read_fast<uintptr_t>(Me+0x4b18),Cam=drv.read_fast<uintptr_t>(PC+0x548);
    FRotator Q=drv.read_fast<FRotator>(Cam+0x554);
    float dP=aim_p-Q.Pitch, dY=fmodf(aim_y-Q.Yaw+540.f,360.f)-180.f;
    printf("\n\033[1;36m==== 采样对照 (found=%s) ====\033[0m\n",found?"Y":"N");
    printf("  准星Q : P=%8.3f Y=%8.3f  <- 瞄靶=命中金标准\n",Q.Pitch,Q.Yaw);
    printf("  我aim : P=%8.3f Y=%8.3f\n",aim_p,aim_y);
    printf("  \033[1;33m差 dP=%+7.3f dY=%+7.3f\033[0m  <- 恒定=补offset; 随朝向变=约定错\n",dP,dY);
    HWBP_HIT_ITEM hits[4];
    for(auto&inf:infos){
        HWBP_HIT_ARGS a={0};a.pid=inf.pid;a.addr=addr;a.out_buf=hits;a.out_len=4;a.real_count=0;
        if(drv.hwbp_get_hits(&a)&&a.real_count>0){
            REGS_INFO&R=hits[0].regs_info;
            printf("  \033[1;32m[hit!] X0=0x%lx X1=0x%lx\033[0m\n",R.regs[0],R.regs[1]);
            for(int i=0;i<2;i++)if(R.regs[i]>0x10000){float m[4]={0};drv.read_fast(R.regs[i],m,16);printf("     *X%d=[%.2f,%.2f,%.2f,%.2f]\n",i,m[0],m[1],m[2],m[3]);}
            break;
        }
    }
    printf("\033[1;36m============================\033[0m\n");
}
int main(){
    system("setenforce 0"); signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler);
    paradise_driver drv;
    pid_t pid=drv.get_pid("com.tencent.ig"); if(pid<=0)pid=drv.get_pid("com.rekoo.pubgm"); if(pid<=0)pid=drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;} drv.initialize(pid);
    uintptr_t base=drv.get_module_base("libUE4.so"); if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t addr=base+0x6DFE100; printf("[+] pid=%d addr=0x%lx\n",pid,addr);
    auto tids=enum_tids(pid); std::vector<HW_BP_INFO>infos; int add_ok=0;
    for(pid_t t:tids){HW_BP_INFO i={0};i.pid=t;i.addr=addr;i.type=HW_BP_TYPE_X;i.len=4;if(drv.hwbp_add(&i)){add_ok++;infos.push_back(i);}}
    printf("[+] 断点 %d/%zu\n",add_ok,tids.size()); if(add_ok==0){printf("[-] 全失败\n");return 1;}
    pthread_t tid_in; pthread_create(&tid_in,NULL,InputThread,NULL); pthread_detach(tid_in);
    int tick=0; static bool last_found=false; static bool speed_warned=false;
    while(g_Running){
        float aim_pitch=0,aim_yaw=0; bool found=false,cam_ok=false; float minDist=0; int bestTeam=0; bool bestBot=false;
        FRotator Q_snap={0,0,0}; bool q_valid=false;
        uintptr_t pA=drv.read_fast<uintptr_t>(base+0xf1fb900),pB=drv.read_fast<uintptr_t>(pA+0x810),UWorld=drv.read_fast<uintptr_t>(pB+0x78);
        if(UWorld>0x10000){
            uintptr_t pD=drv.read_fast<uintptr_t>(UWorld+0x38),pE=drv.read_fast<uintptr_t>(pD+0x78),pF=drv.read_fast<uintptr_t>(pE+0x30),Oneself=drv.read_fast<uintptr_t>(pF+0x28c8);
            if(Oneself>0x10000){
                int selfTeam=drv.read_fast<int>(Oneself+0x998);
                uintptr_t _w0=drv.read_fast<uintptr_t>(Oneself+0x2608);
                uintptr_t _w1=drv.read_fast<uintptr_t>(_w0+0x5d8);
                uintptr_t _w2=drv.read_fast<uintptr_t>(_w1+0x1370);
                g_bullet_speed=drv.read_fast<float>(_w2+0x560);
                uintptr_t PC=drv.read_fast<uintptr_t>(Oneself+0x4b18),Cam=drv.read_fast<uintptr_t>(PC+0x548);
                FVector CamPos={0,0,0};
                if(Cam>0x10000){ CamPos=drv.read_fast<FVector>(Cam+0x530); if(CamPos.X!=0||CamPos.Y!=0){cam_ok=true; Q_snap=drv.read_fast<FRotator>(Cam+0x554); q_valid=true;} }
                if(cam_ok){
                    uintptr_t Uleve=drv.read_fast<uintptr_t>(UWorld+0x30),Arr=drv.read_fast<uintptr_t>(Uleve+0xA0); int Cnt=drv.read_fast<int>(Uleve+0xA8);
                    minDist=999999; float minAng=999999.0f; FVector best={0,0,0}; int loopN=Cnt; if(loopN<0)loopN=0; if(loopN>512)loopN=512;
                    int c_see=0,c_fp=0,c_team=0,c_dead=0,c_mesh=0,c_ok=0,c_fov=0;
                    for(int i=0;i<loopN;i++){
                        uintptr_t O=drv.read_fast<uintptr_t>(Arr+8*i); if(O<0x10000||O==Oneself)continue;
                        c_see++;
                        if(drv.read_fast<float>(O+0x2b78)!=479.5f)continue;
                        c_fp++;
                        int team=drv.read_fast<int>(O+0x998);
                        if(g_team_filter&&selfTeam!=0&&selfTeam!=-1&&team==selfTeam){c_team++;continue;}
                        int st=drv.read_fast<int>(O+0x1058); if(st==1048592||st==1048576){c_dead++;continue;}
                        int bf=drv.read_fast<int>(O+0xa59); bool isBot=(team!=0&&(bf==16842753||bf==16843009||bf==16843008));
                        if(g_skip_bot&&isBot)continue;
                        uintptr_t Mesh=drv.read_fast<uintptr_t>(O+0x510); if(Mesh<0x10000){c_mesh++;continue;}
                        uintptr_t BB=drv.read_fast<uintptr_t>(Mesh+0x9a8)+0x30; if(BB<0x10000){c_mesh++;continue;}
                        FTransform mT,hT; getBone(&drv,Mesh+0x210,mT); getBone(&drv,BB+HEAD_BONE_IDX*48,hT);
                        float c2w[4][4],bM[4][4],fM[4][4]; T2M(mT,c2w);T2M(hT,bM);MM(bM,c2w,fM);
                        FVector H={fM[3][0],fM[3][1],fM[3][2]};
                        float dx=H.X-CamPos.X,dy=H.Y-CamPos.Y,dz=H.Z-CamPos.Z,dist=sqrt(dx*dx+dy*dy+dz*dz)*0.01f;
                        c_ok++;
                        if(dist<300){
                            float d2=sqrt(dx*dx+dy*dy); if(d2<0.01f)d2=0.01f;
                            float ty=atan2(dy,dx)*(180.f/PI), tp=atan2(dz,d2)*(180.f/PI);
                            if(g_yaw_invert) ty=-ty;
                            ty+=g_yaw_offset; tp+=g_pitch_offset;
                            float dyaw=fmodf(ty-Q_snap.Yaw+540.f,360.f)-180.f, dpitch=tp-Q_snap.Pitch;
                            float ang=dyaw*dyaw+dpitch*dpitch, fov2=g_fov_deg*g_fov_deg;
                            if(ang>=fov2){c_fov++;}
                            else if(ang<minAng){minAng=ang;minDist=dist;best=H;found=true;bestTeam=team;bestBot=isBot;}
                        }
                    }
                    static int tick_diag=0;
                    if(++tick_diag>=50){tick_diag=0;printf("\n\033[1;36m[漏斗] 遍历%d 见%d 指纹%d 队杀%d 死杀%d 网格杀%d 过过滤%d FOV杀%d -> found=%s\033[0m\n",loopN,c_see,c_fp,c_team,c_dead,c_mesh,c_ok,c_fov,found?"Y":"N");}
                    if(found){
                        if(g_bullet_speed>10.0f){ float _t=minDist/g_bullet_speed; best.Z+=g_drop*_t*_t; }
                        else if(!speed_warned){ speed_warned=true; printf("\n\033[1;31m[!] 子弹速度无效(%.1f)，下坠=0\033[0m\n",g_bullet_speed); }
                        float dx=best.X-CamPos.X,dy=best.Y-CamPos.Y,dz=best.Z-CamPos.Z,d2=sqrt(dx*dx+dy*dy); if(d2<0.01f)d2=0.01f;
                        aim_pitch=atan2(dz,d2)*(180.f/PI); aim_yaw=atan2(dy,dx)*(180.f/PI);
                        if(g_yaw_invert)aim_yaw=-aim_yaw; aim_yaw+=g_yaw_offset; aim_pitch+=g_pitch_offset;
                    }
                }
            }
        }
        float fp,fy; bool inject;
        if(g_inject_q&&q_valid){ fp=Q_snap.Pitch; fy=Q_snap.Yaw; inject=true; }
        else if(found){ fp=aim_pitch; fy=aim_yaw; inject=true; }
        else { fp=0; fy=0; inject=false; }
        if(g_swap_pv){ float tmp=fp; fp=fy; fy=tmp; }
        if(g_capture){ do_capture(drv,base,aim_pitch,aim_yaw,found,infos,addr); g_capture=false; }
        for(auto&inf:infos){fill_inject(inf,fp,fy,inject);drv.hwbp_enable(&inf);}
        if(found!=last_found){ last_found=found; if(found) printf("\n\033[1;32m[+] 已锁定 %.1fm\033[0m\n",minDist); else printf("\n\033[1;33m[-] 目标丢失\033[0m\n"); }
        if(g_verbose&&++tick%10==0){
            if(found) printf("\r\033[1;32m[追踪] %.1fm P%.1f Y%.1f 坠%.0f 速%.0f FOV%.0f   \033[0m",minDist,aim_pitch,aim_yaw,g_drop,g_bullet_speed,g_fov_deg);
            else if(cam_ok) printf("\r\033[1;33m[无目标]   \033[0m");
            else printf("\r\033[1;31m[无相机]   \033[0m");
            fflush(stdout);
        }
        usleep(10000);
    }
    cleanup(drv,infos); return 0;
}
