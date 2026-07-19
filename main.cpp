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
    printf(" 极简防卡顿·开火标定器 (兼容人机)\n");
    printf("========================================\n");

    paradise_driver drv;
    pid_t pid = drv.get_pid("com.tencent.ig");
    if (pid <= 0) pid = drv.get_pid("com.rekoo.pubgm");
    if (pid <= 0) pid = drv.get_pid("com.vng.pubgmobile");
    if (pid <= 0) { printf("[-] 未找到游戏\n"); return 1; }
    drv.initialize(pid);

    uintptr_t base = drv.get_module_base("libUE4.so");
    if (base == 0) { printf("[-] 获取基址失败\n"); return 1; }

    pthread_t tid_input;
    pthread_create(&tid_input, NULL, InputThread, NULL);

    printf("\n\033[1;42;37m[*] 标定器已启动！(后台极简轮询，绝不卡顿)\033[0m\n");
    printf("\033[1;33m[!] 请进训练场，手动瞄准【人机】头部，开枪！\033[0m\n");
    printf("\033[1;33m[!] 开完枪看终端输出的【标定报告】。按 'q' 退出。\033[0m\n");
    printf("--------------------------------------------------\n");
    printf("等待开火中...\n");

    int last_firing_state = 0;
    int shot_count = 0;
    uint64_t loop_count = 0;

    while (g_Running) {
        // 【极简轮询】只读最基础的指针，耗时 < 0.05ms
        uintptr_t p1 = drv.read_fast<uintptr_t>(base + 0xf1fb900);
        uintptr_t p2 = drv.read_fast<uintptr_t>(p1 + 0x810);
        uintptr_t UWorld = drv.read_fast<uintptr_t>(p2 + 0x78);
        if (UWorld < 0x10000) { usleep(10000); continue; }

        uintptr_t o1 = drv.read_fast<uintptr_t>(UWorld + 0x38);
        uintptr_t o2 = drv.read_fast<uintptr_t>(o1 + 0x78);
        uintptr_t o3 = drv.read_fast<uintptr_t>(o2 + 0x30);
        uintptr_t Oneself = drv.read_fast<uintptr_t>(o3 + 0x28c8);
        if (Oneself < 0x10000) { usleep(10000); continue; }

        int is_firing = drv.read_fast<int>(Oneself + 0x1830);

        // 每 2 秒打印一次心跳，证明程序没死
        if (++loop_count % 2000 == 0) {
            printf("\r[心跳] 程序正常运行中，已循环 %lu 次，等待开火...   ", loop_count);
            fflush(stdout);
        }

        // 【核心】检测到开火上升沿 (从 0 变 1)
        if (is_firing == 1 && last_firing_state == 0) {
            shot_count++;
            
            // 1. 瞬间冻结 V3/V4 (只读 V3 和 V4，极快)
            uint8_t vregs[32][16] = {0};
            uint32_t fpsr, fpcr;
            uint32_t mask = (1 << 3) | (1 << 4); 
            drv.fpr_read(pid, mask, vregs, &fpsr, &fpcr);
            
            float real_pitch, real_yaw;
            memcpy(&real_pitch, &vregs[3][0], sizeof(float));
            memcpy(&real_yaw, &vregs[4][0], sizeof(float));

            // 2. 开火后，再去慢条斯理地读取相机和计算骨骼
            uintptr_t PlayerController = drv.read_fast<uintptr_t>(Oneself + 0x2A0); 
            if (PlayerController < 0x10000) PlayerController = Oneself;
            FRotator cam_rot = drv.read_fast<FRotator>(PlayerController + 0x3A8);
            if (cam_rot.Pitch < -90.0f || cam_rot.Pitch > 90.0f) {
                cam_rot = drv.read_fast<FRotator>(PlayerController + 0x410);
            }

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

            uintptr_t Uleve = drv.read_fast<uintptr_t>(UWorld + 0x30);
            uintptr_t Arrayaddr = drv.read_fast<uintptr_t>(Uleve + 0xA0);
            int Count = drv.read_fast<int>(Uleve + 0xA8);

            // 【兼容人机】放宽过滤条件：只要血量 > 0，且不是自己，就是目标！
            float minDist = 999999.0f;
            FVector bestHead = {0,0,0};
            bool found = false;

            for (int i = 0; i < Count && i < 200; i++) { 
                uintptr_t Objaddr = drv.read_fast<uintptr_t>(Arrayaddr + 8 * i);
                if (Objaddr < 0x10000 || Objaddr == Oneself) continue;
                
                // 读取血量 (如果人机血量偏移不同，这里可能读不到，但通常 0xe60 是通用的)
                float hp = drv.read_fast<float>(Objaddr + 0xe60);
                if (hp <= 0.0f || hp > 200.0f) continue; // 过滤掉非角色实体

                uintptr_t Mesh = drv.read_fast<uintptr_t>(Objaddr + 0x510);
                if (Mesh < 0x10000) continue;
                uintptr_t BoneBase = drv.read_fast<uintptr_t>(Mesh + 0x9a8) + 0x30;
                if (BoneBase < 0x10000) continue;

                FTransform meshT, headT;
                getBone(&drv, Mesh + 0x210, meshT);
                getBone(&drv, BoneBase + 6 * 48, headT); // 6号骨骼是头
                
                float c2w[4][4], boneM[4][4], finalM[4][4];
                TransformToMatrix(meshT, c2w);
                TransformToMatrix(headT, boneM);
                MatrixMulti(boneM, c2w, finalM);

                FVector HeadPos = {finalM[3][0], finalM[3][1], finalM[3][2] + 7.0f};
                float dx = HeadPos.X - CamPos.X, dy = HeadPos.Y - CamPos.Y, dz = HeadPos.Z - CamPos.Z;
                float dist = sqrt(dx*dx + dy*dy + dz*dz) * 0.01f;
                
                if (dist < minDist && dist < 500.0f) {
                    minDist = dist; bestHead = HeadPos; found = true;
                }
            }

            float calc_pitch = 0.0f, calc_yaw = 0.0f;
            if (found) {
                float dx = bestHead.X - CamPos.X;
                float dy = bestHead.Y - CamPos.Y;
                float dz = bestHead.Z - CamPos.Z;
                float d2 = sqrt(dx*dx + dy*dy);
                if (d2 < 0.01f) d2 = 0.01f;
                calc_pitch = atan2(dz, d2) * (180.0f / PI);
                calc_yaw = atan2(dy, dx) * (180.0f / PI);
            }

            // 打印报告
            printf("\n\n\033[1;41;37m================ 第 %d 枪 标定报告 ================\033[0m\n", shot_count);
            if(!found) printf("  \033[31m[警告] 未在附近找到存活的人机/玩家，理论角度为0\033[0m\n");
            
            printf("  当前相机绝对角度: Pitch=%.2f, Yaw=%.2f\n", cam_rot.Pitch, cam_rot.Yaw);
            printf("  \033[1;33m[程序理论计算] (世界绝对角度): Pitch=%.2f, Yaw=%.2f\033[0m\n", calc_pitch, calc_yaw);
            printf("  \033[1;32m[引擎真实使用] (开枪瞬间V3/V4): Pitch=%.2f, Yaw=%.2f\033[0m\n", real_pitch, real_yaw);
            
            printf("  \033[1;36m--- 误差分析 ---\033[0m\n");
            printf("  Pitch 差值 (真实 - 理论): %.2f\n", real_pitch - calc_pitch);
            printf("  Yaw 差值 (真实 - 理论): %.2f\n", real_yaw - calc_yaw);
            printf("  Yaw 差值 (真实 - 相机Yaw): %.2f\n", real_yaw - cam_rot.Yaw);
            printf("\033[1;41;37m==================================================\033[0m\n\n");
            printf("等待下一次开火...\n");
        }
        
        last_firing_state = is_firing;
        usleep(1000); // 1ms 极速轮询
    }

    printf("\n[+] 安全退出！\n");
    return 0;
}
