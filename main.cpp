#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

const char* STOP_FILE = "/data/断点测试/退出";
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

int main() {
    remove(STOP_FILE);
    system("setenforce 0");
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    printf("========================================\n");
    printf("  Paradise 驱动·子弹射向天空断点测试\n");
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
    
    // 安装硬件断点 (仅用于触发追踪)
    HW_BP_INFO info;
    memset(&info, 0, sizeof(info));
    info.pid = pid;
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    if (drv.hwbp_add(&info)) {
        printf("\033[1;32m[+] 断点安装成功！\033[0m\n");
    } else {
        printf("\033[1;31m[-] 断点安装失败！\033[0m\n");
        return 1;
    }

    printf("\n\033[1;33m[!] 进游戏随便开枪，子弹会直接射向天空！\033[0m\n");
    printf("\033[1;33m[!] 退出请在终端执行: touch %s\033[0m\n\n", STOP_FILE);

    // 核心：喂坐标让内核自动计算角度
    TRACKING_DATA track_data;
    track_data.bp_addr = shoot_addr;
    track_data.is_active = true;
    
    // 【关键】将 (0, 0, 10000) 高空坐标喂给内核
    // 内核会自动计算出完美的 -89.0f Pitch 角度
    track_data.x = 0.0f;
    track_data.y = 0.0f;
    track_data.z = 10000.0f;

    while (g_Running) {
        if (access(STOP_FILE, F_OK) == 0) break;
        
        // 每次开枪时，内核会自动把子弹导向 (0, 0, 10000)
        drv.hwbp_update_tracking(&track_data);
        usleep(5000);
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    remove(STOP_FILE);
    printf("[+] 安全退出，内核零残留！\n");
    return 0;
}
