// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RammsStreamProtocol.h"
#include "RammsStreamServer.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "RammsStreamingSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRammsStreamMessage, int32,
	ConnectionId,
	ERammsStreamMessageType,
	MessageType);

/**
 * Game-instance subsystem that owns the RMSS streaming server.
 *
 * Persists across level transitions. Provides Blueprint-accessible API
 * for starting/stopping the server and broadcasting frames.
 */
UCLASS()
class RAMMSSTREAMING_API URammsStreamingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	URammsStreamingSubsystem();
	virtual ~URammsStreamingSubsystem();

	// ── USubsystem ───────────────────────────────────────────────────
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Server control ───────────────────────────────────────────────
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	bool StartServer(int32 Port = 30030, int32 MaxClients = 8);

	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void StopServer();

	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	bool IsServerRunning() const;

	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	int32 GetConnectionCount() const;

	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	int32 GetServerPort() const;

	/** Why the last StartServer failed (a port another program listens on, say); "" after a success. */
	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	FString GetLastServerError() const { return LastServerError; }

	/** True when the server is running and a connected client would be sent a frame on ChannelID: it subscribed to
	 *  that channel, or to every channel (a Subscribe without channels). A client that never subscribed is sent no
	 *  frames and does not count. Frame broadcasts to a channel nobody wants are skipped. */
	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	bool IsChannelWanted(int32 ChannelID) const;

	/**
	 * C++: the key that tells one stream from another on the same channel and message type (FRammsStreamConnection::
	 * EnqueueOutbound): a newer frame of a stream takes the place of its queued older one, and frames of different
	 * streams -- several cameras a source sends on one channel -- never take each other's. Made from the stream id,
	 * the actor and the camera; 0 when all three are empty (the channel and type's only stream).
	 */
	static uint64 MakeStreamKey(const FString& StreamId, const FString& Actor, const FString& Camera);

	/** C++: MakeStreamKey from a frame's metadata JSON ("stream_id", "actor", "camera"); 0 without any of them. */
	static uint64 StreamKeyFromMetadata(const FString& MetadataJson);

	// ── Compression configuration ───────────────────────────────────

	/** Enable JPEG compression for RGB frames and LZ4 for depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bEnableCompression = false;

	/** JPEG quality (1–100) used when bEnableCompression is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming",
		meta = (ClampMin = "1", ClampMax = "100",
			EditCondition = "bEnableCompression"))
	int32 JpegQuality = 85;

	// ── Connection queue configuration ──────────────────────────────

	/** Maximum inbound messages queued per connection before oldest are dropped.
	 *  Applied to new connections when the server accepts them.
	 *  0 = unlimited (legacy behavior, preserved by default). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming",
		meta = (ClampMin = "0"))
	int32 MaxInboundQueueSize = 0;

	/** Maximum outbound messages queued per connection. A client is sent the latest frame of each stream (a newer
	 *  frame takes the place of a queued older one of the same stream), so this bounds how many streams can wait at
	 *  once; past it the oldest queued frame is dropped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming",
		meta = (ClampMin = "1"))
	int32 MaxOutboundQueueSize = 32;

	// ── Frame broadcasting ───────────────────────────────────────────

	/**
	 * Broadcast raw RGB frame data to all subscribed clients.
	 * @param ChannelID  Identifies this camera/stream.
	 * @param PixelData  BGRA8 pixel data.
	 * @param Width      Image width.
	 * @param Height     Image height.
	 * @param MetadataJson  Optional JSON metadata (intrinsics, transform, etc.).
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastRGBFrame(int32 ChannelID, const TArray<uint8>& PixelData,
		int32 Width, int32 Height,
		const FString& MetadataJson = TEXT(""));

	/**
	 * Broadcast raw depth frame data to all subscribed clients.
	 * @param ChannelID  Identifies this camera/stream.
	 * @param DepthData  float32 depth values (cm).
	 * @param DepthWidth/DepthHeight  the grid DepthData is actually in. A camera
	 *        with separate depth intrinsics captures depth at its own resolution,
	 *        so this is NOT Width/Height in general. Zero means "same as
	 *        Width/Height", which is the common case and the old behaviour.
	 * @param Width      Image width.
	 * @param Height     Image height.
	 * @param MetadataJson  Optional JSON metadata.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastDepthFrame(int32 ChannelID, const TArray<float>& DepthData,
		int32 Width, int32 Height,
		const FString& MetadataJson = TEXT(""),
		int32 DepthWidth = 0, int32 DepthHeight = 0);

	/**
	 * Broadcast combined RGBD frame (RGB followed by depth) to all subscribed
	 * clients.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastRGBDFrame(int32 ChannelID, const TArray<uint8>& PixelData,
		const TArray<float>& DepthData, int32 Width,
		int32 Height, const FString& MetadataJson = TEXT(""),
		int32 DepthWidth = 0, int32 DepthHeight = 0);

	/**
	 * Broadcast a motion-vector frame (FrameMotion) to all subscribed clients.
	 * @param ChannelID   Identifies this stream (a camera source sends motion on
	 *                    its ChannelID + 200).
	 * @param MotionData  Interleaved float32 x,y per pixel (pixels per frame),
	 *                    row-major: Width * Height * 2 values.
	 * @param Width/Height  The motion grid. "w"/"h", "motion_w"/"motion_h" and
	 *                    fmt "rg32f" are added to MetadataJson. Never compressed.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastMotionFrame(int32 ChannelID, const TArray<float>& MotionData,
		int32 Width, int32 Height, const FString& MetadataJson = TEXT(""));

	/**
	 * Broadcast a generic frame (FrameData) whose pixel format is named by
	 * Format ("mono8", "float32", "rg32f", "bgra8" ...) to all subscribed
	 * clients. "w"/"h" and "fmt" are added to MetadataJson. Never compressed.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastFrameData(int32 ChannelID, const TArray<uint8>& Data,
		int32 Width, int32 Height, const FString& Format,
		const FString& MetadataJson = TEXT(""));

	/**
	 * C++: broadcast a frame whose payload is already in its wire format, taking the payload over (no copy).
	 * MetadataJson is sent as given, so it must describe the payload ("w", "h", "fmt" ...). FrameRGB (BGRA8 of
	 * Width x Height) and FrameDepth payloads are JPEG / LZ4 compressed like BroadcastRGBFrame and
	 * BroadcastDepthFrame when bEnableCompression is on; any other type is sent as is. Skipped when nobody
	 * subscribes to ChannelID. StreamKey: the frame's stream (MakeStreamKey); 0 takes it from MetadataJson.
	 */
	void BroadcastEncodedFrame(ERammsStreamMessageType Type, int32 ChannelID, TArray<uint8>&& Payload, int32 Width,
		int32 Height, const FString& MetadataJson, uint64 StreamKey = 0);

	// ── Events ───────────────────────────────────────────────────────

	/** Fired on the game thread when an inbound message arrives from a client. */
	UPROPERTY(BlueprintAssignable, Category = "RAMMS|Streaming")
	FOnRammsStreamMessage OnMessageReceived;

	/**
	 * Native delegate with full message data.  Sinks should bind to this
	 * to receive IMAGE_DATA (or any inbound) messages with payload.
	 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNativeStreamMessage,
		int32 /*ConnectionId*/,
		const FRammsStreamMessage& /*Message*/);
	FOnNativeStreamMessage OnNativeMessageReceived;

	// ── Direct access ────────────────────────────────────────────────
	FRammsStreamServer* GetServer() const { return Server.Get(); }

private:
	TUniquePtr<FRammsStreamServer> Server;

	FTSTicker::FDelegateHandle TickHandle;
	void					   OnTick(float DeltaTime);

	/** Internal: broadcast a built message as a frame of the stream StreamKey (see MakeStreamKey). */
	void BroadcastMessage(TSharedRef<FRammsStreamMessage> Msg, uint64 StreamKey);

	/** A frame message of Type on ChannelID with its sequence number and timestamp, or null when the channel is
	 *  out of range or nobody would be sent it. */
	TSharedPtr<FRammsStreamMessage> NewFrameMessage(ERammsStreamMessageType Type, int32 ChannelID);

	FString LastServerError;

	/** Atomic sequence counter per channel. */
	TMap<uint16, uint32> SequenceCounters;
	uint32				 GetNextSequence(uint16 ChannelID);

	/** Compress BGRA8 pixel data to JPEG. Returns empty array on failure. */
	TArray<uint8> CompressJPEG(const TArray<uint8>& PixelData, int32 Width,
		int32 Height, int32 Quality) const;

	/** Compress raw bytes with LZ4. Returns empty array on failure. */
	static TArray<uint8> CompressLZ4(const uint8* Data, int32 DataSize);
};
