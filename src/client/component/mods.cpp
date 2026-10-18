#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"

#include "command.hpp"
#include "console.hpp"
#include "mods.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace mods
{
	namespace
	{
		utils::hook::detour sys_create_file_hook;
		utils::hook::detour sp_test_sys_create_file_hook;

		struct sp_test_zone_request
		{
			std::string filename;
			std::string path;
		};

		std::mutex sp_test_zone_request_mutex;
		std::optional<sp_test_zone_request> active_sp_test_zone_request;

		class scoped_sp_test_zone_request
		{
		public:
			explicit scoped_sp_test_zone_request(sp_test_zone_request request)
			{
				std::lock_guard lock(sp_test_zone_request_mutex);
				if (active_sp_test_zone_request.has_value())
				{
					return;
				}

				active_sp_test_zone_request = std::move(request);
				this->active_ = true;
			}

			~scoped_sp_test_zone_request()
			{
				if (!this->active_)
				{
					return;
				}

				std::lock_guard lock(sp_test_zone_request_mutex);
				active_sp_test_zone_request.reset();
			}

			scoped_sp_test_zone_request(const scoped_sp_test_zone_request&) = delete;
			scoped_sp_test_zone_request& operator=(const scoped_sp_test_zone_request&) = delete;

			bool active() const
			{
				return this->active_;
			}

		private:
			bool active_{};
		};

		std::optional<sp_test_zone_request> get_active_sp_test_zone_request(const char* base_filename)
		{
			if (base_filename == nullptr)
			{
				return std::nullopt;
			}

			std::lock_guard lock(sp_test_zone_request_mutex);
			if (!active_sp_test_zone_request.has_value() ||
				active_sp_test_zone_request->filename != base_filename)
			{
				return std::nullopt;
			}

			return active_sp_test_zone_request;
		}

		void db_build_os_path_from_source(const char* zone_name, const game::FF_DIR source, const int size, char* filename)
		{
			char user_map[MAX_PATH]{};

			switch (source)
			{
			case game::FFD_DEFAULT:
				(void)game::Com_sprintf(filename, size, "%s\\%s.ff", std::filesystem::current_path().string().c_str(), zone_name);
				break;
			case game::FFD_MOD_DIR:
				assert(mods::is_using_mods());

				(void)game::Com_sprintf(filename, size, "%s\\%s\\%s.ff", std::filesystem::current_path().string().c_str(), (*dvars::fs_gameDirVar)->current.string, zone_name);
				break;
			case game::FFD_USER_MAP:
				game::I_strncpyz(user_map, zone_name, sizeof(user_map));

				(void)game::Com_sprintf(filename, size, "%s\\%s\\%s\\%s.ff", std::filesystem::current_path().string().c_str(), "usermaps", user_map, zone_name);
				break;
			default:
				assert(false && "inconceivable");
				break;
			}
		}

		bool fs_game_dir_domain_func(game::dvar_t* dvar, game::DvarValue new_value)
		{
			if (*new_value.string == '\0')
			{
				return true;
			}

			if (game::I_strnicmp(new_value.string, "mods", 4) != 0)
			{
				game::LiveStorage_StatsWriteNotNeeded(game::CONTROLLER_INDEX_0);
				console::error("ERROR: Invalid server value '%s' for '%s'\n", new_value.string, dvar->name);
				return false;
			}

			if (5 < std::strlen(new_value.string) && (new_value.string[4] == '\\' || new_value.string[4] == '/'))
			{
				const auto* s1 = std::strstr(new_value.string, "..");
				const auto* s2 = std::strstr(new_value.string, "::");
				if (s1 == nullptr && s2 == nullptr)
				{
					return true;
				}

				game::LiveStorage_StatsWriteNotNeeded(game::CONTROLLER_INDEX_0);
				console::error("ERROR: Invalid server value '%s' for '%s'\n", new_value.string, dvar->name);
				return false;
			}

			// Invalid path specified
			game::LiveStorage_StatsWriteNotNeeded(game::CONTROLLER_INDEX_0);
			console::error("ERROR: Invalid server value '%s' for '%s'\n", new_value.string, dvar->name);
			return false;
		}

		const auto skip_extra_zones_stub = utils::hook::assemble([](utils::hook::assembler& a) -> void
		{
			const auto skip = a.newLabel();
			const auto original = a.newLabel();

			a.pushad64();
			a.test(esi, game::DB_ZONE_CUSTOM); // allocFlags
			a.jnz(skip);

			a.bind(original);
			a.popad64();
			a.mov(rdx, 0x140809D40);
			a.mov(rcx, rbp);
			a.call(0x1406FE120);
			a.jmp(0x140271B63);

			a.bind(skip);
			a.popad64();
			a.mov(r13d, game::DB_ZONE_CUSTOM);
			a.not_(r13d);
			a.and_(esi, r13d);
			a.jmp(0x140271D02);
		});

		game::Sys_File sys_create_file_stub(game::Sys_Folder folder, const char* base_filename)
		{
			auto result = sys_create_file_hook.invoke<game::Sys_File>(folder, base_filename);

			if (result.handle != INVALID_HANDLE_VALUE)
			{
				return result;
			}

			if (!is_using_mods())
			{
				return result;
			}

			// .ff extension was added previously
			if (!std::strcmp(base_filename, "mod.ff") && mods::db_mod_file_exists())
			{
				char file_path[MAX_PATH]{};
				db_build_os_path_from_source("mod", game::FFD_MOD_DIR, sizeof(file_path), file_path);
				result.handle = game::Sys_OpenFileReliable(file_path);
			}

			return result;
		}

		game::Sys_File sp_test_sys_create_file_stub(game::Sys_Folder folder, const char* base_filename)
		{
			auto result = sp_test_sys_create_file_hook.invoke<game::Sys_File>(folder, base_filename);

			if (result.handle != INVALID_HANDLE_VALUE || folder != game::SF_ZONE || !game::environment::is_sp())
			{
				return result;
			}

			const auto request = get_active_sp_test_zone_request(base_filename);
			if (!request.has_value())
			{
				return result;
			}

			result.handle = game::Sys_OpenFileReliable(request->path.c_str());
			if (result.handle == INVALID_HANDLE_VALUE)
			{
				console::error("Failed to open the preflighted SP test fastfile '%s'.\n", request->path.c_str());
				return result;
			}

			console::info("Opened SP test fastfile fallback '%s'.\n", request->path.c_str());
			return result;
		}

		void db_load_x_assets_stub(game::XZoneInfo* zone_info, unsigned int zone_count, game::DBSyncMode sync_mode)
		{
			std::vector<game::XZoneInfo> zones(zone_info, zone_info + zone_count);

			if (db_mod_file_exists())
			{
				zones.emplace_back("mod", game::DB_ZONE_COMMON | game::DB_ZONE_CUSTOM, 0);
			}

			game::DB_LoadXAssets(zones.data(), static_cast<unsigned int>(zones.size()), sync_mode);
		}

		constexpr bool is_valid_fastfile_basename(const std::string_view name)
		{
			if (name.empty() || name.size() > 64)
			{
				return false;
			}

			for (const auto character : name)
			{
				const auto is_ascii_letter = (character >= 'a' && character <= 'z') ||
					(character >= 'A' && character <= 'Z');
				const auto is_ascii_digit = character >= '0' && character <= '9';
				if (!is_ascii_letter && !is_ascii_digit && character != '_' && character != '-')
				{
					return false;
				}
			}

			return true;
		}

		static_assert(is_valid_fastfile_basename("campaign_mp_weapons_fxfix_pointerfixed_r1"));
		static_assert(!is_valid_fastfile_basename("../campaign_mp_weapons"));
		static_assert(!is_valid_fastfile_basename("campaign_mp_weapons.ff"));
		static_assert(!is_valid_fastfile_basename("campaign_mp_weapons\\zone"));

		void load_sp_test_zone(const char* zone_name)
		{
			std::error_code path_error;
			const auto staging_root = std::filesystem::current_path(path_error);
			if (path_error)
			{
				const auto error_message = path_error.message();
				console::error("Unable to resolve the working directory for SP test zones: %s\n", error_message.c_str());
				return;
			}

			const auto zone_path = (staging_root / "sp_test_zones" / (std::string(zone_name) + ".ff")).string();
			console::info("Resolved SP test fastfile path: '%s'\n", zone_path.c_str());

			const auto preflight_handle = game::Sys_OpenFileReliable(zone_path.c_str());
			if (preflight_handle == INVALID_HANDLE_VALUE)
			{
				console::error("SP test fastfile is missing or unreadable: '%s'\n", zone_path.c_str());
				return;
			}

			::CloseHandle(preflight_handle);

			{
				scoped_sp_test_zone_request active_request{{std::string(zone_name) + ".ff", zone_path}};
				if (!active_request.active())
				{
					console::error("Another SP test fastfile load is already active.\n");
					return;
				}

				console::warn("EXPERIMENTAL: loading SP test fastfile '%s'. SP flags, lookup and zone residency still require runtime validation.\n", zone_name);

				game::XZoneInfo zone_info{zone_name, game::DB_ZONE_COMMON, 0};
				game::DB_LoadXAssets(&zone_info, 1U, game::DB_LOAD_SYNC);
			}

			console::info("loadzone returned for '%s'; verify asset visibility and map-transition residency manually.\n", zone_name);
		}

		void register_sp_loadzone_test_command()
		{
			const auto* enabled = game::Dvar_RegisterBool("sp_testLoadZoneEnabled", false, game::DVAR_FLAG_NONE);

			command::add("loadzone", [enabled](const command::params& params)
			{
				if (!game::environment::is_sp())
				{
					console::error("loadzone is only available in Singleplayer.\n");
					return;
				}

				if (params.size() != 2)
				{
					console::info("USAGE: loadzone <fastfile_basename>\n");
					console::info("Enable this experimental command with: set sp_testLoadZoneEnabled 1\n");
					return;
				}

				if (enabled == nullptr || !enabled->current.enabled)
				{
					console::error("loadzone is disabled; set sp_testLoadZoneEnabled 1 to opt in.\n");
					return;
				}

				if (!game::Sys_IsDatabaseReady2())
				{
					console::error("loadzone requires the asset database to be ready.\n");
					return;
				}

				const auto* zone_name = params.get(1);
				if (!is_valid_fastfile_basename(zone_name))
				{
					console::error("Invalid fastfile basename. Use 1-64 ASCII letters, digits, '_' or '-'; omit paths and '.ff'.\n");
					return;
				}

				load_sp_test_zone(zone_name);
			});
		}
	}

	bool is_using_mods()
	{
		return (*dvars::fs_gameDirVar) && *(*dvars::fs_gameDirVar)->current.string;
	}

	bool db_mod_file_exists()
	{
		if (!*(*dvars::fs_gameDirVar)->current.string)
		{
			return false;
		}

		char filename[MAX_PATH]{};
		db_build_os_path_from_source("mod", game::FFD_MOD_DIR, sizeof(filename), filename);

		if (auto zone_file = game::Sys_OpenFileReliable(filename); zone_file != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(zone_file);
			return true;
		}

		return false;
	}

	class component final : public component_interface
	{
	public:
		static_assert(sizeof(game::Sys_File) == 8);

		void post_unpack() override
		{
			dvars::fs_gameDirVar = reinterpret_cast<game::dvar_t**>(SELECT_VALUE(0x14A6A7D98, 0x14B20EB48));

			// Remove DVAR_INIT from fs_game
			utils::hook::set<std::uint32_t>(SELECT_VALUE(0x14036137F + 2, 0x1404AE4CB + 2), SELECT_VALUE(game::DVAR_FLAG_NONE, game::DVAR_FLAG_SERVERINFO));

			utils::hook::inject(SELECT_VALUE(0x140361391 + 3, 0x1404AE4D6 + 3), &fs_game_dir_domain_func);

			if (game::environment::is_sp())
			{
				sp_test_sys_create_file_hook.create(game::Sys_CreateFile, sp_test_sys_create_file_stub);
				register_sp_loadzone_test_command();
				return;
			}

			utils::hook::nop(0x140271B54, 15);
			utils::hook::jump(0x140271B54, skip_extra_zones_stub, true);

			// Add custom zone paths
			sys_create_file_hook.create(game::Sys_CreateFile, sys_create_file_stub);

			// Load mod.ff
			utils::hook::call(0x1405A562A, db_load_x_assets_stub); // R_LoadGraphicsAssets According to myself but I don't remember where I got it from

			command::add("loadmod", [](const command::params& params) -> void
			{
				if (params.size() != 2)
				{
					console::info("USAGE: %s \"mods/<mod name>\"", params.get(0));
					return;
				}

				std::string mod_name = utils::string::to_lower(params.get(1));

				if (!mod_name.empty() && !mod_name.starts_with("mods/"))
				{
					mod_name.insert(0, "mods/");
				}

				// change fs_game if needed
				if (mod_name != (*dvars::fs_gameDirVar)->current.string)
				{
					game::Dvar_SetString((*dvars::fs_gameDirVar), mod_name.c_str());
					command::execute("vid_restart\n");
				}
			});

			command::add("unloadmod", []([[maybe_unused]] const command::params& params) -> void
			{
				if (*dvars::fs_gameDirVar == nullptr || *(*dvars::fs_gameDirVar)->current.string == '\0')
				{
					return;
				}

				game::Dvar_SetString(*dvars::fs_gameDirVar, "");
				command::execute("vid_restart\n");
			});

			// TODO: without a way to monitor all the ways fs_game can be changed there is no way to detect when we
			// should unregister the path from the internal filesystem we use
			// HINT: It could be done in fs_game_dir_domain_func, but I haven't tested if that's the best place to monitor for changes and register/unregister the mods folder
		}
	};
}

REGISTER_COMPONENT(mods::component)
