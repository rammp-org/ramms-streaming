// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamSourceComponent.h"
#include "CameraCaptureSegmentation.h"
#include "CameraCaptureSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "RammsCameraStreams.h"
#include "RammsStreamProtocol.h"
#include "RammsStreamingSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStreamSource, Log, All);

URammsStreamSourceComponent::URammsStreamSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void URammsStreamSourceComponent::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
		return;

	UCameraCaptureSubsystem* CaptureSub =
		World->GetSubsystem<UCameraCaptureSubsystem>();
	if (!CaptureSub)
	{
		UE_LOG(
			LogRammsStreamSource, Warning,
			TEXT("CameraCaptureSubsystem not found — streaming source disabled"));
		return;
	}

	CaptureHandle = CaptureSub->OnFrameCaptured.AddUObject(
		this, &URammsStreamSourceComponent::OnFrameCaptured);
	UE_LOG(LogRammsStreamSource, Log,
		TEXT("StreamSource[Ch%d] hooked into CameraCapture (filter='%s')"),
		ChannelID, *CameraFilter);
}

void URammsStreamSourceComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (CaptureHandle.IsValid())
	{
		UWorld* World = GetWorld();
		if (World)
		{
			UCameraCaptureSubsystem* CaptureSub =
				World->GetSubsystem<UCameraCaptureSubsystem>();
			if (CaptureSub)
			{
				CaptureSub->OnFrameCaptured.Remove(CaptureHandle);
			}
		}
		CaptureHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

bool URammsStreamSourceComponent::CameraMatchesFilter(const FString& Filter,
	bool bExact, const FCameraIdentifier& Camera,
	const FString& CameraActorPath, const FString& OwnerActorPath)
{
	if (Filter.IsEmpty())
	{
		return true;
	}
	if (bExact)
	{
		// The camera component of exactly that name, on this actor: a substring
		// of "Actor::Component" would also take "front_wide" for "front", or
		// another actor's "front".
		return Camera.ComponentName.Equals(Filter, ESearchCase::IgnoreCase)
			&& !OwnerActorPath.IsEmpty() && CameraActorPath == OwnerActorPath;
	}
	return Camera.ToString().Contains(Filter);
}

bool URammsStreamSourceComponent::AcceptsCamera(
	const FCameraIdentifier& Camera, const FString& CameraActorPath) const
{
	const AActor* Owner = GetOwner();
	return CameraMatchesFilter(CameraFilter, bExactCameraFilter, Camera,
		CameraActorPath, Owner ? Owner->GetPathName() : FString());
}

FString URammsStreamSourceComponent::GetStreamNameFor(
	const FCameraIdentifier& Camera) const
{
	if (!StreamName.IsEmpty())
	{
		return StreamName;
	}
	return Camera.ComponentName.IsEmpty() ? Camera.ToString() : Camera.ComponentName;
}

TArray<FRammsCameraStreamDesc> URammsStreamSourceComponent::DescribeStreams(
	const FString& CameraStreamName) const
{
	TArray<FRammsCameraStreamDesc> Streams;
	if (!bStreamRGBD)
	{
		if (bStreamRGB)
		{
			Streams.Add(FRammsCameraStreams::Describe(CameraStreamName, ERammsCameraStreamRole::Color, ChannelID));
		}
		if (bStreamDepth)
		{
			Streams.Add(FRammsCameraStreams::Describe(CameraStreamName, ERammsCameraStreamRole::Depth, ChannelID));
		}
	}
	if (bStreamMotion)
	{
		Streams.Add(FRammsCameraStreams::Describe(CameraStreamName, ERammsCameraStreamRole::Motion, ChannelID));
	}
	if (bStreamSegmentation)
	{
		Streams.Add(FRammsCameraStreams::Describe(CameraStreamName, ERammsCameraStreamRole::Mask, ChannelID));
	}
	return Streams;
}

void URammsStreamSourceComponent::OnFrameCaptured(
	TSharedRef<const FCaptureData> Data)
{
	// Filter by camera ID if configured
	if (!AcceptsCamera(Data->CameraID, Data->ActorPath))
	{
		return;
	}

	// Get the streaming subsystem
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (!GI)
		return;
	URammsStreamingSubsystem* StreamSub =
		GI->GetSubsystem<URammsStreamingSubsystem>();
	if (!StreamSub || !StreamSub->IsServerRunning())
		return;

	const FString CameraStreamName = GetStreamNameFor(Data->CameraID);

	// Depth may be on its own grid; carry those dimensions rather than letting a
	// client assume the colour ones. Zero means "no depth captured".
	const int32 DepthW = Data->DepthWidth > 0 ? Data->DepthWidth : Data->Width;
	const int32 DepthH = Data->DepthHeight > 0 ? Data->DepthHeight : Data->Height;

	if (bStreamRGBD && Data->ImageData.Num() > 0 && Data->DepthData.Num() > 0)
	{
		// Combined RGBD, with the metadata it has always had: one message is two
		// roles, so it carries no single stream id.
		FString RgbdMeta;
		{
			const TSharedRef<FJsonObject>										   Base = FRammsCameraStreams::BuildBaseMetadata(*Data);
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RgbdMeta);
			FJsonSerializer::Serialize(Base, Writer);
		}

		TArray<uint8> RgbBytes;
		RgbBytes.SetNumUninitialized(Data->ImageData.Num() * sizeof(FColor));
		FMemory::Memcpy(RgbBytes.GetData(), Data->ImageData.GetData(),
			RgbBytes.Num());

		TArray<float> DepthCopy = Data->DepthData;
		StreamSub->BroadcastRGBDFrame(ChannelID, RgbBytes, DepthCopy, Data->Width,
			Data->Height, RgbdMeta, DepthW, DepthH);
	}
	else
	{
		if (bStreamRGB && Data->ImageData.Num() > 0)
		{
			TArray<uint8> RgbBytes;
			RgbBytes.SetNumUninitialized(Data->ImageData.Num() * sizeof(FColor));
			FMemory::Memcpy(RgbBytes.GetData(), Data->ImageData.GetData(),
				RgbBytes.Num());
			StreamSub->BroadcastRGBFrame(ChannelID, RgbBytes, Data->Width,
				Data->Height,
				FRammsCameraStreams::BuildFrameMetadataJson(*Data, CameraStreamName, ERammsCameraStreamRole::Color));
		}

		if (bStreamDepth && Data->DepthData.Num() > 0)
		{
			// Use ChannelID + 100 for depth so clients can subscribe independently
			TArray<float> DepthCopy = Data->DepthData;
			StreamSub->BroadcastDepthFrame(ChannelID + FRammsCameraStreams::DepthChannelOffset, DepthCopy, DepthW,
				DepthH,
				FRammsCameraStreams::BuildFrameMetadataJson(*Data, CameraStreamName, ERammsCameraStreamRole::Depth),
				DepthW, DepthH);
		}
	}

	// Motion: its own pass and its own grid. FVector2D is double; the wire is
	// float32 x,y.
	const int32 MotionPixels = Data->MotionWidth * Data->MotionHeight;
	if (bStreamMotion && MotionPixels > 0 && Data->MotionVectorData.Num() == MotionPixels)
	{
		TArray<float> MotionFloats;
		MotionFloats.SetNumUninitialized(MotionPixels * 2);
		for (int32 Pixel = 0; Pixel < MotionPixels; ++Pixel)
		{
			const FVector2D& Velocity = Data->MotionVectorData[Pixel];
			MotionFloats[Pixel * 2] = static_cast<float>(Velocity.X);
			MotionFloats[Pixel * 2 + 1] = static_cast<float>(Velocity.Y);
		}
		StreamSub->BroadcastMotionFrame(ChannelID + FRammsCameraStreams::MotionChannelOffset, MotionFloats,
			Data->MotionWidth, Data->MotionHeight,
			FRammsCameraStreams::BuildFrameMetadataJson(*Data, CameraStreamName, ERammsCameraStreamRole::Motion));
	}

	// Mask: one fine class per pixel on the colour grid, with the class table so
	// a client can name the values.
	const int32 ClassPixels = Data->ClassWidth * Data->ClassHeight;
	if (bStreamSegmentation && ClassPixels > 0 && Data->ClassData.Num() == ClassPixels)
	{
		const UCameraCaptureSegmentationSubsystem* Labels =
			GetWorld() ? GetWorld()->GetSubsystem<UCameraCaptureSegmentationSubsystem>() : nullptr;
		TSharedPtr<const FCameraCaptureSegTable> SegTable;
		if (Labels)
		{
			SegTable = Labels->GetTable();
		}
		StreamSub->BroadcastFrameData(ChannelID + FRammsCameraStreams::MaskChannelOffset, Data->ClassData,
			Data->ClassWidth, Data->ClassHeight, FRammsCameraStreams::Format(ERammsCameraStreamRole::Mask),
			FRammsCameraStreams::BuildFrameMetadataJson(*Data, CameraStreamName, ERammsCameraStreamRole::Mask,
				SegTable.Get()));
	}
}
