// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "RammsStreamProtocol.h"

class FSocket;

/**
 * Represents a single client connection to the RMSS streaming server.
 *
 * Owns a background receive thread that reads incoming messages from the
 * client socket and places them in an inbound queue.  Outbound messages
 * are enqueued by the server/game thread and flushed by the server's
 * send loop.
 */
class RAMMSSTREAMING_API FRammsStreamConnection : public FRunnable
{
public:
	FRammsStreamConnection(FSocket* InSocket, uint32 InConnectionId);
	virtual ~FRammsStreamConnection();

	// ── Connection identity ──────────────────────────────────────────
	uint32	GetConnectionId() const { return ConnectionId; }
	FString GetRemoteAddress() const;

	// ── Lifecycle ────────────────────────────────────────────────────
	void Start();
	void RequestStop();
	bool IsConnected() const { return bConnected; }

	// ── Subscriptions ────────────────────────────────────────────────
	/** Channels this client wants to receive. Empty = receive nothing. */
	TSet<uint16> SubscribedChannels;

	/** Preferred compression for outbound frames. */
	ERammsStreamCompression PreferredCompression = ERammsStreamCompression::None;

	// ── Outbound (server → client) ───────────────────────────────────
	/**
	 * Enqueue a message for sending.  Thread-safe.
	 * @param Message  The message to send.
	 * @param bDropOldest  If true and the queue is full, drop the oldest message.
	 * @return true if enqueued (or replaced oldest), false if dropped (queue full
	 * and !bDropOldest).
	 */
	bool EnqueueOutbound(const TSharedRef<FRammsStreamMessage>& Message,
		bool													bDropOldest = true);

	/** Flush as many outbound messages as possible to the socket. Call from the
	 * send thread. */
	int32 FlushOutbound();

	/** Maximum number of outbound messages to buffer before dropping. */
	int32 MaxOutboundQueueSize = 3;

	// ── Inbound (client → server) ────────────────────────────────────
	/** Dequeue one inbound message. Returns false if empty. Thread-safe. */
	bool DequeueInbound(FRammsStreamMessage& OutMessage);

	/** Drain all currently buffered inbound messages into OutMessages. Thread-safe. */
	void DrainInbound(TArray<FRammsStreamMessage>& OutMessages);

	/** Maximum number of inbound messages to buffer before dropping oldest.
	 *  0 = unlimited (legacy behaviour). */
	TAtomic<int32> MaxInboundQueueSize = 0;

	// ── FRunnable (receive thread) ───────────────────────────────────
	virtual bool   Init() override;
	virtual uint32 Run() override;
	virtual void   Stop() override;

	/** Delegate broadcast when the connection is closed (from any thread). */
	FSimpleDelegate OnDisconnected;

private:
	FSocket*		 Socket = nullptr;
	uint32			 ConnectionId = 0;
	FRunnableThread* RecvThread = nullptr;
	TAtomic<bool>	 bStopping{ false };
	TAtomic<bool>	 bConnected{ false };

	// Receive buffer for accumulating partial messages
	TArray<uint8>		   RecvBuffer;
	static constexpr int32 RECV_CHUNK_SIZE = 65536;

	/** Minimum allocated RecvBuffer size before shrink logic is considered. */
	static constexpr int32 RECV_BUFFER_SHRINK_THRESHOLD = 4 * RECV_CHUNK_SIZE; // 256 KB

	// Thread-safe queues
	FCriticalSection						OutboundLock;
	TArray<TSharedRef<FRammsStreamMessage>> OutboundQueue;

	// Inbound ring buffer — O(1) enqueue, dequeue and drop-oldest.
	// When MaxInboundQueueSize > 0 the ring is pre-allocated to that
	// capacity and never grows.  When 0 (unbounded) it falls back to
	// plain TArray append semantics.
	FCriticalSection			InboundLock;
	TArray<FRammsStreamMessage> InboundRing;
	int32						InboundHead = 0;  // index of oldest message
	int32						InboundCount = 0; // number of valid messages

	/** Ensure the ring backing store has room for at least Cap elements. */
	void EnsureInboundRingCapacity(int32 Cap);

	/** Send raw bytes, handling partial sends. Returns false on error. */
	bool SendAll(const uint8* Data, int32 Len);
};
