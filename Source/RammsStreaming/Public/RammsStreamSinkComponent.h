// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "RammsStreamProtocol.h"
#include "RammsStreamSinkComponent.generated.h"

class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStreamFrameReceived, int32,
	ChannelID, UTexture2D*, Texture,
	const FString&, MetadataJson);

/**
 * Component that receives image data from RMSS streaming clients and
 * creates UTexture2D objects for in-engine display.
 *
 * Listens for IMAGE_DATA messages on specified channels and converts
 * the pixel data into dynamic textures that can be used with materials,
 * UMG Image widgets, or the RammsCameraProvider system.
 */
UCLASS(ClassGroup = (RAMMS), meta = (BlueprintSpawnableComponent))
class RAMMSSTREAMING_API URammsStreamSinkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	URammsStreamSinkComponent();

	/** Channels to listen for incoming IMAGE_DATA and FRAME_DEPTH. Empty = listen
	 * to all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	TArray<int32> ListenChannels;

	/** Fired on game thread when a new frame texture is ready. */
	UPROPERTY(BlueprintAssignable, Category = "RAMMS|Streaming")
	FOnStreamFrameReceived OnFrameReceived;

	/** Get the most recently received texture for a channel. */
	UFUNCTION(BlueprintPure, Category = "RAMMS|Streaming")
	UTexture2D* GetLatestTexture(int32 ChannelID) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void
	TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Pending frames to process on game thread (from server's message dispatch).
	 */
	FCriticalSection			PendingLock;
	TArray<FRammsStreamMessage> PendingFrames;

	/** Cached textures per channel. */
	UPROPERTY()
	TMap<int32, UTexture2D*> ChannelTextures;

	/** Called when the streaming subsystem receives a message. */
	void OnNativeStreamMessage(int32 ConnectionId,
		const FRammsStreamMessage&	 Message);

	/** Process a single IMAGE_DATA message into a texture. */
	void ProcessImageMessage(const FRammsStreamMessage& Msg);

	/** Process a FRAME_DEPTH message: float32 → grayscale BGRA8 texture. */
	void ProcessDepthMessage(const FRammsStreamMessage& Msg);

	/** Create or update a UTexture2D from BGRA8 pixel data. */
	UTexture2D* UpdateTexture(int32 ChannelID, const uint8* Data, int32 Width,
		int32 Height);

	/** Create or update a UTexture2D (R32F) from raw float32 depth data. */
	UTexture2D* UpdateDepthTexture(int32 ChannelID, const uint8* Data,
		int32 Width, int32 Height);
};
