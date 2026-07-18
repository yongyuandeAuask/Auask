#include "paradise_api.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
    printf("========================================\n");
    printf("  Paradise 驱动 ELF验证 + 读写测试\n");
    printf("========================================\n");

    // 1. 实例化 (构造函数内部自动连接驱动节点)
    paradise_driver drv;

    // 2. 获取 PID
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) pid = drv.get_pid("com.pubg.krmobile");
    if (pid <= 0) pid = drv.get_pid("com.pubg.imobile");
    if (pid <= 0) {
        printf("[-] 未找到游戏进程\n");
        return 1;
    }
    printf("[+] PID: %d\n", pid);
    drv.initialize(pid);

    // 3. 获取基址
    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) {
        printf("[-] 获取 libUE4.so 基址失败\n");
        return 1;
    }
    printf("[+] libUE4.so: 0x%lx\n", base);

    // 4. 验证 ELF 头
    unsigned char elf[4] = {0};
    if (drv.read(base, elf, 4)) {
        if (elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F') {
            printf("[+] ELF 头验证通过 (\\x7fELF)\n");
        } else {
            printf("[-] ELF 头不匹配: %02x %02x %02x %02x\n", elf[0], elf[1], elf[2], elf[3]);
        }
    } else {
        printf("[-] 读取失败\n");
        return 1;
    }

    // 5. 读取 ShootBulletInner (0x6DFE100) 前 16 字节
    uintptr_t shoot = base + 0x6DFE100;
    unsigned char code[16] = {0};
    printf("\n[*] ShootBulletInner 地址: 0x%lx\n", shoot);

    if (drv.read(shoot, code, 16)) {
        printf("[+] 读取成功，机器码:\n    ");
        for (int i = 0; i < 16; i++) {
            printf("%02x ", code[i]);
            if ((i + 1) % 4 == 0) printf(" ");
        }
        printf("\n");
    } else {
        printf("[-] 读取 ShootBulletInner 失败\n");
        return 1;
    }

    // 6. 尝试写回原值 (验证代码段写入能力)
    printf("[*] 尝试写回原值...\n");
    if (drv.write(shoot, code, 16)) {
        printf("[+] 写入成功 (驱动支持代码段强写)\n");
    } else {
        printf("[!] 写入失败 (代码段只读，正常现象，不影响断点追踪)\n");
    }

    printf("\n[+] 测试完成\n");
    return 0;
}