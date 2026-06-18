#ifndef CHRONO_RIFT_NPC_LOGIC_H
#define CHRONO_RIFT_NPC_LOGIC_H

#include "../shared.h"

/*
 * Per-slot decision (npc_slot_index 0 .. CR_MAX_NPCS-1 maps to entity CR_MAX_PLAYERS+index).
 * Must hold state_mutex. No-op unless it is this NPC’s turn and the encounter still uses this slot.
 */
void asp_submit_npc_action_for_slot(cr_shared_state_t *state, int npc_slot_index);

/*
 * Convenience: resolve active_entity_id to a slot and call asp_submit_npc_action_for_slot.
 * Must hold state_mutex.
 */
void asp_submit_npc_action(cr_shared_state_t *state);

#endif /* CHRONO_RIFT_NPC_LOGIC_H */
