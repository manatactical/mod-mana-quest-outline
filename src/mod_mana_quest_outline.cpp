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
 *   The body always starts with "S:" and there is no request half: the module re-resolves a player's
 *   visible set when they move, resends it whenever it differs from the last one sent, and re-resends
 *   it unconditionally on a slow timer so a change no movement reports (a quest turned in, a level or
 *   reputation threshold crossed, an NPC phasing in) still arrives -- so a dropped sync is corrected
 *   without any client-side request/response state. A zone change or a teleport resolves immediately.
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
 * (DIALOG_STATUS_UNAVAILABLE and DIALOG_STATUS_NONE are dropped, and INCOMPLETE with them). The
 * _LOW_LEVEL_ variants go too, so a giver whose every quest is far below the player is not outlined.
 * That also keeps the body small, because the client outlines everything in it.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "QuestDef.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace
{
    // The statuses worth sending: a quest can be taken, or a finished one handed in. LABEL_* on the
    // client side mirror these, and the client's own minimap uses the same numbering.
    //
    // The three _LOW_LEVEL_ variants are left out, so a giver whose every quest is far below the
    // player gets no outline. GetQuestDialogStatus keeps the highest status over all the giver's
    // quests, so 2/3/4 only survives here when there is no normal quest to offer instead.
    bool IsActiveQuestStatus(uint32 status)
    {
        switch (status)
        {
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
        bool     enabled        = true;
        uint32   pollIntervalMs = 250;  // how often the movement-driven poll may re-resolve the set
        uint32   idleIntervalMs = 3000; // unconditional safety resend, so a missed change self-corrects
        uint32   maxCount       = 48;   // givers per packet, so one crowded hub cannot split a body
        float    moveDistance   = 5.0f; // moved this far since the last resolve -> re-resolve
    };

    Config g_config;

    // Per-player send state, keyed by GUID: the set last sent, so an unchanged set is never resent,
    // and the position last resolved, so a walking player is re-resolved on movement rather than on a
    // timer.
    struct SendState
    {
        std::string body;
        float       x           = 0.0f;
        float       y           = 0.0f;
        uint32      lastPollMs  = 0; // ms of the last movement-driven resolve
        uint32      lastIdleMs  = 0; // ms of the last unconditional resend
    };

    std::unordered_map<ObjectGuid, SendState> g_sent;

    // The clock, refreshed once per world tick so N players cost one getMSTime call rather than one
    // each. The zero initial value is deliberate: it makes the first update of a freshly seen player
    // resolve and send immediately instead of waiting out the interval.
    uint32 g_nowMs = 0;

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

    // One script, three hook surfaces. The world tick keeps the clock; the poll is the player's own, so
    // it only runs for players that actually exist; the zone-change hook covers the case the movement
    // poll's deliberate window would otherwise conceal: a teleport or a flight into a new hub.
    class ManaQuestOutlineScript final : public WorldScript, public PlayerScript
    {
    public:
        ManaQuestOutlineScript()
            : WorldScript("ManaQuestOutlineWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }),
              PlayerScript("ManaQuestOutlinePlayer", { PLAYERHOOK_ON_BEFORE_UPDATE, PLAYERHOOK_ON_UPDATE_ZONE,
                                                       PLAYERHOOK_ON_LOGOUT }) {}

        // The only per-world-tick work here: one clock read for every player update that follows.
        void OnUpdate(uint32 /*diff*/) override
        {
            g_nowMs = getMSTime();
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            g_config.enabled        = sConfigMgr->GetOption<bool>("ManaQuestOutline.Enable", true);
            g_config.pollIntervalMs = sConfigMgr->GetOption<uint32>("ManaQuestOutline.PollIntervalMs", 250);
            g_config.idleIntervalMs = sConfigMgr->GetOption<uint32>("ManaQuestOutline.IdleIntervalMs", 3000);
            g_config.maxCount       = sConfigMgr->GetOption<uint32>("ManaQuestOutline.MaxPerSync", 48);
            g_config.moveDistance   = sConfigMgr->GetOption<float>("ManaQuestOutline.MoveDistance", 5.0f);

            // The poll is what bounds the snappiness; below the tick rate it buys nothing, and the
            // idle resend must stay the slower of the two or it is not a safety net at all.
            if (g_config.pollIntervalMs < 100)
                g_config.pollIntervalMs = 100;
            if (g_config.idleIntervalMs < g_config.pollIntervalMs)
                g_config.idleIntervalMs = g_config.pollIntervalMs;
            if (!g_config.maxCount)
                g_config.maxCount = 1;
            if (g_config.moveDistance < 0.0f)
                g_config.moveDistance = 0.0f;
        }

        // Change-driven, not timer-driven. An idle player costs two ms subtractions and two float
        // compares; a moving player is re-resolved at most once per PollIntervalMs, and only once it
        // has covered MoveDistance. The idle resend is the safety net for the changes no movement
        // reports: a quest turning in, a reputation or level threshold crossed.
        void OnPlayerBeforeUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!g_config.enabled || !player)
                return;

            SendState& state   = g_sent[player->GetGUID()];
            const bool idleDue = g_nowMs - state.lastIdleMs >= g_config.idleIntervalMs;
            bool       moved   = false;
            if (!idleDue)
            {
                const float dx = player->GetPositionX() - state.x;
                const float dy = player->GetPositionY() - state.y;
                moved = (dx * dx + dy * dy) >= (g_config.moveDistance * g_config.moveDistance);
            }

            if (!idleDue && (!moved || g_nowMs - state.lastPollMs < g_config.pollIntervalMs))
                return;

            state.lastPollMs = g_nowMs;
            if (idleDue)
                state.lastIdleMs = g_nowMs;
            Resolve(player, state);
        }

        // A teleport or a zoning flight lands the player in a new hub; do not make them wait out the
        // poll's deliberate window for it.
        void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
        {
            if (!g_config.enabled || !player)
                return;
            SendState& state = g_sent[player->GetGUID()];
            Resolve(player, state);
            state.lastPollMs = g_nowMs;
            state.lastIdleMs = g_nowMs;
        }

        // Without this the per-guid state would accumulate over the module's uptime; the next login of
        // the same character starts from a zeroed state and so sends immediately, which is wanted.
        void OnPlayerLogout(Player* player) override
        {
            if (player)
                g_sent.erase(player->GetGUID());
        }

    private:

        // Mirrors Player::SendQuestGiverStatusMultiple, minus the gossip/session bookkeeping: same
        // visibility walk, same two kinds of giver, same status call, so the client sees exactly the
        // set the minimap was already built from.
        //
        // The walk is a full first pass over the visible objects on a movement poll, which is the cost
        // that matters: the timer version did it for every player, always. The send itself is skipped
        // when the set is byte-identical to the last one sent.
        void Resolve(Player* player, SendState& state)
        {
            state.x = player->GetPositionX();
            state.y = player->GetPositionY();

            std::string body = "S:";
            uint32      sent = 0;

            player->DoForAllVisibleWorldObjects([&](WorldObject* object)
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

            // Nothing visible changed -> nothing to send. The client keeps the set it already has.
            if (body == state.body)
                return;
            state.body = body;

            // "S:" and nothing after it is the honest answer -- the player can see no giver at all,
            // and the client clears its set on it rather than keeping a stale one.
            WorldPacket data;
            std::string message = std::string("WXLQ\t") + body;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
            player->SendDirectMessage(&data);
        }
    };
}

void AddSC_ManaQuestOutline()
{
    new ManaQuestOutlineScript();
}