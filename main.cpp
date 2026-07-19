#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>

// 魔数：极端角度，一旦注入生效，子弹必明显飞歪；123 这种怪值便于在 hit 里一眼认出
static const float MAGIC_PITCH = 85.0f;
static const float MAGIC_YAW   = 123.0f;

static std::vector<pid_t> enum_tids(pid_t pid){
    std::vector<pid_t> v; char p[64]; snprintf(p,sizeof(p),"/proc/%d/task",pid);
    DIR* d=opendir(p); if(!d) return v; struct dirent* e;
    while((e=readdir(d))) if(e->d_name[0]!='.') { int t=atoi(e->d_name); if(t>0) v.push_back(t); }
    closedir(d); return v;
}

// 把 float 塞进 V 寄存器 128bit 的低 32 位
static void pack_v(uint64_t out[2], float f){
    out[0]=0; out[1]=0; memcpy(&out[0], &f, 4);
}

// 配置"注入魔数到 V3/V4"的 enable 信息
static void fill_inject(HW_BP_INFO& info, pid_t tid, uintptr_t addr){
    memset(&info,0,sizeof(info));
    info.pid=tid; info.addr=addr; info.type=HW_BP_TYPE_X; info.len=4;
    info.is_write_gp_regs=false;
    info.is_write_fp_regs=true;  info.fp_reg_count=2;
    info.fp_reg_indices[0]=3;    info.fp_reg_indices[1]=4;   // V3=Pitch, V4=Yaw（你原假设）
    pack_v(info.fp_reg_values[0], MAGIC_PITCH);
    pack_v(info.fp_reg_values[1], MAGIC_YAW);
}

int main(){
    system("setenforce 0");
    printf("==== 魔数注入实验：用子弹飞行方向当显示器 ====\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if(pid<=0) pid = drv.get_pid("com.rekoo.pubgm");
    if(pid<=0) pid = drv.get_pid("com.vng.pubgmobile");
    if(pid<=0){printf("[-] 未找到游戏\n");return 1;}
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if(!base){printf("[-] 基址失败\n");return 1;}
    uintptr_t addr = base + 0x6DFE100;
    printf("[+] pid=%d base=0x%lx ShootBulletInner=0x%lx\n", pid, base, addr);

    // —— 硬门槛①：地址有效性（读 4 条指令）——
    uint32_t insn[4]={0};
    bool rd = drv.read_fast(addr, insn, 16);
    printf("[地址自检] 指令: %08x %08x %08x %08x  (read=%s)\n",
           insn[0],insn[1],insn[2],insn[3], rd?"OK":"FAIL");
    bool invalid = !rd || (insn[0]==0||insn[0]==0xFFFFFFFFu ||
                 (insn[0]==insn[1]&&insn[1]==insn[2]&&insn[2]==insn[3]));
    if(invalid){
        printf("\033[1;31m[!!] 地址无效！0x6DFE100 落在未映射/空区。断点机制再对也没用。\033[0m\n");
        printf("     请立刻去 IDA 跳到 libUE4.so:0x6DFE100 肉眼确认是不是 ShootBulletInner 首条指令。\n");
        printf("     在地址修对之前，下面所有结果都无意义。\n");
    } else {
        printf("[地址自检] 内容多样，地址大概率有效（最终请以 IDA 为准）。\n");
    }

    // —— 枚举线程，每个都装"魔数注入"断点 ——
    auto tids = enum_tids(pid);
    printf("[+] 线程数=%zu\n", tids.size());
    std::vector<HW_BP_INFO> infos; int add_ok=0, en_ok=0;
    for(pid_t t : tids){
        HW_BP_INFO info; fill_inject(info, t, addr);
        if(drv.hwbp_add(&info)){ add_ok++; infos.push_back(info); }
    }
    for(auto& inf : infos) if(drv.hwbp_enable(&inf)) en_ok++;
    printf("[+] add=%d/%zu  enable=%d\n", add_ok, tids.size(), en_ok);
    if(add_ok==0){printf("\033[1;31m[-] 一个都没装上，退出。\033[0m\n");return 1;}

    printf("\033[1;33m[!] 现在进训练场，朝【平地正前方】单点射 2~3 发。\033[0m\n");
    printf("\033[1;33m[!] 注入魔数 Pitch=%.0f Yaw=%.0f，若生效子弹应明显朝天/朝怪方向飞。\033[0m\n",
           MAGIC_PITCH, MAGIC_YAW);
    printf("----------------------------------------------------\n");

    HWBP_HIT_ITEM hits[16];
    while(true){
        // 尝试读 hit：写了寄存器后，hit 也许就开始记录了（白捡真实 StartRot）
        for(size_t k=0;k<infos.size();k++){
            HWBP_HIT_ARGS a={0}; a.pid=infos[k].pid; a.addr=addr;
            a.out_buf=hits; a.out_len=16; a.real_count=0;
            if(drv.hwbp_get_hits(&a) && a.real_count>0){
                REGS_INFO& R=hits[0].regs_info;
                printf("\n\033[1;32m[!!] 读到 hit! tid=%d PC=0x%lx\033[0m  -> 断点确实触发了！\n",
                       hits[0].task_id, R.pc);
                printf("   X0=0x%lx X1=0x%lx X2=0x%lx\n", R.regs[0],R.regs[1],R.regs[2]);
                // 把 X0/X1 当指针解引用看 rot
                for(int i=0;i<2;i++){
                    if(R.regs[i]>0x10000){
                        float m[4]={0}; drv.read_fast(R.regs[i], m, 16);
                        printf("   *X%d = [%.3f, %.3f, %.3f, %.3f]\n", i, m[0],m[1],m[2],m[3]);
                    }
                }
            }
        }
        // single-shot 重武装（保活 + 让下一发也注入魔数）
        for(auto& inf : infos) drv.hwbp_enable(&inf);
        usleep(50000);
    }
    drv.hwbp_clear();
    return 0;
}
