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
	// Guarded by the server's connection lock. A client is sent broadcasts (frames) only once it has subscribed: a
	// client that never sends Subscribe gets none -- only the replies to its own requests (acks, pongs) -- so it
	// never makes a channel wanted, and one that only sends (an image feed into UE) is never queued frames it does
	// not read. Subscribe with a list of channels adds those; Subscribe with no list (or an empty one) means every
	// channel. Unsubscribe with a list removes those; with no list (or an empty one) it removes them all, and the
	// client is then sent nothing.

	/** Channels this client subscribed to by number (while it is not subscribed to every channel). */
	TSet<uint16> SubscribedChannels;

	/** Subscribed to every channel (a Subscribe without channels), but UnsubscribedChannels. */
	bool bSubscribedToAll = false;

	/** Channels taken out (Unsubscribe with a list) of a subscription to every channel. */
	TSet<uint16> UnsubscribedChannels;

	/** Whether this client is sent broadcasts on Channel. Call under the server's connection lock. */
	bool WantsChannel(uint16 Channel) const
	{
		return bSubscribedToAll ? !UnsubscribedChannels.Contains(Channel) : SubscribedChannels.Contains(Channel);
	}

	/** Apply a Subscribe (bSubscribe) or an Unsubscribe of Channels; bListed false: the message named no channel (no
	 *  list, or an empty one), which means every channel. Call under the server's connection lock. */
	void ApplySubscription(bool bSubscribe, bool bListed, const TArray<uint16>& Channels);

	/** Preferred compression for outbound frames. */
	ERammsStreamCompression PreferredCompression = ERammsStreamCompression::None;

	// ── Outbound (server → client) ───────────────────────────────────
	/**
	 * Enqueue a message for sending.  Thread-safe.
	 *
	 * With bDropOldest a client that cannot keep up gets the latest of every stream rather than a backlog: a frame
	 * (any non-control message) takes the place of a queued, not yet started frame of the same stream -- the same
	 * channel, message type and StreamKey -- so each stream keeps its turn in the queue and none is starved by the
	 * ones queued after it; and when the queue is full the oldest queued frame makes room. Control messages (acks,
	 * pongs) are never dropped to make room for a frame: with only those queued, the frame is refused. A sender
	 * that numbers its messages (an IMU sample feed) lets a client see what was dropped.
	 * @param Message  The message to send.
	 * @param bDropOldest  If true, supersede and drop as above; if false, a full queue refuses the message.
	 * @param OutDropped  If given, the number of queued messages this call dropped (plus 1 when it refused Message).
	 * @param StreamKey  Which stream of its channel and message type Message belongs to (0: the only one), so the
	 *        frames of several cameras sent on one channel do not take each other's place.
	 * @return true if enqueued, false if refused (queue full and nothing it may drop).
	 */
	bool EnqueueOutbound(const TSharedRef<FRammsStreamMessage>& Message,
		bool bDropOldest = true, int32* OutDropped = nullptr, uint64 StreamKey = 0);

	/**
	 * Send as much as the socket takes right now, without waiting: the message in flight first, then queued ones
	 * oldest first. A message the socket will not take whole stays in flight and continues on the next call, so a
	 * slow client holds up only its own stream. A client that has taken no byte for SendStallTimeoutSeconds is
	 * marked disconnected. Call from the send thread only.
	 * @param OutBytesSent  If given, the bytes this call wrote to the socket.
	 * @return the number of messages completed by this call.
	 */
	int32 FlushOutbound(int64* OutBytesSent = nullptr);

	/** True while a message is part-sent or waiting in the queue. Call from the send thread. */
	bool HasPendingOutbound() const;

	/** The queued (not yet started) outbound messages, oldest first. Thread-safe; for diagnostics and tests. */
	void GetQueuedOutbound(TArray<TSharedRef<FRammsStreamMessage>>& OutMessages) const;

	/** Maximum number of outbound messages queued per connection (see EnqueueOutbound). */
	int32 MaxOutboundQueueSize = 32;

	/** Seconds a client may go without accepting a byte while a message waits for it before it is dropped. */
	static constexpr double SendStallTimeoutSeconds = 5.0;

	// ── Inbound (client → server) ────────────────────────────────────
	/** Dequeue one inbound message. Returns false if empty. Thread-safe. */
	bool DequeueInbound(FRammsStreamMessage& OutMessage);

	/** Drain all currently buffered inbound messages into OutMessages. Thread-safe. */
	void DrainInbound(TArray<FRammsStreamMessage>& OutMessages);

	/** Maximum number of inbound messages to buffer before dropping oldest.
	 *  0 = unlimited (legacy behaviour). */
	TAtomic<int32> MaxInboundQueueSize = 0;

	/** The bytes received since the last call (for the server's statistics). Thread-safe. */
	uint64 ConsumeReceivedBytes() { return ReceivedBytes.Exchange(0); }

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

	/** Bytes the receive thread read since ConsumeReceivedBytes last took them. */
	TAtomic<uint64> ReceivedBytes{ 0 };

	// Receive buffer for accumulating partial messages
	TArray<uint8>		   RecvBuffer;
	static constexpr int32 RECV_CHUNK_SIZE = 65536;

	/** Minimum allocated RecvBuffer size before shrink logic is considered. */
	static constexpr int32 RECV_BUFFER_SHRINK_THRESHOLD = 4 * RECV_CHUNK_SIZE; // 256 KB

	/** A queued outbound message and the stream it belongs to (see EnqueueOutbound). */
	struct FQueuedOutbound
	{
		TSharedRef<FRammsStreamMessage> Message;
		uint64							StreamKey = 0;
	};

	// Thread-safe queues
	mutable FCriticalSection OutboundLock;
	TArray<FQueuedOutbound>	 OutboundQueue;

	// The message being sent (send thread only): it is never dropped once started, since half a message would
	// desynchronise the stream. Sent straight from the shared message (header bytes, then metadata, then payload),
	// so a frame broadcast to several clients is not copied per client.
	TSharedPtr<FRammsStreamMessage> InFlight;
	uint8							InFlightHeader[FRammsStreamHeader::HEADER_SIZE] = {};
	int64							InFlightOffset = 0;
	double							LastSendProgressSeconds = 0.0;

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

	/** Mark the connection dead after a send failure or stall. */
	void FailSend(const TCHAR* Why);
};
