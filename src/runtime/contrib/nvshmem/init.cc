/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <nvshmem.h>
#include <nvshmemx.h>
#include <picojson.h>
#include <tvm/ffi/function.h>
#include <tvm/ffi/reflection/registry.h>
#include <tvm/runtime/disco/disco_worker.h>

#include "../../cuda/cuda_common.h"

namespace tvm {
namespace runtime {

ffi::Shape InitNVSHMEMUID() {
  nvshmemx_uniqueid_t uid;
  nvshmemx_get_uniqueid(&uid);
  std::vector<int64_t> uid_64;
  uid_64.push_back(static_cast<int64_t>(uid.version));
  for (int i = 0; i < UNIQUEID_PADDING; ++i) {
    uid_64.push_back(static_cast<int64_t>(uid.internal[i]));
  }
  return ffi::Shape(uid_64);
}

void InitNVSHMEM(ffi::Shape uid_64, int num_workers, int worker_id_start) {
  DiscoWorker* worker = ThreadLocalDiscoWorker::Get()->worker;
  int worker_id;
  if (worker == nullptr) {
    worker_id = worker_id_start;
  } else {
    worker_id = worker_id_start + worker->worker_id;
  }
  CHECK_EQ(uid_64.size(), UNIQUEID_PADDING + 1)
      << "ValueError: The length of unique_id must be " << UNIQUEID_PADDING << ", but got "
      << uid_64.size() << ".";

  nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;

  nvshmemx_uniqueid_t uid;
  uid.version = static_cast<int>(uid_64[0]);
  for (int i = 0; i < UNIQUEID_PADDING; ++i) {
    uid.internal[i] = static_cast<char>(uid_64[i + 1]);
  }
  // FIXME: this is a hack to avoid the issue of NVSHMEM using Multi-process-per-GPU to initialize
  cudaSetDevice(worker_id);
  nvshmemx_set_attr_uniqueid_args(worker_id, num_workers, &uid, &attr);
  nvshmemx_init_attr(NVSHMEMX_INIT_WITH_UNIQUEID, &attr);
  int mype_node = nvshmem_team_my_pe(NVSHMEMX_TEAM_NODE);
  CUDA_CALL(cudaSetDevice(mype_node));
  if (worker != nullptr) {
    if (worker->default_device.device_type == DLDeviceType::kDLCPU) {
      worker->default_device = Device{DLDeviceType::kDLCUDA, mype_node};
    } else {
      ICHECK(worker->default_device.device_type == DLDeviceType::kDLCUDA &&
             worker->default_device.device_id == mype_node)
          << "The default device of the worker is inconsistent with the device used for NVSHMEM. "
          << "The default device is " << worker->default_device
          << ", but the device used for NVSHMEM is " << Device{DLDeviceType::kDLCUDA, mype_node}
          << ".";
    }
  }
  LOG_INFO << "NVSHMEM init finished: mype=" << nvshmem_my_pe() << " "
           << ", npes=" << nvshmem_n_pes();
}

void InitNVSHMEMWrapper(String args) {
  picojson::value v;
  std::string err = picojson::parse(v, args);
  if (!err.empty()) {
    LOG(FATAL) << "JSON parse error: " << err;
  }

  if (!v.is<picojson::object>()) {
    LOG(FATAL) << "JSON is not an object";
  }

  picojson::object& obj = v.get<picojson::object>();

  picojson::array uid_array = obj["uid"].get<picojson::array>();
  std::vector<int64_t> uid_vector;
  for (const auto& elem : uid_array) {
    uid_vector.push_back(elem.get<int64_t>());
  }

  ffi::Shape uid_64(uid_vector);

  int num_workers = static_cast<int>(obj["npes"].get<int64_t>());
  int worker_id_start = static_cast<int>(obj["pe_start"].get<int64_t>());

  InitNVSHMEM(uid_64, num_workers, worker_id_start);
}

void NVSHMEMXCumoduleInit(void* cuModule) {
  CUmodule mod = static_cast<CUmodule>(cuModule);
  auto status = nvshmemx_init_status();
  // The NVSHMEM library must have completed device initialization prior to
  // nvshmemx_cumodule_init. If not, we skip the cumodule initialization.
  if (status == NVSHMEM_STATUS_IS_INITIALIZED || status == NVSHMEM_STATUS_LIMITED_MPG ||
      status == NVSHMEM_STATUS_FULL_MPG) {
    int result = nvshmemx_cumodule_init(mod);
    ICHECK_EQ(result, 0) << "nvshmemx_cumodule_init failed with error code: " << result;
  }
}

TVM_FFI_STATIC_INIT_BLOCK({
  namespace refl = tvm::ffi::reflection;
  refl::GlobalDef()
      .def("runtime.disco.nvshmem.init_nvshmem_uid", InitNVSHMEMUID)
      .def("runtime.disco.nvshmem.init_nvshmem", InitNVSHMEM)
      .def("runtime.disco.nvshmem.init_nvshmem_wrapper", InitNVSHMEMWrapper)
      .def("runtime.nvshmem.cumodule_init", NVSHMEMXCumoduleInit);
});

}  // namespace runtime
}  // namespace tvm
