#include <atomic>
#include <fstream>
#include <exception>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <future>
#include <string>
#include <vector>

#include "quickapp/core/foundation/app_runtime_factory.h"
#include "quickapp/core/package/package_loader.h"
#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/observation.h"
#include "quickapp/js/framework/static_facade_catalog.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"

namespace qc = quickapp::core;
namespace qp = quickapp::core::package;
namespace qj = quickapp::js;
namespace ja = quickapp::js::abi;

namespace {

class Source final : public qp::PackageSource {
 public:
  explicit Source(qp::Bytes bytes) : bytes_(std::make_shared<const qp::Bytes>(std::move(bytes))) {}
  qc::RuntimeResult<std::uint64_t> size() noexcept override {
    return closed_ ? qc::RuntimeResult<std::uint64_t>::failure(qc::RuntimeError::simple(qc::RuntimeErrorCode::kPackageIoError, "closed"))
                   : qc::RuntimeResult<std::uint64_t>::success(bytes_->size());
  }
  qc::EnqueueResult read_at(qp::PackageReadRequest request, qp::PackageReadCompletion completion) noexcept override {
    if (closed_ || !completion || request.offset > bytes_->size() || request.length > bytes_->size() - request.offset)
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(qc::RuntimeErrorCode::kPackageIoError, "invalid read"));
    auto value = std::make_shared<qp::Bytes>(bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset), bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset + request.length));
    completion(qp::PackageReadResult{std::move(request.request_id), qc::RuntimeResult<qp::ImmutableBytes>::success(std::move(value))});
    return qc::EnqueueResult::success(qc::Accepted{});
  }
  void close() noexcept override { closed_ = true; }
 private:
  std::shared_ptr<const qp::Bytes> bytes_;
  bool closed_{false};
};

qp::Bytes read_file(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open RPK");
  input.seekg(0, std::ios::end); const auto n = input.tellg(); input.seekg(0, std::ios::beg);
  qp::Bytes bytes(static_cast<std::size_t>(n)); input.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!input) throw std::runtime_error("cannot read RPK"); return bytes;
}

class Clock final : public qj::MonotonicClock {
 public: std::uint64_t nowNs() const noexcept override { return tick_.fetch_add(100, std::memory_order_relaxed); }
 private: mutable std::atomic<std::uint64_t> tick_{1000};
};
class Sink final : public qj::TraceSink { public: void emit(const qj::TraceEvent&) noexcept override {} };
class Completion final : public qj::module::ModuleCompletionPort {
 public:
  qj::module::ModuleEnqueueResult post(const qj::module::ModuleLoadCompletion& value) noexcept override {
    status = value.status;
    if (value.error) error = value.error->message;
    std::fprintf(stderr, "module_completion kind=%s id=%s status=%s error=%s\n", value.moduleKind.c_str(), value.moduleId.c_str(), value.status.c_str(), error.c_str());
    return {qj::module::ModuleEnqueueStatus::Accepted};
  }
  std::string status;
  std::string error;
};

}  // namespace

int main(int argc, char** argv) {
  std::set_terminate([] {
    std::fprintf(stderr, "case001_js_terminate\n");
    if (auto exception = std::current_exception()) {
      try { std::rethrow_exception(exception); }
      catch (const std::exception& error) {
        std::fprintf(stderr, "terminate_exception=%s\n", error.what());
      } catch (...) { std::fprintf(stderr, "terminate_exception=unknown\n"); }
    }
    std::_Exit(134);
  });
  const char* artifact = argc > 1 ? argv[1] : "../quickapp-toolkit/evidence/tk-s07-case001.rpk";
  try {
    std::fprintf(stderr, "phase=load\n");
    auto source = std::make_shared<Source>(read_file(artifact));
    qc::AppRuntimeFactory factory; auto identity = std::move(factory.create()).value();
    qp::RuntimeComposition composition;
    composition.runtime_abi = "quickapp-kit-runtime-v1"; composition.engine_abi = "quickapp-kit-js-engine-v1";
    composition.components = {"View", "Text", "Button"};
    composition.capabilities = {"system.router", "system.prompt", "system.device", "system.fetch", "system.shortcut"};
    auto loader = std::move(qp::PackageLoader::create(source, identity.request_ids(), std::move(composition))).value();
    std::optional<std::shared_ptr<const qp::VerifiedPackage>> package;
    std::string failure;
    if (!loader->open([&](auto result) {
          if (!result) { failure = std::string(result.error().message); return; }
          package = std::move(result).value();
        })) throw std::runtime_error("open enqueue failed");
    if (!failure.empty() || !package) throw std::runtime_error("RPK open failed: " + failure);

    std::optional<qp::VerifiedModule> app; std::optional<qp::VerifiedModule> page;
    const auto& page_desc = package.value()->pages().at("/pages/Demo");
    if (!loader->load_module({"@quickapp-kit/app", std::nullopt}, [&](auto result) {
          if (!result) { failure = std::string(result.error().message); return; }
          app = std::move(result).value();
        })) throw std::runtime_error("app module enqueue failed");
    const auto surface = qc::SurfaceId::parse("srf:case001").value();
    if (!loader->load_module({page_desc.module_id, surface}, [&](auto result) {
          if (!result) { failure = std::string(result.error().message); return; }
          page = std::move(result).value();
        })) throw std::runtime_error("page module enqueue failed");
    if (!failure.empty() || !app || !page) throw std::runtime_error("module callbacks incomplete: " + failure);

    auto provider = std::make_unique<qj::QuickJsEngineProvider>(); const auto descriptor = provider->describe();
    Clock clock; Sink sink; auto registration = qj::TraceSinkRegistration::admit(sink, {.nonblocking = true, .noReentry = true});
    if (!registration.ok()) throw std::runtime_error("trace registration failed");
    qj::JsEngineConfig config; config.expectedEngine = descriptor; config.limits.maxPendingTasks = 32;
    qj::JsEngineService engine("app:case001", std::move(provider), config, clock, std::move(registration).value(), {false, "case001-js", "steady", 0});
    std::promise<qj::ServiceResult> started; if (!engine.start([&](qj::ServiceResult result) { started.set_value(std::move(result)); })) throw std::runtime_error("engine start failed");
    if (!started.get_future().get().ok()) throw std::runtime_error("QuickJS start failed");
    std::fprintf(stderr, "phase=engine_started\n");
    Completion completion;
    auto *facade_raw = new qj::framework::StaticFacadeCatalog();
    qj::module::ModuleLoader *module_raw = nullptr;
    std::promise<bool> loaded; auto loaded_future = loaded.get_future(); std::atomic<bool> loaded_once{false};
    if (engine.post([&](qj::JsEnginePort& js, const qj::JsContextRef& context) {
      const auto finish = [&](bool value) { if (!loaded_once.exchange(true)) loaded.set_value(value); };
      try {
      std::fprintf(stderr, "phase=js_callback_start\n");
      if (!facade_raw->startOnExecutor(js, context)) { finish(false); return; }
      std::fprintf(stderr, "phase=facades\n");
      module_raw = new qj::module::ModuleLoader(engine, completion, "app:case001", package.value()->package_id(), qj::module::ModuleLoaderLimits{}, facade_raw);
      if (!module_raw->startOnExecutor(js, context)) { finish(false); return; }
      std::fprintf(stderr, "phase=modules_started\n");
      std::uint64_t js_request_sequence = 0;
      const auto send = [&](const qp::VerifiedModule& value, std::string kind, std::optional<std::string> surface_id, std::optional<ja::BootstrapExpectation> bootstrap, std::optional<std::vector<std::uint64_t>> bindings, std::optional<std::vector<std::uint64_t>> handlers = std::nullopt) {
        ja::LoadVerifiedModule message; message.requestId = "req:j-" + std::to_string(++js_request_sequence); message.packageId = value.package_id(); message.moduleKind = std::move(kind); message.moduleId = value.module_id(); message.cacheScope = value.cache_scope() == qp::ModuleCacheScope::kAppRuntime ? "appRuntime" : "surface"; message.surfaceId = std::move(surface_id); message.dependencies = value.dependencies(); message.bundle = {value.descriptor().path, value.descriptor().byte_length, value.descriptor().sha256, std::make_shared<const std::vector<std::uint8_t>>(*value.bytes())}; message.expectedBootstrap = std::move(bootstrap); message.expectedBindingIds = std::move(bindings); message.expectedHandlerIds = std::move(handlers); module_raw->onLoadVerifiedModule(message);
      };
      // The RPK declares a shared-module DAG. The loader's require path is
      // intentionally strict, so load the verified modules in topological order.
      for (const auto& module_id : {
             std::string_view{"@quickapp-kit/shared/helper/utils"},
             std::string_view{"@quickapp-kit/shared/helper/ajax"},
             std::string_view{"@quickapp-kit/shared/helper/apis/example"},
             std::string_view{"@quickapp-kit/shared/helper/apis/index"}}) {
        auto it = package.value()->modules().find(std::string(module_id));
        if (it == package.value()->modules().end()) { finish(false); return; }
        std::optional<qp::VerifiedModule> shared;
        if (!loader->load_module({it->first, std::nullopt}, [&](auto result) {
              if (!result) { failure = std::string(result.error().message); return; }
              shared = std::move(result).value();
            }) || !shared || !failure.empty()) { finish(false); return; }
        send(*shared, "shared", std::nullopt, std::nullopt, std::nullopt);
      }
      send(*app, "app", std::nullopt, ja::BootstrapExpectation{"app", app->module_id(), std::nullopt}, std::nullopt);
      std::fprintf(stderr, "phase=app_sent\n");
      if (!module_raw->openSurfaceOnExecutor(surface.wire())) { finish(false); return; }
      send(*page, "page", surface.wire(), ja::BootstrapExpectation{"page", page->module_id(), page->expected_template_id()}, page->expected_binding_ids(), page->expected_handler_ids());
      std::fprintf(stderr, "phase=page_sent\n");
      const bool module_success = completion.status == "loaded";
      std::fprintf(stderr, "phase=finish value=%d status=%s error=%s\n", module_success ? 1 : 0, completion.status.c_str(), completion.error.c_str());
      module_raw->stopOnExecutor();
      facade_raw->stopOnExecutor();
      finish(module_success);
      } catch (...) { finish(false); }
    }).status != qj::PostStatus::Accepted) throw std::runtime_error("JS composition enqueue failed");
    std::fprintf(stderr, "phase=await_loaded\n");
    auto *modules_to_cleanup = module_raw;
    auto *facades_to_cleanup = facade_raw;
    bool loaded_ok = false;
    try { loaded_ok = loaded_future.get(); }
    catch (const std::exception &error) { std::fprintf(stderr, "loaded_wait_error=%s\n", error.what()); }
    std::fprintf(stderr, "phase=loaded_result value=%d\n", loaded_ok ? 1 : 0);
    if (!loaded_ok) throw std::runtime_error("real App/Page module load failed");
    std::fprintf(stderr, "phase=modules_loaded\n");
    std::cout << "rpk.opened=true\napp.module.loaded=true\npage.module.loaded=true\nquickjs.started=true\n";
    std::fprintf(stderr, "phase=engine_stop\n");
    std::promise<void> stopped;
    if (!engine.stop([] {}, [&stopped] { std::fprintf(stderr, "phase=engine_stopped\n"); stopped.set_value(); })) throw std::runtime_error("QuickJS stop failed");
    stopped.get_future().get();
    delete modules_to_cleanup;
    delete facades_to_cleanup;
    loader->close(); identity.reset(); factory.stop(); if (!factory.teardown()) throw std::runtime_error("teardown failed");
    std::cout << "resources_released=true\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "case001_js_error=" << error.what() << '\n'; return 1; }
}
