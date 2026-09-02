#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "mechanics.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

namespace mechanics
{
	namespace
	{
		constexpr auto PM_CMD_BUTTONS = 0xC;
		constexpr auto PM_CMD_WEAPON = 0x1C;

		constexpr auto PS_PM_FLAGS = 0x54;
		constexpr auto PS_WEAPON = 0x5C4;
		constexpr auto PS_WEAPFLAGS = 0x5C8;

		constexpr auto PS_WEAPSTATE = 0x364;
		constexpr auto WEAPSTATE_STRIDE = 28;
		constexpr auto WS_WEAPANIM = 0;
		constexpr auto WS_WEAPONTIME = 4;
		constexpr auto WS_WEAPONDELAY = 8;
		constexpr auto WS_WEAPONRESTRICTKICKTIME = 12;
		constexpr auto WS_WEAPONSTATE = 16;

		constexpr int WEAP_ANIM_IDLE = 1;

		constexpr auto ADDR_PM_BEGIN_WEAPON_CHANGE = 0x140154AB0;
		constexpr auto ADDR_PM_WEAPON_CHECK_FOR_CHANGE = 0x140158DA0;
		constexpr auto ADDR_PM_WEAPON_INVALID_CHANGE_STATE = 0x14015C750;
		constexpr auto ADDR_BG_CLEAR_DROP_WEAPON_ANIM = 0x14012BD10;
		constexpr auto ADDR_PM_IS_SPRINTING = 0x140146050;
		constexpr auto ADDR_SPRINT_STATE_RAISE = 0x140160F00;

		constexpr auto MAX_CLIENTS = 18;

		const game::dvar_t* pm_improvedMechanics = nullptr;
		const game::dvar_t* pm_improvedMechanicsClient = nullptr;
		const game::dvar_t* sv_running = nullptr;

		// authoritative per-client preference, kept in sync server-side (see set_client_pref)
		bool client_pref[MAX_CLIENTS] = {};

		utils::hook::detour pm_begin_weapon_change_hook;
		utils::hook::detour pm_weapon_check_for_change_weapon_hook;

		bool enabled(void* ps)
		{
			if (pm_improvedMechanics != nullptr && pm_improvedMechanics->current.enabled)
			{
				return true; // server master switch forces the mechanics on for everyone
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
			return pm_improvedMechanicsClient != nullptr && pm_improvedMechanicsClient->current.enabled;
		}

		int& weap_field(void* ps, int hand, int field)
		{
			return *reinterpret_cast<int*>(static_cast<char*>(ps)
				+ PS_WEAPSTATE + WEAPSTATE_STRIDE * hand + field);
		}

		int& ps_field(void* ps, int off)
		{
			return *reinterpret_cast<int*>(static_cast<char*>(ps) + off);
		}

		void pm_begin_weapon_change_stub(void* pm, unsigned int newweapon, char is_alternate,
			char quick, unsigned int* holdrand)
		{
			auto* ps = *reinterpret_cast<void**>(pm);

			const auto anim = weap_field(ps, 0, WS_WEAPANIM);
			const auto anim2 = weap_field(ps, 1, WS_WEAPANIM);

			const auto buttons = *reinterpret_cast<int*>(static_cast<char*>(pm) + PM_CMD_BUTTONS);
			const auto keep_anim = (buttons & 3) != 0
				|| utils::hook::invoke<bool>(ADDR_PM_IS_SPRINTING, ps);

			pm_begin_weapon_change_hook.invoke<void>(pm, newweapon, is_alternate, quick, holdrand);

			if (enabled(ps) && keep_anim)
			{
				weap_field(ps, 0, WS_WEAPANIM) = anim;
				weap_field(ps, 1, WS_WEAPANIM) = anim2;
			}
		}

		void pm_weapon_check_for_change_weapon_stub(void* pm, unsigned int* holdrand, float a3)
		{
			auto* ps = *reinterpret_cast<void**>(pm);

			if (enabled(ps)
				&& ps_field(ps, PS_WEAPON) == *reinterpret_cast<int*>(static_cast<char*>(pm) + PM_CMD_WEAPON)
				&& static_cast<unsigned int>(weap_field(ps, 0, WS_WEAPONSTATE) - 3) <= 2
				&& utils::hook::invoke<__int64>(ADDR_PM_WEAPON_INVALID_CHANGE_STATE, pm) != 0
				&& ps_field(ps, PS_WEAPFLAGS) != 128
				&& ps_field(ps, PS_PM_FLAGS) != 8
				&& ps_field(ps, PS_PM_FLAGS) != 40)
			{
				if (utils::hook::invoke<bool>(ADDR_PM_IS_SPRINTING, ps))
				{
					utils::hook::invoke<void>(ADDR_SPRINT_STATE_RAISE, ps);
					return;
				}

				for (int hand = 0; hand < 2; ++hand)
				{
					weap_field(ps, hand, WS_WEAPANIM) = WEAP_ANIM_IDLE;
					weap_field(ps, hand, WS_WEAPONSTATE) = 0;
					weap_field(ps, hand, WS_WEAPONDELAY) = 0;
					weap_field(ps, hand, WS_WEAPONTIME) = 0;
					weap_field(ps, hand, WS_WEAPONRESTRICTKICKTIME) = 0;
				}

				utils::hook::invoke<void>(ADDR_BG_CLEAR_DROP_WEAPON_ANIM, ps);
				return;
			}

			pm_weapon_check_for_change_weapon_hook.invoke<void>(pm, holdrand, a3);
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

			pm_begin_weapon_change_hook.create(ADDR_PM_BEGIN_WEAPON_CHANGE, &pm_begin_weapon_change_stub);
			pm_weapon_check_for_change_weapon_hook.create(ADDR_PM_WEAPON_CHECK_FOR_CHANGE,
				&pm_weapon_check_for_change_weapon_stub);

			pm_improvedMechanics = game::Dvar_RegisterBool("pm_improvedMechanics", false,
				game::DVAR_FLAG_REPLICATED);

			// per-client opt-in; pushed by the server via `self setclientdvar("pm_improvedMechanicsClient", 1)`
			// SCRIPTINFO so setclientdvar accepts it without relaxing the engine's check for every other dvar
			pm_improvedMechanicsClient = game::Dvar_RegisterBool("pm_improvedMechanicsClient", false,
				game::DVAR_FLAG_SCRIPTINFO);
		}
	};
}

REGISTER_COMPONENT(mechanics::component)
