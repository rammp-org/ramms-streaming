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
	const auto Type = Msg.Header.MessageType;
	const bool bIsFrameMessage =
		Type == ERammsStreamMessageType::ImageData || Type == ERammsStreamMessageType::FrameDepth || Type == ERammsStreamMessageType::FrameMotion || Type == ERammsStreamMessageType::FrameData;
	if (!bIsFrameMessage)
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
		switch (Msg.Header.MessageType)
		{
			case ERammsStreamMessageType::FrameDepth:
				ProcessDepthMessage(Msg);
				break;
			case ERammsStreamMessageType::FrameMotion:
			case ERammsStreamMessageType::FrameData:
				ProcessFrameDataMessage(Msg);
				break;
			default:
				ProcessImageMessage(Msg);
				break;
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
		OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
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
		OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
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

// --- Format-driven frame processing (FrameMotion, FrameData, future types) ---

namespace
{
	struct FResolvedFormat
	{
		EPixelFormat Format;
		int32		 BytesPerPixel;
		bool		 bSRGB;
	};

	/** Map "fmt" metadata string → pixel format.  Returns false if unknown. */
	bool ResolvePixelFormat(const FString& Fmt, FResolvedFormat& Out)
	{
		if (Fmt == TEXT("bgra8") || Fmt == TEXT("rgba8"))
		{
			Out = { PF_B8G8R8A8, 4, true };
			return true;
		}
		if (Fmt == TEXT("float32") || Fmt == TEXT("r32f") || Fmt == TEXT("depth"))
		{
			Out = { PF_R32_FLOAT, 4, false };
			return true;
		}
		if (Fmt == TEXT("float32x2") || Fmt == TEXT("rg32f"))
		{
			Out = { PF_G32R32F, 8, false };
			return true;
		}
		if (Fmt == TEXT("float32x4") || Fmt == TEXT("rgba32f"))
		{
			Out = { PF_A32B32G32R32F, 16, false };
			return true;
		}
		if (Fmt == TEXT("float16") || Fmt == TEXT("r16f"))
		{
			Out = { PF_R16F, 2, false };
			return true;
		}
		if (Fmt == TEXT("float16x2") || Fmt == TEXT("rg16f"))
		{
			Out = { PF_G16R16F, 4, false };
			return true;
		}
		if (Fmt == TEXT("float16x4") || Fmt == TEXT("rgba16f"))
		{
			Out = { PF_FloatRGBA, 8, false };
			return true;
		}
		if (Fmt == TEXT("r8") || Fmt == TEXT("gray8"))
		{
			Out = { PF_G8, 1, false };
			return true;
		}
		return false;
	}
} // namespace

void URammsStreamSinkComponent::ProcessFrameDataMessage(
	const FRammsStreamMessage& Msg)
{
	const int32 Channel = static_cast<int32>(Msg.Header.ChannelID);

	if (ListenChannels.Num() > 0 && !ListenChannels.Contains(Channel))
		return;

	// Parse metadata
	FString					  MetaStr = Msg.GetMetadataString();
	TSharedPtr<FJsonObject>	  Meta;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MetaStr);
	if (!FJsonSerializer::Deserialize(Reader, Meta) || !Meta.IsValid())
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Bad metadata in frame message (type=0x%02X) on channel %d"),
			static_cast<uint8>(Msg.Header.MessageType), Channel);
		return;
	}

	// Safe metadata extraction — missing or non-numeric fields yield 0
	double WVal = 0.0, HVal = 0.0;
	Meta->TryGetNumberField(TEXT("w"), WVal);
	Meta->TryGetNumberField(TEXT("h"), HVal);
	const int32 Width = static_cast<int32>(WVal);
	const int32 Height = static_cast<int32>(HVal);

	FString Fmt;
	if (Meta->TryGetStringField(TEXT("fmt"), Fmt))
	{
		Fmt = Fmt.ToLower();
	}

	static constexpr int32 MaxDimension = 16384;
	if (Width <= 0 || Height <= 0 || Width > MaxDimension || Height > MaxDimension)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Invalid dimensions %dx%d on channel %d (max %d)"),
			Width, Height, Channel, MaxDimension);
		return;
	}

	// Default format based on message type
	FString EffectiveFmt = Fmt;
	if (EffectiveFmt.IsEmpty())
	{
		if (Msg.Header.MessageType == ERammsStreamMessageType::FrameMotion)
			EffectiveFmt = TEXT("float32x2");
		else
			EffectiveFmt = TEXT("bgra8");
	}

	FResolvedFormat FormatInfo;
	if (!ResolvePixelFormat(EffectiveFmt, FormatInfo))
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Unsupported fmt '%s' on channel %d"), *EffectiveFmt, Channel);
		return;
	}

	const int64 ExpectedBytes =
		static_cast<int64>(Width) * static_cast<int64>(Height) * static_cast<int64>(FormatInfo.BytesPerPixel);
	if (Msg.Payload.Num() < ExpectedBytes)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Payload too small for fmt '%s': %d < %lld on channel %d"),
			*EffectiveFmt, Msg.Payload.Num(), ExpectedBytes, Channel);
		return;
	}

	UTexture2D* Tex = UpdateGenericTexture(
		Channel, Msg.Payload.GetData(), Width, Height,
		FormatInfo.Format, FormatInfo.bSRGB);
	if (Tex)
	{
		OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
	}
}

UTexture2D* URammsStreamSinkComponent::UpdateGenericTexture(int32 ChannelID,
	const uint8* Data, int32 Width, int32 Height,
	EPixelFormat Format, bool bIsSRGB)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	// Recreate if dimensions or format changed
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height || Tex->GetPixelFormat() != Format)
	{
		Tex = UTexture2D::CreateTransient(Width, Height, Format);
		if (!Tex)
		{
			UE_LOG(LogRammsStreamSink, Error,
				TEXT("Failed to create texture %dx%d (fmt=%d) for channel %d"),
				Width, Height, static_cast<int32>(Format), ChannelID);
			return nullptr;
		}
		Tex->SRGB = bIsSRGB;
		Tex->Filter = TF_Bilinear;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		ChannelTextures.Add(ChannelID, Tex);
	}

	const int64 BytesPerPixel = static_cast<int64>(GPixelFormats[Format].BlockBytes);
	const int64 ByteCount =
		static_cast<int64>(Width) * static_cast<int64>(Height) * BytesPerPixel;

	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	const int64		  BulkSize = Mip.BulkData.GetBulkDataSize();
	if (BulkSize < ByteCount)
	{
		UE_LOG(LogRammsStreamSink, Error,
			TEXT("Mip bulk data (%lld) smaller than expected (%lld) for channel %d"),
			BulkSize, ByteCount, ChannelID);
		return nullptr;
	}

	void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Data, static_cast<SIZE_T>(ByteCount));
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	return Tex;
}
