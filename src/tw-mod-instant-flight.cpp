#include "ScriptObjects.h"
#include "Config/Config.h"
#include "DBCStructure.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace
{
    bool s_enabled = false;
    bool s_allowHeadless = false;
    std::set<uint32> s_excludedNodeIds;

    // Parses a comma-separated list of unsigned integers into s_excludedNodeIds.
    void LoadExcludedNodeIds(std::string const& value)
    {
        s_excludedNodeIds.clear();
        if (value.empty())
            return;

        size_t start = 0;
        while (start < value.size())
        {
            size_t end = value.find(',', start);
            if (end == std::string::npos)
                end = value.size();

            std::string token = value.substr(start, end - start);
            // Trim whitespace.
            size_t first = token.find_first_not_of(" \t");
            size_t last = token.find_last_not_of(" \t");
            if (first != std::string::npos && last != std::string::npos)
            {
                token = token.substr(first, last - first + 1);
                if (!token.empty())
                {
                    char* endPtr = nullptr;
                    unsigned long id = std::strtoul(token.c_str(), &endPtr, 10);
                    if (endPtr && *endPtr == '\0')
                        s_excludedNodeIds.insert(static_cast<uint32>(id));
                }
            }

            start = end + 1;
        }
    }

    void LoadConfig()
    {
        s_enabled = sConfig.GetBoolDefault("InstantFlight.Enable", true);
        s_allowHeadless = sConfig.GetBoolDefault("InstantFlight.AllowHeadless", false);
        LoadExcludedNodeIds(sConfig.GetStringDefault("InstantFlight.ExcludeNodeIDs", ""));
    }

    // Returns true for transport nodes: boats, zeppelins, canoes and other
    // routes that have no mount model. These are intended to stay on rails.
    bool IsTransportNode(uint32 nodeId)
    {
        TaxiNodesEntry const* nodeEntry = sObjectMgr.GetTaxiNodeEntry(nodeId);
        if (!nodeEntry)
            return false;

        return nodeEntry->MountCreatureID[0] == 0 && nodeEntry->MountCreatureID[1] == 0;
    }

    class TwModInstantFlightWorldScript : public WorldScript
    {
    public:
        TwModInstantFlightWorldScript()
            : WorldScript("tw-mod-instant-flight_world", { WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_AFTER_CONFIG_LOAD })
        {
        }

        void OnBeforeWorldInitialized() override
        {
            sLog.outString("[tw-mod-instant-flight] module loaded.");
            LoadConfig();
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }
    };

    class TwModInstantFlightServerScript : public ServerScript
    {
    public:
        TwModInstantFlightServerScript()
            : ServerScript("tw-mod-instant-flight_server", { SERVERHOOK_CAN_PACKET_RECEIVE })
        {
        }

        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (!s_enabled)
                return true;

            uint16 const opcode = packet.GetOpcode();
            if (opcode != CMSG_ACTIVATETAXI && opcode != CMSG_ACTIVATETAXIEXPRESS)
                return true;

            if (!session)
                return true;

            // Headless/bot sessions cannot complete a TeleportTo because the
            // client ACK for the teleport never arrives. Let them use the
            // normal on-rails flight instead.
            if (!s_allowHeadless && session->IsHeadless())
                return true;

            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld())
                return true;

            // Make a local copy so we can parse without mutating the original packet.
            WorldPacket copy(packet);

            ObjectGuid guid;
            std::vector<uint32> nodes;

            try
            {
                if (opcode == CMSG_ACTIVATETAXI)
                {
                    nodes.resize(2);
                    copy >> guid >> nodes[0] >> nodes[1];
                }
                else // CMSG_ACTIVATETAXIEXPRESS
                {
                    uint32 nodeCount = 0;
                    uint32 clientTotalCost = 0;
                    copy >> guid >> clientTotalCost >> nodeCount;

                    if (nodeCount == 0 || nodeCount > 64)
                        return true;

                    nodes.resize(nodeCount);
                    for (uint32 i = 0; i < nodeCount; ++i)
                        copy >> nodes[i];
                }
            }
            catch (ByteBufferException const&)
            {
                return true;
            }

            if (nodes.size() < 2)
                return true;

            // Leave transport routes and manually excluded nodes on rails.
            for (uint32 node : nodes)
            {
                if (IsTransportNode(node) || s_excludedNodeIds.find(node) != s_excludedNodeIds.end())
                    return true;
            }

            // Validate that the player actually knows the source and destination nodes.
            // GMs with taxi cheat bypass this check in the normal handler, so mirror that.
            if (!player->IsTaxiCheater())
            {
                PlayerTaxi const& taxi = player->GetTaxi();
                for (uint32 node : nodes)
                    if (!taxi.IsTaximaskNodeKnown(node))
                        return true;
            }

            // Calculate the normal flight cost for all legs.
            uint32 totalCost = 0;
            for (size_t i = 1; i < nodes.size(); ++i)
            {
                uint32 pathId = 0;
                uint32 legCost = 0;
                sObjectMgr.GetTaxiPath(nodes[i - 1], nodes[i], pathId, legCost);

                if (pathId == 0)
                    return true; // invalid leg, let the normal handler reject it

                totalCost += legCost;
            }

            if (totalCost > 0 && player->GetMoney() < totalCost)
                return true; // let the normal handler produce the "not enough money" error

            uint32 const finalNode = nodes.back();
            TaxiNodesEntry const* nodeEntry = sObjectMgr.GetTaxiNodeEntry(finalNode);
            if (!nodeEntry)
                return true;

            // Deduct the flight cost and teleport to the destination flightmaster.
            if (totalCost > 0)
                player->ModifyMoney(-(int32)totalCost);

            player->TeleportTo(nodeEntry->map_id, nodeEntry->x, nodeEntry->y, nodeEntry->z, player->GetOrientation(), TELE_TO_NOT_UNSUMMON_PET);

            // Tell the client the flight was accepted so it closes the taxi UI cleanly.
            WorldPacket reply(SMSG_ACTIVATETAXIREPLY, 4);
            reply << uint32(ERR_TAXIOK);
            session->SendPacket(&reply);

            return false; // cancel the normal flight handling
        }
    };
}

void Addtw_mod_instant_flightScripts()
{
    new TwModInstantFlightWorldScript();
    new TwModInstantFlightServerScript();
}
