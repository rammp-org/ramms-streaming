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

	// ── Compression configuration ───────────────────────────────────

	/** Enable JPEG compression for RGB frames and LZ4 for depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bEnableCompression = false;

	/** JPEG quality (1–100) used when bEnableCompression is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming",
		meta = (ClampMin = "1", ClampMax = "100",
			EditCondition = "bEnableCompression"))
	int32 JpegQuality = 85;

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
	 * @param Width      Image width.
	 * @param Height     Image height.
	 * @param MetadataJson  Optional JSON metadata.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastDepthFrame(int32 ChannelID, const TArray<float>& DepthData,
		int32 Width, int32 Height,
		const FString& MetadataJson = TEXT(""));

	/**
	 * Broadcast combined RGBD frame (RGB followed by depth) to all subscribed
	 * clients.
	 */
	UFUNCTION(BlueprintCallable, Category = "RAMMS|Streaming")
	void BroadcastRGBDFrame(int32 ChannelID, const TArray<uint8>& PixelData,
		const TArray<float>& DepthData, int32 Width,
		int32 Height, const FString& MetadataJson = TEXT(""));

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

	/** Internal: build and broadcast a message. */
	void BroadcastMessage(TSharedRef<FRammsStreamMessage> Msg);

	/** Atomic sequence counter per channel. */
	TMap<uint16, uint32> SequenceCounters;
	uint32				 GetNextSequence(uint16 ChannelID);

	/** Compress BGRA8 pixel data to JPEG. Returns empty array on failure. */
	TArray<uint8> CompressJPEG(const TArray<uint8>& PixelData, int32 Width,
		int32 Height, int32 Quality) const;

	/** Compress raw bytes with LZ4. Returns empty array on failure. */
	static TArray<uint8> CompressLZ4(const uint8* Data, int32 DataSize);
};
