/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include "paddle/fluid/platform/device_context.h"

#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "paddle/fluid/memory/memory.h"
#ifdef PADDLE_WITH_CUDA
#include "paddle/fluid/framework/rw_lock.h"
#endif

namespace paddle {
namespace platform {

DeviceContextPool* DeviceContextPool::pool = nullptr;

platform::DeviceContext* DeviceContextPool::Get(const platform::Place& place) {
  auto it = device_contexts_.find(place);
  if (it == device_contexts_.end()) {
    PADDLE_THROW(
        "'Place' is not supported, Please re-compile with WITH_GPU "
        "option");
  }
  return it->second.get();
}

const std::vector<const DeviceContext*>
DeviceContextPool::GetAllDeviceContexts() const {
  std::vector<const DeviceContext*> all_device_ctx;
  all_device_ctx.reserve(device_contexts_.size());
  for (auto& dev_ctx : device_contexts_) {
    all_device_ctx.emplace_back(dev_ctx.second.get());
  }
  return all_device_ctx;
}

DeviceContextPool::DeviceContextPool(
    const std::vector<platform::Place>& places) {
  PADDLE_ENFORCE_GT(places.size(), 0);
  using PtrType = std::unique_ptr<DeviceContext>;
  std::set<Place> set;
  for (auto& p : places) {
    set.insert(p);
  }

  for (auto& p : set) {
    if (platform::is_cpu_place(p)) {
#ifdef PADDLE_WITH_MKLDNN
      device_contexts_.emplace(
          p, PtrType(new MKLDNNDeviceContext(boost::get<CPUPlace>(p))));
#else
      device_contexts_.emplace(
          p, PtrType(new CPUDeviceContext(boost::get<CPUPlace>(p))));
#endif
    } else if (platform::is_gpu_place(p)) {
#ifdef PADDLE_WITH_CUDA
      device_contexts_.emplace(
          p, PtrType(new CUDADeviceContext(boost::get<CUDAPlace>(p))));
#else
      PADDLE_THROW(
          "'CUDAPlace' is not supported, Please re-compile with WITH_GPU "
          "option");
#endif
    } else if (platform::is_cuda_pinned_place(p)) {
#ifdef PADDLE_WITH_CUDA
      device_contexts_.emplace(
          p,
          PtrType(new CUDAPinnedDeviceContext(boost::get<CUDAPinnedPlace>(p))));
#else
      PADDLE_THROW(
          "'CUDAPlace' is not supported, Please re-compile with WITH_GPU "
          "option");
#endif
    }
  }
}

CPUDeviceContext::CPUDeviceContext() {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

CPUDeviceContext::CPUDeviceContext(CPUPlace place) : place_(place) {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

Eigen::DefaultDevice* CPUDeviceContext::eigen_device() const {
  return eigen_device_.get();
}

Place CPUDeviceContext::GetPlace() const { return place_; }

#ifdef PADDLE_WITH_CUDA

class EigenCudaStreamDevice : public Eigen::StreamInterface {
 public:
  EigenCudaStreamDevice() : scratch_(nullptr), semaphore_(nullptr) {
    Eigen::initializeDeviceProp();
  }
  ~EigenCudaStreamDevice() override {}

  void Reinitialize(const cudaStream_t* cuda_stream, CUDAPlace place) {
    stream_ = cuda_stream;
    place_ = place;
    device_prop_ = &Eigen::m_deviceProperties[place.device];
  }

  const cudaStream_t& stream() const override { return *stream_; }

  const cudaDeviceProp& deviceProperties() const override {
    return *device_prop_;
  }

  void* allocate(size_t num_bytes) const override {
    return paddle::memory::Alloc(place_, num_bytes);
  }

  void deallocate(void* buffer) const override {
    paddle::memory::Free(place_, buffer);
  }

  void* scratchpad() const override {
    if (scratch_ == NULL) {
      scratch_ = allocate(Eigen::kCudaScratchSize + sizeof(unsigned int));
    }
    return scratch_;
  }

  unsigned int* semaphore() const override {
    if (semaphore_ == NULL) {
      char* scratch =
          static_cast<char*>(scratchpad()) + Eigen::kCudaScratchSize;
      semaphore_ = reinterpret_cast<unsigned int*>(scratch);
      PADDLE_ENFORCE(
          cudaMemsetAsync(semaphore_, 0, sizeof(unsigned int), *stream_));
    }
    return semaphore_;
  }

 private:
  CUDAPlace place_;
  const cudaStream_t* stream_;         // not owned;
  const cudaDeviceProp* device_prop_;  // not owned;
  mutable void* scratch_;
  mutable unsigned int* semaphore_;
};

class CudnnHolder {
 public:
  CudnnHolder(const cudaStream_t* stream, const CUDAPlace& place)
      : workspace_(nullptr), workspace_len_(0), stream_(stream), place_(place) {
    PADDLE_ENFORCE(dynload::cudnnCreate(&cudnn_handle_));
    PADDLE_ENFORCE(dynload::cudnnSetStream(cudnn_handle_, *stream_));
  }

  cudnnHandle_t cudnn_handle() const { return cudnn_handle_; }

  void RunFunc(const std::function<void(void*)>& cudnn_func,
               size_t required_workspace_len) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (required_workspace_len > workspace_len_) {
      ReallocateWorkspace(required_workspace_len);
    }
    cudnn_func(workspace_);
  }

  ~CudnnHolder() {
    PADDLE_ENFORCE(dynload::cudnnDestroy(cudnn_handle_));
    if (workspace_ != nullptr) {
      paddle::memory::Free(place_, workspace_);
    }
  }

 private:
  void ReallocateWorkspace(size_t required_workspace_len) {
    if (required_workspace_len <= workspace_len_) {
      return;
    }
    if (workspace_ != nullptr) {
      // Maybe someone is using the current workspace
      PADDLE_ENFORCE(cudaStreamSynchronize(*stream_));
      paddle::memory::Free(place_, workspace_);
    }
    workspace_ = paddle::memory::Alloc(place_, required_workspace_len);
    workspace_len_ = required_workspace_len;
  }

  cudnnHandle_t cudnn_handle_;
  void* workspace_;
  size_t workspace_len_;

  const cudaStream_t* stream_;  // not owned;
  const CUDAPlace place_;

  std::mutex mtx_;
};

CUDADeviceContext::CUDADeviceContext(CUDAPlace place)
    : place_(place), cudnn_holder_(nullptr) {
  SetDeviceId(place_.device);
  compute_capability_ = GetCUDAComputeCapability(place_.device);
  multi_process_ = GetCUDAMultiProcessors(place_.device);
  max_threads_per_mp_ = GetCUDAMaxThreadsPerMultiProcessor(place_.device);
  PADDLE_ENFORCE(cudaStreamCreate(&stream_));
  eigen_stream_.reset(new EigenCudaStreamDevice());
  eigen_stream_->Reinitialize(&stream_, place);
  eigen_device_.reset(new Eigen::GpuDevice(eigen_stream_.get()));
  PADDLE_ENFORCE(dynload::cublasCreate(&cublas_handle_));
  PADDLE_ENFORCE(dynload::cublasSetStream(cublas_handle_, stream_));
  if (dynload::HasCUDNN()) {
    cudnn_holder_.reset(new CudnnHolder(&stream_, place));
  }

  driver_version_ = GetCUDADriverVersion(place_.device);
  runtime_version_ = GetCUDARuntimeVersion(place_.device);

  LOG(INFO) << "device: " << place_.device
            << ", CUDA Capability: " << compute_capability_
            << ", Driver Version: " << driver_version_ / 1000 << "."
            << (driver_version_ % 100) / 10
            << ", Runtime Version: " << runtime_version_ / 1000 << "."
            << (runtime_version_ % 100) / 10;

  callback_manager_.reset(new StreamCallbackManager(stream_));
}

CUDADeviceContext::~CUDADeviceContext() {
  SetDeviceId(place_.device);
  Wait();
  WaitStreamCallback();
  PADDLE_ENFORCE(dynload::cublasDestroy(cublas_handle_));
  eigen_stream_.reset();
  eigen_device_.reset();
  PADDLE_ENFORCE(cudaStreamDestroy(stream_));
}

Place CUDADeviceContext::GetPlace() const { return place_; }

void CUDADeviceContext::Wait() const {
  PADDLE_ENFORCE(cudaStreamSynchronize(stream_));
  PADDLE_ENFORCE(cudaGetLastError());
}

int CUDADeviceContext::GetComputeCapability() const {
  return compute_capability_;
}

int CUDADeviceContext::GetMaxPhysicalThreadCount() const {
  return multi_process_ * max_threads_per_mp_;
}

Eigen::GpuDevice* CUDADeviceContext::eigen_device() const {
  return eigen_device_.get();
}

cublasHandle_t CUDADeviceContext::cublas_handle() const {
  return cublas_handle_;
}

cudnnHandle_t CUDADeviceContext::cudnn_handle() const {
  return cudnn_holder_->cudnn_handle();
}

void CUDADeviceContext::RunCudnnFuncWithWorkspace(
    const std::function<void(void*)>& cudnn_func, size_t workspace_len) const {
  cudnn_holder_->RunFunc(cudnn_func, workspace_len);
}

cudaStream_t CUDADeviceContext::stream() const { return stream_; }

CUDAPinnedDeviceContext::CUDAPinnedDeviceContext() {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

CUDAPinnedDeviceContext::CUDAPinnedDeviceContext(CUDAPinnedPlace place)
    : place_(place) {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

Eigen::DefaultDevice* CUDAPinnedDeviceContext::eigen_device() const {
  return eigen_device_.get();
}

Place CUDAPinnedDeviceContext::GetPlace() const { return place_; }
#endif

#ifdef PADDLE_WITH_MKLDNN
MKLDNNDeviceContext::MKLDNNDeviceContext(CPUPlace place)
    : CPUDeviceContext(place), engine_(mkldnn::engine::cpu, 0), p_blobmap_() {
  p_blobmap_.reset(new BlobMap());
  p_mutex_.reset(new std::mutex());
}

namespace {
// Current thread's id.
thread_local int cur_thread_id = 0;
}

void set_cur_thread_id(int tid) { cur_thread_id = tid; }
int get_cur_thread_id(void) { return cur_thread_id; }

void MKLDNNDeviceContext::SetBlob(const std::string& name,
                                  std::shared_ptr<void> data) const {
  BlobMap* pMap = p_blobmap_.get();
  std::shared_ptr<KeyBlob> pBlob = nullptr;

  int tid = platform::get_cur_thread_id();

  std::lock_guard<std::mutex> lock(*p_mutex_.get());

  // Find KeyBlob for current thread
  auto map_it = pMap->find(tid);

  if (map_it == pMap->end()) {
    // 1st time to set blob in current thread
    pBlob = std::shared_ptr<KeyBlob>(new KeyBlob());
    (*pMap)[tid] = pBlob;
  } else {
    pBlob = map_it->second;
  }

  // Find Key in found (or newly created) KeyBlob
  auto key_it = pBlob->find(name);

  if (key_it == pBlob->end()) {
    (*pBlob)[name] = data;  // create new blob
  } else {
    key_it->second = data;  // set data to existing blob
  }

  // lock will be automatically released when out of scope
  return;
}

std::shared_ptr<void> MKLDNNDeviceContext::GetBlob(
    const std::string& name) const {
  BlobMap* pMap = p_blobmap_.get();
  std::shared_ptr<KeyBlob> pBlob = nullptr;

  int tid = platform::get_cur_thread_id();

  std::lock_guard<std::mutex> lock(*p_mutex_.get());

  // Find KeyBlob for current thread firstly
  auto map_it = pMap->find(tid);
  if (map_it == pMap->end()) return nullptr;
  pBlob = map_it->second;

  // Find Blob via name
  auto key_it = pBlob->find(name);

  if (key_it == pBlob->end()) return nullptr;

  // lock will be automatically released when out of scope
  return key_it->second;
}

#endif

}  // namespace platform
}  // namespace paddle
