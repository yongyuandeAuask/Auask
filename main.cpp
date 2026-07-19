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
    printf(" 纯净版 Tracking API 终极验证\n");
    printf(" (彻底放弃寄存器，纯靠驱动黑盒算力)\n");
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

    // 1. 登记断点 (仅登记，不启用任何寄存器修改)
    HW_BP_INFO info = {0};
    info.pid = pid;
    info.addr = shoot_addr;
    info.type = HW_BP_TYPE_X;
    info.len = 4;
    drv.hwbp_add(&info);
    printf("[+] 已登记 PTE UXN 断点 (纯占坑)\n");

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] Tracking 追踪系统已激活！\033[0m\n");
    printf("\033[1;33m[!] 进训练场，靠近人机，把准星瞄准【旁边的空气】，开枪！\033[0m\n");
    printf("\033[1;33m[!] 如果子弹自动拐弯爆头，说明驱动黑盒算力完美！\033[0m\n");
    printf("--------------------------------------------------\n");

    while (g_Running) {
        // --- 读取世界与自身 ---
        uintptr_t p1 = drv.read_fast<uintptr_t>(base + 0xf1fb900);
        uintptr_t p2 = drv.read_fast<uintptr_t>(p1 + 0x810);
        uintptr_t UWorld = drv.read_fast<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(5000); continue; }

        uintptr_t Uleve = drv.read_fast<uintptr_t>(UWorld + 0x30);
        uintptr_t Arrayaddr = drv.read_fast<uintptr_t>(Uleve + 0xA0);
        int Count = drv.read_fast<int>(Uleve + 0xA8);

        uintptr_t o1 = drv.read_fast<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = drv.read_fast<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = drv.read_fast<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = drv.read_fast<uintptr_t>(o3 + 0x28c8);
        if (Oneself < 0x10000) { usleep(5000); continue; }

        int MyTeam = drv.read_fast<int>(Oneself + 0x998);
        
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

        // --- 寻找最近的人机 ---
        float minDist = 999999.0f;
        FVector bestHead = {0,0,0};
        bool found = false;

        for (int i = 0; i < Count && i < 200; i++) {
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

        // --- 【核心】只使用 hwbp_update_tracking 喂坐标 ---
        TRACKING_DATA track = {0};
        track.bp_addr = shoot_addr;
        
        if (found) {
            track.is_active = true;
            track.x = bestHead.X;
            track.y = bestHead.Y;
            track.z = bestHead.Z;
            
            // 调用驱动的黑盒追踪 API
            drv.hwbp_update_tracking(&track);
            
            printf("\r\033[1;32m[追踪] 距离:%.1fm | 喂入坐标: X:%.1f Y:%.1f Z:%.1f   \033[0m", 
                   minDist, bestHead.X, bestHead.Y, bestHead.Z);
            fflush(stdout);
        } else {
            track.is_active = false;
            drv.hwbp_update_tracking(&track);
        }

        usleep(5000); // 5ms 刷新
    }

    drv.hwbp_clear();
    printf("\n[+] 安全退出！\n");
    return 0;
}
