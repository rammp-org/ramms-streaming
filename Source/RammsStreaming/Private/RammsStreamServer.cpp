// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamServer.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "RammsStreamConnection.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStream, Log, All);

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

FRammsStreamServer::FRammsStreamServer(uint16 InPort, int32 InMaxClients)
	: Port(InPort), MaxClients(InMaxClients) {}

FRammsStreamServer::~FRammsStreamServer()
{
	StopListening();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool FRammsStreamServer::StartListening()
{
	if (bListening)
		return true;

	ISocketSubsystem* SocketSubsystem =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogRammsStream, Error, TEXT("Failed to get socket subsystem"));
		return false;
	}

	ListenSocket = FTcpSocketBuilder(TEXT("RammsStreamServer"))
					   .AsReusable()
					   .BoundToPort(Port)
					   .Listening(8)
					   .WithSendBufferSize(2 * 1024 * 1024) // 2 MB send buffer
					   .WithReceiveBufferSize(256 * 1024)
					   .Build();

	if (!ListenSocket)
	{
		UE_LOG(LogRammsStream, Error,
			TEXT("Failed to create listen socket on port %u"), Port);
		return false;
	}

	bStopping = false;
	bListening = true;

	ServerThread =
		FRunnableThread::Create(this, TEXT("RammsStreamServer"), 0, TPri_Normal);

	if (!ServerThread)
	{
		UE_LOG(LogRammsStream, Error,
			TEXT("RMSS: Failed to create server thread!"));
		bListening = false;
		return false;
	}

	UE_LOG(LogRammsStream, Log,
		TEXT("RMSS server listening on port %u (thread=%p)"), Port,
		ServerThread);
	return true;
}

void FRammsStreamServer::StopListening()
{
	bStopping = true;

	// Close all connections
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->RequestStop();
			}
		}
		Connections.Empty();
	}

	// Close listen socket (unblocks accept)
	if (ListenSocket)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
			->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}

	if (ServerThread)
	{
		ServerThread->WaitForCompletion();
		delete ServerThread;
		ServerThread = nullptr;
	}

	bListening = false;
	UE_LOG(LogRammsStream, Log, TEXT("RMSS server stopped"));
}

int32 FRammsStreamServer::GetConnectionCount() const
{
	// Approximate — no lock needed for a count
	FScopeLock Lock(&const_cast<FRammsStreamServer*>(this)->ConnectionsLock);
	return Connections.Num();
}

// ---------------------------------------------------------------------------
// FRunnable — accept loop + outbound flush
// ---------------------------------------------------------------------------

bool FRammsStreamServer::Init()
{
	return true;
}

uint32 FRammsStreamServer::Run()
{
	UE_LOG(LogRammsStream, Log, TEXT("RMSS: Accept/send thread started"));

	// Use non-blocking accept — more reliable than WaitForPendingConnection
	// which can silently fail on some platforms.
	if (ListenSocket)
	{
		ListenSocket->SetNonBlocking(true);
	}

	uint64 IterCount = 0;

	while (!bStopping)
	{
		++IterCount;

		// Try to accept (non-blocking — returns nullptr if no pending connection)
		if (ListenSocket)
		{
			FSocket* ClientSocket = ListenSocket->Accept(TEXT("RammsStreamClient"));
			if (ClientSocket)
			{
				UE_LOG(LogRammsStream, Log,
					TEXT("RMSS: Accepted raw socket, calling OnClientConnected"));
				OnClientConnected(ClientSocket);
			}
		}

		// Flush outbound queues for all connections
		{
			FScopeLock Lock(&ConnectionsLock);
			for (auto& Pair : Connections)
			{
				if (Pair.Value.IsValid() && Pair.Value->IsConnected())
				{
					int32 Flushed = Pair.Value->FlushOutbound();
					Stats.FramesSent += Flushed;
				}
			}
		}

		// Periodic heartbeat log (every ~10 seconds at 100 iterations/sec)
		if (IterCount % 1000 == 0)
		{
			UE_LOG(LogRammsStream, Verbose,
				TEXT("RMSS: Accept loop alive (%llu iterations, %d connections)"),
				IterCount, GetConnectionCount());
		}

		// Sleep briefly to avoid busy-spinning
		FPlatformProcess::Sleep(0.01f);
	}

	UE_LOG(LogRammsStream, Log, TEXT("RMSS: Accept/send thread exiting"));
	return 0;
}

void FRammsStreamServer::Stop()
{
	bStopping = true;
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void FRammsStreamServer::OnClientConnected(FSocket* ClientSocket)
{
	{
		FScopeLock Lock(&ConnectionsLock);
		if (Connections.Num() >= MaxClients)
		{
			UE_LOG(LogRammsStream, Warning,
				TEXT("RMSS: Rejecting connection (max %d clients reached)"),
				MaxClients);
			ClientSocket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
				->DestroySocket(ClientSocket);
			return;
		}
	}

	// Set a reasonable recv buffer on the accepted socket (the listen socket's
	// buffer size is NOT inherited by accepted sockets on all platforms).
	int32 ActualRecvSize = 0;
	ClientSocket->SetReceiveBufferSize(2 * 1024 * 1024, ActualRecvSize);

	const uint32					   ConnId = NextConnectionId++;
	TSharedPtr<FRammsStreamConnection> Conn =
		MakeShared<FRammsStreamConnection>(ClientSocket, ConnId);
	Conn->MaxInboundQueueSize = DefaultMaxInboundQueueSize;
	Conn->MaxOutboundQueueSize = DefaultMaxOutboundQueueSize;

	// Start the recv thread BEFORE adding to the map to avoid a race with
	// CleanupDisconnected() — Start() sets bConnected=true, and Cleanup
	// removes any entry where IsConnected()==false.
	Conn->Start();

	{
		FScopeLock Lock(&ConnectionsLock);
		Connections.Add(ConnId, Conn);
	}

	Stats.TotalConnectionsAccepted++;

	UE_LOG(LogRammsStream, Log,
		TEXT("RMSS: Client %u connected from %s (%d total)"), ConnId,
		*Conn->GetRemoteAddress(), GetConnectionCount());
}

void FRammsStreamServer::CleanupDisconnected()
{
	FScopeLock	   Lock(&ConnectionsLock);
	TArray<uint32> ToRemove;
	for (auto& Pair : Connections)
	{
		if (!Pair.Value.IsValid() || !Pair.Value->IsConnected())
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (uint32 Id : ToRemove)
	{
		UE_LOG(LogRammsStream, Log, TEXT("RMSS: Removing disconnected client %u"),
			Id);
		Connections.Remove(Id);
	}
}

// ---------------------------------------------------------------------------
// Tick (game thread)
// ---------------------------------------------------------------------------

void FRammsStreamServer::Tick()
{
	CleanupDisconnected();

	// Collect inbound messages from all connections
	TArray<TPair<uint32, FRammsStreamMessage>> InboundMessages;
	TArray<FRammsStreamMessage> ConnMessages;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (!Pair.Value.IsValid())
				continue;

			ConnMessages.Reset();
			Pair.Value->DrainInbound(ConnMessages);
			for (auto& Msg : ConnMessages)
			{
				InboundMessages.Emplace(Pair.Key, MoveTemp(Msg));
			}
		}
	}

	// Dispatch
	for (auto& [ConnId, Msg] : InboundMessages)
	{
		UE_LOG(
			LogRammsStream, Verbose,
			TEXT("RMSS: Dispatching msg type=0x%02X ch=%d payload=%d from conn %u"),
			static_cast<uint8>(Msg.Header.MessageType), Msg.Header.ChannelID,
			Msg.Header.PayloadLen, ConnId);

		// Handle control messages internally
		if (Msg.Header.MessageType == ERammsStreamMessageType::Subscribe || Msg.Header.MessageType == ERammsStreamMessageType::Unsubscribe || Msg.Header.MessageType == ERammsStreamMessageType::Ping)
		{
			HandleControlMessage(ConnId, Msg);
		}

		// Always fire the delegate so the subsystem/game code can react
		OnMessageReceived.ExecuteIfBound(ConnId, Msg);
	}
}

// ---------------------------------------------------------------------------
// Broadcasting
// ---------------------------------------------------------------------------

void FRammsStreamServer::BroadcastToSubscribers(
	const TSharedRef<FRammsStreamMessage>& Message)
{
	const uint16 Channel = Message->Header.ChannelID;

	FScopeLock Lock(&ConnectionsLock);
	for (auto& Pair : Connections)
	{
		if (!Pair.Value.IsValid() || !Pair.Value->IsConnected())
			continue;

		// Only send if the client is subscribed to this channel
		if (Pair.Value->SubscribedChannels.Num() > 0 && !Pair.Value->SubscribedChannels.Contains(Channel))
		{
			continue;
		}

		if (!Pair.Value->EnqueueOutbound(Message, /*bDropOldest=*/true))
		{
			Stats.FramesDropped++;
		}
	}
}

bool FRammsStreamServer::SendTo(
	uint32 ConnectionId, const TSharedRef<FRammsStreamMessage>& Message)
{
	FScopeLock							Lock(&ConnectionsLock);
	TSharedPtr<FRammsStreamConnection>* Found = Connections.Find(ConnectionId);
	if (!Found || !Found->IsValid())
		return false;
	return (*Found)->EnqueueOutbound(Message);
}

// ---------------------------------------------------------------------------
// Control message handling
// ---------------------------------------------------------------------------

void FRammsStreamServer::HandleControlMessage(uint32 ConnectionId,
	const FRammsStreamMessage&						 Msg)
{
	FScopeLock							Lock(&ConnectionsLock);
	TSharedPtr<FRammsStreamConnection>* Found = Connections.Find(ConnectionId);
	if (!Found || !Found->IsValid())
		return;
	FRammsStreamConnection& Conn = **Found;

	if (Msg.Header.MessageType == ERammsStreamMessageType::Ping)
	{
		// Respond with Ping (pong)
		auto Pong = MakeShared<FRammsStreamMessage>();
		Pong->Header.MessageType = ERammsStreamMessageType::Ping;
		Pong->Header.Timestamp = Msg.Header.Timestamp;
		Conn.EnqueueOutbound(Pong);
		return;
	}

	// Parse subscribe/unsubscribe metadata JSON
	FString					  JsonStr = Msg.GetMetadataString();
	TSharedPtr<FJsonObject>	  JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogRammsStream, Warning,
			TEXT("RMSS: Bad JSON in control message from client %u"),
			ConnectionId);
		return;
	}

	if (Msg.Header.MessageType == ERammsStreamMessageType::Subscribe)
	{
		// {"channels":[0,1,2],"compression":"jpeg"}
		const TArray<TSharedPtr<FJsonValue>>* ChannelsArray = nullptr;
		if (JsonObj->TryGetArrayField(TEXT("channels"), ChannelsArray))
		{
			for (const auto& Val : *ChannelsArray)
			{
				Conn.SubscribedChannels.Add(static_cast<uint16>(Val->AsNumber()));
			}
		}

		FString CompStr;
		if (JsonObj->TryGetStringField(TEXT("compression"), CompStr))
		{
			if (CompStr == TEXT("lz4"))
				Conn.PreferredCompression = ERammsStreamCompression::LZ4;
			else if (CompStr == TEXT("jpeg"))
				Conn.PreferredCompression = ERammsStreamCompression::JPEG;
			else if (CompStr == TEXT("png"))
				Conn.PreferredCompression = ERammsStreamCompression::PNG;
			else
				Conn.PreferredCompression = ERammsStreamCompression::None;
		}

		UE_LOG(LogRammsStream, Log,
			TEXT("RMSS: Client %u subscribed to %d channels (compression=%s)"),
			ConnectionId, Conn.SubscribedChannels.Num(), *CompStr);

		// Send ACK
		auto Ack = MakeShared<FRammsStreamMessage>();
		Ack->Header.MessageType = ERammsStreamMessageType::Ack;
		Ack->SetMetadataString(TEXT("{\"status\":\"ok\"}"));
		Conn.EnqueueOutbound(Ack);
	}
	else if (Msg.Header.MessageType == ERammsStreamMessageType::Unsubscribe)
	{
		const TArray<TSharedPtr<FJsonValue>>* ChannelsArray = nullptr;
		if (JsonObj->TryGetArrayField(TEXT("channels"), ChannelsArray))
		{
			for (const auto& Val : *ChannelsArray)
			{
				Conn.SubscribedChannels.Remove(static_cast<uint16>(Val->AsNumber()));
			}
		}
		else
		{
			// No channels specified — unsubscribe from all
			Conn.SubscribedChannels.Empty();
		}

		UE_LOG(LogRammsStream, Log,
			TEXT("RMSS: Client %u unsubscribed (%d channels remaining)"),
			ConnectionId, Conn.SubscribedChannels.Num());
	}
}
