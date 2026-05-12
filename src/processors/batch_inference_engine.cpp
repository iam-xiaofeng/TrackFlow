#include "processors/batch_inference_engine.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <onnxruntime_cxx_api.h>
#include <stdexcept>

namespace yolo_edge {

struct BatchInferenceEngine::OrtData {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "BatchEngine"};
  std::unique_ptr<Ort::Session> session;
  Ort::MemoryInfo memory_info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<const char *> input_names;
  std::vector<const char *> output_names;
  std::vector<std::string> input_names_storage;
  std::vector<std::string> output_names_storage;
};

BatchInferenceEngine &BatchInferenceEngine::instance() {
  static BatchInferenceEngine engine;
  return engine;
}

BatchInferenceEngine::~BatchInferenceEngine() { shutdown(); }

void BatchInferenceEngine::init(const std::string &model_path, int input_h,
                                int input_w, bool use_cuda,
                                int max_batch_size, int max_wait_ms,
                                int max_pending, int ort_threads) {
  if (initialized_)
    return;

  input_h_ = input_h;
  input_w_ = input_w;
  max_batch_size_ = max_batch_size;
  max_wait_ms_ = max_wait_ms;
  max_pending_ = max_pending;
  ort_threads_ = ort_threads;

  ort_ = std::make_unique<OrtData>();

  try {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(ort_threads_);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    bool cuda_ok = false;
    if (use_cuda) {
      try {
        OrtCUDAProviderOptions cuda_opts;
        cuda_opts.device_id = 0;
        cuda_opts.arena_extend_strategy = 0;
        cuda_opts.gpu_mem_limit = 0;
        cuda_opts.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
        cuda_opts.do_copy_in_default_stream = 1;
        opts.AppendExecutionProvider_CUDA(cuda_opts);
        cuda_ok = true;
      } catch (const Ort::Exception &e) {
        fprintf(stderr,
                "[WARN] BatchEngine: CUDA failed: %s, using CPU\n",
                e.what());
      }
    }

    ort_->session =
        std::make_unique<Ort::Session>(ort_->env, model_path.c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;

    size_t num_in = ort_->session->GetInputCount();
    for (size_t i = 0; i < num_in; ++i) {
      auto name = ort_->session->GetInputNameAllocated(i, alloc);
      ort_->input_names_storage.push_back(name.get());
      ort_->input_names.push_back(ort_->input_names_storage.back().c_str());
    }

    size_t num_out = ort_->session->GetOutputCount();
    for (size_t i = 0; i < num_out; ++i) {
      auto name = ort_->session->GetOutputNameAllocated(i, alloc);
      ort_->output_names_storage.push_back(name.get());
      ort_->output_names.push_back(ort_->output_names_storage.back().c_str());
    }

    if (cuda_ok) {
      fprintf(stderr, "[INFO] BatchEngine: Warming up CUDA...\n");
      size_t sz = 1 * 3 * input_h_ * input_w_;
      std::vector<float> dummy(sz, 0.0f);
      std::vector<int64_t> shape = {1, 3, (int64_t)input_h_,
                                    (int64_t)input_w_};
      auto tensor = Ort::Value::CreateTensor<float>(
          ort_->memory_info, dummy.data(), sz, shape.data(), shape.size());
      ort_->session->Run(Ort::RunOptions{nullptr}, ort_->input_names.data(),
                         &tensor, 1, ort_->output_names.data(),
                         ort_->output_names.size());
      fprintf(stderr, "[INFO] BatchEngine: Warmup done\n");
    }

    fprintf(stderr,
            "[INFO] BatchEngine: Ready (model=%s, %s, batch_max=%d, wait=%dms, pending_max=%d, ort_threads=%d)\n",
            model_path.c_str(), cuda_ok ? "GPU" : "CPU", max_batch_size_,
            max_wait_ms_, max_pending_, ort_threads_);

    running_ = true;
    initialized_ = true;
    worker_ = std::thread(&BatchInferenceEngine::worker_loop, this);

  } catch (const std::exception &e) {
    fprintf(stderr, "[ERROR] BatchEngine: Init failed: %s\n", e.what());
    ort_.reset();
  }
}

std::future<BatchInferenceEngine::InferResult>
BatchInferenceEngine::submit(InferRequest req) {
  auto future = try_submit(std::move(req));
  if (!future.has_value()) {
    throw std::runtime_error("BatchInferenceEngine queue is full");
  }
  return std::move(*future);
}

std::optional<std::future<BatchInferenceEngine::InferResult>>
BatchInferenceEngine::try_submit(InferRequest req) {
  std::promise<InferResult> promise;
  auto future = promise.get_future();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!running_) {
      return std::nullopt;
    }
    if (max_pending_ > 0 && static_cast<int>(queue_.size()) >= max_pending_) {
      return std::nullopt;
    }
    queue_.push({std::move(req), std::move(promise)});
  }
  queue_cv_.notify_one();

  return std::optional<std::future<InferResult>>(std::move(future));
}

size_t BatchInferenceEngine::pending() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queue_.size();
}

void BatchInferenceEngine::shutdown() {
  if (!running_)
    return;

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    running_ = false;
  }
  queue_cv_.notify_all();

  if (worker_.joinable())
    worker_.join();

  initialized_ = false;
}

void BatchInferenceEngine::worker_loop() {
  while (running_) {
    std::vector<InferRequest> requests;
    std::vector<std::promise<InferResult>> promises;

    {
      std::unique_lock<std::mutex> lock(queue_mutex_);

      queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });

      if (!running_ && queue_.empty())
        break;

      auto &front = queue_.front();
      requests.push_back(std::move(front.request));
      promises.push_back(std::move(front.promise));
      queue_.pop();

      if (max_batch_size_ > 1) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(max_wait_ms_);

        while (static_cast<int>(requests.size()) < max_batch_size_) {
          if (queue_cv_.wait_until(lock, deadline, [this] {
                return !queue_.empty() || !running_;
              })) {
            if (!running_)
              break;
            if (!queue_.empty()) {
              auto &item = queue_.front();
              requests.push_back(std::move(item.request));
              promises.push_back(std::move(item.promise));
              queue_.pop();
            }
          } else {
            break;
          }
        }
      }
    }

    if (!requests.empty()) {
      run_batch(requests, promises);
    }
  }

  std::lock_guard<std::mutex> lock(queue_mutex_);
  while (!queue_.empty()) {
    auto &item = queue_.front();
    InferResult fail;
    fail.success = false;
    item.promise.set_value(std::move(fail));
    queue_.pop();
  }
}

void BatchInferenceEngine::run_batch(
    std::vector<InferRequest> &requests,
    std::vector<std::promise<InferResult>> &promises) {

  int batch_size = static_cast<int>(requests.size());
  size_t per_image = 3 * input_h_ * input_w_;
  size_t total = batch_size * per_image;

  // Batch utilization logging (every 50 batches)
  static std::atomic<int> batch_count{0};
  static std::atomic<int> total_frames{0};
  int cnt = batch_count.fetch_add(1, std::memory_order_relaxed) + 1;
  total_frames.fetch_add(batch_size, std::memory_order_relaxed);
  if (cnt % 50 == 0) {
    float avg = static_cast<float>(total_frames.load(std::memory_order_relaxed)) / cnt;
    fprintf(stderr, "[INFO] BatchEngine: %d batches, avg %.1f frames/batch (this batch=%d)\n",
            cnt, avg, batch_size);
  }

  try {
    std::vector<float> batch_data(total);
    for (int b = 0; b < batch_size; ++b) {
      const float *src = requests[b].blob.ptr<float>();
      std::memcpy(batch_data.data() + b * per_image, src,
                  per_image * sizeof(float));
    }

    std::vector<int64_t> shape = {(int64_t)batch_size, 3, (int64_t)input_h_,
                                  (int64_t)input_w_};

    auto input_tensor = Ort::Value::CreateTensor<float>(
        ort_->memory_info, batch_data.data(), total, shape.data(),
        shape.size());

    auto output_tensors = ort_->session->Run(
        Ort::RunOptions{nullptr}, ort_->input_names.data(), &input_tensor, 1,
        ort_->output_names.data(), ort_->output_names.size());

    if (output_tensors.empty()) {
      for (auto &p : promises) {
        InferResult fail;
        fail.success = false;
        p.set_value(std::move(fail));
      }
      return;
    }

    auto &out_tensor = output_tensors[0];
    auto *data = out_tensor.GetTensorMutableData<float>();
    auto out_shape = out_tensor.GetTensorTypeAndShapeInfo().GetShape();

    if (out_shape.size() < 2) {
      for (auto &p : promises) {
        InferResult fail;
        fail.success = false;
        p.set_value(std::move(fail));
      }
      return;
    }

    size_t out_total = 1;
    for (auto d : out_shape)
      out_total *= d;

    if (batch_size == 1) {
      InferResult result;
      result.data.assign(data, data + out_total);
      result.shape = out_shape;
      result.success = true;
      promises[0].set_value(std::move(result));
      return;
    }

    if (out_total % static_cast<size_t>(batch_size) != 0) {
      for (auto &p : promises) {
        InferResult fail;
        fail.success = false;
        p.set_value(std::move(fail));
      }
      return;
    }

    size_t per_batch = out_total / batch_size;
    std::vector<int64_t> single_shape = out_shape;
    single_shape[0] = 1;

    for (int b = 0; b < batch_size; ++b) {
      InferResult result;
      result.data.assign(data + b * per_batch, data + (b + 1) * per_batch);
      result.shape = single_shape;
      result.success = true;
      promises[b].set_value(std::move(result));
    }

  } catch (const std::exception &e) {
    fprintf(stderr, "[ERROR] BatchEngine: Inference failed (batch=%d): %s\n",
            batch_size, e.what());
    for (int b = 0; b < batch_size; ++b) {
      try {
        size_t sz = per_image;
        std::vector<int64_t> shape = {1, 3, (int64_t)input_h_,
                                      (int64_t)input_w_};
        auto tensor = Ort::Value::CreateTensor<float>(
            ort_->memory_info,
            const_cast<float *>(requests[b].blob.ptr<float>()), sz,
            shape.data(), shape.size());

        auto outs = ort_->session->Run(
            Ort::RunOptions{nullptr}, ort_->input_names.data(), &tensor, 1,
            ort_->output_names.data(), ort_->output_names.size());

        if (!outs.empty()) {
          auto *d = outs[0].GetTensorMutableData<float>();
          auto s = outs[0].GetTensorTypeAndShapeInfo().GetShape();
          size_t total_out = 1;
          for (auto x : s)
            total_out *= x;

          InferResult result;
          result.data.assign(d, d + total_out);
          result.shape = s;
          result.success = true;
          promises[b].set_value(std::move(result));
        } else {
          InferResult fail;
          fail.success = false;
          promises[b].set_value(std::move(fail));
        }
      } catch (...) {
        InferResult fail;
        fail.success = false;
        promises[b].set_value(std::move(fail));
      }
    }
  }
}

} // namespace yolo_edge
