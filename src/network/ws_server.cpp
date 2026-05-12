#include "network/ws_server.hpp"
#include <App.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <vector>

namespace yolo_edge {

struct WebSocketServer::Impl {
  uWS::Loop *loop = nullptr;
  us_listen_socket_t *listen_socket = nullptr;
};

struct ConnectionState {
  void *ws = nullptr;
  bool closed = false;
};

struct PendingRequest {
  json header;
  std::string request_id;
  std::chrono::steady_clock::time_point received_at;
};

struct PendingBatchRequest {
  json header;                 // full infer_batch header (config, features, etc.)
  std::vector<json> frames;    // per-frame {request_id, frame_id}
  std::vector<size_t> sizes;   // byte size of each JPEG in the concatenated binary
  std::chrono::steady_clock::time_point received_at;
};

struct PerSocketData {
  std::string session_id;
  std::optional<PendingRequest> pending;
  std::optional<PendingBatchRequest> pending_batch;
  std::shared_ptr<ConnectionState> connection;
};

using WsHandle = uWS::WebSocket<false, true, PerSocketData>;

namespace {

constexpr auto kPendingHeaderTimeout = std::chrono::seconds(60);

void defer_send_text(uWS::Loop *loop,
                     const std::shared_ptr<ConnectionState> &connection,
                     json response) {
  if (!loop || !connection) {
    return;
  }

  loop->defer([connection, response = std::move(response)]() mutable {
    if (connection->closed || connection->ws == nullptr) {
      return;
    }

    auto *ws = static_cast<WsHandle *>(connection->ws);
    ws->send(response.dump(), uWS::OpCode::TEXT);
  });
}

bool execute_default_pipeline(Session &session, ProcessingContext &ctx) {
  using Clock = std::chrono::high_resolution_clock;
  const auto start = Clock::now();

  int tracker_idx = session.pipeline.find_index("ByteTracker");
  const size_t split =
      (tracker_idx >= 0) ? static_cast<size_t>(tracker_idx)
                         : session.pipeline.size();
  bool success = session.pipeline.execute_range(ctx, 0, split);
  if (session.pipeline.size() <= split) {
    const auto end = Clock::now();
    ctx.total_time_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    return success;
  }

  const bool tracker_turn_ready = session.wait_for_turn(ctx.frame_id);
  if (!tracker_turn_ready) {
    const auto end = Clock::now();
    ctx.total_time_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    return success;
  }
  try {
    if (success) {
      success = session.pipeline.execute_range(ctx, split, session.pipeline.size());
    }
  } catch (...) {
    session.advance_turn();
    throw;
  }
  session.advance_turn();

  const auto end = Clock::now();
  ctx.total_time_ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  return success;
}

bool copy_array_field(const json &src, const char *key, json &dst,
                      size_t exact_size = 0) {
  if (!src.contains(key) || !src[key].is_array()) {
    return false;
  }
  if (exact_size > 0 && src[key].size() != exact_size) {
    return false;
  }
  dst[key] = src[key];
  return true;
}

bool has_camera_params(const json &cfg) {
  return cfg.is_object() && cfg.contains("camera_matrix") &&
         cfg["camera_matrix"].is_array() && cfg["camera_matrix"].size() == 9 &&
         cfg.contains("dist_coeffs") && cfg["dist_coeffs"].is_array();
}

bool has_geo_params(const json &cfg) {
  return cfg.is_object() && cfg.contains("homography") &&
         cfg["homography"].is_array() && cfg["homography"].size() == 9 &&
         cfg.contains("origin_lon") && cfg["origin_lon"].is_number() &&
         cfg.contains("origin_lat") && cfg["origin_lat"].is_number();
}

} // namespace

WebSocketServer::WebSocketServer(int port, ThreadPool &pool)
    : port_(port), pool_(pool), impl_(std::make_unique<Impl>()) {}

WebSocketServer::~WebSocketServer() { stop(); }

void WebSocketServer::set_session_timeout(std::chrono::seconds timeout) {
  session_timeout_ = timeout;
}

void WebSocketServer::run() {
  running_ = true;
  impl_->loop = uWS::Loop::get();

  const json limits = server_config_.value("limits", json::object());
  const int max_payload =
      limits.value("max_payload_bytes", 100 * 1024 * 1024);

  uWS::App()
      .ws<PerSocketData>(
          "/*",
          {.compression = uWS::SHARED_COMPRESSOR,
           .maxPayloadLength = static_cast<unsigned int>(max_payload),
           .idleTimeout = 120,
           .maxBackpressure = 16 * 1024 * 1024,

           .open = [](auto *ws) {
             auto *data = ws->getUserData();
             data->connection = std::make_shared<ConnectionState>();
             data->connection->ws = ws;
             data->connection->closed = false;
             data->session_id =
                 "session_" + std::to_string(std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count());
             data->pending.reset();
             fprintf(stderr, "[INFO] Client connected: %s\n",
                     data->session_id.c_str());
           },

           .message = [this](auto *ws, std::string_view message,
                             uWS::OpCode opCode) {
             handle_message(ws, message, opCode == uWS::OpCode::BINARY);
           },

           .close = [this](auto *ws, int code, std::string_view message) {
             (void)code;
             (void)message;
             auto *data = ws->getUserData();
             if (data->connection) {
               data->connection->closed = true;
               data->connection->ws = nullptr;
             }
             data->pending.reset();
             data->pending_batch.reset();
             fprintf(stderr, "[INFO] Client disconnected: %s\n",
                     data->session_id.c_str());
             sessions_.retire(data->session_id);
           }})
      .listen(port_, [this](auto *listen_socket) {
        if (listen_socket) {
          impl_->listen_socket = listen_socket;
          fprintf(stderr, "[INFO] WebSocket server listening on port %d\n",
                  port_);
        } else {
          fprintf(stderr, "[ERROR] Failed to listen on port %d\n", port_);
          running_ = false;
        }
      })
      .run();

  running_ = false;
}

void WebSocketServer::stop() {
  if (running_ && impl_->listen_socket) {
    us_listen_socket_close(0, impl_->listen_socket);
    impl_->listen_socket = nullptr;
    running_ = false;
  }
}

void WebSocketServer::merge_defaults(json &target, const json &defaults) {
  if (!defaults.is_object()) {
    return;
  }

  for (auto it = defaults.begin(); it != defaults.end(); ++it) {
    if (!target.contains(it.key())) {
      target[it.key()] = it.value();
    } else if (target[it.key()].is_object() && it.value().is_object()) {
      merge_defaults(target[it.key()], it.value());
    }
  }
}

void WebSocketServer::clamp_number(json &obj, const char *key, double min_value,
                                   double max_value) {
  if (!obj.contains(key) || !obj[key].is_number()) {
    return;
  }
  const double value = obj[key].get<double>();
  obj[key] = std::clamp(value, min_value, max_value);
}

json WebSocketServer::sanitize_client_overrides(const json &request) const {
  json sanitized = json::object();
  if (!request.contains("config") || !request["config"].is_object()) {
    return sanitized;
  }

  const json &cfg = request["config"];
  const json limits = server_config_.value("limits", json::object());

  if (cfg.contains("yolo") && cfg["yolo"].is_object()) {
    json yolo = json::object();
    const auto &src = cfg["yolo"];
    if (src.contains("confidence") && src["confidence"].is_number()) {
      yolo["confidence"] = src["confidence"];
    }
    if (src.contains("nms_threshold") && src["nms_threshold"].is_number()) {
      yolo["nms_threshold"] = src["nms_threshold"];
    }
    clamp_number(yolo, "confidence", limits.value("confidence_min", 0.05),
                 limits.value("confidence_max", 0.95));
    clamp_number(yolo, "nms_threshold", limits.value("nms_min", 0.1),
                 limits.value("nms_max", 0.95));
    if (!yolo.empty()) {
      sanitized["yolo"] = std::move(yolo);
    }
  }

  if (cfg.contains("tracker") && cfg["tracker"].is_object()) {
    json tracker = json::object();
    const auto &src = cfg["tracker"];
    for (const char *key : {"track_thresh", "high_thresh", "match_thresh"}) {
      if (src.contains(key) && src[key].is_number()) {
        tracker[key] = src[key];
      }
    }
    for (const char *key : {"max_time_lost", "min_hits"}) {
      if (src.contains(key) && src[key].is_number_integer()) {
        tracker[key] = src[key];
      }
    }
    clamp_number(tracker, "track_thresh",
                 limits.value("tracker_thresh_min", 0.05),
                 limits.value("tracker_thresh_max", 0.99));
    clamp_number(tracker, "high_thresh",
                 limits.value("tracker_thresh_min", 0.05),
                 limits.value("tracker_thresh_max", 0.99));
    clamp_number(tracker, "match_thresh",
                 limits.value("match_thresh_min", 0.1),
                 limits.value("match_thresh_max", 0.99));
    if (!tracker.empty()) {
      sanitized["tracker"] = std::move(tracker);
    }
  }

  if (cfg.contains("undistort") && cfg["undistort"].is_object()) {
    json undistort = json::object();
    const auto &src = cfg["undistort"];
    copy_array_field(src, "camera_matrix", undistort, 9);
    copy_array_field(src, "dist_coeffs", undistort);
    if (has_camera_params(undistort)) {
      sanitized["undistort"] = std::move(undistort);
    }
  }

  if (cfg.contains("geo_transform") && cfg["geo_transform"].is_object()) {
    json geo = json::object();
    const auto &src = cfg["geo_transform"];
    copy_array_field(src, "homography", geo, 9);
    if (src.contains("origin_lon") && src["origin_lon"].is_number()) {
      geo["origin_lon"] = src["origin_lon"];
    }
    if (src.contains("origin_lat") && src["origin_lat"].is_number()) {
      geo["origin_lat"] = src["origin_lat"];
    }
    if (src.contains("camera_matrix") && src.contains("dist_coeffs")) {
      copy_array_field(src, "camera_matrix", geo, 9);
      copy_array_field(src, "dist_coeffs", geo);
    }
    if (has_geo_params(geo)) {
      sanitized["geo_transform"] = std::move(geo);
    }
  }

  return sanitized;
}

std::vector<std::string> WebSocketServer::build_pipeline(const json &features,
                                                         const json &config) const {
  std::vector<std::string> pipeline{"decoder", "yolo"};

  const bool enable_undistort =
      features.value("undistort", false) && config.contains("undistort") &&
      has_camera_params(config["undistort"]);
  const bool enable_tracker = features.value("tracker", true);
  const bool enable_geo = features.value("geo_transform", false) &&
                          config.contains("geo_transform") &&
                          has_geo_params(config["geo_transform"]);

  if (enable_undistort) {
    pipeline.push_back("undistort");
  }
  if (enable_tracker) {
    pipeline.push_back("tracker");
  }
  if (enable_geo) {
    pipeline.push_back("geo_transform");
  }

  return pipeline;
}

json WebSocketServer::build_pipeline_config(const json &request) const {
  json effective_config = server_config_.value("defaults", json::object());
  json features = server_config_.value("features", json::object());

  if (request.contains("features") && request["features"].is_object()) {
    const auto &src = request["features"];
    for (const char *key : {"tracker", "undistort", "geo_transform"}) {
      if (src.contains(key) && src[key].is_boolean()) {
        features[key] = src[key];
      }
    }
  } else if (request.contains("pipeline") && request["pipeline"].is_array()) {
    for (const auto &item : request["pipeline"]) {
      if (!item.is_string()) {
        continue;
      }
      const auto name = item.get<std::string>();
      if (name == "tracker") {
        features["tracker"] = true;
      } else if (name == "undistort") {
        features["undistort"] = true;
      } else if (name == "geo_transform") {
        features["geo_transform"] = true;
      }
    }
  }

  json overrides = sanitize_client_overrides(request);
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    if (effective_config.contains(it.key()) && effective_config[it.key()].is_object() &&
        it.value().is_object()) {
      effective_config[it.key()].merge_patch(it.value());
    } else {
      effective_config[it.key()] = it.value();
    }
  }

  if (overrides.contains("undistort")) {
    features["undistort"] = true;
  }
  if (overrides.contains("geo_transform")) {
    features["geo_transform"] = true;
  }

  return {{"pipeline", build_pipeline(features, effective_config)},
          {"config", effective_config}};
}

json WebSocketServer::build_error(const std::string &code,
                                  const std::string &message,
                                  const std::string &request_id,
                                  const json &extra) const {
  json response = {{"type", "error"},
                   {"request_id", request_id},
                   {"error", message},
                   {"code", code}};
  if (extra.is_object()) {
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      response[it.key()] = it.value();
    }
  }
  return response;
}

void WebSocketServer::handle_message(void *ws_ptr, std::string_view message,
                                     bool is_binary) {
  auto *ws = static_cast<WsHandle *>(ws_ptr);
  auto *socket_data = ws->getUserData();
  auto *loop = impl_->loop;

  static std::atomic<uint64_t> cleanup_tick{0};
  if ((cleanup_tick.fetch_add(1, std::memory_order_relaxed) & 0xFF) == 0) {
    sessions_.cleanup_expired(session_timeout_);
  }

  const auto now = std::chrono::steady_clock::now();
  if (socket_data->pending.has_value() &&
      now - socket_data->pending->received_at > kPendingHeaderTimeout) {
    const auto expired_request_id = socket_data->pending->request_id;
    socket_data->pending.reset();
    ws->send(build_error("header_timeout",
                         "Timed out waiting for binary frame after infer_header",
                         expired_request_id, json::object())
                 .dump(),
             uWS::OpCode::TEXT);
  }
  if (socket_data->pending_batch.has_value() &&
      now - socket_data->pending_batch->received_at > kPendingHeaderTimeout) {
    socket_data->pending_batch.reset();
    ws->send(build_error("header_timeout",
                         "Timed out waiting for binary frame after infer_batch",
                         "", json::object())
                 .dump(),
             uWS::OpCode::TEXT);
  }

  const json limits = server_config_.value("limits", json::object());
  const size_t max_requests_per_session =
      static_cast<size_t>(limits.value("max_requests_per_session", 8));
  const size_t max_payload_bytes =
      static_cast<size_t>(limits.value("max_payload_bytes", 100 * 1024 * 1024));

  if (is_binary) {
    // ── Batch binary: split concatenated JPEGs and dispatch all at once ──
    if (socket_data->pending_batch.has_value()) {
      auto batch = std::move(*socket_data->pending_batch);
      socket_data->pending_batch.reset();

      if (message.size() > max_payload_bytes) {
        ws->send(build_error("payload_too_large", "Batch binary is too large",
                             "", json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }

      size_t expected = 0;
      for (auto s : batch.sizes) expected += s;
      if (message.size() != expected) {
        ws->send(
            build_error("payload_size_mismatch",
                        "Batch binary size (" + std::to_string(message.size()) +
                            ") != sum of sizes (" + std::to_string(expected) + ")",
                        "", json::object())
                .dump(),
            uWS::OpCode::TEXT);
        return;
      }

      auto connection = socket_data->connection;
      auto default_session_id = socket_data->session_id;
      size_t offset = 0;
      const size_t batch_count = batch.frames.size();

      // Shared collector: each thread stores its result; the last one sends all.
      struct BatchCollector {
        std::mutex mutex;
        std::vector<json> results;
        std::atomic<size_t> remaining;
      };
      auto collector = std::make_shared<BatchCollector>();
      collector->results.resize(batch_count);
      collector->remaining.store(batch_count);

      for (size_t i = 0; i < batch_count; ++i) {
        auto image_data = std::make_shared<std::vector<uint8_t>>(
            message.begin() + offset,
            message.begin() + offset + batch.sizes[i]);
        offset += batch.sizes[i];

        json frame_request = batch.header;
        frame_request["request_id"] = batch.frames[i].value("request_id", "");
        frame_request["frame_id"] = batch.frames[i].value("frame_id", 0);

        const auto request_id = frame_request.value("request_id", "");

        auto task = pool_.try_enqueue(
            [this, i, loop, frame_request, image_data, connection,
             default_session_id, max_requests_per_session, collector]() {
              json response;
              std::shared_ptr<Session> session;
              try {
                ProcessingContext ctx;
                ctx.request_id = frame_request.value("request_id", "");
                ctx.session_id =
                    frame_request.value("session_id", default_session_id);
                ctx.frame_id = frame_request.value("frame_id", 0);
                ctx.set("image_binary", image_data);

                json pipeline_config = build_pipeline_config(frame_request);
                session =
                    sessions_.get_or_create(ctx.session_id, pipeline_config);
                if (!session->try_acquire_request_slot(
                        max_requests_per_session)) {
                  response = build_error(
                      "session_busy",
                      "Too many outstanding requests for session",
                      ctx.request_id, {{"session_id", ctx.session_id}});
                } else {
                  struct SessionLease {
                    std::shared_ptr<Session> session;
                    ~SessionLease() {
                      if (session)
                        session->release_request_slot();
                    }
                  } lease{session};

                  if (session->is_retired()) {
                    response =
                        build_error("stale_session", "Session was reset",
                                    ctx.request_id,
                                    {{"session_id", ctx.session_id}});
                  } else {
                    bool success = execute_default_pipeline(*session, ctx);
                    if (session->is_retired()) {
                      response = build_error(
                          "stale_session",
                          "Session was reset during processing", ctx.request_id,
                          {{"session_id", ctx.session_id}});
                    } else {
                      response = build_response(ctx, success);
                    }
                  }
                }
              } catch (const std::exception &e) {
                response =
                    build_error("internal_error", e.what(),
                                frame_request.value("request_id", ""),
                                json::object());
              }

              // Store result at its index; last thread sends the combined batch.
              {
                std::lock_guard<std::mutex> lock(collector->mutex);
                collector->results[i] = std::move(response);
              }
              if (collector->remaining.fetch_sub(1) == 1) {
                json batch_response = {{"type", "batch_result"},
                                       {"results", collector->results}};
                defer_send_text(loop, connection, std::move(batch_response));
              }
            });

        if (!task.has_value()) {
          // Frame couldn't be enqueued — store error and count it.
          {
            std::lock_guard<std::mutex> lock(collector->mutex);
            collector->results[i] =
                build_error("queue_full", "Server is busy, try again",
                            request_id, json::object());
          }
          if (collector->remaining.fetch_sub(1) == 1) {
            json batch_response = {{"type", "batch_result"},
                                   {"results", collector->results}};
            defer_send_text(loop, connection, std::move(batch_response));
          }
        }
      }
      return;
    }

    // ── Single-frame binary (legacy infer_header + binary) ──
    if (!socket_data->pending.has_value()) {
      ws->send(build_error("unexpected_binary",
                           "Received binary image frame without infer_header",
                           "", json::object())
                   .dump(),
               uWS::OpCode::TEXT);
      return;
    }

    if (message.size() > max_payload_bytes) {
      const auto request_id = socket_data->pending->request_id;
      socket_data->pending.reset();
      ws->send(build_error("payload_too_large", "Binary payload is too large",
                           request_id, json::object())
                   .dump(),
               uWS::OpCode::TEXT);
      return;
    }

    json request = socket_data->pending->header;
    socket_data->pending.reset();

    auto image_data =
        std::make_shared<std::vector<uint8_t>>(message.begin(), message.end());

    auto connection = socket_data->connection;
    auto default_session_id = socket_data->session_id;
    const auto request_id = request.value("request_id", "");

    auto task = pool_.try_enqueue(
        [this, loop, request, image_data, connection, default_session_id,
         max_requests_per_session]() {
          json response;
          std::shared_ptr<Session> session;
          try {
            ProcessingContext ctx;
            ctx.request_id = request.value("request_id", "");
            ctx.session_id = request.value("session_id", default_session_id);
            ctx.frame_id = request.value("frame_id", 0);
            ctx.set("image_binary", image_data);

            json pipeline_config = build_pipeline_config(request);
            session = sessions_.get_or_create(ctx.session_id, pipeline_config);
            if (!session->try_acquire_request_slot(max_requests_per_session)) {
              response = build_error(
                  "session_busy", "Too many outstanding requests for session",
                  ctx.request_id, {{"session_id", ctx.session_id}});
              defer_send_text(loop, connection, std::move(response));
              return;
            }

            struct SessionLease {
              std::shared_ptr<Session> session;
              ~SessionLease() {
                if (session) {
                  session->release_request_slot();
                }
              }
            } lease{session};

            if (session->is_retired()) {
              response = build_error("stale_session", "Session was reset",
                                     ctx.request_id,
                                     {{"session_id", ctx.session_id}});
            } else {
              bool success = execute_default_pipeline(*session, ctx);
              if (session->is_retired()) {
                response = build_error(
                    "stale_session", "Session was reset during processing",
                    ctx.request_id, {{"session_id", ctx.session_id}});
              } else {
                response = build_response(ctx, success);
              }
            }
          } catch (const std::exception &e) {
            response = build_error("internal_error", e.what(),
                                   request.value("request_id", ""),
                                   json::object());
          }

          defer_send_text(loop, connection, std::move(response));
        });

    if (!task.has_value()) {
      defer_send_text(loop, connection,
                      build_error("queue_full", "Server is busy, try again",
                                  request_id, json::object()));
    }
    return;
  }

  try {
    json request = json::parse(message);
    if (!request.is_object()) {
      ws->send(build_error("invalid_request", "Request must be a JSON object",
                           "", json::object())
                   .dump(),
               uWS::OpCode::TEXT);
      return;
    }

    std::string request_type = request.value("type", "");

    if (request_type == "infer_header") {
      const auto request_id = request.value("request_id", "");
      const int frame_id = request.value("frame_id", 0);

      if (request_id.empty()) {
        ws->send(build_error("invalid_request", "infer_header requires request_id",
                             "", json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      if (frame_id <= 0) {
        ws->send(build_error("invalid_request", "infer_header requires frame_id > 0",
                             request_id, json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      if (request.contains("config") && !request["config"].is_object()) {
        ws->send(build_error("invalid_request", "config must be a JSON object",
                             request_id, json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      if (request.contains("features") && !request["features"].is_object()) {
        ws->send(build_error("invalid_request", "features must be a JSON object",
                             request_id, json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }

      if (socket_data->pending.has_value()) {
        ws->send(build_error("previous_header_unpaired",
                             "Previous infer_header has not received its binary frame",
                             request_id, json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }

      socket_data->session_id =
          request.value("session_id", socket_data->session_id);
      socket_data->pending = PendingRequest{request, request_id, now};
      return;
    }

    if (request_type == "infer_batch") {
      if (!request.contains("frames") || !request["frames"].is_array() ||
          request["frames"].empty()) {
        ws->send(build_error("invalid_request",
                             "infer_batch requires non-empty frames array", "",
                             json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      if (!request.contains("sizes") || !request["sizes"].is_array()) {
        ws->send(build_error("invalid_request",
                             "infer_batch requires sizes array", "",
                             json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      const auto &frames = request["frames"];
      const auto &sizes = request["sizes"];
      if (frames.size() != sizes.size()) {
        ws->send(build_error("invalid_request",
                             "frames and sizes arrays must have same length", "",
                             json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }
      if (socket_data->pending.has_value() ||
          socket_data->pending_batch.has_value()) {
        ws->send(build_error("previous_header_unpaired",
                             "Previous header has not received its binary frame",
                             "", json::object())
                     .dump(),
                 uWS::OpCode::TEXT);
        return;
      }

      PendingBatchRequest batch;
      batch.header = request;
      batch.header.erase("frames");
      batch.header.erase("sizes");
      for (const auto &f : frames) {
        batch.frames.push_back(f);
      }
      for (const auto &s : sizes) {
        batch.sizes.push_back(s.get<size_t>());
      }
      batch.received_at = now;

      socket_data->session_id =
          request.value("session_id", socket_data->session_id);
      socket_data->pending_batch = std::move(batch);
      return;
    }

    if (request_type == "ping") {
      json pong = {{"type", "pong"},
                   {"request_id", request.value("request_id", "")}};
      ws->send(pong.dump(), uWS::OpCode::TEXT);
      return;
    }

    if (request_type == "reset") {
      std::string rid = request.value("request_id", "");
      std::string sid = request.value("session_id", socket_data->session_id);
      socket_data->session_id = sid;
      socket_data->pending.reset();
      socket_data->pending_batch.reset();
      sessions_.retire(sid);
      json response = {{"type", "reset_ack"},
                       {"request_id", rid},
                       {"session_id", sid}};
      ws->send(response.dump(), uWS::OpCode::TEXT);
      return;
    }

    if (request_type == "infer") {
      ws->send(build_error(
                   "legacy_infer_removed",
                   "Legacy JSON infer is no longer supported. Use infer_header followed by a binary image frame.",
                   request.value("request_id", ""), json::object())
                   .dump(),
               uWS::OpCode::TEXT);
      return;
    }

    if (request_type.empty()) {
      ws->send(build_error("invalid_request", "Missing type in request",
                           request.value("request_id", ""), json::object())
                   .dump(),
               uWS::OpCode::TEXT);
      return;
    }

    ws->send(build_error("unknown_type", "Unknown type: " + request_type,
                         request.value("request_id", ""), json::object())
                 .dump(),
             uWS::OpCode::TEXT);
    return;

  } catch (const std::exception &e) {
    json err = build_error("invalid_json", "Invalid JSON: " + std::string(e.what()),
                           "", json::object());
    ws->send(err.dump(), uWS::OpCode::TEXT);
  }
}

json WebSocketServer::build_response(const ProcessingContext &ctx, bool success) {
  json response = {{"type", success ? "result" : "error"},
                   {"request_id", ctx.request_id},
                   {"session_id", ctx.session_id},
                   {"frame_id", ctx.frame_id}};

  if (!success) {
    response["error"] = "Processing failed";
    return response;
  }

  json detections = json::array();
  detections.get_ref<json::array_t &>().reserve(ctx.detections.size());

  for (const auto &det : ctx.detections) {
    json det_json;

    det_json["track_id"] = det.track_id;
    det_json["class_id"] = det.class_id;
    det_json["class_name"] = det.class_name;
    det_json["confidence"] = det.confidence;
    det_json["angle"] = det.obb.angle;

    if (det.geometry_ready) {
      det_json["obb"] = {det.obb_points[0], det.obb_points[1], det.obb_points[2],
                          det.obb_points[3], det.obb_points[4], det.obb_points[5],
                          det.obb_points[6], det.obb_points[7]};
      det_json["bbox"] = {det.bbox[0], det.bbox[1], det.bbox[2], det.bbox[3]};
    } else {
      cv::Point2f pts[4];
      det.obb.points(pts);
      det_json["obb"] = {pts[0].x, pts[0].y, pts[1].x, pts[1].y,
                         pts[2].x, pts[2].y, pts[3].x, pts[3].y};
      cv::Rect rect = det.obb.boundingRect();
      det_json["bbox"] = {rect.x, rect.y, rect.width, rect.height};
    }

    if (det.ground_x.has_value())
      det_json["ground_x"] = det.ground_x.value();
    if (det.ground_y.has_value())
      det_json["ground_y"] = det.ground_y.value();
    if (det.lon.has_value())
      det_json["lon"] = det.lon.value();
    if (det.lat.has_value())
      det_json["lat"] = det.lat.value();

    detections.push_back(std::move(det_json));
  }
  response["detections"] = std::move(detections);

  response["timing"] = {{"decode_ms", ctx.decode_time_ms},
                        {"infer_ms", ctx.infer_time_ms},
                        {"track_ms", ctx.track_time_ms},
                        {"geo_ms", ctx.geo_time_ms},
                        {"total_ms", ctx.total_time_ms}};

  return response;
}

json WebSocketServer::build_error(const std::string &message,
                                  const std::string &request_id) {
  return {{"type", "error"}, {"request_id", request_id}, {"error", message}};
}

} // namespace yolo_edge
