#ifndef HOST_INTERFACE_DIRECT_H
#define HOST_INTERFACE_DIRECT_H

// 直接注入(direct)Host 接口:Host_Interface_Base 的子类,让外部把请求“直接”
// 注入 MQSim 的介质子系统(Data Cache -> FTL -> TSU -> ONFI -> NAND),
// 全程无 PCIe / NVMe / SATA 协议,也不构造 Host_System。
//
// 请求经一个自调度注入器按到达时间进入(Start_simulation 排第一个,
// Execute_simulator_event 注入一个并按下一条 arrival 续排),交给
// Input_Stream_Manager 分段成 flash 事务并向 cache/FTL 广播到达 —— 复用与 NVMe
// 接口完全相同的下游路径。完成由基类继承的 Setup_triggers 接线送达,在其中累计
// 每请求的器件内延迟。
//
// 本文件是 MQSim 本体的一等公民:只依赖 MQSim 头、不依赖 adapter,因此打完补丁后
// MQSim 可 `make` 独立编译,并经 main.cpp 的 DIRECT 分支独立运行。外部(standalone
// 的 direct-trace 读取器,或 pybind adapter)通过 Set_workload 喂入请求向量,并从
// Get_* 取结果。

#include <list>
#include <vector>
#include <cstdint>

#include "Host_Interface_Base.h"
#include "User_Request.h"
#include "Host_Interface_Defs.h"
#include "../sim/Sim_Event.h"
#include "../utils/XMLWriter.h"

namespace SSD_Components {

// 一笔已展开的直注入请求。standalone 从 direct-trace 读入、adapter 从 Python 转入;
// 两者都构造该向量后经 Set_workload 交给接口,由注入器按 arrival 回放。
struct Direct_Request {
    sim_time_type arrival_ns = 0;    // 绝对到达时刻(ns)
    LHA_type      lba = 0;           // 起始逻辑地址(512B 扇区)
    unsigned int  size_sectors = 0;  // 请求大小(512B 扇区数)
    bool          is_read = false;
};

// 一条注入流,持有分给它的 LSA 区间(供请求分段用)+ 在飞请求表。
class Input_Stream_Direct : public Input_Stream_Base {
public:
    Input_Stream_Direct(IO_Flow_Priority_Class::Priority priority_class,
                        LHA_type start_logical_sector_address,
                        LHA_type end_logical_sector_address)
        : Input_Stream_Base(), Priority_class(priority_class),
          Start_logical_sector_address(start_logical_sector_address),
          End_logical_sector_address(end_logical_sector_address) {}
    IO_Flow_Priority_Class::Priority Priority_class;
    LHA_type Start_logical_sector_address;
    LHA_type End_logical_sector_address;
    std::list<User_Request*> Waiting_user_requests;
};

class Input_Stream_Manager_Direct : public Input_Stream_Manager_Base {
public:
    Input_Stream_Manager_Direct(Host_Interface_Base* host_interface);
    stream_id_type Create_new_stream(IO_Flow_Priority_Class::Priority priority_class,
                                     LHA_type start_logical_sector_address,
                                     LHA_type end_logical_sector_address);
    void Handle_new_arrived_request(User_Request* request);
    void Handle_arrived_write_data(User_Request* request);
    void Handle_serviced_request(User_Request* request);
private:
    void segment_user_request(User_Request* user_request);
};

// 无 PCIe:fetch-unit 的每个钩子都是空实现(直注入路径永不触达)。
class Request_Fetch_Unit_Direct : public Request_Fetch_Unit_Base {
public:
    Request_Fetch_Unit_Direct(Host_Interface_Base* host_interface)
        : Request_Fetch_Unit_Base(host_interface) {}
    void Fetch_next_request(stream_id_type) {}
    void Fetch_write_data(User_Request*) {}
    void Send_read_data(User_Request*) {}
    void Process_pcie_write_message(uint64_t, void*, unsigned int) {}
    void Process_pcie_read_message(uint64_t, void*, unsigned int) {}
};

class Host_Interface_Direct : public Host_Interface_Base {
    friend class Input_Stream_Manager_Direct;
public:
    Host_Interface_Direct(const sim_object_id_type& id, LHA_type max_logical_sector_address,
                          unsigned int no_of_input_streams, unsigned int io_queue_depth,
                          unsigned int sectors_per_page, Data_Cache_Manager_Base* cache);

    // 外部在 Simulator->Start_simulation() 之前把(指向)展开后请求流的指针交进来;
    // 注入器按 arrival 时间回放它。
    void Set_workload(const std::vector<Direct_Request>* reqs) { workload = reqs; }

    // Sim_Object / Sim_Reporter 覆写。
    void Start_simulation();
    void Validate_simulation_config();
    void Execute_simulator_event(MQSimEngine::Sim_Event*);
    void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);

    // 结果访问器(供 standalone 打印 / adapter 收集)。
    uint32_t Get_generated_request_count() { return generated_count; }
    uint32_t Get_serviced_request_count()  { return serviced_count; }
    uint32_t Get_latency_sample_count()    { return lat_count; }
    sim_time_type Get_sum_request_latency() { return lat_sum; }   // sim ns
    sim_time_type Get_min_request_latency() { return lat_min_set ? lat_min : 0; }
    sim_time_type Get_max_request_latency() { return lat_max; }

    // 由流管理器在到达/完成时调用(让派生类访问基类的 broadcast 辅助与延迟累加器)。
    void Notify_arrival(User_Request* request) { broadcast_user_request_arrival_signal(request); }
    void Account_completion(sim_time_type latency, bool is_read);

private:
    // 按 arrival 注入,但把并发上限卡在 io_queue_depth(仿真真实器件的 IO 队列)。
    // 无界在飞会让大量写并发命中同一 LPA、破坏 GC 期 FTL 的 LPA 锁;到期但被满队列
    // 挡住的请求,会在完成腾出槽位后再注入(背压)。
    void try_inject_due();               // 队列有空位时,注入所有到期请求
    void schedule_event(sim_time_type when);  // 最多只排一个注入/泵事件
    void arm_next_event();               // 队列有空位时,排下一个未来到达
    void inject_one();                   // 构造并交出一个 User_Request

    unsigned int no_of_input_streams;
    unsigned int io_queue_depth;
    const std::vector<Direct_Request>* workload = nullptr;
    std::size_t cursor = 0;
    uint32_t in_flight = 0;
    bool event_pending = false;
    uint32_t generated_count = 0;
    uint32_t serviced_count = 0;
    // 每请求器件内延迟(注入 -> 完成),以 sim ns 累计
    sim_time_type lat_sum = 0, lat_min = 0, lat_max = 0;
    uint32_t lat_count = 0;
    bool lat_min_set = false;
};

}  // namespace SSD_Components

#endif  // HOST_INTERFACE_DIRECT_H
