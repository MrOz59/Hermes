/**
 * @file tools/hermes-gamemode.cpp
 * @brief The console Hermes shows when there is no keyboard and no desktop.
 *
 * SteamOS and CachyOS Game Mode are a compositor, a controller and Steam. There
 * is no desktop to open a browser on, no keyboard to type a password into, and
 * leaving for Desktop Mode is exactly the thing an appliance is supposed to
 * never ask. Everything Hermes needs from its owner in that state is small -
 * type four digits to pair a device, restart the service, see whether it is up -
 * and none of it justifies a keyboard.
 *
 * So this is a separate program, added to Steam as a non-Steam shortcut and
 * launched from the library like a game. It is deliberately not part of the
 * streaming host: the host has no business drawing windows, and a console that
 * crashes must not take a stream down with it.
 *
 * It is driven with the D-pad and A. Every control is a button in a grid, focus
 * moves with the arrow keys, and Enter or Space presses. That covers both ways
 * Steam Input presents a controller to a non-Steam app - as a mouse, where the
 * buttons are large enough to hit with a thumbstick, and as a keyboard, where
 * the arrows land directly. Nothing here reads a gamepad device itself, which
 * is why it needs no permissions and works the same when a keyboard is plugged
 * in after all.
 */

// standard includes
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <QApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

namespace {

  constexpr int DEFAULT_BASE_PORT = 47989;
  constexpr int WEB_UI_PORT_OFFSET = 1;
  constexpr int POLL_INTERVAL_MS = 2000;

  /**
   * @brief Where Hermes publishes the token a local process may authenticate with.
   *
   * The runtime directory is tmpfs and mode 0700, so the file is readable by
   * this user and gone at reboot. Its absence is the normal way to discover
   * that Hermes is not running, which is why a missing token is reported as
   * "not running" rather than as a permission problem.
   */
  std::optional<std::string> read_local_api_token() {
    const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir) {
      return std::nullopt;
    }
    std::ifstream in {std::string {runtime_dir} + "/hermes/local-api.token"};
    if (!in) {
      return std::nullopt;
    }
    std::string token;
    std::getline(in, token);
    if (token.empty()) {
      return std::nullopt;
    }
    return token;
  }

  /**
   * @brief The user unit that runs Hermes on this machine.
   *
   * The CMake build installs sunshine.service; the Arch packaging renames it
   * to hermes.service so it can sit beside an apollo/sunshine install. A
   * restart has to name the unit that is actually there, so the usual unit
   * directories are checked and the first candidate found wins.
   */
  std::string hermes_unit_name() {
    std::vector<std::filesystem::path> directories;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
      directories.emplace_back(std::string {xdg} + "/systemd/user");
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
      directories.emplace_back(std::string {home} + "/.config/systemd/user");
      directories.emplace_back(std::string {home} + "/.local/share/systemd/user");
    }
    directories.emplace_back("/etc/systemd/user");
    directories.emplace_back("/usr/local/lib/systemd/user");
    directories.emplace_back("/usr/lib/systemd/user");

    for (const char *candidate : {"hermes", "sunshine"}) {
      for (const auto &directory : directories) {
        std::error_code error;
        if (std::filesystem::exists(directory / (std::string {candidate} + ".service"), error)) {
          return candidate;
        }
      }
    }
    // Neither unit is visible (a container, a distribution that moved it);
    // the canonical name is still the best guess.
    return "sunshine";
  }

  /**
   * @brief The port the web UI is on, which is the configured port plus one.
   *
   * Read out of the config file rather than assumed, because somebody who moved
   * Hermes off 47989 to run two hosts on one machine would otherwise be handed
   * a console talking to the wrong one. A file that is missing or says nothing
   * about the port means the default is in force.
   */
  int web_ui_port() {
    std::string config_path;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
      config_path = std::string {xdg} + "/hermes/hermes.conf";
    } else if (const char *home = std::getenv("HOME"); home && *home) {
      config_path = std::string {home} + "/.config/hermes/hermes.conf";
    } else {
      return DEFAULT_BASE_PORT + WEB_UI_PORT_OFFSET;
    }

    std::ifstream in {config_path};
    for (std::string line; std::getline(in, line);) {
      const auto equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r");
        const auto last = value.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string {} : value.substr(first, last - first + 1);
      };
      if (trim(line.substr(0, equals)) != "port") {
        continue;
      }
      try {
        return std::stoi(trim(line.substr(equals + 1))) + WEB_UI_PORT_OFFSET;
      } catch (const std::exception &) {
        break;
      }
    }
    return DEFAULT_BASE_PORT + WEB_UI_PORT_OFFSET;
  }

  size_t collect_body(char *data, size_t size, size_t count, void *userp) {
    static_cast<std::string *>(userp)->append(data, size * count);
    return size * count;
  }

  /**
   * @brief One request to the local Hermes, authenticated with a token.
   *
   * The token is an argument, not a member: Hermes publishes a new one every
   * time it starts, so a token held for the console's whole life goes stale
   * the moment anybody presses "Restart Hermes". Taking it per request lets
   * the caller re-read the file when it needs to.
   *
   * TLS verification is off because the only peer is this machine's own Hermes
   * behind a certificate it signed itself, and the token - not the certificate -
   * is what establishes who is asking. Anything positioned to intercept
   * loopback is already this user or root, and both can simply read the token
   * file. Verification here would buy nothing and would break every host that
   * has not been given a real certificate, which is all of them.
   */
  struct ApiResult {
    bool ok {false};
    long status {0};
    nlohmann::json body;
    std::string error;
  };

  class Api {
  public:
    explicit Api(int port):
        base_ {"https://localhost:" + std::to_string(port)} {
    }

    ApiResult get(const std::string &path, const std::string &token) const {
      return request(path, std::nullopt, token);
    }

    ApiResult post(const std::string &path, const nlohmann::json &payload, const std::string &token) const {
      return request(path, payload.dump(), token);
    }

  private:
    ApiResult request(const std::string &path, const std::optional<std::string> &payload,
                      const std::string &token) const {
      ApiResult result;

      CURL *curl = curl_easy_init();
      if (!curl) {
        result.error = "Could not initialise HTTP client";
        return result;
      }

      std::string response;
      const std::string url = base_ + path;
      const std::string authorization = "Authorization: Bearer " + token;

      curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, authorization.c_str());
      if (payload) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
      }

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
      if (payload) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
      }

      const CURLcode code = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (code != CURLE_OK) {
        result.error = curl_easy_strerror(code);
        return result;
      }

      result.body = nlohmann::json::parse(response, nullptr, false);
      if (result.body.is_discarded()) {
        result.body = nlohmann::json::object();
      }
      result.ok = result.status >= 200 && result.status < 300;
      if (!result.ok && result.error.empty()) {
        result.error = "Hermes answered " + std::to_string(result.status);
      }
      return result;
    }

    std::string base_;
  };

  // ---------------------------------------------------------------------------
  // Presentation
  // ---------------------------------------------------------------------------

  /**
   * @brief A television is further away than a monitor, so everything is large.
   *
   * Ten-foot sizing is not a style choice here: the same panel that is
   * comfortable at desk distance is unreadable from a sofa, and a target small
   * enough to need precision is one a thumbstick cannot hit.
   */
  constexpr auto STYLE_SHEET = R"(
    QWidget {
      background: #14181d;
      color: #e6e1cf;
      font-family: "Noto Sans", "DejaVu Sans", sans-serif;
      font-size: 20px;
    }
    QLabel#title { font-size: 40px; font-weight: 600; }
    QLabel#subtitle { color: #8c9aa8; font-size: 18px; }
    QLabel#status { font-size: 24px; }
    QLabel#digits { font-size: 64px; font-weight: 600; letter-spacing: 18px; }
    QPushButton {
      background: #1f272f;
      border: 2px solid #1f272f;
      border-radius: 10px;
      padding: 18px 24px;
      text-align: left;
      font-size: 24px;
    }
    QPushButton#key { text-align: center; font-size: 32px; padding: 12px; }
    QPushButton:focus {
      background: #2b6cb0;
      border: 2px solid #63b3ed;
      outline: none;
    }
    QPushButton#attention { border: 2px solid #d69e2e; }
    QFrame#card { background: #1a1f26; border-radius: 14px; }
  )";

  QPushButton *make_button(const QString &text, const QString &object_name = {}) {
    auto *button = new QPushButton(text);
    if (!object_name.isEmpty()) {
      button->setObjectName(object_name);
    }
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
  }

  /**
   * @brief The window, and the only thing that reads a key.
   *
   * Focus movement is handled here rather than left to Qt's own tab order
   * because the arrow keys are what a D-pad produces, and Qt moves focus on Tab.
   * Mapping the four arrows onto next/previous is enough for a layout that is a
   * single column or a single grid, which is all either screen is, and it costs
   * none of the machinery that a general spatial-navigation implementation
   * would.
   */
  class Console: public QWidget {
  public:
    Console(Api api, int port):
        api_ {std::move(api)},
        port_ {port} {
      setStyleSheet(STYLE_SHEET);
      setWindowTitle("Hermes");

      stack_ = new QStackedWidget;
      stack_->addWidget(build_home());
      stack_->addWidget(build_pair());

      auto *layout = new QVBoxLayout(this);
      layout->setContentsMargins(48, 40, 48, 40);
      layout->addWidget(stack_);

      poll_ = new QTimer(this);
      // A pairing request arrives while the console is already open and nobody
      // is touching it, so the screen has to notice on its own; that is the
      // whole reason this polls rather than refreshing on keypress.
      connect(poll_, &QTimer::timeout, this, [this] {
        refresh();
      });
      // Results arrive through signals connected to this window, which is
      // also what keeps a late answer from touching a destroyed console: the
      // connection dies with the receiver.
      connect(&status_watcher_, &QFutureWatcher<ApiResult>::finished, this, [this] {
        apply_status(status_watcher_.result());
      });
      connect(&pin_watcher_, &QFutureWatcher<ApiResult>::finished, this, [this] {
        apply_pin_result(pin_watcher_.result());
      });
      poll_->start(POLL_INTERVAL_MS);
      refresh();
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
      switch (event->key()) {
        case Qt::Key_Down:
        case Qt::Key_Right:
          focusNextChild();
          return;
        case Qt::Key_Up:
        case Qt::Key_Left:
          focusPreviousChild();
          return;
        case Qt::Key_Escape:
        case Qt::Key_Backspace:
          // B on a controller. On the first screen there is nowhere further
          // back, so it closes - which is what a player expects from a
          // non-Steam app they are done with.
          if (stack_->currentIndex() == 0) {
            close();
          } else {
            show_home();
          }
          return;
        default:
          break;
      }
      QWidget::keyPressEvent(event);
    }

  private:
    // -- home -----------------------------------------------------------------

    QWidget *build_home() {
      auto *page = new QWidget;
      auto *layout = new QVBoxLayout(page);
      layout->setSpacing(14);

      auto *title = new QLabel("Hermes");
      title->setObjectName("title");
      layout->addWidget(title);

      host_label_ = new QLabel;
      host_label_->setObjectName("subtitle");
      layout->addWidget(host_label_);

      status_label_ = new QLabel;
      status_label_->setObjectName("status");
      layout->addWidget(status_label_);
      layout->addSpacing(18);

      pair_button_ = make_button("Pair a device");
      connect(pair_button_, &QPushButton::clicked, this, [this] {
        show_pair();
      });
      layout->addWidget(pair_button_);

      auto *restart = make_button("Restart Hermes");
      connect(restart, &QPushButton::clicked, this, [this] {
        restart_service();
      });
      layout->addWidget(restart);

      auto *settings = make_button("Open settings in a browser");
      connect(settings, &QPushButton::clicked, this, [this] {
        open_web_ui();
      });
      layout->addWidget(settings);

      auto *quit = make_button("Close");
      connect(quit, &QPushButton::clicked, this, [this] {
        close();
      });
      layout->addWidget(quit);

      layout->addStretch();
      message_label_ = new QLabel;
      message_label_->setObjectName("subtitle");
      message_label_->setWordWrap(true);
      layout->addWidget(message_label_);

      return page;
    }

    // -- pairing --------------------------------------------------------------

    QWidget *build_pair() {
      auto *page = new QWidget;
      auto *layout = new QVBoxLayout(page);
      layout->setSpacing(14);

      pair_title_ = new QLabel("Pair a device");
      pair_title_->setObjectName("title");
      layout->addWidget(pair_title_);

      pair_hint_ = new QLabel;
      pair_hint_->setObjectName("subtitle");
      pair_hint_->setWordWrap(true);
      layout->addWidget(pair_hint_);

      digits_label_ = new QLabel;
      digits_label_->setObjectName("digits");
      digits_label_->setAlignment(Qt::AlignCenter);
      layout->addWidget(digits_label_);

      auto *pad = new QFrame;
      pad->setObjectName("card");
      auto *grid = new QGridLayout(pad);
      grid->setSpacing(10);
      for (int digit = 1; digit <= 9; ++digit) {
        auto *key = make_button(QString::number(digit), "key");
        connect(key, &QPushButton::clicked, this, [this, digit] {
          append_digit(QChar('0' + digit));
        });
        grid->addWidget(key, (digit - 1) / 3, (digit - 1) % 3);
      }

      auto *backspace = make_button(QString::fromUtf8("\u232b"), "key");
      connect(backspace, &QPushButton::clicked, this, [this] {
        if (!entered_.isEmpty()) {
          entered_.chop(1);
          render_digits();
        }
      });
      grid->addWidget(backspace, 3, 0);

      auto *zero = make_button("0", "key");
      connect(zero, &QPushButton::clicked, this, [this] {
        append_digit('0');
      });
      grid->addWidget(zero, 3, 1);

      submit_button_ = make_button("OK", "key");
      connect(submit_button_, &QPushButton::clicked, this, [this] {
        submit_pin();
      });
      grid->addWidget(submit_button_, 3, 2);

      layout->addWidget(pad);

      auto *back = make_button("Back");
      connect(back, &QPushButton::clicked, this, [this] {
        show_home();
      });
      layout->addWidget(back);

      layout->addStretch();
      pair_message_ = new QLabel;
      pair_message_->setObjectName("subtitle");
      pair_message_->setWordWrap(true);
      layout->addWidget(pair_message_);

      return page;
    }

    void append_digit(QChar digit) {
      if (entered_.size() >= 4) {
        return;
      }
      entered_.append(digit);
      render_digits();
      // Four digits is the whole PIN, so there is nothing to confirm: moving
      // focus to OK saves the one press that would otherwise be spent hunting
      // for it.
      if (entered_.size() == 4) {
        submit_button_->setFocus();
      }
    }

    void render_digits() {
      // Filled slots show the digit, empty ones a low line rather than a dot:
      // four dots floating in the middle of a panel do not read as "four
      // places, two of them used" from across a room.
      QString shown;
      for (int index = 0; index < 4; ++index) {
        shown += index < entered_.size() ? entered_.at(index) : QChar('_');
      }
      digits_label_->setText(shown);
    }

    void submit_pin() {
      if (entered_.size() != 4) {
        pair_message_->setText("The PIN is four digits.");
        return;
      }
      if (pin_watcher_.isRunning()) {
        return;
      }
      pair_message_->setText("Checking...");

      const std::string pin = entered_.toStdString();
      const std::string name = pending_name_;
      // Read at submit time rather than held: the pair screen can be open
      // across a restart, and the token from before it is already gone.
      const std::string token = read_local_api_token().value_or(std::string {});
      pin_watcher_.setFuture(QtConcurrent::run([api = api_, pin, name, token]() {
        ApiResult result = api.post("/api/pin", {{"pin", pin}, {"name", name}}, token);
        if (result.status == 401) {
          if (const auto fresh = read_local_api_token()) {
            result = api.post("/api/pin", {{"pin", pin}, {"name", name}}, *fresh);
          }
        }
        return result;
      }));
    }

    void apply_pin_result(ApiResult result) {
      const bool accepted = result.ok && result.body.value("status", false);
      if (accepted) {
        pair_message_->setText("Paired. The device can connect now.");
        entered_.clear();
        render_digits();
        QTimer::singleShot(1500, this, [this] {
          show_home();
        });
      } else {
        // Wrong digits and no pending request are different mistakes and the
        // remedy differs, so they do not share a message.
        pair_message_->setText(
          pending_ ?
            "That PIN was not accepted. Check the digits on the device and try again." :
            "No device is waiting to pair. Start pairing on the device first."
        );
        entered_.clear();
        render_digits();
      }
    }

    // -- actions --------------------------------------------------------------

    void restart_service() {
      message_label_->setText("Restarting...");
      // The service is the user manager's, and so is this console: no
      // privilege is involved and nothing has to be asked of polkit. The unit
      // is named as installed - hermes on Arch, sunshine everywhere else -
      // because a restart of a unit that does not exist is a silent nothing.
      QProcess::startDetached("systemctl", {"--user", "restart", QString::fromStdString(hermes_unit_name())});
      QTimer::singleShot(2500, this, [this] {
        refresh();
      });
    }

    void open_web_ui() {
      const QString url = QString::fromStdString("https://localhost:" + std::to_string(port_));
      // Steam's own browser is the only one guaranteed to exist in Game Mode,
      // and it is already controller-navigable with an on-screen keyboard.
      // Outside Game Mode there is a desktop, and the desktop's default
      // browser is the better answer.
      if (std::getenv("GAMESCOPE_WAYLAND_DISPLAY")) {
        if (QProcess::startDetached("steam", {"steam://openurl/" + url})) {
          message_label_->setText("Opened in Steam's browser.");
          return;
        }
      }
      QDesktopServices::openUrl(QUrl(url));
      message_label_->setText("Opened " + url);
    }

    // -- polling --------------------------------------------------------------

    /**
     * @brief The GUI thread must never block on the network.
     *
     * A stale token or a stopped Hermes turns every request into a
     * seconds-long timeout, and on the GUI thread that is a console that
     * stops drawing. Requests run through QtConcurrent's pool instead; the
     * Api holds nothing but a URL, so a copy is all the worker needs, and the
     * answer comes back through the watcher signals connected in the
     * constructor.
     */
    void refresh() {
      if (status_watcher_.isRunning()) {
        // The previous request is still out and its answer will land shortly;
        // the timer outruns the request, not the other way around.
        return;
      }

      // The token is re-read here rather than held from startup: Hermes
      // publishes a new one every time it starts, and the whole reason this
      // screen has a "Restart Hermes" button is that restarting is normal.
      // Reading it fresh keeps that button from permanently 401-ing the very
      // console that pressed it.
      const std::string token = read_local_api_token().value_or(std::string {});
      status_watcher_.setFuture(QtConcurrent::run([api = api_, token]() {
        ApiResult result = api.get("/api/gamemode/status", token);
        // The restart race in the other direction: Hermes came up again
        // between the read above and this request. One retry with the token
        // as it is now covers it.
        if (result.status == 401) {
          if (const auto fresh = read_local_api_token()) {
            result = api.get("/api/gamemode/status", *fresh);
          }
        }
        return result;
      }));
    }

    void apply_status(ApiResult result) {
      if (!result.ok) {
        status_label_->setText(
          QStringLiteral("<span style=\"color:#e05252\">\u25cf</span> Not reachable")
        );
        host_label_->setText("Hermes is not answering on port " + QString::number(port_));
        message_label_->setText(QString::fromStdString(result.error));
        pending_ = false;
        pair_button_->setObjectName({});
        restyle(pair_button_);
        return;
      }

      const auto &body = result.body;
      host_label_->setText(
        QString::fromStdString(body.value("hostname", std::string {"this host"})) + "  ·  " +
        QString::fromStdString(body.value("version", std::string {"unknown"}))
      );

      const auto paired = body.value("paired_clients", 0);
      const auto streaming = body.value("streaming_sessions", 0);
      // Rich text for the dot alone. Only fixed strings and counts are
      // interpolated here; the hostname and version go into a plain label,
      // where markup in a name cannot reach.
      status_label_->setText(
        QStringLiteral("<span style=\"color:#48bb78\">\u25cf</span> Running  &middot;  ") +
        QString::number(paired) + (paired == 1 ? " device" : " devices") + "  &middot;  " +
        (streaming > 0 ? QString::number(streaming) + " streaming" : QString {"nothing streaming"})
      );

      pending_ = body.value("pending_pair", false);
      pending_name_ = body.value("pending_pair_name", std::string {});
      if (pending_) {
        pair_button_->setText(
          "Pair “" + QString::fromStdString(pending_name_.empty() ? "a device" : pending_name_) +
          "”  ← waiting"
        );
        pair_button_->setObjectName("attention");
        message_label_->setText("A device is waiting for its PIN.");
      } else {
        pair_button_->setText("Pair a device");
        pair_button_->setObjectName({});
        if (message_label_->text() == "A device is waiting for its PIN.") {
          message_label_->clear();
        }
      }
      restyle(pair_button_);

      if (stack_->currentIndex() == 1) {
        pair_hint_->setText(
          pending_ ?
            "Enter the PIN shown on “" +
              QString::fromStdString(pending_name_.empty() ? "the device" : pending_name_) + "”." :
            "Nothing is waiting to pair yet. Start pairing on the device, then enter its PIN here."
        );
      }
    }

    /** Re-apply the sheet, which Qt does not do on its own when a name changes. */
    void restyle(QWidget *widget) {
      widget->style()->unpolish(widget);
      widget->style()->polish(widget);
    }

    void show_home() {
      stack_->setCurrentIndex(0);
      entered_.clear();
      pair_message_->clear();
      pair_button_->setFocus();
    }

    void show_pair() {
      stack_->setCurrentIndex(1);
      entered_.clear();
      render_digits();
      pair_message_->clear();
      refresh();
      // The keypad is where every press on this screen goes, so focus starts
      // on it rather than on Back.
      if (auto *first = stack_->currentWidget()->findChild<QPushButton *>("key")) {
        first->setFocus();
      }
    }

    Api api_;
    int port_ {};

    // One request in flight at a time per watcher: the poll timer outruns a
    // slow request, and a second one would only pile timeouts on top of it.
    QFutureWatcher<ApiResult> status_watcher_;
    QFutureWatcher<ApiResult> pin_watcher_;

    QTimer *poll_ {};
    QStackedWidget *stack_ {};
    QLabel *host_label_ {};
    QLabel *status_label_ {};
    QLabel *message_label_ {};
    QPushButton *pair_button_ {};

    QLabel *pair_title_ {};
    QLabel *pair_hint_ {};
    QLabel *digits_label_ {};
    QLabel *pair_message_ {};
    QPushButton *submit_button_ {};

    QString entered_;
    bool pending_ {false};
    std::string pending_name_;
  };

  /** The screen shown when there is no Hermes to talk to at all. */
  class NotRunning: public QWidget {
  public:
    NotRunning() {
      setStyleSheet(STYLE_SHEET);
      setWindowTitle("Hermes");

      auto *layout = new QVBoxLayout(this);
      layout->setContentsMargins(48, 40, 48, 40);
      layout->setSpacing(14);

      auto *title = new QLabel("Hermes is not running");
      title->setObjectName("title");
      layout->addWidget(title);

      auto *hint = new QLabel(
        "No local API token was found, which is how this console finds a "
        "running Hermes. Starting the service publishes one."
      );
      hint->setObjectName("subtitle");
      hint->setWordWrap(true);
      layout->addWidget(hint);
      layout->addSpacing(18);

      auto *start = make_button("Start Hermes");
      connect(start, &QPushButton::clicked, this, [this] {
        // The unit is named as installed - hermes on Arch, sunshine elsewhere
        // - because a restart of a unit that does not exist is a silent
        // nothing.
        QProcess::startDetached("systemctl", {"--user", "restart", QString::fromStdString(hermes_unit_name())});
        // Relaunching is the honest way back: the token has to be read at
        // startup, and re-reading it here would mean rebuilding the whole
        // console around a token that may still not exist.
        QTimer::singleShot(3000, this, [this] {
          close();
        });
      });
      layout->addWidget(start);

      auto *quit = make_button("Close");
      connect(quit, &QPushButton::clicked, this, [this] {
        close();
      });
      layout->addWidget(quit);
      layout->addStretch();

      start->setFocus();
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
      switch (event->key()) {
        case Qt::Key_Down:
        case Qt::Key_Right:
          focusNextChild();
          return;
        case Qt::Key_Up:
        case Qt::Key_Left:
          focusPreviousChild();
          return;
        case Qt::Key_Escape:
        case Qt::Key_Backspace:
          close();
          return;
        default:
          break;
      }
      QWidget::keyPressEvent(event);
    }
  };

}  // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  curl_global_init(CURL_GLOBAL_DEFAULT);

  const auto token = read_local_api_token();
  QWidget *window = nullptr;
  if (token) {
    const int port = web_ui_port();
    window = new Console(Api {port}, port);
  } else {
    window = new NotRunning;
  }

  // Full screen by default, because gamescope gives a non-Steam app the whole
  // output and a window decorated for a desktop that is not there only wastes
  // it. `--windowed` is for the other case: running the same console on a
  // desktop, where a window that cannot be moved or dismissed with a mouse is
  // the wrong shape for helping somebody troubleshoot.
  const QStringList arguments = QApplication::arguments();
  if (arguments.contains("--windowed")) {
    window->resize(900, 720);
    window->show();
  } else {
    window->showFullScreen();
  }

  const int code = app.exec();
  // A request may still be in flight when the window goes; wait it out before
  // tearing libcurl down underneath it.
  QThreadPool::globalInstance()->waitForDone();
  curl_global_cleanup();
  return code;
}
