// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "RammsStreamSourceComponent.generated.h"

struct FCaptureData;
class UCameraCaptureSubsystem;
class URammsStreamingSubsystem;

/**
 * Component that bridges the CameraCapture subsystem to the RMSS streaming
 * server.
 *
 * Attach to any actor that also has IntrinsicSceneCaptureComponents.  On
 * BeginPlay, it hooks into the world's CameraCaptureSubsystem and forwards
 * harvested frames to the RammsStreamingSubsystem for network distribution.
 *
 * Configuration:
 *   - ChannelID:       Stream channel (unique per camera)
 *   - bStreamRGB:      Forward RGB pixel data
 *   - bStreamDepth:    Forward depth data
 *   - bStreamRGBD:     Forward combined RGBD (overrides individual RGB/Depth)
 *   - CameraFilter:    If set, only forward frames from cameras whose ID
 * contains this string
 */
UCLASS(ClassGroup = (RAMMS), meta = (BlueprintSpawnableComponent))
class RAMMSSTREAMING_API URammsStreamSourceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	URammsStreamSourceComponent();

	/** Stream channel ID for this source (clients subscribe to channels). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	int32 ChannelID = 0;

	/** Stream RGB frames. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bStreamRGB = true;

	/** Stream depth frames. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bStreamDepth = true;

	/** Stream combined RGBD frames (if true, overrides bStreamRGB and
	 * bStreamDepth). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bStreamRGBD = false;

	/** Optional: only stream frames from cameras whose CameraID contains this
	 * string. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	FString CameraFilter;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Called when a frame is harvested by CameraCaptureSubsystem. */
	void OnFrameCaptured(TSharedRef<const FCaptureData> Data);

	FDelegateHandle CaptureHandle;

	/** Build metadata JSON for a frame. */
	FString BuildMetadataJson(const FCaptureData& Data) const;
};
