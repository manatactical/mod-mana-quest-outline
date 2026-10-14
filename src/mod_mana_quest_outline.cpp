/*
 * mod-mana-quest-outline: tell the client which visible units are quest givers.
 *
 * Server-side companion to the client's wxl-mana-unit-outline module. That module draws a silhouette
 * outline, but it can only select a unit the player pointed at or targeted -- the client's own UI
 * exposes no quest state for a unit that is merely nearby, and stock 3.3.5a has no
 * C_QuestLog.GetUnitQuestInfo. The server already knows: Player::GetQuestDialogStatus answers a
 * QuestGiverStatus per visible object and SendQuestGiverStatusMultiple pushes it for the minimap.
 *
 * This module computes the same answer for the visible objects and pushes it to the owning player
 * over the addon channel, which is the only channel a native client module can read.
 *
 * Protocol, an addon whisper (prefix "WXLQ", the client registers the prefix):
 *   server -> client:  "WXLQ\tS:<hi>:<lo>:<status>,<hi>:<lo>:<status>,..."
 *
 *   The body always starts with "S:" and there is no request half: the module re-sends the whole set
 *   on a timer, so a sync that is dropped (an addon message the client's chat layer limits) is
 *   corrected by the next one and no client-side request/response state is needed.
 *
 *   <hi>:<lo> is the GUID ObjectGuid::GetRawValue() split into two 32-bit halves, hex without a
 *   prefix, which is what the client's world::Guid() yields. The two halves are sent separately
 *   because Lua 5.1 numbers are doubles and a packed 64-bit GUID would round. One status per
 *   questgiver, in the DIALOG_STATUS_* numbering the client links back to a color.
 *
 *   Fields are appended over time, never reordered: an older client body that cannot parse the tail
 *   keeps every field it does understand.
 *
 * Only givers with something to do are sent -- an available or turn-in quest, and its _REP variants
 * (DIALOG_STATUS_UNAVAILABLE and DIALOG_STATUS_NONE are dropped, and INCOMPLETE with them). That
 * also keeps the body small, because the client outlines everything in it.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "QuestDef.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
    // The statuses worth sending: a quest can be taken, or a finished one handed in. LABEL_* on the
    // client side mirror these, and the client's own minimap uses the same numbering.
    bool IsActiveQuestStatus(uint32 status)
    {
        switch (status)
        {
            case DIALOG_STATUS_LOW_LEVEL_AVAILABLE:
            case DIALOG_STATUS_LOW_LEVEL_REWARD_REP:
            case DIALOG_STATUS_LOW_LEVEL_AVAILABLE_REP:
            case DIALOG_STATUS_REWARD_REP:
            case DIALOG_STATUS_AVAILABLE_REP:
            case DIALOG_STATUS_AVAILABLE:
            case DIALOG_STATUS_REWARD2:
            case DIALOG_STATUS_REWARD:
                return true;
            default:
                return false;
        }
    }

    struct Config
    {
        bool     enabled    = true;
        uint32   intervalMs = 4000; // full resend cadence; the client holds the last set in between
        uint32   maxCount   = 48;   // givers per packet, so one crowded hub cannot split a body
    };

    Config g_config;

    // Appends one "hi:lo:status" field; both halves are printed as hex so a full 64-bit GUID survives
    // the round trip through the client's double-based number parsing.
    void AppendEntry(std::string& out, ObjectGuid guid, uint32 status)
    {
        const uint64 raw = guid.GetRawValue();
        char field[48];
        std::snprintf(field, sizeof(field), "%08X:%08X:%u",
                      static_cast<unsigned>(raw >> 32), static_cast<unsigned>(raw & 0xFFFFFFFFu), status);
        if (!out.empty())
            out += ',';
        out += field;
    }

    class ManaQuestOutlineWorld : public WorldScript
    {
    public:
        explicit ManaQuestOutlineWorld()
            : WorldScript("ManaQuestOutlineWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            g_config.enabled    = sConfigMgr->GetOption<bool>("ManaQuestOutline.Enable", true);
            g_config.intervalMs = sConfigMgr->GetOption<uint32>("ManaQuestOutline.IntervalMs", 4000);
            g_config.maxCount   = sConfigMgr->GetOption<uint32>("ManaQuestOutline.MaxPerSync", 48);

            // A tiny interval would turn the sync into a flood; the client only samples it once a
            // frame and holds the last set, so anything under half a second buys nothing.
            if (g_config.intervalMs < 500)
                g_config.intervalMs = 500;
            if (!g_config.maxCount)
                g_config.maxCount = 1;
        }

        // One timer for the whole world, not a per-player accumulator: the walk is per player anyway,
        // and the module owns no per-character state to track.
        void OnUpdate(uint32 diff) override
        {
            if (!g_config.enabled)
                return;

            elapsed_ += diff;
            if (elapsed_ < g_config.intervalMs)
                return;
            elapsed_ = 0;

            // this is not cheap (a status resolve per visible giver), so it runs here rather than every
            // tick, and the walk below stops as soon as the cap is reached.
            for (auto const& pair : sWorldSessionMgr->GetAllSessions())
            {
                Player* player = pair.second->GetPlayer();
                if (!player || !player->IsInWorld())
                    continue;
                SendSync(player);
            }
        }

    private:
        // Mirrors Player::SendQuestGiverStatusMultiple, minus the gossip/session bookkeeping: same
        // visibility walk, same two kinds of giver, same status call, so the client sees exactly the
        // set the minimap was already built from.
        void SendSync(Player* player) const
        {
            std::string body = "S:";
            uint32      sent = 0;

            player->DoForAllVisibleWorldObjects([&, this](WorldObject* object)
            {
                if (sent >= g_config.maxCount)
                    return;

                uint32 status = DIALOG_STATUS_NONE;

                if (object->IsCreature())
                {
                    Creature* giver = object->ToCreature();
                    if (!giver || giver->IsHostileTo(player))
                        return;
                    if (!giver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                        return;
                    status = player->GetQuestDialogStatus(giver);
                }
                else if (object->IsGameObject())
                {
                    GameObject* giver = object->ToGameObject();
                    if (!giver || !giver->GetGOInfo() ||
                        giver->GetGOInfo()->type != GAMEOBJECT_TYPE_QUESTGIVER)
                        return;
                    status = player->GetQuestDialogStatus(giver);
                }
                else
                {
                    return;
                }

                if (!IsActiveQuestStatus(status))
                    return;

                AppendEntry(body, object->GetGUID(), status);
                ++sent;
            });

            // "S:" and nothing after it is the honest answer -- the player can see no giver at all,
            // and the client clears its set on it rather than keeping a stale one.
            WorldPacket data;
            std::string message = std::string("WXLQ\t") + body;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
            player->SendDirectMessage(&data);
        }

        uint32 elapsed_ = 0;
    };
}

void AddSC_ManaQuestOutline()
{
    new ManaQuestOutlineWorld();
}