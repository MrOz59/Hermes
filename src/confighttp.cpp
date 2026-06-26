/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <numeric>
#include <algorithm>
#include <atomic>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/v1/search_path.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

#ifdef _WIN32
  #include "platform/windows/utils.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  // Defined later; declared here so the web-UI metrics handler can reuse it.
  nlohmann::json hestia_runtime_status_json();

  class HestiaHTTPSServer: public SimpleWeb::ServerBase<SimpleWeb::HTTPS> {
  public:
    HestiaHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SimpleWeb::HTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<void(std::shared_ptr<Request>, SSL *)> authenticate_client;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      // Request a client certificate but permit clients that do not have one so
      // the read-only capabilities probe remains available before pairing.
      context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_client_once);
      context.set_verify_callback([](int, boost::asio::ssl::verify_context &) {
        return true;
      });
    }

    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);
        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code socket_error;
          session->connection->socket->lowest_layer().set_option(option, socket_error);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &handshake_error) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }

            if (!handshake_error) {
              if (authenticate_client) {
                authenticate_client(session->request, session->connection->socket->native_handle());
              }
              this->read(session);
            } else if (this->on_error) {
              this->on_error(session->request, handshake_error);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = HestiaHTTPSServer;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  // SESSION COOKIE
  std::string sessionCookie;
  static std::chrono::time_point<std::chrono::steady_clock> cookie_creation_time;

  // 0 = idle, 1 = running, 2 = succeeded, 3 = failed/cancelled.
  static std::atomic<int> evdi_install_status {0};
  // 0 = idle, 1 = running, 2 = succeeded, 3 = failed/cancelled.
  static std::atomic<int> clipboard_install_status {0};

#ifdef __linux__
  static const char *evdi_diagnostic_name(VDISPLAY::EVDI_DIAGNOSTIC diagnostic) {
    switch (diagnostic) {
      case VDISPLAY::EVDI_DIAGNOSTIC::READY:
        return "ready";
      case VDISPLAY::EVDI_DIAGNOSTIC::INITIAL_DEVICE_CONFIGURATION_REQUIRED:
        return "setup_required";
      case VDISPLAY::EVDI_DIAGNOSTIC::LIBRARY_MISSING:
        return "library_missing";
      case VDISPLAY::EVDI_DIAGNOSTIC::MODULE_NOT_INSTALLED:
        return "module_not_installed";
      case VDISPLAY::EVDI_DIAGNOSTIC::MODULE_NOT_LOADED:
        return "module_not_loaded";
      case VDISPLAY::EVDI_DIAGNOSTIC::DKMS_BUILD_FAILED:
        return "dkms_build_failed";
    }

    return "module_not_loaded";
  }

  static nlohmann::json evdi_status_json() {
    const auto status = VDISPLAY::getEvdiStatus();
    nlohmann::json output {
      {"diagnostic", evdi_diagnostic_name(status.diagnostic)},
      {"libraryInstalled", status.library_installed},
      {"libraryLoaded", status.library_loaded},
      {"moduleLoaded", status.module_loaded},
      {"moduleInstalled", status.module_installed},
      {"deviceCount", status.device_count},
      {"sessionType", status.session_type},
      {"exclusiveLayoutSupported", status.exclusive_layout_supported},
      {"outputLayoutBackend", status.output_layout_backend},
      {"captureFallbackActive", status.capture_fallback_active},
      {"libraryVersion", status.library_version},
      {"runningKernel", status.running_kernel},
      {"dkmsKernels", status.dkms_kernels},
      {"activeDisplays", nlohmann::json::array()},
    };

    for (const auto &display : status.active_displays) {
      output["activeDisplays"].push_back({
        {"name", display.name},
        {"deviceIndex", display.device_index},
        {"drmCardIndex", display.drm_card_index},
        {"width", display.width},
        {"height", display.height},
        {"fps", display.fps},
        {"frameUpdates", display.frame_updates},
        {"capturePath", "evdi_cpu_buffer"},
        {"zeroCopyCapture", false},
        {"hardwareEncodingAvailable", true},
      });
    }

    return output;
  }

  static nlohmann::json clipboard_status_json() {
    const bool wl_copy_available = !boost::process::v1::search_path("wl-copy").empty();
    const bool wl_paste_available = !boost::process::v1::search_path("wl-paste").empty();
    const bool xclip_available = !boost::process::v1::search_path("xclip").empty();
    const bool wayland_display_available = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool x11_display_available = std::getenv("DISPLAY") != nullptr;
    const char *diagnostic = platf::clipboard_available() ? "ready" :
                             wayland_display_available && (!wl_copy_available || !wl_paste_available) ? "wl_clipboard_missing" :
                             x11_display_available && !xclip_available ? "xclip_missing" :
                             (!wayland_display_available && !x11_display_available) ? "desktop_session_unavailable" : "clipboard_unavailable";
    return {
      {"available", platf::clipboard_available()},
      {"diagnostic", diagnostic},
      {"wlCopyAvailable", wl_copy_available},
      {"wlPasteAvailable", wl_paste_available},
      {"xclipAvailable", xclip_available},
      {"waylandDisplayAvailable", wayland_display_available},
      {"x11DisplayAvailable", x11_display_available},
      {"manualInstall", "Arch/CachyOS: sudo pacman -S --needed wl-clipboard xclip\nDebian/Ubuntu: sudo apt install wl-clipboard xclip\nFedora: sudo dnf install wl-clipboard xclip"},
    };
  }
#endif

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;
    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }
    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_unauthorized;
    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- redirecting"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Retrieve the value of a key from a cookie string.
   * @param cookieString The cookie header string.
   * @param key The key to search.
   * @return The value if found, empty string otherwise.
   */
  std::string getCookieValue(const std::string& cookieString, const std::string& key) {
    std::string keyWithEqual = key + "=";
    std::size_t startPos = cookieString.find(keyWithEqual);
    if (startPos == std::string::npos)
      return "";
    startPos += keyWithEqual.length();
    std::size_t endPos = cookieString.find(";", startPos);
    if (endPos == std::string::npos)
      return cookieString.substr(startPos);
    return cookieString.substr(startPos, endPos - startPos);
  }

  /**
   * @brief Check if the IP origin is allowed.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if allowed, false otherwise.
   */
  bool checkIPOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }

  /**
   * @brief Authenticate the request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param needsRedirect Whether to redirect on failure.
   * @return True if authenticated, false otherwise.
   *
   * This function uses session cookies (if set) and ensures they have not expired.
   */
  bool authenticate(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (!checkIPOrigin(response, request))
      return false;
    // If credentials not set, redirect to welcome.
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }
    // Guard: on failure, redirect if requested.
    auto fg = util::fail_guard([&]() {
      if (needsRedirect) {
        std::string redir_path = "/login?redir=.";
        redir_path += request->path;
        send_redirect(response, request, redir_path.c_str());
      } else {
        send_unauthorized(response, request);
      }
    });
    if (sessionCookie.empty())
      return false;
    // Check for expiry
    if (std::chrono::steady_clock::now() - cookie_creation_time > SESSION_EXPIRE_DURATION) {
      sessionCookie.clear();
      return false;
    }
    auto cookies = request->header.find("cookie");
    if (cookies == request->header.end())
      return false;
    auto authCookie = getCookieValue(cookies->second, "auth");
    if (authCookie.empty() ||
        util::hex(crypto::hash(authCookie + config::sunshine.salt)).to_string() != sessionCookie)
      return false;
    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_not_found;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["error"] = "Not Found";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_bad_request;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }


  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The required content type.
   */
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view& contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }

    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;

    return true;
  }

  /**
   * @brief Get the index page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getIndexPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the PIN page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPinPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "pin.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the apps page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAppsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "apps.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    response->write(content, headers);
  }

  /**
   * @brief Get the clients page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getClientsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "clients.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the configuration page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfigPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "config.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the password page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPasswordPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "password.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the login page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo Combine this function with getWelcomePage if appropriate.
   */
  void getLoginPage(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "login.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the welcome page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getWelcomePage(resp_https_t response, req_https_t request) {
    print_req(request);

    if (!config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "welcome.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the troubleshooting page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getTroubleshootingPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "troubleshooting.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/apollo.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Apollo logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getApolloLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-apollo-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);

    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }

    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  // Web UI metrics endpoint: same runtime view as the Hestia diagnostics, but
  // behind the normal web-UI session auth so the dashboard can poll it.
  void getMetrics(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    send_response(response, hestia_runtime_status_json());
  }

  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      file_tree["current_app"] = proc::proc.get_running_app_uuid();
      file_tree["host_uuid"] = http::unique_id;
      file_tree["host_name"] = config::nvhttp.sunshine_name;

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application the UUID must be empty.
   *        To update an existing application, you must provide the current UUID of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file.",
   *   "uuid": "aaaa-bbbb"
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","uuid": "aaaa-bbbb"}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    BOOST_LOG(info) << config::stream.file_apps;
    try {
      // TODO: Input Validation

      // Read the input JSON from the request body.
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Migrate/merge the new app into the file tree.
      proc::migrate_apps(&fileTree, &inputTree);

      // Write the updated file tree back to disk.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the output response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Reorder applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/reorder| POST| {"order": ["aaaa-bbbb", "cccc-dddd"]}}
   */
  void reorderApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();

      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Get the desired order of UUIDs from the request.
      if (!input_tree.contains("order") || !input_tree["order"].is_array()) {
        throw std::runtime_error("Missing or invalid 'order' array in request body");
      }
      const auto& order_uuids_json = input_tree["order"];

      // Get the original apps array from the fileTree.
      // Default to an empty array if "apps" key is missing or if it's present but not an array (after logging an error).
      nlohmann::json original_apps_list = nlohmann::json::array();
      if (fileTree.contains("apps")) {
        if (fileTree["apps"].is_array()) {
          original_apps_list = fileTree["apps"];
        } else {
          // "apps" key exists but is not an array. This is a malformed state.
          BOOST_LOG(error) << "ReorderApps: 'apps' key in apps configuration file ('" << config::stream.file_apps
                           << "') is present but not an array.";
          throw std::runtime_error("'apps' in file is not an array, cannot reorder.");
        }
      } else {
        // "apps" key is missing. Treat as an empty list. Reordering an empty list is valid.
        BOOST_LOG(debug) << "ReorderApps: 'apps' key missing in apps configuration file ('" << config::stream.file_apps
                         << "'). Treating as an empty list for reordering.";
        // original_apps_list is already an empty array, so no specific action needed here.
      }

      nlohmann::json reordered_apps_list = nlohmann::json::array();
      std::vector<bool> item_moved(original_apps_list.size(), false);

      // Phase 1: Place apps according to the 'order' array from the request.
      // Iterate through the desired order of UUIDs.
      for (const auto& uuid_json_value : order_uuids_json) {
        if (!uuid_json_value.is_string()) {
          BOOST_LOG(warning) << "ReorderApps: Encountered a non-string UUID in the 'order' array. Skipping this entry.";
          continue;
        }
        std::string target_uuid = uuid_json_value.get<std::string>();
        bool found_match_for_ordered_uuid = false;

        // Find the first unmoved app in the original list that matches the current target_uuid.
        for (size_t i = 0; i < original_apps_list.size(); ++i) {
          if (item_moved[i]) {
            continue; // This specific app object has already been placed.
          }

          const auto& app_item = original_apps_list[i];
          // Ensure the app item is an object and has a UUID to match against.
          if (app_item.is_object() && app_item.contains("uuid") && app_item["uuid"].is_string()) {
            if (app_item["uuid"].get<std::string>() == target_uuid) {
              reordered_apps_list.push_back(app_item); // Add the found app object to the new list.
              item_moved[i] = true;                    // Mark this specific object as moved.
              found_match_for_ordered_uuid = true;
              break; // Found an app for this UUID, move to the next UUID in the 'order' array.
            }
          }
        }

        if (!found_match_for_ordered_uuid) {
          // This means a UUID specified in the 'order' array was not found in the original_apps_list
          // among the currently available (unmoved) app objects.
          // Per instruction "If the uuid is missing from the original json file, omit it."
          BOOST_LOG(debug) << "ReorderApps: UUID '" << target_uuid << "' from 'order' array not found in available apps list or its matching app was already processed. Omitting.";
        }
      }

      // Phase 2: Append any remaining apps from the original list that were not explicitly ordered.
      // These are app objects that were not marked 'item_moved' in Phase 1.
      for (size_t i = 0; i < original_apps_list.size(); ++i) {
        if (!item_moved[i]) {
          reordered_apps_list.push_back(original_apps_list[i]);
        }
      }

      // Update the fileTree with the new, reordered list of apps.
      fileTree["apps"] = reordered_apps_list;

      // Write the modified fileTree back to the apps configuration file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));

      // Notify relevant parts of the system that the apps configuration has changed.
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ReorderApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/delete | POST| { uuid: 'aaaa-bbbb' }}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      auto uuid = input_tree["uuid"].get<std::string>();

      // Read the apps file into a nlohmann::json object.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Remove any app with the matching uuid directly from the "apps" array.
      if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
        auto& apps = fileTree["apps"];
        apps.erase(
          std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json& app) {
            return app.value("uuid", "") == uuid;
          }),
          apps.end()
        );
      }

      // Write the updated JSON back to the file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
#ifdef _WIN32
    output_tree["platform"] = "windows";
#endif
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Update client information.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "name": "<Friendly Name>",
   *   "display_mode": "1920x1080x59.94",
   *   "do": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "undo": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "perm": <uint32_t>
   * }
   * @endcode
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      std::string name = input_tree.value("name", "");
      std::string display_mode = input_tree.value("display_mode", "");
      bool enable_legacy_ordering = input_tree.value("enable_legacy_ordering", true);
      bool allow_client_commands = input_tree.value("allow_client_commands", true);
      bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      auto do_cmds = nvhttp::extract_command_entries(input_tree, "do");
      auto undo_cmds = nvhttp::extract_command_entries(input_tree, "undo");
      auto perm = static_cast<crypto::PERM>(input_tree.value("perm", static_cast<uint32_t>(crypto::PERM::_no)) & static_cast<uint32_t>(crypto::PERM::_all));
      output_tree["status"] = nvhttp::update_device_info(
        uuid,
        name,
        display_mode,
        do_cmds,
        undo_cmds,
        perm,
        enable_legacy_ordering,
        allow_client_commands,
        always_use_virtual_display
      );
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/clients/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::unpair_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#ifdef __linux__
    output_tree["evdiSetupRequired"] = VDISPLAY::needsInitialDeviceConfiguration();
    output_tree["evdiInfo"] = evdi_status_json();
    output_tree["evdiDiagnostic"] = output_tree["evdiInfo"]["diagnostic"];
    output_tree["clipboardInfo"] = clipboard_status_json();
#endif
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output_tree[name] = value;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Return the static Hestia extension capabilities.
   *
   * This endpoint is deliberately read-only and unauthenticated so clients can
   * detect Hermes before an extension authentication flow is available. It does
   * not expose host state or permit any host-control operation.
   */
  void getHestiaCapabilities(resp_https_t response, req_https_t request) {
    print_req(request);

    const nlohmann::json capabilities {
      {"ok", true},
      {"server_name", "Hermes"},
      {"base", "Hermes"},
      {"hestia_protocol", 1},
      {"min_client_protocol", 1},
      {"max_client_protocol", 1},
      {"server_version", "0.1.0-hermes"},
      {"compatibility", {
        {"gamestream", true},
        {"moonlight", true},
        {"sunshine", true},
      }},
      {"features", {
        {"virtual_display", true},
        {"virtual_display_backend", {"evdi", "hermes_kms"}},
        {"kde_kscreen", true},
        {"display_recovery", true},
        {"client_resolution_matching", true},
        {"client_fps_matching", true},
        {"hdr_mode_control", true},
        {"scale_factor", true},
        {"gamescope_session", !boost::process::v1::search_path("gamescope").empty()},
        {"server_commands", true},
        {"clipboard_sync", platf::clipboard_available()},
        {"permission_system", true},
      }},
      {"limits", {
        {"max_width", 7680},
        {"max_height", 4320},
        {"max_fps", 120},
        {"supported_fps", {30, 40, 45, 60, 90, 120}},
        {"supported_codecs", {"h264", "hevc", "av1"}},
      }},
    };

    send_response(response, capabilities);
  }

  void send_hestia_error(resp_https_t response, SimpleWeb::StatusCode status, const char *code, const char *message) {
    const nlohmann::json output {
      {"ok", false},
      {"error", {
        {"code", code},
        {"message", message},
      }},
    };
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"},
    };
    response->write(status, output.dump(), headers);
  }

  bool authenticate_hestia_client(resp_https_t response, req_https_t request, crypto::PERM permission, const char *operation) {
    auto client = std::static_pointer_cast<crypto::named_cert_t>(request->userp);
    if (!client) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "unauthorized", "A paired client certificate is required");
      return false;
    }

    if (!(client->perm & permission)) {
      BOOST_LOG(info) << "[HestiaAPI] permission denied for " << operation << " client=" << client->uuid;
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", "This client does not have the required permission");
      return false;
    }

    return true;
  }

  std::shared_ptr<crypto::named_cert_t> authenticate_hestia_paired_client(resp_https_t response, req_https_t request) {
    auto client = std::static_pointer_cast<crypto::named_cert_t>(request->userp);
    if (!client) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "unauthorized", "A paired client certificate is required");
    }
    return client;
  }

  bool hestia_has_exact_keys(const nlohmann::json &object, const std::set<std::string> &keys) {
    if (!object.is_object() || object.size() != keys.size()) {
      return false;
    }

    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!keys.contains(it.key())) {
        return false;
      }
    }

    return true;
  }

  bool hestia_is_positive_integer(const nlohmann::json &value) {
    return value.is_number_integer() && value.get<int>() > 0;
  }

  bool hestia_is_one_of(const nlohmann::json &value, const std::set<std::string> &values) {
    return value.is_string() && values.contains(value.get<std::string>());
  }

  bool validate_hestia_session_prepare(const nlohmann::json &request, std::string &error) {
    static const std::set<std::string> request_keys {
      "client", "stream", "virtual_display", "app",
    };
    static const std::set<std::string> client_keys {
      "name", "version", "platform", "display_width", "display_height", "refresh_rate", "hdr",
    };
    static const std::set<std::string> stream_keys {
      "requested_width", "requested_height", "requested_fps", "codec", "bitrate_kbps", "hdr_mode", "scale_factor",
    };
    static const std::set<std::string> virtual_display_keys {
      "enabled", "backend", "desktop_integration", "recover_physical_monitor",
    };
    static const std::set<std::string> app_keys {
      "id", "launch_mode",
    };

    if (!hestia_has_exact_keys(request, request_keys)) {
      error = "Request must contain only client, stream, virtual_display, and app";
      return false;
    }

    const auto &client = request["client"];
    if (!hestia_has_exact_keys(client, client_keys) || !client["name"].is_string() ||
        !client["version"].is_string() || !hestia_is_one_of(client["platform"], {"linux", "windows", "macos", "steamdeck", "unknown"}) ||
        !hestia_is_positive_integer(client["display_width"]) || !hestia_is_positive_integer(client["display_height"]) ||
        !hestia_is_positive_integer(client["refresh_rate"]) || !client["hdr"].is_boolean()) {
      error = "Invalid client object";
      return false;
    }

    const auto &stream = request["stream"];
    if (!hestia_has_exact_keys(stream, stream_keys) || !hestia_is_positive_integer(stream["requested_width"]) ||
        !hestia_is_positive_integer(stream["requested_height"]) || !hestia_is_positive_integer(stream["requested_fps"]) ||
        !hestia_is_one_of(stream["codec"], {"h264", "hevc", "av1"}) || !hestia_is_positive_integer(stream["bitrate_kbps"]) ||
        !hestia_is_one_of(stream["hdr_mode"], {"sdr", "hdr"}) || !hestia_is_positive_integer(stream["scale_factor"])) {
      error = "Invalid stream object";
      return false;
    }

    const int requested_width = stream["requested_width"].get<int>();
    const int requested_height = stream["requested_height"].get<int>();
    const int requested_fps = stream["requested_fps"].get<int>();
    const int scale_factor = stream["scale_factor"].get<int>();
    static const std::set<int> supported_fps {30, 40, 45, 60, 90, 120};
    if (requested_width > 7680 || requested_height > 4320 || !supported_fps.contains(requested_fps) || scale_factor > 500) {
      error = "Requested stream settings exceed host limits";
      return false;
    }

    const auto &virtual_display = request["virtual_display"];
    if (!hestia_has_exact_keys(virtual_display, virtual_display_keys) || !virtual_display["enabled"].is_boolean() ||
        !hestia_is_one_of(virtual_display["backend"], {"auto", "evdi", "hermes_kms", "none"}) ||
        !hestia_is_one_of(virtual_display["desktop_integration"], {"auto", "kde_kscreen", "none"}) ||
        !virtual_display["recover_physical_monitor"].is_boolean()) {
      error = "Invalid virtual_display object";
      return false;
    }

    const auto &app = request["app"];
    if (!hestia_has_exact_keys(app, app_keys) || !app["id"].is_string() ||
        !hestia_is_one_of(app["launch_mode"], {"normal", "desktop", "steam_big_picture", "gamescope", "custom"})) {
      error = "Invalid app object";
      return false;
    }

    return true;
  }

  /**
   * @brief Store settings for a paired Hestia client's next stream launch.
   *
   * The normal GameStream launch path consumes the short-lived settings and
   * creates a virtual display only when the stream actually begins.
   */
  void prepare_hestia_session(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::launch, "session prepare")) {
      return;
    }

    auto content_type = request->header.find("content-type");
    if (content_type == request->header.end() || !boost::istarts_with(content_type->second, "application/json")) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Content-Type must be application/json");
      return;
    }

    try {
      const nlohmann::json input = nlohmann::json::parse(request->content.string());
      std::string validation_error;
      if (!validate_hestia_session_prepare(input, validation_error)) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", validation_error.c_str());
        return;
      }

      const auto &stream = input["stream"];
      const auto &virtual_display = input["virtual_display"];
      const auto &app = input["app"];
#ifdef _WIN32
      if (app["launch_mode"] == "gamescope") {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "unsupported_feature", "Gamescope is only available on Linux hosts");
        return;
      }
#else
      if (app["launch_mode"] == "gamescope" && boost::process::v1::search_path("gamescope").empty()) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "unsupported_feature", "Gamescope is not available on this host");
        return;
      }
#endif
      const auto client = std::static_pointer_cast<crypto::named_cert_t>(request->userp);
      nvhttp::store_hestia_session_prepare(client->uuid, {
        .virtual_display = virtual_display["enabled"].get<bool>(),
        .width = stream["requested_width"].get<int>(),
        .height = stream["requested_height"].get<int>(),
        .fps = stream["requested_fps"].get<int>(),
        .hdr = stream["hdr_mode"] == "hdr",
        .scale_factor = stream["scale_factor"].get<uint32_t>(),
        .launch_mode = app["launch_mode"].get<std::string>(),
      });
      BOOST_LOG(debug) << "[HestiaAPI] session prepare requested: width=" << stream["requested_width"]
                       << " height=" << stream["requested_height"] << " fps=" << stream["requested_fps"]
                       << " virtual_display=" << virtual_display["enabled"]
                       << " launch_mode=" << app["launch_mode"];

      const nlohmann::json output {
        {"ok", true},
        {"session_id", "hestia-" + crypto::rand_alphabet(12)},
        {"virtual_display", {
          {"created", false},
          {"name", ""},
          {"backend", virtual_display["backend"].get<std::string>()},
          {"width", stream["requested_width"]},
          {"height", stream["requested_height"]},
          {"fps", stream["requested_fps"]},
          {"hdr", stream["hdr_mode"] == "hdr"},
          {"kscreen_enabled", false},
        }},
        {"warnings", {"Prepared settings will be applied when the normal stream launches."}},
      };
      send_response(response, output);
    } catch (const nlohmann::json::exception &) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request body must be valid JSON");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "[HestiaAPI] session prepare failed: " << e.what();
      send_hestia_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "internal_error", "Unable to prepare session");
    }
  }

  void stop_hestia_session(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::launch, "session stop")) {
      return;
    }

    const auto client = std::static_pointer_cast<crypto::named_cert_t>(request->userp);
    nvhttp::clear_hestia_session_prepare(client->uuid);
    const bool stopped = nvhttp::find_and_stop_session(client->uuid, true);
    BOOST_LOG(debug) << "[HestiaAPI] session stop requested by client=" << client->uuid
                     << " stopped=" << stopped;
    send_response(response, {{"ok", true}, {"stopped", stopped}});
  }

  nlohmann::json hestia_display_status_json() {
#ifdef __linux__
    const auto evdi_status = VDISPLAY::getEvdiStatus();
    const bool virtual_display_active = !evdi_status.active_displays.empty();
    const auto *display = virtual_display_active ? &evdi_status.active_displays.front() : nullptr;
    const bool exclusive_virtual_display = virtual_display_active && config::video.isolated_virtual_display_option;

    return {
      {"ok", true},
      {"active", virtual_display_active},
      {"virtual_display", {
        {"enabled", virtual_display_active},
        {"name", display ? display->name : ""},
        {"backend", virtual_display_active ? "evdi" : "none"},
        {"width", display ? display->width : 0},
        {"height", display ? display->height : 0},
        {"fps", display ? display->fps : 0},
        {"hdr", false},
      }},
      {"physical_display", {
        {"disabled_during_stream", exclusive_virtual_display},
        {"saved_primary", exclusive_virtual_display ? proc::proc.initial_display : ""},
        {"recovery_state_exists", exclusive_virtual_display},
      }},
      {"desktop", {
        {"environment", evdi_status.session_type},
        {"kscreen_available", evdi_status.output_layout_backend == "kscreen"},
        {"kscreen_output", display ? display->name : ""},
      }},
    };
#else
    return {
      {"ok", true},
      {"active", false},
      {"virtual_display", {{"enabled", false}, {"name", ""}, {"backend", "none"}, {"width", 0}, {"height", 0}, {"fps", 0}, {"hdr", false}}},
      {"physical_display", {{"disabled_during_stream", false}, {"saved_primary", ""}, {"recovery_state_exists", false}}},
      {"desktop", {{"environment", "unknown"}, {"kscreen_available", false}, {"kscreen_output", ""}}},
    };
#endif
  }

  void get_hestia_display_status(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::view, "display status")) {
      return;
    }

    send_response(response, hestia_display_status_json());
  }

  void recover_hestia_display(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::launch, "display recovery")) {
      return;
    }

    auto content_type = request->header.find("content-type");
    if (content_type == request->header.end() || !boost::istarts_with(content_type->second, "application/json")) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Content-Type must be application/json");
      return;
    }

    try {
      const nlohmann::json input = nlohmann::json::parse(request->content.string());
      if (!hestia_has_exact_keys(input, {"force"}) || !input["force"].is_boolean()) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request must contain a boolean force field");
        return;
      }

      const std::string restored_output = proc::proc.initial_display;
      if (proc::proc.running() > 0) {
        proc::proc.terminate();
      } else {
#ifdef __linux__
        VDISPLAY::restoreExclusiveVirtualDisplay();
#endif
        display_device::revert_configuration();
      }

      const nlohmann::json output {
        {"ok", true},
        {"message", "Physical display restored"},
        {"restored_output", restored_output},
      };
      send_response(response, output);
    } catch (const nlohmann::json::exception &) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request body must be valid JSON");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "[HestiaAPI] display recovery failed: " << e.what();
      send_hestia_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "display_recovery_failed", "Unable to recover the physical display");
    }
  }

  void get_hestia_commands(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::server_cmd, "server command list")) {
      return;
    }

    nlohmann::json commands = nlohmann::json::array();
    for (size_t index = 0; index < config::sunshine.server_cmds.size(); ++index) {
      commands.push_back({
        {"id", index},
        {"name", config::sunshine.server_cmds[index].cmd_name},
        {"requires_confirmation", true},
      });
    }

    send_response(response, {{"ok", true}, {"commands", commands}});
  }

  void get_hestia_client_permissions(resp_https_t response, req_https_t request) {
    auto client = authenticate_hestia_paired_client(response, request);
    if (!client) {
      return;
    }

    const auto has_permission = [&client](crypto::PERM permission) {
      return static_cast<bool>(client->perm & permission);
    };
    send_response(response, {
      {"ok", true},
      {"client_id", client->uuid},
      {"permissions", {
        {"view_streams", has_permission(crypto::PERM::view)},
        {"list_apps", has_permission(crypto::PERM::list)},
        {"launch_apps", has_permission(crypto::PERM::launch)},
        {"keyboard_input", has_permission(crypto::PERM::input_kbd)},
        {"mouse_input", has_permission(crypto::PERM::input_mouse)},
        {"gamepad_input", has_permission(crypto::PERM::input_controller)},
        {"virtual_display", has_permission(crypto::PERM::launch)},
        {"server_commands", has_permission(crypto::PERM::server_cmd)},
        {"clipboard_sync", platf::clipboard_available() && has_permission(crypto::PERM::clipboard_set) && has_permission(crypto::PERM::clipboard_read)},
        {"display_recovery", has_permission(crypto::PERM::launch)},
      }},
    });
  }

  void get_hestia_clipboard(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::clipboard_read, "clipboard read")) {
      return;
    }
    if (!platf::clipboard_available()) {
      send_hestia_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "clipboard_unavailable", "Clipboard synchronization is unavailable on this host");
      return;
    }

    send_response(response, {{"ok", true}, {"text", platf::get_clipboard()}});
  }

  // Build the live encoder/session view shared by the diagnostics endpoint, so
  // users can see the real encoder (hardware vs software) and who is connected
  // instead of guessing from logs.
  nlohmann::json hestia_runtime_status_json() {
    nlohmann::json runtime;

    const auto enc = video::get_encoder_status();
    if (enc.probed) {
      nlohmann::json codecs = nlohmann::json::array();
      if (enc.h264) {
        codecs.push_back("h264");
      }
      if (enc.hevc) {
        codecs.push_back("hevc");
      }
      if (enc.av1) {
        codecs.push_back("av1");
      }
      runtime["encoder"] = {
        {"name", enc.encoder},
        {"hardware", enc.hardware},
        {"codecs", std::move(codecs)},
      };
    } else {
      runtime["encoder"] = {{"name", nullptr}, {"hardware", false}, {"probed", false}};
    }

    const int sessions = rtsp_stream::session_count();
    runtime["sessions"] = {
      {"active", sessions},
      {"streaming", sessions > 0},
    };

    // Live per-frame pipeline metrics (only meaningful while streaming).
    const auto pm = video::get_pipeline_metrics();
    if (pm.valid) {
      runtime["pipeline"] = {
        {"fps", pm.fps},
        {"bitrate_kbps", pm.bitrate_kbps},
        {"encode_ms", pm.encode_ms},
        {"capture_to_encode_ms", pm.capture_to_encode_ms},
        {"frames_encoded", pm.frames_encoded},
        {"frames_dropped", pm.frames_dropped},
      };
    } else {
      runtime["pipeline"] = nullptr;
    }

    return runtime;
  }

  void get_hestia_diagnostics(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::view, "diagnostics")) {
      return;
    }

#ifdef __linux__
    send_response(response, {{"ok", true}, {"runtime", hestia_runtime_status_json()}, {"dependencies", {{"clipboard", clipboard_status_json()}}}});
#else
    send_response(response, {{"ok", true}, {"runtime", hestia_runtime_status_json()}, {"dependencies", {{"clipboard", {{"available", platf::clipboard_available()}, {"diagnostic", platf::clipboard_available() ? "ready" : "clipboard_unavailable"}, {"manualInstall", "Use the native clipboard service for this platform."}}}}}});
#endif
  }

  void set_hestia_clipboard(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::clipboard_set, "clipboard write")) {
      return;
    }
    if (!platf::clipboard_available()) {
      send_hestia_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "clipboard_unavailable", "Clipboard synchronization is unavailable on this host");
      return;
    }

    auto content_type = request->header.find("content-type");
    if (content_type == request->header.end() || !boost::istarts_with(content_type->second, "application/json")) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Content-Type must be application/json");
      return;
    }

    try {
      const nlohmann::json input = nlohmann::json::parse(request->content.string());
      if (!hestia_has_exact_keys(input, {"text"}) || !input["text"].is_string()) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request must contain only a text field");
        return;
      }
      const std::string text = input["text"].get<std::string>();
      if (text.size() > 64 * 1024 || text.find('\0') != std::string::npos || !platf::set_clipboard(text)) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Clipboard text is invalid, too large, or unavailable");
        return;
      }
      send_response(response, {{"ok", true}});
    } catch (const nlohmann::json::exception &) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request body must be valid JSON");
    }
  }

  void run_hestia_command(resp_https_t response, req_https_t request) {
    if (!authenticate_hestia_client(response, request, crypto::PERM::server_cmd, "server command execution")) {
      return;
    }

    auto content_type = request->header.find("content-type");
    if (content_type == request->header.end() || !boost::istarts_with(content_type->second, "application/json")) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Content-Type must be application/json");
      return;
    }

    try {
      const nlohmann::json input = nlohmann::json::parse(request->content.string());
      if (!hestia_has_exact_keys(input, {"id"}) || !input["id"].is_number_integer()) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request must contain an integer id field");
        return;
      }

      const int command_id = input["id"].get<int>();
      if (command_id < 0 || static_cast<size_t>(command_id) >= config::sunshine.server_cmds.size()) {
        send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Unknown server command id");
        return;
      }

      const auto &configured_command = config::sunshine.server_cmds[command_id];
      const std::string command = configured_command.cmd_val;
      const bool elevated = configured_command.elevated;
      BOOST_LOG(info) << "[HestiaAPI] executing configured server command id=" << command_id;
      std::thread([command, elevated] {
        std::error_code ec;
        auto env = proc::proc.get_env();
        auto working_dir = proc::find_working_directory(command, env);
        auto child = platf::run_command(elevated, true, command, working_dir, env, nullptr, ec, nullptr);
        if (ec) {
          BOOST_LOG(error) << "[HestiaAPI] server command failed to start: " << ec.message();
        } else {
          child.detach();
        }
      }).detach();

      send_response(response, {{"ok", true}});
    } catch (const nlohmann::json::exception &) {
      send_hestia_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_request", "Request body must be valid JSON");
    }
  }

  /**
   * @brief Get the locale setting.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * Start EVDI installation only after the user explicitly confirms it in the UI.
   * pkexec owns the privilege prompt, so Apollo never handles an admin password.
   */
  void installEvdi(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    nlohmann::json output_tree;

#ifdef __linux__
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (!nlohmann::json::parse(ss).value("confirm", false)) {
        bad_request(response, request, "EVDI installation requires explicit confirmation");
        return;
      }

      // Fixed command only: no request data is passed to a privileged shell.
      constexpr auto install_command = R"EVDI(pkexec sh -c 'set -eu
if command -v pacman >/dev/null 2>&1; then
  pacman -S --needed --noconfirm evdi
elif command -v apt-get >/dev/null 2>&1; then
  apt-get update
  apt-get install -y evdi-dkms libevdi1
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y evdi akmod-evdi
else
  echo "Unsupported package manager. Install EVDI manually." >&2
  exit 2
fi
if command -v dkms >/dev/null 2>&1; then
  dkms autoinstall -k "$(uname -r)"
fi
printf "options evdi initial_device_count=1\n" > /etc/modprobe.d/apollo-evdi.conf
modprobe -r evdi 2>/dev/null || true
modprobe evdi
printf "evdi\\n" > /etc/modules-load.d/evdi.conf')EVDI";

      if (evdi_install_status.load() == 1) {
        output_tree["status"] = true;
        output_tree["installStatus"] = "running";
        send_response(response, output_tree);
        return;
      }

      evdi_install_status.store(1);
      std::thread([install_command] {
        auto working_dir = boost::filesystem::path(std::getenv("HOME") ?: "/tmp");
        auto env = boost::this_process::environment();
        std::error_code ec;
        auto child = platf::run_command(false, true, install_command, working_dir, env, nullptr, ec, nullptr);
        if (ec) {
          BOOST_LOG(warning) << "Unable to launch EVDI installer: " << ec.message();
          evdi_install_status.store(3);
          return;
        }

        child.wait();
        if (child.exit_code() != 0) {
          BOOST_LOG(warning) << "EVDI installer exited with code " << child.exit_code();
          evdi_install_status.store(3);
          return;
        }

        proc::initVDisplayDriver();
        evdi_install_status.store(proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK ? 2 : 3);
      }).detach();

      output_tree["status"] = true;
      output_tree["installStatus"] = "running";
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "InstallEvdi: " << e.what();
      bad_request(response, request, e.what());
      return;
    }
#else
    output_tree["status"] = false;
    output_tree["error"] = "EVDI is only available on Linux.";
#endif

    send_response(response, output_tree);
  }

  /** Return the state of the EVDI installation started by this Web UI. */
  void getEvdiInstallStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["installStatus"] = evdi_install_status.load();
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#ifdef __linux__
    output_tree["evdiInfo"] = evdi_status_json();
    output_tree["evdiDiagnostic"] = output_tree["evdiInfo"]["diagnostic"];
#endif
    send_response(response, output_tree);
  }

  /** Return the current EVDI runtime and DKMS status for the Audio/Video UI. */
  void getEvdiStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
#ifdef __linux__
    output_tree["status"] = true;
    output_tree["evdiInfo"] = evdi_status_json();
#else
    output_tree["status"] = false;
    output_tree["error"] = "EVDI is only available on Linux.";
#endif
    send_response(response, output_tree);
  }

  void getClipboardStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
#ifdef __linux__
    output_tree["status"] = true;
    output_tree["clipboardInfo"] = clipboard_status_json();
    output_tree["installStatus"] = clipboard_install_status.load();
#else
    output_tree["status"] = true;
    output_tree["clipboardInfo"] = {{"available", platf::clipboard_available()}, {"diagnostic", platf::clipboard_available() ? "ready" : "clipboard_unavailable"}, {"manualInstall", "Use the native clipboard service for this platform."}};
    output_tree["installStatus"] = 0;
#endif
    send_response(response, output_tree);
  }

  void installClipboard(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
#ifdef __linux__
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (!nlohmann::json::parse(ss).value("confirm", false)) {
        bad_request(response, request, "Clipboard installation requires explicit confirmation");
        return;
      }
      if (clipboard_install_status.load() == 1) {
        output_tree["status"] = true;
        output_tree["installStatus"] = "running";
        send_response(response, output_tree);
        return;
      }

      // Fixed package-manager commands only. No request value reaches the shell.
      constexpr auto install_command = R"CLIP(pkexec sh -c 'set -eu
if command -v pacman >/dev/null 2>&1; then
  pacman -S --needed --noconfirm wl-clipboard xclip
elif command -v apt-get >/dev/null 2>&1; then
  apt-get update
  apt-get install -y wl-clipboard xclip
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y wl-clipboard xclip
else
  echo "Unsupported package manager. Install wl-clipboard and xclip manually." >&2
  exit 2
fi')CLIP";
      clipboard_install_status.store(1);
      std::thread([install_command] {
        auto working_dir = boost::filesystem::path(std::getenv("HOME") ?: "/tmp");
        auto env = boost::this_process::environment();
        std::error_code ec;
        auto child = platf::run_command(false, true, install_command, working_dir, env, nullptr, ec, nullptr);
        if (ec) {
          BOOST_LOG(warning) << "Unable to launch clipboard installer: " << ec.message();
          clipboard_install_status.store(3);
          return;
        }
        child.wait();
        clipboard_install_status.store(child.exit_code() == 0 ? 2 : 3);
      }).detach();
      output_tree["status"] = true;
      output_tree["installStatus"] = "running";
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "InstallClipboard: " << e.what();
      bad_request(response, request, e.what());
      return;
    }
#else
    output_tree["status"] = false;
    output_tree["error"] = "Automatic clipboard dependency installation is only available on Linux.";
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    std::stringstream ss;

    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");
      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));
        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    std::string content = file_handler::read_file(config::sunshine.log_file.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    if ((!config::sunshine.username.empty() && !authenticate(response, request)) || !validateContentType(response, request, "application/json"))
      return;
    print_req(request);
    std::vector<std::string> errors;
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty())
        newUsername = username;
      if (newUsername.empty()) {
        errors.push_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() ||
            (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            sessionCookie.clear(); // force re-login
            output_tree["status"] = true;
          }
        } else {
          errors.push_back("Invalid Current Credentials");
        }
      }
      if (!errors.empty()) {
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(),
                                              [](const std::string &a, const std::string &b) {
                                                return a.empty() ? b : a + ", " + b;
                                              });
        bad_request(response, request, error);
        return;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get a one-time password (OTP).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/otp| GET| null}
   */
  void getOTP(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      std::string passphrase = input_tree.value("passphrase", "");
      if (passphrase.empty())
        throw std::runtime_error("Passphrase not provided!");
      if (passphrase.size() < 4)
        throw std::runtime_error("Passphrase too short!");

      std::string deviceName = input_tree.value("deviceName", "");
      output_tree["otp"] = nvhttp::request_otp(passphrase, deviceName);
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.sunshine_name;
      output_tree["status"] = true;
      output_tree["message"] = "OTP created, effective within 3 minutes.";
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "OTP creation failed: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a PIN code to the host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string pin = input_tree.value("pin", "");
      std::string name = input_tree.value("name", "");
      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Quit Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * On Windows, if running in a service, a special shutdown code is returned.
   */
  void quit(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    BOOST_LOG(warning) << "Requested quit from config page!"sv;

    proc::proc.terminate();

#ifdef _WIN32
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
    } else
#endif
    {
      lifetime::exit_sunshine(0, true);
    }
    // If exit fails, write a response after 5 seconds.
    std::thread write_resp([response]{
      std::this_thread::sleep_for(5s);
      response->write();
    });
    write_resp.detach();
  }

  /**
   * @brief Launch an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void launchApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      std::string uuid = input_tree["uuid"].get<std::string>();

      nlohmann::json output_tree;
      const auto &apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          crypto::named_cert_t named_cert {
            .name = "",
            .uuid = http::unique_id,
            .perm = crypto::PERM::_all,
          };
          BOOST_LOG(info) << "Launching app ["sv << app.name << "] from web UI"sv;
          auto launch_session = nvhttp::make_launch_session(true, false, request->parse_query_string(), &named_cert);
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            bad_request(response, request, err == 503 ?
                        "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
                        "Failed to start the specified application");
          } else {
            output_tree["status"] = true;
            send_response(response, output_tree);
          }
          return;
        }
      }
      BOOST_LOG(error) << "Couldn't find app with uuid ["sv << uuid << ']';
      bad_request(response, request, "Cannot find requested application");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "LaunchApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void disconnect(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::find_and_stop_session(uuid, true);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Disconnect: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Login the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "username": "<username>",
   *   "password": "<password>"
   * }
   * @endcode
   */
  void login(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    auto fg = util::fail_guard([&]{
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      std::string hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
      if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password)
        return;
      std::string sessionCookieRaw = crypto::rand_alphabet(64);
      sessionCookie = util::hex(crypto::hash(sessionCookieRaw + config::sunshine.salt)).to_string();
      cookie_creation_time = std::chrono::steady_clock::now();
      const SimpleWeb::CaseInsensitiveMultimap headers {
        { "Set-Cookie", "auth=" + sessionCookieRaw + "; Secure; SameSite=Strict; Max-Age=2592000; Path=/" }
      };
      response->write(headers);
      fg.disable();
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << net::addr_to_normalized_string(request->remote_endpoint().address())
                               << "]: "sv << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      fg.disable();
      return;
    }
  }

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.authenticate_client = [](const req_https_t &request, SSL *ssl) {
      crypto::x509_t certificate {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!certificate) {
        return;
      }

      crypto::p_named_cert_t client;
      if (nvhttp::verify_paired_client_certificate(certificate.get(), client)) {
        request->userp = std::move(client);
      } else {
        BOOST_LOG(info) << "[HestiaAPI] unpaired client certificate rejected";
      }
    };
    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["GET"] = not_found;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/login/?$"]["GET"] = getLoginPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/otp$"]["POST"] = getOTP;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/reorder$"]["POST"] = reorderApps;
    server.resource["^/api/apps/delete$"]["POST"] = deleteApp;
    server.resource["^/api/apps/launch$"]["POST"] = launchApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/metrics$"]["GET"] = getMetrics;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/evdi/install$"]["POST"] = installEvdi;
    server.resource["^/api/evdi/install/status$"]["GET"] = getEvdiInstallStatus;
    server.resource["^/api/evdi/status$"]["GET"] = getEvdiStatus;
    server.resource["^/api/clipboard/status$"]["GET"] = getClipboardStatus;
    server.resource["^/api/clipboard/install$"]["POST"] = installClipboard;
    server.resource["^/api/hestia/v1/?$"]["GET"] = getHestiaCapabilities;
    server.resource["^/api/hestia/v1/capabilities$"]["GET"] = getHestiaCapabilities;
    server.resource["^/api/hestia/v1/session/prepare$"]["POST"] = prepare_hestia_session;
    server.resource["^/api/hestia/v1/session/stop$"]["POST"] = stop_hestia_session;
    server.resource["^/api/hestia/v1/display/status$"]["GET"] = get_hestia_display_status;
    server.resource["^/api/hestia/v1/display/recover$"]["POST"] = recover_hestia_display;
    server.resource["^/api/hestia/v1/client/permissions$"]["GET"] = get_hestia_client_permissions;
    server.resource["^/api/hestia/v1/diagnostics$"]["GET"] = get_hestia_diagnostics;
    server.resource["^/api/hestia/v1/clipboard$"]["GET"] = get_hestia_clipboard;
    server.resource["^/api/hestia/v1/clipboard$"]["POST"] = set_hestia_clipboard;
    server.resource["^/api/hestia/v1/commands$"]["GET"] = get_hestia_commands;
    server.resource["^/api/hestia/v1/commands/run$"]["POST"] = run_hestia_command;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/quit$"]["POST"] = quit;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/disconnect$"]["POST"] = disconnect;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/images/apollo.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-apollo-45.png$"]["GET"] = getApolloLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([port_https](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
