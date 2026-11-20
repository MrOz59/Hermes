/**
 * @file src/platform/linux/game_library.h
 * @brief The games installed in Steam and Lutris, for adding them as apps.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace platf::game_library {

  struct game_t {
    /// "steam" or "lutris".
    std::string source;
    /// Steam's app ID, or Lutris' game ID.
    std::string id;
    std::string name;
    /// Starts the game; an app runs it detached.
    std::string cmd;
  };

  /**
   * @brief A Valve KeyValues (VDF/ACF) block: quoted keys with a quoted value
   * or a nested block. Keys are case-insensitive, so they are kept lowercase.
   */
  struct vdf_t {
    std::map<std::string, std::string> values;
    std::map<std::string, vdf_t> blocks;

    const std::string *value(const std::string &key) const {
      const auto it = values.find(key);
      return it == values.end() ? nullptr : &it->second;
    }

    const vdf_t *block(const std::string &key) const {
      const auto it = blocks.find(key);
      return it == blocks.end() ? nullptr : &it->second;
    }
  };

  namespace detail {
    inline std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return text;
    }

    class vdf_parser_t {
    public:
      explicit vdf_parser_t(std::string_view text):
          text {text} {
      }

      vdf_t parse() {
        vdf_t root;
        block(root, 0);
        return root;
      }

    private:
      std::string_view text;
      std::size_t at = 0;

      enum class token_e {
        string,
        open,
        close,
        end
      };

      token_e next(std::string &out) {
        for (;;) {
          while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) {
            ++at;
          }
          if (at + 1 < text.size() && text[at] == '/' && text[at + 1] == '/') {
            while (at < text.size() && text[at] != '\n') {
              ++at;
            }
            continue;
          }
          break;
        }
        if (at >= text.size()) {
          return token_e::end;
        }
        const char c = text[at];
        if (c == '{' || c == '}') {
          ++at;
          return c == '{' ? token_e::open : token_e::close;
        }
        out.clear();
        if (c == '"') {
          for (++at; at < text.size() && text[at] != '"'; ++at) {
            if (text[at] == '\\' && at + 1 < text.size()) {
              const char escaped = text[++at];
              out += escaped == 'n' ? '\n' : escaped == 't' ? '\t' :
                                                              escaped;
            } else {
              out += text[at];
            }
          }
          ++at;
          return token_e::string;
        }
        // Unquoted tokens are allowed too; they end at whitespace or a brace.
        while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && text[at] != '{' && text[at] != '}') {
          out += text[at++];
        }
        return token_e::string;
      }

      void block(vdf_t &into, int depth) {
        std::string key;
        std::string value;
        for (;;) {
          const auto token = next(key);
          if (token != token_e::string) {
            // A close ends this block; the end of the text ends them all.
            return;
          }
          const auto after = next(value);
          if (after == token_e::open) {
            // Bounded, so a malformed file cannot exhaust the stack.
            if (depth < 32) {
              block(into.blocks[lower(key)], depth + 1);
            }
          } else if (after == token_e::string) {
            into.values.emplace(lower(key), value);
          } else {
            return;
          }
        }
      }
    };

    inline std::string read_file(const std::filesystem::path &file) {
      std::ifstream stream {file, std::ios::binary};
      std::ostringstream text;
      text << stream.rdbuf();
      return text.str();
    }

    /// Runtimes, Proton and redistributables are installed like games but
    /// are not games.
    inline bool is_tool(std::string_view appid, std::string_view name) {
      static const std::set<std::string_view> tools {
        "228980",  // Steamworks Common Redistributables
        "1070560",  // Steam Linux Runtime 1.0 (scout)
        "1391110",  // Steam Linux Runtime 2.0 (soldier)
        "1628350",  // Steam Linux Runtime 3.0 (sniper)
      };
      return tools.contains(appid) || name.starts_with("Proton ") || name == "Proton Experimental" ||
             name.starts_with("Proton Hotfix") || name.starts_with("Steam Linux Runtime") ||
             name.starts_with("Steamworks Common");
    }
  }  // namespace detail

  inline vdf_t parse_vdf(std::string_view text) {
    return detail::vdf_parser_t {text}.parse();
  }

  /**
   * @brief Every Steam library of a Steam installation, itself first.
   * @param root The Steam directory, the one that holds `steamapps`.
   */
  inline std::vector<std::filesystem::path> steam_libraries(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> libraries {root};
    const auto file = parse_vdf(detail::read_file(root / "steamapps" / "libraryfolders.vdf"));
    if (const auto *folders = file.block("libraryfolders")) {
      for (const auto &[key, folder] : folders->blocks) {
        if (const auto *path = folder.value("path")) {
          libraries.emplace_back(*path);
        }
      }
      // Before 2021 a library was a numbered value rather than a block.
      for (const auto &[key, path] : folders->values) {
        if (!key.empty() && std::ranges::all_of(key, [](unsigned char c) {
              return std::isdigit(c);
            })) {
          libraries.emplace_back(path);
        }
      }
    }

    std::vector<std::filesystem::path> unique;
    std::set<std::filesystem::path> seen;
    for (const auto &library : libraries) {
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(library, ec);
      if (ec) {
        canonical = library;
      }
      if (seen.insert(canonical).second) {
        unique.push_back(library);
      }
    }
    return unique;
  }

  /**
   * @brief The games fully installed in a Steam installation's libraries.
   * @param root The Steam directory.
   * @param launcher The command that runs this Steam, such as `steam` or
   *                 `flatpak run com.valvesoftware.Steam`.
   */
  inline std::vector<game_t> steam_games(const std::filesystem::path &root, const std::string &launcher) {
    std::vector<game_t> games;
    for (const auto &library : steam_libraries(root)) {
      std::error_code ec;
      for (const auto &entry : std::filesystem::directory_iterator {library / "steamapps", ec}) {
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with("appmanifest_") || !filename.ends_with(".acf")) {
          continue;
        }
        const auto manifest = parse_vdf(detail::read_file(entry.path()));
        const auto *state = manifest.block("appstate");
        if (!state) {
          continue;
        }
        const auto *appid = state->value("appid");
        const auto *name = state->value("name");
        const auto *flags = state->value("stateflags");
        if (!appid || appid->empty() || !name || name->empty() || detail::is_tool(*appid, *name)) {
          continue;
        }
        // StateFlags bit 4 is "fully installed"; a game mid-download or
        // only partly uninstalled does not have it.
        try {
          if (!flags || !(std::stoul(*flags) & 4)) {
            continue;
          }
        } catch (...) {
          continue;
        }
        games.push_back({"steam", *appid, *name, "setsid " + launcher + " steam://rungameid/" + *appid});
      }
    }
    return games;
  }

  /**
   * @brief Lutris' installed games, from `lutris --list-games --installed --json`.
   *
   * Games of Lutris' Steam runner are left out: Steam's own list has them.
   *
   * @param output What the command printed. Anything before the JSON is
   *               ignored, as Lutris can print log lines first.
   * @param launcher The command that runs this Lutris.
   */
  inline std::vector<game_t> lutris_games(std::string_view output, const std::string &launcher) {
    std::vector<game_t> games;
    const auto start = output.find('[');
    if (start == std::string_view::npos) {
      return games;
    }
    const auto list = nlohmann::json::parse(output.substr(start), nullptr, false);
    if (!list.is_array()) {
      return games;
    }
    for (const auto &game : list) {
      if (!game.is_object() || !game.contains("id") || !game.contains("name") || !game["name"].is_string()) {
        continue;
      }
      if (game.value("runner", nlohmann::json {}).is_string() && game["runner"] == "steam") {
        continue;
      }
      const auto &id = game["id"];
      std::string game_id;
      if (id.is_number_integer()) {
        game_id = std::to_string(id.get<long long>());
      } else if (id.is_string()) {
        game_id = id.get<std::string>();
      }
      const auto name = game["name"].get<std::string>();
      if (game_id.empty() || name.empty() || !std::ranges::all_of(game_id, [](unsigned char c) {
            return std::isdigit(c);
          })) {
        continue;
      }
      games.push_back({"lutris", game_id, name, "setsid " + launcher + " lutris:rungameid/" + game_id});
    }
    return games;
  }

  /// A game per source and ID, the first seen kept, sorted by name.
  inline std::vector<game_t> merge(std::vector<game_t> games) {
    std::vector<game_t> unique;
    std::set<std::pair<std::string, std::string>> seen;
    for (auto &game : games) {
      if (seen.emplace(game.source, game.id).second) {
        unique.push_back(std::move(game));
      }
    }
    std::ranges::stable_sort(unique, [](const game_t &a, const game_t &b) {
      return detail::lower(a.name) < detail::lower(b.name);
    });
    return unique;
  }

}  // namespace platf::game_library
