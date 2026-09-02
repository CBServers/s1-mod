#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "barrier_clips.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

namespace barrier_clips
{
	namespace
	{
		constexpr int MASK_PLAYER_CLIP = 0x10000;
		constexpr int MASK_BARRIER_CLIP = 0x400;
		constexpr int PMF_LADDER = 0x8;

		constexpr auto PM_TRACEMASK = 0x90; // pmove_t::tracemask
		constexpr auto PS_PM_FLAGS = 0x54;  // playerState_s::pm_flags

		constexpr auto ADDR_PMOVE_SINGLE = 0x14014AA70;

		constexpr auto MAX_CLIENTS = 18;

		const game::dvar_t* bg_disableBarrierClips = nullptr;
		const game::dvar_t* bg_disableBarrierClipsClient = nullptr;
		const game::dvar_t* sv_running = nullptr;

		// authoritative per-client preference, kept in sync server-side (see set_client_pref)
		bool client_pref[MAX_CLIENTS] = {};

		utils::hook::detour pmove_single_hook;

		bool enabled(void* ps)
		{
			if (bg_disableBarrierClips != nullptr && bg_disableBarrierClips->current.enabled)
			{
				return true; // server master switch forces the barrier clips off for everyone
			}

			if (sv_running == nullptr)
			{
				sv_running = game::Dvar_FindVar("sv_running");
			}

			if (sv_running != nullptr && sv_running->current.enabled)
			{
				// running the authoritative sim: honour this client's own preference (ps+0 is the client num)
				const int client_num = *reinterpret_cast<std::uint16_t*>(ps);
				return client_num < MAX_CLIENTS && client_pref[client_num];
			}

			// remote client: only ever predicts the local player, so use our own local preference
			return bg_disableBarrierClipsClient != nullptr && bg_disableBarrierClipsClient->current.enabled;
		}

		void pmove_single_stub(void* pm)
		{
			if (pm != nullptr)
			{
				auto* ps = *reinterpret_cast<void**>(pm); // pmove_t::ps
				if (ps != nullptr && enabled(ps)
					&& (*reinterpret_cast<int*>(static_cast<char*>(ps) + PS_PM_FLAGS) & PMF_LADDER) == 0)
				{
					auto& tracemask = *reinterpret_cast<int*>(static_cast<char*>(pm) + PM_TRACEMASK);
					tracemask &= ~MASK_PLAYER_CLIP;
					tracemask |= MASK_BARRIER_CLIP;
				}
			}

			pmove_single_hook.invoke<void>(pm);
		}
	}

	void set_client_pref(const int client_num, const bool value)
	{
		if (client_num >= 0 && client_num < MAX_CLIENTS)
		{
			client_pref[client_num] = value;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp())
			{
				return;
			}

			pmove_single_hook.create(ADDR_PMOVE_SINGLE, &pmove_single_stub);

			// server master switch: "Disable player collision with out of bound barriers" for everyone
			bg_disableBarrierClips = game::Dvar_RegisterBool("bg_disableBarrierClips", false,
				game::DVAR_FLAG_REPLICATED);

			// per-client opt-in; pushed by the server via `self setclientdvar("bg_disableBarrierClipsClient", 1)`
			bg_disableBarrierClipsClient = game::Dvar_RegisterBool("bg_disableBarrierClipsClient", false,
				game::DVAR_FLAG_NONE);
		}
	};
}

REGISTER_COMPONENT(barrier_clips::component)
