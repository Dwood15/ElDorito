#define _CRT_SECURE_NO_WARNINGS

#include "ServerChat.hpp"
#include "../Patches/CustomPackets.hpp"
#include "../Modules/ModuleServer.hpp"
#include "../Utils/String.hpp"
#include <unordered_map>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <Windows.h>

namespace
{
	using namespace Server::Chat;

	typedef Patches::CustomPackets::Packet<ChatMessage> ChatMessagePacket;
	typedef Patches::CustomPackets::PacketSender<ChatMessage> ChatMessagePacketSender;

	std::shared_ptr<ChatMessagePacketSender> PacketSender;

	bool HostReceivedMessage(Blam::Network::Session *session, int peer, const ChatMessage &message);
	void ClientReceivedMessage(const ChatMessage &message);

	struct ClientSpamStats
	{
		ClientSpamStats() : SpamScore(0), TimeoutSeconds(0), NextTimeoutSeconds(0), TimeoutResetSeconds(0) {}

		int SpamScore;           // Decreased by 1 each second.
		int TimeoutSeconds;      // The number of seconds that the client is timed out for.
		int NextTimeoutSeconds;  // The length of the next timeout period in seconds (0 = default).
		int TimeoutResetSeconds; // The number of seconds remaining before the next timeout length is reset.
	};

	// Maps IP addresses to spam statistics.
	std::unordered_map<uint32_t, ClientSpamStats> SpamStats;

	DWORD LastTimeMs;   // The time, in milliseconds, of the last tick.
	DWORD SpamUpdateMs; // Number of milliseconds since the last spam score decrement.

	// Packet handler for chat messages.
	class ChatMessagePacketHandler: public Patches::CustomPackets::PacketHandler<ChatMessage>
	{
	public:
		void Serialize(Blam::BitStream *stream, const ChatMessage *data) override
		{
			// Message type
			stream->WriteUnsigned(static_cast<uint32_t>(data->Type), 0U, static_cast<uint32_t>(ChatMessageType::Count));

			// Body
			stream->WriteString(data->Body);

			// For non-server messages, serialize the sender name
			if (data->Type != ChatMessageType::Server)
				stream->WriteString(data->Sender);

			// For whisper messages, serialize the target UID
			if (data->Type == ChatMessageType::Whisper)
				stream->WriteUnsigned(data->Target, 64);
		}

		bool Deserialize(Blam::BitStream *stream, ChatMessage *data) override
		{
			memset(data, 0, sizeof(*data));

			// Message type
			data->Type = static_cast<ChatMessageType>(stream->ReadUnsigned(0U, static_cast<uint32_t>(ChatMessageType::Count)));
			if (static_cast<uint32_t>(data->Type) >= static_cast<uint32_t>(ChatMessageType::Count))
				return false;

			// Body
			if (!stream->ReadString(data->Body))
				return false;

			// For non-server messages, deserialize the sender name
			if (data->Type != ChatMessageType::Server && !stream->ReadString(data->Sender))
				return false;

			// For whisper messages, deserialize the target UID
			if (data->Type == ChatMessageType::Whisper)
				data->Target = stream->ReadUnsigned<uint64_t>(64);
			return true;
		}

		void HandlePacket(Blam::Network::ObserverChannel *sender, const ChatMessagePacket *packet) override
		{
			auto session = Blam::Network::GetActiveSession();
			if (!session)
				return;
			auto peer = session->GetChannelPeer(sender);
			if (peer < 0)
				return;
			if (session->IsHost())
				HostReceivedMessage(session, peer, packet->Data);
			else if (peer == session->MembershipInfo.HostPeerIndex)
				ClientReceivedMessage(packet->Data);
		}
	};

	// Registered chat handlers
	std::vector<std::shared_ptr<ChatHandler>> chatHandlers;

	// Sends a message to a peer as a packet.
	bool SendMessagePacket(int peer, const ChatMessage &message)
	{
		if (peer < 0)
			return false;
		auto packet = PacketSender->New();
		packet.Data = message;
		PacketSender->Send(peer, packet);
		return true;
	}

	// Broadcasts a message to a set of peers.
	bool BroadcastMessage(Blam::Network::Session *session, int senderPeer, ChatMessage *message, PeerBitSet peers)
	{
		if (senderPeer < 0)
			return false;

		// Run the message through each registered handler
		auto ignore = false;
		for (auto &&handler : chatHandlers)
			handler->MessageSent(senderPeer, message, &ignore);
		if (ignore)
			return true; // Message was rejected by a handler

		// Loop through each peer and send them a packet (or handle the message
		// immediately if it's being sent to the local peer)
		auto membership = &session->MembershipInfo;
		for (auto peer = membership->FindFirstPeer(); peer >= 0; peer = membership->FindNextPeer(peer))
		{
			if (!peers[peer])
				continue; // Not being sent to this peer
			if (peer == membership->LocalPeerIndex)
				ClientReceivedMessage(*message);
			else if (!SendMessagePacket(peer, *message))
				return false;
		}
		return true;
	}

	// Gets a bitset of peers on the same team as a peer.
	bool GetTeamPeers(Blam::Network::Session *session, int senderPeer, PeerBitSet *result)
	{
		result->reset();

		// Get the sender's team
		if (!session->HasTeams())
			return false;
		auto membership = &session->MembershipInfo;
		auto senderTeam = membership->GetPeerTeam(senderPeer);
		if (senderTeam < 0)
			return false;

		// Loop through each peer and check if the peer is on the sender's team
		for (auto peer = membership->FindFirstPeer(); peer >= 0; peer = membership->FindNextPeer(peer))
		{
			if (membership->GetPeerTeam(peer) == senderTeam)
				result->set(peer);
		}
		return true;
	}

	// Gets a bitset of peers to send a message to.
	bool GetMessagePeers(Blam::Network::Session *session, int senderPeer, const ChatMessage &message, PeerBitSet *result)
	{
		switch (message.Type)
		{
		case ChatMessageType::Global:
			result->set();
			break;
		case ChatMessageType::Team:
			GetTeamPeers(session, senderPeer, result);
			break;
		// TODO: ChatMessageType::Whisper
		default:
			return false;
		}
		return true;
	}

	// Fills in the sender name field of a message.
	bool FillInSenderName(Blam::Network::Session *session, int senderPeer, ChatMessage *message)
	{
		// Look up the player associated with the peer and copy the name in
		auto membership = &session->MembershipInfo;
		auto playerIndex = membership->GetPeerPlayer(senderPeer);
		if (playerIndex < 0)
			return false;
		memset(message->Sender, 0, sizeof(message->Sender));
		wcsncpy(message->Sender, membership->PlayerSessions[playerIndex].DisplayName, sizeof(message->Sender) / sizeof(message->Sender[0]) - 1);
		return true;
	}

	// Calculates the spam score of a message.
	int CalculateSpamScore(uint32_t ip, ClientSpamStats *stats, const ChatMessage &message)
	{
		// Compute a score between the short and long scores based on the message length
		// Messages which are closer to the maximum length will have a score closer to the maximum score
		auto &serverModule = Modules::ModuleServer::Instance();
		auto shortScore = serverModule.VarFloodMessageScoreShort->ValueInt;
		auto longScore = serverModule.VarFloodMessageScoreLong->ValueInt;
		return shortScore + strlen(message.Body) * (longScore + 1 - shortScore) / (MaxMessageLength + 1);
	}

	// Checks a message against the flood filter and returns true if it should be thrown out.
	bool FloodFilterMessage(Blam::Network::Session *session, int peer, const ChatMessage &message)
	{
		// Increase the IP's spam score and put it in timeout if it exceeds the maximum
		auto ip = session->GetPeerAddress(peer).Address.IPv4;
		auto spamIt = SpamStats.find(ip);
		if (spamIt == SpamStats.end())
			spamIt = SpamStats.insert({ ip, ClientSpamStats() }).first;
		if (spamIt->second.TimeoutSeconds <= 0)
		{
			// Calculate the message's spam score and add it to the IP's total score
			auto score = CalculateSpamScore(ip, &spamIt->second, message);
			spamIt->second.SpamScore += score;

			// If the total score reached the timeout score, then start a timeout
			auto &serverModule = Modules::ModuleServer::Instance();
			if (spamIt->second.SpamScore >= static_cast<int>(serverModule.VarFloodTimeoutScore->ValueInt))
			{
				// If the IP had a previous timeout that hasn't been reset yet, double it, otherwise start with the default
				if (spamIt->second.NextTimeoutSeconds > 0)
					spamIt->second.NextTimeoutSeconds *= 2;
				else
					spamIt->second.NextTimeoutSeconds = serverModule.VarFloodTimeoutSeconds->ValueInt;

				spamIt->second.TimeoutSeconds = spamIt->second.NextTimeoutSeconds;
				spamIt->second.TimeoutResetSeconds = serverModule.VarFloodTimeoutResetSeconds->ValueInt;
			}
		}

		// If the IP is in a timeout state, send an error and return
		if (spamIt->second.TimeoutSeconds > 0)
		{
			PeerBitSet targetPeers;
			targetPeers.set(peer);
			SendServerMessage("You have exceeded the server's spam limit. You can chat again in " + std::to_string(spamIt->second.TimeoutSeconds) + " second(s).", targetPeers);
			return true;
		}
		return false;
	}

	// Writes a message to the log file.
	void LogMessage(Blam::Network::Session *session, int peer, const ChatMessage &message)
	{
		auto &serverModule = Modules::ModuleServer::Instance();
		if (!serverModule.VarChatLogEnabled->ValueInt)
			return;

		// Try to open the log file for appending
		auto logPath = Modules::ModuleServer::Instance().VarChatLogPath->ValueString;
		std::ofstream logFile(logPath, std::ios::app);
		if (!logFile)
			return;

		// Get the UTC time
		auto now = std::chrono::system_clock::now();
		auto time = std::chrono::system_clock::to_time_t(now);
		struct tm gmTime;
		gmtime_s(&gmTime, &time);

		// Get the name and IP address
		auto sender = Utils::String::ThinString(message.Sender);
		auto ip = session->GetPeerAddress(peer).Address.IPv4;

		// Get the UID
		uint64_t uid = 0;
		auto playerIndex = session->MembershipInfo.GetPeerPlayer(peer);
		if (playerIndex >= 0)
			uid = session->MembershipInfo.PlayerSessions[playerIndex].Uid;

		logFile << "[" << std::put_time(&gmTime, "%m/%d/%y %H:%M:%S") << "] "; // Timestamp
		logFile << "<" << sender << "/"; // Sender name
		logFile << std::setw(16) << std::setfill('0') << std::hex << uid << std::dec << std::setw(0) << "/"; // UID
		logFile << (ip >> 24) << "." << ((ip >> 16) & 0xFF) << "." << ((ip >> 8) & 0xFF) << "." << (ip & 0xFF) << "> "; // IP address
		logFile << message.Body << "\n"; // Message body
	}

	// Callback for when a message is received as the host.
	bool HostReceivedMessage(Blam::Network::Session *session, int peer, const ChatMessage &message)
	{
		// Verify that the message isn't empty and that the type is valid
		// TODO: Implement support for message types other than Global
		if (peer < 0 || !message.Body[0] || message.Type == ChatMessageType::Server)
			return false;

		// Don't trust the Sender field
		auto broadcastMessage = message;
		if (!FillInSenderName(session, peer, &broadcastMessage))
			return false;

		// Check the message against the flood filter if it's enabled
		auto &serverModule = Modules::ModuleServer::Instance();
		if (peer != session->MembershipInfo.LocalPeerIndex)
		{
			if (serverModule.VarFloodFilterEnabled->ValueInt)
			{
				if (FloodFilterMessage(session, peer, broadcastMessage))
					return true; // Message was thrown out
			}
		}

		LogMessage(session, peer, broadcastMessage);

		PeerBitSet targetPeers;
		if (!GetMessagePeers(session, peer, message, &targetPeers))
			return false;
		return BroadcastMessage(session, peer, &broadcastMessage, targetPeers);
	}

	// Callback for when a message is received as the client.
	void ClientReceivedMessage(const ChatMessage &message)
	{
		// Send the message out to handlers
		for (auto &&handler : chatHandlers)
			handler->MessageReceived(message);
	}

	// Sends a message as a client.
	bool SendClientMessage(Blam::Network::Session *session, const ChatMessage &message)
	{
		if (session->IsHost())
		{
			// We're the host, so pretend a client sent us the message
			return HostReceivedMessage(session, session->MembershipInfo.LocalPeerIndex, message);
		}

		// Send the message across the network to the host
		return SendMessagePacket(session->MembershipInfo.HostPeerIndex, message);
	}
}

namespace Server
{
	namespace Chat
	{
		void Initialize()
		{
			LastTimeMs = timeGetTime();

			// Register custom packet type
			auto handler = std::make_shared<ChatMessagePacketHandler>();
			PacketSender = Patches::CustomPackets::RegisterPacket<ChatMessage>("eldewrito-text-chat", handler);
		}

		void Tick()
		{
			// Compute the time delta (the game also uses timeGetTime in its various subsystems to do this)
			auto currentTimeMs = timeGetTime();
			auto timeDeltaMs = currentTimeMs - LastTimeMs;
			LastTimeMs = currentTimeMs;

			// Update the flood filter for each second that has passed since the last flood filter update
			SpamUpdateMs += timeDeltaMs;
			while (SpamUpdateMs >= 1000)
			{
				SpamUpdateMs -= 1000;

				// Decrease each spam score and timeout, and remove empty structures to save memory
				auto it = SpamStats.begin();
				while (it != SpamStats.end())
				{
					auto nextIt = it;
					++nextIt;

					if (it->second.SpamScore > 0)
						it->second.SpamScore--;

					if (it->second.TimeoutSeconds > 0)
						it->second.TimeoutSeconds--;
					else if (it->second.TimeoutResetSeconds > 0)
						it->second.TimeoutResetSeconds--; // Only decrement the timeout reset if no timeout is active

					if (it->second.TimeoutResetSeconds == 0)
						it->second.NextTimeoutSeconds = 0;

					if (it->second.TimeoutSeconds <= 0 && it->second.SpamScore <= 0 && it->second.TimeoutResetSeconds <= 0)
						SpamStats.erase(it);

					it = nextIt;
				}
			}
		}

		bool SendGlobalMessage(const std::string &body)
		{
			auto session = Blam::Network::GetActiveSession();
			if (!session || !session->IsEstablished())
				return false;

			ChatMessage message(ChatMessageType::Global, body);
			return SendClientMessage(session, message);
		}

		bool SendTeamMessage(const std::string &body)
		{
			auto session = Blam::Network::GetActiveSession();
			if (!session || !session->IsEstablished() || !session->HasTeams())
				return false;

			ChatMessage message(ChatMessageType::Team, body);
			return SendClientMessage(session, message);
		}

		bool SendServerMessage(const std::string &body, PeerBitSet peers)
		{
			auto session = Blam::Network::GetActiveSession();
			if (!session || !session->IsEstablished() || !session->IsHost())
				return false;

			ChatMessage message(ChatMessageType::Server, body);
			return BroadcastMessage(session, session->MembershipInfo.LocalPeerIndex, &message, peers);
		}

		void AddHandler(std::shared_ptr<ChatHandler> handler)
		{
			chatHandlers.push_back(handler);
		}

		ChatMessage::ChatMessage(ChatMessageType type, const std::string &body)
		{
			memset(this, 0, sizeof(*this));
			Type = type;
			strncpy(Body, body.c_str(), sizeof(Body) / sizeof(Body[0]) - 1);
		}
	}
}