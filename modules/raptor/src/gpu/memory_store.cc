#ifdef MOTIS_CUDA
#include "motis/raptor/gpu/memory_store.h"

#include "cuda_runtime.h"

#include "motis/raptor/criteria/configs.h"
#include "motis/raptor/gpu/cuda_util.h"
#include "motis/raptor/gpu/gpu_raptor.cuh"
#include "motis/raptor/gpu/mc_gpu_raptor.cuh"

namespace motis::raptor {

#define FILL_LAUNCH_PARAMETER_MAP(VAL, ACCESSOR) \
  launch_configs_[ACCESSOR::VAL] =               \
      get_launch_config(get_mc_gpu_launch_config<VAL>());

inline void print_device_properties(cudaDeviceProp const& dp) {
  printf("Properties of device '%s':\n", dp.name);
  printf("\tCompute Capability:\t%i.%i\n", dp.major, dp.minor);
  printf("\tMultiprocessor Count:\t%i\n", dp.multiProcessorCount);
  printf("\tmaxThreadsPerBlock:\t%i\n", dp.maxThreadsPerBlock);
  printf("\tmaxThreadsPerDim:\t%i, %i, %i\n", dp.maxThreadsDim[0],
         dp.maxThreadsDim[1], dp.maxThreadsDim[2]);
  printf("\tmaxGridSizePerDim:\t%i, %i, %i\n", dp.maxGridSize[0],
         dp.maxGridSize[1], dp.maxGridSize[2]);
  printf("\tmaxBlocksPerMult.Proc.:\t%i\n", dp.maxBlocksPerMultiProcessor);
  printf("\tmaxThreadsPerMul.Proc.:\t%i\n", dp.maxThreadsPerMultiProcessor);
  printf("\tWarp Size:\t\t%i\n", dp.warpSize);
  printf("\tSupports Coop Launch:\t%i\n", dp.cooperativeLaunch);
}

inline void print_launch_parameters(
    std::unordered_map<raptor_criteria_config, kernel_launch_config> const&
        lps) {
  for(auto const& entry : lps) {
    printf("Launch Parameters for config: %s\n",
           get_string_for_criteria_config(entry.first).c_str());
    auto const& block = entry.second.threads_per_block_;
    auto const& grid  = entry.second.grid_;
    printf("\tBlock Dimensions:\t%i, %i, %i\n", block.x, block.y, block.z);
    printf("\tThreads per Block:\t%i\n", (block.x * block.y * block.z));
    printf("\tGrid Dimensions:\t%i, %i, %i\n", grid.x, grid.y, grid.z);
    printf("\tBlocks per Launch:\t%i\n", (grid.x * grid.y * grid.z));
  }
}

std::pair<dim3, dim3> get_launch_paramters(
    cudaDeviceProp const& prop, int32_t const concurrency_per_device) {
  int32_t block_dim_x = 32;  // must always be 32!
  int32_t block_dim_y = 32;  // range [1, ..., 32]
  int32_t block_size = block_dim_x * block_dim_y;
  int32_t max_blocks_per_sm = prop.maxThreadsPerMultiProcessor / block_size;

  if (max_blocks_per_sm < 1) {
    throw std::runtime_error{
        "Require a to large Block size to be executed on one SM!"};
  }

  auto const mp_count = prop.multiProcessorCount / concurrency_per_device;

  int32_t num_blocks = mp_count * max_blocks_per_sm;

  dim3 threads_per_block(block_dim_x, block_dim_y, 1);
  dim3 grid(num_blocks, 1, 1);

  return {threads_per_block, grid};
}

inline kernel_launch_config get_launch_config(
    const std::tuple<int, int>& params) {
  auto const& [grid_size, block_size] = params;
  kernel_launch_config klc{};
  klc.grid_.x = grid_size;
  klc.grid_.y = 1;
  klc.grid_.z = 1;

  klc.threads_per_block_.x =
      32;  // must always be 32 for the route scanning to work properly
  klc.threads_per_block_.y = std::floor(block_size / 32);
  klc.threads_per_block_.z = 1;

  return klc;
}

device_context::device_context(device_id const device_id,
                               int32_t const concurrency_per_device)
    : id_(device_id), launch_configs_{} {
  cudaSetDevice(id_);
  cuda_check();

  cudaGetDeviceProperties(&props_, device_id);
  cuda_check();
  print_device_properties(props_);

  launch_configs_[raptor_criteria_config::Default] =
      get_launch_config(get_gpu_launch_config());
  RAPTOR_CRITERIA_CONFIGS_WO_DEFAULT(FILL_LAUNCH_PARAMETER_MAP,
                                     raptor_criteria_config)

  print_launch_parameters(launch_configs_);

  cudaStreamCreate(&proc_stream_);
  cuda_check();
  cudaStreamCreate(&transfer_stream_);
  cuda_check();
}

void device_context::destroy() {
  cudaSetDevice(id_);
  cudaStreamDestroy(proc_stream_);
  proc_stream_ = cudaStream_t{};
  cudaStreamDestroy(transfer_stream_);
  transfer_stream_ = cudaStream_t{};
  cuda_check();
}

host_memory::host_memory(stop_id const stop_count,
                         raptor_criteria_config const criteria_config)
    : result_{
          std::make_unique<raptor_result_pinned>(stop_count, criteria_config)} {

  cudaMallocHost(&any_station_marked_, sizeof(bool));
  *any_station_marked_ = false;
}

void host_memory::destroy() {
  cudaFreeHost(any_station_marked_);
  any_station_marked_ = nullptr;
  result_ = nullptr;
}

void host_memory::reset() const {
  *any_station_marked_ = false;
  result_->reset();
}

device_memory::device_memory(stop_id const stop_count,
                             raptor_criteria_config const criteria_config,
                             route_id const route_count,
                             size_t const max_add_starts)
    : stop_count_{stop_count},
      route_count_{route_count},
      max_add_starts_{max_add_starts},
      arrival_times_count_{
          stop_count * get_trait_size_for_criteria_config(criteria_config)} {
  cudaMalloc(&(result_.front()), get_result_bytes());
  for (auto k = 1U; k < result_.size(); ++k) {
    result_[k] = result_[k - 1] + stop_count;
  }

  cudaMalloc(&footpaths_scratchpad_, get_scratchpad_bytes());
  cudaMalloc(&station_marks_, get_station_mark_bytes());
  cudaMalloc(&route_marks_, get_route_mark_bytes());
  cudaMalloc(&any_station_marked_, sizeof(bool));
  cudaMalloc(&additional_starts_, get_additional_starts_bytes());
  cuda_check();

  this->reset_async(nullptr);
}

void device_memory::destroy() {
  cudaFree(result_.front());
  cudaFree(footpaths_scratchpad_);
  cudaFree(station_marks_);
  cudaFree(route_marks_);
  cudaFree(any_station_marked_);
  cudaFree(additional_starts_);
}

size_t device_memory::get_result_bytes() const {
  return arrival_times_count_ * sizeof(time) * max_raptor_round;
}

size_t device_memory::get_station_mark_bytes() const {
  return ((stop_count_ / 32) + 1) * 4;
}

size_t device_memory::get_route_mark_bytes() const {
  return ((route_count_ / 32) + 1) * 4;
}

size_t device_memory::get_scratchpad_bytes() const {
  return arrival_times_count_ * sizeof(time);
}

size_t device_memory::get_additional_starts_bytes() const {
  return max_add_starts_ * sizeof(additional_start);
}

void device_memory::reset_async(cudaStream_t s) {
  cudaMemsetAsync(result_.front(), 0xFF, get_result_bytes(), s);
  cudaMemsetAsync(footpaths_scratchpad_, 0xFF, get_scratchpad_bytes(), s);
  cudaMemsetAsync(station_marks_, 0, get_station_mark_bytes(), s);
  cudaMemsetAsync(route_marks_, 0, get_route_mark_bytes(), s);
  cudaMemsetAsync(any_station_marked_, 0, sizeof(bool), s);
  cudaMemsetAsync(additional_starts_, 0xFF, get_additional_starts_bytes(), s);
  additional_start_count_ = invalid<decltype(additional_start_count_)>;
}

mem::mem(stop_id const stop_count, route_id const route_count,
         size_t const max_add_starts, device_id const device_id,
         int32_t const concurrency_per_device)
    : host_memories_{},
      device_memories_{},
      context_{device_id, concurrency_per_device},
      active_host_{nullptr},
      active_device_{nullptr},
      active_config_{raptor_criteria_config::Default},
      is_reset_{true}

// host_{stop_count, raptor_criteria_config::Default},
// device_{stop_count, raptor_criteria_config::Default, route_count,
//        max_add_starts}
{

  host_memories_.emplace(raptor_criteria_config::Default,
                         std::make_unique<host_memory>(
                             stop_count, raptor_criteria_config::Default));
  device_memories_.emplace(raptor_criteria_config::Default,
                           std::make_unique<device_memory>(
                               stop_count, raptor_criteria_config::Default,
                               route_count, max_add_starts));

  RAPTOR_CRITERIA_CONFIGS_WO_DEFAULT(INIT_HOST_AND_DEVICE_MEMORY,
                                     raptor_criteria_config)

  active_host_ = host_memories_[raptor_criteria_config::Default].get();
  active_device_ = device_memories_[raptor_criteria_config::Default].get();
}

mem::~mem() {
  std::for_each(host_memories_.begin(), host_memories_.end(),
                [](auto& el) { el.second->destroy(); });
  std::for_each(device_memories_.begin(), device_memories_.end(),
                [](auto& el) { el.second->destroy(); });
  // host_.destroy();
  // device_.destroy();
  context_.destroy();
}

void mem::reset_active() {
  active_device_->reset_async(context_.proc_stream_);
  active_host_->reset();
  is_reset_ = true;
}

void mem::require_active(raptor_criteria_config const criteria_config) {
  if (!is_reset_) {
    reset_active();
  }

  if (criteria_config != active_config_) {
    active_host_ = host_memories_[criteria_config].get();
    active_device_ = device_memories_[criteria_config].get();
    active_config_ = criteria_config;
  }

  // small safety net
  utl::verify_ex(active_host_ != nullptr && active_device_ != nullptr,
                 "Either active host or active device are null!");

  is_reset_ = false;
}

void memory_store::init(raptor_meta_info const& meta_info,
                        raptor_timetable const& tt,
                        int32_t const concurrency_per_device) {
  int32_t device_count = 0;
  cudaGetDeviceCount(&device_count);

  auto const max_add_starts = get_max_add_starts(meta_info);

  for (auto device_id = 0; device_id < device_count; ++device_id) {
    for (auto i = 0; i < concurrency_per_device; ++i) {
      memory_.emplace_back(std::make_unique<struct mem>(
          tt.stop_count(), tt.route_count(), max_add_starts, device_id,
          concurrency_per_device));
    }
  }

  memory_mutexes_ = std::vector<std::mutex>(memory_.size());
}

memory_store::mem_idx memory_store::get_mem_idx() {
  return current_idx_.fetch_add(1) % memory_.size();
}

loaned_mem::loaned_mem(memory_store& store) {
  auto const idx = store.get_mem_idx();
  lock_ = std::unique_lock(store.memory_mutexes_[idx]);
  mem_ = store.memory_[idx].get();
}

loaned_mem::~loaned_mem() {
  mem_->reset_active();
  cuda_sync_stream(mem_->context_.proc_stream_);
}

}  // namespace motis::raptor
#endif