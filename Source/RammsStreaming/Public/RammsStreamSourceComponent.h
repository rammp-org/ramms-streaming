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

	/** Start the RMSS server on BeginPlay if nothing else has (e.g. for the ROS 2 bridge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming|Auto Start")
	bool bAutoStartServer = false;

	/** Port used when bAutoStartServer starts the server. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming|Auto Start", meta = (EditCondition = "bAutoStartServer"))
	int32 ServerPort = 30030;

	/**
	 * Start the camera capture subsystem once cameras are registered, so frames flow without a
	 * recording running. Disk serialization is switched off and only the streamed planes are
	 * captured. Skipped if a capture is already running.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming|Auto Start")
	bool bAutoStartCapture = false;

	/** Capture every N game frames when bAutoStartCapture starts the capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming|Auto Start", meta = (EditCondition = "bAutoStartCapture", ClampMin = "1"))
	int32 CaptureEveryNFrames = 3;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Called when a frame is harvested by CameraCaptureSubsystem. */
	void OnFrameCaptured(TSharedRef<const FCaptureData> Data);

	FDelegateHandle CaptureHandle;

	/** True when this component started the capture, so EndPlay stops it again. */
	bool bStartedCapture = false;

	/** Build metadata JSON for a frame. */
	FString BuildMetadataJson(const FCaptureData& Data) const;
};
