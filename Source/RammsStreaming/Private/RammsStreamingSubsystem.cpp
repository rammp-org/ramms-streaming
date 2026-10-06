// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamingSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Compression.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
	const int32 SanitizedMaxInboundQueueSize =
		(MaxInboundQueueSize > 0) ? MaxInboundQueueSize : 1;
	const int32 SanitizedMaxOutboundQueueSize =
		(MaxOutboundQueueSize > 0) ? MaxOutboundQueueSize : 1;
	Server->DefaultMaxInboundQueueSize = SanitizedMaxInboundQueueSize;
	Server->DefaultMaxOutboundQueueSize = SanitizedMaxOutboundQueueSize;

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

namespace
{
	/**
	 * Add fields to a caller-supplied metadata JSON instead of replacing it.
	 *
	 * The RGBD path used to rebuild the metadata with Printf whenever
	 * compression was on, which silently dropped everything the source component
	 * had put there -- intrinsics, extrinsics, camera id, timestamp -- so a
	 * compressed stream carried strictly less than an uncompressed one. Parsing
	 * and adding keeps the caller's fields and lets the transport describe the
	 * payload it actually produced.
	 */
	FString MergeStreamMetadata(const FString& BaseJson, const TMap<FString, double>& Numbers, const TMap<FString, FString>& Strings)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!BaseJson.IsEmpty())
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BaseJson);
			if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
			{
				// Unparseable metadata is the caller's business, not something to
				// throw away silently; start fresh but say so.
				UE_LOG(LogTemp, Warning, TEXT("[RammsStreaming] Could not parse supplied metadata; emitting transport fields only"));
				Obj.Reset();
			}
		}
		if (!Obj.IsValid())
		{
			Obj = MakeShared<FJsonObject>();
		}

		for (const TPair<FString, double>& N : Numbers)
		{
			Obj->SetNumberField(N.Key, N.Value);
		}
		for (const TPair<FString, FString>& S : Strings)
		{
			Obj->SetStringField(S.Key, S.Value);
		}

		FString					  Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}
} // namespace

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
	const FString& MetadataJson, int32 DepthWidth, int32 DepthHeight)
{
	// This message carries depth and nothing else, so "w"/"h" describe the depth
	// grid. Callers forwarding a camera frame used to pass the COLOUR size here,
	// which told every client to reshape a depth buffer by the wrong dimensions.
	const int32 PayloadW = DepthWidth > 0 ? DepthWidth : Width;
	const int32 PayloadH = DepthHeight > 0 ? DepthHeight : Height;

	auto Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = ERammsStreamMessageType::FrameDepth;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000000LL + FDateTime::UtcNow().GetMillisecond() * 1000LL;

	// Reinterpret float array as bytes
	const int32 ByteCount = DepthData.Num() * sizeof(float);

	FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)PayloadW },
			{ TEXT("h"), (double)PayloadH },
			{ TEXT("depth_w"), (double)PayloadW },
			{ TEXT("depth_h"), (double)PayloadH } },
		{ { TEXT("fmt"), TEXT("float32") }, { TEXT("unit"), TEXT("cm") } });

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
	const FString& MetadataJson, int32 DepthWidth, int32 DepthHeight)
{
	// "w"/"h" describe the RGB half, which is what a client decodes the JPEG or
	// BGRA block with. Depth may be on a different grid entirely, so it gets its
	// own dimensions rather than borrowing these.
	const int32 DepthW = DepthWidth > 0 ? DepthWidth : Width;
	const int32 DepthH = DepthHeight > 0 ? DepthHeight : Height;

	auto Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = ERammsStreamMessageType::FrameRGBD;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000000LL + FDateTime::UtcNow().GetMillisecond() * 1000LL;

	const int32 DepthBytes = DepthData.Num() * sizeof(float);

	// Transport fields are ADDED to whatever the caller supplied, never swapped
	// for it, so intrinsics and extrinsics survive regardless of compression.
	FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)Width },
			{ TEXT("h"), (double)Height },
			{ TEXT("depth_w"), (double)DepthW },
			{ TEXT("depth_h"), (double)DepthH },
			{ TEXT("rgb_size"), (double)PixelData.Num() },
			{ TEXT("depth_size"), (double)DepthBytes } },
		{ { TEXT("rgb_fmt"), TEXT("bgra8") },
			{ TEXT("depth_fmt"), TEXT("float32") },
			{ TEXT("depth_unit"), TEXT("cm") } });

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

			// Restate the sizes now that they are the COMPRESSED ones, and say
			// how each half was compressed. This used to rebuild the whole JSON
			// and so dropped every field the caller had set.
			Meta = MergeStreamMetadata(Meta,
				{ { TEXT("rgb_size"), (double)RgbCompressed.Num() },
					{ TEXT("depth_size"), (double)DepthCompressed.Num() },
					{ TEXT("rgb_raw_size"), (double)PixelData.Num() },
					{ TEXT("depth_raw_size"), (double)DepthBytes } },
				{ { TEXT("rgb_comp"), TEXT("jpeg") }, { TEXT("depth_comp"), TEXT("lz4") } });

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
