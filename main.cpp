#include "paradise_api.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

const char* STOP_FILE = "/data/local/tmp/stop_tracker";
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

int main() {
    remove(STOP_FILE);
    system("setenforce 0"); // 强制关闭 SELinux
    
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
    
    // 配置硬件断点信息
    HW_BP_INFO info;
    memset(&info, 0, sizeof(info));
    info.pid = pid;
    info.addr = shoot_addr;
    info.type = 4; // HW_BP_TYPE_X (执行断点)
    info.len = 4;
    
    // 【核心】配置修改浮点寄存器 V3 (Pitch)
    info.is_write_fp_regs = true;
    info.fp_reg_count = 1;
    info.fp_reg_indices[0] = 3; // 锁定 V3 寄存器
    
    // 将 -89.0f (接近垂直朝天) 写入 V3 的低 32 位
    float pitch_sky = -89.0f;
    memcpy(&info.fp_reg_values[0][0], &pitch_sky, sizeof(float));
    
    // 安装断点
    if (drv.hwbp_add(&info)) {
        printf("\033[1;32m[+] 射天断点 (0x6DFE100) 安装成功！\033[0m\n");
    } else {
        printf("\033[1;31m[-] 断点安装失败！\033[0m\n");
        return 1;
    }

    printf("\n\033[1;33m[!] 进游戏随便朝地上或平视开枪，子弹会直接射向天空！\033[0m\n");
    printf("\033[1;33m[!] 退出请在终端执行: touch %s\033[0m\n\n", STOP_FILE);

    // 保持程序运行，等待退出信号
    while (g_Running) {
        if (access(STOP_FILE, F_OK) == 0) break;
        usleep(100000);
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    remove(STOP_FILE);
    printf("[+] 安全退出，内核零残留！\n");
    return 0;
}