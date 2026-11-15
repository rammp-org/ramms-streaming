// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamServer.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "IPAddress.h"
#include "RammsStreamConnection.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStream, Log, All);

#if !PLATFORM_WINDOWS
namespace RammsStreamServerImpl
{
	/** Whether something on this machine accepts TCP connections on Port (a connect to loopback succeeds). */
	bool IsPortListenedOn(uint16 Port)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!Sockets)
		{
			return false;
		}
		const TSharedRef<FInternetAddr> Loopback = FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), Port).ToInternetAddr();
		FSocket*						Probe = Sockets->CreateSocket(NAME_Stream, TEXT("RammsStreamPortProbe"), Loopback->GetProtocolType());
		if (!Probe)
		{
			return false;
		}
		bool bListenedOn = false;
		Probe->SetNonBlocking(true);
		Probe->Connect(*Loopback);
		if (Probe->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(250)))
		{
			bListenedOn = Probe->GetConnectionState() == SCS_Connected;
		}
		Probe->Close();
		Sockets->DestroySocket(Probe);
		return bListenedOn;
	}
} // namespace RammsStreamServerImpl
#endif

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

	LastError.Reset();
	ISocketSubsystem* SocketSubsystem =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		LastError = TEXT("no socket subsystem");
		UE_LOG(LogRammsStream, Error, TEXT("Failed to get socket subsystem"));
		return false;
	}

	// Never share the port with another listener: a client would reach either one.
#if PLATFORM_WINDOWS
	// Windows' SO_REUSEADDR is exactly what lets a second socket bind a port another one listens on; without it
	// that bind fails, as it should.
	constexpr bool bReuseAddress = false;
#else
	// Elsewhere SO_REUSEADDR is what lets a restarted server rebind a port its old connections still hold in
	// TIME_WAIT. UE sets SO_REUSEPORT with it, though, which does let two listeners share a port: look first.
	constexpr bool bReuseAddress = true;
	if (Port != 0 && RammsStreamServerImpl::IsPortListenedOn(Port))
	{
		LastError = FString::Printf(TEXT("port %u is already in use by another program"), Port);
		UE_LOG(LogRammsStream, Error, TEXT("RMSS: %s"), *LastError);
		return false;
	}
#endif

	ListenSocket = FTcpSocketBuilder(TEXT("RammsStreamServer"))
					   .AsReusable(bReuseAddress)
					   .AsNonBlocking()
					   .BoundToPort(Port)
					   .Listening(8)
					   .WithSendBufferSize(2 * 1024 * 1024) // 2 MB send buffer
					   .WithReceiveBufferSize(256 * 1024)
					   .Build();

	if (!ListenSocket)
	{
		LastError = FString::Printf(TEXT("could not listen on port %u (is another program using it?)"), Port);
		UE_LOG(LogRammsStream, Error, TEXT("RMSS: %s"), *LastError);
		return false;
	}
	// The port actually bound: a configured 0 takes any free one.
	Port = static_cast<uint16>(ListenSocket->GetPortNo());

	SendEvent = FPlatformProcess::GetSynchEventFromPool(false);
	bStopping = false;
	bListening = true;

	ServerThread =
		FRunnableThread::Create(this, TEXT("RammsStreamServer"), 0, TPri_Normal);

	if (!ServerThread)
	{
		LastError = TEXT("could not create the server thread");
		UE_LOG(LogRammsStream, Error,
			TEXT("RMSS: Failed to create server thread!"));
		bListening = false;
		ListenSocket->Close();
		SocketSubsystem->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
		FPlatformProcess::ReturnSynchEventToPool(SendEvent);
		SendEvent = nullptr;
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

	// Close all connections. Stopping one makes a send in progress on it fail at once, so the send thread is not
	// held up; the connections themselves are destroyed outside the lock (each joins its receive thread).
	TMap<uint32, TSharedPtr<FRammsStreamConnection>> Closing;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->RequestStop();
			}
		}
		Closing = MoveTemp(Connections);
		Connections.Reset();
	}
	Closing.Empty();

	// Let the send thread finish before the listen socket goes: it accepts on it.
	if (ServerThread)
	{
		if (SendEvent)
		{
			SendEvent->Trigger();
		}
		ServerThread->WaitForCompletion();
		delete ServerThread;
		ServerThread = nullptr;
	}

	// OnClientConnected refuses a client once bStopping is set, so none is added after the map was taken above; with
	// the send thread gone, make sure of it.
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->RequestStop();
			}
		}
		Closing = MoveTemp(Connections);
		Connections.Reset();
	}
	Closing.Empty();

	if (ListenSocket)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
			->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}

	// Taken under the lock that broadcasting triggers it under.
	FEvent* Event = nullptr;
	{
		FScopeLock Lock(&ConnectionsLock);
		Event = SendEvent;
		SendEvent = nullptr;
	}
	if (Event)
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	if (bListening)
	{
		UE_LOG(LogRammsStream, Log, TEXT("RMSS server stopped"));
	}
	bListening = false;
}

int32 FRammsStreamServer::GetConnectionCount() const
{
	FScopeLock Lock(&ConnectionsLock);
	return Connections.Num();
}

bool FRammsStreamServer::HasSubscriber(uint16 Channel) const
{
	FScopeLock Lock(&ConnectionsLock);
	for (const TPair<uint32, TSharedPtr<FRammsStreamConnection>>& Pair : Connections)
	{
		// A client that never subscribed wants nothing: it is sent no broadcasts.
		const FRammsStreamConnection* Conn = Pair.Value.Get();
		if (Conn && Conn->IsConnected() && Conn->WantsChannel(Channel))
		{
			return true;
		}
	}
	return false;
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

	// The listen socket is non-blocking (StartListening): accept polls, which is
	// more reliable than WaitForPendingConnection on some platforms.
	uint64									   IterCount = 0;
	TArray<TSharedPtr<FRammsStreamConnection>> Sending;

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

		// Send to every client what its socket takes now, outside the connection lock: broadcasting (game thread)
		// and Tick only ever wait for this copy, never for a send. FlushOutbound does not wait either, so a client
		// that reads slowly or not at all holds up nobody else.
		{
			FScopeLock Lock(&ConnectionsLock);
			for (auto& Pair : Connections)
			{
				if (Pair.Value.IsValid() && Pair.Value->IsConnected())
				{
					Sending.Add(Pair.Value);
				}
			}
		}
		bool bAnyPending = false;
		for (const TSharedPtr<FRammsStreamConnection>& Conn : Sending)
		{
			int64 BytesSent = 0;
			Stats.FramesSent += static_cast<uint32>(Conn->FlushOutbound(&BytesSent));
			Stats.TotalBytesSent += static_cast<uint64>(BytesSent);
			bAnyPending |= Conn->IsConnected() && Conn->HasPendingOutbound();
		}
		// A connection the game thread dropped meanwhile is destroyed here, on this thread, when its last reference goes.
		Sending.Reset();

		// Periodic heartbeat log
		if (IterCount % 1000 == 0)
		{
			UE_LOG(LogRammsStream, Verbose,
				TEXT("RMSS: Accept loop alive (%llu iterations, %d connections)"),
				IterCount, GetConnectionCount());
		}

		// Sleep until something is queued, or briefly while a client's socket is full, so its stream resumes as soon
		// as it drains; new connections are picked up within the idle wait.
		SendEvent->Wait(FTimespan::FromMilliseconds(bAnyPending ? 2.0 : 10.0));
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
		if (bStopping)
		{
			// StopListening has taken (or is about to take) the connections: one added now would outlive it.
			ClientSocket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			return;
		}
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

	// Non-blocking, explicitly: Windows passes the listen socket's mode on to an accepted socket, Linux does not,
	// and the send thread must never block in a send (FRammsStreamConnection::FlushOutbound).
	ClientSocket->SetNonBlocking(true);

	// Set a reasonable recv buffer on the accepted socket (the listen socket's
	// buffer size is NOT inherited by accepted sockets on all platforms).
	int32 ActualRecvSize = 0;
	ClientSocket->SetReceiveBufferSize(2 * 1024 * 1024, ActualRecvSize);
	// And a large send buffer, so a camera's frames (a 640x480 colour frame is 1.2 MB, its four streams about
	// 5 MB) mostly go out in one pass. The OS may grant less (Linux caps it at net.core.wmem_max); a full buffer is
	// back-pressure, handled by sending the rest on a later pass.
	int32 ActualSendSize = 0;
	ClientSocket->SetSendBufferSize(4 * 1024 * 1024, ActualSendSize);
	UE_LOG(LogRammsStream, Verbose, TEXT("RMSS: Accepted socket buffers: send %d, receive %d bytes"), ActualSendSize,
		ActualRecvSize);

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
		// Checked under the lock StopListening takes the connections under, after it set bStopping: either this
		// connection is in the map it takes, or it is refused here.
		if (bStopping)
		{
			Conn->RequestStop();
			return; // destroyed (its receive thread joined) as the last reference goes, after the lock
		}
		Connections.Add(ConnId, Conn);
	}

	Stats.TotalConnectionsAccepted++;

	UE_LOG(LogRammsStream, Log,
		TEXT("RMSS: Client %u connected from %s (%d total)"), ConnId,
		*Conn->GetRemoteAddress(), GetConnectionCount());
}

void FRammsStreamServer::CleanupDisconnected()
{
	// Destroyed after the lock is released: a connection's destructor joins its receive thread.
	TArray<TSharedPtr<FRammsStreamConnection>> Removed;
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
			TSharedPtr<FRammsStreamConnection> Conn;
			Connections.RemoveAndCopyValue(Id, Conn);
			Removed.Add(MoveTemp(Conn));
		}
	}
	for (const TSharedPtr<FRammsStreamConnection>& Conn : Removed)
	{
		if (Conn.IsValid())
		{
			Conn->RequestStop();
			Stats.TotalBytesReceived += Conn->ConsumeReceivedBytes();
		}
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
	TArray<FRammsStreamMessage>				   ConnMessages;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (!Pair.Value.IsValid())
				continue;

			Pair.Value->DrainInbound(ConnMessages);
			for (auto& Msg : ConnMessages)
			{
				InboundMessages.Emplace(Pair.Key, MoveTemp(Msg));
			}
			Stats.TotalBytesReceived += Pair.Value->ConsumeReceivedBytes();
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
	const TSharedRef<FRammsStreamMessage>& Message, uint64 StreamKey)
{
	const uint16 Channel = Message->Header.ChannelID;

	bool bQueuedAny = false;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (auto& Pair : Connections)
		{
			if (!Pair.Value.IsValid() || !Pair.Value->IsConnected())
				continue;

			// Only to a client subscribed to this channel; one that never subscribed is sent nothing, so a client
			// that only sends (an image feed into UE) never fills up with frames it does not read.
			if (!Pair.Value->WantsChannel(Channel))
			{
				continue;
			}

			// Queuing never waits on the client: a slow one has an older frame of this stream replaced.
			int32 Dropped = 0;
			bQueuedAny |= Pair.Value->EnqueueOutbound(Message, /*bDropOldest=*/true, &Dropped, StreamKey);
			Stats.FramesDropped += Dropped;
		}
		// Under the lock: StopListening releases the event under it too.
		if (bQueuedAny && SendEvent)
		{
			SendEvent->Trigger();
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
	const bool bQueued = (*Found)->EnqueueOutbound(Message);
	if (bQueued && SendEvent)
	{
		SendEvent->Trigger();
	}
	return bQueued;
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
		if (SendEvent)
		{
			SendEvent->Trigger();
		}
		return;
	}

	// Parse subscribe/unsubscribe metadata JSON (none at all reads as {}: no channel list)
	const FString			JsonStr = Msg.GetMetadataString().TrimStartAndEnd();
	TSharedPtr<FJsonObject> JsonObj;
	if (JsonStr.IsEmpty())
	{
		JsonObj = MakeShared<FJsonObject>();
	}
	else
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			UE_LOG(LogRammsStream, Warning,
				TEXT("RMSS: Bad JSON in control message from client %u"),
				ConnectionId);
			return;
		}
	}

	// "channels": [0, 1, 2]. No list, or an empty one, names every channel. Entries that are not channel numbers
	// (0..65535) are ignored.
	TArray<uint16>						  Channels;
	const TArray<TSharedPtr<FJsonValue>>* ChannelsArray = nullptr;
	const bool							  bListed =
		JsonObj->TryGetArrayField(TEXT("channels"), ChannelsArray) && ChannelsArray && ChannelsArray->Num() > 0;
	if (bListed)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ChannelsArray)
		{
			double Number = 0.0;
			if (Val.IsValid() && Val->TryGetNumber(Number) && Number >= 0.0 && Number <= static_cast<double>(MAX_uint16))
			{
				Channels.Add(static_cast<uint16>(Number));
			}
		}
	}

	if (Msg.Header.MessageType == ERammsStreamMessageType::Subscribe)
	{
		// {"channels":[0,1,2],"compression":"jpeg"}; {} subscribes to every channel. Only now is the client sent
		// broadcasts at all.
		Conn.ApplySubscription(/*bSubscribe=*/true, bListed, Channels);

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

		if (Conn.bSubscribedToAll)
		{
			UE_LOG(LogRammsStream, Log, TEXT("RMSS: Client %u subscribed to every channel (compression=%s)"),
				ConnectionId, *CompStr);
		}
		else
		{
			UE_LOG(LogRammsStream, Log, TEXT("RMSS: Client %u subscribed to %d channels (compression=%s)"),
				ConnectionId, Conn.SubscribedChannels.Num(), *CompStr);
		}

		// Send ACK
		auto Ack = MakeShared<FRammsStreamMessage>();
		Ack->Header.MessageType = ERammsStreamMessageType::Ack;
		Ack->SetMetadataString(TEXT("{\"status\":\"ok\"}"));
		Conn.EnqueueOutbound(Ack);
		if (SendEvent)
		{
			SendEvent->Trigger();
		}
	}
	else if (Msg.Header.MessageType == ERammsStreamMessageType::Unsubscribe)
	{
		// Those channels; no list (or an empty one): all of them, after which the client is sent nothing. Removing
		// the last listed channel leaves nothing too, never "every channel".
		Conn.ApplySubscription(/*bSubscribe=*/false, bListed, Channels);

		const FString Remaining = Conn.bSubscribedToAll
			? FString::Printf(TEXT("every channel but %d"), Conn.UnsubscribedChannels.Num())
			: FString::Printf(TEXT("%d channels remaining"), Conn.SubscribedChannels.Num());
		UE_LOG(LogRammsStream, Log, TEXT("RMSS: Client %u unsubscribed (%s)"), ConnectionId, *Remaining);
	}
}
