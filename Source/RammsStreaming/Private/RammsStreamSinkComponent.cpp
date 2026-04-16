// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamSinkComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "RammsStreamServer.h"
#include "RammsStreamingSubsystem.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStreamSink, Log, All);

// --- Format resolution (used by ProcessImageMessage and ProcessFrameDataMessage) ---

namespace
{
	struct FResolvedFormat
	{
		EPixelFormat Format;
		bool		 bSRGB;
	};

	/** Map "fmt" metadata string → pixel format.  Returns false if unknown.
	 *  Note: "rgba8" is intentionally excluded — it requires an RGBA→BGRA swizzle
	 *  that ProcessImageMessage handles; the generic path would render with swapped channels. */
	bool ResolvePixelFormat(const FString& Fmt, FResolvedFormat& Out)
	{
		if (Fmt == TEXT("bgra8"))
		{
			Out = { PF_B8G8R8A8, true };
			return true;
		}
		if (Fmt == TEXT("float32") || Fmt == TEXT("r32f") || Fmt == TEXT("depth"))
		{
			Out = { PF_R32_FLOAT, false };
			return true;
		}
		if (Fmt == TEXT("float32x2") || Fmt == TEXT("rg32f"))
		{
			Out = { PF_G32R32F, false };
			return true;
		}
		if (Fmt == TEXT("float32x4") || Fmt == TEXT("rgba32f"))
		{
			Out = { PF_A32B32G32R32F, false };
			return true;
		}
		if (Fmt == TEXT("float16") || Fmt == TEXT("r16f"))
		{
			Out = { PF_R16F, false };
			return true;
		}
		if (Fmt == TEXT("float16x2") || Fmt == TEXT("rg16f"))
		{
			Out = { PF_G16R16F, false };
			return true;
		}
		if (Fmt == TEXT("float16x4") || Fmt == TEXT("rgba16f"))
		{
			Out = { PF_FloatRGBA, false };
			return true;
		}
		if (Fmt == TEXT("r8") || Fmt == TEXT("gray8") || Fmt == TEXT("mono8"))
		{
			Out = { PF_G8, false };
			return true;
		}
		return false;
	}
} // namespace

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

	// Keep only the latest frame per (channel, message-type) pair to avoid
	// redundant texture uploads when the sender is faster than the frame rate.
	// Different message types on the same channel (e.g., RGB + Depth) are
	// kept independently.
	TMap<uint64, int32> LatestPerKey; // packed (channel<<32|type) → index
	for (int32 i = 0; i < ToProcess.Num(); ++i)
	{
		const uint64 Key =
			(static_cast<uint64>(ToProcess[i].Header.ChannelID) << 32)
			| static_cast<uint64>(ToProcess[i].Header.MessageType);
		LatestPerKey.FindOrAdd(Key) = i;
	}

	TArray<int32> LatestIndices;
	LatestIndices.Reserve(LatestPerKey.Num());
	for (const auto& Pair : LatestPerKey)
	{
		LatestIndices.Add(Pair.Value);
	}
	LatestIndices.Sort();

	for (const int32 Idx : LatestIndices)
	{
		const auto& Msg = ToProcess[Idx];
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

	const int32 Width = static_cast<int32>(Meta->GetNumberField(TEXT("w")));
	const int32 Height = static_cast<int32>(Meta->GetNumberField(TEXT("h")));
	FString		Fmt = Meta->GetStringField(TEXT("fmt"));
	Fmt.ToLowerInline();

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Invalid dimensions %dx%d on channel %d"), Width, Height,
			Channel);
		return;
	}

	// Support BGRA8, RGBA8 (swizzled to BGRA8), and RGB8 (expanded to BGRA8)
	const bool bIsRGB8 = (Fmt == TEXT("rgb8"));
	const bool bIsLegacyColor = (Fmt == TEXT("bgra8") || Fmt == TEXT("rgba8") || bIsRGB8);

	if (!bIsLegacyColor)
	{
		// Try the generic format resolver (mono8, r8, gray8, float formats, etc.)
		FResolvedFormat FormatInfo;
		if (!ResolvePixelFormat(Fmt, FormatInfo))
		{
			UE_LOG(LogRammsStreamSink, Warning,
				TEXT("Unsupported format '%s' on channel %d"), *Fmt, Channel);
			return;
		}

		// Validate payload using GPixelFormats[].BlockBytes — the same source UpdateGenericTexture uses.
		const int64 BlockBytes = static_cast<int64>(GPixelFormats[FormatInfo.Format].BlockBytes);
		const int64 ExpectedBytes =
			static_cast<int64>(Width) * static_cast<int64>(Height) * BlockBytes;
		if (ExpectedBytes > MAX_int32 || Msg.Payload.Num() < ExpectedBytes)
		{
			UE_LOG(LogRammsStreamSink, Warning,
				TEXT("Payload size mismatch for fmt '%s': have %d, need %lld on channel %d"),
				*Fmt, Msg.Payload.Num(), ExpectedBytes, Channel);
			return;
		}

		UTexture2D* Tex = UpdateGenericTexture(
			Channel, Msg.Payload.GetData(), Width, Height,
			FormatInfo.Format, FormatInfo.bSRGB);
		if (Tex)
		{
			OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
		}
		return;
	}

	const int32 BytesPerPixel = bIsRGB8 ? 3 : 4;
	const int64 ExpectedSize = static_cast<int64>(Width) * static_cast<int64>(Height) * BytesPerPixel;
	const int64 ConvertedSize = static_cast<int64>(Width) * static_cast<int64>(Height) * 4LL;
	if (ExpectedSize > MAX_int32 || ConvertedSize > MAX_int32 || Msg.Payload.Num() < ExpectedSize)
	{
		UE_LOG(LogRammsStreamSink, Warning,
			TEXT("Payload size mismatch: have %d, need %lld on channel %d"), Msg.Payload.Num(),
			ExpectedSize, Channel);
		return;
	}

	const uint8*  PixelData = Msg.Payload.GetData();
	TArray<uint8> ConvertedData;

	if (bIsRGB8)
	{
		// Expand RGB8 (3 bpp) → BGRA8 (4 bpp)
		const int32 NumPixels = Width * Height;
		ConvertedData.SetNumUninitialized(static_cast<int32>(ConvertedSize));
		for (int32 i = 0; i < NumPixels; ++i)
		{
			const int32 SrcOff = i * 3;
			const int32 DstOff = i * 4;
			ConvertedData[DstOff + 0] = PixelData[SrcOff + 2]; // B ← src B
			ConvertedData[DstOff + 1] = PixelData[SrcOff + 1]; // G ← src G
			ConvertedData[DstOff + 2] = PixelData[SrcOff + 0]; // R ← src R
			ConvertedData[DstOff + 3] = 255;				   // A
		}
		PixelData = ConvertedData.GetData();
	}
	else if (Fmt == TEXT("rgba8"))
	{
		// Swizzle RGBA → BGRA
		const int32 NumPixels = Width * Height;
		ConvertedData.SetNumUninitialized(static_cast<int32>(ConvertedSize));
		for (int32 i = 0; i < NumPixels; ++i)
		{
			const int32 Offset = i * 4;
			ConvertedData[Offset + 0] = PixelData[Offset + 2]; // B ← src B
			ConvertedData[Offset + 1] = PixelData[Offset + 1]; // G ← src G
			ConvertedData[Offset + 2] = PixelData[Offset + 0]; // R ← src R
			ConvertedData[Offset + 3] = PixelData[Offset + 3]; // A ← src A
		}
		PixelData = ConvertedData.GetData();
	}

	// Update metadata to reflect the actual texture format after conversion
	if (Fmt != TEXT("bgra8"))
	{
		Meta->SetStringField(TEXT("fmt"), TEXT("bgra8"));
		FString					  UpdatedMeta;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&UpdatedMeta, 0);
		FJsonSerializer::Serialize(Meta.ToSharedRef(), Writer);
		MetaStr = MoveTemp(UpdatedMeta);
	}

	UTexture2D* Tex =
		UpdateTexture(Channel, PixelData, Width, Height);
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

	// Parse metadata for dimensions and format
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

	// Detect depth encoding from metadata
	FString Fmt;
	Meta->TryGetStringField(TEXT("fmt"), Fmt);
	Fmt.ToLowerInline();
	const bool bIsUint16 = (Fmt == TEXT("16uc1") || Fmt == TEXT("uint16") || Fmt == TEXT("mono16"));

	if (bIsUint16)
	{
		// uint16 mm depth — keep as native PF_G16
		const int64 NumPixels = static_cast<int64>(Width) * static_cast<int64>(Height);
		const int64 ExpectedBytes = NumPixels * 2LL; // 2 bytes per pixel
		if (ExpectedBytes > MAX_int32 || Msg.Payload.Num() < ExpectedBytes)
		{
			UE_LOG(LogRammsStreamSink, Warning,
				TEXT("Depth16 payload too small: %d < %lld on channel %d"),
				Msg.Payload.Num(), ExpectedBytes, Channel);
			return;
		}

		UTexture2D* Tex = UpdateDepthTexture16(Channel, Msg.Payload.GetData(), Width, Height);
		if (Tex)
		{
			OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
		}
	}
	else
	{
		// Default: float32 cm depth
		const int64 NumPixels = static_cast<int64>(Width) * static_cast<int64>(Height);
		const int64 ExpectedBytes = NumPixels * static_cast<int64>(sizeof(float));
		if (ExpectedBytes > MAX_int32 || Msg.Payload.Num() < ExpectedBytes)
		{
			UE_LOG(LogRammsStreamSink, Warning,
				TEXT("Depth payload too small: %d < %lld on channel %d"),
				Msg.Payload.Num(), ExpectedBytes, Channel);
			return;
		}

		UTexture2D* Tex = UpdateDepthTexture(Channel, Msg.Payload.GetData(), Width, Height);
		if (Tex)
		{
			OnFrameReceived.Broadcast(Channel, Tex, MetaStr, Msg.Header.MessageType);
		}
	}
}

UTexture2D* URammsStreamSinkComponent::UpdateTexture(int32 ChannelID,
	const uint8*										   Data,
	int32												   Width,
	int32												   Height)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	// Create new texture if dimensions or format changed
	bool bNewTexture = false;
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height || Tex->GetPixelFormat() != PF_B8G8R8A8)
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
		bNewTexture = true;
	}

	// Update pixel data
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void*			  MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	const int64		  ByteCount = static_cast<int64>(Width) * static_cast<int64>(Height) * 4LL;
	if (ByteCount > MAX_int32 || ByteCount > static_cast<int64>(Mip.BulkData.GetBulkDataSize()))
	{
		Mip.BulkData.Unlock();
		UE_LOG(LogRammsStreamSink, Error, TEXT("Byte count overflow (%lld) for channel %d"), ByteCount, ChannelID);
		return nullptr;
	}
	FMemory::Memcpy(MipData, Data, static_cast<SIZE_T>(ByteCount));
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	// Cache texture only after successful update
	if (bNewTexture)
	{
		ChannelTextures.Add(ChannelID, Tex);
	}

	// Store CPU-side copy for PGM / CPU consumers
	TArray<uint8>& Raw = ChannelRawData.FindOrAdd(ChannelID);
	Raw.SetNumUninitialized(static_cast<int32>(ByteCount));
	FMemory::Memcpy(Raw.GetData(), Data, static_cast<SIZE_T>(ByteCount));
	ChannelPixelFormats.Add(ChannelID, PF_B8G8R8A8);

	return Tex;
}

UTexture2D* URammsStreamSinkComponent::UpdateDepthTexture(int32 ChannelID,
	const uint8*												Data,
	int32														Width,
	int32														Height)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	const int64 ByteCount = static_cast<int64>(Width) * static_cast<int64>(Height) * static_cast<int64>(sizeof(float));
	if (ByteCount > MAX_int32)
	{
		UE_LOG(LogRammsStreamSink, Error, TEXT("Depth byte count overflow (%lld) for channel %d"), ByteCount, ChannelID);
		return nullptr;
	}

	// Create new R32F texture if dimensions or format changed
	bool bNewTexture = false;
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height || Tex->GetPixelFormat() != PF_R32_FLOAT)
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
		bNewTexture = true;
	}

	// Update raw float32 pixel data
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	const int64		  BulkSize = Mip.BulkData.GetBulkDataSize();
	if (BulkSize < ByteCount)
	{
		UE_LOG(LogRammsStreamSink, Error, TEXT("Depth mip bulk data (%lld) smaller than expected (%lld) for channel %d"), BulkSize, ByteCount, ChannelID);
		return nullptr;
	}
	void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Data, static_cast<SIZE_T>(ByteCount));
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	// Cache texture only after successful update
	if (bNewTexture)
	{
		ChannelTextures.Add(ChannelID, Tex);
	}

	// Store CPU-side copy
	TArray<uint8>& Raw = ChannelRawData.FindOrAdd(ChannelID);
	Raw.SetNumUninitialized(static_cast<int32>(ByteCount));
	FMemory::Memcpy(Raw.GetData(), Data, static_cast<SIZE_T>(ByteCount));
	ChannelPixelFormats.Add(ChannelID, PF_R32_FLOAT);

	return Tex;
}

UTexture2D* URammsStreamSinkComponent::UpdateDepthTexture16(int32 ChannelID,
	const uint8* Data, int32 Width, int32 Height)
{
	UTexture2D** Existing = ChannelTextures.Find(ChannelID);
	UTexture2D*	 Tex = Existing ? *Existing : nullptr;

	const int64 ByteCount = static_cast<int64>(Width) * static_cast<int64>(Height) * 2LL;
	if (ByteCount > MAX_int32)
	{
		UE_LOG(LogRammsStreamSink, Error, TEXT("Depth16 byte count overflow (%lld) for channel %d"), ByteCount, ChannelID);
		return nullptr;
	}

	// Create G16 (uint16) texture if dimensions or format changed
	bool bNewTexture = false;
	if (!Tex || Tex->GetSizeX() != Width || Tex->GetSizeY() != Height || Tex->GetPixelFormat() != PF_G16)
	{
		Tex = UTexture2D::CreateTransient(Width, Height, PF_G16);
		if (!Tex)
		{
			UE_LOG(LogRammsStreamSink, Error,
				TEXT("Failed to create G16 texture %dx%d for channel %d"), Width,
				Height, ChannelID);
			return nullptr;
		}
		Tex->SRGB = false;
		Tex->Filter = TF_Bilinear;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		bNewTexture = true;
	}

	// Update raw uint16 pixel data
	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	const int64		  BulkSize = Mip.BulkData.GetBulkDataSize();
	if (BulkSize < ByteCount)
	{
		UE_LOG(LogRammsStreamSink, Error, TEXT("Depth16 mip bulk data (%lld) smaller than expected (%lld) for channel %d"), BulkSize, ByteCount, ChannelID);
		return nullptr;
	}
	void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Data, static_cast<SIZE_T>(ByteCount));
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	// Cache texture only after successful update
	if (bNewTexture)
	{
		ChannelTextures.Add(ChannelID, Tex);
	}

	// Store CPU-side copy
	TArray<uint8>& Raw = ChannelRawData.FindOrAdd(ChannelID);
	Raw.SetNumUninitialized(static_cast<int32>(ByteCount));
	FMemory::Memcpy(Raw.GetData(), Data, static_cast<SIZE_T>(ByteCount));
	ChannelPixelFormats.Add(ChannelID, PF_G16);

	return Tex;
}

UTexture2D* URammsStreamSinkComponent::GetLatestTexture(int32 ChannelID) const
{
	const UTexture2D* const* Found = ChannelTextures.Find(ChannelID);
	return Found ? const_cast<UTexture2D*>(*Found) : nullptr;
}

bool URammsStreamSinkComponent::GetLatestRawData(int32 ChannelID, TArray<uint8>& OutData) const
{
	const TArray<uint8>* Found = ChannelRawData.Find(ChannelID);
	if (Found && Found->Num() > 0)
	{
		OutData = *Found;
		return true;
	}
	OutData.Reset();
	return false;
}

EPixelFormat URammsStreamSinkComponent::GetLatestPixelFormat(int32 ChannelID) const
{
	const EPixelFormat* Found = ChannelPixelFormats.Find(ChannelID);
	return Found ? *Found : PF_Unknown;
}

// --- Format-driven frame processing (FrameMotion, FrameData, future types) ---

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

	const int64 BlockBytes = static_cast<int64>(GPixelFormats[FormatInfo.Format].BlockBytes);
	const int64 ExpectedBytes =
		static_cast<int64>(Width) * static_cast<int64>(Height) * BlockBytes;
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

	// Store CPU-side copy
	if (ByteCount > MAX_int32)
	{
		UE_LOG(LogRammsStreamSink, Error, TEXT("Raw data too large (%lld bytes) to store for channel %d"), ByteCount, ChannelID);
		return Tex;
	}
	TArray<uint8>& Raw = ChannelRawData.FindOrAdd(ChannelID);
	Raw.SetNumUninitialized(static_cast<int32>(ByteCount));
	FMemory::Memcpy(Raw.GetData(), Data, static_cast<SIZE_T>(ByteCount));
	ChannelPixelFormats.Add(ChannelID, Format);

	return Tex;
}
