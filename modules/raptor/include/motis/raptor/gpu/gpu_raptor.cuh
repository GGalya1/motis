#pragma once

#include "motis/raptor/raptor_query.h"

namespace motis::raptor {

__device__ void init_arrivals_dev(base_query const& query,
                                  device_memory const& device_mem,
                                  device_gpu_timetable const& tt);

__device__ void update_routes_dev(time const* prev_arrivals, time* arrivals,
                                  unsigned int* station_marks,
                                  unsigned int* route_marks,
                                  bool* any_station_marked,
                                  device_gpu_timetable const& tt);

__device__ void update_footpaths_dev(device_memory const& device_mem,
                                     raptor_round round_k,
                                     device_gpu_timetable const& tt);

void invoke_gpu_raptor(d_query const&);
void invoke_hybrid_raptor(d_query const&);

}  // namespace motis::raptor