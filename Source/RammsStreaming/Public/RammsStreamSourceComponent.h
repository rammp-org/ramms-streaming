// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "RammsCameraStreams.h"
#include "RammsStreamSourceComponent.generated.h"

struct FCaptureData;
struct FCameraIdentifier;
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
 *   - bStreamMotion:   Forward motion vectors (FrameMotion, ChannelID + 200)
 *   - bStreamSegmentation: Forward the class mask (FrameData mono8,
 *                      ChannelID + 300)
 *   - CameraFilter:    If set, only forward frames from cameras whose ID
 * contains this string (or, with bExactCameraFilter, the camera component of
 * that name on this actor)
 *   - StreamName:      The camera's name in the stream ids and "group"
 *
 * Colour, depth, motion and mask frames carry the association metadata
 * (stream_id, group, role, name, fmt) described in RammsCameraStreams.h; the
 * RGBD path sends exactly what it always has.
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

	/** Stream motion vectors: FrameMotion (float32 x,y per pixel, pixels per
	 * frame, fmt rg32f) on ChannelID + 200. Needs the capture subsystem's motion
	 * channel on. Off by default so existing levels send what they always have. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bStreamMotion = false;

	/** Stream the segmentation class mask: FrameData (one uint8 fine class per
	 * pixel, fmt mono8) on ChannelID + 300, with the class table in its
	 * metadata. Needs the capture subsystem's segmentation channel on. Off by
	 * default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bStreamSegmentation = false;

	/** CameraFilter is not a substring of the camera id but the exact name of a
	 * camera component on this component's own actor -- so "front" does not also
	 * forward "front_wide", nor a "front" on another actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	bool bExactCameraFilter = false;

	/** The camera's name in the stream ids (camera/<name>/color ...) and the
	 * "group" key. Empty = the camera component's name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RAMMS|Streaming")
	FString StreamName;

	/** Whether this source forwards frames from a camera with this identity,
	 * owned by the actor at CameraActorPath. */
	bool AcceptsCamera(const FCameraIdentifier& Camera, const FString& CameraActorPath) const;

	/** The camera filter as a pure function: Filter empty accepts everything;
	 * otherwise a substring of the camera's unique id, or with bExact the
	 * camera's component name on the actor at OwnerActorPath. */
	static bool CameraMatchesFilter(const FString& Filter, bool bExact, const FCameraIdentifier& Camera,
		const FString& CameraActorPath, const FString& OwnerActorPath);

	/** The name a camera's streams go out under from this source: StreamName,
	 * else the camera component's name. */
	FString GetStreamNameFor(const FCameraIdentifier& Camera) const;

	/** The streams this source sends for a camera streamed under
	 * CameraStreamName: colour and depth as configured (none when bStreamRGBD,
	 * which sends one combined FrameRGBD on ChannelID without stream ids), then
	 * motion and mask. */
	TArray<FRammsCameraStreamDesc> DescribeStreams(const FString& CameraStreamName) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Called when a frame is harvested by CameraCaptureSubsystem. */
	void OnFrameCaptured(TSharedRef<const FCaptureData> Data);

	FDelegateHandle CaptureHandle;
};
