/**
 * @file src/capture_backend.h
 * @brief Injectable capture-backend factory for Hermes video pipelines.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace platf {
  enum class mem_type_e;
  class display_t;
}  // namespace platf

namespace video {
  struct config_t;

  namespace capture_backend {

    /**
     * @brief Factory boundary for platform capture sessions.
     *
     * H1 deliberately retains platf::display_t as the frame/image ABI used by
     * the existing capture and encoder loops. Implementations own backend
     * selection and session creation, but do not own returned shared sessions.
     * Calls happen outside the per-frame callback and may enumerate devices.
     */
    class ICaptureBackend {
    public:
      virtual ~ICaptureBackend() = default;

      [[nodiscard]] virtual std::vector<std::string> enumerate_displays(
        platf::mem_type_e memory_type
      ) = 0;
      [[nodiscard]] virtual std::shared_ptr<platf::display_t> create_display(
        platf::mem_type_e memory_type,
        const std::string &display_name,
        const config_t &config
      ) = 0;
    };

    /**
     * @brief Adapter preserving the current platform capture selection.
     */
    class legacy_platform_capture_backend_t final:
        public ICaptureBackend {
    public:
      [[nodiscard]] std::vector<std::string> enumerate_displays(
        platf::mem_type_e memory_type
      ) override;
      [[nodiscard]] std::shared_ptr<platf::display_t> create_display(
        platf::mem_type_e memory_type,
        const std::string &display_name,
        const config_t &config
      ) override;
    };

  }  // namespace capture_backend
}  // namespace video
