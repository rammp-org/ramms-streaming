// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamingSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Compression.h"
#include "RammsStreamServer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRammsStreamSub, Log, All);

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

URammsStreamingSubsystem::URammsStreamingSubsystem() = default;
URammsStreamingSubsystem::~URammsStreamingSubsystem() = default;

// ---------------------------------------------------------------------------
// USubsystem lifecycle
// ---------------------------------------------------------------------------

void URammsStreamingSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Register a tick callback via the Engine delegate
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool {
			OnTick(DeltaTime);
			return true; // keep ticking
		}),
		0.0f // tick every frame
	);

	UE_LOG(LogRammsStreamSub, Log, TEXT("RammsStreaming subsystem initialized"));
}

void URammsStreamingSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	StopServer();

	Super::Deinitialize();
	UE_LOG(LogRammsStreamSub, Log,
		TEXT("RammsStreaming subsystem deinitialized"));
}

// ---------------------------------------------------------------------------
// Server control
// ---------------------------------------------------------------------------

bool URammsStreamingSubsystem::StartServer(int32 Port, int32 MaxClients)
{
	if (Server.IsValid() && Server->IsListening())
	{
		UE_LOG(LogRammsStreamSub, Warning,
			TEXT("Server already running on port %d"), Server->GetPort());
		return true;
	}

	Server =
		MakeUnique<FRammsStreamServer>(static_cast<uint16>(Port), MaxClients);

	// Apply queue size configuration
	Server->DefaultMaxInboundQueueSize = MaxInboundQueueSize;
	Server->DefaultMaxOutboundQueueSize = MaxOutboundQueueSize;

	// Wire up inbound message delegate
	Server->OnMessageReceived.BindLambda(
		[this](uint32 ConnId, const FRammsStreamMessage& Msg) {
			OnMessageReceived.Broadcast(static_cast<int32>(ConnId),
				Msg.Header.MessageType);
			OnNativeMessageReceived.Broadcast(static_cast<int32>(ConnId), Msg);
		});

	if (!Server->StartListening())
	{
		UE_LOG(LogRammsStreamSub, Error,
			TEXT("Failed to start RMSS server on port %d"), Port);
		Server.Reset();
		return false;
	}

	return true;
}

void URammsStreamingSubsystem::StopServer()
{
	if (Server.IsValid())
	{
		Server->StopListening();
		Server.Reset();
	}
	SequenceCounters.Empty();
}

bool URammsStreamingSubsystem::IsServerRunning() const
{
	return Server.IsValid() && Server->IsListening();
}

int32 URammsStreamingSubsystem::GetConnectionCount() const
{
	return Server.IsValid() ? Server->GetConnectionCount() : 0;
}

int32 URammsStreamingSubsystem::GetServerPort() const
{
	return Server.IsValid() ? Server->GetPort() : 0;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void URammsStreamingSubsystem::OnTick(float DeltaTime)
{
	if (Server.IsValid() && Server->IsListening())
	{
		Server->Tick();
	}
}

// ---------------------------------------------------------------------------
// Frame broadcasting
// ---------------------------------------------------------------------------

uint32 URammsStreamingSubsystem::GetNextSequence(uint16 ChannelID)
{
	uint32& Seq = SequenceCounters.FindOrAdd(ChannelID, 0);
	return Seq++;
}

void URammsStreamingSubsystem::BroadcastMessage(
	TSharedRef<FRammsStreamMessage> Msg)
{
	if (!Server.IsValid() || !Server->IsListening())
		return;
	Server->BroadcastToSubscribers(Msg);
}

void URammsStreamingSubsystem::BroadcastRGBFrame(int32 ChannelID,
	const TArray<uint8>&							   PixelData,
	int32 Width, int32 Height,
	const FString& MetadataJson)
{
	auto Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = ERammsStreamMessageType::FrameRGB;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000000LL + FDateTime::UtcNow().GetMillisecond() * 1000LL;

	// Build metadata: include width/height at minimum
	FString Meta = MetadataJson;
	if (Meta.IsEmpty())
	{
		Meta = FString::Printf(TEXT("{\"w\":%d,\"h\":%d,\"fmt\":\"bgra8\"}"), Width,
			Height);
	}

	// Optional JPEG compression
	if (bEnableCompression)
	{
		TArray<uint8> Compressed =
			CompressJPEG(PixelData, Width, Height, JpegQuality);
		if (Compressed.Num() > 0)
		{
			Msg->Header.SetCompression(ERammsStreamCompression::JPEG);
			Msg->Payload = MoveTemp(Compressed);
		}
		else
		{
			Msg->Payload = PixelData;
		}
	}
	else
	{
		Msg->Payload = PixelData;
	}

	Msg->SetMetadataString(Meta);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg);
}

void URammsStreamingSubsystem::BroadcastDepthFrame(
	int32 ChannelID, const TArray<float>& DepthData, int32 Width, int32 Height,
	const FString& MetadataJson)
{
	auto Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = ERammsStreamMessageType::FrameDepth;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000000LL + FDateTime::UtcNow().GetMillisecond() * 1000LL;

	FString Meta = MetadataJson;
	if (Meta.IsEmpty())
	{
		Meta = FString::Printf(
			TEXT("{\"w\":%d,\"h\":%d,\"fmt\":\"float32\",\"unit\":\"cm\"}"), Width,
			Height);
	}

	// Reinterpret float array as bytes
	const int32 ByteCount = DepthData.Num() * sizeof(float);

	// Optional LZ4 compression
	if (bEnableCompression)
	{
		TArray<uint8> Compressed = CompressLZ4(
			reinterpret_cast<const uint8*>(DepthData.GetData()), ByteCount);
		if (Compressed.Num() > 0)
		{
			Msg->Header.SetCompression(ERammsStreamCompression::LZ4);
			Msg->Payload = MoveTemp(Compressed);
		}
		else
		{
			Msg->Payload.SetNumUninitialized(ByteCount);
			FMemory::Memcpy(Msg->Payload.GetData(), DepthData.GetData(), ByteCount);
		}
	}
	else
	{
		Msg->Payload.SetNumUninitialized(ByteCount);
		FMemory::Memcpy(Msg->Payload.GetData(), DepthData.GetData(), ByteCount);
	}

	Msg->SetMetadataString(Meta);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg);
}

void URammsStreamingSubsystem::BroadcastRGBDFrame(
	int32 ChannelID, const TArray<uint8>& PixelData,
	const TArray<float>& DepthData, int32 Width, int32 Height,
	const FString& MetadataJson)
{
	auto Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = ERammsStreamMessageType::FrameRGBD;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000000LL + FDateTime::UtcNow().GetMillisecond() * 1000LL;

	const int32 DepthBytes = DepthData.Num() * sizeof(float);

	FString Meta = MetadataJson;
	if (Meta.IsEmpty())
	{
		Meta = FString::Printf(
			TEXT("{\"w\":%d,\"h\":%d,\"rgb_fmt\":\"bgra8\",\"depth_fmt\":"
				 "\"float32\",")
				TEXT("\"rgb_size\":%d,\"depth_size\":%d,\"depth_unit\":\"cm\"}"),
			Width, Height, PixelData.Num(), DepthBytes);
	}

	// For RGBD, compress RGB portion with JPEG and depth with LZ4
	// Both halves are concatenated; metadata carries their sizes
	if (bEnableCompression)
	{
		TArray<uint8> RgbCompressed =
			CompressJPEG(PixelData, Width, Height, JpegQuality);
		TArray<uint8> DepthCompressed = CompressLZ4(
			reinterpret_cast<const uint8*>(DepthData.GetData()), DepthBytes);

		if (RgbCompressed.Num() > 0 && DepthCompressed.Num() > 0)
		{
			Msg->Header.SetCompression(ERammsStreamCompression::JPEG);
			Msg->Header.Flags |=
				FRammsStreamHeader::FLAG_HIGH_PRIORITY; // signal mixed compression

			// Override metadata with compressed sizes
			Meta = FString::Printf(
				TEXT("{\"w\":%d,\"h\":%d,\"rgb_fmt\":\"bgra8\",\"depth_fmt\":"
					 "\"float32\",")
					TEXT("\"rgb_size\":%d,\"depth_size\":%d,\"depth_unit\":\"cm\",")
						TEXT("\"rgb_comp\":\"jpeg\",\"depth_comp\":\"lz4\",\"rgb_raw_"
							 "size\":%d,\"depth_raw_size\":%d}"),
				Width, Height, RgbCompressed.Num(), DepthCompressed.Num(),
				PixelData.Num(), DepthBytes);

			Msg->Payload.SetNumUninitialized(RgbCompressed.Num() + DepthCompressed.Num());
			FMemory::Memcpy(Msg->Payload.GetData(), RgbCompressed.GetData(),
				RgbCompressed.Num());
			FMemory::Memcpy(Msg->Payload.GetData() + RgbCompressed.Num(),
				DepthCompressed.GetData(), DepthCompressed.Num());
		}
		else
		{
			// Fallback to raw
			Msg->Payload.SetNumUninitialized(PixelData.Num() + DepthBytes);
			FMemory::Memcpy(Msg->Payload.GetData(), PixelData.GetData(),
				PixelData.Num());
			FMemory::Memcpy(Msg->Payload.GetData() + PixelData.Num(),
				DepthData.GetData(), DepthBytes);
		}
	}
	else
	{
		Msg->Payload.SetNumUninitialized(PixelData.Num() + DepthBytes);
		FMemory::Memcpy(Msg->Payload.GetData(), PixelData.GetData(),
			PixelData.Num());
		FMemory::Memcpy(Msg->Payload.GetData() + PixelData.Num(),
			DepthData.GetData(), DepthBytes);
	}

	Msg->SetMetadataString(Meta);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg);
}

// ---------------------------------------------------------------------------
// Compression helpers
// ---------------------------------------------------------------------------

TArray<uint8>
URammsStreamingSubsystem::CompressJPEG(const TArray<uint8>& PixelData,
	int32 Width, int32 Height,
	int32 Quality) const
{
	// PixelData is BGRA8 — same layout as FColor
	const FColor* ColorData =
		reinterpret_cast<const FColor*>(PixelData.GetData());
	FImageView ImageView(ColorData, Width, Height, EGammaSpace::sRGB);

	TArray64<uint8> CompressedData;
	if (!FImageUtils::CompressImage(CompressedData, TEXT("jpg"), ImageView,
			Quality))
	{
		UE_LOG(LogRammsStreamSub, Warning,
			TEXT("JPEG compression failed for %dx%d"), Width, Height);
		return {};
	}

	// Convert TArray64 to TArray (safe — JPEG output is always < 2GB)
	TArray<uint8> Result;
	Result.SetNumUninitialized(static_cast<int32>(CompressedData.Num()));
	FMemory::Memcpy(Result.GetData(), CompressedData.GetData(),
		CompressedData.Num());
	return Result;
}

TArray<uint8> URammsStreamingSubsystem::CompressLZ4(const uint8* Data,
	int32														 DataSize)
{
	const int32	  BoundSize = FCompression::CompressMemoryBound(NAME_LZ4, DataSize);
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(BoundSize + sizeof(int32)); // prefix with original size

	// Store uncompressed size as first 4 bytes (little-endian)
	FMemory::Memcpy(Compressed.GetData(), &DataSize, sizeof(int32));

	int32 CompressedSize = BoundSize;
	if (!FCompression::CompressMemory(NAME_LZ4,
			Compressed.GetData() + sizeof(int32),
			CompressedSize, Data, DataSize))
	{
		UE_LOG(LogRammsStreamSub, Warning,
			TEXT("LZ4 compression failed for %d bytes"), DataSize);
		return {};
	}

	Compressed.SetNum(CompressedSize + sizeof(int32));
	return Compressed;
}
