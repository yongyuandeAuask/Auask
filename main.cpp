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
    printf(" PTE UXN 128位无损射天测试 (终极版)\n");
    printf(" (使用 fpr_read_modify_write 杜绝污染)\n");
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

    // 1. 获取所有 TID 并全量登记 PTE UXN 断点
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

    // 2. 准备 89.0f (正数，垂直朝天！)
    float sky_pitch = 89.0f; 
    uint32_t write_indices[1] = {3}; // 目标：V3
    float write_values[1] = {sky_pitch};
    uint8_t out_vregs[32][16]; // 接收内核读出的完整 128 位数据

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 128位无损篡改已激活！V3 低32位将被替换为 89.0f (朝天)\033[0m\n");
    printf("\033[1;33m[!] 进游戏，把准星瞄准【地板】，然后开枪！\033[0m\n");
    printf("\033[1;33m[!] 子弹必须垂直射向天花板！\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，输入 'q' 并回车安全退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    // 3. 死循环：高频下发“读-改-写”指令与 Enable 规则
    while (g_Running) {
        // 轨道 A：调用 fpr_read_modify_write 
        // 内核会在断点触发时，自动读取当前 V3，替换低 32 位为 89.0f，保留高 96 位
        drv.fpr_read_modify_write(
            pid, 
            0xFFFFFFFF,       // read_mask: 读取所有
            1,                // write_count: 修改 1 个
            write_indices,    // V3
            write_values,     // 89.0f
            out_vregs         // 输出缓冲
        );

        // 轨道 B：将内核读出的“完整 128 位 V3”塞进 hwbp_enable 规则中
        for (pid_t tid : tids) {
            info.pid = tid;
            info.is_write_fp_regs = true;
            info.fp_reg_count = 1;
            info.fp_reg_indices[0] = 3;
            
            // 【核心】把包含 89.0f 和 原始高96位 的完整 16 字节传给内核
            memcpy(info.fp_reg_values[0], out_vregs[3], 16); 
            
            drv.hwbp_enable(&info);
        }

        usleep(2000); // 2ms 刷新
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    printf("[+] 安全退出！\n");
    return 0;
}
