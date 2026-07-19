#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <pthread.h>

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
    printf(" Paradise PTE UXN 断点终极激活修复器\n");
    printf(" (针对页表异常拦截，全量线程覆盖)\n");
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

    // 1. 获取所有 TID (PTE UXN 不占硬件槽位，全挂也不会卡死)
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
    printf("[+] 发现 %zu 个线程，开始全量登记 PTE UXN 断点...\n", tids.size());

    // 2. 全量登记 hwbp_add
    HW_BP_INFO info = {0};
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    
    int add_count = 0;
    for (pid_t tid : tids) {
        info.pid = tid;
        if (drv.hwbp_add(&info)) add_count++;
    }
    printf("[+] 成功登记 %d 个 PTE UXN 拦截点！\n", add_count);

    // 3. 准备 Tracking 数据 (喂入正上方 10000 单位)
    TRACKING_DATA track = {0};
    track.is_active = true;
    track.bp_addr = shoot_addr;
    track.x = 0.0f;
    track.y = 0.0f;
    track.z = 10000.0f; 

    // 4. 准备 V3 寄存器修改数据 (-89.0f)
    uint32_t reg_indices[1] = {3}; // V3
    float reg_values[1] = {-89.0f};

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 双轨激活系统已启动！\033[0m\n");
    printf("\033[1;33m[!] 进游戏，把准星瞄准【地板】，然后开枪！\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，输入 'q' 并回车安全退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    // 5. 死循环：持续下发 Enable 和 Tracking (核心修复逻辑)
    while (g_Running) {
        // 轨道 A：持续调用 hwbp_update_tracking (喂坐标)
        drv.hwbp_update_tracking(&track);

        // 轨道 B：持续对所有线程调用 hwbp_enable (强行改 V3)
        for (pid_t tid : tids) {
            info.pid = tid;
            info.is_write_fp_regs = true;
            info.fp_reg_count = 1;
            info.fp_reg_indices[0] = 3;
            // 将 -89.0f 放入低 32 位
            memcpy(&info.fp_reg_values[0][0], &reg_values[0], sizeof(float));
            
            drv.hwbp_enable(&info);
        }
        
        // 轨道 C：使用 fpr_write_floats 辅助注入 (防止 enable 被内核吞掉)
        drv.fpr_write_floats(pid, 1, reg_indices, reg_values);

        usleep(2000); // 2ms 高频刷新，确保异常触发时规则一定在内存中
    }

    printf("\n[*] 正在清理 PTE UXN 断点...\n");
    drv.hwbp_clear();
    printf("[+] 页表已恢复，安全退出！\n");
    return 0;
}
