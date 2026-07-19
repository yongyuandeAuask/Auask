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
struct FRotator { float Pitch, Yaw, Roll; };
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
    printf(" 真·相机链路 白盒绝对角度锁头\n");
    printf=" (使用 0x4b18 和 0x548 真实链路)\n");
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
    for (pid_t tid : tids) { info.pid = tid; drv.hwbp_add(&info); }

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 真实相机链路白盒锁头已激活！\033[0m\n");
    printf("\033[1;33m[!] 进训练场，靠近人机，把准星瞄准【旁边的空气】，开枪！\033[0m\n");
    printf("--------------------------------------------------\n");

    uint32_t write_indices[2] = {3, 4}; 
    uint8_t out_vregs[32][16];

    while (g_Running) {
        // 1. 获取 Oneself
        uintptr_t p1 = drv.read_fast<uintptr_t>(base + 0xf1fb900);
        uintptr_t p2 = drv.read_fast<uintptr_t>(p1 + 0x810);
        uintptr_t UWorld = drv.read_fast<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(5000); continue; }

        uintptr_t o1 = drv.read_fast<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = drv.read_fast<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = drv.read_fast<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = drv.read_fast<uintptr_t>(o3 + 0x28c8);
        if (Oneself < 0x10000) { usleep(5000); continue; }

        // 2. 【核心】使用真实链路获取相机数据
        uintptr_t PlayerController = drv.read_fast<uintptr_t>(Oneself + 0x4b18);
        uintptr_t CameraPtr = drv.read_fast<uintptr_t>(PlayerController + 0x548);
        
        FVector CamPos = {0,0,0};
        FRotator CamRot = {0,0,0};
        
        if (CameraPtr > 0x10000) {
            // 动态扫描 CameraPtr 前 64 字节，寻找合法的 Rotation (Pitch -90~90)
            for (int off = 0; off <= 64; off += 4) {
                FRotator test_rot = drv.read_fast<FRotator>(CameraPtr + off);
                if (test_rot.Pitch >= -90.0f && test_rot.Pitch <= 90.0f && 
                    test_rot.Yaw >= -180.0f && test_rot.Yaw <= 180.0f && 
                    test_rot.Yaw != 0.0f) {
                    CamRot = test_rot;
                    // Location 通常在 Rotation 前面 12 字节 (或 16字节对齐)
                    CamPos = drv.read_fast<FVector>(CameraPtr + off - 12);
                    if (CamPos.X == 0 && CamPos.Y == 0) {
                        CamPos = drv.read_fast<FVector>(CameraPtr + off - 16);
                    }
                    break;
                }
            }
        }

        if (CamPos.X == 0 && CamPos.Y == 0) { usleep(5000); continue; }

        // 3. 寻找最近的人机
        uintptr_t Uleve = drv.read_fast<uintptr_t>(UWorld + 0x30);
        uintptr_t Arrayaddr = drv.read_fast<uintptr_t>(Uleve + 0xA0);
        int Count = drv.read_fast<int>(Uleve + 0xA8);

        float minDist = 999999.0f;
        FVector bestHead = {0,0,0};
        bool found = false;

        for (int i = 0; i < Count && i < 100; i++) {
            uintptr_t Objaddr = drv.read_fast<uintptr_t>(Arrayaddr + 8 * i);
            if (Objaddr < 0x10000 || Objaddr == Oneself) continue;
            float hp = drv.read_fast<float>(Objaddr + 0xe60);
            if (hp <= 0.0f || hp > 200.0f) continue;

            uintptr_t Mesh = drv.read_fast<uintptr_t>(Objaddr + 0x510);
            if (Mesh < 0x10000) continue;
            uintptr_t BoneBase = drv.read_fast<uintptr_t>(Mesh + 0x9a8) + 0x30;
            if (BoneBase < 0x10000) continue;

            FTransform meshT, headT;
            getBone(&drv, Mesh + 0x210, meshT);
            getBone(&drv, BoneBase + 6 * 48, headT);
            
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

        // 4. 计算绝对角度并白盒注入
        if (found) {
            float dx = bestHead.X - CamPos.X;
            float dy = bestHead.Y - CamPos.Y;
            float dz = bestHead.Z - CamPos.Z;
            float d2 = sqrt(dx*dx + dy*dy);
            if (d2 < 0.01f) d2 = 0.01f;

            float aim_pitch = atan2(dz, d2) * (180.0f / PI);
            float aim_yaw = atan2(dy, dx) * (180.0f / PI);
            
            float write_values[2] = {aim_pitch, aim_yaw};
            drv.fpr_read_modify_write(pid, 0xFFFFFFFF, 2, write_indices, write_values, out_vregs);

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

            printf("\r\033[1;32m[锁定] 距:%.1fm | 相机P:%.1f Y:%.1f | 目标P:%.1f Y:%.1f   \033[0m", 
                   minDist, CamRot.Pitch, CamRot.Yaw, aim_pitch, aim_yaw);
            fflush(stdout);
        }
        usleep(5000);
    }

    drv.hwbp_clear();
    printf("\n[+] 安全退出！\n");
    return 0;
}
