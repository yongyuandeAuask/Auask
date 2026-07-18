#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>

const char* STOP_FILE = "/data/断点测试/关闭";
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

// 辅助函数：打印 V0~V7 的低 32 位 (float)
void print_vregs(const char* label, uint8_t vregs[32][16]) {
    printf("\033[1;36m[%s] 浮点寄存器快照 (V0~V7):\033[0m\n", label);
    for (int i = 0; i < 8; i++) {
        float val;
        memcpy(&val, &vregs[i][0], sizeof(float));
        printf("  V%d: %.4f\n", i, val);
    }
}

int main() {
    remove(STOP_FILE);
    system("setenforce 0");
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    printf("========================================\n");
    printf("  Paradise 寄存器对比分析与射天验证\n");
    printf("========================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    printf("[+] PID: %d\n", pid);
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }
    printf("[+] libUE4.so: 0x%lx\n", base);

    uintptr_t shoot_addr = base + 0x6DFE100;
    
    // 1. 安装断点 (仅拦截，不修改)
    HW_BP_INFO info;
    memset(&info, 0, sizeof(info));
    info.pid = pid;
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    
    if (!drv.hwbp_add(&info)) {
        printf("[-] 断点安装失败\n");
        return 1;
    }
    printf("[+] 断点安装成功 (只读模式)\n\n");

    uint8_t vregs_sky[32][16] = {0};
    uint8_t vregs_ground[32][16] = {0};
    uint32_t fpsr, fpcr;
    uint32_t mask = 0xFF; // 读取 V0~V7 (bit0~bit7)

    // ================= 阶段一：朝天开枪抓取 =================
    printf("\033[1;33m[!] 第一步：请进游戏，把准星瞄准【天空】，开一枪。\033[0m\n");
    printf("\033[1;33m[!] 开完枪后，切回终端，按【回车键】继续...\033[0m\n");
    while(getchar() != '\n'); 
    
    drv.fpr_read(pid, mask, vregs_sky, &fpsr, &fpcr);
    print_vregs("朝天开枪", vregs_sky);

    // ================= 阶段二：朝地开枪抓取 =================
    printf("\n\033[1;33m[!] 第二步：请进游戏，把准星瞄准【地面】，开一枪。\033[0m\n");
    printf("\033[1;33m[!] 开完枪后，切回终端，按【回车键】继续...\033[0m\n");
    while(getchar() != '\n'); 
    
    drv.fpr_read(pid, mask, vregs_ground, &fpsr, &fpcr);
    print_vregs("朝地开枪", vregs_ground);

    // ================= 阶段三：自动分析差异 =================
    printf("\n\033[1;32m[*] 正在分析差异，寻找控制 Pitch (俯仰角) 的寄存器...\033[0m\n");
    int pitch_reg = -1;
    float max_diff = 0.0f;
    
    for (int i = 0; i < 8; i++) {
        float v_sky, v_ground;
        memcpy(&v_sky, &vregs_sky[i][0], sizeof(float));
        memcpy(&v_ground, &vregs_ground[i][0], sizeof(float));
        
        float diff = fabs(v_sky - v_ground);
        printf("  V%d 差值: %.4f\n", i, diff);
        
        if (diff > max_diff) {
            max_diff = diff;
            pitch_reg = i;
        }
    }

    if (pitch_reg == -1 || max_diff < 1.0f) {
        printf("\033[31m[-] 分析失败：未找到明显变化的寄存器。可能是 fpr_read 读取时机不对。\033[0m\n");
        printf("    建议：直接使用盲测法，依次修改 V0~V7 测试。\n");
        drv.hwbp_clear();
        return 1;
    }

    printf("\n\033[1;42;37m[!!! 结论] V%d 是控制 Pitch 的核心寄存器！(差值: %.4f)\033[0m\n", pitch_reg, max_diff);

    // ================= 阶段四：强行注入，验证射天 =================
    printf("\n\033[1;33m[*] 正在配置 V%d = -89.0f 并激活断点篡改...\033[0m\n", pitch_reg);
    
    info.is_write_fp_regs = true;
    info.fp_reg_count = 1;
    info.fp_reg_indices[0] = pitch_reg;
    
    float sky_val = -89.0f;
    memcpy(&info.fp_reg_values[0][0], &sky_val, sizeof(float));
    
    // 核心：调用 hwbp_enable 激活断点并注入修改规则
    if (drv.hwbp_enable(&info)) {
        printf("\033[1;32m[+] 篡改规则已激活！\033[0m\n");
    } else {
        printf("\033[31m[-] hwbp_enable 失败！\033[0m\n");
        drv.hwbp_clear();
        return 1;
    }

    printf("\n\033[1;41;37m[!!! 终极验证] 现在进游戏，随便朝哪里开枪，子弹必须射向天空！\033[0m\n");
    printf("\033[1;33m[!] 退出请在终端执行: touch %s\033[0m\n\n", STOP_FILE);

    while (g_Running) {
        if (access(STOP_FILE, F_OK) == 0) break;
        usleep(100000);
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    remove(STOP_FILE);
    printf("[+] 安全退出！\n");
    return 0;
}
