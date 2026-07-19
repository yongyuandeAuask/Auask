#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

volatile bool g_Running = true;

// 终端输入监听 (按 q 安全退出)
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
    printf(" V3 寄存器 128位无损微创注入测试\n");
    printf(" (保留高96位，彻底杜绝引擎NaN闪退)\n");
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

    // 1. 【核心】读取 V3 完整的 128 位 (16字节) 原始快照
    uint8_t vregs[32][16] = {0};
    uint32_t fpsr, fpcr;
    uint32_t mask = (1 << 3); // 只读 V3
    
    if (!drv.fpr_read(pid, mask, vregs, &fpsr, &fpcr)) {
        printf("[-] fpr_read 失败\n"); return 1;
    }
    printf("[+] 成功读取 V3 完整 128 位快照\n");

    // 2. 【微创手术】只修改低 32 位 (S3 = Pitch)，高 96 位保持原样！
    float pitch_sky = -89.0f; 
    memcpy(&vregs[3][0], &pitch_sky, sizeof(float)); 

    // 3. 配置硬件断点 (只给主线程挂，防卡死)
    HW_BP_INFO info = {0};
    info.pid = pid; 
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    
    info.is_write_fp_regs = true;
    info.fp_reg_count = 1;
    info.fp_reg_indices[0] = 3; // 锁定 V3
    
    // 将完整的 16 字节 (包含保留的高 96 位) 传给内核
    memcpy(info.fp_reg_values[0], vregs[3], 16);

    // 4. 注册并激活 (只调用一次！)
    if (!drv.hwbp_add(&info)) {
        printf("[-] hwbp_add 失败\n"); return 1;
    }
    if (!drv.hwbp_enable(&info)) {
        printf("[-] hwbp_enable 失败\n"); return 1;
    }
    
    printf("\033[1;42;37m[+] 128位无损篡改已激活！V3(Pitch) 已锁定为 -89.0f\033[0m\n");

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;33m[!] 进游戏，把准星瞄准【地板】，然后开一枪！\033[0m\n");
    printf("\033[1;33m[!] 如果子弹射向天空且【不闪退】，说明我们彻底征服了底层！\033[0m\n");
    printf("\033[1;33m[!] 测试完成后，输入 'q' 并回车安全退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    while (g_Running) {
        usleep(100000); 
    }

    printf("\n[*] 正在清理断点...\n");
    drv.hwbp_clear();
    printf("[+] 清理完毕，安全退出！\n");
    return 0;
}
