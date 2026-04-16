// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamConnection.h"
#include "HAL/RunnableThread.h"
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

		int32 BytesRead = 0;
		if (!Socket->Recv(ChunkBuffer.GetData(), RECV_CHUNK_SIZE, BytesRead))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("RMSS: Recv error on connection %u (buffer=%d bytes), "
					 "disconnecting"),
				ConnectionId, RecvBuffer.Num());
			break;
		}

		if (BytesRead == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("RMSS: Connection %u closed gracefully"),
				ConnectionId);
			break;
		}

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

bool FRammsStreamConnection::EnqueueOutbound(
	const TSharedRef<FRammsStreamMessage>& Message, bool bDropOldest)
{
	FScopeLock Lock(&OutboundLock);
	if (OutboundQueue.Num() >= MaxOutboundQueueSize)
	{
		if (bDropOldest)
		{
			OutboundQueue.RemoveAt(0);
		}
		else
		{
			return false;
		}
	}
	OutboundQueue.Add(Message);
	return true;
}

int32 FRammsStreamConnection::FlushOutbound()
{
	TArray<TSharedRef<FRammsStreamMessage>> ToSend;
	{
		FScopeLock Lock(&OutboundLock);
		Swap(ToSend, OutboundQueue);
	}

	int32 Sent = 0;
	for (const auto& Msg : ToSend)
	{
		TArray<uint8> Bytes = Msg->Serialize();
		if (!SendAll(Bytes.GetData(), Bytes.Num()))
		{
			// Send failed — connection likely dead
			bConnected = false;
			break;
		}
		++Sent;
	}
	return Sent;
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool FRammsStreamConnection::SendAll(const uint8* Data, int32 Len)
{
	int32 TotalSent = 0;
	while (TotalSent < Len)
	{
		int32 BytesSent = 0;
		if (!Socket->Send(Data + TotalSent, Len - TotalSent, BytesSent))
		{
			return false;
		}
		TotalSent += BytesSent;
	}
	return true;
}
