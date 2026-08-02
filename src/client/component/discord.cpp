#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "console.hpp"
#include "discord.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <utils/string.hpp>
#include <utils/cryptography.hpp>

#include <discord_rpc.h>

#include <atomic>
#include <optional>
#include <utility>

namespace discord
{
	namespace
	{
		constexpr auto* JOIN_SECRET_PREFIX = "s1:1:";

		DiscordRichPresence discord_presence;

		// Invite-driven joins wait here until the game is ready to act on a connect.
		std::mutex pending_route_mutex;
		std::optional<std::pair<std::string, std::string>> pending_route; // (token, address)

		// Set once the engine reaches the menu; connect/network are live only after this.
		std::atomic_bool game_initialized{false};

		// Ownership handoff. wire_* is set from the IPC IO thread; the rest is main-thread only.
		constexpr auto OWNERSHIP_RELEASE_GRACE = 5s;
		std::atomic_bool wire_launcher_owns{false};
		bool effective_launcher_owns = false; // native RPC stays silent while true
		bool release_pending = false;
		std::chrono::steady_clock::time_point release_deadline{};

		std::string truncate(const std::string& value, const size_t max_length)
		{
			return value.size() <= max_length ? value : value.substr(0, max_length);
		}

		std::string strip_colors(const std::string& value)
		{
			std::string stripped(value.size() + 1, '\0');
			utils::string::strip(value.data(), stripped.data(), stripped.size());
			stripped.resize(std::strlen(stripped.data()));
			return stripped;
		}

		// Engine snapshot player count (magic global has a single home here).
		int get_snapshot_player_count()
		{
			return *reinterpret_cast<int*>(0x1414CC290);
		}

		// Splits "ip:port" into its parts.
		bool split_address(const std::string& address, std::string& ip, int& port)
		{
			const auto sep = address.rfind(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			ip = address.substr(0, sep);
			port = atoi(address.substr(sep + 1).data());
			return !ip.empty() && port > 0;
		}

		std::string get_join_address()
		{
			if (!game::CL_IsCgameInitialized())
			{
				return {};
			}

			// Host: our reachable endpoint, paired with a real token to hole-punch.
			if (auto endpoint = nat::get_host_endpoint(); !endpoint.empty())
			{
				return endpoint;
			}

			// Client on a public dedi: advertise it (token "-"); the name check rejects stale/private connection state.
			if (!party::get_public_server_name().empty())
			{
				return network::address_to_string(party::get_target());
			}

			return {};
		}

		std::string make_join_secret(const std::string& address)
		{
			if (address.empty())
			{
				return {};
			}

			// "s1:1:<token>:<ip>:<port>"; token "-" means direct-only (no punch).
			const auto token = nat::current_token();
			const auto token_field = token.empty() ? std::string("-") : token;

			const auto secret = std::string(JOIN_SECRET_PREFIX) + token_field + ":" + address;
			if (secret.size() >= 128)
			{
				return {};
			}

			return secret;
		}

		bool parse_join_secret(const std::string& secret, std::string& token, std::string& address)
		{
			if (!utils::string::starts_with(secret, JOIN_SECRET_PREFIX))
			{
				return false;
			}

			// "<token>:<ip>:<port>"
			const auto raw = secret.substr(std::strlen(JOIN_SECRET_PREFIX));
			const auto sep = raw.find(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			token = raw.substr(0, sep);

			const auto raw_address = raw.substr(sep + 1);
			const auto parsed = network::address_from_string(raw_address);
			if (!network::is_connectable_address(parsed))
			{
				return false;
			}

			address = network::address_to_string(parsed);
			return true;
		}

		// Route a structured join: token "-"/empty => direct connect, else NAT punch. Main pipeline only.
		void route_join(const std::string& token, const std::string& address)
		{
			if (token.empty() || token == "-")
			{
				// party::connect directly; skips a command-buffer round trip.
				const auto target = network::address_from_string(address);
				if (network::is_connectable_address(target))
				{
					party::connect(target);
				}
				else
				{
					console::error("Discord: invalid join address\n");
				}
				return;
			}

			// Hole-punch toward the host; falls back to `address` (port-forward/VPN).
			nat::begin_join(token, address);
		}

		// True once the engine has reached the menu and online data is synced (safe to connect).
		bool join_ready()
		{
			// SP can't join MP sessions.
			if (game::environment::is_sp())
			{
				return false;
			}

			return game_initialized.load() && game::Live_SyncOnlineDataFlags(0) == 0;
		}

		// Drains an invite-driven join, but only once join_ready() (routing mid-load crashes).
		void process_pending_route()
		{
			if (!join_ready())
			{
				return; // engine still coming up; keep waiting
			}

			std::optional<std::pair<std::string, std::string>> route;
			{
				std::lock_guard<std::mutex> lock(pending_route_mutex);
				route = std::move(pending_route);
				pending_route.reset();
			}

			if (route)
			{
				route_join(route->first, route->second);
			}
		}

		void join_game(const char* join_secret)
		{
			if (!join_secret || !join_secret[0])
			{
				return;
			}

			std::string token;
			std::string address;
			if (!parse_join_secret(join_secret, token, address))
			{
				// Legacy/raw-address invite (pre-token secrets were just "ip:port").
				const auto parsed = network::address_from_string(join_secret);
				if (!network::is_connectable_address(parsed))
				{
					console::error("Discord: invalid join secret\n");
					return;
				}

				token = "-";
				address = network::address_to_string(parsed);
			}

			// Queue like launcher joins; process_pending_route routes once join_ready().
			queue_join(token, address);
		}

		void join_request(const DiscordUser* request)
		{
#ifdef _DEBUG
			console::info("Discord: Join request from %s (%s)\n", request->username, request->userId);
#endif
			Discord_Respond(request->userId, DISCORD_REPLY_IGNORE);
		}

		// Native<->silent handoff, debounced on release so a launcher restart doesn't flicker the card.
		void ownership_tick()
		{
			if (wire_launcher_owns.load())
			{
				release_pending = false;
				if (!effective_launcher_owns)
				{
					effective_launcher_owns = true;
					Discord_ClearPresence(); // clear once on entry; keep the connection initialized
				}
				return;
			}

			if (!effective_launcher_owns)
			{
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			if (!release_pending)
			{
				release_pending = true;
				release_deadline = now + OWNERSHIP_RELEASE_GRACE;
				return;
			}

			if (now >= release_deadline)
			{
				effective_launcher_owns = false; // native RPC resumes on the next update_discord tick
				release_pending = false;
			}
		}

		void update_discord()
		{
			if (effective_launcher_owns)
			{
				return; // launcher owns presence; stay silent but connected
			}

			std::string join_secret_storage;
			std::string party_id_storage;
			std::string host_name_storage;

			auto* dvar = game::Dvar_FindVar("virtualLobbyActive");
			if (!game::CL_IsCgameInitialized() || (dvar && dvar->current.enabled == 1))
			{
				discord_presence.details = game::environment::is_sp() ? "Singleplayer" : "Multiplayer";

				dvar = game::Dvar_FindVar("virtualLobbyInFiringRange");
				if (dvar && dvar->current.enabled == 1)
				{
					discord_presence.state = "Firing Range";
				}
				else
				{
					discord_presence.state = "Main Menu";
				}

				discord_presence.partySize = 0;
				discord_presence.partyMax = 0;
				discord_presence.partyId = nullptr;
				discord_presence.joinSecret = nullptr;
				discord_presence.partyPrivacy = 0;
				discord_presence.startTimestamp = 0;
			}
			else
			{
				if (game::environment::is_sp()) return;

				const auto* gametype = game::UI_GetGameTypeDisplayName(game::Dvar_FindVar("ui_gametype")->current.string);
				const auto* map = game::UI_GetMapDisplayName(game::Dvar_FindVar("ui_mapname")->current.string);

				discord_presence.details = utils::string::va("%s on %s", gametype, map);

				// Host name via the shared reader; strip into function-scope storage so state stays valid until update.
				const auto raw_host_name = party::get_raw_host_name();
				host_name_storage.assign(raw_host_name.size() + 1, '\0');
				utils::string::strip(raw_host_name.data(), host_name_storage.data(), host_name_storage.size());
				host_name_storage.resize(std::strlen(host_name_storage.data()));

				discord_presence.partySize = get_snapshot_player_count();

				const auto* name_dvar = game::Dvar_FindVar("name");
				if (name_dvar && host_name_storage == name_dvar->current.string) // host name == player name => private match
				{
					discord_presence.state = "Private Match";
					discord_presence.partyMax = game::Dvar_FindVar("sv_maxclients")->current.integer;
				}
				else
				{
					discord_presence.state = host_name_storage.data();
					discord_presence.partyMax = party::server_client_count();
				}

				// Join secret: an open private match (hole-punch) OR a directly reachable server.
				const auto join_address = get_join_address();
				join_secret_storage = make_join_secret(join_address);

				if (!join_secret_storage.empty())
				{
					static const auto nonce = utils::cryptography::random::get_integer();
					std::hash<std::string> hash_fn;
					party_id_storage = utils::string::va("%zu", hash_fn(join_address) ^ nonce);

					discord_presence.partyId = party_id_storage.data();
					discord_presence.joinSecret = join_secret_storage.data();
					discord_presence.partyPrivacy = DISCORD_PARTY_PUBLIC;
				}
				else
				{
					discord_presence.partyId = nullptr;
					discord_presence.joinSecret = nullptr;
					discord_presence.partyPrivacy = 0;
				}

				if (!discord_presence.startTimestamp)
				{
					discord_presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
						std::chrono::system_clock::now().time_since_epoch()).count();
				}
			}

			Discord_UpdatePresence(&discord_presence);
		}
	}

	std::string get_current_mode()
	{
		// From the launch mode, not a game call: the IPC hello reads this off-thread before the engine is unpacked.
		switch (game::environment::get_real_mode())
		{
		case launcher::mode::zombies:
			return "zm";
		case launcher::mode::survival:
			return "sv";
		case launcher::mode::singleplayer:
			return "sp";
		default:
			return "mp";
		}
	}

	presence_state get_presence_state()
	{
		presence_state state{};

		// Always reported (even at the menu) so the launcher knows which play mode is running.
		state.mode = get_current_mode();

		auto* vl = game::Dvar_FindVar("virtualLobbyActive");
		state.in_game = game::CL_IsCgameInitialized() && !(vl && vl->current.enabled == 1);
		if (!state.in_game)
		{
			return state; // menu => empty map
		}

		// SP: UI_Get*DisplayName and the MP players global don't exist in the SP binary.
		if (game::environment::is_sp())
		{
			const auto* sp_map = game::Dvar_FindVar("mapname");
			state.mapname = sp_map && sp_map->current.string ? sp_map->current.string : std::string{};
			state.map_display = truncate(strip_colors(state.mapname), 128);
			return state;
		}

		const auto* mapname_dvar = game::Dvar_FindVar("ui_mapname");
		const auto* gametype_dvar = game::Dvar_FindVar("ui_gametype");
		const std::string mapname = mapname_dvar ? mapname_dvar->current.string : std::string{};
		const std::string gametype = gametype_dvar ? gametype_dvar->current.string : std::string{};

		state.mapname = mapname;
		state.map_display = mapname.empty()
			                    ? std::string{}
			                    : truncate(strip_colors(game::UI_GetMapDisplayName(mapname.data())), 128);
		state.gametype = gametype.empty()
			                 ? std::string{}
			                 : truncate(strip_colors(game::UI_GetGameTypeDisplayName(gametype.data())), 128);
		state.server_name = truncate(strip_colors(party::get_public_server_name()), 128);

		const auto* max_clients_dvar = game::Dvar_FindVar("sv_maxclients");
		const auto max_clients = max_clients_dvar ? max_clients_dvar->current.integer : 0;
		if (max_clients > 0)
		{
			auto players = get_snapshot_player_count();
			if (players < 1)
			{
				players = 1;
			}
			state.players = players;
			state.max_players = max_clients;
		}

		return state;
	}

	std::optional<join_transport> get_join_transport()
	{
		const auto address = get_join_address();
		if (address.empty())
		{
			return std::nullopt;
		}

		const auto token = nat::current_token();

		join_transport transport{};
		if (token.empty())
		{
			// Directly reachable public server: advertise it as a direct connect.
			if (!split_address(address, transport.ip, transport.port))
			{
				return std::nullopt;
			}
			transport.is_nat = false;
			return transport;
		}

		// Hosting: NAT punch with the reachable endpoint as fallback.
		transport.is_nat = true;
		transport.token = token;
		nat::get_rendezvous(transport.rendezvous_host, transport.rendezvous_port);
		split_address(address, transport.fallback_ip, transport.fallback_port);
		return transport;
	}

	void queue_join(const std::string& token, const std::string& address)
	{
		std::lock_guard<std::mutex> lock(pending_route_mutex);
		pending_route = std::make_pair(token, address);
	}

	void set_launcher_presence_owner(const bool launcher_owns)
	{
		wire_launcher_owns.store(launcher_owns);
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			DiscordEventHandlers handlers;
			ZeroMemory(&handlers, sizeof(handlers));
			handlers.ready = ready;
			handlers.errored = errored;
			handlers.disconnected = errored;
			handlers.joinGame = join_game;
			handlers.spectateGame = nullptr;
			handlers.joinRequest = join_request;

			Discord_Initialize("1117777088301240350", &handlers, 1, nullptr);

			scheduler::on_game_initialized([]
			{
				game_initialized = true;
			}, scheduler::pipeline::main);

			scheduler::loop(update_discord, scheduler::pipeline::main, 5s);

			// Callbacks and join routing drive the NAT punch and game socket; main thread only.
			scheduler::loop([]
			{
				ownership_tick();
				Discord_RunCallbacks();
				process_pending_route();
			}, scheduler::pipeline::main, 250ms);

			initialized_ = true;
		}

		void pre_destroy() override
		{
			if (!initialized_)
			{
				return;
			}

			Discord_Shutdown();
		}

	private:
		bool initialized_ = false;

		static void ready(const DiscordUser* /*request*/)
		{
			ZeroMemory(&discord_presence, sizeof(discord_presence));

			discord_presence.instance = 1;

			console::info("Discord: Ready\n");

			// Don't prime a card while the launcher owns presence (e.g. a Discord reconnect mid-session).
			if (!effective_launcher_owns)
			{
				Discord_UpdatePresence(&discord_presence);
			}
		}

		static void errored(const int error_code, const char* message)
		{
			console::error("Discord: Error (%i): %s\n", error_code, message);
		}
	};
}

#ifndef DEV_BUILD
REGISTER_COMPONENT(discord::component)
#endif
