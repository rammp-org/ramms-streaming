// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "RammsStreamProtocol.h"

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
 *   2. Call StartListening() — spawns accept thread.
 *   3. Call Tick() every frame (or timer) to process inbound messages and flush
 * outbound.
 *   4. Call BroadcastToSubscribers() to push camera frames to interested
 * clients.
 *   5. Call StopListening() on shutdown.
 */
class RAMMSSTREAMING_API FRammsStreamServer : public FRunnable
{
public:
	explicit FRammsStreamServer(uint16 InPort = 30030, int32 InMaxClients = 8);
	virtual ~FRammsStreamServer();

	// ── Lifecycle ────────────────────────────────────────────────────
	bool StartListening();
	void StopListening();
	bool IsListening() const { return bListening; }

	// ── Configuration ────────────────────────────────────────────────
	uint16 GetPort() const { return Port; }
	int32  GetMaxClients() const { return MaxClients; }
	int32  GetConnectionCount() const;

	/** Queue size limits applied to new connections. */
	int32 DefaultMaxInboundQueueSize = 64;
	int32 DefaultMaxOutboundQueueSize = 3;

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
	 * Process one tick: flush outbound queues, collect and dispatch inbound
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

	// Active connections — guarded by ConnectionsLock
	FCriticalSection								 ConnectionsLock;
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
