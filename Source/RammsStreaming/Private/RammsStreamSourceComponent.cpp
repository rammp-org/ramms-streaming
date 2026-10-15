// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamSourceComponent.h"
#include "CameraCaptureSubsystem.h"
#include "Dom/JsonObject.h"
#include "RammsStreamProtocol.h"
#include "RammsStreamingSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStreamSource, Log, All);

URammsStreamSourceComponent::URammsStreamSourceComponent()
{
	// Ticks only while waiting to auto-start the capture (cameras register on their own BeginPlay).
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
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

	if (bAutoStartServer)
	{
		URammsStreamingSubsystem* StreamSub = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<URammsStreamingSubsystem>() : nullptr;
		if (StreamSub && !StreamSub->IsServerRunning())
		{
			StreamSub->StartServer(ServerPort);
		}
	}
	if (bAutoStartCapture)
	{
		SetComponentTickEnabled(true);
	}
}

void URammsStreamSourceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UCameraCaptureSubsystem* CaptureSub = GetWorld() ? GetWorld()->GetSubsystem<UCameraCaptureSubsystem>() : nullptr;
	if (!CaptureSub || CaptureSub->IsCapturing())
	{
		SetComponentTickEnabled(false);
		return;
	}
	if (CaptureSub->GetRegisteredCameras().Num() == 0)
	{
		return; // try again next frame
	}

	const bool bDepth = bStreamDepth || bStreamRGBD;
	CaptureSub->SetSerializationEnabled(false);
	CaptureSub->SetCaptureChannels(bStreamRGB || bStreamRGBD, bDepth, false);
	// Without depth, the tone-mapped mode keeps 8-bit colour targets (e.g. the fisheye's) as they are.
	if (!bDepth)
	{
		CaptureSub->SetCaptureMode(ERammsCaptureMode::TonemappedColorPlusDepth);
	}
	CaptureSub->SetCaptureRate(CaptureEveryNFrames);
	CaptureSub->StartCapture();
	bStartedCapture = CaptureSub->IsCapturing();
	UE_LOG(LogRammsStreamSource, Log, TEXT("StreamSource[Ch%d] started capture of %d camera(s), every %d frame(s), depth %s"),
		ChannelID, CaptureSub->GetRegisteredCameras().Num(), CaptureEveryNFrames, bDepth ? TEXT("on") : TEXT("off"));
	SetComponentTickEnabled(false);
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
				if (bStartedCapture)
				{
					CaptureSub->StopCapture();
				}
			}
		}
		CaptureHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void URammsStreamSourceComponent::OnFrameCaptured(
	TSharedRef<const FCaptureData> Data)
{
	// Filter by camera ID if configured
	if (!CameraFilter.IsEmpty() && !Data->CameraID.ToString().Contains(CameraFilter))
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

	// Build metadata JSON
	FString MetaJson = BuildMetadataJson(*Data);

	// Depth may be on its own grid; carry those dimensions rather than letting a
	// client assume the colour ones. Zero means "no depth captured".
	const int32 DepthW = Data->DepthWidth > 0 ? Data->DepthWidth : Data->Width;
	const int32 DepthH = Data->DepthHeight > 0 ? Data->DepthHeight : Data->Height;

	if (bStreamRGBD && Data->ImageData.Num() > 0 && Data->DepthData.Num() > 0)
	{
		// Combined RGBD
		TArray<uint8> RgbBytes;
		RgbBytes.SetNumUninitialized(Data->ImageData.Num() * sizeof(FColor));
		FMemory::Memcpy(RgbBytes.GetData(), Data->ImageData.GetData(),
			RgbBytes.Num());

		TArray<float> DepthCopy = Data->DepthData;
		StreamSub->BroadcastRGBDFrame(ChannelID, RgbBytes, DepthCopy, Data->Width,
			Data->Height, MetaJson, DepthW, DepthH);
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
				Data->Height, MetaJson);
		}

		if (bStreamDepth && Data->DepthData.Num() > 0)
		{
			// Use ChannelID + 100 for depth so clients can subscribe independently
			TArray<float> DepthCopy = Data->DepthData;
			StreamSub->BroadcastDepthFrame(ChannelID + 100, DepthCopy, DepthW,
				DepthH, MetaJson, DepthW, DepthH);
		}
	}
}

FString
URammsStreamSourceComponent::BuildMetadataJson(const FCaptureData& Data) const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetNumberField(TEXT("w"), Data.Width);
	Json->SetNumberField(TEXT("h"), Data.Height);
	if (Data.DepthWidth > 0 && Data.DepthHeight > 0)
	{
		Json->SetNumberField(TEXT("depth_w"), Data.DepthWidth);
		Json->SetNumberField(TEXT("depth_h"), Data.DepthHeight);
	}
	Json->SetNumberField(TEXT("frame"), Data.FrameNumber);
	Json->SetNumberField(TEXT("timestamp"), Data.Timestamp);
	Json->SetStringField(TEXT("camera"), Data.CameraID.ToString());
	Json->SetStringField(TEXT("camera_name"), Data.CameraID.ComponentName);
	Json->SetStringField(TEXT("actor"), Data.ActorPath);
	// Wall-clock kick time, shared by every camera captured together (use it to stamp and group frames).
	Json->SetNumberField(TEXT("unix_time"), Data.CaptureUnixTime);

	// Transform
	const FVector			Loc = Data.WorldTransform.GetLocation();
	const FRotator			Rot = Data.WorldTransform.Rotator();
	TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
	TransformJson->SetNumberField(TEXT("x"), Loc.X);
	TransformJson->SetNumberField(TEXT("y"), Loc.Y);
	TransformJson->SetNumberField(TEXT("z"), Loc.Z);
	TransformJson->SetNumberField(TEXT("pitch"), Rot.Pitch);
	TransformJson->SetNumberField(TEXT("yaw"), Rot.Yaw);
	TransformJson->SetNumberField(TEXT("roll"), Rot.Roll);
	Json->SetObjectField(TEXT("transform"), TransformJson);

	// Unambiguous poses as UE position (cm) + quaternion: the camera relative to its actor's root
	// (the mount extrinsics) and the actor root in the world.
	auto PoseJson = [](const FTransform& T) {
		const FVector			P = T.GetLocation();
		const FQuat				Q = T.GetRotation();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), P.X);
		O->SetNumberField(TEXT("y"), P.Y);
		O->SetNumberField(TEXT("z"), P.Z);
		O->SetNumberField(TEXT("qx"), Q.X);
		O->SetNumberField(TEXT("qy"), Q.Y);
		O->SetNumberField(TEXT("qz"), Q.Z);
		O->SetNumberField(TEXT("qw"), Q.W);
		return O;
	};
	Json->SetObjectField(TEXT("rel_transform"), PoseJson(Data.RelativeTransform));
	Json->SetObjectField(TEXT("actor_transform"), PoseJson(Data.RelativeTransform.Inverse() * Data.WorldTransform));

	// Intrinsics
	TSharedRef<FJsonObject> IntrinsicsJson = MakeShared<FJsonObject>();
	IntrinsicsJson->SetNumberField(TEXT("fx"), Data.Intrinsics.FocalLengthX);
	IntrinsicsJson->SetNumberField(TEXT("fy"), Data.Intrinsics.FocalLengthY);
	IntrinsicsJson->SetNumberField(TEXT("cx"), Data.Intrinsics.PrincipalPointX);
	IntrinsicsJson->SetNumberField(TEXT("cy"), Data.Intrinsics.PrincipalPointY);
	IntrinsicsJson->SetNumberField(TEXT("res_x"), Data.Intrinsics.ImageWidth);
	IntrinsicsJson->SetNumberField(TEXT("res_y"), Data.Intrinsics.ImageHeight);
	Json->SetObjectField(TEXT("intrinsics"), IntrinsicsJson);

	if (!Data.DistortionModel.IsEmpty())
	{
		TSharedRef<FJsonObject>		   DistortionJson = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> K;
		for (int32 i = 0; i < 4; ++i)
		{
			K.Add(MakeShared<FJsonValueNumber>(Data.DistortionCoeffs[i]));
		}
		DistortionJson->SetStringField(TEXT("model"), Data.DistortionModel);
		DistortionJson->SetArrayField(TEXT("k"), K);
		Json->SetObjectField(TEXT("distortion"), DistortionJson);
	}

	// Serialize to string
	FString															 OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
			&OutputString);
	FJsonSerializer::Serialize(Json, Writer);
	return OutputString;
}
