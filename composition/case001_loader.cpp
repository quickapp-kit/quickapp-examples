#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "quickapp/core/foundation/app_runtime_factory.h"
#include "quickapp/core/package/package_loader.h"

namespace q = quickapp::core;
namespace qp = quickapp::core::package;

namespace {

class FilePackageSource final : public qp::PackageSource {
 public:
  explicit FilePackageSource(qp::Bytes bytes)
      : bytes_(std::make_shared<const qp::Bytes>(std::move(bytes))) {}

  q::RuntimeResult<std::uint64_t> size() noexcept override {
    if (closed_) {
      return q::RuntimeResult<std::uint64_t>::failure(q::RuntimeError::simple(
          q::RuntimeErrorCode::kPackageIoError, "package source is closed"));
    }
    return q::RuntimeResult<std::uint64_t>::success(bytes_->size());
  }

  q::EnqueueResult read_at(qp::PackageReadRequest request,
                           qp::PackageReadCompletion completion) noexcept override {
    if (closed_ || !completion || request.offset > bytes_->size() ||
        request.length > bytes_->size() - request.offset) {
      return q::EnqueueResult::failure(q::RuntimeError::simple(
          q::RuntimeErrorCode::kPackageIoError, "invalid package read"));
    }
    auto result = std::make_shared<qp::Bytes>(
        bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset),
        bytes_->begin() + static_cast<std::ptrdiff_t>(request.offset + request.length));
    completion(qp::PackageReadResult{
        std::move(request.request_id),
        q::RuntimeResult<qp::ImmutableBytes>::success(std::move(result))});
    return q::EnqueueResult::success(q::Accepted{});
  }

  void close() noexcept override { closed_ = true; }

 private:
  std::shared_ptr<const qp::Bytes> bytes_;
  bool closed_{false};
};

qp::Bytes read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open RPK: " + path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  input.seekg(0, std::ios::beg);
  qp::Bytes bytes(static_cast<std::size_t>(length));
  input.read(reinterpret_cast<char*>(bytes.data()), length);
  if (!input) throw std::runtime_error("cannot read RPK: " + path);
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string artifact = argc > 1
      ? argv[1]
      : "../quickapp-toolkit/evidence/tk-s07-case001.rpk";
  try {
    auto source = std::make_shared<FilePackageSource>(read_file(artifact));
    q::AppRuntimeFactory factory;
    auto identity_result = factory.create();
    if (!identity_result) throw std::runtime_error("identity creation failed");
    auto identity = std::move(identity_result).value();

    qp::RuntimeComposition composition;
    composition.runtime_abi = "quickapp-kit-runtime-v1";
    composition.engine_abi = "quickapp-kit-js-engine-v1";
    composition.components = {"View", "Text", "Button"};
    composition.capabilities = {"system.router", "system.prompt", "system.device",
                                "system.fetch", "system.shortcut"};

    auto loader_result = qp::PackageLoader::create(
        source, identity.request_ids(), std::move(composition));
    if (!loader_result) throw std::runtime_error("PackageLoader creation failed");
    auto loader = std::move(loader_result).value();

    bool opened = false;
    std::optional<q::RuntimeError> open_error;
    if (!loader->open([&](auto result) {
          if (!result) {
            open_error = result.error();
            return;
          }
          opened = true;
        })) throw std::runtime_error("RPK open enqueue failed");
    if (open_error) {
      throw std::runtime_error("RPK verification failed: " +
                               std::string(open_error->message));
    }
    if (!opened) throw std::runtime_error("RPK open did not complete");

    bool page_loaded = false;
    std::optional<q::RuntimeError> page_error;
    if (!loader->load_page_ir("/pages/Demo", [&](auto result) {
          if (!result) {
            page_error = result.error();
            return;
          }
          page_loaded = true;
          std::cout << "page_ir.template_id=" << result.value()->template_id() << '\n';
        })) throw std::runtime_error("Page IR enqueue failed");
    if (page_error) {
      throw std::runtime_error("Page IR load failed: " +
                               std::string(page_error->message));
    }
    if (!page_loaded) throw std::runtime_error("Page IR did not complete");

    std::cout << "rpk.opened=true\n";
    std::cout << "package=" << loader->verified_package()->package_id() << '\n';
    std::cout << "entry_route=" << loader->verified_package()->entry_route() << '\n';
    std::cout << "page.route=/pages/Demo\n";
    loader->close();
    identity.reset();
    factory.stop();
    if (!factory.teardown()) throw std::runtime_error("factory teardown failed");
    std::cout << "resources_released=true\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "case001_loader_error=" << error.what() << '\n';
    return 1;
  }
}
