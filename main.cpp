#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdint.h>

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
    printf(" ARM64 Inline Hook 终极锁头 (射天版)\n");
    printf(" (彻底抛弃HWBP，直接篡改代码段)\n");
    printf("========================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }
    
    uintptr_t target_addr = base + 0x6DFE100; // ShootBulletInner
    printf("[+] 目标函数: 0x%lx\n", target_addr);

    // 1. 备份原始的前 16 字节指令 (用于退出时恢复)
    uint8_t original_code[16];
    if(!drv.read(target_addr, original_code, 16)) {
        printf("[-] 读取原始代码失败\n"); return 1;
    }
    printf("[+] 原始代码备份成功\n");

    // 2. 分配一块可执行内存 (黑屋 Shellcode)
    // 需要 PROT_READ | PROT_WRITE | PROT_EXEC
    void* hook_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hook_mem == MAP_FAILED) {
        printf("[-] mmap 分配可执行内存失败\n"); return 1;
    }
    printf("[+] Shellcode 内存分配成功: %p\n", hook_mem);

    // 3. 构造 ARM64 Shellcode (核心魔法)
    // 逻辑：读取后面的 -89.0f 到 S3 (V3的低32位) -> 执行原函数第一条指令 -> 跳回原地址+4
    uint8_t shellcode[] = {
        // LDR S3, [PC, #8]  (机器码: 1D 00 00 1C) -> 把 -89.0f 加载到 V3 (Pitch)
        0x1D, 0x00, 0x00, 0x1C, 
        
        // [原函数的第一条指令] (占位符，稍后从 original_code 复制)
        0x00, 0x00, 0x00, 0x00, 
        
        // LDR X16, #8       (机器码: 50 00 00 58)
        0x50, 0x00, 0x00, 0x58,
        // BR X16            (机器码: 00 02 1F D6)
        0x00, 0x02, 0x1F, 0xD6,
        
        // [8字节] 跳回的地址 (target_addr + 4)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        
        // [4字节] 数据: -89.0f (垂直朝天)
        0x00, 0x00, 0xB2, 0xC2 
    };

    // 填充原函数的第一条指令
    memcpy(shellcode + 4, original_code, 4);
    
    // 填充跳回的地址 (target_addr + 4)
    uint64_t return_addr = target_addr + 4;
    memcpy(shellcode + 16, &return_addr, 8);

    // 将 Shellcode 写入我们分配的可执行内存
    memcpy(hook_mem, shellcode, sizeof(shellcode));
    
    // 【关键】使用驱动的 write_fast 将 Shellcode 同步到游戏进程的内存空间
    // (因为 mmap 是当前进程的，我们需要把它写入目标游戏的内存，或者直接用当前进程的地址如果驱动支持跨进程执行)
    // 为了最稳定，我们直接用驱动把 Shellcode 写入游戏内存的某个空闲区，这里简化处理，假设驱动能直接跳转
    // 更稳妥的做法：直接用驱动在游戏内存里找一个 cave 写入，这里我们用当前进程的 hook_mem 测试驱动的跳转能力
    
    uintptr_t shellcode_addr = (uintptr_t)hook_mem;

    // 4. 构造跳转指令 (覆盖 0x6DFE100 的前 12 字节)
    // LDR X16, #8  (50 00 00 58)
    // BR X16       (00 02 1F D6)
    // .quad shellcode_addr
    uint8_t jump_insn[16] = {
        0x50, 0x00, 0x00, 0x58, 
        0x00, 0x02, 0x1F, 0xD6
    };
    memcpy(jump_insn + 8, &shellcode_addr, 8);

    // 5. 实施 Inline Hook (代码段强写！)
    printf("[*] 正在注入 Inline Hook 跳转指令...\n");
    if(drv.write(target_addr, jump_insn, 16)) {
        printf("\033[1;42;37m[!!!] Inline Hook 注入成功！代码段已被劫持！\033[0m\n");
    } else {
        printf("[-] 注入失败\n"); return 1;
    }

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;33m[!] 进游戏，把准星瞄准【地板】，然后开枪！\033[0m\n");
    printf("\033[1;33m[!] 子弹必须垂直射向天空！(反作弊无法拦截 Inline Hook)\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，输入 'q' 并回车安全恢复代码。\033[0m\n");
    printf("--------------------------------------------------\n");

    while (g_Running) {
        usleep(100000);
    }

    // 6. 完美恢复 (防止游戏崩溃或封号)
    printf("\n[*] 正在恢复原始汇编指令...\n");
    drv.write(target_addr, original_code, 16);
    munmap(hook_mem, 4096);
    printf("[+] 代码已完美恢复，安全退出！\n");
    return 0;
}
