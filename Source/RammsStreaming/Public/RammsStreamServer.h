// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "RammsStreamProtocol.h"

class FEvent;
class FSocket;
class FTcpListener;
class FRammsStreamConnection;

DECLARE_DELEGATE_TwoParams(FOnStreamMessageReceived, uint32 /*ConnectionId*/,
	const FRammsStreamMessage& /*Message*/);

/**
 * TCP server for the RMSS binary streaming protocol.
 *
 * Listens on a configurable port, accepts client connections, and manages
 * per-client send/receive.  All socket I/O runs on background threads;
 * the game thread interacts through thread-safe queues.
 *
 * Typical flow:
 *   1. Construct with port number.
 *   2. Call StartListening() — spawns the accept / send thread.
 *   3. Call Tick() every frame (or timer) to process inbound messages and drop
 * dead connections.
 *   4. Call BroadcastToSubscribers() to push camera frames to interested
 * clients.
 *   5. Call StopListening() on shutdown.
 *
 * Sending never blocks the caller or holds the connection lock: broadcasting
 * only queues, and the send thread writes to each client what its socket takes
 * without waiting, so a slow or stalled client delays only its own streams.
 */
class RAMMSSTREAMING_API FRammsStreamServer : public FRunnable
{
public:
	explicit FRammsStreamServer(uint16 InPort = 30030, int32 InMaxClients = 8);
	virtual ~FRammsStreamServer();

	// ── Lifecycle ────────────────────────────────────────────────────
	/**
	 * Bind the port and start the accept / send thread. Refuses a port another program already listens on
	 * (no second listener sharing it). False on failure, with the reason in GetLastError().
	 */
	bool StartListening();
	void StopListening();
	bool IsListening() const { return bListening; }

	/** Why the last StartListening failed ("" after a success). */
	const FString& GetLastError() const { return LastError; }

	// ── Configuration ────────────────────────────────────────────────
	/** The port: as configured, and once listening the one actually bound (a configured 0 binds any free one). */
	uint16 GetPort() const { return Port; }
	int32  GetMaxClients() const { return MaxClients; }
	int32  GetConnectionCount() const;

	/** True when a connected client would be sent a message on Channel: it subscribed to it, or to nothing (which
	 *  means everything). Thread-safe. */
	bool HasSubscriber(uint16 Channel) const;

	/** Queue size limits applied to new connections. Outbound: a client is sent the latest frame of each stream
	 *  (see FRammsStreamConnection::EnqueueOutbound), so this bounds the number of streams queued at once. */
	int32 DefaultMaxInboundQueueSize = 64;
	int32 DefaultMaxOutboundQueueSize = 32;

	// ── Frame / message distribution ─────────────────────────────────
	/**
	 * Broadcast a message to all clients subscribed to its channel.
	 * Thread-safe — can be called from any thread.
	 */
	void BroadcastToSubscribers(const TSharedRef<FRammsStreamMessage>& Message);

	/** Send a message to a specific connection. */
	bool SendTo(uint32						   ConnectionId,
		const TSharedRef<FRammsStreamMessage>& Message);

	/**
	 * Process one tick: drop dead connections, collect and dispatch inbound
	 * messages. Should be called from the game thread (e.g., via subsystem Tick).
	 */
	void Tick();

	/** Delegate fired when an inbound message arrives (called during Tick on game
	 * thread). */
	FOnStreamMessageReceived OnMessageReceived;

	// ── Statistics ────────────────────────────────────────────────────
	struct FStats
	{
		TAtomic<uint64> TotalBytesSent{ 0 };
		TAtomic<uint64> TotalBytesReceived{ 0 };
		TAtomic<uint32> FramesSent{ 0 };
		TAtomic<uint32> FramesDropped{ 0 };
		TAtomic<uint32> TotalConnectionsAccepted{ 0 };
	};
	const FStats& GetStats() const { return Stats; }

	// ── FRunnable (accept + send thread) ─────────────────────────────
	virtual bool   Init() override;
	virtual uint32 Run() override;
	virtual void   Stop() override;

private:
	uint16		  Port;
	int32		  MaxClients;
	TAtomic<bool> bListening{ false };
	TAtomic<bool> bStopping{ false };

	FSocket*		 ListenSocket = nullptr;
	FRunnableThread* ServerThread = nullptr;

	/** Wakes the send thread when something is queued (or the server stops). */
	FEvent* SendEvent = nullptr;

	FString LastError;

	// Active connections — guarded by ConnectionsLock, which is only ever held briefly: the send thread copies the
	// list under it and sends outside it.
	mutable FCriticalSection						 ConnectionsLock;
	TMap<uint32, TSharedPtr<FRammsStreamConnection>> Connections;
	TAtomic<uint32>									 NextConnectionId{ 1 };

	FStats Stats;

	/** Called by the accept loop when a new client connects. */
	void OnClientConnected(FSocket* ClientSocket);

	/** Remove dead connections. Called during Tick. */
	void CleanupDisconnected();

	/** Handle subscribe/unsubscribe control messages. */
	void HandleControlMessage(uint32 ConnectionId,
		const FRammsStreamMessage&	 Msg);
};
