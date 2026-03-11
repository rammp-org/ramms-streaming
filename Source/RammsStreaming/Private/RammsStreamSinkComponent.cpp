// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamSinkComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "RammsStreamServer.h"
#include "RammsStreamingSubsystem.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStreamSink, Log, All);

URammsStreamSinkComponent::URammsStreamSinkComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.0f; // every frame
}

void URammsStreamSinkComponent::BeginPlay()
{
	Super::BeginPlay();

	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (!GI)
		return;

	URammsStreamingSubsystem* StreamSub =
		GI->GetSubsystem<URammsStreamingSubsystem>();
	if (!StreamSub)
		return;

	// Bind to the native delegate to receive full message payloads
	StreamSub->OnNativeMessageReceived.AddUObject(
		this, &URammsStreamSinkComponent::OnNativeStreamMessage);

	UE_LOG(LogRammsStreamSink, Log, TEXT("StreamSink listening on %d channels"),
		ListenChannels.Num());
}

void URammsStreamSinkComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (GI)
	{
		URammsStreamingSubsystem* StreamSub =
			GI->GetSubsystem<URammsStreamingSubsystem>();
		if (StreamSub)
		{
			StreamSub->OnNativeMessageReceived.RemoveAll(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void URammsStreamSinkComponent::OnNativeStreamMessage(
	int32 ConnectionId, const FRammsStreamMessage& Msg)
{
	// Called from game thread (subsystem Tick dispatches on game thread)
	if (Msg.Header.MessageType != ERammsStreamMessageType::ImageData && Msg.Header.MessageType != ERammsStreamMessageType::FrameDepth)
	{
		return;
	}

	const int32 Channel = static_cast<int32>(Msg.Header.ChannelID);
	if (ListenChannels.Num() > 0 && !ListenChannels.Contains(Channel))
		return;

	// Queue for processing in TickComponent
	FScopeLock Lock(&PendingLock);
	PendingFrames.Add(Msg);
}

void URammsStreamSinkComponent::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Process pending frames queued by OnNativeStreamMessage
	TArray<FRammsStreamMessage> ToProcess;
	{
		FScopeLock Lock(&PendingLock);
		Swap(ToProcess, PendingFrames);
	}

	for (const auto& Msg : ToProcess)
	{
		if (Msg.Header.MessageType == ERammsStreamMessageType::FrameDepth)
		{
			ProcessDepthMessage(Msg);
		}
		else
		{
			ProcessImageMessage(Msg);
		}
	}
}

void URammsStreamSinkComponent::ProcessImageMessage(
	const FRammsStreamMessage& Msg)
{
	if (Msg.Header.MessageType != ERammsStreamMessageType::ImageData)
		return;

	const int32 Channel = static_cast<int32>(Msg.Header.ChannelID);

	// Check channel filter
	if (ListenChannels.Num() > 0 && !ListenChannels.Contains(Channel))
	{
		return;
	}

	// Parse metadata
	FString					  MetaStr = Msg.GetMetadataString();
	TSharedPtr<FJsonObject>	  Meta;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MetaStr);
	if (!FJsonSerializer::Deserialize(Reader, Meta) || !Meta.IsValid())
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Bad metadata in IMAGE_DATA on channel %d"), Channel);
		return;
	}

	const int32	  Width = static_cast<int32>(Meta->GetNumberField(TEXT("w")));
	const int32	  Height = static_cast<int32>(Meta->GetNumberField(TEXT("h")));
	const FString Fmt = Meta->GetStringField(TEXT("fmt"));

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Invalid dimensions %dx%d on channel %d"), Width, Height,
			Channel);
		return;
	}

	// Only support BGRA8 for now
	if (Fmt != TEXT("bgra8") && Fmt != TEXT("rgba8"))
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Unsupported format '%s' on channel %d (expected bgra8)"), *Fmt,
			Channel);
		return;
	}

	const int32 ExpectedSize = Width * Height * 4;
	if (Msg.Payload.Num() < ExpectedSize)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Payload too small: %d < %d on channel %d"), Msg.Payload.Num(),
			ExpectedSize, Channel);
		return;
	}

	UTexture2D* Tex =
		UpdateTexture(Channel, Msg.Payload.GetData(), Width, Height);
	if (Tex)
	{
		OnFrameReceived.Broadcast(Channel, Tex, MetaStr);
	}
}

void URammsStreamSinkComponent::ProcessDepthMessage(
	const FRammsStreamMessage& Msg)
{
	if (Msg.Header.MessageType != ERammsStreamMessageType::FrameDepth)
		return;

	const int32 Channel = static_cast<int32>(Msg.Header.ChannelID);

	if (ListenChannels.Num() > 0 && !ListenChannels.Contains(Channel))
	{
		return;
	}

	// Parse metadata for dimensions
	FString					  MetaStr = Msg.GetMetadataString();
	TSharedPtr<FJsonObject>	  Meta;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MetaStr);
	if (!FJsonSerializer::Deserialize(Reader, Meta) || !Meta.IsValid())
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Bad metadata in FRAME_DEPTH on channel %d"), Channel);
		return;
	}

	const int32 Width = static_cast<int32>(Meta->GetNumberField(TEXT("w")));
	const int32 Height = static_cast<int32>(Meta->GetNumberField(TEXT("h")));

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Invalid depth dimensions %dx%d on channel %d"), Width, Height,
			Channel);
		return;
	}

	const int32 NumPixels = Width * Height;
	const int32 ExpectedBytes = NumPixels * static_cast<int32>(sizeof(float));
	if (Msg.Payload.Num() < ExpectedBytes)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Depth payload too small: %d < %d on channel %d"),
			Msg.Payload.Num(), ExpectedBytes, Channel);
		return;
	}

	// Store raw float32 depth in an R32F texture (no conversion)
	UTexture2D* Tex =
		UpdateDepthTexture(Channel, Msg.Payload.GetData(), Width, Height);
	if (Tex)
	{
		OnFrameReceived.Broadcast(Channel, Tex, MetaStr);
	}
}

UTexture2D* URammsStreamSinkComponent::UpdateTexture(int32 ChannelID,
	const uint8*										   Data,
	int32												   Width,
	int32												   Height)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	// Create new texture if dimensions changed or first time
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height)
	{
		Tex = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!Tex)
		{
			UE_LOG(LogRammsStreamSink, Error,
				TEXT("Failed to create texture %dx%d for channel %d"), Width,
				Height, ChannelID);
			return nullptr;
		}
		Tex->SRGB = true;
		Tex->Filter = TF_Bilinear;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		ChannelTextures.Add(ChannelID, Tex);
	}

	// Update pixel data
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void*			  MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Data, Width * Height * 4);
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	return Tex;
}

UTexture2D* URammsStreamSinkComponent::UpdateDepthTexture(int32 ChannelID,
	const uint8*												Data,
	int32														Width,
	int32														Height)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	const int32 ByteCount = Width * Height * static_cast<int32>(sizeof(float));

	// Create new R32F texture if dimensions changed or first time
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height)
	{
		Tex = UTexture2D::CreateTransient(Width, Height, PF_R32_FLOAT);
		if (!Tex)
		{
			UE_LOG(LogRammsStreamSink, Error,
				TEXT("Failed to create R32F texture %dx%d for channel %d"), Width,
				Height, ChannelID);
			return nullptr;
		}
		Tex->SRGB = false;
		Tex->Filter = TF_Bilinear;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		ChannelTextures.Add(ChannelID, Tex);
	}

	// Update raw float32 pixel data
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void*			  MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Data, ByteCount);
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	return Tex;
}

UTexture2D* URammsStreamSinkComponent::GetLatestTexture(int32 ChannelID) const
{
	const UTexture2D* const* Found = ChannelTextures.Find(ChannelID);
	return Found ? const_cast<UTexture2D*>(*Found) : nullptr;
}
