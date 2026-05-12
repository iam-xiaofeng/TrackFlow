#include "core/processor_factory.hpp"
#include "network/ws_server.hpp"
#include "utils/config.hpp"
#include "utils/thread_pool.hpp"

#include "processors/byte_tracker.hpp"
#include "processors/geo_transformer.hpp"
#include "processors/image_decoder.hpp"
#include "processors/undistort_processor.hpp"
#include "processors/yolo_detector.hpp"

#include <csignal>
#include <cstdio>
#include <iostream>

namespace {

yolo_edge::json build_server_config(const yolo_edge::Config &config) {
  using yolo_edge::json;

  json server_config = {
      {"defaults",
       {{"yolo",
         {{"model_path", "models/3class410.onnx"},
          {"input_width", 1024},
          {"input_height", 1024},
          {"confidence", 0.5},
          {"nms_threshold", 0.45},
          {"is_obb", true},
          {"use_cuda", true},
          {"ort_threads", 4},
          {"batch_size", 6},
          {"batch_wait_ms", 30},
          {"batch_max_pending", 16}}},
        {"tracker",
         {{"enabled", true},
          {"track_thresh", 0.5},
          {"high_thresh", 0.6},
          {"match_thresh", 0.8},
          {"max_time_lost", 30},
          {"min_hits", 3}}}}},
      {"features",
       {{"tracker", true}, {"undistort", false}, {"geo_transform", false}}},
      {"limits",
       {{"session_timeout_sec", 300},
        {"max_payload_bytes", 100 * 1024 * 1024},
        {"max_pending_tasks", 32},
        {"max_pending_batches", 16},
        {"max_requests_per_session", 8},
        {"ort_threads", 4},
        {"batch_size", 6},
        {"batch_wait_ms", 8},
        {"confidence_min", 0.05},
        {"confidence_max", 0.95},
        {"nms_min", 0.1},
        {"nms_max", 0.95},
        {"tracker_thresh_min", 0.05},
        {"tracker_thresh_max", 0.99},
        {"match_thresh_min", 0.1},
        {"match_thresh_max", 0.99}}}};

  if (auto sec = config.get_section("runtime_defaults"); !sec.empty()) {
    server_config["defaults"].merge_patch(sec);
  }
  if (auto sec = config.get_section("features"); !sec.empty()) {
    server_config["features"].merge_patch(sec);
  }
  if (auto sec = config.get_section("limits"); !sec.empty()) {
    server_config["limits"].merge_patch(sec);
  }

  for (const auto &section : {"yolo", "tracker"}) {
    auto sec = config.get_section(section);
    if (!sec.empty()) {
      server_config["defaults"][section].merge_patch(sec);
    }
  }

  return server_config;
}

void signal_handler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    fprintf(stderr, "[INFO] Received signal %d, shutting down...\n", signal);
  }
}

void print_usage(const char *program) {
  std::cout << "Usage: " << program << " [OPTIONS]\n\n"
            << "Options:\n"
            << "  -p, --port PORT      Listen port (default: 9001)\n"
            << "  -t, --threads NUM    Thread pool size (default: 2)\n"
            << "  -c, --config PATH    Config file path\n"
            << "  -v, --verbose        Enable debug logging\n"
            << "  -h, --help           Show this help\n"
            << std::endl;
}

struct Args {
  int port = 9001;
  int threads = 2;
  std::string config_path;
  bool verbose = false;
  yolo_edge::json server_config;
};

Args parse_args(int argc, char *argv[]) {
  Args args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "-v" || arg == "--verbose") {
      args.verbose = true;
    } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
      args.port = std::stoi(argv[++i]);
    } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
      args.threads = std::stoi(argv[++i]);
    } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
      args.config_path = argv[++i];
    } else {
      std::cerr << "Unknown option: " << arg << std::endl;
      print_usage(argv[0]);
      std::exit(1);
    }
  }

  return args;
}

void setup_logging(bool verbose) {
  if (verbose) {
    fprintf(stderr, "[INFO] Verbose logging enabled (approximate)\n");
  }
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  Args args = parse_args(argc, argv);
  setup_logging(args.verbose);

  fprintf(stderr, "=================================\n");
  fprintf(stderr, "  TrackFlow YOLO Edge Server\n");
  fprintf(stderr, "=================================\n");

  if (!args.config_path.empty()) {
    try {
      auto config = yolo_edge::Config::load(args.config_path);
      args.port = config.get_nested("server.port", args.port);
      args.threads = config.get_nested("server.threads", args.threads);
      args.server_config = build_server_config(config);
      fprintf(stderr, "[INFO] Server config defaults: %s\n",
              args.server_config.dump().c_str());
    } catch (const std::exception &e) {
      fprintf(stderr, "[WARN] Failed to load config: %s\n", e.what());
    }
  }

  auto &factory = yolo_edge::ProcessorFactory::instance();
  auto types = factory.registered_types();
  fprintf(stderr, "[INFO] Registered processors: %zu\n", types.size());
  for (const auto &type : types) {
    fprintf(stderr, "  - %s\n", type.c_str());
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  const auto max_pending_tasks =
      static_cast<size_t>(args.server_config.value("limits", yolo_edge::json::object())
                              .value("max_pending_tasks", args.threads * 4));

  fprintf(stderr, "[INFO] Creating thread pool with %d threads\n",
          args.threads);
  yolo_edge::ThreadPool pool(args.threads, max_pending_tasks);

  fprintf(stderr, "[INFO] Starting WebSocket server on port %d\n", args.port);
  yolo_edge::WebSocketServer server(args.port, pool);
  if (!args.server_config.empty()) {
    server.set_server_config(args.server_config);
    const auto timeout_sec = args.server_config.value("limits", yolo_edge::json::object())
                                 .value("session_timeout_sec", 300);
    server.set_session_timeout(std::chrono::seconds(timeout_sec));
  }

  try {
    server.run();
  } catch (const std::exception &e) {
    fprintf(stderr, "[ERROR] Server error: %s\n", e.what());
    return 1;
  }

  fprintf(stderr, "[INFO] Server stopped\n");
  return 0;
}
