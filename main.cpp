#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <math.h>
#include <time.h>
#include <pthread.h>

#define PI 3.14159265358979323846f

volatile bool g_Running = true;

void* InputThread(void* arg) {
    char buf[16];
    while(g_Running) {
        if(fgets(buf, sizeof(buf), stdin)) {
            if(buf[0] == 'q' || buf[0] == 'Q') { g_Running = false; break; }
        }
    }
    return NULL;
}

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" PTE UXN 正弦波阶梯测试 (验证动态注入)\n");
    printf("========================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }
    uintptr_t shoot_addr = base + 0x6DFE100;

    // 1. 全量登记 PTE UXN 断点
    std::vector<pid_t> tids;
    char path[256];
    sprintf(path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_DIR) {
                pid_t tid = atoi(entry->d_name);
                if (tid > 0) tids.push_back(tid);
            }
        }
        closedir(dir);
    }

    HW_BP_INFO info = {0};
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    
    for (pid_t tid : tids) {
        info.pid = tid;
        drv.hwbp_add(&info);
    }
    printf("[+] 已为 %zu 个线程登记 PTE UXN 断点\n", tids.size());

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 正弦波注入已激活！Pitch 将在 -45° 到 45° 之间波动。\033[0m\n");
    printf("\033[1;33m[!] 进游戏，找一面墙，【按住开火键不松手】打空一个弹匣！\033[0m\n");
    printf("\033[1;33m[!] 观察墙上的弹孔：如果是【上下波浪形】，说明通道完美！\033[0m\n");
    printf("\033[1;33m[!] 测试完按 'q' 退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    uint32_t write_indices[2] = {3, 4}; // V3(Pitch), V4(Yaw)
    float write_values[2] = {0.0f, 0.0f};
    uint8_t out_vregs[32][16];

    while (g_Running) {
        // 获取高精度时间
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec / 1e9;

        // 【核心】生成正弦波 Pitch (-45.0 到 45.0)
        write_values[0] = 45.0f * sin(t * 3.0); // 每 2 秒一个完整波浪
        write_values[1] = 0.0f; // Yaw 锁定为 0 (正前方)

        // 轨道 A：128位无损注入
        drv.fpr_read_modify_write(pid, 0xFFFFFFFF, 2, write_indices, write_values, out_vregs);

        // 轨道 B：全线程 hwbp_enable
        for (pid_t tid : tids) {
            info.pid = tid;
            info.is_write_fp_regs = true;
            info.fp_reg_count = 2;
            info.fp_reg_indices[0] = 3;
            info.fp_reg_indices[1] = 4;
            memcpy(info.fp_reg_values[0], out_vregs[3], 16);
            memcpy(info.fp_reg_values[1], out_vregs[4], 16);
            drv.hwbp_enable(&info);
        }

        printf("\r\033[1;32m[注入] 当前 Pitch: %.2f° | Yaw: %.2f°   \033[0m", write_values[0], write_values[1]);
        fflush(stdout);

        usleep(2000); // 2ms 刷新
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    printf("[+] 安全退出！\n");
    return 0;
}
