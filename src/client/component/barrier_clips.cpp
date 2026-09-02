#include <std_include.hpp>
#include "loader/component_loader.hpp"

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

		const game::dvar_t* bg_disableBarrierClips = nullptr;

		utils::hook::detour pmove_single_hook;

		void pmove_single_stub(void* pm)
		{
			if (bg_disableBarrierClips && bg_disableBarrierClips->current.enabled && pm != nullptr)
			{
				auto* ps = *reinterpret_cast<void**>(pm); // pmove_t::ps
				if (ps != nullptr
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

			// "Disable player collision with out of bound barriers"
			bg_disableBarrierClips = game::Dvar_RegisterBool("bg_disableBarrierClips", false,
				game::DVAR_FLAG_REPLICATED);
		}
	};
}

REGISTER_COMPONENT(barrier_clips::component)
