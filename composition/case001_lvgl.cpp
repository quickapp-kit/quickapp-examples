#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <cstdio>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <SDL3/SDL.h>
#include <lvgl.h>
#include <lvgl/drivers/sdl/lv_sdl_window.h>

#include "quickapp/core/foundation/app_runtime_factory.h"
#include "quickapp/core/package/package_loader.h"
#include "quickapp/core/event/event_router.h"
#include "quickapp/core/render/initial_render_pipeline.h"
#include "quickapp/core/surface/surface_controller.h"
#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/alpha/alpha_page_initialization_stage.h"
#include "quickapp/js/binding/alpha_initial_binding_stage.h"
#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/observation.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"
#include "quickapp/js/event/handler_registry.h"
#include "quickapp/js/framework/static_facade_catalog.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/page/page_host_control.h"
#include "quickapp/js/render/alpha_initial_transaction_builder.h"
#include "quickapp/js/vm/vm_lifecycle_service.h"
#include "quickapp/lvgl/font/system_default_font_asset.h"
#include "quickapp/lvgl/foundation/owner_task_queue.h"
#include "quickapp/lvgl/integration/core_mount_bridge.h"
#include "quickapp/lvgl/measure/font_measure.h"
#include "quickapp/lvgl/mount/lvgl_mount_backend.h"
#include "quickapp/lvgl/mount/mount_host.h"
#include "quickapp/lvgl/surface/lvgl_page_root_backend.h"
#include "quickapp/lvgl/surface/surface_host.h"

namespace qc = quickapp::core;
namespace qp = quickapp::core::package;
namespace qr = quickapp::core::render;
namespace qs = quickapp::core::surface;
namespace qj = quickapp::js;
namespace ja = quickapp::js::abi;
namespace qlf = quickapp::lvgl::foundation;
namespace qli = quickapp::lvgl::integration;
namespace qlm = quickapp::lvgl::mount;
namespace qls = quickapp::lvgl::surface;
namespace qm = quickapp::lvgl::measure;

namespace {

constexpr qlf::OwnerToken kOwner{1};

#if defined(QUICKAPP_EXAMPLES_INTERACTIVE_SIMULATOR)
constexpr bool kInteractiveSimulator = true;
volatile std::sig_atomic_t gSimulatorSignalExit = 0;

struct SimulatorExitState final {
  std::atomic<bool> requested{false};
  SDL_EventFilter previousFilter{nullptr};
  void* previousUserdata{nullptr};
};

void simulatorSignalHandler(int) { gSimulatorSignalExit = 1; }

class SimulatorSignalScope final {
 public:
  SimulatorSignalScope()
      : previousInterrupt_(std::signal(SIGINT, &simulatorSignalHandler)),
        previousTerminate_(std::signal(SIGTERM, &simulatorSignalHandler)) {
    if (previousInterrupt_ == SIG_ERR || previousTerminate_ == SIG_ERR) {
      throw std::runtime_error("Simulator signal handler registration failed");
    }
  }

  SimulatorSignalScope(const SimulatorSignalScope&) = delete;
  SimulatorSignalScope& operator=(const SimulatorSignalScope&) = delete;

  ~SimulatorSignalScope() {
    std::signal(SIGINT, previousInterrupt_);
    std::signal(SIGTERM, previousTerminate_);
  }

 private:
  using SignalHandler = void (*)(int);
  SignalHandler previousInterrupt_;
  SignalHandler previousTerminate_;
};

bool simulatorEventFilter(void* userdata, SDL_Event* event) {
  auto* state = static_cast<SimulatorExitState*>(userdata);
  if (state != nullptr && event != nullptr &&
      (event->type == SDL_EVENT_QUIT ||
       event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
    state->requested.store(true, std::memory_order_release);
    return false;
  }
  return state == nullptr || state->previousFilter == nullptr ||
         state->previousFilter(state->previousUserdata, event);
}

class SimulatorEventFilterScope final {
 public:
  explicit SimulatorEventFilterScope(SimulatorExitState& state) : state_(state) {
    SDL_GetEventFilter(&state_.previousFilter, &state_.previousUserdata);
    SDL_SetEventFilter(&simulatorEventFilter, &state_);
  }

  SimulatorEventFilterScope(const SimulatorEventFilterScope&) = delete;
  SimulatorEventFilterScope& operator=(const SimulatorEventFilterScope&) = delete;

  ~SimulatorEventFilterScope() {
    SDL_SetEventFilter(state_.previousFilter, state_.previousUserdata);
  }

 private:
  SimulatorExitState& state_;
};
#else
constexpr bool kInteractiveSimulator = false;
#endif

constexpr float kRuntimeViewportWidth = kInteractiveSimulator ? 720.0F : 320.0F;
constexpr float kRuntimeViewportHeight = kInteractiveSimulator ? 1280.0F : 240.0F;

class Source final : public qp::PackageSource {
 public:
  explicit Source(qp::Bytes bytes) : bytes_(std::make_shared<const qp::Bytes>(std::move(bytes))) {}
  qc::RuntimeResult<std::uint64_t> size() noexcept override { return qc::RuntimeResult<std::uint64_t>::success(bytes_->size()); }
  qc::EnqueueResult read_at(qp::PackageReadRequest request, qp::PackageReadCompletion completion) noexcept override {
    if (!completion || request.offset > bytes_->size() || request.length > bytes_->size() - request.offset) return qc::EnqueueResult::failure(qc::RuntimeError::simple(qc::RuntimeErrorCode::kPackageIoError, "invalid read"));
    auto value = std::make_shared<qp::Bytes>(bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset), bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset + request.length));
    completion(qp::PackageReadResult{std::move(request.request_id), qc::RuntimeResult<qp::ImmutableBytes>::success(std::move(value))});
    return qc::EnqueueResult::success(qc::Accepted{});
  }
  void close() noexcept override { closed_ = true; }
 private:
  std::shared_ptr<const qp::Bytes> bytes_;
  bool closed_{false};
};

qp::Bytes readFile(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open RPK");
  input.seekg(0, std::ios::end); const auto size = input.tellg(); input.seekg(0, std::ios::beg);
  qp::Bytes bytes(static_cast<std::size_t>(size)); input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) throw std::runtime_error("cannot read RPK"); return bytes;
}

qc::RequestId request(std::string value) { return qc::RequestId::parse(std::move(value)).value(); }
class SurfaceResults final : public qc::CoreIngressPort<qls::SurfaceResult> {
 public:
  explicit SurfaceResults(qli::CoreMountBridge& bridge) : bridge_(bridge) {}
  qc::EnqueueResult post(qls::SurfaceResult&& value) noexcept override { return bridge_.acceptSurfaceResult(std::move(value)); }
  void close() noexcept override {}
 private: qli::CoreMountBridge& bridge_;
};

class SurfaceContent final : public qls::SurfaceContentLifecyclePort {
 public:
  void bind(qlm::MountHost& mounts) noexcept { mounts_ = &mounts; }
  [[nodiscard]] qlf::LocalResult canRelease(
      const qc::SurfaceId&) noexcept override {
    return mounts_ ? qlf::LocalResult::success()
                   : qlf::LocalResult::failure(qlf::LocalError::kInvalidState);
  }
  void releaseNoFail(const qc::SurfaceId& surfaceId) noexcept override {
    if (mounts_ != nullptr)
      static_cast<void>(mounts_->releaseSurface(kOwner, surfaceId));
  }
  void resetNoFail(const qc::SurfaceId& surfaceId) noexcept override {
    releaseNoFail(surfaceId);
  }
 private:
  qlm::MountHost* mounts_{nullptr};
};

class AppState final : public qs::AppRuntimeStateView {
 public:
  [[nodiscard]] qc::lifecycle::AppRuntimeState state() const noexcept override {
    return qc::lifecycle::AppRuntimeState::kForeground;
  }
};

class PageResolver final : public qs::VerifiedPageResolver {
 public:
  PageResolver(qp::PackageLoader& loader,
               std::shared_ptr<const qp::VerifiedPackage> package) noexcept
      : loader_(loader), package_(std::move(package)) {}

  [[nodiscard]] qc::RuntimeResult<qs::VerifiedSurfacePage> resolve(
      std::string_view route, const qc::SurfaceId& surfaceId) noexcept override {
    const auto descriptor = package_->pages().find(std::string(route));
    if (descriptor == package_->pages().end()) {
      return fail(qc::RuntimeErrorCode::kRouteNotFound, "route is absent from RPK");
    }
    std::optional<qp::VerifiedModule> module;
    std::optional<qp::PageIrHandle> pageIr;
    std::optional<qc::RuntimeError> failure;
    if (!loader_.load_module({descriptor->second.module_id, surfaceId},
                             [&](auto result) {
          if (result) module = std::move(result).value();
          else failure = result.error();
        }) || failure || !module) {
      return fail(failure ? failure->code : qc::RuntimeErrorCode::kPackageIoError,
                  failure ? failure->message : "page module load was rejected");
    }
    if (!loader_.load_page_ir(std::string(route), [&](auto result) {
          if (result) pageIr = std::move(result).value();
          else failure = result.error();
        }) || failure || !pageIr) {
      return fail(failure ? failure->code : qc::RuntimeErrorCode::kPackageIoError,
                  failure ? failure->message : "page IR load was rejected");
    }
    return qc::RuntimeResult<qs::VerifiedSurfacePage>::success(
        {std::string(route), std::move(*module), std::move(*pageIr)});
  }

 private:
  static qc::RuntimeResult<qs::VerifiedSurfacePage> fail(
      qc::RuntimeErrorCode code, std::string_view message) noexcept {
    return qc::RuntimeResult<qs::VerifiedSurfacePage>::failure(
        qc::RuntimeError::simple(code, message));
  }

  qp::PackageLoader& loader_;
  std::shared_ptr<const qp::VerifiedPackage> package_;
};

class CoreSurfaceResultIngress final
    : public qc::CoreIngressPort<qls::SurfaceResult> {
 public:
  void bind(qs::SurfaceController& controller) noexcept { controller_ = &controller; }

  qc::EnqueueResult post(qls::SurfaceResult&& result) noexcept override {
    if (controller_ == nullptr) return rejected("SurfaceController is unavailable");
    auto converted = std::visit([](auto&& value) -> qs::SurfaceCommandResult {
      using T = std::decay_t<decltype(value)>;
      constexpr bool visibility = std::is_same_v<T, qls::SetSurfaceVisibilityResult>;
      constexpr bool push = std::is_same_v<T, qls::PresentPushSurfaceHostResult>;
      constexpr bool close = std::is_same_v<T, qls::CloseSurfaceHostResult>;
      qs::SurfaceCommandKind kind = qs::SurfaceCommandKind::kDestroy;
      bool completed = value.status != qls::SurfaceResultStatus::kFailed;
      if constexpr (std::is_same_v<T, qls::CreateSurfaceHostResult>) {
        kind = qs::SurfaceCommandKind::kCreate;
        completed = value.status == qls::SurfaceResultStatus::kCreated;
      } else if constexpr (std::is_same_v<T, qls::PresentRootSurfaceHostResult> || push) {
        kind = qs::SurfaceCommandKind::kPresent;
        completed = value.status == qls::SurfaceResultStatus::kPresented;
      } else if constexpr (visibility) {
        kind = qs::SurfaceCommandKind::kVisibility;
        completed = value.status == qls::SurfaceResultStatus::kCompleted;
      } else if constexpr (close) {
        kind = qs::SurfaceCommandKind::kClose;
        completed = value.status == qls::SurfaceResultStatus::kCompleted;
      } else {
        completed = value.status == qls::SurfaceResultStatus::kDestroyed;
      }
      std::optional<qc::SurfaceId> source;
      std::optional<qc::SurfaceId> reveal;
      std::optional<qc::lifecycle::SurfaceVisibility> coreVisibility;
      if constexpr (push) source = value.source_surface_id;
      if constexpr (close) {
        source = value.surface_id;
        reveal = value.reveal_surface_id;
      }
      if constexpr (visibility) {
        coreVisibility = value.visibility == qls::SurfaceVisibility::kVisible
                             ? qc::lifecycle::SurfaceVisibility::kVisible
                             : qc::lifecycle::SurfaceVisibility::kHidden;
      }
      return {value.request_id, kind, value.surface_id, std::move(source),
              std::move(reveal), coreVisibility, completed, value.error};
    }, std::move(result));
    return controller_->enqueue(std::move(converted));
  }

  void close() noexcept override {}

 private:
  static qc::EnqueueResult rejected(std::string_view message) noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, message));
  }
  qs::SurfaceController* controller_{nullptr};
};

class SurfacePlatform final : public qs::SurfacePlatformPort {
 public:
  explicit SurfacePlatform(qls::SurfaceHostAdapter& surfaces) noexcept
      : surfaces_(surfaces) {}
  void failNextCreate() noexcept { fail_next_create_ = true; }
  void holdNextCreate() noexcept { hold_next_create_ = true; }
  [[nodiscard]] std::optional<qs::SurfaceCreateHostCommand>
  takeHeldCreate() noexcept {
    auto value = std::move(held_create_);
    held_create_.reset();
    return value;
  }

  qc::EnqueueResult post(qs::SurfaceCommand&& command) noexcept override {
    return std::visit([this](auto&& value) -> qc::EnqueueResult {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, qs::SurfaceCreateHostCommand>) {
        if (hold_next_create_) {
          hold_next_create_ = false;
          held_create_.emplace(std::move(value));
          return qc::EnqueueResult::success(qc::Accepted{});
        }
        if (fail_next_create_) {
          fail_next_create_ = false;
          return qc::EnqueueResult::failure(qc::RuntimeError::simple(
              qc::RuntimeErrorCode::kPlatformRejected,
              "injected Surface creation failure"));
        }
        return surfaces_.post(qls::CreateSurfaceHost{
            std::move(value.request_id), std::move(value.surface_id),
            {kRuntimeViewportWidth, kRuntimeViewportHeight}});
      } else if constexpr (std::is_same_v<T, qs::SurfacePresentCommand>) {
        if (value.mode == qs::SurfacePresentMode::kRoot) {
          return surfaces_.post(qls::PresentRootSurfaceHost{
              std::move(value.request_id), std::move(value.target)});
        }
        if (!value.source) return rejected("push source is absent");
        return surfaces_.post(qls::PresentPushSurfaceHost{
            std::move(value.request_id), std::move(value.target),
            std::move(*value.source)});
      } else if constexpr (std::is_same_v<T, qs::SurfaceVisibilityCommand>) {
        return surfaces_.post(qls::SetSurfaceVisibility{
            std::move(value.request_id), std::move(value.surface_id),
            value.visibility == qc::lifecycle::SurfaceVisibility::kVisible
                ? qls::SurfaceVisibility::kVisible
                : qls::SurfaceVisibility::kHidden});
      } else if constexpr (std::is_same_v<T, qs::SurfaceCloseCommand>) {
        return surfaces_.post(qls::CloseSurfaceHost{
            std::move(value.request_id), std::move(value.source),
            std::move(value.reveal)});
      } else {
        return surfaces_.post(qls::DestroySurfaceHost{
            std::move(value.request_id), std::move(value.surface_id)});
      }
    }, std::move(command));
  }

  void close() noexcept override {}

 private:
  static qc::EnqueueResult rejected(std::string_view message) noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kAbiInvalidArgument, message));
  }
  qls::SurfaceHostAdapter& surfaces_;
  std::optional<qs::SurfaceCreateHostCommand> held_create_;
  bool fail_next_create_{false};
  bool hold_next_create_{false};
};

class PlatformBackPort {
 public:
  virtual ~PlatformBackPort() = default;
  [[nodiscard]] virtual qc::EnqueueResult post(
      qs::NavigationCloseRequest request) noexcept = 0;
};

class PlatformBackIngress final : public PlatformBackPort {
 public:
  void bind(qs::SurfaceController& controller) noexcept {
    controller_ = &controller;
  }

  qc::EnqueueResult post(qs::NavigationCloseRequest request) noexcept override {
    if (controller_ == nullptr) {
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(
          qc::RuntimeErrorCode::kPlatformRejected,
          "Core SurfaceController is unavailable"));
    }
    std::fprintf(stderr,
                 "platform.back.captured request=%s source=%s\n",
                 request.request_id.wire().c_str(), request.source.wire().c_str());
    return controller_->enqueue(qs::SurfaceRequest(std::move(request)));
  }

 private:
  qs::SurfaceController* controller_{nullptr};
};

class ControllerInitialResults final : public qr::InitialContentResultSink {
 public:
  void bind(qs::SurfaceController& controller) noexcept { controller_ = &controller; }
  void complete(qs::InitialContentResult result) noexcept override {
    std::fprintf(stderr,
                 "core.initial.complete request=%s surface=%s completed=%d\n",
                 result.request_id.wire().c_str(),
                 result.surface_id.wire().c_str(), result.prepared ? 1 : 0);
    if (controller_ != nullptr) static_cast<void>(controller_->enqueue(std::move(result)));
  }
  void close() noexcept override {}
 private:
  qs::SurfaceController* controller_{nullptr};
};

class ControllerOperationResults final : public qs::SurfaceOperationResultSink {
 public:
  using Callback = std::function<void(qs::SurfaceOperationKind, qc::RequestId,
                                      std::optional<qc::SurfaceId>, bool,
                                      std::optional<qc::RuntimeError>)>;
  explicit ControllerOperationResults(Callback callback) noexcept
      : callback_(std::move(callback)) {}
  void complete(qs::SurfaceOperationKind kind, qc::RequestId requestId,
                std::optional<qc::SurfaceId> target, bool completed,
                std::optional<qc::RuntimeError> error) noexcept override {
    if (callback_) callback_(kind, std::move(requestId), std::move(target),
                             completed, std::move(error));
  }
  void close() noexcept override {}
 private:
  Callback callback_;
};

class ControllerStatus final : public qs::SurfaceStatusSink {
 public:
  void status(qs::SurfaceStatusChanged change) noexcept override {
    std::fprintf(stderr, "core.surface.status surface=%s lifecycle=%u revision=%llu\n",
                 change.surface_id.wire().c_str(),
                 static_cast<unsigned>(change.lifecycle),
                 static_cast<unsigned long long>(change.revision));
  }
  void close() noexcept override {}
};

class ControllerLifecycleResults final : public qs::SurfaceLifecycleResultSink {
 public:
  void complete(qc::lifecycle::SurfaceLifecycleResult result) noexcept override {
    std::fprintf(stderr, "core.surface.lifecycle request=%s completed=%d\n",
                 result.request_id.wire().c_str(), result.completed ? 1 : 0);
  }
  void close() noexcept override {}
};

class ControllerPageLifecycle final : public qs::PageLifecyclePort {
 public:
  using Handler = std::function<qc::EnqueueResult(qs::PageCommand&&)>;
  explicit ControllerPageLifecycle(Handler handler) noexcept
      : handler_(std::move(handler)) {}
  qc::EnqueueResult post(qs::PageCommand&& command) noexcept override {
    if (!handler_) return rejected();
    return handler_(std::move(command));
  }
  void close() noexcept override { handler_ = {}; }
 private:
  static qc::EnqueueResult rejected() noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, "page lifecycle is closed"));
  }
  Handler handler_;
};

class ControllerInitialPipeline final : public qs::InitialSurfacePipeline {
 public:
  using Handler = std::function<qc::EnqueueResult(qs::InitialContentCommand&&)>;
  explicit ControllerInitialPipeline(Handler handler) noexcept
      : handler_(std::move(handler)) {}
  qc::EnqueueResult post(qs::InitialContentCommand&& command) noexcept override {
    if (!handler_) return rejected();
    return handler_(std::move(command));
  }
  void release_surface(const qc::SurfaceId& surfaceId) noexcept override {
    if (release_) release_(surfaceId);
  }
  void close() noexcept override { handler_ = {}; release_ = {}; }
  void onRelease(std::function<void(const qc::SurfaceId&)> release) noexcept {
    release_ = std::move(release);
  }
 private:
  static qc::EnqueueResult rejected() noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, "initial pipeline is closed"));
  }
  Handler handler_;
  std::function<void(const qc::SurfaceId&)> release_;
};

class MountResults final : public qc::CoreIngressPort<qr::MountTransactionResult> {
 public:
  qc::EnqueueResult post(qr::MountTransactionResult&& value) noexcept override {
    std::fprintf(stderr,
                 "platform.mount.complete source=%s surface=%s attempt=%s revision=%llu mounted=%d transaction=none(initial)\n",
                 qr::render_source_wire(value.source_id).c_str(), value.surface_id.wire().c_str(),
                 value.mount_attempt_id.wire().c_str(),
                 static_cast<unsigned long long>(value.revision),
                 value.mounted ? 1 : 0);
    return coordinator_ ? coordinator_->accept(std::move(value)) : qc::EnqueueResult::failure(qc::RuntimeError::simple(qc::RuntimeErrorCode::kPlatformRejected, "coordinator unavailable"));
  }
  void close() noexcept override {}
  void bind(qr::MountCoordinator& coordinator) noexcept { coordinator_ = &coordinator; }
 private: qr::MountCoordinator* coordinator_{nullptr};
};

class RenderResults final : public qr::RenderTransactionResultSink {
 public:
  void bind(ja::RuntimeAbiService& runtimeAbi) noexcept {
    runtimeAbi_ = &runtimeAbi;
  }

  void complete(qr::RenderTransactionResult result) noexcept override {
    last = result;
    completed = true;
    if (runtimeAbi_ == nullptr) return;
    std::optional<ja::MessageRuntimeError> error;
    if (result.error.has_value()) {
      error = ja::MessageRuntimeError{
          std::string(qc::to_wire(result.error->code)),
          std::string(result.error->message), result.error->retryable,
          result.surface_id.wire(), std::nullopt,
          result.transaction_id.wire(), std::nullopt};
    }
    static_cast<void>(runtimeAbi_->postCallback(
        ja::JsInboundMessage(ja::RenderTransactionResult{
            result.surface_id.wire(), result.transaction_id.wire(),
            result.presented ? "presented" : "presentationFailed",
            result.submitted_revision, result.committed_revision,
            std::move(error)})));
  }

  void close() noexcept override {}

  ja::RuntimeAbiService* runtimeAbi_{nullptr};
  std::optional<qr::RenderTransactionResult> last;
  bool completed{false};
};

class FontResults final : public qc::CoreIngressPort<qm::PlatformFontGenerationChanged> {
 public:
  qc::EnqueueResult post(qm::PlatformFontGenerationChanged&&) noexcept override { return qc::EnqueueResult::success(qc::Accepted{}); }
  void close() noexcept override {}
};

class InitialResults final : public qr::InitialContentResultSink {
 public:
  void complete(qc::surface::InitialContentResult result) noexcept override { prepared = result.prepared; }
  void close() noexcept override {}
  bool prepared{false};
};

class CoreMeasure final : public qr::MeasurePort {
 public:
  explicit CoreMeasure(qm::FontMeasureAdapter& measure) : measure_(measure) {}
  qr::MeasureResult measure(const qr::MeasureRequest& value) noexcept override {
    const auto role = value.role == qr::MeasureRole::kText ? qm::MeasureRole::kText : qm::MeasureRole::kButtonLabel;
    const auto constraint = [](qr::MeasureConstraint value) {
      return qm::MeasureConstraint{value.kind == qr::MeasureConstraintKind::kExactly ? qm::ConstraintKind::kExactly : value.kind == qr::MeasureConstraintKind::kAtMost ? qm::ConstraintKind::kAtMost : qm::ConstraintKind::kUnconstrained, value.value};
    };
    const auto result = measure_.measure({value.request_id.wire(), value.surface_id.wire(), value.node_id.wire(), value.content_revision, value.platform_font_generation, role, value.text, value.font_token, value.font_size, value.font_weight, constraint(value.width_constraint), constraint(value.height_constraint)});
    return {value.request_id, value.surface_id, value.node_id, value.content_revision, value.platform_font_generation, result.measured, result.width, result.height, result.error};
  }
 private: qm::FontMeasureAdapter& measure_;
};

class Clock final : public qj::MonotonicClock {
 public:
  std::uint64_t nowNs() const noexcept override {
    return tick_.fetch_add(100, std::memory_order_relaxed);
  }
 private:
  mutable std::atomic<std::uint64_t> tick_{1000};
};

class TraceSink final : public qj::TraceSink {
 public:
  void emit(const qj::TraceEvent&) noexcept override {}
};

class ModuleCompletion final : public qj::module::ModuleCompletionPort {
 public:
  qj::module::ModuleEnqueueResult post(
      const qj::module::ModuleLoadCompletion& completion) noexcept override {
    status = completion.status;
    error = completion.error ? completion.error->message : "";
    return {qj::module::ModuleEnqueueStatus::Accepted};
  }
  std::string status;
  std::string error;
};

class JsRequestIds final : public qj::framework::JsRequestIdAllocatorPort {
 public:
  std::string nextRequestId() noexcept override {
    return "req:j-" + std::to_string(next_++);
  }
 private:
  std::uint64_t next_{100};
};

class LvglClickToCore final {
 public:
  explicit LvglClickToCore(qc::event::EventRouter& router) noexcept
      : router_(router) {}
  static void callback(void* context, const qc::SurfaceId& surface,
                       const qc::NodeId& node, std::uint64_t timestamp) noexcept {
    auto* self = static_cast<LvglClickToCore*>(context);
    if (self == nullptr) return;
    const auto request = qc::RequestId::parse(
        "req:p-" + std::to_string(++self->sequence_));
    if (!request) return;
    const auto result = self->router_.dispatch(qc::event::PlatformInputMessage{
        request.value(), surface, node, qp::EventType::kClick, timestamp, {}});
    std::fprintf(stderr,
                 "lvgl.input.dispatch request=%s surface=%s node=%s accepted=%d error=%s\n",
                 request.value().wire().c_str(), surface.wire().c_str(),
                 node.wire().c_str(), result ? 1 : 0,
                 result ? "" : std::string(qc::to_wire(result.error().code)).c_str());
  }
 private:
  qc::event::EventRouter& router_;
  std::uint64_t sequence_{0};
};

// This is the composition boundary: JS submits a typed initial template, and
// Core materializes it in the one authoritative RuntimeTreeStore.
class JsCoreIngress final : public ja::CoreIngressPort,
                            public qc::event::JsEventDispatchPort {
 public:
  explicit JsCoreIngress() = default;

  void bindCoordinator(qr::MountCoordinator& coordinator) noexcept {
    coordinator_ = &coordinator;
  }
  void bindPage(qc::SurfaceId surfaceId, qp::PageIrHandle page) noexcept {
    std::lock_guard lock(pagesMutex_);
    pages_[surfaceId.wire()] = std::move(page);
  }
  void bindSurfaceController(qs::SurfaceController& controller) noexcept {
    surfaceController_ = &controller;
  }
  void bindAbi(ja::RuntimeAbiService& runtimeAbi) noexcept {
    runtimeAbi_ = &runtimeAbi;
  }

  qc::EnqueueResult post(qc::event::JsEventDispatch&& message) noexcept override {
    if (runtimeAbi_ == nullptr) {
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(
          qc::RuntimeErrorCode::kPlatformRejected, "JS Runtime ABI is unavailable"));
    }
    std::fprintf(stderr,
                 "core.event.dispatch request=%s surface=%s node_owner=%s template_node=%llu handler=%s\n",
                 message.request_id.wire().c_str(),
                 message.surface_id.wire().c_str(),
                 qc::runtime_tree::owner_wire(message.target.owner).c_str(),
                 static_cast<unsigned long long>(
                     message.target.template_node_id.value()),
                 message.handler_id.wire().c_str());
    ja::JsEventDispatch typed{
        message.request_id.wire(), message.surface_id.wire(),
        message.handler_id.wire(), std::string(qc::event::event_type_wire(message.event_type)),
        message.phase,
        {qc::runtime_tree::owner_wire(message.target.owner),
         message.target.template_node_id.value()},
        {qc::runtime_tree::owner_wire(message.current_target.owner),
         message.current_target.template_node_id.value()},
        static_cast<double>(message.timestamp_ns), {}};
    const auto posted = runtimeAbi_->postCallback(
        ja::JsInboundMessage(std::move(typed)));
    return posted.ok
               ? qc::EnqueueResult::success(qc::Accepted{})
               : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                     qc::RuntimeErrorCode::kQueueOverflow,
                     "JS event callback queue rejected"));
  }

  void close() noexcept override {}

  ja::EnqueueResult post(ja::CoreInboundMessage message) noexcept override {
    try {
      std::fprintf(stderr, "js.core.message kind=%zu\n", message.index());
      if (std::holds_alternative<ja::CompleteVmInitialization>(message)) {
        const auto& complete = std::get<ja::CompleteVmInitialization>(message);
        std::fprintf(stderr, "js.core.complete scope=%s status=%s phase=%s error=%s\n",
                     complete.scope.c_str(), complete.status.c_str(),
                     complete.failedPhase ? complete.failedPhase->c_str() : "",
                     complete.error ? complete.error->message.c_str() : "");
      }
      if (const auto* render = std::get_if<ja::SubmitRenderTransaction>(&message)) {
        const auto surface = qc::SurfaceId::parse(render->surfaceId);
        const auto transaction = qc::TransactionId::parse(render->transactionId);
        if (!surface || !transaction || render->revision == 0 ||
            coordinator_ == nullptr) {
          return rejectTransaction("invalid RenderTransaction identity",
                                  render->surfaceId, render->transactionId);
        }
        std::vector<qc::runtime_tree::BindingUpdate> updates;
        updates.reserve(render->operations.size());
        for (const auto& operation : render->operations) {
          const auto* update = std::get_if<ja::UpdateBindingOperation>(&operation);
          if (update == nullptr) {
            return rejectTransaction("S3.5 accepts only binding updates",
                                    render->surfaceId, render->transactionId);
          }
          const auto owner = qc::ComponentInstanceId::parse(update->ownerInstanceId);
          if (!owner || update->templateBindingId == 0) {
            return rejectTransaction("invalid binding target",
                                    render->surfaceId, render->transactionId);
          }
          const auto value = std::visit(
              [](const auto& item) -> qc::runtime_tree::BindingValue {
                return item;
              }, update->value);
          updates.push_back({owner.value(), update->templateBindingId, value});
        }
        std::optional<qc::RequestId> requestId;
        if (render->requestId.has_value()) {
          const auto parsed = qc::RequestId::parse(*render->requestId);
          if (!parsed) {
            return rejectTransaction("invalid causal request identity",
                                    render->surfaceId, render->transactionId);
          }
          requestId = parsed.value();
        }
        const auto accepted = coordinator_->submit(qr::RenderTransactionIntent{
            surface.value(), transaction.value(), render->revision, requestId,
            std::move(updates)});
        if (!accepted) {
          return rejectTransaction("Core rejected RenderTransaction",
                                  render->surfaceId, render->transactionId);
        }
        return ja::EnqueueResult::accepted();
      }
      if (!std::holds_alternative<ja::InstantiateTemplate>(message)) {
        if (const auto* navigation = std::get_if<ja::NavigationPush>(&message)) {
          std::fprintf(stderr,
                       "js.core.navigation_push request=%s uri=%s source=%s\n",
                       navigation->requestId.c_str(), navigation->uri.c_str(),
                       navigation->sourceSurfaceId.c_str());
          const auto requestId = qc::RequestId::parse(navigation->requestId);
          const auto source = qc::SurfaceId::parse(navigation->sourceSurfaceId);
          if (!requestId || !source || surfaceController_ == nullptr) {
            return reject("invalid navigation push", navigation->sourceSurfaceId,
                          navigation->requestId);
          }
          const auto accepted = surfaceController_->enqueue(qs::SurfaceRequest(
              qs::NavigationPushRequest{requestId.value(), source.value(),
                                        navigation->uri}));
          if (!accepted) {
            return reject("Core rejected navigation push",
                          navigation->sourceSurfaceId, navigation->requestId);
          }
          {
            std::lock_guard lock(navigationMutex_);
            navigationSources_[navigation->requestId] = navigation->sourceSurfaceId;
          }
          navigationPushes_.fetch_add(1, std::memory_order_relaxed);
        }
        return ja::EnqueueResult::accepted();
      }
      const auto& instantiate = std::get<ja::InstantiateTemplate>(message);
      std::fprintf(stderr,
                   "js.core.instantiate request=%s surface=%s template=%s bindings=%zu handlers=%zu\n",
                   instantiate.requestId.c_str(), instantiate.surfaceId.c_str(),
                   instantiate.templateId.c_str(), instantiate.initialBindings.size(),
                   instantiate.initialHandlers.size());
      const auto parsedSurface = qc::SurfaceId::parse(instantiate.surfaceId);
      const auto pageOwner = qc::ComponentInstanceId::parse(instantiate.ownerInstanceId);
      qp::PageIrHandle page;
      {
        std::lock_guard lock(pagesMutex_);
        const auto found = pages_.find(instantiate.surfaceId);
        if (found != pages_.end()) page = found->second;
      }
      if (!parsedSurface || !pageOwner || !page) {
        return reject("invalid initial template identity", instantiate.surfaceId,
                      instantiate.requestId);
      }
      std::map<std::uint64_t, qc::runtime_tree::BindingValue> bindings;
      for (const auto& [id, value] : instantiate.initialBindings) {
        bindings.emplace(id, std::visit(
            [](const auto& item) -> qc::runtime_tree::BindingValue { return item; },
            value));
      }
      std::vector<qc::runtime_tree::HandlerRegistration> handlers;
      for (const auto& binding : instantiate.initialHandlers) {
        const auto owner = qc::ComponentInstanceId::parse(binding.ownerInstanceId);
        const auto templateId = qc::TemplateHandlerId::from(binding.templateHandlerId);
        const auto handlerId = qc::HandlerId::parse(binding.handlerId);
        if (!owner || !templateId || !handlerId) {
          return reject("invalid initial handler identity", instantiate.surfaceId,
                        instantiate.requestId);
        }
        handlers.push_back({owner.value(), templateId.value(), handlerId.value()});
      }
      if (coordinator_ == nullptr) {
        return reject("Core render pipeline is unavailable", instantiate.surfaceId,
                      instantiate.requestId);
      }
      const auto sourceId = qc::RequestId::parse(instantiate.requestId);
      if (!sourceId) {
        return reject("invalid initial render request identity",
                      instantiate.surfaceId, instantiate.requestId);
      }
      const auto submitted = coordinator_->submit(qr::InitialRenderIntent{
          parsedSurface.value(), sourceId.value(), pageOwner.value(), page,
          std::move(bindings), {kRuntimeViewportWidth, kRuntimeViewportHeight},
          std::move(handlers)});
      if (!submitted) {
        return reject("Core rejected initial render", instantiate.surfaceId,
                      instantiate.requestId);
      }
      submitted_ = true;
      return ja::EnqueueResult::accepted();
    } catch (...) {
      return reject("out of memory while submitting initial render", "", "");
    }
  }

  [[nodiscard]] bool submitted() const noexcept { return submitted_; }
  [[nodiscard]] std::size_t navigationPushes() const noexcept {
    return navigationPushes_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::optional<std::string> takeNavigationSource(
      std::string_view requestId) noexcept {
    std::lock_guard lock(navigationMutex_);
    const auto found = navigationSources_.find(std::string(requestId));
    if (found == navigationSources_.end()) return std::nullopt;
    auto source = std::move(found->second);
    navigationSources_.erase(found);
    return source;
  }

 private:
  static ja::EnqueueResult reject(std::string message, std::string surfaceId,
                                  std::string requestId) noexcept {
    return ja::EnqueueResult::rejected({ja::AbiErrorCode::InvalidArgument,
                                        std::move(message), false,
                                        std::move(surfaceId),
                                        std::move(requestId), {}, {}});
  }

  static ja::EnqueueResult rejectTransaction(std::string message,
                                             std::string surfaceId,
                                             std::string transactionId) noexcept {
    return ja::EnqueueResult::rejected({ja::AbiErrorCode::InvalidArgument,
                                        std::move(message), false,
                                        std::move(surfaceId), std::nullopt,
                                        std::move(transactionId), std::nullopt});
  }

  qr::MountCoordinator* coordinator_{nullptr};
  std::mutex pagesMutex_;
  std::map<std::string, qp::PageIrHandle, std::less<>> pages_;
  qs::SurfaceController* surfaceController_{nullptr};
  std::mutex navigationMutex_;
  std::map<std::string, std::string, std::less<>> navigationSources_;
  ja::RuntimeAbiService* runtimeAbi_{nullptr};
  bool submitted_{false};
  std::atomic<std::size_t> navigationPushes_{0};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (kInteractiveSimulator && argc > 1 &&
        std::string_view(argv[1]) == "--interactive") {
      throw std::runtime_error(
          "quickapp_lvgl_simulator is already interactive; pass an optional RPK path");
    }
    const bool binding001 = argc > 1 && std::string_view(argv[1]) == "--binding-001";
    const bool s4Back = argc > 1 && std::string_view(argv[1]) == "--s4-back";
    const char* artifact = binding001
                               ? "../quickapp-toolkit/evidence/tk-s08-binding001.rpk"
                               : (s4Back ? "../quickapp-toolkit/evidence/tk-s07-case001.rpk"
                                         : (argc > 1 ? argv[1] : "../quickapp-toolkit/evidence/tk-s07-case001.rpk"));
    qc::AppRuntimeFactory factory;
    auto identity = std::move(factory.create()).value();
    auto source = std::make_shared<Source>(readFile(artifact));
    qp::RuntimeComposition composition{"quickapp-kit-runtime-v1", "quickapp-kit-js-engine-v1", {"View", "Text", "Button"}, {"system.prompt", "system.router", "system.fetch", "system.device", "system.shortcut"}};
    auto loader = std::move(qp::PackageLoader::create(source, identity.request_ids(), std::move(composition))).value();
    std::shared_ptr<const qp::VerifiedPackage> package;
    std::string failure;
    if (!loader->open([&](auto result) { if (result) package = std::move(result).value(); else failure = result.error().message; })) throw std::runtime_error("RPK open enqueue failed");
    if (!package || !failure.empty()) throw std::runtime_error("RPK open failed: " + failure);
    std::fprintf(stderr, "phase=rpk_opened\n");
    lv_init();
    lv_display_t* display = lv_sdl_window_create(
        static_cast<std::int32_t>(kRuntimeViewportWidth),
        static_cast<std::int32_t>(kRuntimeViewportHeight));
    if (!display) throw std::runtime_error("SDL display creation failed");
    if (kInteractiveSimulator) {
      // Keep logical layout at 720x1280 while fitting the desktop window at 360x640.
      lv_sdl_window_set_size(display, 360, 640);
      lv_sdl_window_set_zoom(display, 0.5F);
    }
    lv_display_set_default(display);
    lv_indev_t* mouse = lv_sdl_mouse_create();
    if (!mouse) throw std::runtime_error("SDL mouse creation failed");
    lv_indev_set_display(mouse, display);
    std::array<qlf::OwnerTask, 128> taskStorage{};
    qlf::OwnerTaskQueue tasks(taskStorage.data(), taskStorage.size(), 128, nullptr);
    if (!tasks.bindOwner(kOwner).ok()) throw std::runtime_error("owner bind failed");
    std::fprintf(stderr, "phase=display_ready\n");
    qls::LvglPageRootBackend roots(lv_screen_active());
    SurfaceContent content;
    MountResults mountResults;
    auto bridge = std::make_unique<qli::CoreMountBridge>(kOwner, mountResults,
                                                         nullptr, false);
    auto* bridgeRaw = bridge.get();
    SurfaceResults surfaceResults(*bridgeRaw);
    qls::SurfaceHostAdapter surfaces(tasks, kOwner, roots, content, surfaceResults, qls::simulatorSurfaceHostLimits());
    qlm::LvglMountBackend nativeRoots(roots);
    qlm::MountHost mounts(tasks, kOwner, surfaces, nativeRoots, *bridgeRaw,
                          qlm::simulatorMountHostLimits());
    content.bind(mounts);
    qc::RuntimeCounters counters;
    FontResults fontResults;
    qm::FontSnapshotPublisher publisher(fontResults);
    if (!publisher.initialize(kOwner, qm::FontMetricsSnapshot::makeV1(1)).ok()) throw std::runtime_error("font initialization failed");
    qm::FontMeasureAdapter platformMeasure(publisher, qm::simulatorMeasureLimits());
    auto measure = std::make_unique<CoreMeasure>(platformMeasure);
    auto initialResults = std::make_unique<ControllerInitialResults>();
    auto* initialResultsRaw = initialResults.get();
    auto renderResults = std::make_unique<RenderResults>();
    auto* renderResultsRaw = renderResults.get();
    JsCoreIngress coreIngress;
    qc::event::EventRouter eventRouter(coreIngress);
    auto coordinatorResult = qr::MountCoordinator::create(
      {&identity.request_ids(), &counters, std::move(measure), std::move(bridge),
         std::move(initialResults), nullptr, nullptr, &eventRouter,
         std::move(renderResults)});
    if (!coordinatorResult) throw std::runtime_error("MountCoordinator create failed");
    auto coordinator = std::move(coordinatorResult).value();
    auto* coordinatorRaw = coordinator.get();
    coreIngress.bindCoordinator(*coordinatorRaw);
    mountResults.bind(*coordinator);
    std::fprintf(stderr, "phase=coordinator_ready\n");

    auto provider = std::make_unique<qj::QuickJsEngineProvider>();
    const auto descriptor = provider->describe();
    Clock clock;
    TraceSink traceSink;
    auto registration = qj::TraceSinkRegistration::admit(
        traceSink, {.nonblocking = true, .noReentry = true});
    if (!registration.ok()) throw std::runtime_error("trace registration failed");
    qj::JsEngineConfig engineConfig;
    engineConfig.expectedEngine = descriptor;
    engineConfig.limits.maxPendingTasks = 32;
    qj::JsEngineService engine(binding001 ? "app:binding001" : "app:case001", std::move(provider), engineConfig,
                               clock, std::move(registration).value(),
                               {false, binding001 ? "binding001-lvgl" : "case001-lvgl", "steady", 0});
    std::promise<qj::ServiceResult> started;
    if (!engine.start([&](qj::ServiceResult result) {
          started.set_value(std::move(result));
        }) || !started.get_future().get().ok()) {
      throw std::runtime_error("QuickJS start failed");
    }

    qj::JsEngineService* enginePtr = &engine;

    ModuleCompletion completion;
    JsRequestIds jsRequestIds;
    auto* facades = new qj::framework::StaticFacadeCatalog();
    qj::module::ModuleLoader* modules = nullptr;
    std::shared_ptr<qj::abi::RuntimeAbiService> runtimeAbi;
    qj::event::HandlerRegistry* handlerRegistry = nullptr;
    qj::page::PageHostControlInstaller* pageControls = nullptr;
    qj::binding::AlphaInitialBindingStage* bindingStage = nullptr;
    qj::render::AlphaInitialTransactionBuilder* transactionBuilder = nullptr;
    qj::alpha::AlphaPageInitializationStage* pageStage = nullptr;
    qj::vm::VmLifecycleService* vm = nullptr;
    qs::SurfaceController* controller = nullptr;
    std::uint64_t moduleRequestSequence = 0;
    std::uint64_t pageInitSequence = 0;

    CoreSurfaceResultIngress surfaceIngress;
    AppState appState;
    ControllerStatus statusSink;
    ControllerLifecycleResults lifecycleSink;
    PlatformBackIngress platformBack;
    std::atomic<bool> closeCompleted{false};
    std::optional<qc::RuntimeError> closeError;
    std::atomic<std::uint64_t> jsThreadHash{0};
    std::atomic<std::uint64_t> ownerThreadHash{0};

    auto pageLifecycle = std::make_unique<ControllerPageLifecycle>(
        [&](qs::PageCommand&& command) -> qc::EnqueueResult {
          if (enginePtr == nullptr || controller == nullptr) {
            return qc::EnqueueResult::failure(qc::RuntimeError::simple(
                qc::RuntimeErrorCode::kPlatformRejected,
                "JS page lifecycle is unavailable"));
          }
          return enginePtr->post(
              [&, command = std::move(command)](qj::JsEnginePort&,
                                                const qj::JsContextRef&) mutable {
                auto complete = [&](const qs::PageCommand& value,
                                    bool ok,
                                    std::optional<qc::RuntimeError> error = std::nullopt) {
                  const auto* start = std::get_if<qs::PageStartCommand>(&value);
                  const auto* hook = std::get_if<qs::PageHookCommand>(&value);
                  static_cast<void>(controller->enqueue(qs::PageLifecycleResult{
                      start ? start->request_id : hook->request_id,
                      start ? qs::PageCommandKind::kStart : qs::PageCommandKind::kHook,
                      start ? start->surface_id : hook->surface_id,
                      hook ? std::optional<qs::PageHook>(hook->hook) : std::nullopt,
                      ok, std::move(error)}));
                };
                try {
                  if (auto* start = std::get_if<qs::PageStartCommand>(&command)) {
                    if (modules == nullptr || vm == nullptr || runtimeAbi == nullptr ||
                        !runtimeAbi->openSurfaceOnExecutor(
                            start->surface_id.wire()).ok() ||
                        !modules->openSurfaceOnExecutor(start->surface_id.wire())) {
                      complete(command, false, qc::RuntimeError::simple(
                          qc::RuntimeErrorCode::kSurfaceNotFound,
                          "page JS surface could not open"));
                      return;
                    }
                    ja::LoadVerifiedModule message;
                    message.requestId = "req:j-" +
                                        std::to_string(++moduleRequestSequence);
                    const auto& module = start->page.module;
                    message.packageId = module.package_id();
                    message.moduleKind = "page";
                    message.moduleId = module.module_id();
                    message.cacheScope = "surface";
                    message.surfaceId = start->surface_id.wire();
                    message.dependencies = module.dependencies();
                    message.bundle = {module.descriptor().path,
                                      module.descriptor().byte_length,
                                      module.descriptor().sha256,
                                      std::make_shared<const std::vector<std::uint8_t>>(
                                          *module.bytes())};
                    message.expectedBootstrap = ja::BootstrapExpectation{
                        "page", module.module_id(), module.expected_template_id()};
                    message.expectedBindingIds = module.expected_binding_ids();
                    message.expectedHandlerIds = module.expected_handler_ids();
                    modules->onLoadVerifiedModule(message);
                    coreIngress.bindPage(start->surface_id, start->page.page_ir);
                    vm->onSurfaceContext({start->surface_id.wire(), package->package_id(),
                                          start->page.route,
                                          module.expected_template_id().value_or(""),
                                          {}, {"setTitleBar", "setMeta"},
                                          {kRuntimeViewportWidth,
                                           kRuntimeViewportHeight,
                                           "logical-px"}});
                    complete(command, completion.status == "loaded",
                             completion.status == "loaded"
                                 ? std::nullopt
                                 : std::optional<qc::RuntimeError>(qc::RuntimeError::simple(
                                       qc::RuntimeErrorCode::kModuleAbiUnsupported,
                                       completion.error)));
                    return;
                  }
                  auto* hook = std::get_if<qs::PageHookCommand>(&command);
                  if (hook == nullptr) return;
                  if (hook->hook == qs::PageHook::kOnDestroy) {
                    if (handlerRegistry != nullptr)
                      handlerRegistry->closeSurface(hook->surface_id.wire());
                    eventRouter.closeSurface(hook->surface_id);
                    if (vm != nullptr)
                      vm->closeSurfaceOnExecutor(hook->surface_id.wire());
                  }
                  complete(command, true);
                } catch (...) {
                  complete(command, false, qc::RuntimeError::simple(
                      qc::RuntimeErrorCode::kJsException,
                      "page lifecycle execution failed"));
                }
              }).status == qj::PostStatus::Accepted
                     ? qc::EnqueueResult::success(qc::Accepted{})
                     : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                           qc::RuntimeErrorCode::kQueueOverflow,
                           "JS page lifecycle queue rejected"));
        });

    auto initialPipeline = std::make_unique<ControllerInitialPipeline>(
        [&](qs::InitialContentCommand&& command) -> qc::EnqueueResult {
          if (coordinatorRaw == nullptr || enginePtr == nullptr || vm == nullptr)
            return qc::EnqueueResult::failure(qc::RuntimeError::simple(
                qc::RuntimeErrorCode::kPlatformRejected,
                "initial render services are unavailable"));
          const auto surfaceId = command.surface_id;
          const auto pageIr = command.page_ir;
          auto posted = coordinatorRaw->post(std::move(command));
          if (!posted) return posted;
          const auto task = enginePtr->post(
              [&, surfaceId, pageIr](qj::JsEnginePort&, const qj::JsContextRef&) {
                vm->onVmInitialization({
                    "req:" + std::to_string(1000 + ++pageInitSequence),
                    "page", surfaceId.wire()});
                if (modules == nullptr || handlerRegistry == nullptr) return;
                const auto definition = modules->pageDefinitionForSurfaceOnExecutor(
                    surfaceId.wire(), pageIr->template_id());
                if (!definition) return;
                const auto handlers = modules->handlerBindingsOnExecutor(
                    *definition, "cmp:" + surfaceId.wire());
                if (!handlers.ok()) return;
                for (const auto& binding : handlers.value()) {
                  const auto method = modules->handlerMethodNameOnExecutor(
                      *definition, binding.templateHandlerId);
                  auto retained = vm->pageVmOnExecutor(surfaceId.wire());
                  if (method && retained.ok())
                    static_cast<void>(handlerRegistry->bind(
                        surfaceId.wire(), binding.handlerId, *method,
                        std::move(retained).value()));
                }
              });
          return task.status == qj::PostStatus::Accepted
                     ? qc::EnqueueResult::success(qc::Accepted{})
                     : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                           qc::RuntimeErrorCode::kQueueOverflow,
                           "JS initial render queue rejected"));
        });
    initialPipeline->onRelease([&](const qc::SurfaceId& surfaceId) {
      if (coordinatorRaw != nullptr) coordinatorRaw->release_surface(surfaceId);
    });

    auto operationResults = std::make_unique<ControllerOperationResults>(
        [&](qs::SurfaceOperationKind kind, qc::RequestId requestId,
            std::optional<qc::SurfaceId> target, bool completed,
            std::optional<qc::RuntimeError> error) {
          if (kind == qs::SurfaceOperationKind::kClose) {
            closeCompleted.store(completed, std::memory_order_release);
            closeError = std::move(error);
            std::fprintf(stderr,
                         "core.navigation.close request=%s source=%s completed=%d\n",
                         requestId.wire().c_str(),
                         target ? target->wire().c_str() : "",
                         completed ? 1 : 0);
            return;
          }
          if (kind != qs::SurfaceOperationKind::kPush || runtimeAbi == nullptr) return;
          const auto source = coreIngress.takeNavigationSource(requestId.wire());
          if (!source) return;
          ja::NavigationPushResult result{
              requestId.wire(), *source, completed ? "completed" : "failed",
              target ? std::optional<std::string>(target->wire()) : std::nullopt,
              error ? std::optional<ja::MessageRuntimeError>(ja::MessageRuntimeError{
                         std::string(qc::to_wire(error->code)),
                         std::string(error->message),
                         error->retryable, std::nullopt, requestId.wire(), std::nullopt,
                         std::nullopt})
                    : std::nullopt};
          static_cast<void>(runtimeAbi->postCallback(
              ja::JsInboundMessage(std::move(result))));
        });

    auto pages = std::make_unique<PageResolver>(*loader, package);
    auto platform = std::make_unique<SurfacePlatform>(surfaces);
    [[maybe_unused]] auto* platformRaw = platform.get();
    qs::SurfaceLimits surfaceLimits;
    if (s4Back) surfaceLimits.ingress_capacity = 1;
    auto controllerResult = qs::SurfaceController::create(
        {&appState, &identity.request_ids(), std::move(pages), std::move(platform),
         std::move(pageLifecycle), std::move(initialPipeline),
         std::move(operationResults), std::make_unique<ControllerStatus>(),
         std::make_unique<ControllerLifecycleResults>(), &counters},
        surfaceLimits);
    if (!controllerResult) throw std::runtime_error("SurfaceController create failed");
    auto surfaceController = std::move(controllerResult).value();
    controller = surfaceController.get();
    coreIngress.bindSurfaceController(*controller);
    platformBack.bind(*controller);
    surfaceIngress.bind(*controller);
    initialResultsRaw->bind(*controller);
    bridgeRaw->bind(mounts, surfaces, &surfaceIngress);
    std::promise<bool> baseReady;
    auto baseReadyFuture = baseReady.get_future();
    std::atomic<bool> baseReadyOnce{false};
    const auto finishBase = [&](bool value) {
      if (!baseReadyOnce.exchange(true)) baseReady.set_value(value);
    };
    if (engine.post([&](qj::JsEnginePort& js, const qj::JsContextRef& context) {
          jsThreadHash.store(static_cast<std::uint64_t>(
              std::hash<std::thread::id>{}(std::this_thread::get_id())),
              std::memory_order_relaxed);
          try {
            if (!facades->startOnExecutor(js, context)) { finishBase(false); return; }
            modules = new qj::module::ModuleLoader(
                engine, completion, binding001 ? "app:binding001" : "app:case001", package->package_id(),
                qj::module::ModuleLoaderLimits{}, facades);
            if (!modules->startOnExecutor(js, context)) { finishBase(false); return; }
            runtimeAbi = std::make_shared<qj::abi::RuntimeAbiService>(
                engine, coreIngress, qj::abi::RuntimeAbiLimits{},
                qj::abi::CapabilitySupportSnapshot{});
            if (!runtimeAbi->startOnExecutor(js, context,
                                             qj::abi::kRuntimeAbiIdentity).ok()) {
              finishBase(false); return;
            }
            renderResultsRaw->bind(*runtimeAbi);
            coreIngress.bindAbi(*runtimeAbi);
            handlerRegistry = new qj::event::HandlerRegistry(engine);
            pageControls = new qj::page::PageHostControlInstaller(
                engine, *runtimeAbi, jsRequestIds);
            bindingStage = new qj::binding::AlphaInitialBindingStage(engine, *modules);
            transactionBuilder = new qj::render::AlphaInitialTransactionBuilder(
                engine, jsRequestIds);
            if (!handlerRegistry->startOnExecutor(js, context) ||
                !pageControls->startOnExecutor(js, context) ||
                !bindingStage->startOnExecutor(js, context) ||
                !transactionBuilder->startOnExecutor(js, context)) {
              finishBase(false); return;
            }
            pageStage = new qj::alpha::AlphaPageInitializationStage(
                *bindingStage, *transactionBuilder);
            vm = new qj::vm::VmLifecycleService(engine, *modules, *pageControls,
                                                 *pageStage, package->package_id());
            auto slots = modules->callbackSlots();
            auto vmSlots = vm->callbackSlots();
            slots.appContext = std::move(vmSlots.appContext);
            slots.surfaceContext = std::move(vmSlots.surfaceContext);
            slots.vmInitializationDispatch = std::move(vmSlots.vmInitializationDispatch);
            slots.jsEventDispatch = [handlerRegistry](const ja::JsEventDispatch& value) {
              static_cast<void>(handlerRegistry->dispatchOnExecutor(value));
            };
            slots.renderTransactionResult = [](const ja::RenderTransactionResult& value) {
              std::fprintf(stderr,
                           "js.render.complete surface=%s transaction=%s status=%s revision=%llu\n",
                           value.surfaceId.c_str(), value.transactionId.c_str(),
                           value.status.c_str(),
                           static_cast<unsigned long long>(value.committedRevision));
            };
            if (!runtimeAbi->registerConsumersOnExecutor(std::move(slots)).ok() ||
                !vm->startOnExecutor(js, context)) { finishBase(false); return; }

            const auto send = [&](const qp::VerifiedModule& module,
                                  std::string kind,
                                  std::optional<ja::BootstrapExpectation> bootstrap) {
              ja::LoadVerifiedModule message;
              message.requestId = "req:j-" +
                                  std::to_string(++moduleRequestSequence);
              message.packageId = module.package_id();
              message.moduleKind = std::move(kind);
              message.moduleId = module.module_id();
              message.cacheScope = "appRuntime";
              message.dependencies = module.dependencies();
              message.bundle = {module.descriptor().path,
                                module.descriptor().byte_length,
                                module.descriptor().sha256,
                                std::make_shared<const std::vector<std::uint8_t>>(
                                    *module.bytes())};
              message.expectedBootstrap = std::move(bootstrap);
              modules->onLoadVerifiedModule(message);
            };
            for (const auto moduleId : binding001
                                          ? std::initializer_list<std::string_view>{}
                                          : std::initializer_list<std::string_view>{
                                                "@quickapp-kit/shared/helper/utils",
                                                "@quickapp-kit/shared/helper/ajax",
                                                "@quickapp-kit/shared/helper/apis/example",
                                                "@quickapp-kit/shared/helper/apis/index"}) {
              const auto descriptorIt = package->modules().find(std::string(moduleId));
              if (descriptorIt == package->modules().end()) { finishBase(false); return; }
              std::optional<qp::VerifiedModule> shared;
              if (!loader->load_module({descriptorIt->first, std::nullopt},
                                       [&](auto result) {
                    if (result) shared = std::move(result).value();
                  }) || !shared) { finishBase(false); return; }
              send(*shared, "shared", std::nullopt);
            }
            std::optional<qp::VerifiedModule> app;
            if (!loader->load_module({"@quickapp-kit/app", std::nullopt},
                                     [&](auto result) {
                  if (result) app = std::move(result).value();
                }) || !app) { finishBase(false); return; }
            send(*app, "app",
                 ja::BootstrapExpectation{"app", app->module_id(), std::nullopt});
            vm->onAppContext({package->package_id(), "1.0.0", "1", 1,
                              {"system.router", "system.prompt", "system.fetch"}});
            vm->onVmInitialization({"req:2", "app", std::nullopt});
            finishBase(vm->resources().appVms == 1);
          } catch (...) {
            finishBase(false);
          }
        }).status != qj::PostStatus::Accepted || !baseReadyFuture.get()) {
      throw std::runtime_error("QuickJS base composition failed");
    }

    const auto service = [&]() {
      const auto ownerHash = static_cast<std::uint64_t>(
          std::hash<std::thread::id>{}(std::this_thread::get_id()));
      ownerThreadHash.store(ownerHash, std::memory_order_relaxed);
      if (!controller->drain()) throw std::runtime_error("Core surface drain failed");
      if (!tasks.pump(kOwner, 128).ok())
        throw std::runtime_error("LVGL owner task pump failed");
      if (!mounts.service(kOwner, 128).ok())
        throw std::runtime_error("LVGL mount service failed");
      if (surfaces.service(kOwner, 128).error != qlf::LocalError::kNone)
        throw std::runtime_error("LVGL surface service failed");
      if (!bridgeRaw->service(kOwner, 128).ok())
        throw std::runtime_error("Core mount bridge service failed");
      if (!controller->drain()) throw std::runtime_error("Core result drain failed");
      lv_timer_handler();
    };
    const auto waitFor = [&](auto predicate, std::string_view failureMessage) {
      for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        service();
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      throw std::runtime_error(std::string(failureMessage));
    };

    if (!controller->enqueue(qs::SurfaceRequest(qs::RootSurfaceRequest{
            request("req:j-root"), binding001 ? "/pages/Binding" : "/pages/Demo"}))) {
      throw std::runtime_error("root navigation enqueue failed");
    }
    waitFor([&] {
      const auto snapshot = controller->snapshot();
      return snapshot.navigation_stack.size() == 1 && !snapshot.navigation_active &&
             snapshot.records.size() == 1 &&
             snapshot.records.front().lifecycle == qs::SurfaceLifecycle::kVisible;
    }, "root surface did not become visible");
    const auto rootSnapshot = controller->snapshot();
    const auto surfaceId = rootSnapshot.navigation_stack.front();
    std::fprintf(stderr, "surface.root.visible=%s\n", surfaceId.wire().c_str());

    const auto handlerId = qc::HandlerId::parse("hdl:1");
    if (!handlerId) throw std::runtime_error("Case 001 handler id invalid");
    const auto buttonNode = eventRouter.nodeForHandler(surfaceId, handlerId.value());
    if (!buttonNode) throw std::runtime_error("Case 001 button node not registered");
    const auto missingNode = qc::NodeId::parse("node:999");
    if (!missingNode) throw std::runtime_error("negative NodeId is invalid");
    if (!kInteractiveSimulator) {
      const auto invalidHandler = eventRouter.dispatch(qc::event::PlatformInputMessage{
          request("req:p-900"), surfaceId, missingNode.value(),
          qp::EventType::kClick, 900, {}});
      if (invalidHandler ||
          invalidHandler.error().code != qc::RuntimeErrorCode::kHandlerNotFound) {
        throw std::runtime_error("invalid HandlerId input was not rejected");
      }
      std::fprintf(stderr,
                   "negative.invalid_handler rejected=1 error=HANDLER_NOT_FOUND\n");
    }
    LvglClickToCore clickSink(eventRouter);
    if (!mounts.installClickHandler(surfaceId, buttonNode.value(),
                                    &LvglClickToCore::callback, &clickSink)) {
      throw std::runtime_error("LVGL click handler install failed");
    }
    std::fprintf(stderr, "lvgl.button.node=%s\n", buttonNode->wire().c_str());
    lv_timer_handler();
    auto* pageRoot = static_cast<lv_obj_t*>(roots.nativeObject(qls::PageRootHandle{1}));
    auto* hostRoot = pageRoot ? lv_obj_get_child(pageRoot, 0) : nullptr;
    auto* title = hostRoot ? lv_obj_get_child(hostRoot, 0) : nullptr;
    const bool rootVisible = title &&
        std::string_view(lv_label_get_text(title)) ==
            (binding001 ? std::string_view{"0"} : std::string_view{"欢迎体验快应用开发"});
    if (!rootVisible) throw std::runtime_error("Case 001 root text is not visible");

    auto* buttonObject = static_cast<lv_obj_t*>(hostRoot ? lv_obj_get_child(hostRoot, 1) : nullptr);
    if (buttonObject == nullptr)
      throw std::runtime_error("Case 001 real LVGL button is absent");

    std::optional<qc::SurfaceId> s4DetailSurface;
#if defined(QUICKAPP_EXAMPLES_INTERACTIVE_SIMULATOR)
    SimulatorExitState simulatorExit;
    SimulatorEventFilterScope simulatorEventFilterScope(simulatorExit);
    SimulatorSignalScope simulatorSignalScope;
    std::fprintf(stderr,
                 "simulator.ready rpk=%s surface=%s input=lvgl_sdl\n",
                 artifact, surfaceId.wire().c_str());
    while (!simulatorExit.requested.load(std::memory_order_acquire) &&
           gSimulatorSignalExit == 0) {
      service();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "simulator.close requested=1\n");
#else
    if (binding001) {
      lv_obj_send_event(buttonObject, LV_EVENT_CLICKED, nullptr);
      waitFor([&] {
        return renderResultsRaw->completed && renderResultsRaw->last.has_value() &&
               renderResultsRaw->last->presented &&
               renderResultsRaw->last->submitted_revision == 1 &&
               renderResultsRaw->last->committed_revision == 1 && title != nullptr &&
               std::string_view(lv_label_get_text(title)) == "1";
      }, "BINDING-001 incremental render did not settle");
      std::fprintf(stderr,
                   "binding001.chain state_write=1 dirty_binding=1 microtask_flush=1 render_transaction=1 revision=0->1 mount_transaction=1 lvgl.text=1\n");
    } else if (s4Back) {
      lv_obj_send_event(buttonObject, LV_EVENT_CLICKED, nullptr);
      waitFor([&] {
        const auto snapshot = controller->snapshot();
        return snapshot.navigation_stack.size() == 2 &&
               snapshot.records.size() == 2 && !snapshot.navigation_active &&
               snapshot.records.back().lifecycle == qs::SurfaceLifecycle::kVisible;
      }, "S4 detail surface did not become visible");
      const auto detailSnapshot = controller->snapshot();
      const auto detailSurface = detailSnapshot.navigation_stack.back();
      s4DetailSurface = detailSurface;
      const auto detailHandler = handlerId;
      if (!detailHandler ||
          !eventRouter.nodeForHandler(detailSurface, detailHandler.value())) {
        throw std::runtime_error("S4 detail Handler was not registered");
      }
      auto* detailPage = static_cast<lv_obj_t*>(
          roots.nativeObject(qls::PageRootHandle{2}));
      auto* detailRoot = detailPage ? lv_obj_get_child(detailPage, 0) : nullptr;
      auto* detailTitle = detailRoot ? lv_obj_get_child(detailRoot, 0) : nullptr;
      if (detailTitle == nullptr ||
          std::string_view(lv_label_get_text(detailTitle)) != "快应用是什么？") {
        throw std::runtime_error("S4 detail content is not visible");
      }
      const auto detailObjectsBefore = mounts.liveObjectCount();
      const auto detailRootsBefore = surfaces.liveRootCount();
      const auto detailHandlersBefore = eventRouter.handlerCount();
      const auto detailNodesBefore = coordinatorRaw->snapshot().committed_nodes;
      closeCompleted.store(false, std::memory_order_release);
      closeError.reset();
      const auto firstBack = platformBack.post(qs::NavigationCloseRequest{
          request("req:p-back"), detailSurface});
      if (!firstBack) throw std::runtime_error("Platform Back was rejected");
      const auto overflowBack = platformBack.post(qs::NavigationCloseRequest{
          request("req:p-back-overflow"), detailSurface});
      if (overflowBack ||
          overflowBack.error().code != qc::RuntimeErrorCode::kQueueOverflow) {
        throw std::runtime_error("bounded Platform/Core queue did not reject overflow");
      }
      waitFor([&] {
        const auto snapshot = controller->snapshot();
        return closeCompleted.load(std::memory_order_acquire) &&
               !closeError.has_value() && snapshot.navigation_stack.size() == 1 &&
               snapshot.records.size() == 1 && !snapshot.navigation_active &&
               snapshot.records.front().surface_id == surfaceId &&
               snapshot.records.front().lifecycle == qs::SurfaceLifecycle::kVisible &&
               snapshot.records.front().host_state == qs::SurfaceHostState::kVisible;
      }, "S4 Platform Back did not close Detail");
      const auto afterClose = controller->snapshot();
      const auto rootPage = static_cast<lv_obj_t*>(
          roots.nativeObject(qls::PageRootHandle{1}));
      if (rootPage == nullptr || lv_obj_is_hidden(rootPage) ||
          roots.nativeObject(qls::PageRootHandle{2}) != nullptr ||
          eventRouter.nodeForHandler(detailSurface, detailHandler.value()) ||
          eventRouter.handlerCount() != detailHandlersBefore - 1 ||
          surfaces.liveRootCount() != detailRootsBefore - 1 ||
          mounts.liveObjectCount() >= detailObjectsBefore ||
          coordinatorRaw->snapshot().committed_nodes >= detailNodesBefore ||
          afterClose.navigation_stack.back() != surfaceId) {
        throw std::runtime_error("S4 Detail resources or Demo reveal are inconsistent");
      }
      std::fprintf(stderr,
                   "s4.chain platform_back=1 navigation_close=1 close_surface=1 detail_onDestroy=1 demo_onShow=1 stack=2->1 queue_overflow=1 detail_resources_released=1 demo_visible=1\n");
      std::fprintf(stderr,
                   "s5.queue typed=NavigationCloseRequest capacity=1 rejected=1 error=QUEUE_OVERFLOW\n");

      const auto stableAfterClose = controller->snapshot();
      const auto resourcesAfterClose = coordinatorRaw->snapshot();
      platformRaw->failNextCreate();
      if (!controller->enqueue(qs::SurfaceRequest(qs::NavigationPushRequest{
              request("req:s5-create-failure"), surfaceId, "/pages/DemoDetail"}))) {
        throw std::runtime_error("S5 failure request enqueue failed");
      }
      waitFor([&] { return !controller->snapshot().navigation_active; },
              "S5 injected create failure did not settle");
      const auto afterFailure = controller->snapshot();
      if (afterFailure.navigation_stack != stableAfterClose.navigation_stack ||
          afterFailure.records.size() != stableAfterClose.records.size() ||
          coordinatorRaw->snapshot().surfaces != resourcesAfterClose.surfaces ||
          coordinatorRaw->snapshot().committed_nodes != resourcesAfterClose.committed_nodes) {
        throw std::runtime_error("S5 failed navigation polluted Core authority");
      }
      std::fprintf(stderr,
                   "s5.failure mid_create=1 core_stack_unchanged=1 runtime_tree_unchanged=1\n");
      if (jsThreadHash.load() == 0 || ownerThreadHash.load() == 0 ||
          jsThreadHash.load() == ownerThreadHash.load()) {
        throw std::runtime_error("S5 JS/Core/LVGL thread ownership is not distinct");
      }
      std::fprintf(stderr,
                   "s5.threads js_executor=%llu core_runtime=%llu lvgl_owner=%llu core_lvgl_same=%d\n",
                   static_cast<unsigned long long>(jsThreadHash.load()),
                   static_cast<unsigned long long>(ownerThreadHash.load()),
                   static_cast<unsigned long long>(ownerThreadHash.load()),
                   1);
    } else {
    platformRaw->holdNextCreate();
    lv_obj_send_event(buttonObject, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(buttonObject, LV_EVENT_CLICKED, nullptr);
    for (std::size_t attempt = 0;
         attempt < 2000 && coreIngress.navigationPushes() != 2; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (coreIngress.navigationPushes() != 2 || !controller->drain())
      throw std::runtime_error("duplicate clicks did not reach Core together");
    auto heldCreate = platformRaw->takeHeldCreate();
    if (!heldCreate || !controller->enqueue(qs::SurfaceCommandResult{
                           heldCreate->request_id,
                           qs::SurfaceCommandKind::kCreate,
                           heldCreate->surface_id,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           false,
                           qc::RuntimeError::simple(
                               qc::RuntimeErrorCode::kPlatformRejected,
                               "injected delayed Surface creation failure")})) {
      throw std::runtime_error("held Surface failure injection failed");
    }
    waitFor([&] { return !controller->snapshot().navigation_active; },
            "duplicate click navigation did not settle");
    if (controller->snapshot().navigation_stack.size() != 1 ||
        controller->snapshot().records.size() != 1) {
      throw std::runtime_error("duplicate clicks committed concurrent Pushes");
    }
    std::fprintf(stderr,
                 "negative.duplicate_click concurrent_pushes=0 stack_unchanged=1\n");

    const auto pushesBeforeHappyClick = coreIngress.navigationPushes();
    lv_obj_send_event(buttonObject, LV_EVENT_CLICKED, nullptr);

    waitFor([&] {
      const auto snapshot = controller->snapshot();
      return snapshot.navigation_stack.size() == 2 && !snapshot.navigation_active &&
             snapshot.records.size() == 2 &&
             snapshot.records.back().lifecycle == qs::SurfaceLifecycle::kVisible;
    }, "detail surface did not become visible");
    const auto detailSnapshot = controller->snapshot();
    const auto detailSurface = detailSnapshot.navigation_stack.back();
    auto* detailPage = static_cast<lv_obj_t*>(
        roots.nativeObject(qls::PageRootHandle{2}));
    auto* detailRoot = detailPage ? lv_obj_get_child(detailPage, 0) : nullptr;
    auto* detailTitle = detailRoot ? lv_obj_get_child(detailRoot, 0) : nullptr;
    auto* detailBody = detailRoot ? lv_obj_get_child(detailRoot, 1) : nullptr;
    auto* detailButton = detailRoot ? lv_obj_get_child(detailRoot, 3) : nullptr;
    auto* detailButtonLabel = detailButton ? lv_obj_get_child(detailButton, 0) : nullptr;
    const bool detailVisible = detailTitle && detailBody && detailButtonLabel &&
        std::string_view(lv_label_get_text(detailTitle)) == "快应用是什么？" &&
        std::string_view(lv_label_get_text(detailButtonLabel)) == "欢迎使用";
    if (!detailVisible)
      throw std::runtime_error("Case 001 detail content is not visible");
    if (coreIngress.navigationPushes() != pushesBeforeHappyClick + 1)
      throw std::runtime_error("one click did not produce exactly one push");
    std::fprintf(stderr,
                 "navigation.presented request_count=1 source=%s target=%s stack=2\n",
                 surfaceId.wire().c_str(), detailSurface.wire().c_str());

    if (!controller->enqueue(qs::SurfaceRequest(qs::NavigationPushRequest{
            request("req:j-404"), detailSurface, "/pages/Missing"}))) {
      throw std::runtime_error("missing-route request enqueue failed");
    }
    waitFor([&] { return !controller->snapshot().navigation_active; },
            "missing-route request did not settle");
    if (controller->snapshot().navigation_stack.size() != 2 ||
        controller->snapshot().records.size() != 2) {
      throw std::runtime_error("missing route changed the Core page stack");
    }
    std::fprintf(stderr, "negative.missing_route stack_unchanged=1\n");

    platformRaw->failNextCreate();
    if (!controller->enqueue(qs::SurfaceRequest(qs::NavigationPushRequest{
            request("req:j-405"), detailSurface, "/pages/Demo"}))) {
      throw std::runtime_error("create-failure request enqueue failed");
    }
    waitFor([&] { return !controller->snapshot().navigation_active; },
            "create-failure request did not settle");
    if (controller->snapshot().navigation_stack.size() != 2 ||
        controller->snapshot().records.size() != 2) {
      throw std::runtime_error("failed Surface creation left a partial navigation");
    }
    std::fprintf(stderr,
                 "negative.surface_create_failure stack_unchanged=1\n");
    }
#endif

    const auto coordinatorBeforeDestroy = coordinatorRaw->snapshot();
    std::fprintf(stderr,
                 "resources.before_destroy surfaces=%zu nodes=%zu handlers=%zu live_surface=%llu mount_objects=%zu roots=%zu\n",
                 coordinatorBeforeDestroy.surfaces,
                 coordinatorBeforeDestroy.committed_nodes,
                 eventRouter.handlerCount(),
                 static_cast<unsigned long long>(
                     counters.snapshot().surface_live),
                 mounts.liveObjectCount(), surfaces.liveRootCount());

    if (!controller->post(qc::lifecycle::SurfaceLifecycleCommand{
            request("req:j-destroy-all"),
            qc::lifecycle::SurfaceLifecycleCommandKind::kDestroyAll,
            std::nullopt, std::nullopt})) {
      throw std::runtime_error("destroy-all enqueue failed");
    }
    waitFor([&] {
      const auto snapshot = controller->snapshot();
      return snapshot.records.empty() && snapshot.navigation_stack.empty() &&
             snapshot.pending_correlations == 0;
    }, "surface teardown did not complete");
    if (s4Back && s4DetailSurface.has_value()) {
      const auto lateCallback = controller->enqueue(qs::SurfaceCommandResult{
          request("req:p-late-callback"), qs::SurfaceCommandKind::kClose,
          *s4DetailSurface, *s4DetailSurface, surfaceId, std::nullopt, true,
          std::nullopt});
      if (!lateCallback) throw std::runtime_error("late callback enqueue failed");
      if (!controller->drain()) throw std::runtime_error("late callback drain failed");
      const auto afterLateCallback = controller->snapshot();
      if (!afterLateCallback.records.empty() ||
          !afterLateCallback.navigation_stack.empty()) {
        throw std::runtime_error("late callback revived a destroyed Surface");
      }
      std::fprintf(stderr,
                   "s5.late_callback accepted=1 resurrected=0 core_surfaces=0\n");
    }
    if (!kInteractiveSimulator) {
      const auto lateEvent = eventRouter.dispatch(qc::event::PlatformInputMessage{
          request("req:p-901"), surfaceId, buttonNode.value(),
          qp::EventType::kClick, 901, {}});
      if (lateEvent ||
          lateEvent.error().code != qc::RuntimeErrorCode::kHandlerNotFound) {
        throw std::runtime_error("late Event executed a released Handler");
      }
      std::fprintf(stderr,
                   "negative.late_event rejected=1 error=HANDLER_NOT_FOUND\n");
    }
    const auto coordinatorAfterDestroy = coordinatorRaw->snapshot();
    std::fprintf(stderr,
                 "resources.before_cleanup surfaces=%zu nodes=%zu handlers=%zu live_surface=%llu mount_objects=%zu roots=%zu\n",
                 coordinatorAfterDestroy.surfaces,
                 coordinatorAfterDestroy.committed_nodes,
                 eventRouter.handlerCount(),
                 static_cast<unsigned long long>(counters.snapshot().surface_live),
                 mounts.liveObjectCount(), surfaces.liveRootCount());
    if (coordinatorAfterDestroy.surfaces != 0 ||
        coordinatorAfterDestroy.committed_nodes != 0 ||
        eventRouter.handlerCount() != 0 || counters.snapshot().surface_live != 0) {
      throw std::runtime_error("Core resources did not return to baseline");
    }

    controller->force_teardown();
    surfaceController.reset();
    coordinatorRaw->close();
    if (!mounts.finishClose(kOwner).ok() || mounts.liveObjectCount() != 0 ||
        mounts.liveFontCount() != 0)
      throw std::runtime_error("mount resource cleanup failed");
    surfaces.close();
    if (!surfaces.finishClose(kOwner).ok())
      throw std::runtime_error("surface cleanup failed");
    if (!tasks.beginStop(kOwner, qlf::StopPolicy::kCancel).ok() ||
        !tasks.finishStop(kOwner).ok())
      throw std::runtime_error("task cleanup failed");
    publisher.closeAdmission();
    if (!publisher.tryFinalizeClose(kOwner).ok())
      throw std::runtime_error("font cleanup failed");
    lv_indev_delete(mouse);
    lv_display_delete(display);

    std::promise<void> jsCleanup;
    if (engine.post([&](qj::JsEnginePort&, const qj::JsContextRef&) {
          if (handlerRegistry) handlerRegistry->stopOnExecutor();
          if (vm) vm->stopOnExecutor();
          if (transactionBuilder) transactionBuilder->stopOnExecutor();
          if (bindingStage) bindingStage->stopOnExecutor();
          if (pageControls) pageControls->stopOnExecutor();
          if (runtimeAbi) runtimeAbi->stopOnExecutor();
          if (modules) modules->stopOnExecutor();
          if (facades) facades->stopOnExecutor();
          const auto moduleResources = modules->resources();
          const auto vmResources = vm->resources();
          const auto abiResources = runtimeAbi->resources();
          const auto pageResources = pageControls->resources();
          std::fprintf(stderr,
                       "resources.js_after_cleanup handlers=%zu module_entries=%zu page_leases=%zu active_loads=%zu module_bytes=%zu module_pending=%zu app_vms=%zu page_vms=%zu vm_surfaces=%zu abi_entries=%zu abi_correlations=%zu abi_consumers=%zu abi_surfaces=%zu abi_callbacks=%zu page_entries=%zu page_factories=%zu queue_depth=%zu\n",
                       handlerRegistry->size(), moduleResources.liveEntries,
                       moduleResources.livePageLeases,
                       moduleResources.activeLoads, moduleResources.retainedBytes,
                       moduleResources.pendingCompletions, vmResources.appVms,
                       vmResources.pageVms, vmResources.openSurfaces,
                       abiResources.liveNativeEntries,
                       abiResources.liveBridgeCorrelations,
                       abiResources.liveConsumerRegistrations,
                       abiResources.openSurfaceScopes,
                       abiResources.queuedAbiCallbacks,
                       pageResources.liveNativeEntries,
                       pageResources.liveFactoryValues,
                       engine.executor().pendingDepth());
          jsCleanup.set_value();
        }).status != qj::PostStatus::Accepted) {
      throw std::runtime_error("QuickJS cleanup enqueue failed");
    }
    jsCleanup.get_future().get();
    std::promise<void> engineStopped;
    if (!engine.stop([] {}, [&] { engineStopped.set_value(); })) {
      throw std::runtime_error("QuickJS stop failed");
    }
    engineStopped.get_future().get();
    delete handlerRegistry;
    delete vm;
    delete pageStage;
    delete transactionBuilder;
    delete bindingStage;
    delete pageControls;
    runtimeAbi.reset();
    delete modules;
    delete facades;
    loader->close(); identity.reset(); factory.stop(); if (!factory.teardown()) throw std::runtime_error("factory teardown failed");
    if (kInteractiveSimulator) {
      std::cout << "simulator.closed=true\n"
                   "resources_released=true\n";
    } else if (binding001) {
      std::cout << "rpk.binding001=true\n"
                   "render_transaction.revision=1\n"
                   "lvgl.binding_text=1\n"
                   "resources_released=true\n";
    } else {
      std::cout << "rpk.opened=true\n"
                   "event.real_lvgl_button=true\n"
                   "event.sdl_pointer_attached=true\n"
                   "event.dispatch.once=true\n"
                   "navigation.core_stack=true\n"
                   "detail.real_rpk.visible=true\n"
                   "resources_released=true\n";
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "case001_lvgl_error=" << error.what() << '\n'; return 1; }
}
