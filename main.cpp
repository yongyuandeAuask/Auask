#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <vector>
#include <string>
#include <utility>

// 候选清单：自动收集所有"长得像角度"的 float，附带来源标签
static std::vector<std::pair<std::string,float>> g_cand;

static bool is_angle_like(float v, uint32_t raw) {
    if (raw == 0x00000000u || raw == 0x80000000u) return false; // 排除 +0 / -0
    return (v >= -180.0f && v <= 180.0f);
}
static void note(const char* tag, float v, uint32_t raw) {
    if (is_angle_like(v, raw)) g_cand.push_back({tag, v});
}

// 把一段内存按 float 打印，base_off 用于显示偏移，tag_prefix 用于候选标签
static void dump_floats(const char* tag_prefix, int base_off,
                        const uint8_t* buf, int n_floats) {
    for (int i = 0; i < n_floats; i++) {
        uint32_t raw = *(uint32_t*)&buf[i*4];
        float v = *(float*)&raw;
        char tag[64]; snprintf(tag, sizeof(tag), "%s+%d", tag_prefix, base_off + i*4);
        bool star = is_angle_like(v, raw) && fabsf(v) > 0.5f; // 非零角度=强候选
        if (star)      printf("    \033[1;32m★ %-10s = %9.4f\033[0m  (0x%08x)\n", tag, v, raw);
        else if (is_angle_like(v, raw))
                       printf("    \033[0;32m  %-10s = %9.4f\033[0m  (0x%08x)\n", tag, v, raw);
        else           printf("      %-10s = %9.4f  (0x%08x)\n", tag, v, raw);
        note(tag, v, raw);
    }
}

// 尝试把 reg 值当指针，解引用 16 字节并按 float 打印（探测 const ref 传递）
static void dump_deref(paradise_driver& drv, int reg_idx, uint64_t ptr) {
    if (ptr < 0x10000) { printf("    X%-2d 解引用: 跳过(值太小,非指针)\n", reg_idx); return; }
    uint8_t m[16] = {0};
    if (!drv.read_fast(ptr, m, 16)) { printf("    X%-2d 解引用: 无效地址 0x%lx\n", reg_idx, ptr); return; }
    char tag[32]; snprintf(tag, sizeof(tag), "*X%d", reg_idx);
    printf("    \033[1;36mX%-2d -> 0x%lx 解引用:\033[0m\n", reg_idx, ptr);
    dump_floats(tag, 0, m, 4); // 16 字节 = 4 个 float，正好覆盖 FVector/FRotator
}

// 用"空修改"重新武装 single-shot 断点（探测阶段不改任何寄存器）
static bool arm(paradise_driver& drv, HW_BP_INFO& info) {
    info.is_write_gp_regs = false;
    info.is_write_fp_regs = false;
    return drv.hwbp_enable(&info);
}

int main() {
    system("setenforce 0");
    printf("====================================================\n");
    printf("  ShootBulletInner 参数传递方式探测 (改进版)\n");
    printf("====================================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (!base) { printf("[-] 基址失败\n"); return 1; }
    uintptr_t shoot_addr = base + 0x6DFE100;
    printf("[+] ShootBulletInner = 0x%lx\n", shoot_addr);

    HW_BP_INFO info = {0};
    info.pid  = pid;
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len  = 4;
    if (!drv.hwbp_add(&info)) { printf("[-] hwbp_add 失败\n"); return 1; }

    // 先武装第一发
    bool armed = arm(drv, info);
    printf("[+] 断点已武装(首次 enable=%s)。请单点射 2~3 发，朝【斜上/斜侧】开！\n",
           armed ? "OK" : "FAIL");
    printf("[!] 别朝正北平视开(那样 Pitch/Yaw 都≈0 看不出)。每发间隔>1秒。\n");
    printf("----------------------------------------------------\n");

    HWBP_HIT_ITEM hits[16];
    uint8_t vregs[32][16];
    uint32_t fpsr, fpcr;
    int total = 0;

    while (true) {
        HWBP_HIT_ARGS args = {0};
        args.pid = pid; args.addr = shoot_addr;
        args.out_buf = hits; args.out_len = 16; args.real_count = 0;

        if (drv.hwbp_get_hits(&args) && args.real_count > 0) {
            for (int h = 0; h < args.real_count; h++) {
                total++;
                g_cand.clear();
                REGS_INFO& R = hits[h].regs_info;
                printf("\n\033[1;33m########## 命中 #%d  PC=0x%lx  SP=0x%lx ##########\033[0m\n",
                       total, R.pc, R.sp);

                // —— 视角1：通用寄存器 X0-X7，三合一(整型 / float / 指针解引用) ——
                printf("\033[1;36m[GPR] X0-X7 (整型 | 低32位float):\033[0m\n");
                for (int i = 0; i < 8; i++) {
                    uint32_t lo = (uint32_t)(R.regs[i] & 0xFFFFFFFF);
                    float fv = *(float*)&lo;
                    printf("    X%-2d = 0x%016lx | int=%-11d | float=%9.4f\n",
                           i, R.regs[i], (int32_t)lo, fv);
                    char tag[16]; snprintf(tag, sizeof(tag), "X%d.lo", i);
                    note(tag, fv, lo);
                }
                // 把 X0/X1 当指针解引用（const ref 传递的关键证据）
                dump_deref(drv, 0, R.regs[0]);
                dump_deref(drv, 1, R.regs[1]);

                // —— 视角2：栈 [SP, SP+96) ——
                printf("\033[1;34m[STACK] SP..SP+96:\033[0m\n");
                uint8_t stk[96] = {0};
                if (drv.read_fast(R.sp, stk, 96)) dump_floats("SP", 0, stk, 24);
                else printf("    栈读取失败\n");

                // —— 视角3：浮点寄存器 V0-V7 (⚠ 可能是实时值，需对比验证) ——
                printf("\033[0;31m[FP] V0-V7 (⚠ 若非命中快照则为实时值，请对比多发射击是否随瞄准变化):\033[0m\n");
                if (drv.fpr_read(pid, 0xFF, vregs, &fpsr, &fpcr)) {
                    for (int i = 0; i < 8; i++) {
                        uint32_t raw = *(uint32_t*)vregs[i];
                        float v = *(float*)&raw;
                        char tag[16]; snprintf(tag, sizeof(tag), "V%d", i);
                        bool star = is_angle_like(v, raw) && fabsf(v) > 0.5f;
                        if (star) printf("    \033[1;32m★ %-4s = %9.4f\033[0m (0x%08x)\n", tag, v, raw);
                        else      printf("      %-4s = %9.4f (0x%08x)\n", tag, v, raw);
                        note(tag, v, raw);
                    }
                } else printf("    fpr_read 失败\n");

                // —— 自动候选汇总 ——
                printf("\033[1;35m[候选清单] 落在[-180,180]的值(★=非零强候选):\033[0m\n");
                if (g_cand.empty()) printf("    (无)\n");
                for (auto& kv : g_cand) {
                    printf("    %-12s = %9.4f %s\n", kv.first.c_str(), kv.second,
                           fabsf(kv.second) > 0.5f ? "★" : "");
                }
                printf("\033[1;33m############################################################\033[0m\n");
            }
            // 命中冷却 800ms：single-shot 命中后已自动卸甲，sleep 期间不抓，
            // 避免一梭子刷 30 发；sleep 结束后重新武装，正好抓下一发用于对比
            usleep(800000);
            arm(drv, info);   // 重新武装，准备抓下一发
        } else {
            usleep(5000);
            // 保活：长时间没命中也周期性重新武装，防止 single-shot 状态丢失
            static int idle = 0;
            if (++idle > 200) { idle = 0; arm(drv, info); }
        }
    }

    drv.hwbp_clear();
    return 0;
}
