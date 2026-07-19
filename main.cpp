#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>

#define PI 3.14159265358979323846f

// 【核心开关】设为 true 测试射向天空，设为 false 开启真实锁头
bool TEST_SKY = true; 

// ================= 全局状态与多线程退出机制 =================
volatile bool g_Running = true;
paradise_driver* g_drv = nullptr;
pid_t g_pid = 0;
uintptr_t g_base = 0;
uintptr_t g_shoot_addr = 0;
std::vector<pid_t> g_tids;

// 【优雅退出】终端输入监听线程 (彻底告别文件哨兵)
void* InputListenerThread(void* arg) {
    char buf[64];
    while (g_Running) {
        if (fgets(buf, sizeof(buf), stdin) != NULL) {
            if (buf[0] == 'q' || buf[0] == 'Q' || buf[0] == 'e' || buf[0] == 'E') {
                printf("\n\033[1;31m[!] 接收到退出指令，正在准备安全清理...\033[0m\n");
                g_Running = false;
                break;
            }
        }
    }
    return NULL;
}

// 获取进程的所有线程 ID (TID)
std::vector<pid_t> get_all_tids(pid_t pid) {
    std::vector<pid_t> tids;
    char path[256];
    sprintf(path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (!dir) return tids;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            pid_t tid = atoi(entry->d_name);
            if (tid > 0) tids.push_back(tid);
        }
    }
    closedir(dir);
    return tids;
}

// ================= 骨骼矩阵运算 (hook.h 链路) =================
struct FVector { float X, Y, Z; };
struct FTransform { float rot[4]; float trans[3]; float scale[3]; };

void getBone(uintptr_t addr, FTransform& out) {
    uint8_t buf[48] = {0};
    g_drv->read(addr, buf, 44);
    memcpy(out.rot, buf, 16);
    memcpy(out.trans, buf + 16, 12);
    memcpy(out.scale, buf + 28, 12);
}

void TransformToMatrix(const FTransform& T, float M[4][4]) {
    float x=T.rot[0], y=T.rot[1], z=T.rot[2], w=T.rot[3];
    float x2=x+x, y2=y+y, z2=z+z;
    float xx=x*x2, xy=x*y2, xz=x*z2, yy=y*y2, yz=y*z2, zz=z*z2;
    float wx=w*x2, wy=w*y2, wz=w*z2;
    float sx=T.scale[0], sy=T.scale[1], sz=T.scale[2];
    M[0][0]=(1.0f-(yy+zz))*sx; M[0][1]=(xy-wz)*sy; M[0][2]=(xz+wy)*sz; M[0][3]=0;
    M[1][0]=(xy+wz)*sx; M[1][1]=(1.0f-(xx+zz))*sy; M[1][2]=(yz-wx)*sz; M[1][3]=0;
    M[2][0]=(xz-wy)*sx; M[2][1]=(yz+wx)*sy; M[2][2]=(1.0f-(xx+yy))*sz; M[2][3]=0;
    M[3][0]=T.trans[0]; M[3][1]=T.trans[1]; M[3][2]=T.trans[2]; M[3][3]=1;
}

void MatrixMulti(const float A[4][4], const float B[4][4], float C[4][4]) {
    for(int i=0; i<4; i++) for(int j=0; j<4; j++)
        C[i][j] = A[i][0]*B[0][j] + A[i][1]*B[1][j] + A[i][2]*B[2][j] + A[i][3]*B[3][j];
}

// ================= 主函数 =================
int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf("  Paradise 终极追踪 (终端交互安全退出)\n");
    printf("========================================\n");

    g_drv = new paradise_driver();
    g_pid = g_drv->get_pid("com.tencent.ig");
    if (g_pid <= 0) g_pid = g_drv->get_pid("com.rekoo.pubgm");
    if (g_pid <= 0) g_pid = g_drv->get_pid("com.vng.pubgmobile");
    if (g_pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    printf("[+] PID: %d\n", g_pid);
    g_drv->initialize(g_pid);

    g_base = g_drv->get_module_base("libUE4.so");
    if (g_base == 0) { printf("[-] 获取基址失败\n"); return 1; }
    g_shoot_addr = g_base + 0x6DFE100;

    // 1. 全线程挂载断点
    g_tids = get_all_tids(g_pid);
    printf("[+] 发现 %zu 个线程，开始全线程挂载...\n", g_tids.size());
    int success_count = 0;
    for (pid_t tid : g_tids) {
        HW_BP_INFO info = {0};
        info.pid = tid;
        info.addr = g_shoot_addr;
        info.type = HW_BP_TYPE_X;
        info.len = 4;
        if (g_drv->hwbp_add(&info)) success_count++;
    }
    printf("[+] 成功在 %d 个线程上挂载了断点！\n", success_count);

    // 2. 启动终端输入监听线程
    pthread_t input_tid;
    pthread_create(&input_tid, NULL, InputListenerThread, NULL);
    
    printf("\n\033[1;42;37m[*] 追踪系统已激活！当前模式: %s\033[0m\n", TEST_SKY ? "射向天空" : "自动锁头");
    printf("\033[1;33m[!] 进游戏开枪测试。测试完成后，请在此终端输入 'q' 并回车，即可安全退出！\033[0m\n\n");

    // 3. 核心追踪循环
    while (g_Running) {
        // --- 读取世界基址 ---
        uintptr_t p1 = g_drv->read<uintptr_t>(g_base + 0xf1fb900);
        uintptr_t p2 = g_drv->read<uintptr_t>(p1 + 0x810);
        uintptr_t UWorld = g_drv->read<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(10000); continue; }

        uintptr_t Uleve = g_drv->read<uintptr_t>(UWorld + 0x30);
        uintptr_t Arrayaddr = g_drv->read<uintptr_t>(Uleve + 0xA0);
        int Count = g_drv->read<int>(Uleve + 0xA8);
        if (Count <= 0 || Count > 2000) { usleep(10000); continue; }

        // --- 读取自身与相机 ---
        uintptr_t o1 = g_drv->read<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = g_drv->read<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = g_drv->read<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = g_drv->read<uintptr_t>(o3 + 0x28c8);
        if (Oneself < 0x10000) { usleep(10000); continue; }

        int MyTeam = g_drv->read<int>(Oneself + 0x998);
        FVector MyPos = {0,0,0};
        uintptr_t RootComp = g_drv->read<uintptr_t>(Oneself + 0x208);
        if (RootComp > 0x10000) g_drv->read(RootComp + 0x1c8, &MyPos, 12);

        FVector CamPos = {MyPos.X, MyPos.Y, MyPos.Z + 150.0f};
        uintptr_t cam1 = g_drv->read<uintptr_t>(Oneself + 0x90);
        if (cam1 > 0x10000) {
            uintptr_t cam2 = g_drv->read<uintptr_t>(cam1 + 0x490);
            if (cam2 > 0x10000) g_drv->read(cam2 + 0x480, &CamPos, 12);
        }

        // --- 寻找最近敌人 ---
        float minDist = 999999.0f;
        FVector bestHead = {0,0,0};
        bool found = false;

        for (int i = 0; i < Count; i++) {
            uintptr_t Objaddr = g_drv->read<uintptr_t>(Arrayaddr + 8 * i);
            if (Objaddr < 0x10000) continue;
            if (g_drv->read<float>(Objaddr + 0x2b78) != 479.5f) continue;
            int TeamID = g_drv->read<int>(Objaddr + 0x998);
            if (TeamID == MyTeam || TeamID < 1) continue;
            if (g_drv->read<float>(Objaddr + 0xe60) <= 0.0f) continue;

            uintptr_t Mesh = g_drv->read<uintptr_t>(Objaddr + 0x510);
            if (Mesh < 0x10000) continue;
            uintptr_t BoneBase = g_drv->read<uintptr_t>(Mesh + 0x9a8) + 0x30;
            if (BoneBase < 0x10000) continue;

            FTransform meshT, headT;
            getBone(Mesh + 0x210, meshT);
            getBone(BoneBase + 6 * 48, headT);
            
            float c2w[4][4], boneM[4][4], finalM[4][4];
            TransformToMatrix(meshT, c2w);
            TransformToMatrix(headT, boneM);
            MatrixMulti(boneM, c2w, finalM);

            FVector HeadPos = {finalM[3][0], finalM[3][1], finalM[3][2] + 7.0f};
            float dx = HeadPos.X - CamPos.X, dy = HeadPos.Y - CamPos.Y, dz = HeadPos.Z - CamPos.Z;
            float dist = sqrt(dx*dx + dy*dy + dz*dz) * 0.01f;
            
            if (dist < minDist && dist < 400.0f) {
                minDist = dist; bestHead = HeadPos; found = true;
            }
        }

        // --- 计算角度并全线程注入 ---
        float aimPitch = 0.0f, aimYaw = 0.0f;
        if (found) {
            float dx = bestHead.X - CamPos.X, dy = bestHead.Y - CamPos.Y, dz = bestHead.Z - CamPos.Z;
            float d2 = sqrt(dx*dx + dy*dy);
            if (d2 < 0.01f) d2 = 0.01f;
            aimYaw = atan2(dy, dx) * (180.0f / PI);
            aimPitch = atan2(dz, d2) * (180.0f / PI);
        }

        // 强制射天模式
        if (TEST_SKY) aimPitch = -89.0f;

        // 饱和式注入准备
        uint32_t pitch_hex, yaw_hex;
        memcpy(&pitch_hex, &aimPitch, 4);
        memcpy(&yaw_hex, &aimYaw, 4);

        for (pid_t tid : g_tids) {
            HW_BP_INFO info = {0};
            info.pid = tid;
            info.addr = g_shoot_addr;
            info.type = HW_BP_TYPE_X;
            info.len = 4;
            
            // 覆盖 V0~V3
            info.is_write_fp_regs = true;
            info.fp_reg_count = 4;
            for(int i=0; i<4; i++) {
                info.fp_reg_indices[i] = i;
                memcpy(&info.fp_reg_values[i][0], &aimPitch, 4);
            }
            
            // 覆盖 X0~X2 (防指针传参)
            info.is_write_gp_regs = true;
            info.gp_reg_count = 3;
            info.gp_reg_indices[0] = 0; info.gp_reg_values[0] = pitch_hex;
            info.gp_reg_indices[1] = 1; info.gp_reg_values[1] = yaw_hex;
            info.gp_reg_indices[2] = 2; info.gp_reg_values[2] = pitch_hex;

            g_drv->hwbp_enable(&info);
        }
        
        if (found) {
            printf("\r\033[1;32m[追踪] 距离:%.1fm | Pitch:%.1f Yaw:%.1f | 输入 'q' 退出   \033[0m", minDist, aimPitch, aimYaw);
            fflush(stdout);
        }
        usleep(5000); // 5ms 轮询
    }

    // 4. 完美收尾：自动清理断点
    printf("\n\033[1;33m[*] 正在清除所有内核断点...\033[0m\n");
    g_drv->hwbp_clear();
    printf("\033[1;32m[+] 断点已完全清理，后台零残留！进程安全结束。\033[0m\n");
    
    delete g_drv;
    return 0;
}
