#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <pthread.h>

volatile bool g_Running = true;
volatile float g_aim_pitch = 89.0f; // 【核心】默认改为 89.0f (抬头)

// 终端交互线程：实时修改 Pitch 值
void* InputThread(void* arg) {
    char buf[64];
    printf("\n\033[1;33m[调参台] 请输入 Pitch 角度 (如 89, -45, 0)，按回车生效。\033[0m\n");
    printf("\033[1;33m[调参台] 输入 'q' 退出程序。\033[0m\n");
    
    while(g_Running) {
        if(fgets(buf, sizeof(buf), stdin)) {
            if(buf[0] == 'q' || buf[0] == 'Q') { 
                g_Running = false; 
                break; 
            }
            float val = atof(buf);
            g_aim_pitch = val;
            printf("\033[1;32m[*] V3 (Pitch) 已锁定为: %.2f (请进游戏开枪测试)\033[0m\n", g_aim_pitch);
        }
    }
    return NULL;
}

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" PTE UXN 断点 V3 交互式调参台\n");
    printf(" (纯手动篡改 V3，关闭 Tracking 干扰)\n");
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

    // 1. 获取所有 TID 并全量登记断点
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

    // 2. 【关键】彻底关闭 Tracking API，防止它覆盖我们的 V3 修改
    TRACKING_DATA track = {0};
    track.is_active = false; 
    track.bp_addr = shoot_addr;
    drv.hwbp_update_tracking(&track);

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 调参台已启动！当前 V3 = %.2f\033[0m\n", g_aim_pitch);
    printf("--------------------------------------------------\n");

    // 3. 死循环高频注入 V3
    uint32_t reg_indices[1] = {3}; // 锁定 V3
    
    while (g_Running) {
        float current_pitch = g_aim_pitch;
        
        for (pid_t tid : tids) {
            info.pid = tid;
            info.is_write_fp_regs = true;
            info.fp_reg_count = 1;
            info.fp_reg_indices[0] = 3;
            memcpy(&info.fp_reg_values[0][0], &current_pitch, sizeof(float));
            
            drv.hwbp_enable(&info);
        }
        
        // 辅助注入
        drv.fpr_write_floats(pid, 1, reg_indices, &current_pitch);

        usleep(2000); // 2ms 刷新
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    printf("[+] 安全退出！\n");
    return 0;
}
