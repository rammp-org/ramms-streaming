// Copyright Epic Games, Inc. All Rights Reserved.

#include "RammsStreamingSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Hash/CityHash.h"
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

	LastServerError.Reset();
	if (Port < 0 || Port > MAX_uint16)
	{
		LastServerError = FString::Printf(TEXT("port %d is not a TCP port"), Port);
		UE_LOG(LogRammsStreamSub, Error, TEXT("Failed to start RMSS server: %s"), *LastServerError);
		return false;
	}

	Server =
		MakeUnique<FRammsStreamServer>(static_cast<uint16>(Port), MaxClients);

	// Apply queue size configuration
	// Pass the configured size through; 0 means unlimited. It used to be coerced
	// to 1, which made the ring drop every channel but the last one sent between
	// two ticks. See MaxInboundQueueSize for why the bound matters either way.
	const int32 SanitizedMaxInboundQueueSize = FMath::Max(MaxInboundQueueSize, 0);
	const int32 SanitizedMaxOutboundQueueSize =
		(MaxOutboundQueueSize > 0) ? MaxOutboundQueueSize : 1;
	Server->DefaultMaxInboundQueueSize = SanitizedMaxInboundQueueSize;
	Server->DefaultMaxOutboundQueueSize = SanitizedMaxOutboundQueueSize;

	// Say which sizes are actually in force. They are latched here, so a caller
	// that sets the property after the server starts -- or that loses the race
	// to start it at all -- gets no feedback otherwise, and an inbound queue too
	// small for the channel count fails as "that camera never appears" rather
	// than as anything resembling a dropped message.
	UE_LOG(LogRammsStreamSub, Log,
		TEXT("Stream server queue sizes: inbound %s, outbound %d"),
		SanitizedMaxInboundQueueSize > 0 ? *FString::FromInt(SanitizedMaxInboundQueueSize) : TEXT("unlimited"),
		SanitizedMaxOutboundQueueSize);

	// Wire up inbound message delegate
	Server->OnMessageReceived.BindLambda(
		[this](uint32 ConnId, const FRammsStreamMessage& Msg) {
			OnMessageReceived.Broadcast(static_cast<int32>(ConnId),
				Msg.Header.MessageType);
			OnNativeMessageReceived.Broadcast(static_cast<int32>(ConnId), Msg);
		});

	if (!Server->StartListening())
	{
		LastServerError = Server->GetLastError();
		UE_LOG(LogRammsStreamSub, Error,
			TEXT("Failed to start RMSS server on port %d: %s"), Port, *LastServerError);
		Server.Reset();
		return false;
	}

	return true;
}

bool URammsStreamingSubsystem::IsChannelWanted(int32 ChannelID) const
{
	return Server.IsValid() && Server->IsListening() && ChannelID >= 0 && ChannelID <= MAX_uint16
		&& Server->HasSubscriber(static_cast<uint16>(ChannelID));
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

FRammsStreamStats URammsStreamingSubsystem::GetStreamStats() const
{
	FRammsStreamStats Out;
	if (!Server.IsValid())
	{
		return Out;
	}

	// Counters are 32/64-bit unsigned and Blueprint has no unsigned types; a run long enough to
	// overflow a signed count has bigger problems than a wrapped readout, so this clamps rather
	// than wraps, leaving a stuck maximum as the visible symptom.
	const FRammsStreamServer::FStats& Stats = Server->GetStats();
	Out.bServerRunning = true;
	Out.MessagesReceived = static_cast<int32>(FMath::Min<uint32>(Stats.MessagesReceived.Load(), MAX_int32));
	Out.TotalBytesReceived = static_cast<int64>(FMath::Min<uint64>(Stats.TotalBytesReceived.Load(), MAX_int64));
	Out.FramesSent = static_cast<int32>(FMath::Min<uint32>(Stats.FramesSent.Load(), MAX_int32));
	Out.TotalBytesSent = static_cast<int64>(FMath::Min<uint64>(Stats.TotalBytesSent.Load(), MAX_int64));
	Out.FramesDropped = static_cast<int32>(FMath::Min<uint32>(Stats.FramesDropped.Load(), MAX_int32));
	Out.ConnectionsAccepted = static_cast<int32>(FMath::Min<uint32>(Stats.TotalConnectionsAccepted.Load(), MAX_int32));
	return Out;
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
	TSharedRef<FRammsStreamMessage> Msg, uint64 StreamKey)
{
	if (!Server.IsValid() || !Server->IsListening())
		return;
	Server->BroadcastToSubscribers(Msg, StreamKey);
}

namespace RammsStreamingSubsystemImpl
{
	/** MakeStreamKey of a parsed metadata object. */
	uint64 StreamKeyOf(const FJsonObject& Metadata)
	{
		FString StreamId, Actor, Camera;
		Metadata.TryGetStringField(TEXT("stream_id"), StreamId);
		Metadata.TryGetStringField(TEXT("actor"), Actor);
		Metadata.TryGetStringField(TEXT("camera"), Camera);
		return URammsStreamingSubsystem::MakeStreamKey(StreamId, Actor, Camera);
	}

	/** Microseconds since the Unix epoch, from one clock reading (two readings could straddle a second). */
	int64 NowMicroseconds()
	{
		const FTimespan SinceEpoch = FDateTime::UtcNow() - FDateTime(1970, 1, 1);
		return SinceEpoch.GetTicks() / ETimespan::TicksPerMicrosecond;
	}

	/** Bytes per pixel of a FrameData "fmt" this plugin knows, else 0. */
	int32 FrameFormatBytesPerPixel(const FString& Format)
	{
		const FString Fmt = Format.ToLower();
		if (Fmt == TEXT("mono8") || Fmt == TEXT("r8") || Fmt == TEXT("gray8") || Fmt == TEXT("u8"))
		{
			return 1;
		}
		if (Fmt == TEXT("mono16") || Fmt == TEXT("uint16") || Fmt == TEXT("16uc1") || Fmt == TEXT("g16"))
		{
			return 2;
		}
		if (Fmt == TEXT("rgb8") || Fmt == TEXT("bgr8"))
		{
			return 3;
		}
		if (Fmt == TEXT("float32") || Fmt == TEXT("r32f") || Fmt == TEXT("bgra8") || Fmt == TEXT("rgba8"))
		{
			return 4;
		}
		if (Fmt == TEXT("rg32f") || Fmt == TEXT("float32x2"))
		{
			return 8;
		}
		return 0;
	}
} // namespace RammsStreamingSubsystemImpl

TSharedPtr<FRammsStreamMessage> URammsStreamingSubsystem::NewFrameMessage(ERammsStreamMessageType Type, int32 ChannelID)
{
	if (ChannelID < 0 || ChannelID > MAX_uint16)
	{
		UE_LOG(LogRammsStreamSub, Warning, TEXT("Frame dropped: channel %d is outside 0..65535"), ChannelID);
		return nullptr;
	}
	// Nobody to send it to: skip the copies and compression a frame costs.
	if (!IsChannelWanted(ChannelID))
	{
		return nullptr;
	}
	TSharedRef<FRammsStreamMessage> Msg = MakeShared<FRammsStreamMessage>();
	Msg->Header.MessageType = Type;
	Msg->Header.ChannelID = static_cast<uint16>(ChannelID);
	Msg->Header.SequenceNum = GetNextSequence(static_cast<uint16>(ChannelID));
	Msg->Header.Timestamp = RammsStreamingSubsystemImpl::NowMicroseconds();
	return Msg;
}

uint64 URammsStreamingSubsystem::MakeStreamKey(const FString& StreamId, const FString& Actor, const FString& Camera)
{
	if (StreamId.IsEmpty() && Actor.IsEmpty() && Camera.IsEmpty())
	{
		return 0;
	}
	const FString Identity = StreamId + TEXT("\n") + Actor + TEXT("\n") + Camera;
	const uint64  Key = CityHash64(reinterpret_cast<const char*>(*Identity), static_cast<uint32>(Identity.Len() * sizeof(TCHAR)));
	return Key != 0 ? Key : 1; // 0 means "no identity"
}

uint64 URammsStreamingSubsystem::StreamKeyFromMetadata(const FString& MetadataJson)
{
	if (MetadataJson.IsEmpty())
	{
		return 0;
	}
	TSharedPtr<FJsonObject>			Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MetadataJson);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		return 0;
	}
	return RammsStreamingSubsystemImpl::StreamKeyOf(*Obj);
}

void URammsStreamingSubsystem::BroadcastEncodedFrame(ERammsStreamMessageType Type, int32 ChannelID,
	TArray<uint8>&& Payload, int32 Width, int32 Height, const FString& MetadataJson, uint64 StreamKey)
{
	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(Type, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

	// The same optional compression as BroadcastRGBFrame / BroadcastDepthFrame; the header says which was used,
	// and the metadata keeps describing the uncompressed pixels.
	if (bEnableCompression && Type == ERammsStreamMessageType::FrameRGB && Width > 0 && Height > 0
		&& Payload.Num() == Width * Height * 4)
	{
		TArray<uint8> Compressed = CompressJPEG(Payload, Width, Height, JpegQuality);
		if (Compressed.Num() > 0)
		{
			Msg->Header.SetCompression(ERammsStreamCompression::JPEG);
			Payload = MoveTemp(Compressed);
		}
	}
	else if (bEnableCompression && Type == ERammsStreamMessageType::FrameDepth)
	{
		TArray<uint8> Compressed = CompressLZ4(Payload.GetData(), Payload.Num());
		if (Compressed.Num() > 0)
		{
			Msg->Header.SetCompression(ERammsStreamCompression::LZ4);
			Payload = MoveTemp(Compressed);
		}
	}

	Msg->Payload = MoveTemp(Payload);
	Msg->SetMetadataString(MetadataJson);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg, StreamKey != 0 ? StreamKey : StreamKeyFromMetadata(MetadataJson));
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
	 * payload it actually produced. OutStreamKey, if given, receives the stream key of the caller's metadata
	 * (URammsStreamingSubsystem::MakeStreamKey).
	 */
	FString MergeStreamMetadata(const FString& BaseJson, const TMap<FString, double>& Numbers, const TMap<FString, FString>& Strings,
		uint64* OutStreamKey = nullptr)
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
		if (OutStreamKey)
		{
			*OutStreamKey = RammsStreamingSubsystemImpl::StreamKeyOf(*Obj);
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
	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(ERammsStreamMessageType::FrameRGB, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

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

	BroadcastMessage(Msg, StreamKeyFromMetadata(MetadataJson));
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

	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(ERammsStreamMessageType::FrameDepth, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

	// Reinterpret float array as bytes
	const int32 ByteCount = DepthData.Num() * sizeof(float);

	uint64	StreamKey = 0;
	FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)PayloadW },
			{ TEXT("h"), (double)PayloadH },
			{ TEXT("depth_w"), (double)PayloadW },
			{ TEXT("depth_h"), (double)PayloadH } },
		{ { TEXT("fmt"), TEXT("float32") }, { TEXT("unit"), TEXT("cm") } }, &StreamKey);

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

	BroadcastMessage(Msg, StreamKey);
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

	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(ERammsStreamMessageType::FrameRGBD, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

	const int32 DepthBytes = DepthData.Num() * sizeof(float);

	// Transport fields are ADDED to whatever the caller supplied, never swapped
	// for it, so intrinsics and extrinsics survive regardless of compression.
	uint64	StreamKey = 0;
	FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)Width },
			{ TEXT("h"), (double)Height },
			{ TEXT("depth_w"), (double)DepthW },
			{ TEXT("depth_h"), (double)DepthH },
			{ TEXT("rgb_size"), (double)PixelData.Num() },
			{ TEXT("depth_size"), (double)DepthBytes } },
		{ { TEXT("rgb_fmt"), TEXT("bgra8") },
			{ TEXT("depth_fmt"), TEXT("float32") },
			{ TEXT("depth_unit"), TEXT("cm") } },
		&StreamKey);

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

	BroadcastMessage(Msg, StreamKey);
}

void URammsStreamingSubsystem::BroadcastMotionFrame(int32 ChannelID,
	const TArray<float>& MotionData, int32 Width, int32 Height,
	const FString& MetadataJson)
{
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height) * 2;
	if (Width <= 0 || Height <= 0 || MotionData.Num() != Expected)
	{
		UE_LOG(LogRammsStreamSub, Warning,
			TEXT("Motion frame on channel %d dropped: %d values for a %dx%d grid (need %lld)"),
			ChannelID, MotionData.Num(), Width, Height, Expected);
		return;
	}

	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(ERammsStreamMessageType::FrameMotion, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

	// The payload is the motion grid, so "w"/"h" describe it, as they describe
	// the depth grid on a depth frame.
	uint64		  StreamKey = 0;
	const FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)Width },
			{ TEXT("h"), (double)Height },
			{ TEXT("motion_w"), (double)Width },
			{ TEXT("motion_h"), (double)Height } },
		{ { TEXT("fmt"), TEXT("rg32f") } }, &StreamKey);

	const int32 ByteCount = MotionData.Num() * sizeof(float);
	Msg->Payload.SetNumUninitialized(ByteCount);
	FMemory::Memcpy(Msg->Payload.GetData(), MotionData.GetData(), ByteCount);

	Msg->SetMetadataString(Meta);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg, StreamKey);
}

void URammsStreamingSubsystem::BroadcastFrameData(int32 ChannelID,
	const TArray<uint8>& Data, int32 Width, int32 Height,
	const FString& Format, const FString& MetadataJson)
{
	const FString Fmt = Format.IsEmpty() ? FString(TEXT("mono8")) : Format;
	// A format this plugin knows must fill the grid exactly, or no client can reshape it; others are only
	// required to be non-empty.
	const int32 BytesPerPixel = RammsStreamingSubsystemImpl::FrameFormatBytesPerPixel(Fmt);
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height) * BytesPerPixel;
	if (Width <= 0 || Height <= 0 || Data.Num() == 0 || (BytesPerPixel > 0 && Data.Num() != Expected))
	{
		UE_LOG(LogRammsStreamSub, Warning,
			TEXT("FrameData on channel %d dropped: %d bytes for a %dx%d %s grid"),
			ChannelID, Data.Num(), Width, Height, *Fmt);
		return;
	}

	const TSharedPtr<FRammsStreamMessage> NewMsg = NewFrameMessage(ERammsStreamMessageType::FrameData, ChannelID);
	if (!NewMsg.IsValid())
	{
		return;
	}
	const TSharedRef<FRammsStreamMessage> Msg = NewMsg.ToSharedRef();

	uint64		  StreamKey = 0;
	const FString Meta = MergeStreamMetadata(MetadataJson,
		{ { TEXT("w"), (double)Width }, { TEXT("h"), (double)Height } },
		{ { TEXT("fmt"), Fmt } }, &StreamKey);

	Msg->Payload = Data;

	Msg->SetMetadataString(Meta);
	Msg->Header.PayloadLen = Msg->Payload.Num();
	Msg->Header.MetadataLen = Msg->Metadata.Num();

	BroadcastMessage(Msg, StreamKey);
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
