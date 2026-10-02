#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_MESHBOT
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * A slash-command text bot ("/help", "/ping", ...). Accepts DMs and broadcasts on its command
 * channel (MESHBOT_COMMAND_CHANNEL, primary by default) and always answers on the channel the
 * command arrived on. Commands live in kCommands (see the .cpp); a per-sender cooldown and an
 * allowlist for the state-changing commands keep it from flooding the mesh.
 */
class MeshBotModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    MeshBotModule();

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

    // Handler signature: given the received packet and any text after the command word,
    // write the reply into out (a caller-owned buffer of outLen bytes).
    using CommandHandler = void (MeshBotModule::*)(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);

    struct Command {
        const char *name; // without leading '/'
        CommandHandler handler;
        const char *help;            // one-line description for /help
        bool privileged;             // transmits or mutates state; gated behind the sender allowlist
        const char *alias = nullptr; // optional second spelling; accepted but not listed by /help
    };

    // Dispatch table; defined in the .cpp. A class member so it can reference the private handlers.
    static const Command kCommands[];

    // Split a text payload into a command word and its argument tail. Returns the matching
    // command, or nullptr if the message isn't a recognized slash-command.
    const Command *parseCommand(const char *msg, const char **argsOut) const;

    void sendReply(const meshtastic_MeshPacket &rx, const char *text);

    // Send a text packet to an explicit destination/channel, bypassing the request/reply pairing.
    void sendTextTo(uint32_t dest, uint8_t channel, const char *text);

    // Build and send a bare request packet (empty payload) on a portnum toward one node.
    bool sendRequestTo(const meshtastic_MeshPacket &rx, uint32_t dest, meshtastic_PortNum portnum, bool wantAck, char *out,
                       size_t outLen);

    // Command handlers.
    void cmdHelp(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSignal(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdNodes(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdUptime(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdEcho(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdOnline(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdWhoami(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdVer(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdNode(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPos(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSeen(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPeerBatt(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPeerUtil(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    // Action commands: trigger a transmit or mutate local state.
    void cmdTrace(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdReqPos(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdReqTel(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdAnnounce(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSharePos(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdFav(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdUnfav(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdAckPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdRoll(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdFlip(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmd8ball(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdJoke(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);

  private:
    // Where to deliver the route once the traceroute /trace kicked off comes back.
    struct PendingTrace {
        uint32_t requester = 0;
        uint32_t target = 0;
        uint32_t startedMs = 0;
        uint8_t channel = 0;
        bool broadcast = false;
        bool active = false;
    };
    PendingTrace pendingTrace;

    // Decode a TRACEROUTE_APP response and relay the route to whoever asked for it.
    void handleTraceRouteReply(const meshtastic_MeshPacket &mp);
};
#endif // MESHTASTIC_EXCLUDE_MESHBOT
