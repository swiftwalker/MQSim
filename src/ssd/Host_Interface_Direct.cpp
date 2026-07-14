#include "Host_Interface_Direct.h"

#include <stdexcept>

#include "../sim/Engine.h"
#include "NVM_Transaction_Flash_RD.h"
#include "NVM_Transaction_Flash_WR.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"

namespace SSD_Components {

// ---------------- Input_Stream_Manager_Direct ----------------

Input_Stream_Manager_Direct::Input_Stream_Manager_Direct(Host_Interface_Base* host_interface)
    : Input_Stream_Manager_Base(host_interface) {}

stream_id_type Input_Stream_Manager_Direct::Create_new_stream(
    IO_Flow_Priority_Class::Priority priority_class,
    LHA_type start_logical_sector_address, LHA_type end_logical_sector_address) {
    if (end_logical_sector_address < start_logical_sector_address) {
        PRINT_ERROR("Direct host interface: stream start LSA must be <= end LSA.")
    }
    Input_Stream_Direct* input_stream =
        new Input_Stream_Direct(priority_class, start_logical_sector_address, end_logical_sector_address);
    this->input_streams.push_back(input_stream);
    return (stream_id_type)(this->input_streams.size() - 1);
}

void Input_Stream_Manager_Direct::Handle_new_arrived_request(User_Request* request) {
    Input_Stream_Direct* stream = (Input_Stream_Direct*)input_streams[request->Stream_id];
    stream->Waiting_user_requests.push_back(request);
    if (request->Type == UserRequestType::READ)
        stream->STAT_number_of_read_requests++;
    else
        stream->STAT_number_of_write_requests++;
    // 直注入:请求数据已在本地,读写都立即分段(无 PCIe 写数据 DMA 往返)。
    segment_user_request(request);
    ((Host_Interface_Direct*)host_interface)->Notify_arrival(request);
}

void Input_Stream_Manager_Direct::Handle_arrived_write_data(User_Request* request) {
    // 直注入路径不使用(写在 Handle_new_arrived_request 里已分段)。
}

void Input_Stream_Manager_Direct::Handle_serviced_request(User_Request* request) {
    Input_Stream_Direct* stream = (Input_Stream_Direct*)input_streams[request->Stream_id];
    stream->Waiting_user_requests.remove(request);

    sim_time_type latency = Simulator->Time() - request->STAT_InitiationTime;
    ((Host_Interface_Direct*)host_interface)->Account_completion(
        latency, request->Type == UserRequestType::READ);

    // 自定义清理(不用 DELETE_REQUEST_NVME):IO_command_info 里没有
    // Submission_Queue_Entry,且非集成模式下 Data 是借用指针。
    if (request->Transaction_list.size() != 0)
        PRINT_ERROR("Direct host interface: a serviced request still has unfinished transactions!")
    delete request;
}

// 照抄 Input_Stream_Manager_NVMe::segment_user_request,改指向 Input_Stream_Direct
// (同样的 Start/End_logical_sector_address 字段)。
void Input_Stream_Manager_Direct::segment_user_request(User_Request* user_request) {
    LHA_type lsa = user_request->Start_LBA;
    unsigned int req_size = user_request->SizeInSectors;

    page_status_type access_status_bitmap = 0;
    unsigned int handled_sectors_count = 0;
    unsigned int transaction_size = 0;
    Input_Stream_Direct* stream = (Input_Stream_Direct*)input_streams[user_request->Stream_id];
    while (handled_sectors_count < req_size) {
        // 把 LSA 限制在分给该流的区间内。
        if (lsa < stream->Start_logical_sector_address || lsa > stream->End_logical_sector_address) {
            lsa = stream->Start_logical_sector_address
                + (lsa % (stream->End_logical_sector_address - stream->Start_logical_sector_address));
        }
        LHA_type internal_lsa = lsa - stream->Start_logical_sector_address;

        transaction_size = host_interface->Get_no_of_LHAs_in_an_NVM_write_unit()
                         - (unsigned int)(lsa % host_interface->Get_no_of_LHAs_in_an_NVM_write_unit());
        if (handled_sectors_count + transaction_size >= req_size) {
            transaction_size = req_size - handled_sectors_count;
        }
        LPA_type lpa = internal_lsa / host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();

        page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);
        access_status_bitmap = temp << (int)(internal_lsa % host_interface->Get_no_of_LHAs_in_an_NVM_write_unit());

        if (user_request->Type == UserRequestType::READ) {
            NVM_Transaction_Flash_RD* transaction = new NVM_Transaction_Flash_RD(
                Transaction_Source_Type::USERIO, user_request->Stream_id,
                transaction_size * SECTOR_SIZE_IN_BYTE, lpa, NO_PPA, user_request,
                user_request->Priority_class, 0, access_status_bitmap, CurrentTimeStamp);
            user_request->Transaction_list.push_back(transaction);
            stream->STAT_number_of_read_transactions++;
        } else {
            NVM_Transaction_Flash_WR* transaction = new NVM_Transaction_Flash_WR(
                Transaction_Source_Type::USERIO, user_request->Stream_id,
                transaction_size * SECTOR_SIZE_IN_BYTE, lpa, user_request,
                user_request->Priority_class, 0, access_status_bitmap, CurrentTimeStamp);
            user_request->Transaction_list.push_back(transaction);
            stream->STAT_number_of_write_transactions++;
        }

        lsa = lsa + transaction_size;
        handled_sectors_count += transaction_size;
    }
}

// ---------------- Host_Interface_Direct ----------------

Host_Interface_Direct::Host_Interface_Direct(const sim_object_id_type& id,
    LHA_type max_logical_sector_address, unsigned int no_of_input_streams,
    unsigned int io_queue_depth, unsigned int sectors_per_page, Data_Cache_Manager_Base* cache)
    : Host_Interface_Base(id, HostInterface_Types::DIRECT, max_logical_sector_address,
                          sectors_per_page, cache),
      no_of_input_streams(no_of_input_streams),
      io_queue_depth(io_queue_depth > 0 ? io_queue_depth : 1) {
    this->input_stream_manager = new Input_Stream_Manager_Direct(this);
    this->request_fetch_unit = new Request_Fetch_Unit_Direct(this);
}

void Host_Interface_Direct::Start_simulation() {
    // 每个 flow 建一条流,覆盖地址分区单元分给它的 LSA 区间(在 SSD_Device 构造时设好)。
    Input_Stream_Manager_Direct* ism = (Input_Stream_Manager_Direct*)input_stream_manager;
    for (unsigned int i = 0; i < no_of_input_streams; i++) {
        ism->Create_new_stream(IO_Flow_Priority_Class::HIGH,
            Utils::Logical_Address_Partitioning_Unit::Start_lha_available_to_flow((stream_id_type)i),
            Utils::Logical_Address_Partitioning_Unit::End_lha_available_to_flow((stream_id_type)i));
    }
    // 启动注入:先灌入所有已到期请求,再排下一个未来到达。
    try_inject_due();
    arm_next_event();
}

void Host_Interface_Direct::Validate_simulation_config() {
    Host_Interface_Base::Validate_simulation_config();
    if (this->input_stream_manager == NULL)
        throw std::logic_error("Input stream manager is not set for the direct host interface");
    if (this->request_fetch_unit == NULL)
        throw std::logic_error("Request fetch unit is not set for the direct host interface");
}

// 从当前 workload 项构造一个 User_Request 并交给流管理器(它负责分段并向介质路径广播到达)。
void Host_Interface_Direct::inject_one() {
    const Direct_Request& r = (*workload)[cursor];
    User_Request* request = new User_Request;   // 构造函数会设 ID / ToBeIgnored / Sectors_serviced_from_cache
    request->Stream_id = 0;                      // 单一注入流
    request->Priority_class = IO_Flow_Priority_Class::HIGH;
    request->Type = r.is_read ? UserRequestType::READ : UserRequestType::WRITE;
    request->Start_LBA = (LHA_type)r.lba;
    request->SizeInSectors = r.size_sectors;
    request->Size_in_byte = r.size_sectors * SECTOR_SIZE_IN_BYTE;
    request->STAT_InitiationTime = Simulator->Time();
    request->IO_command_info = NULL;             // 直注入路径无 NVMe SQE
    request->Data = NULL;
    generated_count++;
    in_flight++;
    cursor++;
    ((Input_Stream_Manager_Direct*)input_stream_manager)->Handle_new_arrived_request(request);
}

// 在飞队列有空位时,注入所有到达时间已过的请求。并发上限卡在 io_queue_depth(器件队列
// 背压)—— 无界在飞会让大量写同时命中同一 LPA、破坏 GC 期 FTL 的 LPA 锁。
void Host_Interface_Direct::try_inject_due() {
    if (workload == nullptr) return;
    while (cursor < workload->size()
           && in_flight < io_queue_depth
           && (*workload)[cursor].arrival_ns <= Simulator->Time()) {
        inject_one();
    }
}

// 最多只排一个注入/泵事件。when 若在过去则钳到 now+(泵用 now+1 严格落在未来,以免
// 改动当前正在处理的事件节点)。
void Host_Interface_Direct::schedule_event(sim_time_type when) {
    if (event_pending) return;
    sim_time_type now = Simulator->Time();
    Simulator->Register_sim_event(when <= now ? now + 1 : when, this, NULL, 0);
    event_pending = true;
}

// 队列有空位时,排下一个(未来到期)请求的到达。到期即注的请求由 try_inject_due 处理;
// 满队列由 Account_completion 在腾出槽位时重新泵。
void Host_Interface_Direct::arm_next_event() {
    if (workload == nullptr || cursor >= workload->size()) return;
    if (in_flight < io_queue_depth)
        schedule_event((*workload)[cursor].arrival_ns);
}

void Host_Interface_Direct::Execute_simulator_event(MQSimEngine::Sim_Event* /*event*/) {
    event_pending = false;   // 已排的事件已触发
    try_inject_due();
    arm_next_event();
}

void Host_Interface_Direct::Account_completion(sim_time_type latency, bool /*is_read*/) {
    serviced_count++;
    if (in_flight > 0) in_flight--;
    lat_sum += latency;
    lat_count++;
    if (!lat_min_set || latency < lat_min) { lat_min = latency; lat_min_set = true; }
    if (latency > lat_max) lat_max = latency;
    // 不要在这里注入:此处运行在事务完成回调内,重入 FTL/AMU 派发会破坏状态。改为排一个
    // 新的泵事件,让腾出的槽位从 Execute_simulator_event 里被填。
    if (workload != nullptr && cursor < workload->size())
        schedule_event(Simulator->Time() + 1);
}

void Host_Interface_Direct::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter) {
    std::string tmp = name_prefix + ".HostInterface";
    xmlwriter.Write_open_tag(tmp);
    std::string attr = "Name";
    std::string val = ID();
    xmlwriter.Write_attribute_string(attr, val);
    for (unsigned int stream_id = 0; stream_id < no_of_input_streams; stream_id++) {
        std::string stag = name_prefix + ".IO_Stream";
        xmlwriter.Write_open_tag(stag);
        attr = "Stream_ID"; val = std::to_string(stream_id);
        xmlwriter.Write_attribute_string(attr, val);
        attr = "Average_Read_Transaction_Turnaround_Time";
        val = std::to_string(input_stream_manager->Get_average_read_transaction_turnaround_time(stream_id));
        xmlwriter.Write_attribute_string(attr, val);
        attr = "Average_Write_Transaction_Turnaround_Time";
        val = std::to_string(input_stream_manager->Get_average_write_transaction_turnaround_time(stream_id));
        xmlwriter.Write_attribute_string(attr, val);
        xmlwriter.Write_close_tag();
    }
    xmlwriter.Write_close_tag();
}

}  // namespace SSD_Components
