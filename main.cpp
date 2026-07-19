#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>
#include <vector>
#include <string>
#include <utility>

static std::vector<std::pair<std::string,float>> g_cand;
static bool is_angle(float v, uint32_t raw){
    if(raw==0u||raw==0x80000000u) return false;
    return v>=-180.0f && v<=180.0f;
}
static void note(const char* t,float v,uint32_t r){ if(is_angle(v,r)) g_cand.push_back({t,v}); }

static void dump_floats(const char* pre,int base,const uint8_t* b,int n){
    for(int i=0;i<n;i++){
        uint32_t raw=*(uint32_t*)&b[i*4]; float v=*(float*)&raw;
        char t[64]; snprintf(t,sizeof(t),"%s+%d",pre,base+i*4);
        bool star=is_angle(v,raw)&&fabsf(v)>0.5f;
        if(star) printf("    \033[1;32m★ %-9s=%9.4f\033[0m (0x%08x)\n",t,v,raw);
        else if(is_angle(v,raw)) printf("    \033[0;32m  %-9s=%9.4f\033[0m (0x%08x)\n",t,v,raw);
        else printf("      %-9s=%9.4f (0x%08x)\n",t,v,raw);
        note(t,v,raw);
    }
}
static void dump_deref(paradise_driver& d,int i,uint64_t p){
    if(p<0x10000){printf("    X%-2d 解引用: 跳过(非指针)\n",i);return;}
    uint8_t m[16]={0};
    if(!d.read_fast(p,m,16)){printf("    X%-2d 解引用: 无效 0x%lx\n",i,p);return;}
    char t[16]; snprintf(t,sizeof(t),"*X%d",i);
    printf("    \033[1;36mX%-2d->0x%lx 解引用:\033[0m\n",i,p); dump_floats(t,0,m,4);
}

// 枚举 /proc/<pid>/task 下所有线程 tid
static std::vector<pid_t> enum_tids(pid_t pid){
    std::vector<pid_t> v; char path[64];
    snprintf(path,sizeof(path),"/proc/%d/task",pid);
    DIR* d=opendir(path); if(!d) return v;
    struct dirent* e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.') continue;
        int t=atoi(e->d_name); if(t>0) v.push_back((pid_t)t);
    }
    closedir(d); return v;
}

// 空修改 = 纯探测，不改任何寄存器
static bool arm(paradise_driver& d, HW_BP_INFO& info){
    info.is_write_gp_regs=false; info.is_write_fp_regs=false;
    return d.hwbp_enable(&info);
}

int main(){
    system("setenforce 0");
    printf("====================================================\n");
    printf("  ShootBulletInner 探测 v3 (per-thread 修正版)\n");
    printf("====================================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if(pid<=0) pid = drv.get_pid("com.rekoo.pubgm");
    if(pid<=0) pid = drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;}
    drv.initialize(pid);
    printf("[+] 进程 pid = %d\n", pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t shoot_addr = base + 0x6DFE100;
    printf("[+] libUE4 base = 0x%lx\n", base);
    printf("[+] ShootBulletInner = 0x%lx\n", shoot_addr);

    // —— 自检1：地址活性，读出入口 4 字节指令 ——
    uint32_t insn=0;
    bool rd = drv.read_fast(shoot_addr, &insn, 4);
    printf("[自检] 入口指令字节 = 0x%08x  (read_fast=%s)\n", insn, rd?"OK":"FAIL");
    if(!rd || insn==0u || insn==0xFFFFFFFFu){
        printf("\033[1;31m[!!] 地址读不出有效指令！偏移 0x6DFE100 或基址可能错了。\033[0m\n");
        printf("     这种情况断点永远不会触发，先核对 IDA 偏移。\n");
    }

    // —— 枚举线程，逐个装断点 ——
    std::vector<pid_t> tids = enum_tids(pid);
    printf("[+] 枚举到线程数 = %zu\n", tids.size());
    if(tids.empty()){printf("[-] 无法枚举线程\n");return 1;}

    std::vector<HW_BP_INFO> infos;
    int add_ok=0;
    for(pid_t t : tids){
        HW_BP_INFO info={0};
        info.pid  = t;                 // ★ 关键：填 tid，不是进程 pid
        info.addr = shoot_addr;
        info.type = HW_BP_TYPE_X;
        info.len  = 4;
        if(drv.hwbp_add(&info)){ add_ok++; infos.push_back(info); }
    }
    printf("[+] hwbp_add 成功 = %d / %zu  (每个线程各装一个)\n", add_ok, tids.size());
    if(add_ok==0){printf("\033[1;31m[-] 一个都没装上！驱动可能用 addr 去重或权限不足。\033[0m\n");return 1;}

    // 首次武装全部
    int en_ok=0; for(auto& inf: infos) if(arm(drv,inf)) en_ok++;
    printf("[+] 首次 hwbp_enable 成功 = %d\n", en_ok);
    printf("\033[1;33m[!] 进训练场，朝【斜上/斜侧】单点射 2~3 发，间隔>1秒。\033[0m\n");
    printf("----------------------------------------------------\n");

    HWBP_HIT_ITEM hits[16];
    uint8_t vregs[32][16]; uint32_t fpsr,fpcr;
    int total=0;

    while(true){
        bool any=false;
        for(size_t k=0;k<infos.size();k++){          // 按 tid 轮询 hit 队列
            HWBP_HIT_ARGS args={0};
            args.pid = infos[k].pid;                 // ★ get_hits 也用 tid
            args.addr = shoot_addr;
            args.out_buf = hits; args.out_len = 16; args.real_count = 0;
            if(!drv.hwbp_get_hits(&args) || args.real_count<=0) continue;
            any=true;

            for(int h=0;h<args.real_count;h++){
                total++; g_cand.clear();
                REGS_INFO& R = hits[h].regs_info;
                printf("\n\033[1;33m##### 命中#%d tid=%d PC=0x%lx SP=0x%lx #####\033[0m\n",
                       total, hits[h].task_id, R.pc, R.sp);

                printf("\033[1;36m[GPR] X0-X7:\033[0m\n");
                for(int i=0;i<8;i++){
                    uint32_t lo=(uint32_t)(R.regs[i]&0xFFFFFFFF); float fv=*(float*)&lo;
                    printf("    X%-2d=0x%016lx int=%-11d float=%9.4f\n",i,R.regs[i],(int32_t)lo,fv);
                    char t[16]; snprintf(t,sizeof(t),"X%d.lo",i); note(t,fv,lo);
                }
                dump_deref(drv,0,R.regs[0]);
                dump_deref(drv,1,R.regs[1]);

                printf("\033[1;34m[STACK] SP..SP+96:\033[0m\n");
                uint8_t stk[96]={0};
                if(drv.read_fast(R.sp,stk,96)) dump_floats("SP",0,stk,24);

                printf("\033[0;31m[FP] V0-V7 (⚠ 可能为实时值，需多发射击对比):\033[0m\n");
                if(drv.fpr_read(hits[h].task_id,0xFF,vregs,&fpsr,&fpcr)){
                    for(int i=0;i<8;i++){
                        uint32_t raw=*(uint32_t*)vregs[i]; float v=*(float*)&raw;
                        bool star=is_angle(v,raw)&&fabsf(v)>0.5f;
                        if(star) printf("    \033[1;32m★ V%-2d=%9.4f\033[0m (0x%08x)\n",i,v,raw);
                        else     printf("      V%-2d=%9.4f (0x%08x)\n",i,v,raw);
                        char t[8]; snprintf(t,sizeof(t),"V%d",i); note(t,v,raw);
                    }
                }
                printf("\033[1;35m[候选清单]:\033[0m\n");
                for(auto& kv:g_cand) printf("    %-12s=%9.4f %s\n",kv.first.c_str(),kv.second,fabsf(kv.second)>0.5f?"★":"");
                printf("\033[1;33m##################################################\033[0m\n");
            }
        }
        if(any){
            usleep(800000);                          // 命中冷却，避免一梭子刷屏
            for(auto& inf: infos) arm(drv,inf);      // 重新武装所有线程(single-shot)
        }else{
            usleep(5000);
            static int idle=0; if(++idle>200){idle=0; for(auto& inf: infos) arm(drv,inf);} // 保活重武装
        }
    }
    drv.hwbp_clear();
    return 0;
}
