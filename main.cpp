#include "paradise_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <math.h>
#include <pthread.h>

#define PI 3.14159265358979323846f

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

struct FVector { float X, Y, Z; };
struct FTransform { float rot[4]; float trans[3]; float scale[3]; };

void getBone(paradise_driver* drv, uintptr_t addr, FTransform& out) {
    uint8_t buf[48] = {0};
    drv->read_fast(addr, buf, 44);
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

int main() {
    system("setenforce 0");
    printf("========================================\n");
    printf(" 训练场人机自动锁头 (PTE UXN 终极实战)\n");
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

    // 1. 全量登记 PTE UXN 断点
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

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 锁头系统已激活！\033[0m\n");
    printf("\033[1;33m[!] 进训练场，靠近人机，按住开火键！\033[0m\n");
    printf("\033[1;33m[!] 子弹会自动拐弯爆头！测试完按 'q' 退出。\033[0m\n");
    printf("--------------------------------------------------\n");

    uint32_t write_indices[2] = {3, 4}; // V3(Pitch), V4(Yaw)
    float write_values[2] = {0.0f, 0.0f};
    uint8_t out_vregs[32][16];

    while (g_Running) {
        // --- 读取世界与自身 ---
        uintptr_t p1 = drv.read_fast<uintptr_t>(base + 0xf1fb900);
        uintptr_t p2 = drv.read_fast<uintptr_t>(p1 + 0x810);
        uintptr_t UWorld = drv.read_fast<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(5000); continue; }

        uintptr_t Uleve = drv.read_fast<uintptr_t>(UWorld + 0x30);
        uintptr_t Arrayaddr = drv.read_fast<uintptr_t>(Uleve + 0xA0);
        int Count = drv.read_fast<int>(Uleve + 0xA8);
        if (Count <= 0 || Count > 2000) { usleep(5000); continue; }

        uintptr_t o1 = drv.read_fast<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = drv.read_fast<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = drv.read_fast<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = drv.read_fast<uintptr_t>(o3 + 0x28c8);
        if (Oneself < 0x10000) { usleep(5000); continue; }

        int MyTeam = drv.read_fast<int>(Oneself + 0x998);
        
        // 相机坐标
        FVector CamPos = {0,0,0};
        uintptr_t cam1 = drv.read_fast<uintptr_t>(Oneself + 0x90);
        if (cam1 > 0x10000) {
            uintptr_t cam2 = drv.read_fast<uintptr_t>(cam1 + 0x490);
            if (cam2 > 0x10000) drv.read_fast(cam2 + 0x480, &CamPos, 12);
        }
        if (CamPos.X == 0 && CamPos.Y == 0) {
            uintptr_t RootComp = drv.read_fast<uintptr_t>(Oneself + 0x208);
            if (RootComp > 0x10000) drv.read_fast(RootComp + 0x1c8, &CamPos, 12);
            CamPos.Z += 150.0f;
        }

        // --- 寻找最近的人机/敌人 ---
        float minDist = 999999.0f;
        FVector bestHead = {0,0,0};
        bool found = false;

        for (int i = 0; i < Count; i++) {
            uintptr_t Objaddr = drv.read_fast<uintptr_t>(Arrayaddr + 8 * i);
            if (Objaddr < 0x10000) continue;
            
            int TeamID = drv.read_fast<int>(Objaddr + 0x998);
            // 过滤自己和队友 (训练场人机 TeamID 可能是 0 或其他，只要不是 MyTeam 且血量 > 0 就行)
            if (TeamID == MyTeam && MyTeam != 0) continue; 
            if (drv.read_fast<float>(Objaddr + 0xe60) <= 0.0f) continue;

            uintptr_t Mesh = drv.read_fast<uintptr_t>(Objaddr + 0x510);
            if (Mesh < 0x10000) continue;
            uintptr_t BoneBase = drv.read_fast<uintptr_t>(Mesh + 0x9a8) + 0x30;
            if (BoneBase < 0x10000) continue;

            FTransform meshT, headT;
            getBone(&drv, Mesh + 0x210, meshT);
            getBone(&drv, BoneBase + 6 * 48, headT); // 6号骨骼通常是头
            
            float c2w[4][4], boneM[4][4], finalM[4][4];
            TransformToMatrix(meshT, c2w);
            TransformToMatrix(headT, boneM);
            MatrixMulti(boneM, c2w, finalM);

            FVector HeadPos = {finalM[3][0], finalM[3][1], finalM[3][2] + 7.0f};
            float dx = HeadPos.X - CamPos.X, dy = HeadPos.Y - CamPos.Y, dz = HeadPos.Z - CamPos.Z;
            float dist = sqrt(dx*dx + dy*dy + dz*dz) * 0.01f;
            
            if (dist < minDist && dist < 300.0f) {
                minDist = dist; bestHead = HeadPos; found = true;
            }
        }

        // --- 计算角度并注入 ---
        if (found) {
            float dx = bestHead.X - CamPos.X;
            float dy = bestHead.Y - CamPos.Y;
            float dz = bestHead.Z - CamPos.Z;
            float d2 = sqrt(dx*dx + dy*dy);
            if (d2 < 0.01f) d2 = 0.01f;

            // 计算真实的 Pitch 和 Yaw
            write_values[0] = atan2(dz, d2) * (180.0f / PI); // Pitch -> V3
            write_values[1] = atan2(dy, dx) * (180.0f / PI); // Yaw -> V4

            // 轨道 A：128位无损注入 V3, V4
            drv.fpr_read_modify_write(pid, 0xFFFFFFFF, 2, write_indices, write_values, out_vregs);

            // 轨道 B：全线程 hwbp_enable
            for (pid_t tid : tids) {
                info.pid = tid;
                info.is_write_fp_regs = true;
                info.fp_reg_count = 2;
                info.fp_reg_indices[0] = 3;
                info.fp_reg_indices[1] = 4;
                memcpy(info.fp_reg_values[0], out_vregs[3], 16);
                memcpy(info.fp_reg_values[1], out_vregs[4], 16);
                drv.hwbp_enable(&info);
            }

            // 轨道 C：喂坐标给黑盒 Tracking API (双保险)
            TRACKING_DATA track = {0};
            track.is_active = true;
            track.bp_addr = shoot_addr;
            track.x = bestHead.X;
            track.y = bestHead.Y;
            track.z = bestHead.Z;
            drv.hwbp_update_tracking(&track);

            printf("\r\033[1;32m[锁定] 距离:%.1fm | Pitch:%.1f Yaw:%.1f   \033[0m", minDist, write_values[0], write_values[1]);
            fflush(stdout);
        } else {
            // 没找到目标，关闭 Tracking
            TRACKING_DATA track = {0};
            track.is_active = false;
            drv.hwbp_update_tracking(&track);
        }
        usleep(2000); // 2ms 极致刷新
    }

    printf("\n[*] 清理断点...\n");
    drv.hwbp_clear();
    printf("[+] 安全退出！\n");
    return 0;
}
