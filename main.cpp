#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" 开枪瞬间寄存器全自动快照器\n");
    printf(" (利用 IsFiring 内存状态触发抓取)\n");
    printf("========================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    printf("[+] PID: %d\n", pid);
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }

    // 获取 Oneself 基址
    uintptr_t p1 = drv.read_fast<uintptr_t>(base + 0xf1fb900);
    uintptr_t p2 = drv.read_fast<uintptr_t>(p1 + 0x810);
    uintptr_t UWorld = drv.read_fast<uintptr_t>(p2 + 0x78);
    uintptr_t o1 = drv.read_fast<uintptr_t>(UWorld + 0x38);
    uintptr_t o2 = drv.read_fast<uintptr_t>(o1 + 0x78);
    uintptr_t o3 = drv.read_fast<uintptr_t>(o2 + 0x30);
    uintptr_t Oneself = drv.read_fast<uintptr_t>(o3 + 0x28c8);
    if (Oneself < 0x10000) { printf("[-] 获取 Oneself 失败，请确保在对局中\n"); return 1; }
    printf("[+] Oneself: 0x%lx\n", Oneself);

    printf("\n\033[1;33m[!] 准备就绪。请进游戏：\033[0m\n");
    printf("\033[1;32m 1. 把准星瞄准【天空】，开一枪！\033[0m\n");
    printf("\033[1;32m 2. 把准星瞄准【地面】，开一枪！\033[0m\n");
    printf("\033[1;33m[!] 程序会自动检测开火状态，并瞬间抓取寄存器快照。\033[0m\n");
    printf("--------------------------------------------------\n");

    int last_firing_state = 0;
    int snapshot_count = 0;

    while (snapshot_count < 2) { // 只抓取前两次开火（朝天和朝地）
        // 读取开火状态 (偏移 0x1830，如果不对可以尝试 0x1820 或 0x1840)
        int is_firing = drv.read_fast<int>(Oneself + 0x1830);

        // 检测开火瞬间 (从 0 变为 1)
        if (is_firing == 1 && last_firing_state == 0) {
            snapshot_count++;
            printf("\n\033[1;41;37m[!!! 捕获到第 %d 次开火！正在冻结寄存器...]\033[0m\n", snapshot_count);

            // 1. 抓取浮点寄存器 (V0 - V31)
            uint8_t vregs[32][16] = {0};
            uint32_t fpsr, fpcr;
            drv.fpr_read(pid, 0xFFFFFFFF, vregs, &fpsr, &fpcr);

            printf("\033[1;36m--- 浮点寄存器 (V0~V15) 低32位 (Float) ---\033[0m\n");
            for (int i = 0; i < 16; i++) {
                float val;
                memcpy(&val, &vregs[i][0], sizeof(float));
                // 高亮显示在 -90 到 90 之间的值（极大概率是角度）
                if (val >= -90.0f && val <= 90.0f && val != 0.0f) {
                    printf("  \033[1;32mV%d: %.4f  <--- (疑似角度!)\033[0m\n", i, val);
                } else {
                    printf("  V%d: %.4f\n", i, val);
                }
            }

            // 2. 抓取通用寄存器 (X0 - X30)
            uint64_t xregs[31] = {0};
            uint64_t sp, pc, pstate;
            drv.gpr_read(pid, xregs, &sp, &pc, &pstate);

            printf("\n\033[1;36m--- 通用寄存器 (X0~X7) 拆解 ---\033[0m\n");
            for (int i = 0; i < 8; i++) {
                float f_low, f_high;
                memcpy(&f_low, &xregs[i], 4);
                memcpy(&f_high, (uint8_t*)&xregs[i] + 4, 4);
                printf("  X%d: 0x%lx (Low:%.2f | High:%.2f)\n", i, xregs[i], f_low, f_high);
            }
            
            // 等待开火状态恢复为 0，防止连发重复抓取
            while (drv.read_fast<int>(Oneself + 0x1830) == 1) {
                usleep(1000);
            }
        }
        last_firing_state = is_firing;
        usleep(1000); // 1ms 高频轮询，确保不错过开火瞬间
    }

    printf("\n\033[1;42;37m[+] 两次开火快照抓取完毕！\033[0m\n");
    printf("请对比上面两次打印的 \033[1;32m绿色疑似角度\033[0m 数据。\n");
    printf("找出那个在【朝天】和【朝地】时，数值发生剧烈变化（比如从 89 变成 -89）的寄存器！\n");
    printf("告诉我它是 V 几，或者 X 几，我们立刻用真实数据去改写它！\n");

    return 0;
}
