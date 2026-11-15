// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamConnection.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FRammsStreamConnection::FRammsStreamConnection(FSocket* InSocket,
	uint32												InConnectionId)
	: Socket(InSocket), ConnectionId(InConnectionId) {}

FRammsStreamConnection::~FRammsStreamConnection()
{
	RequestStop();

	if (RecvThread)
	{
		RecvThread->WaitForCompletion();
		delete RecvThread;
		RecvThread = nullptr;
	}

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

FString FRammsStreamConnection::GetRemoteAddress() const
{
	if (!Socket)
		return TEXT("(none)");
	TSharedRef<FInternetAddr> Addr =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	Socket->GetPeerAddress(*Addr);
	return Addr->ToString(true);
}

void FRammsStreamConnection::Start()
{
	bConnected = true;
	UE_LOG(LogTemp, Log, TEXT("RMSS: Starting recv thread for connection %u"),
		ConnectionId);
	RecvThread = FRunnableThread::Create(
		this, *FString::Printf(TEXT("RammsStreamRecv_%u"), ConnectionId), 0,
		TPri_Normal);
	if (!RecvThread)
	{
		UE_LOG(LogTemp, Error,
			TEXT("RMSS: Failed to create recv thread for connection %u!"),
			ConnectionId);
		bConnected = false;
	}
}

void FRammsStreamConnection::RequestStop()
{
	bStopping = true;
	if (Socket)
	{
		Socket->Shutdown(ESocketShutdownMode::ReadWrite);
	}
}

// ---------------------------------------------------------------------------
// FRunnable
// ---------------------------------------------------------------------------

bool FRammsStreamConnection::Init()
{
	return true;
}

uint32 FRammsStreamConnection::Run()
{
	TArray<uint8> ChunkBuffer;
	ChunkBuffer.SetNumUninitialized(RECV_CHUNK_SIZE);

	UE_LOG(LogTemp, Log, TEXT("RMSS: Recv thread started for connection %u"),
		ConnectionId);

	while (!bStopping)
	{
		// Wait for data with a short timeout so we can check bStopping
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead,
				FTimespan::FromMilliseconds(100)))
		{
			continue;
		}

		// FSocket::Recv on a stream socket returns false both for an error and for the peer closing (zero bytes),
		// and true with zero bytes when a non-blocking socket had nothing after all. Accepted sockets are
		// non-blocking (see FRammsStreamServer::OnClientConnected), so the latter is not a close.
		int32 BytesRead = 0;
		if (!Socket->Recv(ChunkBuffer.GetData(), RECV_CHUNK_SIZE, BytesRead))
		{
			UE_LOG(LogTemp, Log,
				TEXT("RMSS: Connection %u closed by the peer or failed (buffer=%d bytes)"),
				ConnectionId, RecvBuffer.Num());
			break;
		}

		if (BytesRead == 0)
		{
			continue;
		}
		ReceivedBytes += static_cast<uint64>(BytesRead);

		// Append to receive buffer
		const int32 OldSize = RecvBuffer.Num();
		RecvBuffer.SetNumUninitialized(OldSize + BytesRead, EAllowShrinking::No);
		FMemory::Memcpy(RecvBuffer.GetData() + OldSize, ChunkBuffer.GetData(),
			BytesRead);

		// Try to parse complete messages from the buffer
		int32 Offset = 0;
		while (Offset < RecvBuffer.Num())
		{
			FRammsStreamMessage Msg;
			int32				Consumed = 0;
			if (!FRammsStreamMessage::Deserialize(RecvBuffer.GetData() + Offset,
					RecvBuffer.Num() - Offset, Msg,
					Consumed))
			{
				break; // incomplete message, wait for more data
			}

			// Enqueue the parsed message (drop oldest if over capacity)
			{
				FScopeLock Lock(&InboundLock);

				const int32 Cap = MaxInboundQueueSize.Load();
				if (Cap > 0)
				{
					// Bounded ring-buffer path — O(1) drop and enqueue.
					EnsureInboundRingCapacity(Cap);

					if (InboundCount >= Cap)
					{
						// Drop the oldest (Cap - 1) keeps one slot for the new msg.
						const int32 Excess = InboundCount - Cap + 1;
						InboundHead = (InboundHead + Excess) % InboundRing.Num();
						InboundCount -= Excess;
					}

					const int32 Tail = (InboundHead + InboundCount) % InboundRing.Num();
					InboundRing[Tail] = MoveTemp(Msg);
					++InboundCount;
				}
				else
				{
					// Unbounded — plain append (no dropping).
					InboundRing.Add(MoveTemp(Msg));
					++InboundCount;
				}
			}

			Offset += Consumed;
		}

		// Compact the buffer — remove consumed bytes
		if (Offset > 0)
		{
			const int32 Remaining = RecvBuffer.Num() - Offset;
			if (Remaining > 0)
			{
				FMemory::Memmove(RecvBuffer.GetData(), RecvBuffer.GetData() + Offset,
					Remaining);
			}
			RecvBuffer.SetNum(Remaining, EAllowShrinking::No);

			// Periodically reclaim memory when the buffer is far larger than needed
			if (RecvBuffer.GetAllocatedSize() > RECV_BUFFER_SHRINK_THRESHOLD
				&& RecvBuffer.Num() < static_cast<int32>(RecvBuffer.GetAllocatedSize() / 4))
			{
				RecvBuffer.Shrink();
			}
		}
	}

	bConnected = false;
	UE_LOG(LogTemp, Log,
		TEXT("RMSS: Recv thread exiting for connection %u (stopping=%s)"),
		ConnectionId, bStopping ? TEXT("true") : TEXT("false"));
	OnDisconnected.ExecuteIfBound();
	return 0;
}

void FRammsStreamConnection::Stop()
{
	bStopping = true;
}

// ---------------------------------------------------------------------------
// Outbound queue
// ---------------------------------------------------------------------------

namespace RammsStreamConnectionImpl
{
	/** Frames may be superseded or dropped under back-pressure; control messages may not. */
	bool IsDroppableFrame(const FRammsStreamMessage& Message)
	{
		return GetFrameCategory(Message.Header.MessageType) != ERammsFrameCategory::Control;
	}
} // namespace RammsStreamConnectionImpl

bool FRammsStreamConnection::EnqueueOutbound(
	const TSharedRef<FRammsStreamMessage>& Message, bool bDropOldest, int32* OutDropped, uint64 StreamKey)
{
	using RammsStreamConnectionImpl::IsDroppableFrame;

	int32 Dropped = 0;
	bool  bQueued = true;
	{
		FScopeLock Lock(&OutboundLock);
		int32	   SameStream = INDEX_NONE;
		if (bDropOldest && IsDroppableFrame(*Message))
		{
			// A newer frame of the same stream takes the place of one still waiting, so a slow client is sent the
			// latest of every stream instead of working through a backlog. In place, not moved to the back: each
			// stream keeps its turn, so the streams queued last in a tick (a camera's mask after its colour, depth
			// and motion; an IMU feed) are not starved by the ones queued before them. The stream key keeps the
			// cameras a source sends on one channel apart.
			const uint16				  Channel = Message->Header.ChannelID;
			const ERammsStreamMessageType Type = Message->Header.MessageType;
			SameStream = OutboundQueue.IndexOfByPredicate([Channel, Type, StreamKey](const FQueuedOutbound& Queued) {
				return Queued.StreamKey == StreamKey && Queued.Message->Header.ChannelID == Channel
					&& Queued.Message->Header.MessageType == Type;
			});
		}
		if (SameStream != INDEX_NONE)
		{
			OutboundQueue[SameStream].Message = Message;
			++Dropped;
		}
		else
		{
			if (OutboundQueue.Num() >= FMath::Max(1, MaxOutboundQueueSize))
			{
				// Make room with the oldest frame; acks and pongs stay. With only control messages queued, a frame
				// is refused rather than one of them dropped (and a control message replaces the oldest one, so a
				// client that only pings cannot grow the queue).
				const int32 OldestFrame = bDropOldest
					? OutboundQueue.IndexOfByPredicate([](const FQueuedOutbound& Queued) {
						  return IsDroppableFrame(*Queued.Message);
					  })
					: INDEX_NONE;
				if (OldestFrame != INDEX_NONE)
				{
					OutboundQueue.RemoveAt(OldestFrame);
				}
				else if (bDropOldest && !IsDroppableFrame(*Message))
				{
					OutboundQueue.RemoveAt(0);
				}
				else
				{
					bQueued = false;
				}
				++Dropped;
			}
			if (bQueued)
			{
				OutboundQueue.Add(FQueuedOutbound{ Message, StreamKey });
			}
		}
	}
	if (OutDropped)
	{
		*OutDropped = Dropped;
	}
	return bQueued;
}

bool FRammsStreamConnection::HasPendingOutbound() const
{
	if (InFlight.IsValid())
	{
		return true;
	}
	FScopeLock Lock(&OutboundLock);
	return OutboundQueue.Num() > 0;
}

void FRammsStreamConnection::GetQueuedOutbound(TArray<TSharedRef<FRammsStreamMessage>>& OutMessages) const
{
	FScopeLock Lock(&OutboundLock);
	OutMessages.Reset(OutboundQueue.Num());
	for (const FQueuedOutbound& Queued : OutboundQueue)
	{
		OutMessages.Add(Queued.Message);
	}
}

void FRammsStreamConnection::ApplySubscription(bool bSubscribe, bool bListed, const TArray<uint16>& Channels)
{
	if (bSubscribe)
	{
		if (!bListed)
		{
			// Every channel.
			bSubscribedToAll = true;
			UnsubscribedChannels.Reset();
			SubscribedChannels.Reset();
		}
		else if (bSubscribedToAll)
		{
			for (const uint16 Channel : Channels)
			{
				UnsubscribedChannels.Remove(Channel);
			}
		}
		else
		{
			SubscribedChannels.Append(Channels);
		}
		return;
	}

	if (!bListed)
	{
		// Nothing any more.
		bSubscribedToAll = false;
		UnsubscribedChannels.Reset();
		SubscribedChannels.Reset();
	}
	else if (bSubscribedToAll)
	{
		UnsubscribedChannels.Append(Channels);
	}
	else
	{
		for (const uint16 Channel : Channels)
		{
			SubscribedChannels.Remove(Channel);
		}
	}
}

void FRammsStreamConnection::FailSend(const TCHAR* Why)
{
	UE_LOG(LogTemp, Warning, TEXT("RMSS: Dropping connection %u: %s"), ConnectionId, Why);
	InFlight.Reset();
	bConnected = false;
}

int32 FRammsStreamConnection::FlushOutbound(int64* OutBytesSent)
{
	// Never waits. The listen socket is non-blocking, an accepted socket is made non-blocking too, and a frame
	// larger than the socket's send buffer (a 640x480 colour frame is 1.2 MB) fills it: that is back-pressure, not
	// a dead client. What the socket does not take now is sent on a later call, from where it stopped, while the
	// send thread serves the other clients in between.
	int32 Completed = 0;
	int64 Sent = 0;
	ON_SCOPE_EXIT
	{
		if (OutBytesSent)
		{
			*OutBytesSent = Sent;
		}
	};
	while (!bStopping && bConnected)
	{
		if (!InFlight.IsValid())
		{
			{
				FScopeLock Lock(&OutboundLock);
				if (OutboundQueue.Num() == 0)
				{
					return Completed;
				}
				InFlight = OutboundQueue[0].Message;
				OutboundQueue.RemoveAt(0, EAllowShrinking::No);
			}
			FRammsStreamHeader Header = InFlight->Header;
			Header.MetadataLen = static_cast<uint32>(InFlight->Metadata.Num());
			Header.PayloadLen = static_cast<uint32>(InFlight->Payload.Num());
			Header.ToBytes(InFlightHeader);
			InFlightOffset = 0;
			LastSendProgressSeconds = FPlatformTime::Seconds();
		}

		const int64 HeaderBytes = FRammsStreamHeader::HEADER_SIZE;
		const int64 MetadataBytes = InFlight->Metadata.Num();
		const int64 TotalBytes = HeaderBytes + MetadataBytes + InFlight->Payload.Num();
		while (InFlightOffset < TotalBytes)
		{
			const uint8* Data = nullptr;
			int64		 Remaining = 0;
			if (InFlightOffset < HeaderBytes)
			{
				Data = InFlightHeader + InFlightOffset;
				Remaining = HeaderBytes - InFlightOffset;
			}
			else if (InFlightOffset < HeaderBytes + MetadataBytes)
			{
				Data = InFlight->Metadata.GetData() + (InFlightOffset - HeaderBytes);
				Remaining = HeaderBytes + MetadataBytes - InFlightOffset;
			}
			else
			{
				Data = InFlight->Payload.GetData() + (InFlightOffset - HeaderBytes - MetadataBytes);
				Remaining = TotalBytes - InFlightOffset;
			}

			int32 BytesSent = 0;
			if (Socket->Send(Data, static_cast<int32>(Remaining), BytesSent) && BytesSent > 0)
			{
				InFlightOffset += BytesSent;
				Sent += BytesSent;
				LastSendProgressSeconds = FPlatformTime::Seconds();
				continue;
			}

			const ESocketErrors Error = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
			if (bStopping)
			{
				// Being closed (RequestStop shut the socket down): not a failure worth reporting.
				InFlight.Reset();
				return Completed;
			}
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				FailSend(*FString::Printf(TEXT("send error %d after %lld of %lld bytes of a message"),
					static_cast<int32>(Error), InFlightOffset, TotalBytes));
				return Completed;
			}
			const double Stalled = FPlatformTime::Seconds() - LastSendProgressSeconds;
			if (Stalled > SendStallTimeoutSeconds)
			{
				FailSend(*FString::Printf(TEXT("it took no data for %.1f s (%lld of %lld bytes of a message sent)"),
					Stalled, InFlightOffset, TotalBytes));
				return Completed;
			}
			// The socket is full: carry on from here on a later pass.
			return Completed;
		}

		InFlight.Reset();
		++Completed;
	}
	return Completed;
}

// ---------------------------------------------------------------------------
// Inbound queue
// ---------------------------------------------------------------------------

bool FRammsStreamConnection::DequeueInbound(FRammsStreamMessage& OutMessage)
{
	FScopeLock Lock(&InboundLock);
	if (InboundCount == 0)
		return false;

	OutMessage = MoveTemp(InboundRing[InboundHead]);

	// For bounded mode, advance the head index in the ring.
	// For unbounded mode, InboundRing is used as a plain array so we
	// must still remove the element (but DrainInbound via Swap is the
	// typical hot-path consumer, so this is fine).
	const int32 Cap = MaxInboundQueueSize.Load();
	if (Cap > 0)
	{
		InboundHead = (InboundHead + 1) % InboundRing.Num();
	}
	else
	{
		InboundRing.RemoveAt(0, EAllowShrinking::No);
		// InboundHead stays 0 in unbounded mode.
	}
	--InboundCount;
	return true;
}

void FRammsStreamConnection::DrainInbound(
	TArray<FRammsStreamMessage>& OutMessages)
{
	FScopeLock Lock(&InboundLock);
	OutMessages.Reset();

	if (InboundCount == 0)
		return;

	const int32 Cap = MaxInboundQueueSize.Load();
	if (Cap > 0)
	{
		// Ring buffer — copy items in order, then reset head/count.
		const int32 RingSize = InboundRing.Num();
		OutMessages.Reserve(InboundCount);
		for (int32 i = 0; i < InboundCount; ++i)
		{
			OutMessages.Add(MoveTemp(InboundRing[(InboundHead + i) % RingSize]));
		}
		InboundHead = 0;
		InboundCount = 0;
	}
	else
	{
		// Unbounded — O(1) swap.
		Swap(OutMessages, InboundRing);
		InboundHead = 0;
		InboundCount = 0;
	}
}

// ---------------------------------------------------------------------------
// Inbound ring buffer
// ---------------------------------------------------------------------------

void FRammsStreamConnection::EnsureInboundRingCapacity(int32 Cap)
{
	// Already the right size — nothing to do.
	if (InboundRing.Num() == Cap)
		return;

	if (InboundCount == 0)
	{
		// Empty — just resize and reset head.
		InboundRing.SetNum(Cap);
		InboundHead = 0;
		return;
	}

	// Non-empty ring being resized (capacity changed at runtime).
	// Linearize existing items into a new array.
	const int32 OldSize = InboundRing.Num();
	const int32 Keep = FMath::Min(InboundCount, Cap);

	TArray<FRammsStreamMessage> Tmp;
	Tmp.SetNum(Cap);

	// Copy the *newest* Keep items (drop oldest if shrinking).
	const int32 Skip = InboundCount - Keep;
	for (int32 i = 0; i < Keep; ++i)
	{
		Tmp[i] = MoveTemp(InboundRing[(InboundHead + Skip + i) % OldSize]);
	}

	InboundRing = MoveTemp(Tmp);
	InboundHead = 0;
	InboundCount = Keep;
}
