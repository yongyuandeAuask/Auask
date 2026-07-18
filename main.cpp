#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>

const char* STOP_FILE = "/data/断点测试/关闭";
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

paradise_driver* g_drv = nullptr;
pid_t g_pid = 0;
uintptr_t g_shoot_addr = 0;

// 辅助：判断是否像内存指针
bool is_pointer(uint64_t val) {
    return (val > 0x40000000ULL && val < 0xFFFFFFFFFFFFULL);
}

// 【核心线程】事后诸葛亮内存篡改器
// 既然 hwbp_enable 只能改寄存器，如果角度在内存里，我们就用这个线程死循环抓触发，然后强行改内存！
void* MemorySniperThread(void* arg) {
    HWBP_HIT_ITEM hits[4];
    HWBP_HIT_ARGS args = {0};
    args.pid = g_pid;
    args.addr = g_shoot_addr;
    args.out_buf = hits;
    args.out_len = sizeof(hits);

    float sky_val = -89.0f;
    
    while (g_Running) {
        if (g_drv->hwbp_get_hits(&args) && args.real_count > 0) {
            // 抓到触发了！立刻检查 X0~X7 谁像指针
            for (int i = 0; i < 8; i++) {
                uint64_t x_reg = hits[0].regs_info.regs[i];
                if (is_pointer(x_reg)) {
                    // 强行把这块内存的前 4 个 float 全部改成 -89.0f (朝天)
                    float mem_patch[4] = {sky_val, sky_val, sky_val, sky_val};
                    g_drv->write(x_reg, mem_patch, sizeof(mem_patch));
                }
            }
        }
        usleep(1000); // 1ms 极速轮询，争取赶上连发的第二颗子弹
    }
    return NULL;
}

int main() {
    remove(STOP_FILE);
    system("setenforce 0");
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    printf("========================================\n");
    printf("  Paradise 全维度爆破测试台 (修复版)\n");
    printf("========================================\n");

    g_drv = new paradise_driver();
    g_pid = g_drv->get_pid("com.tencent.ig");
    if (g_pid <= 0) g_pid = g_drv->get_pid("com.rekoo.pubgm");
    if (g_pid <= 0) g_pid = g_drv->get_pid("com.vng.pubgmobile");
    if (g_pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    printf("[+] PID: %d\n", g_pid);
    g_drv->initialize(g_pid);

    uintptr_t base = g_drv->get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }
    printf("[+] libUE4.so: 0x%lx\n", base);

    g_shoot_addr = base + 0x6DFE100;
    
    // 1. 配置并安装断点
    HW_BP_INFO info;
    memset(&info, 0, sizeof(info));
    info.pid = g_pid;
    info.addr = g_shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    
    if (!g_drv->hwbp_add(&info)) {
        printf("[-] hwbp_add 失败\n"); return 1;
    }
    printf("[+] 断点注册成功\n");

    // 2. 【致命修复】配置全维度篡改规则，并调用 hwbp_enable 激活！
    info.is_write_fp_regs = true;
    info.fp_reg_count = 3;
    info.fp_reg_indices[0] = 0; // V0
    info.fp_reg_indices[1] = 1; // V1
    info.fp_reg_indices[2] = 2; // V2
    
    info.is_write_gp_regs = true;
    info.gp_reg_count = 3;
    info.gp_reg_indices[0] = 0; // X0
    info.gp_reg_indices[1] = 1; // X1
    info.gp_reg_indices[2] = 2; // X2

    float sky = -89.0f;
    uint32_t sky_hex;
    memcpy(&sky_hex, &sky, 4);

    // 把 V0~V2 的低 32 位设为 -89.0f
    for(int i=0; i<3; i++) memcpy(&info.fp_reg_values[i][0], &sky, 4);
    
    // 把 X0~X2 的低 32 位设为 -89.0f，高 32 位保留 0 (防止破坏指针导致崩溃)
    for(int i=0; i<3; i++) info.gp_reg_values[i] = sky_hex; 

    // 【核心】调用 hwbp_enable 激活篡改规则！
    if (g_drv->hwbp_enable(&info)) {
        printf("\033[1;32m[+] hwbp_enable 激活成功！篡改规则已下发内核！\033[0m\n");
    } else {
        printf("\033[1;31m[-] hwbp_enable 失败！\033[0m\n");
    }

    // 3. 启动内存狙击线程 (对付角度在内存里的情况)
    pthread_t sniper_tid;
    pthread_create(&sniper_tid, NULL, MemorySniperThread, NULL);
    printf("[+] 内存狙击线程已启动 (连发第二枪必中)\n");

    printf("\n\033[1;41;37m[!!! 终极验证] 进游戏开枪！\033[0m\n");
    printf("  - 如果单发朝天：说明 X/V 寄存器篡改生效。\n");
    printf("  - 如果连发第二枪朝天：说明内存狙击生效。\n");
    printf("  - 退出请执行: touch %s\n\n", STOP_FILE);

    while (g_Running) {
        if (access(STOP_FILE, F_OK) == 0) break;
        // 持续刷新 enable 状态，防止内核丢弃规则
        g_drv->hwbp_enable(&info); 
        usleep(50000);
    }

    g_Running = false;
    pthread_join(sniper_tid, NULL);
    g_drv->hwbp_clear();
    remove(STOP_FILE);
    printf("[+] 安全退出！\n");
    return 0;
}
