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

/*!
 * \file cuda_module.cc
 */
#include "cuda_module.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include <tvm/ffi/function.h>
#include <tvm/ffi/reflection/registry.h>

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../file_utils.h"
#include "../meta_data.h"
#include "../pack_args.h"
#include "../thread_storage_scope.h"
#include "cuda_common.h"

namespace tvm {
namespace runtime {

// Module to support thread-safe multi-GPU execution.
// cuModule is a per-GPU module
// The runtime will contain a per-device module table
// The modules will be lazily loaded
class CUDAModuleNode : public runtime::ModuleNode {
 public:
  explicit CUDAModuleNode(std::string data, std::string fmt,
                          std::unordered_map<std::string, FunctionInfo> fmap,
                          std::string cuda_source)
      : data_(data), fmt_(fmt), fmap_(fmap), cuda_source_(cuda_source) {
    std::fill(module_.begin(), module_.end(), nullptr);
  }
  // destructor
  ~CUDAModuleNode() {
    for (size_t i = 0; i < module_.size(); ++i) {
      if (module_[i] != nullptr) {
        CUDA_CALL(cudaSetDevice(static_cast<int>(i)));
        CUDA_DRIVER_CALL(cuModuleUnload(module_[i]));
      }
    }
  }

  const char* type_key() const final { return "cuda"; }

  /*! \brief Get the property of the runtime module .*/
  int GetPropertyMask() const final {
    return ModulePropertyMask::kBinarySerializable | ModulePropertyMask::kRunnable;
  }

  ffi::Function GetFunction(const String& name, const ObjectPtr<Object>& sptr_to_self) final;

  void SaveToFile(const String& file_name, const String& format) final {
    std::string fmt = GetFileFormat(file_name, format);
    std::string meta_file = GetMetaFilePath(file_name);
    if (fmt == "cu") {
      ICHECK_NE(cuda_source_.length(), 0);
      SaveMetaDataToFile(meta_file, fmap_);
      SaveBinaryToFile(file_name, cuda_source_);
    } else {
      ICHECK_EQ(fmt, fmt_) << "Can only save to format=" << fmt_;
      SaveMetaDataToFile(meta_file, fmap_);
      SaveBinaryToFile(file_name, data_);
    }
  }

  void SaveToBinary(dmlc::Stream* stream) final {
    stream->Write(fmt_);
    stream->Write(fmap_);
    stream->Write(data_);
  }

  String GetSource(const String& format) final {
    if (format == fmt_) return data_;
    if (cuda_source_.length() != 0) {
      return cuda_source_;
    } else {
      if (fmt_ == "ptx") return data_;
      return "";
    }
  }

  // get a CUfunction from primary context in device_id
  CUfunction GetFunc(int device_id, const std::string& func_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    // must recheck under the lock scope
    if (module_[device_id] == nullptr) {
      CUDA_DRIVER_CALL(cuModuleLoadData(&(module_[device_id]), data_.c_str()));
      static auto nvshmem_init_hook = ffi::Function::GetGlobal("runtime.nvshmem.cumodule_init");
      if (nvshmem_init_hook.has_value()) {
        (*nvshmem_init_hook)(static_cast<void*>(module_[device_id]));
      }
    }
    CUfunction func;
    CUresult result = cuModuleGetFunction(&func, module_[device_id], func_name.c_str());
    if (result != CUDA_SUCCESS) {
      const char* msg;
      cuGetErrorName(result, &msg);
      LOG(FATAL) << "CUDAError: cuModuleGetFunction " << func_name << " failed with error: " << msg;
    }
    return func;
  }
  // get a global var from primary context in device_id
  CUdeviceptr GetGlobal(int device_id, const std::string& global_name, size_t expect_nbytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    // must recheck under the lock scope
    if (module_[device_id] == nullptr) {
      CUDA_DRIVER_CALL(cuModuleLoadData(&(module_[device_id]), data_.c_str()));
      static auto nvshmem_init_hook = ffi::Function::GetGlobal("runtime.nvshmem.cumodule_init");
      if (nvshmem_init_hook.has_value()) {
        (*nvshmem_init_hook)(static_cast<void*>(module_[device_id]));
      }
    }
    CUdeviceptr global;
    size_t nbytes;

    CUresult result = cuModuleGetGlobal(&global, &nbytes, module_[device_id], global_name.c_str());
    ICHECK_EQ(nbytes, expect_nbytes);
    if (result != CUDA_SUCCESS) {
      const char* msg;
      cuGetErrorName(result, &msg);
      LOG(FATAL) << "CUDAError: cuModuleGetGlobal " << global_name << " failed with error: " << msg;
    }
    return global;
  }

 private:
  // the binary data
  std::string data_;
  // The format
  std::string fmt_;
  // function information table.
  std::unordered_map<std::string, FunctionInfo> fmap_;
  // The cuda source.
  std::string cuda_source_;
  // the internal modules per GPU, to be lazily initialized.
  std::array<CUmodule, kMaxNumGPUs> module_;
  // internal mutex when updating the module
  std::mutex mutex_;
};

// a wrapped function class to get packed func.
class CUDAWrappedFunc {
 public:
  // initialize the CUDA function.
  void Init(CUDAModuleNode* m, ObjectPtr<Object> sptr, const std::string& func_name,
            size_t num_void_args, const std::vector<std::string>& launch_param_tags) {
    m_ = m;
    sptr_ = sptr;
    func_name_ = func_name;
    std::fill(fcache_.begin(), fcache_.end(), nullptr);
    launch_param_config_.Init(num_void_args, launch_param_tags);
  }
  // invoke the function with void arguments
  void operator()(ffi::PackedArgs args, ffi::Any* rv, void** void_args) const {
    int device_id;
    CUDA_CALL(cudaGetDevice(&device_id));
    ThreadWorkLoad wl = launch_param_config_.Extract(args);

    if (fcache_[device_id] == nullptr) {
      fcache_[device_id] = m_->GetFunc(device_id, func_name_);
      if (wl.dyn_shmem_size >= (48 << 10)) {
        // Assumption: dyn_shmem_size doesn't change across different invocations of
        // fcache_[device_id]
        CUresult result = cuFuncSetAttribute(
            fcache_[device_id], CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, wl.dyn_shmem_size);
        if (result != CUDA_SUCCESS) {
          LOG(FATAL) << "Failed to set the allowed dynamic shared memory size to "
                     << wl.dyn_shmem_size;
        }
      }
    }
    CUstream strm = static_cast<CUstream>(CUDAThreadEntry::ThreadLocal()->stream);
    CUresult result = cuLaunchKernel(fcache_[device_id], wl.grid_dim(0), wl.grid_dim(1),
                                     wl.grid_dim(2), wl.block_dim(0), wl.block_dim(1),
                                     wl.block_dim(2), wl.dyn_shmem_size, strm, void_args, nullptr);
    if (result != CUDA_SUCCESS && result != CUDA_ERROR_DEINITIALIZED) {
      const char* msg;
      cuGetErrorName(result, &msg);
      std::ostringstream os;
      os << "CUDALaunch Error: " << msg << "\n"
         << " grid=(" << wl.grid_dim(0) << "," << wl.grid_dim(1) << "," << wl.grid_dim(2) << "), "
         << " block=(" << wl.block_dim(0) << "," << wl.block_dim(1) << "," << wl.block_dim(2)
         << ")\n";
      std::string cuda = m_->GetSource("");
      if (cuda.length() != 0) {
        os << "// func_name=" << func_name_ << "\n"
           << "// CUDA Source\n"
           << "// -----------\n"
           << cuda;
      }
      LOG(FATAL) << os.str();
    }
  }

 private:
  // internal module
  CUDAModuleNode* m_;
  // the resource holder
  ObjectPtr<Object> sptr_;
  // The name of the function.
  std::string func_name_;
  // Device function cache per device.
  // mark as mutable, to enable lazy initialization
  mutable std::array<CUfunction, kMaxNumGPUs> fcache_;
  // launch parameters configuration
  LaunchParamConfig launch_param_config_;
};

class CUDAPrepGlobalBarrier {
 public:
  CUDAPrepGlobalBarrier(CUDAModuleNode* m, ObjectPtr<Object> sptr) : m_(m), sptr_(sptr) {
    std::fill(pcache_.begin(), pcache_.end(), 0);
  }

  void operator()(const ffi::PackedArgs& args, ffi::Any* rv) const {
    int device_id;
    CUDA_CALL(cudaGetDevice(&device_id));
    if (pcache_[device_id] == 0) {
      pcache_[device_id] =
          m_->GetGlobal(device_id, runtime::symbol::tvm_global_barrier_state, sizeof(unsigned));
    }
    CUDA_DRIVER_CALL(cuMemsetD32(pcache_[device_id], 0, 1));
  }

 private:
  // internal module
  CUDAModuleNode* m_;
  // the resource holder
  ObjectPtr<Object> sptr_;
  // mark as mutable, to enable lazy initialization
  mutable std::array<CUdeviceptr, kMaxNumGPUs> pcache_;
};

ffi::Function CUDAModuleNode::GetFunction(const String& name,
                                          const ObjectPtr<Object>& sptr_to_self) {
  ICHECK_EQ(sptr_to_self.get(), this);
  ICHECK_NE(name, symbol::tvm_module_main) << "Device function do not have main";
  if (name == symbol::tvm_prepare_global_barrier) {
    return ffi::Function(CUDAPrepGlobalBarrier(this, sptr_to_self));
  }
  auto it = fmap_.find(name);
  if (it == fmap_.end()) return ffi::Function();
  const FunctionInfo& info = it->second;
  CUDAWrappedFunc f;
  f.Init(this, sptr_to_self, name, info.arg_types.size(), info.launch_param_tags);
  return PackFuncVoidAddr(f, info.arg_types, info.arg_extra_tags);
}

Module CUDAModuleCreate(std::string data, std::string fmt,
                        std::unordered_map<std::string, FunctionInfo> fmap,
                        std::string cuda_source) {
  auto n = make_object<CUDAModuleNode>(data, fmt, fmap, cuda_source);
  return Module(n);
}

// Load module from module.
Module CUDAModuleLoadFile(const std::string& file_name, const String& format) {
  std::string data;
  std::unordered_map<std::string, FunctionInfo> fmap;
  std::string fmt = GetFileFormat(file_name, format);
  std::string meta_file = GetMetaFilePath(file_name);
  LoadBinaryFromFile(file_name, &data);
  LoadMetaDataFromFile(meta_file, &fmap);
  return CUDAModuleCreate(data, fmt, fmap, std::string());
}

Module CUDAModuleLoadBinary(void* strm) {
  dmlc::Stream* stream = static_cast<dmlc::Stream*>(strm);
  std::string data;
  std::unordered_map<std::string, FunctionInfo> fmap;
  std::string fmt;
  stream->Read(&fmt);
  stream->Read(&fmap);
  stream->Read(&data);
  return CUDAModuleCreate(data, fmt, fmap, std::string());
}

TVM_FFI_STATIC_INIT_BLOCK({
  namespace refl = tvm::ffi::reflection;
  refl::GlobalDef()
      .def("runtime.module.loadfile_cubin", CUDAModuleLoadFile)
      .def("runtime.module.loadfile_ptx", CUDAModuleLoadFile)
      .def("runtime.module.loadbinary_cuda", CUDAModuleLoadBinary);
});
}  // namespace runtime
}  // namespace tvm
