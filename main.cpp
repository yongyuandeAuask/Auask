#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <pthread.h>

volatile bool g_Running = true;

// 终端输入监听线程 (按 q 退出)
void* InputThread(void* arg) {
    char buf[16];
    while(g_Running) {
        if(fgets(buf, sizeof(buf), stdin)) {
            if(buf[0] == 'q' || buf[0] == 'Q') {
                g_Running = false;
                break;
            }
        }
    }
    return NULL;
}

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" ShootBulletInner 极简安全断点测试\n");
    printf(" (纯记录模式，绝不修改寄存器，防卡死)\n");
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
    uintptr_t shoot_addr = base + 0x6DFE100;
    printf("[+] 目标地址: 0x%lx\n", shoot_addr);

    // 【防卡死核心 1】获取 TID，但最多只处理前 15 个线程，防止内核 perf_event 爆炸
    std::vector<pid_t> tids;
    char path[256];
    sprintf(path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL && tids.size() < 15) {
            if (entry->d_type == DT_DIR) {
                pid_t tid = atoi(entry->d_name);
                if (tid > 0) tids.push_back(tid);
            }
        }
        closedir(dir);
    }
    if (tids.empty()) tids.push_back(pid); 

    printf("[*] 准备给 %zu 个核心线程挂载纯记录断点...\n", tids.size());
    
    int success = 0;
    for (pid_t tid : tids) {
        HW_BP_INFO info = {0};
        info.pid = tid;
        info.addr = shoot_addr;
        info.type = HW_BP_TYPE_X; // 执行断点
        info.len = 4;
        
        // 【防卡死核心 2】绝对不修改任何寄存器！纯旁路监听！
        info.is_write_gp_regs = false;
        info.is_write_fp_regs = false; 
        
        if (drv.hwbp_add(&info)) {
            success++;
        }
    }
    printf("[+] 成功挂载 %d 个纯记录断点。\n", success);

    if (success == 0) {
        printf("[-] 断点挂载失败，请检查驱动！\n");
        return 1;
    }

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;33m[!] 进游戏开枪测试。游戏绝对不会卡死或闪退。\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，在此终端输入 'q' 并回车安全退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    HWBP_HIT_ITEM hits[16];
    HWBP_HIT_ARGS args = {0};
    args.out_buf = hits;
    args.out_len = sizeof(hits);

    int total_hits = 0;

    while (g_Running) {
        bool hit_this_round = false;
        for (pid_t tid : tids) {
            args.pid = tid;
            args.addr = shoot_addr;
            args.real_count = 0;
            
            // 获取触发记录
            if (drv.hwbp_get_hits(&args) && args.real_count > 0) {
                hit_this_round = true;
                total_hits += args.real_count;
                for (int i = 0; i < args.real_count; i++) {
                    printf("\033[1;32m[触发] TID:%d | PC:0x%lx | X0:0x%lx | X1:0x%lx\033[0m\n", 
                           hits[i].task_id, hits[i].regs_info.pc, 
                           hits[i].regs_info.regs[0], hits[i].regs_info.regs[1]);
                }
            }
        }
        if (!hit_this_round) {
            usleep(2000); // 没触发时稍微休眠，降低 CPU 占用
        }
    }

    printf("\n[*] 总计触发 %d 次。正在清理断点...\n", total_hits);
    drv.hwbp_clear();
    printf("[+] 清理完毕，安全退出！\n");
    return 0;
}
