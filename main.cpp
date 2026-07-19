#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

volatile bool g_Running = true;

// 终端输入监听线程 (按 q 恢复并退出)
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
    printf("  ShootBulletInner 函数瘫痪测试 (RET注入)\n");
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
    printf("[+] 目标函数地址: 0x%lx\n", shoot_addr);

    // 1. 备份原始指令
    uint32_t original_insn = 0;
    if (!drv.read(shoot_addr, &original_insn, 4)) {
        printf("[-] 读取原始指令失败\n"); return 1;
    }
    printf("[+] 原始指令备份成功: 0x%08x\n", original_insn);

    // 2. 注入 RET 指令 (0xD65F03C0)
    uint32_t ret_insn = 0xD65F03C0; 
    printf("[*] 正在注入 RET 指令 (函数瘫痪)...\n");
    
    if (drv.write(shoot_addr, ret_insn)) {
        printf("\033[1;41;37m[!!! 注入成功] ShootBulletInner 已被瘫痪！\033[0m\n");
    } else {
        printf("[-] 注入失败，驱动无代码段写权限\n"); return 1;
    }

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;33m[!] 验证时刻：\033[0m\n");
    printf("\033[1;32m[!] 请进游戏，按住开火键！你应该【打不出任何子弹】！\033[0m\n");
    printf("\033[1;33m[!] 验证完毕后，在此终端输入 'q' 并回车，函数将瞬间恢复正常。\033[0m\n");
    printf("--------------------------------------------------\n");

    // 3. 等待退出并恢复
    while (g_Running) {
        // 持续压制，防止反作弊热修复代码
        drv.write(shoot_addr, ret_insn);
        usleep(50000); 
    }

    // 4. 完美恢复
    printf("\n[*] 正在恢复原始指令 (0x%08x)...\n", original_insn);
    if (drv.write(shoot_addr, original_insn)) {
        printf("\033[1;32m[+] 恢复成功！开枪功能已恢复正常。\033[0m\n");
    } else {
        printf("[-] 恢复失败，请重启游戏！\n");
    }

    printf("[+] 安全退出！\n");
    return 0;
}
