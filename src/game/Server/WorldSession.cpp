/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup u2w
*/

#include <zlib.h>

#include "Server/WorldSocket.h"                                    // must be first to make ACE happy with ACE includes in it
#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Server/Opcodes.h"
#include "WorldPacket.h"
#include "Server/WorldSession.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"
#include "World/World.h"
#include "BattleGround/BattleGroundMgr.h"
#include "Social/SocialMgr.h"
#include "Auth/HMACSHA1.h"
#include "GMTickets/GMTicketMgr.h"
#include "Loot/LootMgr.h"

#include <boost/asio/ip/address_v4.hpp>

#include <mutex>
#include <deque>
#include <cstdarg>
#include <iostream>

#ifdef BUILD_PLAYERBOT
#include "PlayerBot/Base/PlayerbotMgr.h"
#include "PlayerBot/Base/PlayerbotAI.h"
#endif

#ifdef ENABLE_PLAYERBOTS
#include "playerbot.h"
#endif

// select opcodes appropriate for processing in Map::Update context for current session state
static bool MapSessionFilterHelper(WorldSession* session, OpcodeHandler const& opHandle)
{
    // we do not process thread-unsafe packets
    if (opHandle.packetProcessing == PROCESS_THREADUNSAFE)
        return false;

    // we do not process not loggined player packets
    Player* plr = session->GetPlayer();
    if (!plr)
        return false;

    // in Map::Update() we do not process packets where player is not in world!
    return plr->IsInWorld();
}

bool MapSessionFilter::Process(WorldPacket const& packet) const
{
    OpcodeHandler const& opHandle = opcodeTable[packet.GetOpcode()];
    if (opHandle.packetProcessing == PROCESS_INPLACE)
        return true;

    // let's check if our opcode can be really processed in Map::Update()
    return MapSessionFilterHelper(m_pSession, opHandle);
}

// we should process ALL packets when player is not in world/logged in
// OR packet handler is not thread-safe!
bool WorldSessionFilter::Process(WorldPacket const& packet) const
{
    OpcodeHandler const& opHandle = opcodeTable[packet.GetOpcode()];
    // check if packet handler is supposed to be safe
    if (opHandle.packetProcessing == PROCESS_INPLACE)
        return true;

    // let's check if our opcode can't be processed in Map::Update()
    return !MapSessionFilterHelper(m_pSession, opHandle);
}

/// WorldSession constructor
WorldSession::WorldSession(uint32 id, WorldSocket* sock, AccountTypes sec, uint8 expansion, time_t mute_time, LocaleConstant locale) :
    m_muteTime(mute_time), m_GUIDLow(0), _player(nullptr), m_Socket(sock ? sock->shared<WorldSocket>() : nullptr), _security(sec), _accountId(id), m_expansion(expansion), m_orderCounter(0),
    _logoutTime(0), m_inQueue(false), m_playerLoading(false), m_playerLogout(false), m_playerRecentlyLogout(false), m_playerSave(true),
    m_sessionDbcLocale(sWorld.GetAvailableDbcLocale(locale)), m_sessionDbLocaleIndex(sObjectMgr.GetStorageLocaleIndexFor(locale)),
    m_latency(0), m_clientTimeDelay(0), m_tutorialState(TUTORIALDATA_UNCHANGED), m_sessionState(WORLD_SESSION_STATE_CREATED),
    m_timeSyncClockDeltaQueue(6), m_timeSyncClockDelta(0), m_pendingTimeSyncRequests(), m_timeSyncNextCounter(0), m_timeSyncTimer(0),
    m_requestSocket(nullptr) {}

/// WorldSession destructor
WorldSession::~WorldSession()
{
    ///- unload player if not unloaded
    if (_player)
        LogoutPlayer();

    // marks this session as finalized in the socket which references (BUT DOES NOT OWN) it.
    // this lets the socket handling code know that the socket can be safely deleted
    if (m_Socket)
    {
        if (!m_Socket->IsClosed())
            m_Socket->Close();

        m_Socket->FinalizeSession();
    }
}

void WorldSession::SetOffline()
{
    if (_player)
    {
        // friend status
        sSocialMgr.SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetObjectGuid(), true);
        LogoutRequest(time(nullptr));
    }

    // be sure its closed (may occur when second session is opened)
    if (m_Socket)
    {
        if (!m_Socket->IsClosed())
            m_Socket->Close();

        // unexpected socket close, let it be deleted
        m_Socket->FinalizeSession();
        m_Socket = nullptr;
    }

    m_sessionState = WORLD_SESSION_STATE_OFFLINE;
}

void WorldSession::SetOnline()
{
    if (_player && m_Socket && !m_Socket->IsClosed())
    {
        m_sessionState = WORLD_SESSION_STATE_READY;
        m_kickTime = 0;
    }
}

void WorldSession::SetInCharSelection()
{
    m_sessionState = WORLD_SESSION_STATE_CHAR_SELECTION;
    m_kickTime = time(nullptr) + 15 * 60;
}

bool WorldSession::RequestNewSocket(WorldSocket* socket)
{
    std::lock_guard<std::mutex> guard(m_recvQueueLock);
    if (m_requestSocket)
        return false;

    m_requestSocket = socket->shared<WorldSocket>();
    m_sessionState = WORLD_SESSION_STATE_CREATED;
    return true;
}

void WorldSession::SizeError(WorldPacket const& packet, uint32 size) const
{
    sLog.outError("Client (account %u) send packet %s (%u) with size " SIZEFMTD " but expected %u (attempt crash server?), skipped",
                  GetAccountId(), packet.GetOpcodeName(), packet.GetOpcode(), packet.size(), size);
}

/// Get the player name
char const* WorldSession::GetPlayerName() const
{
    return GetPlayer() ? GetPlayer()->GetName() : "<none>";
}

void WorldSession::SetExpansion(uint8 expansion)
{
    m_expansion = expansion;
    if (_player)
        _player->OnExpansionChange();
    SendAuthOk(); // this is a hack but does what we need - resets expansion setting in client
}

/// Send a packet to the client
void WorldSession::SendPacket(WorldPacket const& packet) const
{
#ifdef BUILD_PLAYERBOT
    // Send packet to bot AI
    if (GetPlayer())
    {
        if (GetPlayer()->GetPlayerbotAI())
            GetPlayer()->GetPlayerbotAI()->HandleBotOutgoingPacket(packet);
        else if (GetPlayer()->GetPlayerbotMgr())
            GetPlayer()->GetPlayerbotMgr()->HandleMasterOutgoingPacket(packet);
    }
#endif

#ifdef ENABLE_PLAYERBOTS
    if (GetPlayer()) {
        if (GetPlayer()->GetPlayerbotAI())
            GetPlayer()->GetPlayerbotAI()->HandleBotOutgoingPacket(packet);
        else if (GetPlayer()->GetPlayerbotMgr())
            GetPlayer()->GetPlayerbotMgr()->HandleMasterOutgoingPacket(packet);
    }
#endif

    if (!m_Socket || m_Socket->IsClosed())
        return;

#ifdef MANGOS_DEBUG

    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(nullptr);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(nullptr);

    if ((cur_time - lastTime) < 60)
    {
        sendPacketCount += 1;
        sendPacketBytes += packet.size();

        sendLastPacketCount += 1;
        sendLastPacketBytes += packet.size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        DETAIL_LOG("Send all time packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f time: %u", sendPacketCount, sendPacketBytes, float(sendPacketCount) / fullTime, float(sendPacketBytes) / fullTime, uint32(fullTime));
        DETAIL_LOG("Send last min packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f", sendLastPacketCount, sendLastPacketBytes, float(sendLastPacketCount) / minTime, float(sendLastPacketBytes) / minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet.wpos();               // wpos is real written size
    }

#endif                                                  // !MANGOS_DEBUG

    m_Socket->SendPacket(packet);
}

/// Add an incoming packet to the queue
void WorldSession::QueuePacket(std::unique_ptr<WorldPacket> new_packet)
{
    sWorld.IncrementOpcodeCounter(new_packet->GetOpcode());
    OpcodeHandler const& opHandle = opcodeTable[new_packet->GetOpcode()];
    if (opHandle.packetProcessing == PROCESS_IMMEDIATE)
    {
        (this->*opHandle.handler)(*new_packet);
        if (new_packet->rpos() < new_packet->wpos() && sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
            LogUnprocessedTail(*new_packet);
        return;
    }

    if (opHandle.packetProcessing == PROCESS_MAP_THREAD)
    {
        std::lock_guard<std::mutex> guard(m_recvQueueMapLock);
        m_recvQueueMap.push_back(std::move(new_packet));
    }
    else
    {
        std::lock_guard<std::mutex> guard(m_recvQueueLock);
        m_recvQueue.push_back(std::move(new_packet));
    }
}

void WorldSession::DeleteMovementPackets()
{
    std::lock_guard<std::mutex> guard(m_recvQueueMapLock);
    for (auto itr = m_recvQueueMap.begin(); itr != m_recvQueueMap.end();)
    {
        switch ((*itr)->GetOpcode())
        {
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_HEARTBEAT:
            {
                itr = m_recvQueueMap.erase(itr);
                break;
            }
            default:
                ++itr;
                break;
        }
    }
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnexpectedOpcode(WorldPacket const& packet, const char* reason) const
{
    sLog.outError("SESSION: received unexpected opcode %s (0x%.4X) %s",
                  packet.GetOpcodeName(),
                  packet.GetOpcode(),
                  reason);
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnprocessedTail(WorldPacket& packet) const
{
    sLog.outError("SESSION: opcode %s (0x%.4X) have unprocessed tail data (read stop at " SIZEFMTD " from " SIZEFMTD ")",
                  packet.GetOpcodeName(),
                  packet.GetOpcode(),
                  packet.rpos(), packet.wpos());
}

void WorldSession::ProcessByteBufferException(WorldPacket const& packet)
{
    sLog.outError("WorldSession::Update ByteBufferException occured while parsing a packet (opcode: %u) from client %s, accountid=%i.",
        packet.GetOpcode(), GetRemoteAddress().c_str(), GetAccountId());
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
    {
        DEBUG_LOG("Dumping error causing packet:");
        packet.hexlike();
    }

    if (sWorld.getConfig(CONFIG_BOOL_KICK_PLAYER_ON_BAD_PACKET))
    {
        DETAIL_LOG("Disconnecting session [account id %u / address %s] for badly formatted packet.",
            GetAccountId(), GetRemoteAddress().c_str());
        ObjectGuid guid = _player->GetObjectGuid();
        GetMessager().AddMessage([guid](WorldSession* world) -> void
        {
            ObjectAccessor::KickPlayer(guid);
        });
    }
}

/// Update the WorldSession (triggered by World update)
bool WorldSession::Update(uint32 diff)
{
    GetMessager().Execute(this);

    std::deque<std::unique_ptr<WorldPacket>> recvQueueCopy;
    {
        std::lock_guard<std::mutex> guard(m_recvQueueLock);
        std::swap(recvQueueCopy, m_recvQueue);
    }

    ///- Retrieve packets from the receive queue and call the appropriate handlers
    /// not process packets if socket already closed
    while (m_Socket && !m_Socket->IsClosed() && !recvQueueCopy.empty())
    {
        // sLog.outError("MOEP: %s (0x%.4X)", packet->GetOpcodeName(), packet->GetOpcode());

        auto const packet = std::move(recvQueueCopy.front());
        recvQueueCopy.pop_front();

        OpcodeHandler const& opHandle = opcodeTable[packet->GetOpcode()];
        try
        {
            switch (opHandle.status)
            {
                case STATUS_LOGGEDIN:
                    if (!_player)
                    {
                        // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                        if (!m_playerRecentlyLogout)
                            LogUnexpectedOpcode(*packet, "the player has not logged in yet");
                    }
                    else if (_player->IsInWorld())
                        ExecuteOpcode(opHandle, *packet);

                    // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer

#ifdef BUILD_PLAYERBOT
                    if (_player && _player->GetPlayerbotMgr())
                        _player->GetPlayerbotMgr()->HandleMasterIncomingPacket(*packet);
#endif
#ifdef ENABLE_PLAYERBOTS
                    if (_player && _player->GetPlayerbotMgr())
                        _player->GetPlayerbotMgr()->HandleMasterIncomingPacket(*packet);
#endif
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGEDOUT:
                    if (!_player && !m_playerRecentlyLogout)
                    {
                        LogUnexpectedOpcode(*packet, "the player has not logged in yet and not recently logout");
                    }
                    else
                        // not expected _player or must checked in packet hanlder
                        ExecuteOpcode(opHandle, *packet);
                    break;
                case STATUS_TRANSFER:
                    if (!_player)
                        LogUnexpectedOpcode(*packet, "the player has not logged in yet");
                    else if (_player->IsInWorld())
                        LogUnexpectedOpcode(*packet, "the player is still in world");
                    else
                        ExecuteOpcode(opHandle, *packet);
                    break;
                case STATUS_AUTHED:
                    // prevent cheating with skip queue wait
                    if (m_inQueue)
                    {
                        LogUnexpectedOpcode(*packet, "the player not pass queue yet");
                        break;
                    }

                    // single from authed time opcodes send in to after logout time
                    // and before other STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT opcodes.
                    if (packet->GetOpcode() != CMSG_SET_ACTIVE_VOICE_CHANNEL)
                        m_playerRecentlyLogout = false;

                    ExecuteOpcode(opHandle, *packet);
                    break;
                case STATUS_NEVER:
                    sLog.outError("SESSION: received not allowed opcode %s (0x%.4X)",
                                  packet->GetOpcodeName(),
                                  packet->GetOpcode());
                    break;
                case STATUS_UNHANDLED:
                    DEBUG_LOG("SESSION: received not handled opcode %s (0x%.4X)",
                              packet->GetOpcodeName(),
                              packet->GetOpcode());
                    break;
                default:
                    sLog.outError("SESSION: received wrong-status-req opcode %s (0x%.4X)",
                                  packet->GetOpcodeName(),
                                  packet->GetOpcode());
                    break;
            }
        }
        catch (ByteBufferException&)
        {
            ProcessByteBufferException(*packet);
        }
    }

#ifdef BUILD_PLAYERBOT
    // Process player bot packets
    // The PlayerbotAI class adds to the packet queue to simulate a real player
    // since Playerbots are known to the World obj only by its master's WorldSession object
    // we need to process all master's bot's packets.
    if (GetPlayer() && GetPlayer()->GetPlayerbotMgr())
    {
        for (PlayerBotMap::const_iterator itr = GetPlayer()->GetPlayerbotMgr()->GetPlayerBotsBegin();
                itr != GetPlayer()->GetPlayerbotMgr()->GetPlayerBotsEnd(); ++itr)
        {
            Player* const botPlayer = itr->second;
            WorldSession* const pBotWorldSession = botPlayer->GetSession();
            while(!pBotWorldSession->m_recvQueue.empty())
            {
                auto const botpacket = std::move(pBotWorldSession->m_recvQueue.front());
                pBotWorldSession->m_recvQueue.pop_front();

                OpcodeHandler const& opHandle = opcodeTable[botpacket->GetOpcode()];
                pBotWorldSession->ExecuteOpcode(opHandle, *botpacket);
            }
        }
        GetPlayer()->GetPlayerbotMgr()->RemoveBots();
    }
#endif
#ifdef ENABLE_PLAYERBOTS
    if (GetPlayer() && GetPlayer()->GetPlayerbotMgr())
        GetPlayer()->GetPlayerbotMgr()->UpdateSessions(0);
#endif

    // check if we are safe to proceed with logout
    // logout procedure should happen only in World::UpdateSessions() method!!!
    switch (m_sessionState)
    {
        case WORLD_SESSION_STATE_CREATED:
        {
            if (m_requestSocket)
            {
                if (!IsOffline())
                    SetOffline();

                m_Socket = m_requestSocket;
                m_requestSocket = nullptr;
                sLog.outDetail("New Session key %s", m_Socket->GetSessionKey().AsHexStr());
                SendAuthOk();
            }
            else
            {
                if (m_inQueue)
                    SendAuthQueued();
                else
                    SendAuthOk();
            }
            SetInCharSelection();
            return true;
        }
        case WORLD_SESSION_STATE_CHAR_SELECTION:
        {
            // waiting to go online
            // TODO:: Maybe check if have to send queue update?
            if (!m_Socket || (m_Socket && m_Socket->IsClosed()))
            {
                // directly remove this session
                return false;
            }

            if (ShouldLogOut(time(nullptr)) && !m_playerLoading)   // check if delayed logout is fired
                LogoutPlayer();

            if (m_kickTime && m_kickTime <= time(nullptr))
                KickPlayer(true);

            return true;
        }
        case WORLD_SESSION_STATE_READY:
        {
            if (m_Socket && m_Socket->IsClosed())
            {
                if (!_player)
                    return false;

                // give the opportunity for this player to reconnect within 20 sec
                SetOffline();
            }
            else if (ShouldLogOut(time(nullptr)) && !m_playerLoading)   // check if delayed logout is fired
                LogoutPlayer();

            return true;
        }

        case WORLD_SESSION_STATE_OFFLINE:
        {
            if (ShouldDisconnect(time(nullptr)))   // check if delayed logout is fired
            {
                LogoutPlayer();
                if (!m_requestSocket && (!m_Socket || m_Socket->IsClosed()))
                    return false;
            }

            return true;
        }
        default:
            break;
    }

    return true;
}

#ifdef ENABLE_PLAYERBOTS
void WorldSession::HandleBotPackets()
{
    while (!m_recvQueue.empty())
    {
        auto const packet = std::move(m_recvQueue.front());
        m_recvQueue.pop_front();
        OpcodeHandler const& opHandle = opcodeTable[packet->GetOpcode()];
        (this->*opHandle.handler)(*packet);
    }
}
#endif

void WorldSession::UpdateMap(uint32 diff)
{
    std::deque<std::unique_ptr<WorldPacket>> recvQueueMapCopy;
    {
        std::lock_guard<std::mutex> guard(m_recvQueueMapLock);
        std::swap(recvQueueMapCopy, m_recvQueueMap);
    }

    for (size_t i = 0; m_Socket && !m_Socket->IsClosed() && i < recvQueueMapCopy.size(); ++i)
    {
        auto const packet = std::move(recvQueueMapCopy.front());
        recvQueueMapCopy.pop_front();

        OpcodeHandler const& opHandle = opcodeTable[packet->GetOpcode()];

        try
        {
            if (opHandle.status == STATUS_LOGGEDIN)
            {
                ExecuteOpcode(opHandle, *packet);
            }
        }
        catch (ByteBufferException&)
        {
            ProcessByteBufferException(*packet);
        }
    }
}

/// %Log the player out
void WorldSession::LogoutPlayer()
{
    // finish pending transfers before starting the logout
    while (_player && _player->IsBeingTeleportedFar())
        HandleMoveWorldportAckOpcode();

    m_playerLogout = true;

    if (_player)
    {
#ifdef BUILD_PLAYERBOT
        // Log out all player bots owned by this toon
        if (_player->GetPlayerbotMgr())
            _player->GetPlayerbotMgr()->LogoutAllBots(true);
#endif
#ifdef ENABLE_PLAYERBOTS
        sLog.outChar("Account: %d (IP: %s) Logout Character:[%s] (guid: %u)", GetAccountId(), m_Socket ? GetRemoteAddress().c_str() : "bot", _player->GetName(), _player->GetGUIDLow());
#else
        sLog.outChar("Account: %d (IP: %s) Logout Character:[%s] (guid: %u)", GetAccountId(), GetRemoteAddress().c_str(), _player->GetName(), _player->GetGUIDLow());
#endif

        if (Loot* loot = sLootMgr.GetLoot(_player))
            loot->Release(_player);

#ifdef ENABLE_PLAYERBOTS
        if (_player->GetPlayerbotMgr() && (!_player->GetPlayerbotAI() || _player->GetPlayerbotAI()->IsRealPlayer()))
            _player->GetPlayerbotMgr()->LogoutAllBots();
        sRandomPlayerbotMgr.OnPlayerLogout(_player);
#endif

        if (_player->GetDeathTimer())
        {
            _player->getHostileRefManager().deleteReferences();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        {
            // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
            _player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            //_player->SetDeathPvP(*); set at SPELL_AURA_SPIRIT_OF_REDEMPTION apply time
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (_player->IsInCombat())
            _player->CombatStopWithPets(true, true);

        // drop a flag if player is carrying it
        if (BattleGround* bg = _player->GetBattleGround())
            bg->EventPlayerLoggedOut(_player);

        ///- Teleport to home if the player is in an invalid instance
        if (!_player->m_InstanceValid && !_player->IsGameMaster())
        {
            _player->TeleportToHomebind();
            // this is a bad place to call for far teleport because we need player to be in world for successful logout
            // maybe we should implement delayed far teleport logout?
        }

        // FG: finish pending transfers after starting the logout
        // this should fix players beeing able to logout and login back with full hp at death position
        while (_player->IsBeingTeleportedFar())
            HandleMoveWorldportAckOpcode();

        for (int i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            if (BattleGroundQueueTypeId bgQueueTypeId = _player->GetBattleGroundQueueTypeId(i))
            {
                _player->RemoveBattleGroundQueueId(bgQueueTypeId);
                sBattleGroundMgr.m_battleGroundQueues[ bgQueueTypeId ].RemovePlayer(_player->GetObjectGuid(), true);
            }
        }

        ///- Reset the online field in the account table
        // no point resetting online in character table here as Player::SaveToDB() will set it to 1 since player has not been removed from world at this stage
        // No SQL injection as AccountID is uint32
        static SqlStatementID id;

#ifdef BUILD_PLAYERBOT
        if (!_player->GetPlayerbotAI())
        {
            // Unmodded core code below
            SqlStatement stmt = LoginDatabase.CreateStatement(id, "UPDATE account SET active_realm_id = ? WHERE id = ?");
            stmt.PExecute(uint32(0), GetAccountId());
        }
#else
        SqlStatement stmt = LoginDatabase.CreateStatement(id, "UPDATE account SET active_realm_id = ? WHERE id = ?");
        stmt.PExecute(uint32(0), GetAccountId());
#endif

        ///- If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        if (Guild* guild = sGuildMgr.GetGuildById(_player->GetGuildId()))
        {
            if (MemberSlot* slot = guild->GetMemberSlot(_player->GetObjectGuid()))
            {
                slot->SetMemberStats(_player);
                slot->UpdateLogoutTime();
            }

            guild->BroadcastEvent(GE_SIGNED_OFF, _player->GetObjectGuid(), _player->GetName());
        }

        ///- Remove pet
        _player->RemovePet(PET_SAVE_AS_CURRENT);

        ///- empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if (m_playerSave)
            _player->SaveToDB();

        ///- Leave all channels before player delete...
        _player->CleanupChannels();

        // LFG cleanup
        sLFGMgr.Leave(GetPlayer());

//#ifndef ENABLE_PLAYERBOTS
        ///- If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        // remove player from the group if he is:
        // a) in group; b) not in raid group; c) logging out normally (not being kicked or disconnected)
        if (_player->GetGroup() && !_player->GetGroup()->isRaidGroup() && m_Socket && !m_Socket->IsClosed())
            _player->RemoveFromGroup();
//#endif

        ///- Send update to group
        if (Group* group = _player->GetGroup())
            group->UpdatePlayerOnlineStatus(_player, false);

        ///- Broadcast a logout message to the player's friends
        if (_player->GetSocial()) // might not yet be initialized
        {
            sSocialMgr.SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetObjectGuid(), true);
            sSocialMgr.RemovePlayerSocial(_player->GetGUIDLow());
        }

        // GM ticket notification
        sTicketMgr.OnPlayerOnlineState(*_player, false);

#ifdef BUILD_PLAYERBOT
        // Remember player GUID for update SQL below
        uint32 guid = _player->GetGUIDLow();
#endif

#ifdef ENABLE_PLAYERBOTS
        // Remember player GUID for update SQL below
        uint32 guid = _player->GetGUIDLow();
#endif

        ///- Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes
        if (_player->IsInWorld())
        {
            Map* _map = _player->GetMap();
            _map->Remove(_player, true);
        }
        else
        {
            _player->CleanupsBeforeDelete();
            Map::DeleteFromWorld(_player);
        }

        SetPlayer(nullptr, 0);                                    // deleted in Remove/DeleteFromWorld call

        ///- Send the 'logout complete' packet to the client
        WorldPacket data(SMSG_LOGOUT_COMPLETE, 0);
        SendPacket(data);

        ///- Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        // No SQL injection as AccountId is uint32

        static SqlStatementID updChars;

#ifdef BUILD_PLAYERBOT
        // Set for only character instead of accountid
        // Different characters can be alive as bots
        SqlStatement stmt = CharacterDatabase.CreateStatement(updChars, "UPDATE characters SET online = 0 WHERE guid = ?");
        stmt.PExecute(guid);
#else
#ifdef ENABLE_PLAYERBOTS
        // Set for only character instead of accountid
        // Different characters can be alive as bots
        stmt = CharacterDatabase.CreateStatement(updChars, "UPDATE characters SET online = 0 WHERE guid = ?");
        stmt.PExecute(guid);
#else
        ///- Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        // No SQL injection as AccountId is uint32
        stmt = CharacterDatabase.CreateStatement(updChars, "UPDATE characters SET online = 0 WHERE account = ?");
        stmt.PExecute(GetAccountId());
#endif
#endif

        DEBUG_LOG("SESSION: Sent SMSG_LOGOUT_COMPLETE Message");
    }

    m_playerLogout = false;
    m_playerRecentlyLogout = true;

    SetInCharSelection();

    LogoutRequest(0);
}

/// Kick a player out of the World
void WorldSession::KickPlayer(bool save, bool inPlace)
{
    m_playerSave = save;
    if (inPlace)
    {
        LogoutPlayer();
        return;
    }

#ifdef BUILD_PLAYERBOT
    if (!_player)
        return;

    if (_player->GetPlayerbotAI())
    {
        auto master = _player->GetPlayerbotAI()->GetMaster();
        auto botMgr = master->GetPlayerbotMgr();
        if (botMgr)
            botMgr->LogoutPlayerBot(_player->GetObjectGuid());
    }
    else
        LogoutRequest(time(nullptr) - 20, false);
#else
    LogoutRequest(time(nullptr) - 20, false);
#endif
}

void WorldSession::SendExpectedSpamRecords()
{
    std::vector<std::string> spamRecords = sWorld.GetSpamRecords();

    WorldPacket data(SMSG_EXPECTED_SPAM_RECORDS, 4);

    data << (uint32) spamRecords.size();
    for (const std::string& record : spamRecords)
        data << record;

    SendPacket(data);
}

void WorldSession::SendMotd()
{
    std::vector<std::string> lines;
    std::string token;

    std::string motd = sWorld.GetMotd();
    std::istringstream ss(motd);

    while (std::getline(ss, token, '@'))
        lines.push_back(token);

    WorldPacket data(SMSG_MOTD, 4);
    data << (uint32) lines.size();

    for (const std::string& line : lines)
        data << line;

    SendPacket(data);

    DEBUG_LOG("WORLD: Sent motd (SMSG_MOTD)");
}

void WorldSession::SendOfflineNameQueryResponses()
{
    m_offlineNameQueries.clear();

    for (auto& response : m_offlineNameResponses)
        SendNameQueryResponse(response);

    m_offlineNameResponses.clear();
}

void WorldSession::SendAreaTriggerMessage(const char* Text, ...) const
{
    va_list ap;
    char szStr [1024];
    szStr[0] = '\0';

    va_start(ap, Text);
    vsnprintf(szStr, 1024, Text, ap);
    va_end(ap);

    uint32 length = strlen(szStr) + 1;
    WorldPacket data(SMSG_AREA_TRIGGER_MESSAGE, 4 + length);
    data << length;
    data << szStr;
    SendPacket(data);
}

void WorldSession::SendNotification(const char* format, ...) const
{
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr) + 1));
        data << szStr;
        SendPacket(data);
    }
}

void WorldSession::SendNotification(int32 string_id, ...) const
{
    char const* format = GetMangosString(string_id);
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr) + 1));
        data << szStr;
        SendPacket(data);
    }
}

void WorldSession::SendSetPhaseShift(uint32 PhaseShift) const
{
    WorldPacket data(SMSG_SET_PHASE_SHIFT, 4);
    data << uint32(PhaseShift);
    SendPacket(data);
}

const char* WorldSession::GetMangosString(int32 entry) const
{
    return sObjectMgr.GetMangosString(entry, GetSessionDbLocaleIndex());
}

void WorldSession::Handle_NULL(WorldPacket& recvPacket)
{
    DEBUG_LOG("SESSION: received unimplemented opcode %s (0x%.4X)",
              recvPacket.GetOpcodeName(),
              recvPacket.GetOpcode());
}

void WorldSession::Handle_EarlyProccess(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received opcode %s (0x%.4X) that must be processed in WorldSocket::OnRead",
                  recvPacket.GetOpcodeName(),
                  recvPacket.GetOpcode());
}

void WorldSession::Handle_ServerSide(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received server-side opcode %s (0x%.4X)",
                  recvPacket.GetOpcodeName(),
                  recvPacket.GetOpcode());
}

void WorldSession::Handle_Deprecated(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received deprecated opcode %s (0x%.4X)",
                  recvPacket.GetOpcodeName(),
                  recvPacket.GetOpcode());
}

void WorldSession::SendAuthWaitQue(uint32 position) const
{
    if (position == 0)
        SendAuthOk();
    else
        SendAuthQueued();
}

void WorldSession::LoadGlobalAccountData()
{
    LoadAccountData(
        CharacterDatabase.PQuery("SELECT type, time, data FROM account_data WHERE account='%u'", GetAccountId()),
        GLOBAL_CACHE_MASK
    );
}

void WorldSession::LoadAccountData(QueryResult* result, uint32 mask)
{
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            m_accountData[i] = AccountData();

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        uint32 type = fields[0].GetUInt32();
        if (type >= NUM_ACCOUNT_DATA_TYPES)
        {
            sLog.outError("Table `%s` have invalid account data type (%u), ignore.",
                          mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        if ((mask & (1 << type)) == 0)
        {
            sLog.outError("Table `%s` have non appropriate for table  account data type (%u), ignore.",
                          mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        m_accountData[type].Time = time_t(fields[1].GetUInt64());
        m_accountData[type].Data = fields[2].GetCppString();
    }
    while (result->NextRow());

    delete result;
}

void WorldSession::SetAccountData(AccountDataType type, time_t time_, const std::string& data)
{
    if ((1 << type) & GLOBAL_CACHE_MASK)
    {
        uint32 acc = GetAccountId();

        static SqlStatementID delId;
        static SqlStatementID insId;

        CharacterDatabase.BeginTransaction();

        SqlStatement stmt = CharacterDatabase.CreateStatement(delId, "DELETE FROM account_data WHERE account=? AND type=?");
        stmt.PExecute(acc, uint32(type));

        stmt = CharacterDatabase.CreateStatement(insId, "INSERT INTO account_data VALUES (?,?,?,?)");
        stmt.PExecute(acc, uint32(type), uint64(time_), data.c_str());

        CharacterDatabase.CommitTransaction();
    }
    else
    {
        // _player can be nullptr and packet received after logout but m_GUID still store correct guid
        if (!m_GUIDLow)
            return;

        static SqlStatementID delId;
        static SqlStatementID insId;

        CharacterDatabase.BeginTransaction();

        SqlStatement stmt = CharacterDatabase.CreateStatement(delId, "DELETE FROM character_account_data WHERE guid=? AND type=?");
        stmt.PExecute(m_GUIDLow, uint32(type));

        stmt = CharacterDatabase.CreateStatement(insId, "INSERT INTO character_account_data VALUES (?,?,?,?)");
        stmt.PExecute(m_GUIDLow, uint32(type), uint64(time_), data.c_str());

        CharacterDatabase.CommitTransaction();
    }

    m_accountData[type].Time = time_;
    m_accountData[type].Data = data;
}

void WorldSession::SendAccountDataTimes(uint32 mask)
{
    WorldPacket data(SMSG_ACCOUNT_DATA_TIMES, 4 + 1 + 4 + 8 * 4); // changed in WotLK
    data << uint32(time(nullptr));                             // unix time of something
    data << uint8(1);
    data << uint32(mask);                                   // type mask
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            data << uint32(GetAccountData(AccountDataType(i))->Time);// also unix time
    SendPacket(data);
}

void WorldSession::LoadTutorialsData()
{
    for (unsigned int& m_Tutorial : m_Tutorials)
        m_Tutorial = 0;

    QueryResult* result = CharacterDatabase.PQuery("SELECT tut0,tut1,tut2,tut3,tut4,tut5,tut6,tut7 FROM character_tutorial WHERE account = '%u'", GetAccountId());

    if (!result)
    {
        m_tutorialState = TUTORIALDATA_NEW;
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        for (int iI = 0; iI < 8; ++iI)
            m_Tutorials[iI] = fields[iI].GetUInt32();
    }
    while (result->NextRow());

    delete result;

    m_tutorialState = TUTORIALDATA_UNCHANGED;
}

void WorldSession::SendTutorialsData()
{
    WorldPacket data(SMSG_TUTORIAL_FLAGS, 4 * 8);
    for (unsigned int m_Tutorial : m_Tutorials)
        data << m_Tutorial;
    SendPacket(data);
}

void WorldSession::SaveTutorialsData()
{
    static SqlStatementID updTutorial ;
    static SqlStatementID insTutorial ;

    switch (m_tutorialState)
    {
        case TUTORIALDATA_CHANGED:
        {
            SqlStatement stmt = CharacterDatabase.CreateStatement(updTutorial, "UPDATE character_tutorial SET tut0=?, tut1=?, tut2=?, tut3=?, tut4=?, tut5=?, tut6=?, tut7=? WHERE account = ?");
            for (unsigned int m_Tutorial : m_Tutorials)
                stmt.addUInt32(m_Tutorial);

            stmt.addUInt32(GetAccountId());
            stmt.Execute();
        }
        break;

        case TUTORIALDATA_NEW:
        {
            SqlStatement stmt = CharacterDatabase.CreateStatement(insTutorial, "INSERT INTO character_tutorial (account,tut0,tut1,tut2,tut3,tut4,tut5,tut6,tut7) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");

            stmt.addUInt32(GetAccountId());
            for (unsigned int m_Tutorial : m_Tutorials)
                stmt.addUInt32(m_Tutorial);

            stmt.Execute();
        }
        break;
        case TUTORIALDATA_UNCHANGED:
            break;
    }

    m_tutorialState = TUTORIALDATA_UNCHANGED;
}

// Send chat information about aborted transfer (mostly used by Player::SendTransferAbortedByLockstatus())
void WorldSession::SendTransferAborted(uint32 mapid, uint8 reason, uint8 arg) const
{
    WorldPacket data(SMSG_TRANSFER_ABORTED, 4 + 2);
    data << uint32(mapid);
    data << uint8(reason);                                  // transfer abort reason
    switch (reason)
    {
        case TRANSFER_ABORT_INSUF_EXPAN_LVL:
        case TRANSFER_ABORT_DIFFICULTY:
        case TRANSFER_ABORT_UNIQUE_MESSAGE:
            data << uint8(arg);
            break;
    }
    SendPacket(data);
}

void WorldSession::ReadAddonsInfo(WorldPacket& data)
{
    if (data.rpos() + 4 > data.size())
        return;
    uint32 size;
    data >> size;

    if (!size)
        return;

    if (size > 0xFFFFF)
    {
        sLog.outError("WorldSession::ReadAddonsInfo addon info too big, size %u", size);
        return;
    }

    uLongf uSize = size;

    uint32 pos = data.rpos();

    ByteBuffer addonInfo;
    addonInfo.resize(size);

    if (uncompress(const_cast<uint8*>(addonInfo.contents()), &uSize, const_cast<uint8*>(data.contents() + pos), data.size() - pos) == Z_OK)
    {
        uint32 addonsCount;
        addonInfo >> addonsCount;                         // addons count

        for (uint32 i = 0; i < addonsCount; ++i)
        {
            std::string addonName;
            uint8 enabled;
            uint32 crc, unk1;

            // check next addon data format correctness
            if (addonInfo.rpos() + 1 > addonInfo.size())
                return;

            addonInfo >> addonName;

            addonInfo >> enabled >> crc >> unk1;

            DEBUG_LOG("ADDON: Name: %s, Enabled: 0x%x, CRC: 0x%x, Unknown2: 0x%x", addonName.c_str(), enabled, crc, unk1);

            m_addonsList.push_back(AddonInfo(addonName, enabled, crc));
        }

        uint32 unk2;
        addonInfo >> unk2;

        if (addonInfo.rpos() != addonInfo.size())
            DEBUG_LOG("packet under read!");
    }
    else
        sLog.outError("Addon packet uncompress error!");
}

void WorldSession::SendAddonsInfo()
{
    unsigned char tdata[256] =
    {
        0xC3, 0x5B, 0x50, 0x84, 0xB9, 0x3E, 0x32, 0x42, 0x8C, 0xD0, 0xC7, 0x48, 0xFA, 0x0E, 0x5D, 0x54,
        0x5A, 0xA3, 0x0E, 0x14, 0xBA, 0x9E, 0x0D, 0xB9, 0x5D, 0x8B, 0xEE, 0xB6, 0x84, 0x93, 0x45, 0x75,
        0xFF, 0x31, 0xFE, 0x2F, 0x64, 0x3F, 0x3D, 0x6D, 0x07, 0xD9, 0x44, 0x9B, 0x40, 0x85, 0x59, 0x34,
        0x4E, 0x10, 0xE1, 0xE7, 0x43, 0x69, 0xEF, 0x7C, 0x16, 0xFC, 0xB4, 0xED, 0x1B, 0x95, 0x28, 0xA8,
        0x23, 0x76, 0x51, 0x31, 0x57, 0x30, 0x2B, 0x79, 0x08, 0x50, 0x10, 0x1C, 0x4A, 0x1A, 0x2C, 0xC8,
        0x8B, 0x8F, 0x05, 0x2D, 0x22, 0x3D, 0xDB, 0x5A, 0x24, 0x7A, 0x0F, 0x13, 0x50, 0x37, 0x8F, 0x5A,
        0xCC, 0x9E, 0x04, 0x44, 0x0E, 0x87, 0x01, 0xD4, 0xA3, 0x15, 0x94, 0x16, 0x34, 0xC6, 0xC2, 0xC3,
        0xFB, 0x49, 0xFE, 0xE1, 0xF9, 0xDA, 0x8C, 0x50, 0x3C, 0xBE, 0x2C, 0xBB, 0x57, 0xED, 0x46, 0xB9,
        0xAD, 0x8B, 0xC6, 0xDF, 0x0E, 0xD6, 0x0F, 0xBE, 0x80, 0xB3, 0x8B, 0x1E, 0x77, 0xCF, 0xAD, 0x22,
        0xCF, 0xB7, 0x4B, 0xCF, 0xFB, 0xF0, 0x6B, 0x11, 0x45, 0x2D, 0x7A, 0x81, 0x18, 0xF2, 0x92, 0x7E,
        0x98, 0x56, 0x5D, 0x5E, 0x69, 0x72, 0x0A, 0x0D, 0x03, 0x0A, 0x85, 0xA2, 0x85, 0x9C, 0xCB, 0xFB,
        0x56, 0x6E, 0x8F, 0x44, 0xBB, 0x8F, 0x02, 0x22, 0x68, 0x63, 0x97, 0xBC, 0x85, 0xBA, 0xA8, 0xF7,
        0xB5, 0x40, 0x68, 0x3C, 0x77, 0x86, 0x6F, 0x4B, 0xD7, 0x88, 0xCA, 0x8A, 0xD7, 0xCE, 0x36, 0xF0,
        0x45, 0x6E, 0xD5, 0x64, 0x79, 0x0F, 0x17, 0xFC, 0x64, 0xDD, 0x10, 0x6F, 0xF3, 0xF5, 0xE0, 0xA6,
        0xC3, 0xFB, 0x1B, 0x8C, 0x29, 0xEF, 0x8E, 0xE5, 0x34, 0xCB, 0xD1, 0x2A, 0xCE, 0x79, 0xC3, 0x9A,
        0x0D, 0x36, 0xEA, 0x01, 0xE0, 0xAA, 0x91, 0x20, 0x54, 0xF0, 0x72, 0xD8, 0x1E, 0xC7, 0x89, 0xD2
    };

    WorldPacket data(SMSG_ADDON_INFO, 4);

    for (auto& itr : m_addonsList)
    {
        uint8 state = 2;                                    // 2 is sent here
        data << uint8(state);

        uint8 unk1 = 1;                                     // 1 is sent here
        data << uint8(unk1);
        if (unk1)
        {
            uint8 unk2 = (itr.CRC != 0x4c1c776d);          // If addon is Standard addon CRC
            data << uint8(unk2);                            // if 1, than add addon public signature
            if (unk2)                                       // if CRC is wrong, add public key (client need it)
                data.append(tdata, sizeof(tdata));

            data << uint32(0);
        }

        uint8 unk3 = 0;                                     // 0 is sent here
        data << uint8(unk3);                                // use <Addon>\<Addon>.url file or not
        if (unk3)
        {
            // String, 256 (null terminated?)
            data << uint8(0);
        }
    }

    m_addonsList.clear();

    uint32 count = 0;
    data << uint32(count);                                  // BannedAddons count
    /*for(uint32 i = 0; i < count; ++i)
    {
        uint32
        string (16 bytes)
        string (16 bytes)
        uint32
        uint32
        uint32
    }*/

    SendPacket(data);
}

void WorldSession::SetPlayer(Player* plr, uint32 playerGuid)
{
    _player = plr;
    m_GUIDLow = playerGuid;
}

void WorldSession::SendRedirectClient(std::string& ip, uint16 port) const
{
    const uint32 ip2 = static_cast<uint32>(boost::asio::ip::address_v4::from_string(ip).to_ulong());
    WorldPacket pkt(SMSG_CONNECT_TO, 4 + 2 + 4 + 20);

    pkt << uint32(ip2);                                     // inet_addr(ipstr)
    pkt << uint16(port);                                    // port

    pkt << uint32(0);                                       // unknown

    HMACSHA1 sha1(40, m_Socket->GetSessionKey().AsByteArray());
    sha1.UpdateData((uint8*)&ip2, 4);
    sha1.UpdateData((uint8*)&port, 2);
    sha1.Finalize();
    pkt.append(sha1.GetDigest(), 20);                       // hmacsha1(ip+port) w/ sessionkey as seed

    SendPacket(pkt);
}

void WorldSession::ExecuteOpcode(OpcodeHandler const& opHandle, WorldPacket& packet)
{
    // need prevent do internal far teleports in handlers because some handlers do lot steps
    // or call code that can do far teleports in some conditions unexpectedly for generic way work code
    if (_player)
        _player->SetCanDelayTeleport(true);

    (this->*opHandle.handler)(packet);

    if (_player)
    {
        // can be not set in fact for login opcode, but this not create porblems.
        _player->SetCanDelayTeleport(false);

        // we should execute delayed teleports only for alive(!) players
        // because we don't want player's ghost teleported from graveyard
        if (_player->IsHasDelayedTeleport())
            _player->TeleportTo(_player->m_teleport_dest, _player->m_teleport_options);
    }

    if (packet.rpos() < packet.wpos() && sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        LogUnprocessedTail(packet);
}

void WorldSession::SendPlaySpellVisual(ObjectGuid guid, uint32 spellArtKit) const
{
    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 8 + 4);        // visual effect on guid
    data << guid;
    data << spellArtKit;                                    // index from SpellVisualKit.dbc
    SendPacket(data);
}

void WorldSession::SynchronizeMovement(MovementInfo& movementInfo)
{
    int64 movementTime = (int64)movementInfo.ctime + m_timeSyncClockDelta;
    if (m_timeSyncClockDelta == 0 || movementTime < 0 || movementTime > 0xFFFFFFFF)
    {
        DETAIL_LOG("The computed movement time using clockDelta is erronous. Using fallback instead");
        movementInfo.stime = World::GetCurrentMSTime();
    }
    else
        movementInfo.stime = movementTime;
}

std::deque<uint32> WorldSession::GetOpcodeHistory()
{
    if (m_Socket)
        return m_Socket->GetOpcodeHistory();
    else
        return std::deque<uint32>();
}

void WorldSession::SendAuthOk() const
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
    packet << uint8(AUTH_OK);
    SendPacket(packet);
}

void WorldSession::SendAuthQueued() const
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1 + 4 + 1);
    packet << uint8(AUTH_WAIT_QUEUE);
    packet << uint32(sWorld.GetQueuedSessionPos(this));     // position in queue
    packet << uint8(0);                                     // unk 3.3.0
    SendPacket(packet);
}

void WorldSession::ResetTimeSync()
{
    m_timeSyncNextCounter = 0;
    m_pendingTimeSyncRequests.clear();
}

void WorldSession::SendTimeSync()
{
    WorldPacket data(SMSG_TIME_SYNC_REQ, 4);
    data << uint32(m_timeSyncNextCounter);
    SendPacket(data);

    m_pendingTimeSyncRequests[m_timeSyncNextCounter] = WorldTimer::getMSTime();

    // Schedule next sync in 10 sec (except for the 2 first packets, which are spaced by only 5s)
    m_timeSyncTimer = m_timeSyncNextCounter == 0 ? 5000 : 10000;
    m_timeSyncNextCounter++;
}
