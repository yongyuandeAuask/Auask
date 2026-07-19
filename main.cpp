#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <pthread.h>

volatile bool g_Running = true;

// 终端输入监听 (按 q 退出)
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

// 结构体：存储线程 ID 和它的 CPU 耗时
struct ThreadStat {
    pid_t tid;
    unsigned long long cpu_time;
};

// 获取进程下最活跃的前 N 个线程 (核心防卡死逻辑)
std::vector<pid_t> get_top_active_threads(pid_t pid, int top_n) {
    std::vector<ThreadStat> stats;
    char path[256];
    sprintf(path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (!dir) return {};

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            pid_t tid = atoi(entry->d_name);
            if (tid <= 0) continue;

            // 读取 /proc/pid/task/tid/stat 获取 CPU 耗时 (utime + stime)
            char stat_path[512];
            sprintf(stat_path, "/proc/%d/task/%d/stat", pid, tid);
            FILE* fp = fopen(stat_path, "r");
            if (fp) {
                char line[1024];
                if (fgets(line, sizeof(line), fp)) {
                    unsigned long long utime = 0, stime = 0;
                    // stat 文件的第 14 和 15 个字段是 utime 和 stime
                    char* ptr = line;
                    for(int i=0; i<13; i++) { while(*ptr && *ptr != ' ') ptr++; if(*ptr) ptr++; }
                    utime = strtoull(ptr, &ptr, 10);
                    stime = strtoull(ptr, &ptr, 10);
                    stats.push_back({tid, utime + stime});
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);

    // 按 CPU 耗时降序排序
    std::sort(stats.begin(), stats.end(), [](const ThreadStat& a, const ThreadStat& b) {
        return a.cpu_time > b.cpu_time;
    });

    std::vector<pid_t> top_tids;
    for (int i = 0; i < top_n && i < stats.size(); i++) {
        top_tids.push_back(stats[i].tid);
        printf("  [*] 选中核心线程 TID: %d (CPU耗时: %llu)\n", stats[i].tid, stats[i].cpu_time);
    }
    return top_tids;
}

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" 智能活跃线程 HWBP 狙击 + 追踪验证\n");
    printf========================================\n");

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

    // 1. 智能筛选最活跃的前 5 个线程 (避开主线程和闲置线程，防卡死)
    printf("[*] 正在分析线程活跃度...\n");
    std::vector<pid_t> active_tids = get_top_active_threads(pid, 5);
    if (active_tids.empty()) {
        printf("[-] 获取线程列表失败\n"); return 1;
    }

    // 2. 给这几个核心线程挂载 HWBP
    int success = 0;
    for (pid_t tid : active_tids) {
        HW_BP_INFO info = {0};
        info.pid = tid; // 【核心】传入 TID！
        info.addr = shoot_addr;
        info.type = HW_BP_TYPE_X;
        info.len = 4;
        if (drv.hwbp_add(&info)) success++;
    }
    printf("[+] 成功在 %d 个核心线程上挂载 HWBP！\n", success);

    // 3. 配置 Tracking 数据 (喂入正上方 10000 单位的坐标)
    TRACKING_DATA track = {0};
    track.is_active = true;
    track.bp_addr = shoot_addr;
    track.x = 0.0f;
    track.y = 0.0f;
    track.z = 10000.0f; // 垂直朝天

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 追踪系统已激活！目标: 正上方 10000 单位\033[0m\n");
    printf("\033[1;33m[!] 进游戏，把准星瞄准【地板】，然后开枪！\033[0m\n");
    printf("\033[1;33m[!] 如果子弹射天，说明 HWBP 线程狙击成功！\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，输入 'q' 并回车安全退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    // 4. 死循环下发 Tracking 数据并检测 HWBP 是否真的触发
    HWBP_HIT_ARGS hit_args = {0};
    hit_args.addr = shoot_addr;
    HWBP_HIT_ITEM hits[4];
    hit_args.out_buf = hits;
    hit_args.out_len = sizeof(hits);

    int total_hits = 0;

    while (g_Running) {
        // 持续喂坐标
        drv.hwbp_update_tracking(&track);
        
        // 顺便检测一下 HWBP 到底有没有触发 (虽然作者说不记录，但试试无妨)
        for(pid_t tid : active_tids) {
            hit_args.pid = tid;
            hit_args.real_count = 0;
            drv.hwbp_get_hits(&hit_args);
            if(hit_args.real_count > 0) total_hits += hit_args.real_count;
        }

        if(total_hits > 0) {
            printf("\r\033[1;32m[!!!] 检测到 HWBP 触发 %d 次！内核已接管追踪！   \033[0m", total_hits);
            fflush(stdout);
        }
        usleep(5000); // 5ms 刷新
    }

    printf("\n[*] 总计触发 %d 次。正在清理断点...\n", total_hits);
    drv.hwbp_clear();
    printf("[+] 清理完毕，安全退出！\n");
    return 0;
}
