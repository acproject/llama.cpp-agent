#include "agent-routes.h"
#include "agent-session.h"

// server-context.h already included via agent-server.h -> agent-loop.h
#include "../subagent/subagent-runner.h"
#include "server-http.h"
#include "server-models.h"
#include "../tool-registry.h"
#include "../agent-loop.h"


#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"

#include "../subagent/subagent-display.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// MCP support (Unix only)
#ifndef _WIN32
#include "../mcp/mcp-server-manager.h"
#include "../mcp/mcp-tool-wrapper.h"
#endif

#include <atomic>
#include <csignal>
#include <functional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

static std::function<void(int)> shutdown_handler;
static std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

static inline void signal_handler(int signal) {
  if (is_terminating.test_and_set()) {
    fprintf(stderr, "Received second interrupt, terminating immediately.\n");
    exit(1);
  }
  shutdown_handler(signal);
}


// Wrapper to handle execeptions in handlers
static server_http_context::handler_t
ex_wrapper(server_http_context::handler_t func) {
  return [func = std::move(func)](const server_http_req &req) -> server_http_res_ptr {
    std::string message;
    error_type error;
    try {
      return func(req);
    } catch (const std::invalid_argument &e) {
      error = ERROR_TYPE_INVALID_REQUEST;
      message = e.what();
    } catch (const std::exception &e) {
      error = ERROR_TYPE_SERVER;
      message = e.what();
    } catch (...) {
      error = ERROR_TYPE_SERVER;
      message = "unknown error";
    }

    auto res = std::make_unique<server_http_res>();
    res->status = 500;
    try {
      json error_data = format_error_response(message, error);
      res->status = json_value(error_data, "code", 500);
      res->data = safe_json_to_str({{"error", error_data}});
      SRV_WRN("got exception: %s\n", res->data.c_str());
    } catch (const std::exception &e) {
      SRV_ERR("got another exception: %s | while handling exception: %s\n", e.what(),
              message.c_str());
      res->data = "Internal Server Error";
    }
    return res;
};
}

int main(int argc, char *argv[]) {
  common_params params;

  // Parse custom flag before common_params_parse
  int max_subagent_depth = 0; // Default: subagent disabled
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--subagent") {
      max_subagent_depth = 1;
      for (int j = i + 1; j < argc; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--;
    } else if (arg == "--no-subagents") {
      max_subagent_depth = 0;
      for (int j = i; j < argc; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--;
    } else if (arg == "--max-subagent-depth") {
        if (i + 1 < argc) {
          try {
            max_subagent_depth = std::stoi(argv[i + 1]);
            if (max_subagent_depth < 0)
              max_subagent_depth = 0;
            if (max_subagent_depth > 5)
              max_subagent_depth = 5;
          } catch (...) {
            fprintf(stderr, "Invalid --max-subagent-depth value: %s\n", argv[i + 1]);
           return 1;
          }
          for (int j = i + 1; j < argc; j++) {
            argv[j] = argv[j + 1];
          }
          argc -= 2;
          i--;
    } else {
      fprintf(stderr, "--max-subagent-depth requires a value\n");
      return 1;
    }
  }
  }

  // Set subagent depth before anything else
  subagent_display::instance().set_max_depth(max_subagent_depth);

  if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
    return  1;
  }
  // Set defaults for agent server
  if (params.n_parallel < 0) {
    params.n_parallel = 4;
    params.kv_unified = true;
  }

  if (params.model_alias.empty() && !params.model.name.empty()) {
    params.model_alias = params.model.name;
  }

  common_init();

  // Initialize server context (holds LLM inference)
  server_context ctx_server;

  llama_backend_init();
  llama_numa_init(params.numa);

  LOG_INF("llama-agent-server starting\n");
  LOG_INF("system info: n_threads = %d, n_threads_batch = % d,total_threads = "
          "% d\n ",
          params.cpuparams.n_threads, params.cpuparams_batch.n_threads,
          std::thread::hardware_concurrency());
  LOG_INF("\n");
  LOG_INF("%s\n", common_params_get_system_info(params).c_str());
  LOG_INF("\n");

  // Initialize HTTP context
  server_http_context ctx_http;
  if (!ctx_http.init(params)) {
    LOG_ERR("Failed to initialize HTTP server\n");
    return 1;
  }

  // Llama-server compatible routes (OpenAI-compatible /v1/*, /health, etc.)
  server_routes server_api(params, ctx_server);

  const bool is_router_server = params.model.path.empty();
  std::optional<server_models_routes> models_routes;

  std::unique_ptr<agent_session_manager> session_mgr;
  std::unique_ptr<agent_routes> agent_api;

  if (is_router_server) {
    try {
      models_routes.emplace(params, argc, argv);
    } catch (const std::exception & e) {
      LOG_ERR("Failed to initialize router models: %s\n", e.what());
      return 1;
    }

    server_api.get_metrics                 = models_routes->proxy_get;
    server_api.post_props                  = models_routes->proxy_post;
    server_api.get_api_show                = models_routes->proxy_get;
    server_api.post_completions            = models_routes->proxy_post;
    server_api.post_completions_oai        = models_routes->proxy_post;
    server_api.post_chat_completions       = models_routes->proxy_post;
    server_api.post_responses_oai          = models_routes->proxy_post;
    server_api.post_anthropic_messages     = models_routes->proxy_post;
    server_api.post_anthropic_count_tokens = models_routes->proxy_post;
    server_api.post_infill                 = models_routes->proxy_post;
    server_api.post_embeddings             = models_routes->proxy_post;
    server_api.post_embeddings_oai         = models_routes->proxy_post;
    server_api.post_rerank                 = models_routes->proxy_post;
    server_api.post_tokenize               = models_routes->proxy_post;
    server_api.post_detokenize             = models_routes->proxy_post;
    server_api.post_apply_template         = models_routes->proxy_post;
    server_api.get_lora_adapters           = models_routes->proxy_get;
    server_api.post_lora_adapters          = models_routes->proxy_post;
    server_api.get_slots                   = models_routes->proxy_get;
    server_api.post_slots                  = models_routes->proxy_post;

    server_api.get_props  = models_routes->get_router_props;
    server_api.get_models = models_routes->get_router_models;

    ctx_http.post("/models/load", ex_wrapper(models_routes->post_router_models_load));
    ctx_http.post("/models/unload", ex_wrapper(models_routes->post_router_models_unload));
  } else {
    session_mgr = std::make_unique<agent_session_manager>(ctx_server, params);
    agent_api = std::make_unique<agent_routes>(*session_mgr);
  }

  ctx_http.get("/health",              ex_wrapper(server_api.get_health));
  ctx_http.get("/v1/health",           ex_wrapper(server_api.get_health));
  ctx_http.get("/metrics",             ex_wrapper(server_api.get_metrics));
  ctx_http.get("/props",               ex_wrapper(server_api.get_props));
  ctx_http.post("/props",              ex_wrapper(server_api.post_props));
  ctx_http.post("/api/show",           ex_wrapper(server_api.get_api_show));
  ctx_http.get("/models",              ex_wrapper(server_api.get_models));
  ctx_http.get("/v1/models",           ex_wrapper(server_api.get_models));
  ctx_http.get("/api/tags",            ex_wrapper(server_api.get_models));
  ctx_http.post("/completion",         ex_wrapper(server_api.post_completions));
  ctx_http.post("/completions",        ex_wrapper(server_api.post_completions));
  ctx_http.post("/v1/completions",     ex_wrapper(server_api.post_completions_oai));
  ctx_http.post("/chat/completions",   ex_wrapper(server_api.post_chat_completions));
  ctx_http.post("/v1/chat/completions", ex_wrapper(server_api.post_chat_completions));
  ctx_http.post("/api/chat",           ex_wrapper(server_api.post_chat_completions));
  ctx_http.post("/v1/responses",       ex_wrapper(server_api.post_responses_oai));
  ctx_http.post("/v1/messages",        ex_wrapper(server_api.post_anthropic_messages));
  ctx_http.post("/v1/messages/count_tokens", ex_wrapper(server_api.post_anthropic_count_tokens));
  ctx_http.post("/infill",             ex_wrapper(server_api.post_infill));
  ctx_http.post("/embedding",          ex_wrapper(server_api.post_embeddings));
  ctx_http.post("/embeddings",         ex_wrapper(server_api.post_embeddings));
  ctx_http.post("/v1/embeddings",      ex_wrapper(server_api.post_embeddings_oai));
  ctx_http.post("/rerank",             ex_wrapper(server_api.post_rerank));
  ctx_http.post("/reranking",          ex_wrapper(server_api.post_rerank));
  ctx_http.post("/v1/rerank",          ex_wrapper(server_api.post_rerank));
  ctx_http.post("/v1/reranking",       ex_wrapper(server_api.post_rerank));
  ctx_http.post("/tokenize",           ex_wrapper(server_api.post_tokenize));
  ctx_http.post("/detokenize",         ex_wrapper(server_api.post_detokenize));
  ctx_http.post("/apply-template",     ex_wrapper(server_api.post_apply_template));
  ctx_http.get("/lora-adapters",       ex_wrapper(server_api.get_lora_adapters));
  ctx_http.post("/lora-adapters",      ex_wrapper(server_api.post_lora_adapters));
  ctx_http.get("/slots",               ex_wrapper(server_api.get_slots));
  ctx_http.post("/slots/:id_slot",     ex_wrapper(server_api.post_slots));
  ctx_http.post("/slots",              ex_wrapper(server_api.post_slots));

  if (!is_router_server) {
    ctx_http.get("/v1/agent/health", ex_wrapper(agent_api->get_health));
    ctx_http.post("/v1/agent/session", ex_wrapper(agent_api->post_session));
    ctx_http.get("/v1/agent/session/:id", ex_wrapper(agent_api->get_session));
    ctx_http.post("/v1/agent/session/:id/delete", ex_wrapper(agent_api->delete_session));
    ctx_http.get("/v1/agent/sessions", ex_wrapper(agent_api->get_sessions));
    ctx_http.post("/v1/agent/session/:id/chat", ex_wrapper(agent_api->post_chat));
    ctx_http.get("/v1/agent/session/:id/messages", ex_wrapper(agent_api->get_messages));
    ctx_http.get("/v1/agent/session/:id/permissions", ex_wrapper(agent_api->get_permissions));
    ctx_http.post("/v1/agent/permission/:id", ex_wrapper(agent_api->post_permission));
    ctx_http.get("/v1/agent/tools", ex_wrapper(agent_api->get_tools));
    ctx_http.get("/v1/agent/session/:id/stats", ex_wrapper(agent_api->get_stats));
  } else {
    auto proxy_agent_get = [&models_routes](const server_http_req & req) -> server_http_res_ptr {
      if (!models_routes.has_value()) {
        throw std::runtime_error("router models are not initialized");
      }
      if (req.get_param("model").empty()) {
        throw std::invalid_argument("Missing 'model' query parameter");
      }
      return models_routes->proxy_get(req);
    };

    auto proxy_agent_post = [&models_routes](const server_http_req & req) -> server_http_res_ptr {
      if (!models_routes.has_value()) {
        throw std::runtime_error("router models are not initialized");
      }
      std::string model = req.get_param("model");
      if (model.empty() && !req.body.empty()) {
        try {
          json body = json::parse(req.body);
          if (body.contains("model") && body["model"].is_string()) {
            model = body["model"].get<std::string>();
          }
        } catch (...) {
        }
      }
      if (model.empty()) {
        throw std::invalid_argument("Missing model");
      }

      json body = json::object();
      if (!req.body.empty()) {
        try {
          body = json::parse(req.body);
        } catch (const std::exception &e) {
          throw std::invalid_argument(std::string("Invalid JSON: ") + e.what());
        }
      }
      body["model"] = model;
      server_http_req req2 = req;
      req2.body = body.dump();
      return models_routes->proxy_post(req2);
    };

    ctx_http.get("/v1/agent/health", ex_wrapper([](const server_http_req &) -> server_http_res_ptr {
      auto res = std::make_unique<server_http_res>();
      res->status = 200;
      res->data = json{{"status", "ok"}}.dump();
      return res;
    }));

    ctx_http.post("/v1/agent/session", ex_wrapper(proxy_agent_post));
    ctx_http.get("/v1/agent/session/:id", ex_wrapper(proxy_agent_get));
    ctx_http.post("/v1/agent/session/:id/delete", ex_wrapper(proxy_agent_post));
    ctx_http.get("/v1/agent/sessions", ex_wrapper(proxy_agent_get));
    ctx_http.post("/v1/agent/session/:id/chat", ex_wrapper(proxy_agent_post));
    ctx_http.get("/v1/agent/session/:id/messages", ex_wrapper(proxy_agent_get));
    ctx_http.get("/v1/agent/session/:id/permissions", ex_wrapper(proxy_agent_get));
    ctx_http.post("/v1/agent/permission/:id", ex_wrapper(proxy_agent_post));

    ctx_http.get("/v1/agent/tools", ex_wrapper([](const server_http_req &) -> server_http_res_ptr {
      auto tools = tool_registry::instance().to_chat_tools();
      json response = json::array();
      for (const auto & tool : tools) {
        response.push_back({
          {"name", tool.name},
          {"description", tool.description},
          {"parameters", json::parse(tool.parameters)},
        });
      }
      auto res = std::make_unique<server_http_res>();
      res->status = 200;
      res->data = json{{"tools", response}}.dump();
      return res;
    }));

    ctx_http.get("/v1/agent/session/:id/stats", ex_wrapper(proxy_agent_get));
  }

  // Setup cleanup
  auto clean_up = [&ctx_http, &ctx_server, &models_routes, is_router_server]() {
    LOG_INF("Cleaning up before exit...\n");
    ctx_http.stop();
    if (!is_router_server) {
      ctx_server.terminate();
    }
    if (is_router_server && models_routes.has_value()) {
      models_routes->models.unload_all();
    }
    llama_backend_free();
  };

  // Start HTTP server before loading model (to serve /health)
  if (!ctx_http.start()) {
    clean_up();
    LOG_ERR("Failed to start HTTP server\n");
    return 1;
  }

  shutdown_handler = [&](int) {
    if (is_router_server) {
      ctx_http.stop();
    } else {
      ctx_server.terminate();
    }
  };

  if (is_router_server) {
    ctx_http.is_ready.store(true);
    LOG_INF("\n");
    LOG_INF("==============================================\n");
    LOG_INF("llama-agent-server (router) is listening on %s\n",
            ctx_http.listening_address.c_str());
    LOG_INF("==============================================\n");
    LOG_INF("\n");
    LOG_INF("Router endpoints:\n");
    LOG_INF("  GET  /models                        - List models\n");
    LOG_INF("  POST /models/load                   - Load a model\n");
    LOG_INF("  POST /models/unload                 - Unload a model\n");
    LOG_INF("  GET  /health                        - Health check\n");
    LOG_INF("\n");
    LOG_INF("Agent router requires model selection:\n");
    LOG_INF("  GET endpoints:  ?model=MODEL_ID\n");
    LOG_INF("  POST endpoints: JSON body {\"model\": MODEL_ID, ...} or ?model=MODEL_ID\n");

    if (ctx_http.thread.joinable()) {
      ctx_http.thread.join();
    }
    clean_up();
    LOG_INF("llama-agent-server stoped\n");
    return 0;
  }

  // load the model
  LOG_INF("Loading model...\n");

  if (!ctx_server.load_model(params)) {
    clean_up();
    if (ctx_http.thread.joinable()) {
      ctx_http.thread.join();
    }
    LOG_ERR("Failed to load model\n");
    return 1;
  }
  server_api.update_meta(ctx_server);
  ctx_http.is_ready.store(true);
  LOG_INF("Modle loaded successfully\n");

  const char * router_port = std::getenv("LLAMA_SERVER_ROUTER_PORT");
  std::thread monitor_thread;
  if (router_port != nullptr) {
    monitor_thread = server_models::setup_child_server(shutdown_handler);
  }

  // Initialize MCP servers (Unix only)
  // MCP manager must be declared here to outlive session manager  (tools hold
  // pointer to it)
#ifndef _WIN32
  mcp_server_manager mcp_mgr;
  int mcp_tools_count = 0;
  std::string working_dir =
      "."; // Default working directory for MCP config search
  std::string mcp_config = find_mcp_config(working_dir);
  if (!mcp_config.empty()) {
    LOG_INF("Loading MCP config from: %s\n", mcp_config.c_str());
    if (mcp_mgr.load_config(mcp_config)) {
      int started = mcp_mgr.start_servers();
      if (started > 0) {
        register_mcp_tools(mcp_mgr);
        mcp_tools_count = static_cast<int>(mcp_mgr.list_all_tools().size());
        LOG_INF("MCP: %d servers started, %d tools registered\n", started, mcp_tools_count);
      }
    }
  }
#else
  int mcp_tools_count = 0;
#endif
  LOG_INF("\n");
  LOG_INF("==============================================\n");
  LOG_INF("llama-agent-server is listening on %s\n",
          ctx_http.listening_address.c_str());
  LOG_INF("==============================================\n");
  LOG_INF("\n");
  if (mcp_tools_count > 0) {
    LOG_INF("MCP tools %d\n", mcp_tools_count);
  }

  LOG_INF("API Endpoints:\n");
  LOG_INF("  POST /v1/agent/session              - Create a new session\n");
  LOG_INF("  GET  /v1/agent/session/:id          - Get session info\n");
  LOG_INF(
      "  POST /v1/agent/session/:id/chat  - Send message (streaming SSE)\n");
  LOG_INF("  GET  /v1/agent/session/:id/messages - Get Conversation history\n");
  LOG_INF("  GET  /v1/agent/tools                - List available tools\n");
  LOG_INF("  GET  /health                        - Health check\n");

  // Start the main inference loop
  ctx_server.start_loop();

  // Clean up after shutdown
  clean_up();

  if (ctx_http.thread.joinable()) {
    ctx_http.thread.join();
  }
  if (monitor_thread.joinable()) {
    monitor_thread.join();
  }
  LOG_INF("llama-agent-server stoped\n");
  return 0;
}
