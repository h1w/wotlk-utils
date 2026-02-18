# CMSG Packet Handlers — WoW 3.3.5a (build 12340)

> Source: offsets.txt, section "Packet Handlers"
> Client-to-server message handlers

**Note:** No CMSG handlers are present in the client executable dump. CMSG (client→server) messages are sent by the client but handled by the server, so their handlers exist in the server code, not in Wow.exe.

The packet handler section contains only:
- **MSG** handlers (bidirectional)
- **SMSG** handlers (server→client, handled by the client)
