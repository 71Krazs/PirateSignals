# Privacy statement

Pirate Signals sends public chat messages only between a connected Windrose
client and the dedicated server that client joined. It does not contact an
external chat relay, analytics service, domain, or account provider.

## Data handled by the chat protocol

The dedicated server receives the message text and associates it with the
connected player's authoritative `PlayerState` display name. It broadcasts the
display name, message text, timestamp, and message sequence ID to every client
on that server.

The chat protocol does not ask the client to send a Steam ID, Steam persona
name, account ID, IP address, email address, or client-selected identity.

## Retention

The dedicated-server mod keeps at most 100 messages in memory. That history is
not written to a database by Pirate Signals and clears when the server process
restarts.

Pirate Signals diagnostics record message metadata but do not write message
bodies to the client or server UE4SS log. Server diagnostics include the
visible player name for an accepted message. Server operators and players may
also use independent game, operating-system, moderation, logging, or recording
tools outside this mod.

## Network metadata

Because Pirate Signals uses the existing game connection, it creates no new
listening port. A dedicated-server operator or hosting provider can ordinarily
observe connection metadata such as player IP addresses through the game or
network infrastructure independently of Pirate Signals.

## Public nature of messages

There are no private messages. Anything sent through Pirate Signals is shown
to the server and connected players and should be treated as public within that
server.
