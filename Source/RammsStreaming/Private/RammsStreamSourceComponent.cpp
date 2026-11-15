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
	// Only an exact filter forwards a single camera; with a substring filter one StreamName would give several
	// cameras the same stream ids.
	if (bExactCameraFilter && !StreamName.IsEmpty())
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

	if (bStreamRGBD && Data->ImageData.Num() > 0 && Data->DepthData.Num() > 0)
	{
		if (StreamSub->IsChannelWanted(ChannelID))
		{
			// Combined RGBD, with the metadata it has always had: one message is two
			// roles, so it carries no single stream id. Depth may be on its own grid.
			FString RgbdMeta;
			{
				const TSharedRef<FJsonObject>										   Base = FRammsCameraStreams::BuildBaseMetadata(*Data);
				const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RgbdMeta);
				FJsonSerializer::Serialize(Base, Writer);
			}

			TArray<uint8> RgbBytes;
			RgbBytes.Append(reinterpret_cast<const uint8*>(Data->ImageData.GetData()),
				Data->ImageData.Num() * static_cast<int32>(sizeof(FColor)));
			const int32 DepthW = Data->DepthWidth > 0 ? Data->DepthWidth : Data->Width;
			const int32 DepthH = Data->DepthHeight > 0 ? Data->DepthHeight : Data->Height;
			StreamSub->BroadcastRGBDFrame(ChannelID, RgbBytes, Data->DepthData, Data->Width,
				Data->Height, RgbdMeta, DepthW, DepthH);
		}
	}

	// Each role on its own channel, built only when a client wants that channel: the payload is made once, in its
	// wire format, and handed over without further copies.
	TArray<ERammsCameraStreamRole, TInlineAllocator<4>> Roles;
	if (!bStreamRGBD && bStreamRGB)
	{
		Roles.Add(ERammsCameraStreamRole::Color);
	}
	if (!bStreamRGBD && bStreamDepth)
	{
		Roles.Add(ERammsCameraStreamRole::Depth);
	}
	if (bStreamMotion)
	{
		Roles.Add(ERammsCameraStreamRole::Motion);
	}
	if (bStreamSegmentation)
	{
		Roles.Add(ERammsCameraStreamRole::Mask);
	}

	FString CameraStreamName;
	for (const ERammsCameraStreamRole Role : Roles)
	{
		// The role's channel must fit the header's uint16: wrapped, it would land on another stream.
		const int32 RoleChannel = ChannelID + FRammsCameraStreams::ChannelOffset(Role);
		if (RoleChannel < 0 || RoleChannel > MAX_uint16)
		{
			if (!bWarnedInvalidChannel)
			{
				bWarnedInvalidChannel = true;
				UE_LOG(LogRammsStreamSource, Warning,
					TEXT("%s: ChannelID %d puts the %s stream on channel %d, outside 0..65535; that stream is not sent"),
					*GetPathName(), ChannelID, *FRammsCameraStreams::RoleName(Role), RoleChannel);
			}
			continue;
		}
		if (!FRammsCameraStreams::HasRole(*Data, Role) || !StreamSub->IsChannelWanted(RoleChannel))
		{
			continue;
		}
		if (CameraStreamName.IsEmpty())
		{
			CameraStreamName = GetStreamNameFor(Data->CameraID);
		}

		// The mask's class table, so a client can name the values.
		TSharedPtr<const FCameraCaptureSegTable> SegTable;
		if (Role == ERammsCameraStreamRole::Mask)
		{
			if (const UCameraCaptureSegmentationSubsystem* Labels =
					GetWorld() ? GetWorld()->GetSubsystem<UCameraCaptureSegmentationSubsystem>() : nullptr)
			{
				SegTable = Labels->GetTable();
			}
		}

		FRammsCameraFrameOut Frame;
		if (FRammsCameraStreams::BuildFrame(*Data, CameraStreamName, Role, ChannelID, SegTable.Get(), Frame))
		{
			StreamSub->BroadcastEncodedFrame(Frame.Message, Frame.Channel, MoveTemp(Frame.Payload), Frame.Width,
				Frame.Height, Frame.MetadataJson);
		}
	}
}
