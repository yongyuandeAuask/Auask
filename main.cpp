#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

// 文件哨兵退出机制 (适配手机终端)
const char* STOP_FILE = "/data/断点测试/关闭";
volatile bool g_Running = true;
void SignalHandler(int sig) { g_Running = false; }

struct FVector {
    float X, Y, Z;
};

int main() {
    remove(STOP_FILE);
    system("setenforce 0"); // 强制关闭 SELinux
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    printf("========================================\n");
    printf("  hook.h 链路·自身与相机坐标读取验证器\n");
    printf("========================================\n");

    // 1. 初始化 Paradise 驱动
    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏进程\n"); return 1; }
    printf("[+] PID: %d\n", pid);
    drv.initialize(pid);

    // 2. 获取 libUE4.so 基址
    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取 libUE4.so 基址失败\n"); return 1; }
    printf("[+] libUE4.so: 0x%lx\n", base);

    printf("\n\033[1;33m[!] 进游戏跑动一下，观察下方坐标是否实时变化。\033[0m\n");
    printf("\033[1;33m[!] 退出请在终端执行: touch %s\033[0m\n\n", STOP_FILE);

    int fail_count = 0;

    // 3. 核心循环：读取坐标
    while (g_Running) {
        if (access(STOP_FILE, F_OK) == 0) break;

        // --- 严格按照 hook.h 链路读取 GWorld ---
        uintptr_t p1 = drv.read<uintptr_t>(base + 0xf1fb900);
        if (p1 < 0x10000) { usleep(10000); continue; }
        
        uintptr_t p2 = drv.read<uintptr_t>(p1 + 0x810);
        if (p2 < 0x10000) { usleep(10000); continue; }
        
        uintptr_t UWorld = drv.read<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(10000); continue; }

        // --- 读取 Oneself (自身基址) ---
        uintptr_t o1 = drv.read<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = drv.read<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = drv.read<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = drv.read<uintptr_t>(o3 + 0x28c8);
        
        if (Oneself < 0x10000) { 
            fail_count++;
            if(fail_count % 50 == 0) printf("[-] 等待进入对局...\n");
            usleep(20000); 
            continue; 
        }

        // --- 读取自身坐标 (RootComponent + 0x1C8) ---
        uintptr_t RootComp = drv.read<uintptr_t>(Oneself + 0x208);
        FVector MyPos = {0, 0, 0};
        if (RootComp > 0x10000) {
            drv.read(RootComp + 0x1c8, &MyPos, sizeof(FVector));
        }

        // --- 读取相机坐标 (0x90 -> 0x490 -> 0x480) ---
        FVector CamPos = {0, 0, 0};
        uintptr_t cam1 = drv.read<uintptr_t>(Oneself + 0x90);
        if (cam1 > 0x10000) {
            uintptr_t cam2 = drv.read<uintptr_t>(cam1 + 0x490);
            if (cam2 > 0x10000) {
                drv.read(cam2 + 0x480, &CamPos, sizeof(FVector));
            }
        }
        
        // 兜底：如果相机读不到，用自身坐标 + 150 高度代替
        if (CamPos.X == 0 && CamPos.Y == 0 && CamPos.Z == 0) {
            CamPos = MyPos;
            CamPos.Z += 150.0f;
        }

        // --- 读取队伍和血量 (验证实体有效性) ---
        int MyTeam = drv.read<int>(Oneself + 0x998);
        float MyHealth = drv.read<float>(Oneself + 0xe60);

        // 打印结果
        printf("\r\033[1;32m[自身] X:%.1f Y:%.1f Z:%.1f | [相机] X:%.1f Y:%.1f Z:%.1f | 队伍:%d 血量:%.1f   \033[0m", 
               MyPos.X, MyPos.Y, MyPos.Z, 
               CamPos.X, CamPos.Y, CamPos.Z,
               MyTeam, MyHealth);
        fflush(stdout);

        usleep(50000); // 50ms 刷新一次，避免刷屏
    }

    printf("\n\n[+] 验证结束，安全退出！\n");
    remove(STOP_FILE);
    return 0;
}
