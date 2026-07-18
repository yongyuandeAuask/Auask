#ifndef PARADISE_API_H
#define PARADISE_API_H

#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

#define PARADISE_GYRO_MASK_GYRO (1u << 0)
#define PARADISE_GYRO_MASK_UNCAL (1u << 1)
#define PARADISE_GYRO_MASK_ALL (PARADISE_GYRO_MASK_GYRO | PARADISE_GYRO_MASK_UNCAL)

#define HW_BP_TYPE_R  1
#define HW_BP_TYPE_W  2
#define HW_BP_TYPE_RW 3
#define HW_BP_TYPE_X  4

#define MAX_MODIFY_REGS 10
#define MAX_HIT_RECORDS 16

typedef struct paradise_tracking_data {
    bool is_active;
    uintptr_t bp_addr;
    float x;
    float y;
    float z;
} TRACKING_DATA;

typedef struct paradise_hw_bp_info {
    pid_t pid;
    uintptr_t addr;
    int type;
    int len;
    bool is_write_gp_regs;
    int gp_reg_count;
    int gp_reg_indices[MAX_MODIFY_REGS];
    uint64_t gp_reg_values[MAX_MODIFY_REGS];
    bool is_write_fp_regs;
    int fp_reg_count;
    int fp_reg_indices[MAX_MODIFY_REGS];
    uint64_t fp_reg_values[MAX_MODIFY_REGS][2];
} HW_BP_INFO;

typedef struct paradise_regs_info {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
} REGS_INFO;

typedef struct paradise_hwbp_hit_item {
    pid_t task_id;
    uintptr_t hit_addr;
    uint64_t hit_time;
    REGS_INFO regs_info;
} HWBP_HIT_ITEM;

typedef struct paradise_hwbp_hit_args {
    pid_t pid;
    uintptr_t addr;
    HWBP_HIT_ITEM *out_buf;
    int out_len;
    int real_count;
} HWBP_HIT_ARGS;

class paradise_driver {
private:
    pid_t pid;
    int fd;

    int install_driver_fd();
    void ensure_connected();

public:
    // 构造时连接 Paradise 驱动
    paradise_driver();
    
    // 构析方法
    ~paradise_driver();

    // 初始化目标 pid，读写前务必调用一次
    void initialize(pid_t target_pid);
    
    // 获取进程pid，传入进程名称，从内核层安全获取pid
    pid_t get_pid(const char *name);
    
    // 获取模块基址，传入模块名，从内核层安全获取模块基址
    uintptr_t get_module_base(const char *name);

    // 获取模块映射范围 [base, end)，end 为最高一段 VMA 的 vm_end，便于一次覆盖整个 so，如 libc 多段
    bool get_module_range(const char *name, uintptr_t *base_out, uintptr_t *end_out);

    // 获取模块结束地址，传入模块名，从内核层安全获取模块结束地址
    uintptr_t get_module_end(const char *name);

    /*
    用法示例：
        uintptr_t lo, hi;
        if (get_module_range("libc.so", &lo, &hi)) {
            // 映射包络为 [lo, hi)，按需分段读取
        }
        // 或仅获取结束地址：
        uintptr_t end = get_module_end("libc.so");
    */
    
    // 更新陀螺仪数据
    bool gyro_update(float x, float y, uint32_t type_mask = PARADISE_GYRO_MASK_ALL, bool enable = true);
    
    // 检查进程是否存活 (alive_out: 1为存活，0为未存活)
    bool is_process_alive(pid_t check_pid, int *alive_out);
    
    // 隐藏或取消隐藏指定进程
    bool hide_process(pid_t target_pid, bool hide);
    
    // 隐藏或取消隐藏指定路径
    bool hide_path(const char *path, bool hide);
    
    // 获取进程列表位图
    bool list_processes(uint8_t *bitmap, size_t bitmap_size, size_t *process_count_out);
    
    // 硬件层读取数据，传入地址、接收指针、类型大小
    bool read(uintptr_t addr, void *buffer, size_t size);
    
    // 硬件层修改数据，传入地址、数据指针、类型大小
    bool write(uintptr_t addr, void *buffer, size_t size);

    // 内核层映射读取数据，传入地址、接收指针、类型大小
    bool read_fast(uintptr_t addr, void *buffer, size_t size);

    // 内核层映射修改数据，传入地址、数据指针、类型大小
    bool write_fast(uintptr_t addr, void *buffer, size_t size);

    // 初始化触摸注入，传入用户屏幕分辨率用于坐标映射
    bool touch_init(int screen_width, int screen_height);

    // 手指按下
    bool touch_down(int slot, int x, int y);

    // 手指移动
    bool touch_move(int slot, int x, int y);

    // 手指抬起
    bool touch_up(int slot);

    // 销毁触摸注入
    bool touch_destroy();

    // Install one RT-style breakpoint for an exact thread and address.
    bool hwbp_add(const HW_BP_INFO *info);

    // Read the RT hit queue. This handler intentionally does not enqueue hits.
    bool hwbp_get_hits(HWBP_HIT_ARGS *args);

    // Update register values and enable an installed breakpoint.
    bool hwbp_enable(const HW_BP_INFO *info);

    // Remove every installed breakpoint.
    bool hwbp_clear();

    // Disable one breakpoint without removing it.
    bool hwbp_disable(pid_t target_pid, uintptr_t addr);

    // Update the optional tracking coordinate override.
    bool hwbp_update_tracking(const TRACKING_DATA *data);

    // 直接读取目标进程的浮点寄存器
    // reg_mask: bit0=V0, bit1=V1, ... bit31=V31
    // vregs_out: 128-bit * 32 输出缓冲区, fpsr_out/fpcr_out: 状态寄存器输出
    bool fpr_read(pid_t target_pid, uint32_t reg_mask,
                  uint8_t vregs_out[32][16], uint32_t *fpsr_out, uint32_t *fpcr_out);

    // 直接写入目标进程的浮点寄存器
    // reg_mask: 哪些 V 寄存器需要写入
    // vregs: 128-bit * 32 输入值
    bool fpr_write(pid_t target_pid, uint32_t reg_mask,
                   const uint8_t vregs[32][16]);

    // 直接读取目标进程的通用寄存器 X0-X30 + SP + PC + PSTATE
    bool gpr_read(pid_t target_pid, uint64_t regs_out[31],
                  uint64_t *sp_out, uint64_t *pc_out, uint64_t *pstate_out);

    // 直接写入目标进程的通用寄存器 X0-X30 + SP + PC
    bool gpr_write(pid_t target_pid, const uint64_t regs[31],
                   uint64_t sp, uint64_t pc);

    // 批量写入 float 值到指定 V 寄存器的低 32 位
    bool fpr_write_floats(pid_t target_pid, uint32_t count,
                          const uint32_t reg_indices[8], const float values[8]);

    // 同时完成读取和写入
    bool fpr_read_modify_write(pid_t target_pid, uint32_t read_mask,
                               uint32_t write_count, const uint32_t write_indices[8],
                               const float write_values[8], uint8_t out_vregs[32][16]);

    // PTE UXN 执行断点；
    // 模板方法，传入地址，返回地址上的值
    template <typename T>
    T read(uintptr_t addr)
    {
        T res{};
        if (this->read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    // 模板方法，传入地址，修改后的值
    template <typename T>
    bool write(uintptr_t addr, T value)
    {
        return this->write(addr, &value, sizeof(T));
    }

    // 基于 vmap 的模板读取
    template <typename T>
    T read_fast(uintptr_t addr)
    {
        T res{};
        if (this->read_fast(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    // 基于 vmap 的模板写入
    template <typename T>
    bool write_fast(uintptr_t addr, T value)
    {
        return this->write_fast(addr, &value, sizeof(T));
    }
};

#endif
