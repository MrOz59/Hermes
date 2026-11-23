/**
 * @file tests/unit/platform/test_game_library.cpp
 * @brief Reading the games installed in Steam and Lutris.
 */
#include "../../tests_common.h"

#include <filesystem>
#include <fstream>
#include <src/platform/linux/game_library.h>
#include <string>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace platf::game_library;

  class SteamDir: public ::testing::Test {
  protected:
    fs::path root = fs::temp_directory_path() / ("hermes-game-library-" + std::to_string(::getpid()));
    fs::path steam = root / "Steam";
    fs::path second = root / "Games" / "SteamLibrary";

    void SetUp() override {
      fs::remove_all(root);
      fs::create_directories(steam / "steamapps");
      fs::create_directories(second / "steamapps");
    }

    void TearDown() override {
      fs::remove_all(root);
    }

    static void write(const fs::path &file, const std::string &text) {
      std::ofstream {file} << text;
    }

    static void manifest(const fs::path &library, const std::string &appid, const std::string &name, int flags) {
      write(library / "steamapps" / ("appmanifest_" + appid + ".acf"), "\"AppState\"\n{\n\t\"appid\"\t\t\"" + appid + "\"\n\t\"Universe\"\t\t\"1\"\n\t\"name\"\t\t\"" + name + "\"\n\t\"StateFlags\"\t\t\"" + std::to_string(flags) + "\"\n\t\"installdir\"\t\t\"x\"\n\t\"InstalledDepots\"\n\t{\n\t\t\"1\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"2\"\n\t\t}\n\t}\n}\n");
    }

    void libraryfolders() {
      write(steam / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam.string() + "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"2358720\"\t\t\"1\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + second.string() + "\"\n\t}\n}\n");
    }
  };
}  // namespace

TEST(Vdf, KeysAreCaseInsensitiveAndBlocksNest) {
  const auto vdf = parse_vdf("// comment\n\"AppState\" { \"AppID\" \"10\" \"Name\" \"Say \\\"hi\\\"\" \"Depots\" { \"1\" { \"size\" \"5\" } } }");
  const auto *state = vdf.block("appstate");
  ASSERT_TRUE(state);
  EXPECT_EQ(*state->value("appid"), "10");
  EXPECT_EQ(*state->value("name"), "Say \"hi\"");
  EXPECT_EQ(*state->block("depots")->block("1")->value("size"), "5");
}

TEST(Vdf, TruncatedTextEndsQuietly) {
  const auto vdf = parse_vdf("\"AppState\" { \"appid\" \"10\" \"name\"");
  ASSERT_TRUE(vdf.block("appstate"));
  EXPECT_EQ(*vdf.block("appstate")->value("appid"), "10");
  EXPECT_FALSE(vdf.block("appstate")->value("name"));
}

TEST_F(SteamDir, EveryLibraryOnceWithTheSteamDirectoryFirst) {
  libraryfolders();
  const auto libraries = steam_libraries(steam);
  ASSERT_EQ(libraries.size(), 2u);
  EXPECT_EQ(libraries[0], steam);
  EXPECT_EQ(libraries[1], second);
}

TEST_F(SteamDir, TheOldLibraryFoldersFormatIsRead) {
  write(steam / "steamapps" / "libraryfolders.vdf", "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1600000000\"\n\t\"ContentStatsID\"\t\t\"-1\"\n\t\"1\"\t\t\"" + second.string() + "\"\n}\n");
  const auto libraries = steam_libraries(steam);
  ASSERT_EQ(libraries.size(), 2u);
  EXPECT_EQ(libraries[1], second);
}

TEST_F(SteamDir, InstalledGamesFromEveryLibraryButNotToolsOrDownloads) {
  libraryfolders();
  manifest(steam, "2358720", "Black Myth: Wukong", 4);
  manifest(second, "2074920", "The First Descendant", 4);
  manifest(second, "1245620", "ELDEN RING", 1030);  // an update queued, still installed
  manifest(steam, "570", "Dota 2", 1026);  // downloading, not installed yet
  manifest(steam, "1628350", "Steam Linux Runtime 3.0 (sniper)", 4);
  manifest(steam, "2805730", "Proton 9.0", 4);
  manifest(steam, "228980", "Steamworks Common Redistributables", 4);

  const auto games = merge(steam_games(steam, "steam"));
  ASSERT_EQ(games.size(), 3u);
  EXPECT_EQ(games[0].name, "Black Myth: Wukong");
  EXPECT_EQ(games[0].cmd, "setsid steam steam://rungameid/2358720");
  EXPECT_EQ(games[1].name, "ELDEN RING");
  EXPECT_EQ(games[2].name, "The First Descendant");
  EXPECT_EQ(games[2].source, "steam");
  EXPECT_EQ(games[2].id, "2074920");
}

TEST_F(SteamDir, AFlatpakSteamLaunchesThroughFlatpak) {
  manifest(steam, "2358720", "Black Myth: Wukong", 4);
  const auto games = steam_games(steam, "flatpak run com.valvesoftware.Steam");
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].cmd, "setsid flatpak run com.valvesoftware.Steam steam://rungameid/2358720");
}

TEST_F(SteamDir, NoSteamNoGames) {
  EXPECT_TRUE(steam_games(root / "missing", "steam").empty());
}

TEST(Lutris, InstalledGamesButNotSteamRunnerOnes) {
  const auto games = lutris_games(
    "2026-11-20 12:00:00,000: Startup info\n"
    "[{\"id\": 3, \"slug\": \"diablo-ii\", \"name\": \"Diablo II\", \"runner\": \"wine\", \"platform\": \"Windows\"},"
    " {\"id\": 5, \"slug\": \"wukong\", \"name\": \"Black Myth: Wukong\", \"runner\": \"steam\"},"
    " {\"id\": \"7\", \"slug\": \"gog-game\", \"name\": \"A GOG Game\", \"runner\": \"linux\"},"
    " {\"id\": 9, \"name\": \"\"}, {\"name\": \"no id\"}]",
    "lutris"
  );
  ASSERT_EQ(games.size(), 2u);
  EXPECT_EQ(games[0].name, "Diablo II");
  EXPECT_EQ(games[0].cmd, "setsid lutris lutris:rungameid/3");
  EXPECT_EQ(games[1].id, "7");
}

TEST(Lutris, OutputThatIsNotJsonGivesNoGames) {
  EXPECT_TRUE(lutris_games("", "lutris").empty());
  EXPECT_TRUE(lutris_games("Traceback (most recent call last):\n  ...", "lutris").empty());
  EXPECT_TRUE(lutris_games("[{\"id\": 1, ", "lutris").empty());
}

TEST(Merge, OneEntryPerGameSortedByName) {
  const auto games = merge({
    {"steam", "2", "beta", "b"},
    {"steam", "1", "Alpha", "a"},
    {"steam", "2", "beta (other root)", "b2"},
    {"lutris", "2", "Gamma", "g"},
  });
  ASSERT_EQ(games.size(), 3u);
  EXPECT_EQ(games[0].name, "Alpha");
  EXPECT_EQ(games[1].cmd, "b");
  EXPECT_EQ(games[2].source, "lutris");
}
